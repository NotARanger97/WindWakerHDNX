"""Builds the Switch homebrew (wwhd.nro) on a PC from your own copy of the game: no Docker, no devkitPro.

  python3 tools/switch/builder.py --game <dump> [--sdk DIR] [--out wwhd.nro] [--work DIR] [--jobs N]

  --game   the extracted game (the folder with code/, content/, meta/) or its code/cking.rpx
  --sdk    the Switch SDK (default: sdk-switch/ next to this repository, as in a builder release)
  --out    the homebrew to copy to sdmc:/switch/wwhd/wwhd.nro (default: ./wwhd.nro)
  --work   working directory for the translated and compiled game code (default: ./build/switch-builder)

What it does: translates the game's PowerPC code to C with this repository's recompiler, compiles it for
the Switch's Cortex-A57 with clang from the pinned zig (downloaded once, checksum verified), links it with
the SDK's prebuilt runtime using zig's lld, and packs the result as an NRO (tools/switch/nro.py). The SDK
(tools/switch/sdk_prelink.py) holds no game code; what this script makes from your dump stays on your PC.
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
# title IDs of the supported releases (version 0, without the update): the USA one has the recompiler's
# hooks; the European executable is recompiled without them
REGIONS = {"0005000010143500": "us", "0005000010143600": "eu"}


def say(text):
    print(text, flush=True)


def fail(text):
    say("error: " + text)
    sys.exit(1)


def host():
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x64": "x86_64", "arm64": "aarch64"}.get(machine, machine)
    return system, arch


def get_zig(work):
    system, arch = host()
    if (system, arch) not in ZIG:
        fail("no pinned zig for %s %s" % (system, arch))
    url, sha = ZIG[(system, arch)]
    name = os.path.basename(url).replace(".tar.xz", "").replace(".zip", "")
    root = os.path.join(work, "toolchain")
    exe = os.path.join(root, name, "zig.exe" if system == "windows" else "zig")
    marker = os.path.join(root, name, ".sha256")
    if os.path.isfile(exe) and os.path.isfile(marker) and open(marker).read().strip() == sha:
        return exe
    os.makedirs(root, exist_ok=True)
    archive = os.path.join(root, os.path.basename(url))
    say("Getting the compiler (zig 0.16.0, once): %s" % url)
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
        fail("the zig download is corrupt or was changed (SHA-256 mismatch)")
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


def find_game(path):
    """(cking.rpx, region) from a game folder or the rpx itself; the region from meta/meta.xml's title ID."""
    path = os.path.abspath(path)
    if os.path.isfile(path) and path.lower().endswith(".rpx"):
        rpx, top = path, os.path.dirname(os.path.dirname(path))
    else:
        rpx, top = os.path.join(path, "code", "cking.rpx"), path
    if not os.path.isfile(rpx):
        fail("no code/cking.rpx in %s (choose the extracted game folder or its cking.rpx)" % path)
    region = None
    meta = os.path.join(top, "meta", "meta.xml")
    if os.path.isfile(meta):
        text = open(meta, encoding="utf-8", errors="replace").read()
        for tid, reg in REGIONS.items():
            if tid in text.lower():
                region = reg
    return rpx, region


def translate(rpx, region, gen):
    if os.path.isfile(os.path.join(gen, "table.c")):
        say("Game code already translated (%s)" % gen)
        return
    say("Translating the game code (%s release)..." % region.upper())
    shutil.rmtree(gen, ignore_errors=True)
    env = dict(os.environ)
    if region == "eu":
        hooks = os.path.join(os.path.dirname(gen), "no-hooks.txt")
        open(hooks, "w").close()
        env["WWHD_HOOKS"] = hooks
    recomp = os.path.join(REPO, "tools", "recomp")
    subprocess.run([sys.executable, os.path.join(recomp, "recomp.py"), rpx, gen], env=env, check=True)
    if region == "eu":
        subprocess.run([sys.executable, os.path.join(recomp, "region_compat.py"), gen], check=True)


def compile_all(zig, cflags, gen, obj_dir, jobs):
    os.makedirs(obj_dir, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(gen, "code_*.c"))) + [os.path.join(gen, "table.c"), os.path.join(gen, "imports.c")]
    srcs.sort(key=lambda s: -os.path.getsize(s))  # big files first
    total, done, failures = len(srcs), [0], []

    def one(src):
        obj = os.path.join(obj_dir, os.path.basename(src)[:-2] + ".o")
        p = subprocess.run([zig, "cc"] + cflags + ["-c", src, "-o", obj], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return src, obj, p.returncode, p.stdout.decode("utf-8", "replace")

    objs = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for f in as_completed([ex.submit(one, s) for s in srcs]):
            src, obj, rc, out = f.result()
            done[0] += 1
            if rc:
                failures.append((src, out))
            objs.append(obj)
            print("\r  compiled %d of %d files" % (done[0], total), end="", flush=True)
    print()
    if failures:
        src, out = failures[0]
        fail("compiling %s failed:\n%s" % (os.path.basename(src), out[-3000:]))
    return sorted(objs)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--sdk", default=os.path.join(REPO, "sdk-switch"))
    ap.add_argument("--out", default="wwhd.nro")
    ap.add_argument("--work", default=os.path.join("build", "switch-builder"))
    ap.add_argument("--region", choices=("us", "eu"), help="only needed when the dump has no meta/meta.xml")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--zig", help="an existing zig 0.16.0 instead of downloading it")
    a = ap.parse_args()
    t0 = time.time()

    manifest_path = os.path.join(a.sdk, "manifest.json")
    if not os.path.isfile(manifest_path):
        fail("no Switch SDK at %s (manifest.json missing)" % a.sdk)
    manifest = json.load(open(manifest_path))
    if manifest.get("recompiler") != sdk_prelink.recompiler_revision(REPO):
        fail("this SDK was built for another version of the recompiler: use the SDK of the same release")
    rpx, region = find_game(a.game)
    region = a.region or region
    if not region:
        fail("cannot tell the game's region (no meta/meta.xml): pass --region us or --region eu")
    work = os.path.abspath(a.work)
    gen = os.path.join(work, "gen-" + region)
    zig = a.zig or get_zig(work)

    translate(rpx, region, gen)
    t1 = time.time()
    sdk = os.path.abspath(a.sdk).replace("\\", "/")
    subs = {"{sdk}": sdk, "{gen}": gen.replace("\\", "/")}

    def sub(arg):
        for k, v in subs.items():
            arg = arg.replace(k, v)
        return arg
    cflags = [sub(f) for f in manifest["gamecode_cflags"]]
    say("Compiling the game code for the Switch (%d at a time)..." % a.jobs)
    objs = compile_all(zig, cflags, gen, os.path.join(work, "obj-" + region), a.jobs)
    t2 = time.time()

    say("Linking...")
    name = manifest.get("module_name", "wwhd")
    modname_c = os.path.join(work, "modname.c")
    with open(modname_c, "w") as f:  # what devkitPro's ld --nx-module-name adds: {0, length, name}
        f.write('__attribute__((section(".nx-module-name"), used)) static const struct '
                '{ unsigned zero, length; char name[%d]; } module_name = {0, %d, "%s"};\n' % (len(name) + 1, len(name), name))
    modname_o = os.path.join(work, "modname.o")
    subprocess.run([zig, "cc", "-target", "aarch64-linux-musl", "-c", "-fPIC", modname_c, "-o", modname_o], check=True)
    elf = os.path.join(work, "wwhd.elf")
    link = [zig, "ld.lld"] + [sub(f) for f in manifest["link"]] + ["-o", elf, os.path.join(a.sdk, "runtime.o"), modname_o]
    link += objs + [sub(f) for f in manifest["link_after"]]
    rsp = os.path.join(work, "link.rsp")  # Windows: the object list exceeds the command line limit
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % x.replace("\\", "/") for x in link[2:]))
    subprocess.run([zig, "ld.lld", "@" + rsp], check=True)

    info = manifest.get("nacp", {})
    nacp = nro.make_nacp(info.get("name", "Wind Waker HD"), info.get("author", "WindWakerHDNX"), info.get("version", "0.1"))
    icon = open(os.path.join(a.sdk, "icon.jpg"), "rb").read()
    with open(elf, "rb") as f:
        data = nro.elf_to_nro(f.read(), icon, nacp)
    with open(a.out, "wb") as f:
        f.write(data)
    t3 = time.time()
    say("Done: %s (%.1f MB). Translate %.0f s, compile %.0f s, link %.0f s." % (
        a.out, len(data) / 1e6, t1 - t0, t2 - t1, t3 - t2))
    say("Copy it to sdmc:/switch/wwhd/wwhd.nro (docs/switch.md: Installing).")


if __name__ == "__main__":
    main()
