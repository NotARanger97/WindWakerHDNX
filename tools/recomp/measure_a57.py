#!/usr/bin/env python3
"""Repeatable static A57 metric; generated/game-derived artifacts stay in build/.

Select the profile's functions plus a deterministic, evenly spaced broad sample.
Assembly counts are actual instructions (including host spills); Cpu/guest counts
are optimized LLVM IR memory operations, before AArch64 pairing/spill lowering.
These are static totals, not execution counts or a frame-time prediction.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import subprocess


def functions(gen):
    result = {}
    for path in sorted(gen.glob("code_*.c")):
        src = path.read_text()
        starts = list(re.finditer(r"^void (f_[0-9A-F]{8}(?:_abi|_sync|_orig)?)\(.*?\) \{", src, re.M))
        for i, m in enumerate(starts):
            body = src[m.start():starts[i + 1].start() if i + 1 < len(starts) else len(src)]
            result[m[1]] = body.strip()
    return result


def assembly_counts(src):
    result, current, pending = {}, None, None
    for line in src.splitlines():
        decl = re.match(r"\s*\.type\s+(\w+),\s*@function", line)
        if decl:
            pending = decl[1]
        m = re.match(r"(\w+):", line)
        if m and m[1] == pending:
            current = m[1]
            result[current] = collections.Counter()
        if current and re.match(r"\s*\.size\s+" + current + r"\b", line):
            current = None
        m = re.match(r"\s+([a-z][a-z0-9.]*)\s*(.*)", line)
        if not current or not m:
            continue
        op = m[1]
        c = result[current]
        c["instructions"] += 1
        if op.startswith(("ld", "st")):
            c["loads" if op.startswith("ld") else "stores"] += 1
        if op in ("b", "bl", "blr", "br", "ret", "cbz", "cbnz", "tbz", "tbnz") or op.startswith("b."):
            c["branches"] += 1
        if op.startswith("rev"):
            c["endian_swaps"] += 1
        if op in ("bl", "blr"):
            c["calls"] += 1
            if m[2].startswith("psq_"):
                c["psq_slow_calls"] += 1
    return result


def ir_counts(src):
    result = {}
    for m in re.finditer(r"^define .*?@(f_[0-9A-F]{8}(?:_abi|_sync|_orig)?)\(ptr[^\n]*?(%\w+)[^\n]*\{\n(.*?)^}", src, re.M | re.S):
        name, cpu, body = m.groups()
        roots = {cpu: "cpu", "@g_ppc_mem_base": "base"}
        # Pointer SSA dependencies, including loops/phis. A load from the global
        # base produces a guest pointer; other pointer loads are not Cpu aliases.
        defs = {}
        for line in body.splitlines():
            d = re.match(r"\s*(%\w+) = (.*)", line)
            if d:
                defs[d[1]] = d[2]
        for _ in range(len(defs) + 1):
            changed = False
            for dest, rhs in defs.items():
                if dest in roots:
                    continue
                refs = re.findall(r"[%@][\w.]+", rhs)
                kind = None
                if rhs.startswith("load ptr") and "@g_ppc_mem_base" in refs:
                    kind = "guest"
                elif rhs.startswith(("getelementptr", "phi ptr", "select i1", "bitcast ptr")):
                    kinds = {roots[r] for r in refs if r in roots} - {"base"}
                    if len(kinds) == 1:
                        kind = kinds.pop()
                if kind:
                    roots[dest] = kind
                    changed = True
            if not changed:
                break
        c = collections.Counter()
        for line in body.splitlines():
            mem = re.search(r"\b(load|store) (?:volatile |atomic )?(.+?), ptr ([%@][\w.]+)", line)
            if mem:
                kind = roots.get(mem[3], "other")
                c[kind + "_" + mem[1] + "s"] += 1
        result[name] = c
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("gen", type=Path)
    p.add_argument("out", type=Path)
    p.add_argument("--cc", required=True)
    p.add_argument("--hot", type=Path, required=True)
    p.add_argument("--sample", type=int, default=128)
    p.add_argument("--manifest", type=Path, help="reuse the baseline's exact selection")
    p.add_argument("--include", type=Path, default=Path(__file__).resolve().parents[2] / "runtime/include",
                   help="runtime headers, e.g. a saved baseline ppc.h")
    args = p.parse_args()
    all_funcs = functions(args.gen)
    hot = ["f_" + a.upper() for a in re.findall(r"f_([0-9A-Fa-f]{8})", args.hot.read_text())]
    candidates = sorted(n for n, b in all_funcs.items() if re.search(r"/\* [0-9A-F]{8}: [0-9A-F]{8} \*/", b))
    canonical = {n[:10]: n for n in candidates if not n.endswith("_orig")}
    if args.manifest:
        selection = json.loads(args.manifest.read_text())
        selection = {group: [canonical[n[:10]] for n in names] for group, names in selection.items()}
    else:
        selection = {"hot": [canonical[n] for n in hot], "broad": []}
        for i in range(args.sample):
            n = candidates[i * len(candidates) // args.sample]
            if n not in selection["hot"]:
                selection["broad"].append(n)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "manifest.json").write_text(json.dumps(selection, indent=2))
    # Cross-compiling needs only these libc declarations, not a target sysroot.
    # Clang's stdint.h provides the real target integer types. Retain builtin
    # memcpy/FP handling: -ffreestanding would produce spurious library calls.
    headers = args.out / "headers"
    headers.mkdir(exist_ok=True)
    (headers / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\nvoid *memmove(void *, const void *, size_t);\nvoid *memset(void *, int, size_t);\n")
    (headers / "math.h").write_text("#define isnan(x) __builtin_isnan(x)\ndouble sqrt(double);\ndouble fabs(double);\ndouble fma(double,double,double);\ndouble nearbyint(double);\ndouble trunc(double);\ndouble ceil(double);\ndouble floor(double);\n")
    source = args.out / "sample.c"
    names = selection["hot"] + selection["broad"]
    source.write_text('#include "funcs.h"\n' + "\n\n".join(all_funcs[n] for n in names))
    resource = subprocess.check_output([args.cc, "-print-resource-dir"], text=True).strip()
    flags = [args.cc, "--target=aarch64-linux-gnu", "-std=gnu11", "-O3",
             "-march=armv8-a+crc+crypto", "-mtune=cortex-a57",
             "-ffp-contract=off", "-fno-strict-aliasing", "-nostdinc",
             "-isystem", str(Path(resource) / "include"),
             "-I", str(headers), "-I", str(args.include),
             "-I", str(args.gen), "-D__SWITCH__", "-S", str(source)]
    subprocess.run(flags + ["-o", str(args.out / "sample.s")], check=True)
    subprocess.run(flags + ["-emit-llvm", "-o", str(args.out / "sample.ll")], check=True)
    asm = assembly_counts((args.out / "sample.s").read_text())
    ir = ir_counts((args.out / "sample.ll").read_text())
    rows = {}
    for name in names:
        rows[name] = {"ppc": len(re.findall(r"/\* [0-9A-F]{8}: [0-9A-F]{8} \*/", all_funcs[name])),
                      "round25": all_funcs[name].count("round25("),
                      "to_single": all_funcs[name].count("to_single("),
                      **asm[name], **ir.get(name, {})}
    summary = {}
    for group, group_names in selection.items():
        totals = collections.Counter()
        for name in group_names:
            totals.update(rows[name])
        summary[group] = {"functions": len(group_names), **totals,
                          "expansion": totals["instructions"] / totals["ppc"] if totals["ppc"] else 0}
    version = subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0]
    helpers = {n: dict(c) for n, c in asm.items() if n not in names}
    report = {"compiler": version, "flags": flags[1:], "summary": summary,
              "shared_helpers": helpers, "functions": rows}
    (args.out / "metrics.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(summary, indent=2))
    if helpers:
        print("Outlined helpers (separate from the function totals):", json.dumps(helpers))


if __name__ == "__main__":
    main()
