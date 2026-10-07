#!/usr/bin/env python3
"""Synthetic register-cache checks; no RPX or game data needed.

Run: python tools/recomp/test_locals.py
Set WWHD_RECOMP_TEST_CC to clang/gcc (or put clang in PATH) to also compile
and run the differential C check, at -O0/-O2/-O3, with real musttail calls.
"""
import collections
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from ppc2c import RegisterLocals, translate, Unhandled, single_rounds, dead_flags
from recomp import Recompiler


def dform(op, d, a, imm):
    return op << 26 | d << 21 | a << 16 | (imm & 0xFFFF)


def xform(op, d, a, b, xo, rc=0):
    return op << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc


def aform(op, d, a, b, cc, xo):
    return op << 26 | d << 21 | a << 16 | b << 11 | cc << 6 | xo << 1


def spr(d, number, write=False):
    return xform(31, d, number & 31, number >> 5, 467 if write else 339)


def synthetic(words, enabled=True, hooks=(), sites=(), end=None, jumps=None, imports=None, undef=(), abi=False):
    """Use the actual function emitter with a tiny in-memory Program."""
    r = Recompiler.__new__(Recompiler)
    r.register_locals = enabled
    r.abi = abi
    r.single_precision = enabled
    r.flag_liveness = enabled
    r.leaf_read_inputs = enabled
    r.indirect_cache = True
    r.sync_dataflow = True
    r.abi_safe = set()
    r.abi_calls = frozenset()
    r.sync_safe = set()
    r.call_summaries = {}
    r.p = SimpleNamespace(word=words.__getitem__, text_hi=max(words) + 4,
                          jump_tables=jumps or {}, import_calls=imports or {}, undef_calls=set(undef))
    r.entries = {0x1000, 0x2000}
    function_end = end or r.p.text_hi
    r.func_end = lambda start: function_end
    r.hooks, r.sites = set(hooks), set(sites)
    r.imm_override = {}
    r.used_imports = set()
    r.imports = {0x3000: ("test.rpl", "observe", "f")}
    r.unhandled = collections.Counter()
    return r


def analyzed(words, entries, enabled=True, abi=True, symbols=(), **options):
    r = synthetic(words, enabled=enabled, abi=abi, **options)
    r.entries = set(entries)
    r.p.rpx = SimpleNamespace(symbols=[SimpleNamespace(name=name, value=addr)
                                      for addr, name in symbols])
    r._bounds()
    r.func_end = lambda start: next((e for e in r.sorted_entries if e > start), r.p.text_hi)
    r._analyze_abi()
    return r


class LocalChecks(unittest.TestCase):
    def test_verification_adapters(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "verify"))
        try:
            from funcdb import GenIndex, CALL_RE
            from mktap import tap_body
            from mkunit import instrument
        finally:
            sys.path.pop(0)
        src = ("void f_00001000_sync(Cpu* __restrict c) {\n"
               "    f_00002000_sync(c);\n"
               "    static __thread PpcCallCache ic_00001004; ppc_dispatch_cached(c, &ic_00001004);\n"
               "}\n")
        with tempfile.TemporaryDirectory(prefix="recomp-adapters-") as tmp:
            (Path(tmp) / "code_000.c").write_text(src)
            gen = GenIndex(tmp, cache=str(Path(tmp) / "index.pkl"))
            self.assertEqual(gen.text(0x1000), src)
        self.assertEqual([m[1] for m in CALL_RE.finditer(src)], ["00002000"])
        self.assertIn("tap_call(c, f_00002000_sync, 0x00002000U);", tap_body(src, 0x1000, {}))
        self.assertIn("tap_dispatch(c);", tap_body(src, 0x1000, {}))
        self.assertIn("f_00002000(c);", instrument(src, "unit", False)[0])

    def test_dirty_sync_and_reload_liveness(self):
        statements = ["c->r[14]++;", "ppc_dispatch(c);", "ppc_dispatch(c);", "return;"]
        cache = RegisterLocals(statements, abi=False)
        cache.analyze_sync([{1}, {2}, {3}, set()])
        first = cache.emit(cache.statements[1], 1)
        second = cache.emit(cache.statements[2], 2)
        self.assertIn("c->r[14] = r14;", first)
        self.assertNotIn("r14 = c->r[14];", first)
        self.assertNotIn("c->r[14] = r14;", second)
        self.assertNotIn("c->r[14] = r14;", cache.emit(cache.statements[3], 3))
        # A conditional overwrite must retain a post-call value on the other
        # path, since a later may-dirty flush can still use that local.
        statements = ["ppc_dispatch(c);", "if (c->cr[2]) goto L_yes;",
                      "c->r[14] = 42;", "return;"]
        cache = RegisterLocals(statements, abi=False)
        cache.analyze_sync([{1}, {2, 3}, {3}, set()])
        self.assertIn("r14 = c->r[14];", cache.emit(cache.statements[0], 0))
        # A backedge joins a dirty value into the call's input even if its
        # first iteration starts coherent.
        cache = RegisterLocals(["ppc_dispatch(c);", "c->r[14]++;", "return;"], abi=False)
        cache.analyze_sync([{1}, {0, 2}, set()])
        self.assertIn("c->r[14] = r14;", cache.emit(cache.statements[0], 0))
        self.assertIn("r14 = c->r[14];", cache.emit(cache.statements[0], 0))

    def test_cached_indirect_call_emission(self):
        for branch in (0x4E800421, 0x4E800021, 0x4D820421):  # bctrl, blrl, conditional
            r = synthetic({0x1000: dform(14, 14, 14, 1), 0x1004: branch,
                           0x1008: dform(14, 3, 14, 1), 0x100C: 0x4E800020})
            src, _ = r.emit_function(0x1000)
            call = next(line for line in src.splitlines() if "ppc_dispatch_cached" in line)
            self.assertIn("ppc_dispatch_cached(c, &c->icache[0]);", call)
            self.assertIn("/* site 00001004 */", call)
            self.assertIn("c->r[14] = r14;", call)
            self.assertIn("r14 = c->r[14];", call)
            self.assertIn("uint32_t t = " + ("lr" if branch == 0x4E800021 else "ctr"), call)
            r.register_locals = False
            original, _ = r.emit_function(0x1000)
            self.assertIn("ppc_dispatch(c);", original)
            self.assertNotIn("ppc_dispatch_cached", original)
            r.register_locals, r.indirect_cache = True, False
            original, _ = r.emit_function(0x1000)
            self.assertIn("ppc_dispatch(c);", original)
            self.assertNotIn("ppc_dispatch_cached", original)

    def test_leaf_sync_contracts_and_custom_outputs(self):
        words = {0x1000: dform(14, 14, 14, 1), 0x1004: dform(14, 31, 31, 1),
                 0x1008: 0x48000019, 0x100C: dform(14, 3, 31, 0), 0x1010: 0x4E800020,
                 0x1020: dform(32, 31, 11, -4), 0x1024: 0x4E800020}
        words.update({a: 0x60000000 for a in range(0x1014, 0x1020, 4)})
        r = analyzed(words, (0x1000, 0x1020))
        self.assertNotIn(0x1020, r.abi_safe)
        self.assertIn(0x1020, r.sync_safe)
        accessed, modified = r.call_summaries["f_00001020_sync"]
        self.assertEqual(accessed, {"c->r[11]"})
        self.assertEqual(modified, {"c->r[31]"})
        src, _ = r.emit_function(0x1000)
        call = next(line for line in src.splitlines() if "f_00001020_sync(c);" in line)
        fast = call.split("ppc_preempt(c);")[1].split("}", 1)[1]
        self.assertNotIn("c->r[31] = r31;", fast)
        self.assertIn("r31 = c->r[31];", fast)
        self.assertNotIn("c->r[14] = r14;", fast)
        self.assertNotIn("r14 = c->r[14];", fast)
        self.assertIn("c->r[14] = r14;", call.split("ppc_preempt(c);")[0])
        leaf, _ = r.emit_function(0x1020)
        self.assertEqual(leaf.count("PPC_ENTER"), 1)
        # Fixed registers are actual outputs too; no preservation assumption.
        for field in (1, 2, 13):
            r = analyzed({0x1000: dform(14, field, field, 1), 0x1004: 0x4E800020}, (0x1000,))
            self.assertIn("c->r[%d]" % field, r.call_summaries["f_00001000_abi"][1])

    def test_leaf_inputs_cfg(self):
        sequences = (
            ([dform(14, 31, 0, 42)], set()),
            ([dform(14, 31, 31, 1)], {"c->r[31]"}),
            ([dform(16, 12, 2, 8), dform(14, 31, 0, 42)], {"c->r[31]", "c->cr[2]"}),
            ([dform(14, 31, 0, 42), dform(16, 12, 2, -4)], {"c->cr[2]"}),
            ([dform(14, 7, 0, 2), spr(7, 9, True), dform(14, 31, 31, 1),
              dform(16, 16, 0, -4)], {"c->r[31]"}),
            ([xform(31, 3, 0, 4, 150, 1)], {"c->r[3]", "c->r[4]"}),
        )
        for sequence, expected in sequences:
            words = {0x1000 + 4 * i: w for i, w in enumerate(sequence + [0x4E800020])}
            r = analyzed(words, (0x1000,))
            self.assertEqual(next(iter(r.call_summaries.values()))[0], expected)
        # Imports consume inherited argument registers even when the leaf never
        # mentions them explicitly. A write before the call supplies that input.
        words = {0x1000: dform(14, 3, 0, 42), 0x1004: 0x48000001, 0x1008: 0x4E800020}
        r = analyzed(words, (0x1000,), imports={0x1004: ("test.rpl", "observe", 0x3000)})
        inputs = next(iter(r.call_summaries.values()))[0]
        self.assertNotIn("c->r[3]", inputs)
        self.assertIn("c->r[4]", inputs)
        r.leaf_read_inputs = False
        r._analyze_abi()
        self.assertIn("c->r[3]", next(iter(r.call_summaries.values()))[0])

    def test_dead_flags_cfg_and_observers(self):
        cmp = dform(11, 0, 3, 4)
        ca = dform(12, 3, 3, 1)
        fp = xform(63, 0, 3, 4, 0)
        def optimize(sequence, sites=()):
            r = synthetic({0x1000 + 4 * i: w for i, w in enumerate(sequence)})
            r.cur_start, r.cur_end, r.labels = 0x1000, r.p.text_hi, set()
            return dead_flags([(a, w, translate(a, w, r)) for a, w in r.p.word.__self__.items()], sites)
        # All exit paths still expose the final full Cpu flags.
        src = " ".join(s for _, _, s in optimize([cmp, ca, cmp, ca, fp, fp, 0x4E800020]))
        self.assertEqual(src.count("c->xer_ca ="), 1)
        self.assertEqual(src.count("c->cr[0] ="), 1)
        self.assertEqual(src.count("c->fpscr ="), 1)
        # A consumer in the same fcmp instruction still needs its newly set CR.
        self.assertIn("c->cr[0] =", src)
        # Calls, hooks, mfcr/mfxer/mffs and an early return are observations.
        for middle in (0x4E800421, xform(31, 5, 0, 0, 19), spr(5, 1),
                       xform(63, 5, 0, 0, 583), xform(19, 12, 2, 0, 16)):
            src = " ".join(s for _, _, s in optimize([cmp, ca, fp, middle, cmp, ca, fp, 0x4E800020]))
            if middle == spr(5, 1) or middle in (0x4E800421, xform(19, 12, 2, 0, 16)):
                self.assertEqual(src.count("c->xer_ca ="), 2)
            if middle == xform(63, 5, 0, 0, 583) or middle in (0x4E800421, xform(19, 12, 2, 0, 16)):
                self.assertEqual(src.count("c->fpscr ="), 2)
        src = " ".join(s for _, _, s in optimize([ca, ca, 0x4E800020], sites=(0x1004,)))
        self.assertEqual(src.count("c->xer_ca ="), 2)
        # One bypass path observes the earlier value; a loop consumer also does.
        src = " ".join(s for _, _, s in optimize([ca, dform(16, 12, 2, 8), ca, 0x4E800020]))
        self.assertEqual(src.count("c->xer_ca ="), 2)
        src = " ".join(s for _, _, s in optimize([spr(5, 1), ca, dform(16, 12, 2, -8), ca, 0x4E800020]))
        self.assertEqual(src.count("c->xer_ca ="), 2)
        # fcmp's new CR values feed its masked FPSCR update inside the same
        # instruction; a self-read of XER also consumes the incoming value.
        body = [(0x1000, 0, "c->xer_so = 1;"),
                (0x1004, 0, "c->xer_so = c->xer_so ^ 1;"),
                (0x1008, 0x4E800020, "return;")]
        self.assertEqual(" ".join(s for _, _, s in dead_flags(body)).count("c->xer_so ="), 2)

    def test_leaf_summaries_fail_closed(self):
        for sequence, options in (
            ([dform(14, 14, 14, 1), 0x4E800020], {"hooks": (0x1000,)}),
            ([dform(14, 14, 14, 1), 0x4E800020], {"sites": (0x1000,)}),
            ([dform(14, 14, 14, 1), 0x4E800020], {"symbols": ((0x1000, "_restgpr_14"),)}),
            ([0x4E800421, 0x4E800020], {}),
            ([0x48001001, 0x4E800020], {}),
            ([0x4E800420, 0x4E800020], {}),
            ([0x44000002, 0x4E800020], {}),
            ([0x48000001, 0x4E800020], {"imports": {0x1000: ("coreinit", "OSSaveContext", 0x3000)}}),
        ):
            words = {0x1000 + 4 * i: w for i, w in enumerate(sequence)}
            r = analyzed(words, (0x1000,), **options)
            self.assertFalse(r.call_summaries, (sequence, options))
        r = analyzed({0x1000: 0x60000000, 0x1004: 0x4E800020}, (0x1000, 0x1004))
        self.assertNotIn("f_00001000_abi", r.call_summaries)

    def test_single_rounds_cfg_and_lanes(self):
        mul0 = aform(4, 6, 3, 0, 4, 12)
        mul1 = aform(4, 6, 3, 0, 4, 13)

        def count(sequence, sites=()):
            ws = {0x1000 + i * 4: w for i, w in enumerate(sequence + [0x4E800020])}
            r = synthetic(ws, sites=sites)
            src, _ = r.emit_function(0x1000)
            return src.count("round25(")

        self.assertEqual(count([mul0]), 2)  # unknown entry payload
        for load in (48, 49, 56, 57):
            self.assertEqual(count([dform(load, 4, 3, 0), mul0, mul1]), 0)
        self.assertEqual(count([dform(48, 4, 3, 0), dform(50, 4, 3, 8), mul0, mul1]), 2)
        self.assertEqual(count([dform(48, 4, 3, 0), 0x4E800421, mul0]), 2)  # bctrl
        self.assertEqual(count([dform(48, 4, 3, 0), mul0], sites=(0x1004,)), 2)
        # Join with an unknown lane on one path must retain rounding.
        self.assertEqual(count([dform(48, 4, 3, 0), dform(16, 12, 2, 8),
                                dform(50, 4, 3, 8), mul0]), 2)
        self.assertEqual(count([dform(48, 4, 3, 0), dform(16, 12, 2, 8),
                                dform(48, 4, 3, 8), mul0]), 0)
        # Loop entry is single, but a backedge with an lfd invalidates it.
        self.assertEqual(count([dform(48, 4, 3, 0), mul0, dform(50, 4, 3, 8),
                                dform(16, 12, 2, -8)]), 2)
        self.assertEqual(count([dform(48, 4, 3, 0), mul0, dform(48, 4, 3, 8),
                                dform(16, 12, 2, -8)]), 0)
        # Simultaneous paired writes, copying a lane and writing over the source.
        self.assertEqual(count([dform(48, 4, 3, 0), xform(4, 4, 4, 4, 624), mul0]), 0)
        self.assertEqual(count([dform(48, 4, 3, 0), aform(63, 4, 3, 0, 3, 25), mul0]), 2)
        self.assertEqual(count([dform(18, 0, 0, 8), dform(48, 4, 3, 0), mul0]), 2)
        ws = {0x1000: dform(48, 4, 3, 0), 0x1004: spr(3, 9, True),
              0x1008: 0x4E800420, 0x100C: mul0, 0x1010: 0x4E800020,
              0x1014: dform(50, 4, 3, 8), 0x1018: 0x4BFFFFF4}
        r = synthetic(ws, jumps={0x1008: (0x100C, 3)})
        src, _ = r.emit_function(0x1000)
        # Jump-table cases include a double overwrite on a backward edge;
        # default dispatch is also conservatively a CPU observation.
        self.assertEqual(src.count("round25("), 2)

    def test_packed_cr_observations(self):
        cache = RegisterLocals(["cr_set_u(c, 2, c->r[3], 4);", "ppc_dispatch(c);", "return;"])
        self.assertIn("ppc_cr_load_word(c, 8)", " ".join(cache.declarations()))
        emitted = cache.emit(cache.statements[1])
        self.assertIn("ppc_cr_store_word(c, 8,", emitted)
        self.assertIn("ppc_cr_load_word(c, 8)", emitted)
        for b in range(4):
            self.assertIn("((uint32_t)cr%d << %d)" % (8 + b, 8 * b), emitted)
        # Partial writes never overwrite the other bytes, including arbitrary
        # non-boolean bytes passed to a context/host observer.
        cache = RegisterLocals(["c->cr[9] = c->cr[8];", "return;"])
        emitted = cache.emit(cache.statements[1])
        self.assertIn("c->cr[9] = cr9;", emitted)
        self.assertNotIn("ppc_cr_store_word", emitted)
        r = synthetic({0x1000: dform(10, 0, 3, 4), 0x1004: 0x4E800020})
        leaf, _ = r.emit_function(0x1000)
        self.assertNotIn("ppc_cr_store_word", leaf)
        self.assertIn("c->cr[2] = cr2;", leaf)

    def test_abi_register_sets_and_reduced_traffic(self):
        statements = [" ".join("c->r[%d] = %d; c->cr[%d] = 1;" % (i, i, i) for i in range(32)),
                      "c->lr = 4; c->ctr = 8;", "imp_test_ordinary(c);", "return;"]
        cache = RegisterLocals(statements)
        call = cache.emit(cache.statements[2])
        before, after = call.split("imp_test_ordinary(c);")
        for field in cache.fields:
            store = "%s = %s;" % (field, cache.local(field))
            load = "%s = %s;" % (cache.local(field), field)
            if "->cr[" in field:
                first = int(field[field.index("[") + 1:-1]) // 4 * 4
                store = "ppc_cr_store_word(c, %d," % first
                load = "ppc_cr_load_word(c, %d)" % first
            self.assertEqual(store in before, not cache.nonvolatile(field), field)
            self.assertEqual(load in after, cache.volatile(field), field)
            self.assertIn(store, cache.emit(cache.statements[-1]))
        old = RegisterLocals(statements, abi=False)
        self.assertLess(len(call), len(old.emit(old.statements[2])))
        # Function-wide dirty superset: read-only r1/r14 need no write-back.
        cache = RegisterLocals(["c->r[3] = c->r[1] + c->r[14];", "imp_test_ordinary(c);"])
        call = cache.emit(cache.statements[1])
        self.assertNotIn("c->r[1] = r1;", call)
        self.assertNotIn("r1 = c->r[1];", call)
        self.assertNotIn("r14 = c->r[14];", call)

    def test_custom_conventions_and_unknown_observers(self):
        for name in ("imp_coreinit_OSSwitchFiber", "imp_coreinit_OSSwitchStack",
                     "imp_coreinit___OSSwitchStack", "imp_coreinit_setjmp", "imp_coreinit_longjmp",
                     "imp_coreinit_OSLoadContext", "imp_coreinit_OSSaveContext",
                     "imp_coreinit_OSYieldThread", "imp_lib__savegpr_14", "imp_lib__restgpr_14",
                     "site_00001000", "hook_00002000", "f_00002000", "ppc_dispatch",
                     "ppc_trap", "ppc_unimplemented"):
            cache = RegisterLocals(["c->r[14]++; c->cr[8] ^= 1;", name + "(c);"])
            call = cache.emit(cache.statements[1])
            before, after = call.split(name + "(c);")
            self.assertIn("c->r[14] = r14;", before, name)
            self.assertIn("r14 = c->r[14];", after, name)
            self.assertIn("cr8 = c->cr[8];", after, name)
        cache = RegisterLocals(["c->r[14]++;", "MUSTTAIL return imp_test_ordinary(c);"])
        self.assertIn("c->r[14] = r14; MUSTTAIL return", cache.emit(cache.statements[1]))

    def test_guest_abi_analysis_and_entry_boundary(self):
        words = {0x1000: dform(14, 14, 14, 1), 0x1004: 0x4800000D, 0x1008: 0x4E800020,
                 0x100C: 0x60000000,  # unreachable padding
                 0x1010: dform(14, 3, 3, 1), 0x1014: 0x4E800020,
                 0x1018: dform(14, 3, 14, 1), 0x101C: 0x4E800020,
                 0x1020: dform(14, 14, 0, 1), 0x1024: 0x4E800020,
                 0x1028: 0x4E800020,
                 0x102C: 0x4E800421, 0x1030: 0x4E800020,
                 0x1034: 0x48000009, 0x1038: 0x4E800020,
                 0x103C: dform(14, 3, 3, 1), 0x1040: 0x4E800020}
        entries = (0x1000, 0x1010, 0x1018, 0x1020, 0x1028, 0x102C, 0x1034, 0x103C)
        r = analyzed(words, entries, symbols=((0x1028, "_restgpr_14"),))
        self.assertEqual(r.abi_safe, {0x1010, 0x103C})
        caller, _ = r.emit_function(0x1000)
        call = next(line for line in caller.splitlines() if "f_00001010_abi(c);" in line)
        self.assertIn("ppc_trace_enter(0x00001010u)", call)
        self.assertIn("g_core_preempt[c->core]", call)
        preempt, ordinary = call.split("ppc_preempt(c);")
        self.assertIn("c->r[14] = r14;", preempt)
        self.assertIn("r14 = c->r[14];", ordinary)
        before_call = ordinary.split("f_00001010_abi(c);")[0].split("}")[-1]
        self.assertNotIn("c->r[14] = r14;", before_call)
        leaf, _ = r.emit_function(0x1010)
        self.assertEqual(leaf.count("PPC_ENTER"), 1)  # normal entry wrapper only
        self.assertIn("MUSTTAIL return f_00001010_abi(c);", leaf)
        self.assertIn("void f_00001010_abi", leaf)
        r.abi = False
        r._analyze_abi()
        caller, _ = r.emit_function(0x1000)
        self.assertNotIn("_abi(c)", caller)

    def test_analysis_reachable_paths_and_generation(self):
        cases = (
            ([dform(36, 14, 1, 0), 0x4E800020], False),  # prologue save
            ([dform(47, 14, 1, 0), 0x4E800020], False),  # stmw
            ([xform(31, 3, 0, 0, 19), 0x4E800020], False),  # mfcr includes CR2-4
            ([dform(11, 8, 3, 0), 0x4E800020], False),  # non-volatile CR write
            ([dform(16, 12, 2, 8), 0x4E800020, dform(14, 3, 14, 1), 0x4E800020], False),
            ([dform(14, 3, 3, 1), dform(16, 12, 2, -4), 0x4E800020], True),
            ([0x4E800020, dform(14, 3, 14, 1)], True),  # unreachable
            ([0x44000002, 0x4E800020], False),  # syscall/unimplemented
        )
        for sequence, safe in cases:
            words = {0x1000 + 4 * i: w for i, w in enumerate(sequence)}
            r = analyzed(words, (0x1000,))
            self.assertEqual(0x1000 in r.abi_safe, safe, sequence)
        r = analyzed({0x1000: dform(14, 3, 3, 1), 0x1004: 0x4E800020}, (0x1000,), sites=(0x1000,))
        self.assertFalse(r.abi_safe)
        # Fallthrough to an otherwise safe guest still needs a full entry.
        r = analyzed({0x1000: dform(14, 3, 3, 1), 0x1004: 0x4E800020}, (0x1000, 0x1004))
        self.assertEqual(r.abi_safe, {0x1004})
        for abi in (False, True):
            r = analyzed({0x1000: dform(14, 3, 3, 1), 0x1004: 0x4E800020}, (0x1000,), abi=abi)
            r.p.entry, r.fixpoint_rounds, r.data_import_addr = 0x1000, 1, {}
            with tempfile.TemporaryDirectory(prefix="recomp-abi-") as tmp:
                r.run(tmp, 2)
                header = (Path(tmp) / "funcs.h").read_text()
                report = (Path(tmp) / "report.txt").read_text()
                self.assertEqual("void f_00001000_abi" in header, abi)
                self.assertIn("ABI synchronization: " + ("on" if abi else "off"), report)

    def test_observers_and_write_only_reload(self):
        s = ["c->r[3] = 1; if (c->cr[2]) { c->r[14] = 2; }",
             "c->lr = 0x1004u; imp_test_observe(c);", "return;"]
        cache = RegisterLocals(s, abi=False)
        call = cache.emit(cache.statements[1])
        self.assertLess(call.index("c->r[14] = r14;"), call.index("imp_test_observe(c);"))
        self.assertGreater(call.index("r14 = c->r[14];"), call.index("imp_test_observe(c);"))
        self.assertIn("cr2 = c->cr[2];", call)
        self.assertNotIn("c->cr[2] = cr2;", call)  # read-only field
        self.assertIn("c->r[14] = r14;", cache.emit(cache.statements[2]))

    def test_tail_and_conditional_trap(self):
        cache = RegisterLocals(["c->r[3]++;", "if (c->cr[2]) ppc_trap(c, 4);",
                                "MUSTTAIL return ppc_dispatch(c);"])
        trap = cache.emit(cache.statements[1])
        self.assertIn("if (cr2) { c->r[3] = r3; ppc_trap", trap)
        tail = cache.emit(cache.statements[2])
        self.assertRegex(tail, r"c->r\[3\] = r3; MUSTTAIL return ppc_dispatch\(c\);\s*}")
        self.assertNotIn("r3 = c->r[3]", tail)

    def test_helpers_and_fail_closed(self):
        cache = RegisterLocals(["cr_set_u(c, 0, c->r[3], 42);",
                                "ppc_stwcx(c, c->r[4], c->r[3]);", "return;"])
        self.assertNotIn("cr_set_u", cache.emit(cache.statements[0]))
        self.assertIn("ppc_cr_load_word(c, 0)", cache.emit(cache.statements[1]))
        with self.assertRaisesRegex(ValueError, "unaudited CPU helper"):
            RegisterLocals(["new_helper(c);"]).emit("new_helper(c);")

    def test_entry_labels_hooks_and_fallthrough(self):
        words = {0x1000: dform(14, 3, 3, 1), 0x1004: 0x4E800420, 0x1008: 0x4E800020}
        r = synthetic(words, hooks=(0x1000, 0x100C), sites=(0x1000,),
                      end=0x100C, jumps={0x1004: (0x1000, 3)})
        r.p.text_hi = 0x1010
        src, _ = r.emit_function(0x1000)
        self.assertLess(src.index("PPC_ENTER"), src.index("uint32_t r3"))
        self.assertLess(src.index("L_00001000:"), src.index("site_00001000(c);"))
        self.assertIn("switch (ctr)", src)
        self.assertIn("case 0x00001000u: goto L_00001000;", src)
        self.assertIn("MUSTTAIL return f_0000100C_orig(c);", src)
        self.assertIn("void f_00001000(Cpu* __restrict c) { hook_00001000(c); }", src)
        r.register_locals = False
        baseline, _ = r.emit_function(0x1000)
        self.assertIn("c->r[3] = c->r[3]", baseline)
        self.assertNotIn("uint32_t r3", baseline)

    def test_environment_option(self):
        # Stop just after option selection, before reading an RPX.
        for value, expected in (("0", False), ("1", True)):
            r = Recompiler.__new__(Recompiler)
            with patch.dict(os.environ, {"WWHD_RECOMP_LOCALS": value}), \
                    patch("recomp.Program", side_effect=RuntimeError("stop")):
                with self.assertRaises(RuntimeError):
                    r.__init__("unused")
                self.assertEqual(r.register_locals, expected)
        for value, expected in (("0", False), ("1", True)):
            r = Recompiler.__new__(Recompiler)
            with patch.dict(os.environ, {"WWHD_RECOMP_ABI": value}), \
                    patch("recomp.Program", side_effect=RuntimeError("stop")):
                with self.assertRaises(RuntimeError):
                    r.__init__("unused")
                self.assertEqual(r.abi, expected)
        for value, expected in (("0", False), ("1", True)):
            r = Recompiler.__new__(Recompiler)
            with patch.dict(os.environ, {"WWHD_RECOMP_SINGLE_ROUNDS": value}), \
                    patch("recomp.Program", side_effect=RuntimeError("stop")):
                with self.assertRaises(RuntimeError):
                    r.__init__("unused")
                self.assertEqual(r.single_precision, expected)
                with self.assertRaises(RuntimeError):
                    r.__init__("unused", abi=False)
                self.assertFalse(r.abi)
        for env, attr in (("WWHD_RECOMP_ICACHE", "indirect_cache"),
                          ("WWHD_RECOMP_SYNC_DATAFLOW", "sync_dataflow"),
                          ("WWHD_RECOMP_DEAD_FLAGS", "flag_liveness"),
                          ("WWHD_RECOMP_LEAF_INPUTS", "leaf_read_inputs")):
            for value, expected in (("0", False), ("1", True)):
                r = Recompiler.__new__(Recompiler)
                with patch.dict(os.environ, {env: value}), \
                        patch("recomp.Program", side_effect=RuntimeError("stop")):
                    with self.assertRaises(RuntimeError):
                        r.__init__("unused")
                    self.assertEqual(getattr(r, attr), expected)

    def test_constructor_analysis_with_import(self):
        words = {0x1000: 0x48000001, 0x1004: 0x4E800020}
        sym = SimpleNamespace(import_lib="test.rpl", type=2, value=0x3000,
                              name="ordinary", import_kind="f")
        p = SimpleNamespace(entries=[0x1000], words=list(words.values()),
                            text_lo=0x1000, text_hi=0x1008, word=words.__getitem__,
                            import_calls={0x1000: ("test.rpl", "ordinary", 0x3000)},
                            undef_calls=set(), jump_tables={},
                            rpx=SimpleNamespace(symbols=[sym], relocs=[]))
        p.discover = lambda: p.entries
        p.in_text = lambda a: p.text_lo <= a < p.text_hi
        with patch("recomp.Program", return_value=p), patch.dict(os.environ, {
                "WWHD_RECOMP_LOCALS": "1", "WWHD_RECOMP_ABI": "1", "WWHD_HOOKS": "unused-nonexistent-hooks"}):
            r = Recompiler("unused")
        self.assertEqual(r.abi_safe, {0x1000})
        self.assertEqual(r.used_imports, {0x3000})

    def test_native_differential(self):
        cc = os.environ.get("WWHD_RECOMP_TEST_CC") or shutil.which("clang")
        if not cc:
            self.skipTest("set WWHD_RECOMP_TEST_CC to a C compiler for differential execution")
        ctx = synthetic({0x1000: 0})
        snippets = set()
        # Sweep all extended opcodes, including record/OE variants and helpers.
        # Control flow is exercised separately as complete functions below.
        for op in (4, 19, 31, 59, 63):
            for xo in range(1024):
                if op == 19 and xo in (16, 528):
                    continue
                if op == 4 and (xo & 31) in (6, 7):
                    continue  # quantized memory uses bounded addresses below
                for rc in (0, 1):
                    try:
                        snippets.add(translate(0x1000, xform(op, 3, 4, 5, xo, rc), ctx))
                    except Unhandled:
                        pass
        for op in (3, 7, 8, 10, 11, 12, 13, 14, 15, 20, 21, 23, 24, 25, 26, 27, 28, 29,
                   32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                   48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61):
            snippets.add(translate(0x1000, dform(op, 3, 4, 32), ctx))
        for number in (1, 8, 9, 268, 269, 912, 919):
            for write in (False, True):
                snippets.add(translate(0x1000, spr(3, number, write), ctx))
        snippets.add(translate(0x1000, xform(31, 0, 4, 5, 4), ctx))  # tw: never taken
        snippets.add("ppc_mtxer(c, c->r[3]); cr0_rc(c, c->r[4]); c->r[5] = ppc_mfxer(c);")
        snippets.add("cr_set_u(c, 0, c->r[3], 0); c->r[4] = ppc_mfcr(c); ppc_mtcrf(c, 0xAA, c->r[4]);")
        snippets.add("c->r[3] = ppc_lwarx(c, 64); ppc_stwcx(c, 64, c->r[4]); c->r[5] = ppc_mfcr(c);")
        snippets.add("ppc_mtcrf(c, 0x00, c->r[3]); memmove(&c->cr[4], &c->cr[4], 4);")
        snippets.add("if (c->cr[2]) c->r[14] = 42; imp_test_observe(c); if (c->cr[6]) c->r[14] = 7;")
        snippets.add("c->r[3]++; hook_test(c); c->r[4]++; f_test_orig(c); c->r[5]++; site_test(c);")
        functions, pairs = [], []
        for i, snippet in enumerate(sorted(snippets)):
            for mode in ("baseline", "cached"):
                statements = [snippet, "return;"]
                cache = RegisterLocals(statements, abi=False) if mode == "cached" else None
                body = (" ".join(cache.declarations()) + " " + " ".join(cache.emit(s) for s in cache.statements)
                        if cache else " ".join(statements))
                functions.append("void %s_%d(Cpu* __restrict c) { %s }" % (mode, i, body))
            pairs.append("{baseline_%d, cached_%d}" % (i, i))

        # Sweep repeated flag-producing instructions, independently of the
        # hand-written CFG cases. Avoid memory instructions here: repeating an
        # update-form transfer can leave the harness's bounded guest window.
        flag_snippets = [s for s in sorted(snippets) if
                         ("cr_" in s or "xer_" in s or "fpscr" in s) and
                         not any(x in s for x in ("ld", "st", "psq_", "goto", "return"))]
        for i, snippet in enumerate(flag_snippets):
            for mode in ("baseline", "cached"):
                body = [(0x1000, 0, snippet), (0x1004, 0, snippet), (0x1008, 0x4E800020, "return;")]
                if mode == "cached":
                    body = dead_flags(body)
                statements = [s for _, _, s in body]
                cache = RegisterLocals(statements, abi=False) if mode == "cached" else None
                src = (" ".join(cache.declarations()) + " " + " ".join(cache.emit(s) for s in cache.statements)
                       if cache else " ".join(statements))
                functions.append("void %s_repeat_flags%d(Cpu* __restrict c) { %s }" % (mode, i, src))
            pairs.append("{baseline_repeat_flags%d, cached_repeat_flags%d}" % (i, i))

        # Backedge, conditional call/return, site hook and fallthrough tail call.
        words = {0x1000: dform(14, 3, 0, 3), 0x1004: spr(3, 9, True),
                 0x1008: dform(14, 4, 4, 1), 0x100C: dform(16, 16, 0, -4),
                 0x1010: dform(16, 12, 2, 0xFF0) | 1,  # conditional import call
                 0x1014: xform(19, 12, 2, 0, 16),
                 0x1018: dform(18, 0, 0, 0xFE8) | 1}
        # Jump table: local case, default dispatch, and conditional LR dispatch.
        jump_words = {0x1000: spr(3, 9, True), 0x1004: 0x4E800420,
                      0x1008: dform(14, 4, 4, 1), 0x100C: xform(19, 12, 2, 0, 16, 1),
                      0x1010: 0x4E800020}
        for n, (ws, options) in enumerate(((words, {"sites": (0x1008,), "end": 0x101C,
                                                   "imports": {0x1010: ("test.rpl", "observe", 0x3000)}}),
                                           (jump_words, {"jumps": {0x1004: (0x1008, 2)}}),
                                           ({0x1000: 0x48001000}, {"imports": {0x1000: ("test.rpl", "observe", 0x3000)}}),
                                           ({0x1000: 0x48001001, 0x1004: 0x44000002}, {"undef": (0x1000,)}))):
            for mode in ("baseline", "cached"):
                r = synthetic(ws, enabled=mode == "cached", **options)
                r.p.text_hi += 4  # force a fallthrough into another guest function
                src, _ = r.emit_function(0x1000)
                functions.append(src.replace("f_00001000(", "%s_flow%d(" % (mode, n)))
            pairs.append("{baseline_flow%d, cached_flow%d}" % (n, n))

        # ABI calls preserve non-volatiles, but freely change every volatile,
        # including write-only locals. Full observers still see the whole Cpu.
        abi_snippets = [
            "c->r[14] += c->r[3]; c->cr[8] ^= c->cr[2]; c->r[1] += 16; " +
            " ".join("c->r[3]++; imp_test_ordinary(c); c->r[14] += c->r[3];" for _ in range(16)),
            "c->r[14]++; c->r[3] = 42; c->cr[24] = 1; "
            "if (c->cr[2]) imp_test_ordinary(c); if (c->cr[6]) c->r[3] = 7; imp_test_ordinary(c);",
            "c->r[14]++; c->cr[12] ^= 1; imp_test_ordinary(c); "
            "hook_test(c); c->r[14]++; imp_coreinit_OSSaveContext(c); c->r[14] += c->r[3];",
        ]
        for i, snippet in enumerate(abi_snippets):
            for mode in ("baseline", "cached", "abi"):
                statements = [snippet, "return;"]
                cache = RegisterLocals(statements, abi=mode == "abi") if mode != "baseline" else None
                body = (" ".join(cache.declarations()) + " " + " ".join(cache.emit(s) for s in cache.statements)
                        if cache else " ".join(statements))
                functions.append("void %s_calls%d(Cpu* __restrict c) { %s }" % (mode, i, body))
            pairs.extend(("{baseline_calls%d, cached_calls%d}" % (i, i),
                          "{baseline_calls%d, abi_calls%d}" % (i, i)))

        # Actual guest emission: dirty non-volatiles across repeated leaf calls,
        # asynchronous preempt requests, an ABI-breaking guest and a nested call.
        guest_words = {a: 0x60000000 for a in range(0x1000, 0x1128, 4)}
        sequence = [dform(14, 14, 14, 1), dform(11, 8, 3, 12), dform(14, 1, 1, 16)]
        for i in range(8):
            addr = 0x1000 + 4 * len(sequence)
            sequence.extend(((18 << 26) | ((0x1100 - addr) & 0x03FFFFFC) | 1,
                             dform(14, 14, 14, 1)))
        for target in (0x1110, 0x1120):
            addr = 0x1000 + 4 * len(sequence)
            sequence.append((18 << 26) | ((target - addr) & 0x03FFFFFC) | 1)
        sequence.append(0x4E800020)
        guest_words.update({0x1000 + 4 * i: w for i, w in enumerate(sequence)})
        guest_words.update({0x1100: dform(14, 3, 3, 1), 0x1104: 0x48000001, 0x1108: 0x4E800020,
                            0x1110: dform(14, 3, 14, 1), 0x1114: 0x4E800020,
                            0x1120: 0x4BFFFFE1, 0x1124: 0x4E800020})
        for mode in ("baseline", "cached", "abi"):
            r = analyzed(guest_words, (0x1000, 0x1100, 0x1110, 0x1120),
                         enabled=mode != "baseline", abi=mode == "abi",
                         imports={0x1104: ("test.rpl", "request_preempt", 0x3000)})
            r.imports[0x3000] = ("test.rpl", "request_preempt", "f")
            for e in r.sorted_entries:
                functions.append("void %s_guest_f_%08X(Cpu* __restrict c);" % (mode, e))
            for e in sorted(r.abi_safe):
                functions.append("void %s_guest_f_%08X_abi(Cpu* __restrict c);" % (mode, e))
            for e in sorted(r.sync_safe):
                functions.append("void %s_guest_f_%08X_sync(Cpu* __restrict c);" % (mode, e))
            for e in r.sorted_entries:
                src, _ = r.emit_function(e)
                functions.append(src.replace("f_", mode + "_guest_f_"))
        pairs.extend(("{baseline_guest_f_00001000, cached_guest_f_00001000}",
                      "{baseline_guest_f_00001000, abi_guest_f_00001000}"))

        # A true leaf may save incoming non-volatiles, return a new one, restore
        # an enclosing frame, or conditionally write a register/CR byte. Compare
        # every Cpu byte and guest store, with preemption on/off at both entries.
        leaves = (
            [dform(36, 14, 4, 0), dform(14, 3, 14, 1)],
            [dform(32, 31, 4, 0)],  # f_028F6A70-style enclosing restore
            [dform(14, 14, 0, 42)],  # non-volatile return
            [dform(14, 1, 1, 16)],  # custom SP output
            [dform(14, 2, 2, 1), dform(14, 13, 13, 2)],
            [dform(16, 12, 2, 8), dform(14, 14, 0, 17)],
            [dform(11, 8, 3, 12)],  # CR2 output
            [xform(31, 3, 0, 0, 19)],  # mfcr reads all bits
            [xform(31, 3, 0, 4, 150, 1)],  # stwcx writes implicit CR0
            [dform(14, 3, 3, -1), dform(11, 0, 3, 0), dform(16, 12, 1, -8)],
        )
        for n, leaf in enumerate(leaves):
            words = {0x1000: dform(14, 14, 14, 1), 0x1004: dform(14, 31, 31, 2),
                     0x1008: dform(11, 8, 3, 16), 0x100C: 0x48000035,
                     0x1010: xform(31, 3, 14, 31, 266), 0x1014: xform(31, 5, 1, 2, 266),
                     0x1018: xform(31, 6, 13, 3, 266), 0x101C: xform(31, 7, 0, 0, 19),
                     0x1020: dform(36, 14, 4, 0), 0x1024: 0x4E800020}
            words.update({a: 0x60000000 for a in range(0x1028, 0x1040, 4)})
            words.update({0x1040 + 4 * i: w for i, w in enumerate(leaf + [0x4E800020])})
            for mode in ("baseline", "summary"):
                r = analyzed(words, (0x1000, 0x1040), enabled=mode == "summary", abi=mode == "summary")
                prefix = "%s_leaf%d_" % (mode, n)
                for e in r.sorted_entries:
                    functions.append("void %sf_%08X(Cpu* __restrict c);" % (prefix, e))
                for name in r.call_summaries:
                    functions.append("void %s%s(Cpu* __restrict c);" % (prefix, name))
                for e in r.sorted_entries:
                    src, _ = r.emit_function(e)
                    functions.append(src.replace("f_", prefix + "f_"))
            pairs.append("{baseline_leaf%d_f_00001000, summary_leaf%d_f_00001000}" % (n, n))

        # Full-sync calls kill dirtiness only on the executed edge; joins,
        # write-only registers and backward edges still need current locals.
        sync_sequences = (
            [dform(14, 14, 14, 1), 0x4E800421, 0x4E800421],
            [dform(14, 14, 14, 1), 0x4D820421, dform(14, 3, 14, 0)],
            [0x4E800421, dform(16, 12, 2, 8), dform(14, 14, 0, 42)],
            [0x4E800421, dform(14, 3, 3, 1), dform(16, 12, 2, 8), dform(14, 14, 0, 42)],
            [dform(14, 7, 0, 3), 0x4E800421, dform(14, 14, 14, 1),
             dform(14, 7, 7, -1), dform(11, 0, 7, 0), dform(16, 12, 1, -16)],
        )
        for n, sequence in enumerate(sync_sequences):
            for mode in ("baseline", "cached"):
                ws = {0x1000 + 4 * i: w for i, w in enumerate(sequence + [0x4E800020])}
                r = synthetic(ws, enabled=mode == "cached")
                src, _ = r.emit_function(0x1000)
                functions.append(src.replace("f_00001000", "%s_sync_%d" % (mode, n)))
            pairs.append("{baseline_sync_%d, cached_sync_%d}" % (n, n))

        # Deterministic CFG sweep: forward joins, dead instructions, conditional
        # full observers and ordinary imports with different dirty/live sets.
        import random
        rng = random.Random(0xC0DE)
        for n in range(64):
            words, imports = {}, {}
            for i in range(24):
                addr = 0x1000 + 4 * i
                choice = rng.randrange(10)
                if choice < 3:
                    words[addr] = dform(14, rng.choice((3, 14, 31)), rng.choice((3, 14, 31)), 1)
                elif choice == 3:
                    words[addr] = dform(11, rng.choice((0, 8)), 3, 12)
                elif choice == 4:
                    words[addr] = 0x4E800421
                elif choice == 5:
                    words[addr] = rng.choice((0x4D820421, 0x4D820021))
                elif choice == 6:
                    words[addr] = (18 << 26) | ((0x3000 - addr) & 0x03FFFFFC) | 1
                    imports[addr] = ("test.rpl", "ordinary", 0x3000)
                elif choice < 9:
                    words[addr] = dform(16, rng.choice((4, 12, 20)), 2, 4 * rng.randint(1, 24 - i))
                else:
                    words[addr] = dform(36, rng.choice((14, 31)), 4, 0)
            words[0x1060] = 0x4E800020
            for mode in ("baseline", "cached"):
                r = synthetic(words, enabled=mode == "cached", abi=True, imports=imports)
                r.imports[0x3000] = ("test.rpl", "ordinary", "f")
                src, _ = r.emit_function(0x1000)
                functions.append(src.replace("f_00001000", "%s_cfg_%d" % (mode, n)))
            pairs.append("{baseline_cfg_%d, cached_cfg_%d}" % (n, n))

        # Complete FP functions compare unoptimized emission to CFG-proven
        # round25 removal, including joins, loops, hooks and indirect calls.
        load = dform(48, 4, 4, 16)
        mul = aform(4, 6, 3, 0, 4, 12)
        fp_sequences = (
            [load, mul, mul],
            [dform(56, 4, 4, 16), mul, mul],
            [load, dform(50, 4, 4, 24), mul],
            [load, dform(16, 12, 2, 8), dform(50, 4, 4, 24), mul],
            [load, dform(14, 7, 0, 3), spr(7, 9, True), mul,
             dform(48, 4, 4, 24), dform(16, 16, 0, -8)],
            [load, 0x4E800421, mul],
            [load, xform(4, 4, 4, 4, 624), mul],
        )
        for n, sequence in enumerate(fp_sequences):
            for mode in ("baseline", "cached"):
                ws = {0x1000 + 4 * i: w for i, w in enumerate(sequence + [0x4E800020])}
                r = synthetic(ws, enabled=mode == "cached")
                src, _ = r.emit_function(0x1000)
                functions.append(src.replace("f_00001000", "%s_single_%d" % (mode, n)))
            pairs.append("{baseline_single_%d, cached_single_%d}" % (n, n))

        # Flag liveness against full-state reference, including reads, rc forms,
        # joins, loops, cold preemption and observations between definitions.
        ca = dform(12, 5, 3, 1)
        cmp = dform(11, 0, 3, 4)
        fp = xform(63, 0, 3, 4, 0)
        sequences = (
            [cmp, ca, fp, cmp, ca, fp],
            [ca, spr(6, 1), ca],
            [fp, xform(63, 6, 0, 0, 583), fp],
            [cmp, xform(31, 6, 0, 0, 19), cmp],
            [ca, dform(16, 12, 2, 8), ca],
            [ca, xform(19, 12, 2, 0, 16), ca],
            [fp, 0x4E800421, fp],
            [ca, 0x4E800421, ca],
            [dform(14, 7, 0, 3), spr(7, 9, True), spr(6, 1), ca,
             dform(16, 16, 0, -8), ca],
            [cmp, xform(31, 3, 0, 4, 150, 1), cmp],
            [xform(31, 5, 3, 4, 266 | 512, 1), xform(31, 5, 3, 4, 266 | 512, 1)],
        )
        for n, sequence in enumerate(sequences):
            for mode in ("baseline", "cached"):
                ws = {0x1000 + 4 * i: w for i, w in enumerate(sequence + [0x4E800020])}
                r = synthetic(ws, enabled=mode == "cached", sites=(0x1008,) if n == 7 else ())
                src, _ = r.emit_function(0x1000)
                functions.append(src.replace("f_00001000", "%s_flags_%d" % (mode, n)))
            pairs.append("{baseline_flags_%d, cached_flags_%d}" % (n, n))

        # Unchanged quantizer reference bodies exercise the split helper on all
        # GQR types/scales, single/pair mode, and unaligned addresses.
        original = (Path(__file__).resolve().parents[2] / "runtime/include/ppc.h").read_text()
        for helper in ("load", "store"):
            begin = original.index("static PPC_PSQ_SLOW void psq_" + helper + "_quantized(")
            end = original.index("\n}", begin) + 2
            ref = original[begin:end].replace("static PPC_PSQ_SLOW", "static inline").replace(
                "psq_" + helper + "_quantized", "reference_psq_" + helper)
            functions.append(ref)
        for w in (0, 1):
            for mode, prefix in (("baseline", "reference_psq_"), ("cached", "psq_")):
                functions.append("void %s_psq_%d(Cpu* c) { %sload(c, 3, 65, %d, 0); "
                                 "%sstore(c, 3, 81, %d, 0); }" % (mode, w, prefix, w, prefix, w))
            pairs.append("{baseline_psq_%d, cached_psq_%d}" % (w, w))

        header = Path(__file__).resolve().parents[2] / "runtime" / "include"
        source = C_HARNESS + "\n" + "\n".join(functions) + "\nPair pairs[] = {" + ",\n".join(pairs) + "};\n" + C_MAIN
        with tempfile.TemporaryDirectory(prefix="recomp-locals-") as tmp:
            src = Path(tmp) / "check.c"
            exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            src.write_text(source)
            # Desktop header path also compiles; execution uses a small mock
            # Switch memory window instead of mapping the desktop 4 GiB window.
            result = subprocess.run([cc, "-std=gnu11", "-fsyntax-only", "-I", str(header), str(src)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for opt in ("-O0", "-O2", "-O3"):
                result = subprocess.run([cc, "-std=gnu11", opt, "-ffp-contract=off", "-fno-strict-aliasing",
                                         "-D__SWITCH__", "-I", str(header), str(src), "-o", str(exe), "-lm"],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                print(opt, result.stdout.strip())


C_HARNESS = r'''
#include "ppc.h"
#include <stdio.h>
static uint8_t memory[65536], original_memory[65536], baseline_memory[65536];
uint8_t* const g_ppc_mem_base = memory;
int g_ppc_trace;
volatile int g_core_preempt[3];
static uint32_t observed;
static uint32_t hash(const void* ptr, size_t n) {
    const uint8_t* p = ptr; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static void observe(Cpu* c) {
    observed = observed * 33u + hash(c, sizeof(*c));
    /* Touch even guest callee-saved and write-only registers; no ABI shortcuts. */
    for (int i = 0; i < 32; i++) { c->r[i] = (c->r[i] + i + 1) & 255; c->cr[i] = !c->cr[i]; }
    c->lr ^= 0x40; c->ctr = (c->ctr + 1) & 7;
    c->xer_so ^= 1; c->xer_ca ^= 1; c->f[3].ps0 += 1; c->fpscr ^= 0x1000;
}
void ppc_preempt(Cpu* c) { observe(c); }
void ppc_trace_enter(uint32_t a) { observed += a; }
void ppc_dispatch(Cpu* c) { observe(c); }
uint32_t g_ppc_dispatch_epoch = 1;
PpcFunc ppc_resolve(Cpu* c) { (void)c; return observe; }
PpcFunc ppc_resolve_cached(Cpu* c, PpcCallCache* cache, uint32_t epoch) {
    cache->target = c->pc; cache->epoch = epoch; cache->fn = ppc_resolve(c); return cache->fn;
}
void ppc_unimplemented(Cpu* c, uint32_t a, uint32_t w) { observed += a + w; observe(c); }
void ppc_trap(Cpu* c, uint32_t a) { observed += a; observe(c); }
void imp_test_observe(Cpu* c) { observe(c); }
void imp_coreinit_OSSaveContext(Cpu* c) { observe(c); }
void imp_test_ordinary(Cpu* c) {
    /* Ordinary EABI input/output: no observations of incoming non-volatiles. */
    observed += c->r[1] + c->r[2] + c->r[13];
    for (int i = 0; i < 32; i++) {
        if (i == 0 || (i >= 3 && i <= 12)) {
            observed = observed * 33u + c->r[i];
            c->r[i] = (c->r[i] + i + 1) & 255;
        }
        if (i < 8 || i >= 20) {
            observed = observed * 33u + c->cr[i]; c->cr[i] = !c->cr[i];
        }
    }
    observed += c->lr + c->ctr;
    c->lr ^= 0x40; c->ctr = (c->ctr + 1) & 7; c->xer_so ^= 1;
}
void imp_test_request_preempt(Cpu* c) { imp_test_ordinary(c); g_core_preempt[c->core] = 1; }
void hook_test(Cpu* c) { observe(c); }
void f_test_orig(Cpu* c) { observe(c); }
void site_test(Cpu* c) { observe(c); }
void f_00002000(Cpu* c) { observe(c); }
void f_0000101C(Cpu* c) { observe(c); }
void f_00001014(Cpu* c) { observe(c); }
void f_00001004(Cpu* c) { observe(c); }
void f_00001008(Cpu* c) { observe(c); }
void site_00001008(Cpu* c) { observe(c); c->ctr = 1; }
uint64_t ppc_timebase(void) { return 0x123456789ABCDEF0ull; }
double ppc_fres(double d) { return 1.0 / d; }
double ppc_frsqrte(double d) { return 1.0 / sqrt(d); }
typedef struct { PpcFunc baseline, cached; } Pair;
'''

C_MAIN = r'''
int main(void) {
    uint32_t rng = 1;
    /* Bit-exact identity on promoted floats, including zeros, subnormals,
     * infinities and NaN payloads. No float arithmetic is substituted. */
    for (unsigned i = 0; i < 100000; i++) {
        rng = rng * 1664525u + 1013904223u;
        uint32_t bits = i < 6 ? (uint32_t[]){0, 0x80000000u, 1, 0x7F800000u,
                                                          0x7FA12345u, 0xFFC12345u}[i] : rng;
        double v = (double)u32_as_f32(bits);
        if (f64_as_u64(v) != f64_as_u64(round25(v))) return 2;
    }
    for (int type = 0; type < 8; type++) for (int scale = 0; scale < 64; scale++) {
        for (int w = 0; w < 2; w++) for (int index = 0; index < 8; index++) {
            Cpu a = {0}, b;
            for (int i = 0; i < 32; i++) { a.f[i].ps0 = i + .25; a.f[i].ps1 = -i - .5; }
            a.gqr[index] = (type << 16) | type | (scale << 24) | (scale << 8);
            for (unsigned i = 0; i < sizeof(memory); i++) original_memory[i] = (uint8_t)(i * 31 + type);
            b = a; memcpy(memory, original_memory, sizeof(memory));
            reference_psq_load(&a, 3, 65, w, index); reference_psq_store(&a, 3, 81, w, index);
            memcpy(baseline_memory, memory, sizeof(memory));
            memcpy(memory, original_memory, sizeof(memory));
            psq_load(&b, 3, 65, w, index); psq_store(&b, 3, 81, w, index);
            if (memcmp(&a, &b, sizeof(a)) || memcmp(memory, baseline_memory, sizeof(memory))) return 3;
        }
    }
    for (unsigned test = 0; test < sizeof(pairs) / sizeof(pairs[0]); test++) {
        for (int run = 0; run < 64; run++) {
            Cpu a = {0}, b;
            for (int i = 0; i < 32; i++) {
                rng = rng * 1664525u + 1013904223u;
                a.r[i] = (rng >> 16) & 255;
                a.cr[i] = (rng >> 8) & (run & 1 ? 1 : 255);
                a.f[i].ps0 = (int)(rng & 255) - 128; a.f[i].ps1 = (int)((rng >> 8) & 255) - 128;
            }
            a.r[3] = run & 1 ? 0x1008 : a.r[3]; /* local jump-table case or dispatch */
            a.lr = rng; a.ctr = rng & 7; a.xer_so = (rng >> 9) & 1;
            a.xer_ca = (rng >> 10) & 1; a.xer_ov = (rng >> 11) & 1;
            a.fpscr = rng; a.res_addr = 64; a.res_val = 0;
            for (int i = 0; i < 8; i++) a.gqr[i] = ((run & 7) << 16) | (run & 7) |
                                                  ((run & 63) << 24) | ((run & 63) << 8);
            if (run == 0) { a.f[4].ps0 = NAN; a.f[4].ps1 = NAN; }
            if (run == 1) { a.f[5].ps0 = INFINITY; a.f[5].ps1 = -INFINITY; }
            for (unsigned i = 0; i < sizeof(memory); i++) original_memory[i] = (uint8_t)(rng + i);
            g_ppc_trace = run & 1; g_core_preempt[0] = run & 2;
            b = a; memcpy(memory, original_memory, sizeof(memory)); observed = 0;
            pairs[test].baseline(&a); uint32_t baseline_observed = observed;
            memcpy(baseline_memory, memory, sizeof(memory));
            memcpy(memory, original_memory, sizeof(memory)); observed = 0;
            g_core_preempt[0] = run & 2;
            pairs[test].cached(&b);
            if (memcmp(&a, &b, sizeof(a)) || baseline_observed != observed ||
                memcmp(memory, baseline_memory, sizeof(memory))) {
                fprintf(stderr, "mismatch: function %u, run %d\n", test, run); return 1;
            }
        }
    }
    printf("%zu synthetic functions x 64 states matched (Cpu, observations, memory)\n", sizeof(pairs) / sizeof(pairs[0]));
    return 0;
}
'''


if __name__ == "__main__":
    unittest.main()
