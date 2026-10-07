#!/usr/bin/env python3
"""Exercise the actual core.cpp dispatcher and ppc.h call-site cache in isolation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DispatchChecks(unittest.TestCase):
    def test_native_cache(self):
        cc = os.environ.get("WWHD_RECOMP_TEST_CC") or shutil.which("clang")
        if not cc:
            self.skipTest("set WWHD_RECOMP_TEST_CC to clang for native execution")
        root = Path(__file__).resolve().parents[2]
        core = (root / "runtime/src/core.cpp").read_text()
        body = core[core.index("uint32_t g_ppc_dispatch_epoch ="):core.index("uint32_t guest_call(")]
        # Instrument the real resolver, keeping the dispatcher implementation
        # otherwise unchanged. Hits must not perform a table lookup.
        body = body.replace("PpcFunc f = dispatch::lookup(c->pc);",
                            "++resolves; PpcFunc f = dispatch::lookup(c->pc);")
        source = HARNESS + body + MAIN
        with tempfile.TemporaryDirectory(prefix="recomp-dispatch-") as tmp:
            src = Path(tmp) / "dispatch.cpp"
            exe = Path(tmp) / ("dispatch.exe" if os.name == "nt" else "dispatch")
            src.write_text(source)
            for opt in ("-O0", "-O2", "-O3"):
                result = subprocess.run([cc, "-x", "c++", "-std=c++20", opt, "-pthread",
                                         "-I", str(root / "runtime/include"),
                                         "-I", str(root / "runtime/src"), str(src), "-lstdc++",
                                         "-o", str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(exe), "unknown"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 77, result.stderr)
                self.assertIn("unknown address DEADBEEF (lr=00001234 ctr=00005678)", result.stderr)


HARNESS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include "runtime.h"
#include "recomp_table.h"
static std::atomic<unsigned> resolves;
[[noreturn]] void fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    std::exit(77);
}
static void call_site(Cpu* c);
static void first(Cpu* c) { c->r[3] += 1; }
static void second(Cpu* c) { c->r[3] += 10; }
static void recursive(Cpu* c) {
    c->r[3] += 100;
    if (c->r[0]) { --c->r[0]; c->pc = 0x02000004; call_site(c); }
}
const RecompEntry g_recomp_funcs[] = {{0x02000000, first}, {0x02000004, second}, {0x02000008, recursive}};
const unsigned g_recomp_func_count = 3;
const RecompImport g_recomp_imports[] = {{0xC0000000, 0, "test", "first", 1, first}};
const unsigned g_recomp_import_count = 1;
const uint32_t g_ppc_icache_sites = 0;
'''

MAIN = r'''
static void call_site(Cpu* c) {
    static __thread PpcCallCache cache;
    ppc_dispatch_cached(c, &cache);
}
int main(int argc, char**) {
    dispatch::init();
    Cpu c{};
    if (argc > 1) { c.pc = 0xDEADBEEF; c.lr = 0x1234; c.ctr = 0x5678; call_site(&c); return 1; }
    c.pc = 0x02000000;
    for (int i = 0; i < 20; ++i) call_site(&c);
    assert(c.r[3] == 20 && resolves == 1);
    uint32_t host = dispatch::register_host(first, "first");
    call_site(&c);
    assert(c.r[3] == 21 && resolves == 1); // adding an address retains cached hits
    dispatch::set(c.pc, second);
    call_site(&c);
    assert(c.r[3] == 31 && resolves == 2);
    for (uint32_t addr : {0xC0000000u, host, 0x12340000u}) {
        if (addr == 0x12340000) dispatch::set(addr, first);
        c.pc = addr; c.r[3] = 0;
        call_site(&c); call_site(&c);
        assert(c.r[3] == 2);
        dispatch::set(addr, second);
        call_site(&c);
        assert(c.r[3] == 12);
    }
    // Nested calls change this site's cache while the outer function is active.
    c.pc = 0x02000008; c.r[0] = 1; c.r[3] = 0;
    call_site(&c);
    assert(c.r[3] == 110);
    c.pc = 0x02000008; call_site(&c);
    assert(c.r[3] == 210);
    auto run = [](uint32_t target, uint32_t delta) {
        Cpu local{}; local.pc = target;
        for (int i = 0; i < 10000; ++i) call_site(&local);
        assert(local.r[3] == delta * 10000);
    };
    std::thread a(run, 0x02000004, 10), b(run, 0x02000008, 100);
    a.join(); b.join();
    // Normal dispatch retains its tail path and sees replacements too.
    c.pc = host; c.r[3] = 0; ppc_dispatch(&c); assert(c.r[3] == 10);
    puts("cache hit/miss, replacement, all address domains, recursion and threads matched");
}
'''


if __name__ == "__main__":
    unittest.main()
