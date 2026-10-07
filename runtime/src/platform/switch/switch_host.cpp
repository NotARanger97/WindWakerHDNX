// Nintendo Switch (libnx) platform pieces: guest memory, thread placement, log file, crash log.
#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <malloc.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ppc.h"
#ifdef WWHD_HAS_VULKAN
#include "gx2/shader_program_writes.h"
#endif

namespace threads { Cpu* current(); }

extern "C" { extern int __system_argc; extern char** __system_argv; }  // libnx argv (main's argc/argv)
extern "C" std::atomic<uint64_t> g_vk_draw_calls;  // gfx/vulkan/draw.cpp
static constexpr const char* kDir = "sdmc:/switch/wwhd";

// ---------------------------------------------------------------- guest memory
// The 4 GiB guest window is reserved in the address space; only the regions the runtime uses get
// memory (Horizon has no demand paging). MEM2 (1 GiB: the game's data, heaps, vertex and index
// buffers) is placed inside the stack region and mapped there with svcMapMemory: that memory stays
// CPU-cached and can be mapped for the GPU, so the renderer imports it into Vulkan and draws read
// guest buffers in place (like the Wii U GPU). The other regions are heap memory remapped as code
// memory (svcCreateCodeMemory + MapOwner), which works at any address but cannot be GPU-mapped.
// If no stack-region placement is found, MEM2 is code memory too and the renderer copies.
namespace {
struct Zone { u32 start; u32 size; const char* what; };
const Zone kZones[] = {
    {0x02000000, 0x01000000, "text"},
    {0x10000000, 0x40000000, "MEM2 (data + heaps)"},
    {0x60000000, 0x10000000, "runtime objects"},
    {0xC0000000, 0x03000000, "import slots"},
    {0xE0000000, 0x02800000, "foreground bucket"},
    {0xF4000000, 0x02000000, "MEM1"},
};
constexpr u32 kMem2 = 0x10000000, kMem2Size = 0x40000000;
struct Range { u64 begin = 0, end = 0; };
Range region(InfoType a, InfoType sz) {
    u64 addr = 0, size = 0;
    if (R_FAILED(svcGetInfo(&addr, a, CUR_PROCESS_HANDLE, 0)) || R_FAILED(svcGetInfo(&size, sz, CUR_PROCESS_HANDLE, 0))) return {};
    return {addr, addr + size};
}
bool unmapped(u64 begin, u64 end) {
    for (u64 a = begin; a < end;) {
        MemoryInfo info{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, a)) || (info.type & 0xFF) != MemType_Unmapped) return false;
        a = info.addr + info.size;
    }
    return true;
}
bool inside(const Range& r, u64 b, u64 e) { return b >= r.begin && e <= r.end; }
bool overlaps(const Range& r, u64 b, u64 e) { return b < r.end && e > r.begin; }
// The parts of [b, e) inside and outside the stack region (a zone straddling its boundary is mapped
// in pieces: svcMapMemory inside, code memory outside).
template <class F> void stack_pieces(const Range& stack, u64 b, u64 e, F&& f) {
    const u64 ib = std::max(b, stack.begin), ie = std::min(e, stack.end);
    if (ib >= ie) { f(b, e, false); return; }
    if (b < ib) f(b, ib, false);
    f(ib, ie, true);
    if (ie < e) f(ie, e, false);
}
// a window base with MEM2 inside the stack region and every other zone free (its stack-region part
// unmapped, the rest unmapped in the ASLR region outside the heap and alias regions)
u64 find_stack_placement(const Range& stack, const Range& aslr, const Range& heap, const Range& alias) {
    for (u64 a = stack.begin; a < stack.end;) {
        MemoryInfo info{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, a))) return 0;
        u64 fb = std::max<u64>(info.addr, stack.begin), fe = std::min<u64>(info.addr + info.size, stack.end);
        if ((info.type & 0xFF) == MemType_Unmapped)
            for (u64 m = (fb + 0x1FFFFF) & ~0x1FFFFFull; m + kMem2Size <= fe; m += 0x200000) {
                if (m < kMem2) continue;
                u64 base = m - kMem2;
                bool ok = true;
                for (const Zone& z : kZones) {
                    if (z.start == kMem2) continue;
                    stack_pieces(stack, base + z.start, base + z.start + z.size, [&](u64 b, u64 e, bool in) {
                        if (!ok) return;
                        if (in) ok = unmapped(b, e);
                        else ok = inside(aslr, b, e) && !overlaps(heap, b, e) && !overlaps(alias, b, e) && unmapped(b, e);
                    });
                    if (!ok) break;
                }
                if (ok) return base;
            }
        a = info.addr + info.size;
    }
    return 0;
}
u8* g_mem2_host = nullptr;
bool g_mem2_gpu = false;
}  // namespace

// MEM2 host address and whether it can be imported for the GPU (switch_mem2_importable)
extern "C" bool switch_mem2(uint8_t** host) { if (host) *host = g_mem2_host; return g_mem2_gpu; }

namespace mem { uint8_t* switch_map_guest_memory(); }
uint8_t* mem::switch_map_guest_memory() {
    static uint8_t* mapped = nullptr;  // first call: switch_platform_init, before any thread exists
    if (mapped) return mapped;
    constexpr u64 kWindow = 0x100000000ull;
    Range stack = region(InfoType_StackRegionAddress, InfoType_StackRegionSize);
    Range aslr = region(InfoType_AslrRegionAddress, InfoType_AslrRegionSize);
    Range heap = region(InfoType_HeapRegionAddress, InfoType_HeapRegionSize);
    Range alias = region(InfoType_AliasRegionAddress, InfoType_AliasRegionSize);
    virtmemLock();
    u64 base = getenv("WWHD_MEM2_COPY") ? 0 : find_stack_placement(stack, aslr, heap, alias);
    bool stack_mem2 = base != 0;
    if (stack_mem2) {
        for (const Zone& z : kZones) virtmemAddReservation((u8*)base + z.start, z.size);
    } else {
        void* b = virtmemFindAslr(kWindow, 0x10000);
        if (b && virtmemAddReservation(b, kWindow)) base = (u64)b;
    }
    virtmemUnlock();
    {
        // The loader puts our main thread's stack at a random place in the stack region; sometimes it
        // leaves no MEM2 placement whose other zones end below the heap region, and MEM2 would fall
        // back to code memory (renderer copies every vertex/index buffer: ~9 ms more per frame).
        // hbloader picks a new place on every load: relaunch ourselves (at most 8 times).
        int tries = 0;
        for (int i = 1; i < __system_argc; ++i)
            if (!strncmp(__system_argv[i], "--mem2-retry=", 13)) tries = atoi(__system_argv[i] + 13);
        if (tries) fprintf(stderr, "[switch] relaunched %d time(s) for the MEM2 placement\n", tries);
        // test aid: WWHD_TEST_MEM2_RELAUNCH=n treats the first n placements as failed
        const int forced = getenv("WWHD_TEST_MEM2_RELAUNCH") ? atoi(getenv("WWHD_TEST_MEM2_RELAUNCH")) : 0;
        if ((!stack_mem2 || tries < forced) && !getenv("WWHD_MEM2_COPY") && tries < 8 && envHasNextLoad() && __system_argc > 0) {
            static char args[512];
            snprintf(args, sizeof args, "\"%s\" --mem2-retry=%d", __system_argv[0], tries + 1);
            fprintf(stderr, "[switch] no GPU-readable MEM2 placement: relaunching (%s)\n", args);
            envSetNextLoad(__system_argv[0], args);
            exit(0);
        }
    }
    fprintf(stderr, "[switch] regions: heap %llX-%llX alias %llX-%llX\n", (unsigned long long)heap.begin,
            (unsigned long long)heap.end, (unsigned long long)alias.begin, (unsigned long long)alias.end);
    if (!stack_mem2) {
        // what lies around the stack region (diagnostics for a failed placement)
        for (u64 a = stack.begin > 0x100000000ull ? stack.begin - 0x100000000ull : 0, n = 0;
             a < stack.end + 0x100000000ull && n < 24; ++n) {
            MemoryInfo info{};
            u32 page = 0;
            if (R_FAILED(svcQueryMemory(&info, &page, a))) break;
            fprintf(stderr, "[switch]   %llX-%llX type %X\n", (unsigned long long)info.addr,
                    (unsigned long long)(info.addr + info.size), info.type & 0xFF);
            a = info.addr + info.size;
        }
    }
    if (!base) {
        fprintf(stderr, "[switch] cannot reserve the 4 GiB guest window\n");
        abort();
    }
    fprintf(stderr, "[switch] stack region %llX-%llX, aslr %llX-%llX; MEM2 %s\n", (unsigned long long)stack.begin,
            (unsigned long long)stack.end, (unsigned long long)aslr.begin, (unsigned long long)aslr.end,
            stack_mem2 ? "in the stack region (GPU-readable)" : "as code memory (renderer copies)");
    u64 total = 0;
    for (const Zone& z : kZones) {
        u8* dst = (u8*)base + z.start;
        Result rc = 0;
        const Range none{};
        stack_pieces(stack_mem2 ? stack : none, (u64)dst, (u64)dst + z.size, [&](u64 b, u64 e, bool in) {
            if (R_FAILED(rc)) return;
            const u64 size = e - b;
            // 2 MiB-aligned source (and destination): the kernel can map 2 MiB blocks instead of
            // 4 KiB pages; with 4 KiB pages random guest reads cost ~45% more (TLB misses).
            void* src = memalign((size & 0x1FFFFF) == 0 && (b & 0x1FFFFF) == 0 ? 0x200000 : 0x1000, size);
            if (!src) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); return; }
            if (in) {
                rc = svcMapMemory((void*)b, src, size);  // stack region: only svcMapMemory maps here; the source stays owned
            } else {
                Handle h = INVALID_HANDLE;
                rc = svcCreateCodeMemory(&h, src, size);
                if (R_SUCCEEDED(rc)) {
                    virtmemLock();
                    rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, (void*)b, size, Perm_Rw);
                    virtmemUnlock();
                }
            }
        });
        if (R_FAILED(rc)) {
            fprintf(stderr, "[switch] cannot map guest %s at %08X (%u MiB): rc=%08X\n", z.what, z.start, z.size >> 20, rc);
            abort();
        }
        if (z.start == kMem2) {
            g_mem2_host = dst;
            g_mem2_gpu = stack_mem2 && inside(stack, (u64)dst, (u64)dst + z.size);
        }
        memset(dst, 0, z.size);
        total += z.size;
    }
    fprintf(stderr, "[switch] guest window at %p, %llu MiB mapped\n", (void*)base, (unsigned long long)(total >> 20));
    if (getenv("WWHD_MEMBENCH")) {
        // dependent random reads over 256 MiB: guest MEM2 (stack-region alias) vs ordinary heap memory
        auto bench = [](u8* p, const char* what) {
            constexpr u64 n = 256ull << 20, steps = 4u << 20;
            u32* w = (u32*)p;
            u64 x = 88172645463325252ull;
            for (u64 i = 0; i < n / 4; i += 1024) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; w[i] = u32(x % (n / 4)); }
            u64 t0 = armGetSystemTick(), idx = 0, sum = 0;
            for (u64 i = 0; i < steps; ++i) { idx = (w[(idx * 1024 + (i & 1023)) % (n / 4)] + i * 4099) % (n / 4); sum += idx; }
            u64 t1 = armGetSystemTick();
            fprintf(stderr, "[membench] %s: %.1f ns per dependent random read (%llu)\n", what,
                    armTicksToNs(t1 - t0) / double(steps), (unsigned long long)(sum & 1));
        };
        bench((u8*)base + kMem2, "guest MEM2");
        if (u8* h = (u8*)memalign(0x200000, 256ull << 20)) { memset(h, 0, 256ull << 20); bench(h, "heap"); free(h); }
    }
    mapped = (uint8_t*)base;
    return mapped;
}

// Guest DCFlushRange / DCStoreRange: the Wii U GPU does not snoop CPU caches either, so the game
// flushes every buffer the GPU reads. With MEM2 imported for the GPU, do the same on the host.
extern "C" void switch_dc_store(const void* p, size_t size) {
    if (g_mem2_gpu && size) {
        // Only this imported allocation is read in place by NVK. Other guest
        // regions are copied by the CPU into uploads, so cache cleaning there
        // is unnecessary. Clip crossing ranges without overflowing address+size.
        const uintptr_t address = reinterpret_cast<uintptr_t>(p);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(g_mem2_host);
        const uintptr_t end = begin + kMem2Size;
        if (address < end && (address >= begin || size > begin - address)) {
            const uintptr_t first = address < begin ? begin : address;
            const size_t skip = first - address;
            armDCacheClean(reinterpret_cast<void*>(first), std::min(size - skip, size_t(end - first)));
        }
    }
#ifdef WWHD_HAS_VULKAN
    const uintptr_t base = reinterpret_cast<uintptr_t>(PPC_MEM_BASE);
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    if (address >= base && address - base < 0x100000000ull)
    {
        gx2::shaderProgramWrites.notify(uint32_t(address - base), size);
        gx2::texturePageWrites.notify(uint32_t(address - base), size);
    }
#endif
}

// ---------------------------------------------------------------- threads
// Guest core N runs on CPU core N (cores 0-2 belong to the application). Host threads default
// to core 0; a thread is moved when it takes a guest core. Guest threads use priority 59, at
// which Horizon time-slices threads of equal priority; runtime helpers (renderer, audio) run
// above it so they preempt guest code on their core.
static void profile_register();
static thread_local int t_core = -1;
// WWHD_CORE_MAP=a,b,c: the CPU core of guest cores 0, 1, 2 (default 0,1,2), e.g. 0,1,0 puts the two
// light guest cores together and leaves CPU core 2 to the renderer (WWHD_RENDER_CORE=2).
static int host_core_of(uint32_t core) {
    static int map[3] = {-1, -1, -1};
    if (map[0] < 0) {
        int a = 0, b = 1, c = 2;
        if (const char* e = getenv("WWHD_CORE_MAP")) sscanf(e, "%d,%d,%d", &a, &b, &c);
        map[0] = a; map[1] = b; map[2] = c;
    }
    return core < 3 ? map[core] : (int)core;
}
void switch_pin_guest_core(uint32_t core) {
    if ((int)core == t_core) return;
    if (t_core < 0) svcSetThreadPriority(CUR_THREAD_HANDLE, 59);
    int cpu = host_core_of(core);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, cpu, 1u << cpu);
    if (t_core < 0) profile_register();
    t_core = (int)core;
}
// WWHD_HOLD_PRIO=n: a host thread runs at priority n while it holds a guest core (59 otherwise),
// so a thread that gets its core back is not stuck behind an equal-priority thread on that CPU
// (Horizon does not preempt between equal priorities).
void switch_hold_core(bool holding) {
    static const int hold = getenv("WWHD_HOLD_PRIO") ? atoi(getenv("WWHD_HOLD_PRIO")) : 0;
    if (hold) svcSetThreadPriority(CUR_THREAD_HANDLE, holding ? hold : 59);
}
extern "C" void switch_set_helper_thread(int core, int priority) {
    svcSetThreadPriority(CUR_THREAD_HANDLE, priority);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (t_core < 0) profile_register();
    t_core = 100;  // not a guest core thread
}
// cache writers (host::background_thread): the lowest application priority on any core, so they only
// use time the guest, front-end and render threads leave idle
extern "C" void switch_set_background_thread() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7);
}

// ---------------------------------------------------------------- sampling profiler
// WWHD_PROFILE=1: every 1 ms one registered thread (guest core threads, renderer, audio) is paused
// and its pc and lr recorded; every 10 s profile.log gets the hottest image offsets per thread
// (symbolize against wwhd.elf: offsets are from __start__, the ELF's address 0).
extern "C" void _start();  // first instruction of the image (ELF address 0; __start__ is absolute)
namespace {
struct ProfThread { Handle h; std::string name; };
std::mutex g_prof_m;
std::vector<ProfThread> g_prof_threads;

void profiler_main() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    const u64 base = (u64)&_start;
    std::unordered_map<std::string, std::unordered_map<u64, u32>> pcs, lrs;
    std::unordered_map<std::string, std::unordered_map<std::string, u32>> stacks;
    std::unordered_map<std::string, u32> counts;
    u64 last = armGetSystemTick();
    size_t next = 0;
    for (;;) {
        svcSleepThread(1000000);
        ProfThread t;
        {
            std::lock_guard<std::mutex> lk(g_prof_m);
            if (g_prof_threads.empty()) continue;
            t = g_prof_threads[next++ % g_prof_threads.size()];
        }
        if (R_FAILED(svcSetThreadActivity(t.h, ThreadActivity_Paused))) continue;
        ThreadContext ctx{};
        Result rc = svcGetThreadContext3(&ctx, t.h);
        for (int retry = 0; R_FAILED(rc) && retry < 8; retry++) {
            svcSleepThread(0);
            rc = svcGetThreadContext3(&ctx, t.h);
        }
        // frame records (x29 chain) while the thread is still paused
        u64 frames[6] = {};
        if (R_SUCCEEDED(rc)) {
            u64 fp = ctx.fp, sp = ctx.sp;
            for (int d = 0; d < 6; d++) {
                if (fp < sp || fp >= sp + 0x100000 || (fp & 7)) break;
                frames[d] = ((u64*)fp)[1];
                u64 next = ((u64*)fp)[0];
                if (next <= fp) break;
                fp = next;
            }
        }
        svcSetThreadActivity(t.h, ThreadActivity_Runnable);
        if (R_FAILED(rc)) continue;
        {
            char key[160];
            int n = snprintf(key, sizeof key, "%llX %llX", (unsigned long long)(ctx.pc.x - base), (unsigned long long)(ctx.lr - base));
            for (int d = 0; d < 6 && frames[d]; d++)
                n += snprintf(key + n, sizeof key - n, " %llX", (unsigned long long)(frames[d] - base));
            stacks[t.name][key]++;
        }
        counts[t.name]++;
        pcs[t.name][ctx.pc.x - base]++;
        lrs[t.name][ctx.lr - base]++;
        if (armTicksToNs(armGetSystemTick() - last) < 10000000000ull) continue;
        last = armGetSystemTick();
        if (FILE* f = fopen("sdmc:/switch/wwhd/profile.log", "a")) {
            for (auto& [name, n] : counts) {
                fprintf(f, "# thread %s: %u samples\n", name.c_str(), n);
                for (auto* m : {&pcs[name], &lrs[name]}) {
                    std::vector<std::pair<u32, u64>> rows;
                    for (auto& [k, v] : *m) rows.emplace_back(v, k);
                    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    for (size_t i = 0; i < rows.size() && i < (m == &pcs[name] ? 6000u : 40u); i++)
                        fprintf(f, "%s %5.2f%% %llX\n", m == &pcs[name] ? "pc" : "lr", rows[i].first * 100.0 / n,
                                (unsigned long long)rows[i].second);
                }
                std::vector<std::pair<u32, std::string>> srows;
                for (auto& [k, v] : stacks[name]) srows.emplace_back(v, k);
                std::sort(srows.begin(), srows.end(), [](auto& a, auto& b) { return a.first > b.first; });
                for (size_t i = 0; i < srows.size() && i < 25; i++)
                    fprintf(f, "stack %5.2f%% %s\n", srows[i].first * 100.0 / n, srows[i].second.c_str());
            }
            fclose(f);
        }
        pcs.clear();
        lrs.clear();
        stacks.clear();
        counts.clear();
    }
}
}  // namespace

// Per-thread CPU time (Horizon thread tick counts) of the main game thread and the renderer,
// logged with the fps line: the frame rate is quantized by vsync, these are not.
namespace {
std::atomic<Handle> g_render_handle{INVALID_HANDLE}, g_record_handle{INVALID_HANDLE};
u64 thread_ticks(Handle h) {
    u64 t = 0;
    if (h == INVALID_HANDLE || R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h, (u64)-1))) return 0;
    return t;
}
}  // namespace
extern "C" void switch_note_render_thread() { g_render_handle = threadGetCurHandle(); }
// Idle ticks per CPU core: the kernel reports only the calling thread's core, so the pinned threads
// (game: core 1, render: core 2, VK record: core 0) sample their own core at most once per ms.
static std::atomic<u64> g_core_idle[3];
extern "C" void switch_sample_core_idle() {
    thread_local u64 last = 0;
    const u64 now = armGetSystemTick();
    if (now - last < 19200) return;
    last = now;
    const u32 core = svcGetCurrentProcessorNumber();
    u64 idle = 0;
    if (core < 3 && R_SUCCEEDED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, (u64)-1))) g_core_idle[core] = idle;
}
extern "C" void switch_note_record_thread() { g_record_handle = threadGetCurHandle(); }
static std::atomic<Handle> g_main_handle{INVALID_HANDLE};
// cumulative CPU time of the main game thread (0), renderer (1), VK record thread (2)
extern "C" uint64_t switch_thread_cpu_ns(int which) {
    const Handle h = which == 0 ? g_main_handle.load() : which == 1 ? g_render_handle.load() : g_record_handle.load();
    return armTicksToNs(thread_ticks(h));
}
// called on the thread that swaps (the game's main thread) every few frames
extern "C" void switch_log_thread_cpu(uint64_t frames) {
    g_main_handle = threadGetCurHandle();
    static u64 last_main = 0, last_render = 0, last_record = 0;
    u64 m = thread_ticks(CUR_THREAD_HANDLE), r = thread_ticks(g_render_handle.load());
    u64 k = thread_ticks(g_record_handle.load());
    static uint64_t last_draws = 0;
    const uint64_t draws = g_vk_draw_calls.load(std::memory_order_relaxed);
    switch_sample_core_idle();
    static u64 last_idle[3] = {}, last_tick = 0;
    const u64 tick = armGetSystemTick();
    char idleText[64] = "";
    if (last_tick) {
        int n = 0;
        for (int c = 0; c < 3; ++c) {
            const u64 idle = g_core_idle[c].load();
            n += snprintf(idleText + n, sizeof idleText - n, "%s%d", c ? "/" : "; idle % cores 0/1/2 ",
                          int(100.0 * double(idle - last_idle[c]) / double(tick - last_tick) + 0.5));
            last_idle[c] = idle;
        }
    } else for (int c = 0; c < 3; ++c) last_idle[c] = g_core_idle[c].load();
    last_tick = tick;
    if (last_main && frames)
        fprintf(stderr, "[switch] cpu ms/frame: main %.1f render %.1f record %.1f; draws %llu%s\n",
                armTicksToNs(m - last_main) / 1e6 / frames, armTicksToNs(r - last_render) / 1e6 / frames,
                armTicksToNs(k - last_record) / 1e6 / frames, (unsigned long long)((draws - last_draws) / frames), idleText);
    last_draws = draws;
    last_main = m;
    last_render = r;
    last_record = k;
}

static void profile_register() {
    static const bool on = getenv("WWHD_PROFILE") != nullptr;
    if (!on) return;
    static std::once_flag once;
    std::call_once(once, [] { new std::thread(profiler_main); });  // never joined
    char name[64];
    extern void switch_thread_name(char* out, size_t size);
    switch_thread_name(name, sizeof name);
    // WWHD_PROFILE=1 samples every registered thread, or a comma list of thread names
    std::string list = std::string(",") + getenv("WWHD_PROFILE") + ",";
    if (list != ",1," && list.find(std::string(",") + name + ",") == std::string::npos) return;
    std::lock_guard<std::mutex> lk(g_prof_m);
    g_prof_threads.push_back({threadGetCurHandle(), name});
}

extern "C" void switch_log_flush();
// ---------------------------------------------------------------- crash log
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;
void _start();

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    FILE* f = fopen("sdmc:/switch/wwhd/crash.log", "w");
    if (!f) svcExitProcess();
    const u64 base = (u64)&_start, mem = (u64)PPC_MEM_BASE;
    fprintf(f, "CRASH: error %u, esr %08X, far %016llX", ctx->error_desc, (unsigned)ctx->esr, (unsigned long long)ctx->far.x);
    if (ctx->far.x >= mem && ctx->far.x < mem + 0x100000000ull) fprintf(f, " (guest address %08X)", (unsigned)(ctx->far.x - mem));
    fprintf(f, "\npc %016llX (+%llX)  lr %016llX (+%llX)  sp %016llX\n", (unsigned long long)ctx->pc.x,
            (unsigned long long)(ctx->pc.x - base), (unsigned long long)ctx->lr.x, (unsigned long long)(ctx->lr.x - base),
            (unsigned long long)ctx->sp.x);
    for (int i = 0; i < 29; i++) fprintf(f, "x%-2d %016llX%s", i, (unsigned long long)ctx->cpu_gprs[i].x, i % 4 == 3 ? "\n" : "  ");
    fprintf(f, "\nimage base %016llX\n", (unsigned long long)base);
    // host return addresses on the stack (image offsets): a cheap backtrace
    u64 fp = ctx->fp.x;
    fprintf(f, "frame chain:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        u64 ra = ((u64*)fp)[1];
        fprintf(f, " +%llX", (unsigned long long)(ra - base));
        u64 next = ((u64*)fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    fprintf(f, "\n");
    if (Cpu* c = threads::current()) {
        fprintf(f, "guest lr=%08X ctr=%08X pc=%08X\n", c->lr, c->ctr, c->pc);
        for (int i = 0; i < 32; i++) fprintf(f, "r%-2d %08X%s", i, c->r[i], i % 8 == 7 ? "\n" : " ");
        fprintf(f, "guest call chain:");
        uint32_t sp = c->r[1];
        for (int i = 0; i < 24 && sp >= 0x10000000u && sp < 0x70000000u; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp || prev - sp > 0x100000u) break;
            fprintf(f, " %08X", ld32(prev + 4));
            sp = prev;
        }
        fprintf(f, "\n");
    }
    fclose(f);
    switch_log_flush();
    svcExitProcess();
}
}

// ---------------------------------------------------------------- clocks
// one line per call: CPU/GPU/memory clocks, power source, battery, mode (first call: hardware type)
extern "C" void switch_log_clocks() {
    static bool init = false, clk_ok = false, psm_ok = false;
    static ClkrstSession cpu{}, gpu{}, emc{};
    if (!init) {
        init = true;
        if (R_SUCCEEDED(clkrstInitialize()))
            clk_ok = R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3)) &&
                     R_SUCCEEDED(clkrstOpenSession(&gpu, PcvModuleId_GPU, 3)) &&
                     R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3));
        psm_ok = R_SUCCEEDED(psmInitialize());
        u64 hw = ~0ull;
        if (R_SUCCEEDED(splInitialize())) {
            splGetConfig(SplConfigItem_HardwareType, &hw);
            splExit();
        }
        static const char* const kHw[] = {"Icosa (Erista)", "Copper (Erista)", "Hoag (Lite, Mariko)", "Iowa (Mariko)",
                                          "Calcio (Mariko)", "Aula (OLED, Mariko)"};
        fprintf(stderr, "[switch] hardware type %llu: %s\n", (unsigned long long)hw, hw < 6 ? kHw[hw] : "unknown");
    }
    u32 c = 0, g = 0, m = 0;
    if (clk_ok) {
        clkrstGetClockRate(&cpu, &c);
        clkrstGetClockRate(&gpu, &g);
        clkrstGetClockRate(&emc, &m);
    }
    PsmChargerType charger = PsmChargerType_Unconnected;
    u32 battery = 0;
    if (psm_ok) {
        psmGetChargerType(&charger);
        psmGetBatteryChargePercentage(&battery);
    }
    fprintf(stderr, "[switch] clocks cpu %u gpu %u mem %u MHz | %s, battery %u%%, %s\n", c / 1000000, g / 1000000,
            m / 1000000, charger == PsmChargerType_Unconnected ? "battery" : "charger", battery,
            appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
}

// ---------------------------------------------------------------- log
// stderr goes to an in-memory device; a low-priority thread writes it to log.txt. A line-buffered
// file on the SD card made every log line a synchronous SD write on the logging thread (the game's
// main thread for the fps line, the renderer for its statistics): a dropped frame every log period.
namespace {
std::mutex g_log_m;
std::condition_variable g_log_cv;
std::string g_log_buf;
FILE* g_log_file = nullptr;
int log_open(struct _reent*, void*, const char*, int, int) { return 0; }
int log_close(struct _reent*, void*) { return 0; }
ssize_t log_write(struct _reent*, void*, const char* ptr, size_t len) {
    {
        std::lock_guard<std::mutex> lk(g_log_m);
        g_log_buf.append(ptr, len);
    }
    g_log_cv.notify_one();
    return ssize_t(len);
}
const devoptab_t g_log_dev = {
    .name = "wwlog",
    .structSize = sizeof(int),
    .open_r = log_open,
    .close_r = log_close,
    .write_r = log_write,
};
void log_write_out(std::string& chunk) {
    if (!chunk.empty() && g_log_file) {
        fwrite(chunk.data(), 1, chunk.size(), g_log_file);
        fflush(g_log_file);
    }
    chunk.clear();
}
void log_writer() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
    std::string chunk;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_log_m);
            g_log_cv.wait_for(lk, std::chrono::milliseconds(100), [] { return !g_log_buf.empty(); });
            chunk.swap(g_log_buf);
        }
        log_write_out(chunk);
        svcSleepThread(100'000'000);  // batch: at most ~10 SD writes a second
    }
}
}  // namespace
// synchronous: exit and crash paths (try_lock: the crashing thread may hold the buffer lock)
extern "C" void switch_log_flush() {
    fflush(stderr);
    if (!g_log_m.try_lock()) return;
    std::string chunk;
    chunk.swap(g_log_buf);
    g_log_m.unlock();
    log_write_out(chunk);
}

// ---------------------------------------------------------------- start-up
// The game: WWHD_GAME (env.txt), else a Cemu .wua of it ("Wind Waker" in the name) in
// sdmc:/switch/wwhd or sdmc:/switch/Cemu/games (read in place), else the extracted folders in
// sdmc:/switch/wwhd/game (code, content, meta).
std::string switch_game_dir() {
    if (const char* e = getenv("WWHD_GAME"); e && *e) return e;
    for (const char* dir : {"sdmc:/switch/wwhd", "sdmc:/switch/Cemu/games"}) {
        DIR* d = opendir(dir);
        if (!d) continue;
        std::string found;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".wua") == 0 &&
                name.find("Wind Waker") != std::string::npos) {
                found = std::string(dir) + "/" + name;
                break;
            }
        }
        closedir(d);
        if (!found.empty()) return found;
    }
    return "sdmc:/switch/wwhd/game";
}

void switch_platform_init() {
    mkdir(kDir, 0777);
    chdir(kDir);
    g_log_file = fopen("sdmc:/switch/wwhd/log.txt", "w");
    if (g_log_file && AddDevice(&g_log_dev) >= 0 && freopen("wwlog:/log", "w", stderr)) {
        setvbuf(stderr, nullptr, _IOLBF, 4096);
        atexit(switch_log_flush);  // the writer thread starts after the guest window is mapped
    } else {
        if (g_log_file) fclose(g_log_file);
        g_log_file = nullptr;
        if (freopen("sdmc:/switch/wwhd/log.txt", "w", stderr)) setvbuf(stderr, nullptr, _IOLBF, 4096);
    }
    // one screen: the GamePad picture has no window of its own
    setenv("WWHD_NO_GAMEPAD", "1", 0);
    // the Switch controller is a Wii U Pro Controller: the game then shows the GamePad's menus (items,
    // map, ...) on the TV picture; WWHD_PRO_CONTROLLER=0 in env.txt brings back GamePad mode
    setenv("WWHD_PRO_CONTROLLER", "1", 0);
    // measured defaults (env.txt overrides): guest cores 0 and 2 share CPU core 0, the renderer owns
    // core 2; NVK upload memory stays CPU-cached; exact-compare draw memos on
    setenv("WWHD_CORE_MAP", "0,1,0", 0);
    setenv("WWHD_RENDER_CORE", "2", 0);
    setenv("NVK_SWITCH_CPU_WRITE_MEM_UNCACHED", "false", 0);
    setenv("WWHD_VK_SHADER_STATE_MEMO", "1", 0);
    setenv("WWHD_VK_PIPELINE_LOOKASIDE", "1", 0);
    setenv("WWHD_VK_SKIP_VERTEX_BINDS", "1", 0);
    // GX2DrawDone waits for the frame before the last swap: the game computes the next frame while the
    // render thread draws this one (heavy village view on the hill: 20 -> 30 fps)
    setenv("WWHD_DRAWDONE_LAG", "1", 0);
    // a game thread woken by the vsync after its swap waits for a render thread that is a few ms late
    // with that swap, instead of sleeping a whole further vsync (hill view peaks: 41-50 ms frames -> ~38)
    setenv("WWHD_VK_READY_FLIP_PARK", "1", 0);
    // env.txt: KEY=VALUE lines become environment variables (the runtime's WWHD_* switches)
    if (FILE* f = fopen("sdmc:/switch/wwhd/env.txt", "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[switch] env %s=%s\n", line, eq + 1);
        }
        fclose(f);
    }
    fprintf(stderr, "[switch] Wind Waker HD recompiled, Switch build\n");
    {   // which CPU cores this process may use (core 3 normally belongs to the system)
        u64 coreMask = 0, prioMask = 0;
        svcGetInfo(&coreMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&prioMask, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0);
        s32 ideal = 0;
        u64 affinity = 0;
        svcGetThreadCoreMask(&ideal, &affinity, CUR_THREAD_HANDLE);
        Result r3 = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, 1u << 3);
        fprintf(stderr, "[switch] process core mask %llX, priority mask %llX; main thread core %d mask %llX; to core 3: %X\n",
                (unsigned long long)coreMask, (unsigned long long)prioMask, ideal, (unsigned long long)affinity, r3);
        if (R_SUCCEEDED(r3)) svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, (u32)affinity);
    }
    // Map the guest window before creating any thread: every thread stack lands at a random place in
    // the stack region, and MEM2 needs a 1 GiB hole there (two stacks were enough to break it).
    mem::switch_map_guest_memory();
    if (g_log_file) std::thread(log_writer).detach();
}
