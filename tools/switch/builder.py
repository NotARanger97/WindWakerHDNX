"""Builds the Switch homebrew (wwhd.nro) on a PC from your own copy of the game: no Docker, no devkitPro.

  python3 tools/switch/builder.py --game <dump> [--sdk DIR] [--out wwhd.nro] [--work DIR] [--jobs N]

  --game   a Cemu archive (.wua), the extracted game (the folder with code/, content/, meta/) or its
           code/cking.rpx
  --sdk    the Switch SDK (default: sdk-switch/ next to this repository, as in a builder release)
  --out    the homebrew to copy to sdmc:/switch/wwhd/wwhd.nro (default: ./wwhd.nro)
  --work   working directory for the translated and compiled game code (default: ./build/switch-builder)

What it does: translates the game's PowerPC code to C with this repository's recompiler, compiles it for
the Switch's Cortex-A57 with clang from the pinned zig (bundled with the builder program, else downloaded
once, checksum verified), links it with the SDK's prebuilt runtime using zig's lld, and packs the result
as an NRO (tools/switch/nro.py). The SDK (tools/switch/sdk_prelink.py) holds no game code; what this
makes from your dump stays on your PC. The window program is tools/switch/builder_gui.py.
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import nro  # noqa: E402
import sdk_prelink  # noqa: E402  (recompiler_revision)

# zig 0.16.0 (clang + lld), the version the SDK's flags were measured with; ziglang.org's checksums
ZIG = {
    ("windows", "x86_64"): ("https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip",
                            "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"),
    ("windows", "aarch64"): ("https://ziglang.org/download/0.16.0/zig-aarch64-windows-0.16.0.zip",
                             "aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef"),
    ("macos", "aarch64"): ("https://ziglang.org/download/0.16.0/zig-aarch64-macos-0.16.0.tar.xz",
                           "b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489"),
    ("macos", "x86_64"): ("https://ziglang.org/download/0.16.0/zig-x86_64-macos-0.16.0.tar.xz",
                          "0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7"),
    ("linux", "x86_64"): ("https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz",
                          "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"),
    ("linux", "aarch64"): ("https://ziglang.org/download/0.16.0/zig-aarch64-linux-0.16.0.tar.xz",
                           "ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17"),
}
# the releases the port runs (version 0, the disc and eShop release without the update): title ID ->
# (region, SHA-256 of code/cking.rpx). The USA one gets the recompiler's
# hooks; the European executable is recompiled without them.
GAMES = {
    "0005000010143500": ("us", "c4f0ab300542e0bfc462696850534e71db2ad02288a7eb55e5a4cd4062f16153"),  # setup.py's
    "0005000010143600": ("eu", "f9f461738949a09481dc1a31c01ad27db813c4c6058fdd7d015624a4146bbf0b"),
}
NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0  # CREATE_NO_WINDOW: no console per compiler run


class BuildError(Exception):
    pass


def host():
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x64": "x86_64", "arm64": "aarch64"}.get(machine, machine)
    return system, arch


def run(cmd, log, what, env=None):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, creationflags=NO_WINDOW)
    out = p.stdout.decode("utf-8", "replace")
    if p.returncode:
        log(out[-4000:])
        raise BuildError("%s failed (exit code %d)" % (what, p.returncode))
    return out


def get_zig(root, log):
    """zig from the builder program's own folder (bundled), else downloaded once into root."""
    system, arch = host()
    bundled = os.path.join(REPO, "zig", "zig.exe" if system == "windows" else "zig")
    if os.path.isfile(bundled):
        return bundled
    if (system, arch) not in ZIG:
        raise BuildError("no pinned zig for %s %s" % (system, arch))
    url, sha = ZIG[(system, arch)]
    name = os.path.basename(url).replace(".tar.xz", "").replace(".zip", "")
    exe = os.path.join(root, name, "zig.exe" if system == "windows" else "zig")
    marker = os.path.join(root, name, ".sha256")
    if os.path.isfile(exe) and os.path.isfile(marker) and open(marker).read().strip() == sha:
        return exe
    os.makedirs(root, exist_ok=True)
    archive = os.path.join(root, os.path.basename(url))
    log("Getting the compiler (zig 0.16.0, once): %s" % url)
    h = hashlib.sha256()
    req = urllib.request.Request(url, headers={"User-Agent": "wwhd-switch-builder"})
    with urllib.request.urlopen(req, timeout=60) as r, open(archive + ".part", "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
    if h.hexdigest() != sha:
        os.remove(archive + ".part")
        raise BuildError("the zig download is corrupt or was changed (SHA-256 mismatch)")
    os.replace(archive + ".part", archive)
    shutil.rmtree(os.path.join(root, name), ignore_errors=True)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(root)
    else:
        with tarfile.open(archive) as t:
            t.extractall(root)
    os.remove(archive)
    with open(marker, "w") as f:
        f.write(sha + "\n")
    return exe


def title_of_meta(text):
    for tid in GAMES:
        if tid in text.lower():
            return tid
    return None


def prepare_game(game, work, log):
    """(path of cking.rpx, title id) from a .wua, a game folder or a cking.rpx; a .wua's rpx is taken out
    into the work folder."""
    game = os.path.abspath(game)
    if game.lower().endswith(".wua"):
        import wua
        try:
            archive = wua.Wua(game)
        except (OSError, wua.WuaError) as e:
            raise BuildError("cannot read %s: %s" % (game, e))
        titles = archive.titles()
        bases = [t for t in titles if t[1] in GAMES]
        if not bases:
            found = ", ".join(t[0] for t in titles) or "no Wii U titles"
            raise BuildError("this archive does not contain The Wind Waker HD (Europe or USA) itself (it contains: %s). "
                             "In Cemu, make the archive with the base game included." % found)
        folder, tid, version = bases[0]
        if version != 0:
            raise BuildError("the game in this archive is version %d; the port needs version 0 (the game without "
                             "the update)" % version)
        log("Taking the game code out of the archive (%s)..." % folder)
        rpx = os.path.join(work, "cking.rpx")
        os.makedirs(work, exist_ok=True)
        with open(rpx, "wb") as f:
            f.write(archive.read(folder + "/code/cking.rpx"))
        return rpx, tid
    if os.path.isfile(game) and game.lower().endswith(".rpx"):
        rpx, top = game, os.path.dirname(os.path.dirname(game))
    else:
        rpx, top = os.path.join(game, "code", "cking.rpx"), game
    if not os.path.isfile(rpx):
        raise BuildError("no code/cking.rpx in %s: choose your Cemu archive (.wua) or the extracted game folder" % game)
    tid = None
    for meta in (os.path.join(top, "meta", "meta.xml"), os.path.join(top, "code", "app.xml")):
        if os.path.isfile(meta):
            tid = tid or title_of_meta(open(meta, encoding="utf-8", errors="replace").read())
    return rpx, tid


def check_rpx(rpx, tid):
    """The translated code is made for exactly version 0 of the game: refuse an updated or modified code."""
    with open(rpx, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if tid is None:
        for t, (_region, sha) in GAMES.items():
            if sha == digest:
                tid = t
    if tid is None:
        raise BuildError("this is not The Wind Waker HD (Europe or USA), version 0, as the port needs it (the game "
                         "code's SHA-256 is %s...). Use the game without the update." % digest[:16])
    region, sha = GAMES[tid]
    if sha and digest != sha:
        raise BuildError("the game code is not version 0 of The Wind Waker HD (%s): an updated or modified copy "
                         "(SHA-256 %s...). Use the game without the update." % (region.upper(), digest[:16]))
    return region


def translate(rpx, region, gen, log):
    stamp = os.path.join(gen, ".rpx-sha256")
    digest = hashlib.sha256(open(rpx, "rb").read()).hexdigest()
    if os.path.isfile(os.path.join(gen, "table.c")) and os.path.isfile(stamp) and open(stamp).read().strip() == digest:
        log("Game code already translated.")
        return
    shutil.rmtree(gen, ignore_errors=True)
    env = dict(os.environ)
    if region == "eu":
        hooks = os.path.join(os.path.dirname(gen), "no-hooks.txt")
        open(hooks, "w").close()
        env["WWHD_HOOKS"] = hooks
    recomp = os.path.join(REPO, "tools", "recomp")
    run([sys.executable, os.path.join(recomp, "recomp.py"), rpx, gen], log, "translating the game code", env)
    if region == "eu":
        run([sys.executable, os.path.join(recomp, "region_compat.py"), gen], log, "adapting the game code to Europe")
    with open(stamp, "w") as f:
        f.write(digest + "\n")


def compile_all(zig, cflags, gen, obj_dir, jobs, progress, cancel):
    os.makedirs(obj_dir, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(gen, "code_*.c"))) + [os.path.join(gen, "table.c"), os.path.join(gen, "imports.c")]
    srcs.sort(key=lambda s: -os.path.getsize(s))  # big files first
    lock, done, failures = threading.Lock(), [0], []

    def one(src):
        obj = os.path.join(obj_dir, os.path.basename(src)[:-2] + ".o")
        if cancel():
            return obj
        p = subprocess.run([zig, "cc"] + cflags + ["-c", src, "-o", obj], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        with lock:
            done[0] += 1
            if p.returncode:
                failures.append((src, p.stdout.decode("utf-8", "replace")))
            progress("compile", done[0], len(srcs))
        return obj

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        objs = [f.result() for f in as_completed([ex.submit(one, s) for s in srcs])]
    if cancel():
        raise BuildError("stopped")
    if failures:
        src, out = failures[0]
        raise BuildError("compiling %s failed:\n%s" % (os.path.basename(src), out[-3000:]))
    return sorted(objs)


def build(game, out, sdk, work, jobs=None, zig=None, log=print, progress=lambda step, done, total: None,
          cancel=lambda: False):
    """Makes `out` (wwhd.nro) from the player's game; returns the seconds each step took."""
    t0 = time.time()
    jobs = jobs or max(1, (os.cpu_count() or 2))
    manifest_path = os.path.join(sdk, "manifest.json")
    if not os.path.isfile(manifest_path):
        raise BuildError("no Switch SDK at %s (manifest.json missing)" % sdk)
    with open(manifest_path) as f:
        manifest = json.load(f)
    if manifest.get("recompiler") != sdk_prelink.recompiler_revision(REPO):
        raise BuildError("the Switch SDK was built for another version of the recompiler: use the SDK of the same release")
    work = os.path.abspath(work)
    os.makedirs(work, exist_ok=True)
    progress("prepare", 0, 1)
    rpx, tid = prepare_game(game, work, log)
    region = check_rpx(rpx, tid)
    log("The Wind Waker HD (%s), version 0." % {"eu": "Europe", "us": "USA"}[region])
    zig = zig or get_zig(os.path.join(work, "toolchain"), log)
    gen = os.path.join(work, "gen-" + region)
    progress("translate", 0, 1)
    log("Translating the game code to C...")
    translate(rpx, region, gen, log)
    if cancel():
        raise BuildError("stopped")
    t1 = time.time()
    sdk = os.path.abspath(sdk).replace("\\", "/")
    subs = {"{sdk}": sdk, "{gen}": gen.replace("\\", "/")}

    def sub(arg):
        for k, v in subs.items():
            arg = arg.replace(k, v)
        return arg
    log("Compiling it for the Switch (%d at a time)..." % jobs)
    objs = compile_all(zig, [sub(f) for f in manifest["gamecode_cflags"]], gen, os.path.join(work, "obj-" + region),
                       jobs, progress, cancel)
    t2 = time.time()

    progress("link", 0, 1)
    log("Linking...")
    name = manifest.get("module_name", "wwhd")
    modname_c = os.path.join(work, "modname.c")
    with open(modname_c, "w") as f:  # what devkitPro's ld --nx-module-name adds: {0, length, name}
        f.write('__attribute__((section(".nx-module-name"), used)) static const struct '
                '{ unsigned zero, length; char name[%d]; } module_name = {0, %d, "%s"};\n' % (len(name) + 1, len(name), name))
    modname_o = os.path.join(work, "modname.o")
    run([zig, "cc", "-target", "aarch64-linux-musl", "-c", "-fPIC", modname_c, "-o", modname_o], log, "compiling")
    elf = os.path.join(work, "wwhd.elf")
    args = [sub(f) for f in manifest["link"]] + ["-o", elf, os.path.join(sdk, "runtime.o"), modname_o]
    args += objs + [sub(f) for f in manifest["link_after"]]
    rsp = os.path.join(work, "link.rsp")  # Windows: the object list exceeds the command line limit
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % x.replace("\\", "/") for x in args))
    run([zig, "ld.lld", "@" + rsp], log, "linking")
    info = manifest.get("nacp", {})
    nacp = nro.make_nacp(info.get("name", "Wind Waker HD"), info.get("author", "WindWakerHDNX"), info.get("version", "0.1"))
    with open(os.path.join(sdk, "icon.jpg"), "rb") as f:
        icon = f.read()
    with open(elf, "rb") as f:
        data = nro.elf_to_nro(f.read(), icon, nacp)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    os.remove(elf)
    t3 = time.time()
    progress("done", 1, 1)
    log("Done: %s (%.1f MB). Translating %.0f s, compiling %.0f s, linking %.0f s." % (
        out, len(data) / 1e6, t1 - t0, t2 - t1, t3 - t2))
    return {"translate": t1 - t0, "compile": t2 - t1, "link": t3 - t2, "region": region}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True)
    default_sdk = os.path.join(REPO, "sdk") if os.path.isfile(os.path.join(REPO, "sdk", "manifest.json")) else \
        os.path.join(REPO, "sdk-switch")
    ap.add_argument("--sdk", default=default_sdk)
    ap.add_argument("--out", default="wwhd.nro")
    ap.add_argument("--work", default=os.path.join("build", "switch-builder"))
    ap.add_argument("--jobs", type=int)
    ap.add_argument("--zig", help="an existing zig 0.16.0")
    a = ap.parse_args()
    last = [0]

    def progress(step, done, total):
        if step == "compile" and (done == total or time.time() - last[0] > 2):
            last[0] = time.time()
            print("  compiled %d of %d files" % (done, total), flush=True)
    try:
        build(a.game, a.out, a.sdk, a.work, a.jobs, a.zig, log=lambda s: print(s, flush=True), progress=progress)
    except BuildError as e:
        print("error: %s" % e)
        sys.exit(1)
    print("Copy it to sdmc:/switch/wwhd/wwhd.nro, and the game (.wua) to sdmc:/switch/wwhd/ (docs/switch.md).")


if __name__ == "__main__":
    main()
