#!/usr/bin/env python3
"""Host regressions for bounded runtime queues and the production AX SIMD math.

NEON executes through a scalar intrinsic model on the host; also syntax-check
the real intrinsics with Clang's AArch64 headers. This is not a libnx/game test.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RuntimeChecks(unittest.TestCase):
    def setUp(self):
        self.cc = os.environ.get("WWHD_RUNTIME_TEST_CC") or shutil.which("clang++")
        if not self.cc:
            self.skipTest("set WWHD_RUNTIME_TEST_CC to clang++")

    def run_cpp(self, source, extra=None):
        with tempfile.TemporaryDirectory(prefix="runtime-check-") as tmp:
            tmp = Path(tmp)
            src = tmp / "check.cpp"
            exe = tmp / ("check.exe" if os.name == "nt" else "check")
            src.write_text(source)
            if extra:
                (tmp / "arm_neon.h").write_text(extra)
            result = subprocess.run([self.cc, "-std=c++20", "-O2", "-ffp-contract=off", "-pthread",
                                     "-I", str(tmp), "-I", str(ROOT / "runtime/src"),
                                     str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_queues(self):
        self.run_cpp(QUEUES)

    def test_audio_math(self):
        self.run_cpp(AUDIO, NEON_MODEL)

    def test_audio_output(self):
        source = (ROOT / "runtime/src/audio_out.cpp").read_text()
        pull = source[source.index("void pull("):source.index("#if defined(__APPLE__)", source.index("void pull("))]
        push = source[source.index("void push("):source.index("int buffered_frames(")]
        self.run_cpp(OUTPUT_PREFIX + pull + push + OUTPUT_MAIN)

    def test_dc_store_clipping(self):
        source = (ROOT / "runtime/src/platform/switch/switch_host.cpp").read_text()
        body = source[source.index('extern "C" void switch_dc_store('):source.index('// ---------------------------------------------------------------- threads')]
        self.run_cpp(DC_PREFIX + body + DC_MAIN)

    def test_gx2_emission(self):
        source = (ROOT / "runtime/src/gx2/gx2_core.cpp").read_text()
        recording = source[source.index("struct Recording {"):source.index("// ---------------------------------------------------------------- render thread")]
        emit = source[source.index("static void enqueue("):source.index("#ifdef WWHD_HAS_VULKAN\nvoid checkpoint_vulkan_caches()")]
        regs = source[source.index("void set_regs("):source.index("void set_reg(")]
        fence = source[source.index("    case OP_FENCE: {"):source.index("    default: break;", source.index("    case OP_FENCE: {"))]
        for switch in (True, False):
            with self.subTest(switch=switch):
                prefix = EMIT_PREFIX if switch else EMIT_PREFIX.replace("#define __SWITCH__ 1", "")
                self.run_cpp(prefix + recording + emit + regs + EMIT_EXECUTE + fence + EMIT_MAIN)

    def test_real_neon_headers(self):
        with tempfile.TemporaryDirectory(prefix="ax-a57-") as tmp:
            src = Path(tmp) / "ax.cpp"
            src.write_text('#include "hle/ax_simd.h"\n'
                           'void test(const float* a, float* b, const int32_t* c, int32_t* d, float& h) {\n'
                           'axsimd::envelope(b, 96, 1, 0.5f); axsimd::mix(a, b, 96, 1, 0.5f);\n'
                           'axsimd::upsample(c, d, 96, h, 8); }\n')
            result = subprocess.run([self.cc, "--target=aarch64-linux-gnu", "-std=c++20",
                                     "-ffreestanding", "-fsyntax-only", "-D__SWITCH__",
                                     "-I", str(ROOT / "runtime/src"), str(src)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


QUEUES = r'''
#include <array>
#include <cassert>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include "gx2/command_ring.h"
#include "message_ring.h"

int main() {
    // A sub-batch emitted while the consumer is busy must execute even if the
    // producer stops without a boundary command or another emission.
    gx2::CommandRing<1024> idle;
    idle.lock();
    idle.reserve(1)[0] = 0;
    idle.commit(1, true);
    idle.unlock();
    uint32_t idleCount;
    idle.wait(idleCount);
    idle.lock();
    idle.reserve(1)[0] = 0;
    idle.commit(1, false);
    idle.unlock();
    idle.complete(idleCount);
    idle.wait(idleCount); assert(idleCount == 1); idle.complete(idleCount);

    // Storage cannot be reclaimed before consumer execution finishes.
    gx2::CommandRing<1024> ownership;
    ownership.lock();
    uint32_t* whole = ownership.reserve(1024);
    whole[0] = 1 | (1023 << 8);
    whole[1] = 12345;
    ownership.commit(1024, true);
    ownership.unlock();
    uint32_t count;
    const uint32_t* borrowed = ownership.wait(count);
    assert(count == 1024 && borrowed[1] == 12345);
    std::atomic<bool> entered{false}, finished{false};
    std::thread blocked([&] {
        ownership.lock();
        entered = true;
        uint32_t* w = ownership.reserve(1);
        w[0] = 0;
        ownership.commit(1, true);
        finished = true;
        ownership.unlock();
    });
    while (!entered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!finished && borrowed[1] == 12345);
    ownership.complete(count);
    blocked.join();
    ownership.wait(count); assert(count == 1); ownership.complete(count);

    // Several producers, owned snapshots, batch boundaries, wrap padding,
    // full waits, and repeated idle/latched-wake transitions.
    gx2::CommandRing<4096> ring;
    constexpr unsigned producers = 3, iterations = 1000;
    unsigned next[producers] = {}, ticket = 0;
    std::thread consumer([&] {
        unsigned received = 0;
        while (received != producers * iterations) {
            uint32_t count;
            const uint32_t* data = ring.wait(count);
            for (unsigned i = 0; i < count;) {
                unsigned n = data[i] >> 8, op = data[i] & 255;
                assert(n < count - i);
                if (op) {
                    unsigned p = data[i + 1], seq = data[i + 2];
                    assert(op == 1 && p < producers && seq == next[p]++);
                    assert(data[i + 3] == received);
                    for (unsigned k = 3; k < n; ++k) assert(data[i + 1 + k] == (seq ^ (p << 24) ^ k));
                    ++received;
                }
                i += n + 1;
            }
            if (received % 128 == 0) std::this_thread::sleep_for(std::chrono::microseconds(10));
            ring.complete(count);
        }
    });
    std::thread writers[producers];
    for (unsigned p = 0; p < producers; ++p) writers[p] = std::thread([&, p] {
        for (unsigned seq = 0; seq < iterations; ++seq) {
            unsigned n = 3 + seq % 241;
            std::array<uint32_t, 244> payload;
            payload[0] = p; payload[1] = seq;
            for (unsigned k = 2; k < n; ++k) payload[k] = seq ^ (p << 24) ^ k;
            ring.lock();
            uint32_t* w = ring.reserve(n + 1);
            w[0] = 1 | (n << 8);
            memcpy(w + 1, payload.data(), n * 4);
            w[3] = ticket++;
            ring.commit(n + 1, seq % 17 == 0 || seq + 1 == iterations);
            ring.unlock();
            payload.fill(0xdeadbeef);
            if (seq % 29 == 0) std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });
    for (auto& writer : writers) writer.join();
    consumer.join();
    for (unsigned n : next) assert(n == iterations);

    std::mt19937 random(17);
    for (uint32_t capacity : {0u, 1u, 3u, 7u, 256u}) {
        MessageRing<uint32_t> queue;
        queue.reset(capacity);
        std::deque<uint32_t> expected;
        for (unsigned i = 0; i < 100000; ++i) {
            unsigned op = random() % 8;
            if (op < 4 && expected.size() < capacity) {
                uint32_t value = random();
                if (op & 1) { queue.push_front(value); expected.push_front(value); }
                else { queue.push_back(value); expected.push_back(value); }
            } else if (op < 7 && !expected.empty()) {
                assert(queue.front() == expected.front());
                queue.pop_front(); expected.pop_front();
            } else if (op == 7) { queue.clear(); expected.clear(); }
            assert(queue.size() == expected.size() && queue.empty() == expected.empty());
            for (unsigned k = 0; k < expected.size(); ++k) assert(queue[k] == expected[k]);
        }
    }
}
'''

NEON_MODEL = r'''
#pragma once
#include <cstdint>
struct float32x4_t { float v[4]; };
struct int32x4_t { int32_t v[4]; };
struct int32x4x2_t { int32x4_t val[2]; };
struct int32x4x3_t { int32x4_t val[3]; };
inline float32x4_t vdupq_n_f32(float v) { return {{v,v,v,v}}; }
inline int32x4_t vdupq_n_s32(int32_t v) { return {{v,v,v,v}}; }
inline float32x4_t vld1q_f32(const float* p) { return {{p[0],p[1],p[2],p[3]}}; }
inline void vst1q_f32(float* p, float32x4_t a) { for (int i=0;i<4;++i) p[i]=a.v[i]; }
inline float32x4_t vmulq_f32(float32x4_t a, float32x4_t b) { for(int i=0;i<4;++i) a.v[i]*=b.v[i]; return a; }
inline float32x4_t vaddq_f32(float32x4_t a, float32x4_t b) { for(int i=0;i<4;++i) a.v[i]+=b.v[i]; return a; }
inline float32x4_t vmulq_n_f32(float32x4_t a, float b) { return vmulq_f32(a,vdupq_n_f32(b)); }
inline int32x4x2_t vld2q_s32(const int32_t* p) { int32x4x2_t a; for(int i=0;i<4;++i) { a.val[0].v[i]=p[i*2]; a.val[1].v[i]=p[i*2+1]; } return a; }
inline float32x4_t vcvtq_f32_s32(int32x4_t a) { float32x4_t b; for(int i=0;i<4;++i) b.v[i]=float(a.v[i]); return b; }
inline int32x4_t vcvtq_s32_f32(float32x4_t a) { int32x4_t b; for(int i=0;i<4;++i) b.v[i]=int32_t(a.v[i]); return b; }
inline float32x4_t vextq_f32(float32x4_t a, float32x4_t b, int n) { float32x4_t c; for(int i=0;i<4;++i) c.v[i]=(i+n<4)?a.v[i+n]:b.v[i+n-4]; return c; }
inline int32x4_t vshlq_s32(int32x4_t a, int32x4_t b) { for(int i=0;i<4;++i) a.v[i] >>= -b.v[i]; return a; }
inline void vst3q_s32(int32_t* p, int32x4x3_t a) { for(int i=0;i<4;++i) for(int j=0;j<3;++j) p[i*3+j]=a.val[j].v[i]; }
inline float vgetq_lane_f32(float32x4_t a, int lane) { return a.v[lane]; }
'''

AUDIO = r'''
#include <cassert>
#include <cmath>
#include <cstring>
#include <random>
#define __SWITCH__ 1
#define __aarch64__ 1
#include "hle/ax_simd.h"
int main() {
    std::mt19937 random(24);
    for (unsigned test=0; test<10000; ++test) {
        float samples[96], expected[96], out[96], outExpected[96];
        for (int i=0;i<96;++i) {
            samples[i] = expected[i] = float(int32_t(random()) / 256);
            out[i] = outExpected[i] = float(int32_t(random()) / 256);
        }
        float vol = uint16_t(random()) / 32768.0f;
        float delta = test % 3 ? int16_t(random()) / 32768.0f : 0;
        float gain=vol;
        for (int i=0;i<96;++i) { gain += delta; expected[i] *= gain; }
        axsimd::envelope(samples,96,vol,delta);
        assert(!memcmp(samples,expected,sizeof samples));
        gain=vol;
        for (int i=0;i<96;++i) { if(delta) gain += delta; outExpected[i] += samples[i]*gain; }
        float finalGain=axsimd::mix(samples,out,96,vol,delta);
        assert(finalGain==gain && !memcmp(out,outExpected,sizeof out));
        int32_t in[96], up[144], upExpected[144];
        for(int& v : in) v = int32_t(random()) / 2; // all scalar conversions defined
        float hist = float(int32_t(random()) / 2), prev=hist;
        int shift=test%2?8:0;
        for(int i=0;i<96;i+=2) {
            float s0=float(in[i]), s1=float(in[i+1]);
            upExpected[i/2*3] = int32_t(prev*0.66666669f+s0*0.33333331f) >> shift;
            upExpected[i/2*3+1] = int32_t(s0) >> shift;
            upExpected[i/2*3+2] = int32_t(s1*0.66666669f+s0*0.33333331f) >> shift;
            prev=s1;
        }
        axsimd::upsample(in,up,96,hist,shift);
        assert(hist==prev && !memcmp(up,upExpected,sizeof up));
    }
}
'''

DC_PREFIX = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <random>
#define WWHD_HAS_VULKAN 1
static uint8_t* const PPC_MEM_BASE = reinterpret_cast<uint8_t*>(uintptr_t(0x1000000000ull));
static uint8_t* const g_mem2_host = PPC_MEM_BASE + 0x10000000;
static constexpr size_t kMem2Size = 0x40000000;
static bool g_mem2_gpu = true;
static uintptr_t cleaned;
static size_t bytes;
static void armDCacheClean(void* p, size_t n) { assert(!bytes); cleaned = uintptr_t(p); bytes = n; }
namespace gx2 {
struct Watch {
    uint32_t address;
    size_t bytes;
    void notify(uint32_t a, size_t n) { address = a; bytes = n; }
} shaderProgramWrites, texturePageWrites;
}
'''

DC_MAIN = r'''
int main() {
    const uintptr_t base = uintptr_t(PPC_MEM_BASE), begin = uintptr_t(g_mem2_host), end = begin + kMem2Size;
    const uintptr_t starts[] = {base, begin - 64, begin, begin + 1, end - 1, end, end + 64};
    for (bool imported : {false, true}) for (uintptr_t address : starts)
        for (size_t size : {size_t(0), size_t(1), size_t(64), size_t(kMem2Size), size_t(0x100000000ull), SIZE_MAX}) {
            g_mem2_gpu = imported; cleaned = bytes = 0;
            gx2::shaderProgramWrites.bytes = gx2::texturePageWrites.bytes = 0;
            switch_dc_store(reinterpret_cast<void*>(address), size);
            const unsigned __int128 rangeEnd = static_cast<unsigned __int128>(address) + size;
            const uintptr_t first = std::max(address, begin);
            const unsigned __int128 last = std::min(rangeEnd, static_cast<unsigned __int128>(end));
            const size_t expected = imported && last > first ? size_t(last - first) : 0;
            assert(bytes == expected && (!expected || cleaned == first));
            assert(gx2::shaderProgramWrites.address == uint32_t(address - base));
            assert(gx2::texturePageWrites.address == uint32_t(address - base));
            assert(gx2::shaderProgramWrites.bytes == size && gx2::texturePageWrites.bytes == size);
        }
}
'''

EMIT_PREFIX = r'''
#include <array>
#include <cassert>
#include <cstring>
#include <mutex>
#include <vector>
#include "gx2/command_ring.h"
#include "gx2/gx2_cmd.h"
#define __SWITCH__ 1
#define LOG(...) ((void)0)
namespace host { template<class F> void with_autorelease_pool(F&& f) { f(); } }
namespace mem {
static std::array<uint8_t, 4096> memory;
uint8_t* ptr(uint32_t address) { assert(address < memory.size()); return memory.data() + address; }
}
using uint32 = uint32_t;
namespace gx2 {
struct Snapshot { Op op; std::vector<uint32> words; };
static std::vector<Snapshot> snapshots;
static std::recursive_mutex g_exec_mutex;
static std::mutex g_q_mutex;
static std::mutex g_fence_issue_mutex;
static std::condition_variable g_q_done_cv;
static CommandRing<4096> g_q_ring;
static std::vector<uint32> g_q_pending, g_q_work;
static std::condition_variable g_q_cv;
static bool g_q_waiting = false;
static bool g_render_thread = true;
static uint64_t g_fence_issued = 0, g_fence_done = 0;
static std::atomic<uint64_t> g_main_sync_ns{0};
static uint64_t now_ns() { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }
static void execute_one(Op, const uint32*, uint32);
static void render_thread_main() {
    for (;;) {
#ifdef __SWITCH__
        uint32 count;
        const uint32* words = g_q_ring.wait(count);
#else
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        const uint32 count = uint32(g_q_work.size());
        const uint32* words = g_q_work.data();
#endif
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            for (uint32 i = 0; i < count;) {
                uint32 n = words[i] >> 8;
                execute_one(Op(words[i] & 255), words + i + 1, n);
                i += n + 1;
            }
        }
#ifdef __SWITCH__
        g_q_ring.complete(count);
#else
        g_q_work.clear();
#endif
    }
}
static void apply_regs(uint32 first, const uint32* values, uint32 count) {
    Snapshot result{OP_SET_REGS, {first}};
    result.words.insert(result.words.end(), values, values + count);
    snapshots.push_back(result);
}
'''

EMIT_EXECUTE = r'''
static void execute_one(Op op, const uint32* p, uint32 n) {
    switch (op) {
'''

EMIT_MAIN = r'''
    case OP_NOP: break;
    default: snapshots.push_back({op, std::vector<uint32>(p, p + n)}); break;
    }
}
} // gx2
int main() {
    using namespace gx2;
    uint32 values[32];
    for (unsigned i=0;i<32;++i) values[i]=i*7;
    // Native-endian display-list encoding, direct prefix copy and overflow.
    t_rec = {256,256,512};
    set_regs(123,values,32);
    emit(OP_DRAW,{1,2,3,4});
    emit(OP_NOP,nullptr,0);
    const uint32 end=t_rec.pos;
    set_regs(999,values,32); // overflow must leave the previous bytes intact
    assert(t_rec.pos==end && snapshots.empty());
    const auto* list=reinterpret_cast<const uint32*>(mem::ptr(256));
    assert(list[0]==(OP_SET_REGS | (33<<8)) && list[1]==123);
    for(unsigned i=0;i<32;++i) assert(list[i+2]==values[i]);
    assert(list[34]==(OP_DRAW | (4<<8)) && list[35]==1 && list[38]==4 && list[39]==OP_NOP);
    t_rec={};
    set_regs(123,values,32);
    values[0]=99999; // emitted values must already be owned
    emit(OP_DRAW,{4,3,2,1});
    emit_host(OP_DRAW_DONE,{1}); // save-state/shutdown drain boundary
    g_fence_issued=UINT32_MAX; // fence id must survive the 32-bit rollover
    render_sync();
    assert(g_fence_done==uint64_t(UINT32_MAX)+1);
    assert(snapshots.size()==3 && snapshots[0].op==OP_SET_REGS);
    assert(snapshots[0].words[0]==123 && snapshots[0].words[1]==0);
    assert(snapshots[1].op==OP_DRAW && snapshots[2].op==OP_DRAW_DONE && snapshots[2].words[0]==1);
    std::thread syncers[4];
    for (uint32 p = 0; p < 4; ++p) syncers[p] = std::thread([p] {
        for (uint32 seq = 0; seq < 100; ++seq) {
            emit(OP_DRAW, {p, seq});
            render_sync();
        }
    });
    for (auto& thread : syncers) thread.join();
    render_sync();
    assert(snapshots.size() == 403);
    uint32 next[4] = {};
    for (unsigned i = 3; i < snapshots.size(); ++i) {
        const auto& s = snapshots[i];
        assert(s.op == OP_DRAW && s.words[1] == next[s.words[0]]++);
    }
    snapshots.resize(3);
    g_render_thread=false;
    set_regs(222,values,32);
    emit(OP_DRAW,{6,7,8,9});
    set_regs(222,nullptr,0);
    render_sync();
    assert(snapshots.size()==5 && snapshots[3].op==OP_SET_REGS && snapshots[3].words[1]==99999);
    assert(snapshots[4].op==OP_DRAW && snapshots[4].words[0]==6);
    // The production worker is intentionally process-lifetime.
    std::_Exit(0);
}
'''

OUTPUT_PREFIX = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
constexpr int kCapacity = 128, kRate = 48000;
static int16_t g_ring[kCapacity * 2];
static std::atomic<uint32_t> g_read{0}, g_write{0};
static std::atomic<bool> g_flush{false};
static std::atomic<uint64_t> g_underrun{0}, g_dropped{0};
static bool g_unit = true;
static FILE* g_dump = nullptr;
static uint32_t g_dump_frames = 0;
static void write_wav_header() { assert(false); }
'''

OUTPUT_MAIN = r'''
int main() {
    std::mt19937 random(32);
    std::deque<std::array<int16_t, 2>> expected;
    g_read = g_write = UINT32_MAX - 7; // force sequence rollover and storage wrap
    uint64_t underrun = 0, dropped = 0;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        int16_t samples[kCapacity * 2];
        const unsigned frames = random() % (kCapacity + 1);
        if (random() & 1) {
            for (auto& s : samples) s = int16_t(random());
            push(samples, frames);
            if (expected.size() + frames > kCapacity) dropped += frames;
            else for (unsigned i = 0; i < frames; ++i) expected.push_back({samples[i * 2], samples[i * 2 + 1]});
        } else {
            if (trial % 5 == 0) { g_flush = true; expected.clear(); }
            pull(samples, frames);
            for (unsigned i = 0; i < frames; ++i) {
                std::array<int16_t, 2> s{};
                if (expected.empty()) ++underrun;
                else { s = expected.front(); expected.pop_front(); }
                assert(samples[i * 2] == s[0] && samples[i * 2 + 1] == s[1]);
            }
        }
        assert(g_write - g_read == expected.size());
        assert(g_dropped == dropped && g_underrun == underrun);
    }
}
'''

if __name__ == "__main__":
    unittest.main()
