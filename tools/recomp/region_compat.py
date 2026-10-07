#!/usr/bin/env python3
"""Make a recompiled build/gen of another region (EU) link with the runtime.

usage: region_compat.py GEN_DIR

The runtime's hooks, 60 fps, aspect-ratio and mod code name USA function addresses
(f_XXXXXXXX, f_XXXXXXXX_orig, hook_XXXXXXXX, site_XXXXXXXX). A build/gen made from another
region's cking.rpx with WWHD_HOOKS=<its own list> defines none of the USA-only ones. This
script declares all of them in funcs.h and defines every missing one as a stub that halts via
ppc_unimplemented, so the runtime links. Those features must stay off (they are off by default)
until their addresses are ported to the region.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stubgen import hook_lists, runtime_refs  # noqa: E402


def main(gen):
    defined = set()
    pat = re.compile(r"^void (f_[0-9A-F]{8}(?:_orig)?)\(Cpu\* __restrict c\)\s*\{", re.M)
    for name in os.listdir(gen):
        if name.startswith("code_") and name.endswith(".c"):
            with open(os.path.join(gen, name)) as f:
                defined.update(pat.findall(f.read()))
    hooks, sites = hook_lists()
    wanted = {"f_%08X" % a for a in runtime_refs() | hooks} | {"f_%08X_orig" % a for a in hooks}
    missing = sorted(wanted - defined)
    funcs_h = os.path.join(gen, "funcs.h")
    with open(funcs_h) as f:
        text = f.read()
    marker = "\n/* region_compat.py: USA names used by the runtime */\n"
    if marker in text:
        text = text[:text.index(marker)]
    decl = [marker]
    for n in sorted(wanted):
        decl.append("void %s(Cpu* __restrict c);\n" % n)
    for a in sorted(hooks):
        decl.append("void hook_%08X(Cpu* c);\n" % a)
    for a in sorted(sites):
        decl.append("void site_%08X(Cpu* c);\n" % a)
    with open(funcs_h, "w") as f:
        f.write(text + "".join(decl))
    with open(os.path.join(gen, "code_compat.c"), "w") as f:
        f.write('#include "funcs.h"\n\n/* region_compat.py: USA-only functions, not present in this build */\n')
        for n in missing:
            f.write("void %s(Cpu* __restrict c) { ppc_unimplemented(c, 0x%su, 0); }\n" % (n, n[2:10]))
    clash = sorted(n for n in wanted & defined if not n.endswith("_orig"))
    print("%d stubs written, %d USA addresses are also function entries here (different functions!)" % (
        len(missing), len(clash)))


if __name__ == "__main__":
    main(sys.argv[1])
