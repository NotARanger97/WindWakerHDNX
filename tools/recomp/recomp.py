#!/usr/bin/env python3
"""Statically recompile a Wii U RPX into C.

usage: recomp.py game/code/cking.rpx OUTDIR [--insns-per-file N] [--no-locals] [--no-abi]
                 [--no-single-rounds] [--no-icache] [--no-sync-dataflow]
                 [--no-dead-flags] [--no-leaf-inputs]

Register locals are enabled by default; --no-locals or WWHD_RECOMP_LOCALS=0
restores direct Cpu register accesses (regenerate code when changing this).
ABI-aware synchronization is enabled by default; --no-abi or WWHD_RECOMP_ABI=0
restores full call barriers while retaining the register cache.

Output:
  OUTDIR/funcs.h         prototypes of every recompiled function and import
  OUTDIR/code_NNN.c      recompiled functions
  OUTDIR/table.c         guest address -> host function table
  OUTDIR/imports.c       weak default implementations of imported functions
  OUTDIR/imports.json    import slot addresses (for the runtime loader)
  OUTDIR/report.txt      statistics and unhandled instructions
"""
import bisect
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from analyze import Program, sext
from ppc2c import translate, Unhandled, RegisterLocals, custom_abi, expand_cr_helpers, cpu_calls, single_rounds, leaf_inputs, dead_flags
from rpx import R_PPC_ADDR16_HA, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI


def c_ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def branch_target(addr, w):
    """Static target of a non-linking b/bc, or None."""
    op = w >> 26
    if op == 18 and not (w & 1):
        return (sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else addr)) & 0xFFFFFFFF
    if op == 16 and not (w & 1):
        return (sext(w & 0xFFFC, 16) + (0 if w & 2 else addr)) & 0xFFFFFFFF
    return None


# imported data objects get runtime-owned storage at fixed addresses
DATA_IMPORT_BASE = 0xC1000000
DATA_IMPORT_STRIDE = 0x1000


class Recompiler:
    def __init__(self, path, register_locals=None, abi=None, single_precision=None,
                 indirect_cache=None, sync_dataflow=None, flag_liveness=None, leaf_read_inputs=None):
        self.register_locals = (os.environ.get("WWHD_RECOMP_LOCALS", "1") != "0"
                                if register_locals is None else register_locals)
        self.abi = (os.environ.get("WWHD_RECOMP_ABI", "1") != "0" if abi is None else abi)
        self.single_precision = (os.environ.get("WWHD_RECOMP_SINGLE_ROUNDS", "1") != "0"
                                 if single_precision is None else single_precision)
        self.indirect_cache = (os.environ.get("WWHD_RECOMP_ICACHE", "1") != "0"
                               if indirect_cache is None else indirect_cache)
        # Site caches in an array owned by the guest thread's Cpu (c->icache[site]) instead of
        # host-thread-local variables: -mtp=soft makes every TLS access a __aarch64_read_tp call.
        self.icache_in_cpu = os.environ.get("WWHD_RECOMP_ICACHE_TLS", "0") == "0"
        self.icache_sites = {}
        self.sync_dataflow = (os.environ.get("WWHD_RECOMP_SYNC_DATAFLOW", "1") != "0"
                              if sync_dataflow is None else sync_dataflow)
        self.flag_liveness = (os.environ.get("WWHD_RECOMP_DEAD_FLAGS", "1") != "0"
                              if flag_liveness is None else flag_liveness)
        self.leaf_read_inputs = (os.environ.get("WWHD_RECOMP_LEAF_INPUTS", "1") != "0"
                                 if leaf_read_inputs is None else leaf_read_inputs)
        self.used_imports = set()
        self.p = Program(path)
        self.p.discover()
        self.entries = set(self.p.entries)
        self.imports = {}  # slot address -> (lib, name, kind)
        self.data_import_addr = {}  # slot address -> runtime storage address
        for sym in self.p.rpx.symbols:
            if sym.import_lib and sym.type != 3:  # skip section symbols
                self.imports[sym.value] = (sym.import_lib, sym.name, sym.import_kind)
        for i, slot in enumerate(sorted(s for s, v in self.imports.items() if v[2] == "d")):
            self.data_import_addr[slot] = DATA_IMPORT_BASE + i * DATA_IMPORT_STRIDE
        self._imm_overrides()
        # game functions replaced by runtime hooks (tools/recomp/hooks.txt: one hex address per line)
        # plus optional extra lists (hooks_*.txt, e.g. debug probes)
        import glob
        here = os.path.dirname(os.path.abspath(__file__))
        self.hooks = set()
        # "@ADDR": instruction-level hook; site_ADDR(c) runs just before the instruction at ADDR
        # (also when ADDR is reached by a branch), so it can adjust what that instruction uses
        self.sites = set()
        hook_files = [os.path.join(here, "hooks.txt")] + sorted(glob.glob(os.path.join(here, "hooks_*.txt")))
        if os.environ.get("WWHD_HOOKS"):  # another region: its own hook list (empty file = no hooks)
            hook_files = [os.environ["WWHD_HOOKS"]]
        for hp in hook_files:
            if not os.path.exists(hp):
                continue
            for line in open(hp):
                line = line.split("#")[0].strip()
                if line.startswith("@"):
                    self.sites.add(int(line[1:], 16))
                elif line:
                    self.hooks.add(int(line, 16))
        self._fixpoint()
        self._analyze_abi()

    def _analyze_abi(self):
        """Prove leaf access/output contracts and legacy ABI eligibility.

        Walk reachable instructions, including jump-table cases and tail edges.
        The legacy _abi classification rejects any non-volatile access. Precise
        _sync contracts cover those accesses without assuming preservation.
        Reject nested guest entries so preemption cannot observe an ancestor's
        stale Cpu fields. Unknown code/targets fail closed.
        """
        self.abi_safe = set()
        self.abi_calls = frozenset()
        self.call_summaries = {}
        self.sync_safe = set()
        if not (self.abi and self.register_locals):
            return
        unsafe = set(self.hooks)
        for sym in self.p.rpx.symbols:
            if custom_abi(sym.name):
                unsafe.add(sym.value)
        # Precise leaf contracts need no EABI preservation assumption: even an
        # unnamed restore helper's non-volatile outputs are reloaded. The same
        # entry/preemption restrictions as _abi apply to these fast bodies.
        blocked = set(unsafe)
        contracts = {}
        import_accessed = {"c->r[%d]" % i for i in range(14)} | {
            "c->cr[%d]" % i for i in range(32) if i < 8 or i >= 20} | {"c->lr", "c->ctr"}
        import_modified = {f for f in import_accessed if RegisterLocals.volatile(f)}
        for start in self.sorted_entries:
            if start in unsafe:
                continue
            self.cur_start, self.cur_end = start, self.func_end(start)
            self.labels = set()
            accessed, modified = set(), set()
            body = {}
            seen, pending = set(), [start]
            while pending:
                addr = pending.pop()
                if not start <= addr < self.cur_end:
                    unsafe.add(start)  # fallthrough reaches a normal PPC_ENTER
                    blocked.add(start)
                    continue
                if addr in seen:
                    continue
                seen.add(addr)
                if addr in self.sites:
                    unsafe.add(start)
                    blocked.add(start)
                w = self.p.word(addr)
                try:
                    src = expand_cr_helpers(translate(addr, w, self))
                except (Unhandled, ValueError):
                    unsafe.add(start)
                    blocked.add(start)
                    continue
                body[addr] = (addr, w, src)
                for m in RegisterLocals.field.finditer(src):
                    accessed.add(m[0])
                    if RegisterLocals.write.match(src, m.end()):
                        modified.add(m[0])
                if any(name == "ppc_stwcx" for _, _, name, _ in cpu_calls(src)):
                    modified.update("c->cr[%d]" % i for i in range(4))
                    accessed.update(modified)
                if any(RegisterLocals.nonvolatile(m[0]) for m in RegisterLocals.field.finditer(src)):
                    unsafe.add(start)
                op, link = w >> 26, w & 1
                if op in (16, 18):
                    bits, mask = (26, 0x03FFFFFC) if op == 18 else (16, 0xFFFC)
                    tgt = (sext(w & mask, bits) + (0 if w & 2 else addr)) & 0xFFFFFFFF
                    if addr in self.p.import_calls:
                        if custom_abi(self.p.import_calls[addr][1]):
                            unsafe.add(start)
                            blocked.add(start)
                        else:
                            accessed.update(import_accessed)
                            modified.update(import_modified)
                    elif addr in self.p.undef_calls:
                        unsafe.add(start)
                        blocked.add(start)
                    elif link or not start <= tgt < self.cur_end:
                        # A nested guest entry can receive a new asynchronous
                        # preempt request after our caller's partial flush. We
                        # cannot materialize that ancestor's locals here.
                        unsafe.add(start)
                        blocked.add(start)
                    else:
                        pending.append(tgt)
                    # Conditional branches may fall through. Unconditional bc
                    # (BO=20) cannot; CTR/CR conditions otherwise can.
                    if link or (op == 16 and ((w >> 21) & 20) != 20):
                        pending.append(addr + 4)
                elif op == 19 and ((w >> 1) & 1023) in (16, 528):
                    xo, bo = (w >> 1) & 1023, (w >> 21) & 31
                    if link:
                        unsafe.add(start)  # dynamic callee: no static contract
                        blocked.add(start)
                        pending.append(addr + 4)
                    elif xo == 528:
                        jt = self.p.jump_tables.get(addr)
                        if jt:
                            base, count = jt
                            pending.extend(base + 4 * i for i in range(count))
                        # Even a known table has a default external dispatch.
                        unsafe.add(start)
                        blocked.add(start)
                    if (bo & 20) != 20:
                        pending.append(addr + 4)
                else:
                    if any(name in ("ppc_trap", "ppc_unimplemented")
                           for _, _, name, _ in cpu_calls(src)):
                        unsafe.add(start)
                        blocked.add(start)
                    pending.append(addr + 4)
            if start not in blocked:
                inputs = (leaf_inputs([body[a] for a in sorted(body)], modified,
                                      import_accessed, import_modified)
                          if self.leaf_read_inputs else frozenset(accessed))
                contracts[start] = (inputs, frozenset(modified))
        self.abi_safe = self.entries - unsafe
        self.abi_calls = frozenset("f_%08X_abi" % e for e in self.abi_safe)
        self.sync_safe = set(contracts) - self.abi_safe
        self.call_summaries = {"f_%08X_%s" % (e, "abi" if e in self.abi_safe else "sync"): contract
                               for e, contract in contracts.items()}

    def _imm_overrides(self):
        """Resolve the immediates of instructions referencing imported symbols."""
        self.imm_override = {}
        for sec, addr, typ, sym, add in self.p.rpx.relocs:
            if not sym.import_lib or sec.name != ".text":
                continue
            s = (self.data_import_addr.get(sym.value, sym.value) + add) & 0xFFFFFFFF
            v = {R_PPC_ADDR16_HA: ((s + 0x8000) >> 16) & 0xFFFF,
                 R_PPC_ADDR16_LO: s & 0xFFFF,
                 R_PPC_ADDR16_HI: s >> 16}.get(typ)
            if v is not None:
                self.imm_override[addr & ~3] = v

    def _bounds(self):
        self.sorted_entries = sorted(self.entries)

    def func_of(self, a):
        i = bisect.bisect_right(self.sorted_entries, a) - 1
        return self.sorted_entries[i] if i >= 0 else None

    def func_end(self, start):
        i = bisect.bisect_right(self.sorted_entries, start)
        return self.sorted_entries[i] if i < len(self.sorted_entries) else self.p.text_hi

    def _fixpoint(self):
        """Branch targets that land inside another function become entries."""
        rounds = 0
        while True:
            self._bounds()
            new = set()
            for i, w in enumerate(self.p.words):
                a = self.p.text_lo + 4 * i
                t = branch_target(a, w)
                if t is None or not self.p.in_text(t) or t in self.entries:
                    continue
                if self.func_of(t) != self.func_of(a):
                    new.add(t)
            rounds += 1
            if not new:
                break
            self.entries |= new
        self.fixpoint_rounds = rounds
        self._bounds()

    # --- callbacks used by ppc2c.translate ---
    def branch(self, addr, tgt):
        if addr in self.p.import_calls:  # tail call into an imported function
            lib, name, slot = self.p.import_calls[addr]
            self.used_imports.add(slot)
            return "MUSTTAIL return %s(c);" % self.imp_name(slot)
        if self.cur_start <= tgt < self.cur_end:
            self.labels.add(tgt)
            return "goto L_%08X;" % tgt
        if tgt in self.entries:
            return "MUSTTAIL return f_%08X(c);" % tgt
        return "c->pc = 0x%08Xu; MUSTTAIL return ppc_dispatch(c);" % tgt

    def call(self, addr, tgt):
        if addr in self.p.import_calls:
            lib, name, slot = self.p.import_calls[addr]
            self.used_imports.add(slot)
            return "%s(c);" % self.imp_name(slot)
        if addr in self.p.undef_calls:
            return "ppc_unimplemented(c, 0x%08Xu, 0); /* call to undefined symbol */" % addr
        if tgt in self.entries:
            suffix = ""
            if self.abi and self.register_locals:
                if tgt in self.abi_safe:
                    suffix = "_abi"
                elif tgt in self.sync_safe:
                    suffix = "_sync"
            return "f_%08X%s(c);" % (tgt, suffix)
        return "c->pc = 0x%08Xu; ppc_dispatch(c);" % tgt

    def ret(self):
        return "return;"

    def indirect_call(self, addr):
        if not (self.register_locals and self.indirect_cache):
            return "ppc_dispatch(c);"
        if getattr(self, "icache_in_cpu", True):  # synthetic test instances skip __init__
            sites = self.__dict__.setdefault("icache_sites", {})
            site = sites.setdefault(addr, len(sites))
            return "ppc_dispatch_cached(c, &c->icache[%d]); /* site %08X */" % (site, addr)
        return "static __thread PpcCallCache ic_%08X; ppc_dispatch_cached(c, &ic_%08X);" % (addr, addr)

    def indirect_jump(self, addr):
        jt = self.p.jump_tables.get(addr)
        if jt:
            base, count = jt
            cases = []
            for i in range(count):
                slot = base + 4 * i
                if self.cur_start <= slot < self.cur_end:
                    self.labels.add(slot)
                    cases.append("case 0x%08Xu: goto L_%08X;" % (slot, slot))
            return "switch (c->ctr) { %s } c->pc = c->ctr; MUSTTAIL return ppc_dispatch(c);" % " ".join(cases)
        return "c->pc = c->ctr; MUSTTAIL return ppc_dispatch(c);"

    def imp_name(self, slot):
        lib, name, kind = self.imports[slot]
        return "imp_%s_%s" % (c_ident(lib.replace(".rpl", "")), c_ident(name))

    # --- emission ---
    def emit_function(self, start):
        self.cur_start, self.cur_end = start, self.func_end(start)
        self.labels = set()
        body = []
        for a in range(start, self.cur_end, 4):
            w = self.p.word(a)
            try:
                s = translate(a, w, self)
            except Unhandled as e:
                self.unhandled[str(e)] += 1
                s = "ppc_unimplemented(c, 0x%08Xu, 0x%08Xu);" % (a, w)
            body.append((a, w, s))
        if self.single_precision:
            body = single_rounds(body, self.sites)
        if self.flag_liveness:
            body = dead_flags(body, self.sites)
        # Include site hooks and the implicit exit in the same synchronization
        # pass as instructions. Labels remain before hooks, even on backedges.
        statements = []
        for a, w, s in body:
            if a in self.sites:
                statements.append("site_%08X(c);" % a)
            statements.append(s)
        if self.cur_end < self.p.text_hi:
            # code falling into a hooked function continues with its original code
            nxt = "f_%08X_orig" % self.cur_end if self.cur_end in self.hooks else "f_%08X" % self.cur_end
            statements.append("MUSTTAIL return %s(c);" % nxt)
        else:
            statements.append("ppc_unimplemented(c, 0x%08Xu, 0); /* fell off end of text */" % self.cur_end)
        # Explicit packed stores help repeated Cpu observations, but inhibit
        # Clang's cheaper byte/vector stores in small compare-only leaves.
        pack_cr_stores = any(a in self.sites or any(RegisterLocals.observer.fullmatch(name)
                             for _, _, name, _ in cpu_calls(s)) for a, _, s in body)
        cache = RegisterLocals(statements, abi=self.abi, abi_calls=self.abi_calls,
                               pack_cr_stores=pack_cr_stores,
                               call_summaries=self.call_summaries) if self.register_locals else None
        if cache and self.sync_dataflow:
            # Labels enter before instruction hooks. Each instruction is a CFG
            # node; site observers and the implicit exit get their own nodes.
            by_addr, nodes = {}, []
            for a, w, s in body:
                by_addr[a] = len(nodes)
                if a in self.sites:
                    nodes.append((None, ""))
                nodes.append((w, s))
            nodes.append((None, ""))
            successors = []
            for i, (w, s) in enumerate(nodes):
                edges = {by_addr[int(m[1], 16)] for m in re.finditer(r"goto L_([0-9A-F]{8});", s)
                         if int(m[1], 16) in by_addr}
                fall = True
                if w is not None:
                    op, xo, bo = w >> 26, (w >> 1) & 1023, (w >> 21) & 31
                    if op == 18 or (op == 16 and (bo & 20) == 20) or (op == 19 and xo in (16, 528) and (bo & 20) == 20):
                        fall = bool(w & 1)
                if fall and i + 1 < len(nodes):
                    edges.add(i + 1)
                successors.append(edges)
            cache.analyze_sync(successors)
        emitted = iter([cache.emit(s, i) for i, s in enumerate(cache.statements)] if cache else statements)
        # restrict: guest memory never aliases the register file, so the compiler may keep
        # registers in host registers across guest loads/stores
        hooked = start in self.hooks
        fname = "f_%08X_orig" % start if hooked else "f_%08X" % start
        out = []
        if hooked:
            # runtime hook: callers reach hook_X, which may call the original code (f_X_orig)
            out.append("void f_%08X(Cpu* __restrict c) { hook_%08X(c); }\n" % (start, start))
        fast_entry = self.abi and self.register_locals and start in self.abi_safe | self.sync_safe
        if fast_entry:
            suffix = "_abi" if start in self.abi_safe else "_sync"
            out.append("void %s(Cpu* __restrict c) { PPC_ENTER(0x%08Xu); MUSTTAIL return %s%s(c); }\n" %
                       (fname, start, fname, suffix))
            fname += suffix
        out += ["void %s(Cpu* __restrict c) {" % fname]
        if not fast_entry:
            out.append("    PPC_ENTER(0x%08Xu);" % start)
        if cache:
            out.extend("    " + s for s in cache.declarations())
        for a, w, s in body:
            if a in self.labels:
                out.append("L_%08X: ;" % a)
            if a in self.sites:
                out.append("    " + next(emitted))
            out.append("    %s /* %08X: %08X */" % (next(emitted), a, w))
        # fall through into the next function
        out.append("    " + next(emitted))
        out.append("}")
        return "\n".join(out), len(body)

    def run(self, outdir, per_file):
        os.makedirs(outdir, exist_ok=True)
        self.unhandled = collections.Counter()
        self.used_imports = set()
        self.cached_calls = 0
        self.imm_override = self.imm_override
        files, cur, n = [], [], 0
        for start in self.sorted_entries:
            src, count = self.emit_function(start)
            self.cached_calls += (src.count("static __thread PpcCallCache") +
                                  src.count("ppc_dispatch_cached(c, &c->icache["))
            cur.append(src)
            n += count
            if n >= per_file:
                files.append(cur)
                cur, n = [], 0
        if cur:
            files.append(cur)
        for i, funcs in enumerate(files):
            with open(os.path.join(outdir, "code_%03d.c" % i), "w") as f:
                f.write('#include "funcs.h"\n\n')
                f.write("\n\n".join(funcs))
                f.write("\n")
        self.write_headers(outdir)
        self.write_report(outdir, len(files))

    def write_headers(self, outdir):
        func_slots = sorted(s for s, (lib, name, kind) in self.imports.items() if kind == "f")
        with open(os.path.join(outdir, "funcs.h"), "w") as f:
            f.write('#pragma once\n#include "ppc.h"\n\n')
            for e in self.sorted_entries:
                f.write("void f_%08X(Cpu* __restrict c);\n" % e)
            for e in sorted(self.abi_safe):
                f.write("void f_%08X_abi(Cpu* __restrict c);\n" % e)
            for e in sorted(self.sync_safe):
                f.write("void f_%08X_sync(Cpu* __restrict c);\n" % e)
            f.write("\n/* hooked functions: hook_X is implemented in the runtime, f_X_orig is the game's code */\n")
            for e in sorted(self.hooks):
                f.write("void f_%08X_orig(Cpu* __restrict c);\nvoid hook_%08X(Cpu* c);\n" % (e, e))
            f.write("\n/* instruction-level hooks (\"@ADDR\" in hooks.txt), run before the instruction at ADDR */\n")
            for e in sorted(self.sites):
                f.write("void site_%08X(Cpu* c);\n" % e)
            f.write("\n/* imported functions */\n")
            for s in func_slots:
                f.write("void %s(Cpu* c);\n" % self.imp_name(s))
        with open(os.path.join(outdir, "table.c"), "w") as f:
            f.write('#include "funcs.h"\n#include "recomp_table.h"\n\n')
            f.write("const RecompEntry g_recomp_funcs[] = {\n")
            for e in self.sorted_entries:
                f.write("    {0x%08Xu, f_%08X},\n" % (e, e))
            f.write("};\nconst unsigned g_recomp_func_count = %d;\n\n" % len(self.sorted_entries))
            f.write("const RecompImport g_recomp_imports[] = {\n")
            for s, (lib, name, kind) in sorted(self.imports.items()):
                fn = self.imp_name(s) if kind == "f" else "0"
                addr = self.data_import_addr.get(s, s)
                f.write('    {0x%08Xu, 0x%08Xu, "%s", "%s", %d, %s},\n' % (s, addr, lib, name, kind == "f", fn))
            f.write("};\nconst unsigned g_recomp_import_count = %d;\n" % len(self.imports))
            f.write("const uint32_t g_recomp_entry_point = 0x%08Xu;\n" % self.p.entry)
            f.write("const uint32_t g_ppc_icache_sites = %du;\n" % len(self.__dict__.get("icache_sites", {})))
        with open(os.path.join(outdir, "imports.c"), "w") as f:
            f.write('#include "funcs.h"\n\nvoid hle_unimplemented(Cpu* c, const char* lib, const char* name);\n\n')
            for s in func_slots:
                lib, name, _ = self.imports[s]
                f.write('__attribute__((weak)) void %s(Cpu* c) { hle_unimplemented(c, "%s", "%s"); }\n' % (
                    self.imp_name(s), lib, name))
        with open(os.path.join(outdir, "imports.json"), "w") as f:
            json.dump([{"slot": s, "lib": l, "name": n, "kind": k} for s, (l, n, k) in sorted(self.imports.items())], f, indent=1)

    def write_report(self, outdir, nfiles):
        with open(os.path.join(outdir, "report.txt"), "w") as f:
            f.write("functions: %d\nfiles: %d\nfixpoint rounds: %d\n" % (len(self.sorted_entries), nfiles, self.fixpoint_rounds))
            f.write("register locals: %s\n" % ("on" if self.register_locals else "off"))
            f.write("single-precision round25 elimination: %s\n" % ("on" if self.single_precision else "off"))
            f.write("Dead flag elimination: %s\n" % ("on" if self.flag_liveness else "off"))
            f.write("Leaf input liveness: %s\n" % ("on" if self.leaf_read_inputs and self.abi and self.register_locals else "off"))
            f.write("ABI synchronization: %s\nABI-safe guest callees: %d\n" %
                    ("on" if self.abi and self.register_locals else "off", len(self.abi_safe)))
            f.write("Additional leaf sync callees: %d\n" % len(self.sync_safe))
            f.write("Sync dataflow: %s\nCached indirect call sites: %d\n" %
                    ("on" if self.register_locals and self.sync_dataflow else "off", self.cached_calls))
            f.write("imports used: %d of %d\n" % (len(self.used_imports), len(self.imports)))
            f.write("unhandled instruction kinds:\n")
            for k, v in self.unhandled.most_common():
                f.write("  %6d  %s\n" % (v, k))
        with open(os.path.join(outdir, "report.txt")) as f:
            print(f.read())


if __name__ == "__main__":
    per = 30000
    if "--insns-per-file" in sys.argv:
        per = int(sys.argv[sys.argv.index("--insns-per-file") + 1])
    Recompiler(sys.argv[1], False if "--no-locals" in sys.argv else None,
               False if "--no-abi" in sys.argv else None,
               False if "--no-single-rounds" in sys.argv else None,
               False if "--no-icache" in sys.argv else None,
               False if "--no-sync-dataflow" in sys.argv else None,
               False if "--no-dead-flags" in sys.argv else None,
               False if "--no-leaf-inputs" in sys.argv else None).run(sys.argv[2], per)
