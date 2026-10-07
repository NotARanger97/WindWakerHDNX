"""Makes WindWakerHDNX.exe: the Switch builder as one Windows program (tools/switch/launcher.c).

  python3 tools/switch/package_builder.py --sdk sdk-switch --version 0.1.0 [--out dist/WindWakerHDNX.exe]

The program is the launcher followed by a zip and a trailer. The zip holds: a private Python with Tk
(python-build-standalone, pinned), zig 0.16.0 (pinned), the Switch SDK (tools/switch/build.sh --sdk), the
recompiler, tools/switch and the runtime sources the recompiler scans. None of it is game code or data:
the player's own .wua becomes wwhd.nro on their PC. Downloads are cached in build/package-cache.
"""
import argparse
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import builder  # noqa: E402  (the pinned zig)

PYTHON = ("https://github.com/astral-sh/python-build-standalone/releases/download/20261003/"
          "cpython-3.14.8%2B20261003-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
          "10e5705e44938ee78de35c62b30fdfe2945b53438343d04c0f53548cdd8e91b6")
SWITCH_TOOLS = ["builder.py", "builder_gui.py", "nro.py", "sdk_prelink.py", "wua.py"]
README = """Wind Waker HD NX - the Switch builder
=====================================

Start WindWakerHDNX.exe, choose your own Wind Waker HD (Wii U) Cemu archive (.wua), press Build, then
"Copy to SD card". Its files live in %LOCALAPPDATA%\\WindWakerHDNX (delete that folder to remove it).

Source, documentation and licenses: https://github.com/NotARanger97/WindWakerHDNX
Bundled: Python (PSF license, python/LICENSE.txt), zig 0.16.0 (MIT, zig/LICENSE), the Switch SDK
(sdk/LICENSES.txt), the recompiler and runtime sources of this repository (MPL-2.0, LICENSE).
"""


def cached(url, sha, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, os.path.basename(url).replace("%2B", "+"))
    if not (os.path.isfile(path) and hashlib.sha256(open(path, "rb").read()).hexdigest() == sha):
        print("downloading", url, flush=True)
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "wwhd-package"})) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != sha:
            raise SystemExit("checksum mismatch: %s" % url)
        with open(path, "wb") as f:
            f.write(data)
    return path


def add_file(z, src, arc):
    z.write(src, arc)


def add_tree(z, src, arc, keep=lambda p: True):
    for dp, dns, fns in os.walk(src):
        dns.sort()
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            if keep(p):
                z.write(p, os.path.join(arc, os.path.relpath(p, src)).replace("\\", "/"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", default=os.path.join(REPO, "dist", "WindWakerHDNX.exe"))
    ap.add_argument("--zig", help="zig used to compile the launcher (default: the bundled one)")
    a = ap.parse_args()
    cache = os.path.join(REPO, "build", "package-cache")
    stage = os.path.join(REPO, "build", "package-stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)

    # pinned Python and zig for Windows x86-64
    py = cached(*PYTHON, cache)
    with tarfile.open(py) as t:
        t.extractall(stage)  # python/
    zurl, zsha = builder.ZIG[("windows", "x86_64")]
    zz = cached(zurl, zsha, cache)
    with zipfile.ZipFile(zz) as z:
        z.extractall(stage)
    zdir = os.path.join(stage, os.path.basename(zurl)[:-4])
    os.replace(zdir, os.path.join(stage, "zig"))
    zig = a.zig or os.path.join(stage, "zig", "zig.exe")

    # the launcher
    launcher = os.path.join(stage, "launcher.exe")
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O2", "-municode", "-Wl,--subsystem,windows",
                    "-o", launcher, os.path.join(HERE, "launcher.c"), os.path.join(HERE, "third_party", "miniz", "miniz.c"),
                    "-lcomctl32", "-lshell32", "-luser32", "-lgdi32"], check=True)

    payload = io.BytesIO()
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        add_tree(z, os.path.join(stage, "python"), "python")
        add_tree(z, os.path.join(stage, "zig"), "zig")
        add_tree(z, a.sdk, "sdk")
        add_file(z, os.path.join(REPO, "tools", "rpx.py"), "tools/rpx.py")
        add_tree(z, os.path.join(REPO, "tools", "recomp"), "tools/recomp",
                 lambda p: (p.endswith(".py") or p.endswith(".txt")) and "__pycache__" not in p)
        for name in SWITCH_TOOLS:
            add_file(z, os.path.join(HERE, name), "tools/switch/" + name)
        add_tree(z, os.path.join(REPO, "runtime", "src"), "runtime/src",
                 lambda p: os.path.splitext(p)[1] in (".c", ".cpp", ".h", ".mm", ".inc"))
        add_tree(z, os.path.join(REPO, "runtime", "include"), "runtime/include")
        add_file(z, os.path.join(REPO, "LICENSE"), "LICENSE")
        z.writestr("README.txt", README)
    data = payload.getvalue()

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(launcher, "rb") as f:
        exe = f.read()
    version = a.version.encode()[:31]
    trailer = struct.pack("<QQ32s8s", len(exe), len(data), version, b"WWHDNXP1")
    with open(a.out, "wb") as f:
        f.write(exe + data + trailer)
    print("%s: %.1f MB (launcher %d KB, payload %.1f MB, version %s)" % (
        a.out, os.path.getsize(a.out) / 1e6, len(exe) // 1024, len(data) / 1e6, a.version))


if __name__ == "__main__":
    main()
