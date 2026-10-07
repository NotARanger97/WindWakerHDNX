"""Switch SDK for the PC builder (cmake/switch.cmake, WWHD_SWITCH_SDK=ON; runs inside the devkitA64 build).

Writes <out>/:
  runtime.o      the runtime (renderer, GX2, HLE, host) with its libraries, libnx, newlib, libstdc++ and
                 libgcc, prelinked into one relocatable object without debug info; only the game code's
                 symbols stay undefined
  crtend.o crtn.o  the end of the C runtime's init/fini sections, linked after the game code
  switch.ld      libnx's linker script, adjusted for LLVM's lld (see patch_linker_script)
  include/       the runtime headers the recompiled code includes (ppc.h ...)
  icon.jpg       the hbmenu icon
  manifest.json  compile and link flags for the builder, the recompiler revision this runtime expects
  LICENSES.txt   what runtime.o contains and under which licenses
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

# zig's clang for the Switch's Cortex-A57. A hosted target, so memcpy, fabs ... are builtins (inline);
# aarch64-linux-musl's headers give the same code as newlib's; the Linux macros are removed.
GAMECODE_CFLAGS = [
    "-target", "aarch64-linux-musl", "-mcpu=cortex_a57+crc+crypto", "-O2", "-fPIC",
    "-ffunction-sections", "-fdata-sections", "-ffp-contract=off", "-fno-strict-aliasing", "-std=gnu11", "-w",
    "-D__SWITCH__", "-U__linux__", "-U__linux", "-Ulinux", "-U__gnu_linux__", "-U__unix__", "-U__unix", "-Uunix",
    "-I{sdk}/include", "-I{gen}",
]
# devkitPro's switch.specs link, for ld.lld (its --spare-dynamic-tags=0 is lld's default; --nx-module-name
# is a devkitPro ld extension: the builder adds the .nx-module-name section itself)
LINK_FLAGS = [
    "-T", "{sdk}/switch.ld", "-pie", "--no-dynamic-linker", "--gc-sections", "-z", "text", "-z", "now",
    "-z", "nodynamic-undefined-weak", "-z", "pack-relative-relocs", "--build-id=sha1", "-e", "_start",
]
# C library functions compiled game code could call (normally inlined): kept in runtime.o regardless
# linked as separate archives (LGPL: the player can relink with a modified one)
LGPL_LIBS = {"libelf.a"}
LIBC_KEEP = ["memcpy", "memmove", "memset", "memcmp", "sqrt", "sqrtf", "fabs", "fabsf", "fma", "fmaf", "floor",
             "ceil", "trunc", "round", "fmod", "copysign", "frexp", "ldexp", "abort"]


def patch_linker_script(text):
    """libnx's switch.ld for lld, with the same layout GNU ld makes of it:
    - .relr.dyn takes .relr.dyn only (lld adds an empty .relr.auth.dyn of another section type)
    - a PT_TLS segment over .tdata/.tbss (lld wants one for TLS symbols; GNU ld takes the first TLS
      section without it: the same thread-pointer offsets)
    - the main thread's TLS reservation is NOLOAD (lld would otherwise store .bss as zeros in the file)"""
    out = text.replace(".relr.dyn : { *(.relr.*) }", ".relr.dyn : { *(.relr.dyn) }")
    out = out.replace("\tdyn    PT_DYNAMIC;", "\tdyn    PT_DYNAMIC;\n\ttls    PT_TLS;")
    for sec in (".tdata", ".tbss"):
        i = out.index("\t" + sec + " :")
        j = out.index("} :data", i)
        out = out[:j] + "} :data :tls" + out[j + len("} :data"):]
    out = out.replace(".main.tls ALIGN(MAX(ALIGNOF(.tdata),ALIGNOF(.tbss))) :",
                      ".main.tls ALIGN(MAX(ALIGNOF(.tdata),ALIGNOF(.tbss))) (NOLOAD) :")
    for needed in ("*(.relr.dyn) }", "tls    PT_TLS;", "} :data :tls", "(NOLOAD)"):
        if needed not in out:
            raise SystemExit("switch.ld changed upstream: cannot adjust it for lld (%s)" % needed)
    return out


def recompiler_revision(source):
    """The builder must translate the game with the recompiler this runtime was built with."""
    h = hashlib.sha256()
    rdir = os.path.join(source, "tools", "recomp")
    for name in sorted(os.listdir(rdir)):
        if name.endswith(".py") and not name.startswith("test_"):
            with open(os.path.join(rdir, name), "rb") as f:
                h.update(name.encode() + b"\0" + f.read())
    return h.hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ld", required=True)
    ap.add_argument("--gcc-lib", required=True)
    ap.add_argument("--devkitpro", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--libs", nargs="+", required=True)
    ap.add_argument("--objects", nargs="+", required=True)
    a = ap.parse_args()
    dkp, gcc = a.devkitpro, a.gcc_lib
    a64 = os.path.join(dkp, "devkitA64", "aarch64-none-elf")
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    runtime = os.path.join(a.out, "runtime.o")
    cmd = [a.ld, "-r", "--strip-debug", "-EL", "-maarch64elf", "-o", runtime,
           os.path.join(gcc, "pic", "crti.o"), os.path.join(gcc, "pic", "crtbegin.o")]
    # LGPL libraries stay separate archives (replaceable at the final link), the rest is prelinked
    separate = [l for l in a.libs if os.path.basename(l) in LGPL_LIBS]
    cmd += a.objects + [l for l in a.libs if l not in separate]
    for d in (os.path.join(dkp, "libnx", "lib"), os.path.join(gcc, "pic"), os.path.join(a64, "lib", "pic"), gcc,
              os.path.join(a64, "lib")):
        cmd.append("-L" + d)
    for sym in ["_start", "main"] + LIBC_KEEP:
        cmd += ["-u", sym]
    cmd += ["-lnx", "-lm", "-lstdc++", "--start-group", "-lgcc", "-lg", "-lc", "-lsysbase", "--end-group"]
    subprocess.run(cmd, check=True)

    # what stays undefined must be the game code's (its function table and data)
    nm = os.path.join(os.path.dirname(a.ld), "aarch64-none-elf-nm")
    undefined = subprocess.run([nm, "-u", "--format=just-symbols", runtime], check=True,
                               stdout=subprocess.PIPE, text=True).stdout.split()
    for name in ("crtend.o", "crtn.o"):
        shutil.copy(os.path.join(gcc, "pic", name), a.out)
    for lib in separate:
        shutil.copy(lib, a.out)
    with open(os.path.join(dkp, "libnx", "switch.ld")) as f:
        script = patch_linker_script(f.read())
    with open(os.path.join(a.out, "switch.ld"), "w") as f:
        f.write(script)
    shutil.copytree(os.path.join(a.source, "runtime", "include"), os.path.join(a.out, "include"))
    shutil.copy(os.path.join(dkp, "libnx", "default_icon.jpg"), os.path.join(a.out, "icon.jpg"))
    gcc_version = os.path.basename(gcc.rstrip("/"))
    manifest = {
        "platform": "switch",
        "toolchain": "zig-0.16.0",
        "recompiler": recompiler_revision(a.source),
        "gamecode_cflags": GAMECODE_CFLAGS,
        "link": LINK_FLAGS,
        # after the game code: the LGPL archives, then the end of the C runtime
        "link_after": ["{sdk}/%s" % os.path.basename(l) for l in separate] + ["{sdk}/crtend.o", "{sdk}/crtn.o"],
        "module_name": "wwhd",
        "nacp": {"name": "Wind Waker HD", "author": "WindWakerHDNX", "version": "0.1"},
        "runtime_undefined": sorted(undefined),
        "built_with": "devkitA64 GCC %s, libnx" % gcc_version,
    }
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(a.out, "LICENSES.txt"), "w") as f:
        f.write(LICENSES)
    size = os.path.getsize(runtime)
    print("switch sdk: %s (runtime.o %.1f MiB, %d symbols from the game code)" % (a.out, size / 2**20, len(undefined)))


LICENSES = """runtime.o contains, statically linked:
- the Wind Waker HD recompilation runtime (MPL-2.0; this repository) with Cemu's GPU address library and
  shader decompiler (MPL-2.0), Dear ImGui (MIT), ZArchive (MIT-0), glslang (BSD-3-Clause and others)
- Mesa's NVK Vulkan driver for Horizon (MIT) with libdrm_nouveau (MIT), expat (MIT), zstd (BSD-3-Clause),
  lz4 (BSD-2-Clause), zlib (zlib)
- libnx (ISC), newlib (BSD-style licenses) and devkitPro's libsysbase (MPL-2.0)
- libstdc++ and libgcc from GCC (GPL-3.0 with the GCC Runtime Library Exception)
libelf.a (devkitPro's Switch port of libelf, LGPL) is a separate archive linked at build time, so it can be
replaced with a modified build; its source is at https://github.com/devkitPro/pacman-packages (switch-libelf).
crtend.o and crtn.o are GCC's (GPL-3.0 with the GCC Runtime Library Exception); switch.ld is libnx's (ISC),
adjusted for lld; icon.jpg is libnx's default icon (ISC).
No game code or game data is included: the builder makes the game code from the player's own dump.
"""

if __name__ == "__main__":
    main()
