// Guest threads run on host pthreads. OS synchronization objects live in guest
// memory (the game allocates them) and are backed by host objects keyed by
// their guest address.
#if !defined(_WIN32) && !defined(__SWITCH__)
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "platform/host.h"
#include "frame_trace.h"
#include "platform/sleep.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif


#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime.h"
#include "savestate.h"
#include "platform/perf_hint.h"
#include "message_ring.h"

// ---------------------------------------------------------------- time
namespace timebase {
static const auto g_boot = std::chrono::steady_clock::now();
uint64_t now() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot).count();
    return (uint64_t)((unsigned __int128)ns * kTicksPerSec / 1000000000ull);
}
static std::atomic<int64_t> g_guest_offset{0};
uint64_t guest_now() { return now() + (uint64_t)g_guest_offset.load(std::memory_order_relaxed); }
uint64_t to_guest(uint64_t host_ticks) { return host_ticks + (uint64_t)g_guest_offset.load(std::memory_order_relaxed); }
uint64_t to_host(uint64_t guest_ticks) { return guest_ticks - (uint64_t)g_guest_offset.load(std::memory_order_relaxed); }
void set_guest_now(uint64_t guest_ticks) { g_guest_offset = (int64_t)(guest_ticks - now()); }
}  // namespace timebase

extern "C" uint64_t ppc_timebase(void) { return timebase::guest_now(); }

static std::chrono::nanoseconds ticks_to_ns(uint64_t t) {
    return std::chrono::nanoseconds((int64_t)((unsigned __int128)t * 1000000000ull / timebase::kTicksPerSec));
}

// ---------------------------------------------------------------- OSThread
// Offsets into the guest OSThread structure (see Cemu coreinit_Thread.h)
namespace osthread {
constexpr uint32_t kSize = 0x6A0;
constexpr uint32_t kState = 0x324;
constexpr uint32_t kAttr = 0x325;
constexpr uint32_t kId = 0x326;
constexpr uint32_t kSuspend = 0x328;
constexpr uint32_t kEffPrio = 0x32C;
constexpr uint32_t kBasePrio = 0x330;
constexpr uint32_t kExitValue = 0x334;
constexpr uint32_t kStackBase = 0x394;
constexpr uint32_t kStackEnd = 0x398;
constexpr uint32_t kEntry = 0x39C;
constexpr uint32_t kSpecific = 0x57C;
constexpr uint32_t kName = 0x5C0;
constexpr uint32_t kAffinity = 0x304;
enum State : uint8_t { NONE = 0, READY = 1, RUNNING = 2, WAITING = 4, MORIBUND = 8 };
}  // namespace osthread

struct HostThread {
    uint32_t guest = 0;  // OSThread*
    Cpu cpu{};
    std::mutex m;
    std::condition_variable cv;
    int suspend = 1;
    bool started = false;
    bool exited = false;
    uint32_t exit_value = 0;
    uint32_t entry = 0, argc = 0, argv = 0;
    uint32_t core = 1;  // emulated core this thread is bound to (for OSGetCoreId)
#ifdef _WIN32
    HANDLE pt = nullptr;
#else
    pthread_t pt{};
#endif
    // scheduling
    int prio = 16;            // lower runs first; service threads (alarms, audio) use -1
    bool holds_core = false;
    uint32_t held_core = 0;   // core actually held (affinity may change while holding)
    int irq_off = 0;          // OSDisableInterrupts: no preemption
    std::chrono::steady_clock::time_point ready_since;  // when it started waiting for its core
    bool service = false;
    bool throttle = false;    // Miiverse operation manager: nap before each message (see OSReceiveMessage)
    // statistics
    std::chrono::steady_clock::time_point acquired;
    std::atomic<uint64_t> held_ns{0}, wait_ns{0};
#ifdef __APPLE__
    mach_port_t mach = 0;     // for CPU time statistics
    uint64_t last_cpu_us = 0;
#endif
    // save states: whether the thread is parked at a point where its whole state is its Cpu plus
    // guest memory plus the HLE objects (see park_wait)
    std::atomic<int> wst{0};   // kRunning, kParked or kRequester
    uint8_t wait_kind = 0;     // WaitKind of the park
    uint32_t wait_obj = 0;     // guest address of the object waited on
    bool woken = false;        // wakeup token of OSWaitEvent / OSSleepThread (guarded by the object's mutex)
    // loading a save state: park at this function entry (thread saved there, see try_entry_park)
    bool has_target = false;
    uint32_t tgt_fn = 0, tgt_r1 = 0, tgt_lr = 0;
};
enum WaitState { kRunning = 0, kParked = 1, kRequester = 2 };
enum WaitKind : uint8_t { W_NONE, W_MUTEX, W_EVENT, W_MSG_SEND, W_MSG_RECV, W_SLEEPQ, W_JOIN, W_RDV, W_SLEEP, W_SERVICE, W_ENTRY };

// core from an affinity mask (bit0 = core 0, bit1 = core 1, bit2 = core 2); fallback if none set
static uint32_t core_from_affinity(uint32_t mask, uint32_t fallback) {
    mask &= 7;
    if (!mask || mask == 7) return fallback;
    return (uint32_t)__builtin_ctz(mask);
}

// lazy: env.txt is applied after static initialization on the Switch
static bool trace_msg() { static const bool on = getenv("WWHD_TRACE_MSG") != nullptr; return on; }
static std::mutex g_threads_mutex;
static std::unordered_map<uint32_t, HostThread*> g_threads;  // by guest OSThread*
static thread_local HostThread* t_self = nullptr;
static thread_local Cpu* t_cpu = nullptr;
static uint32_t g_sda_base, g_sda2_base;
static uint16_t g_next_id = 1;

namespace threads {
Cpu* current() { return t_cpu; }
uint32_t current_thread() { return t_self ? t_self->guest : 0; }
}  // namespace threads

// ---------------------------------------------------------------- per-core scheduling
// Each emulated core runs one guest thread at a time; a ready thread with a higher priority than
// the running one sets g_core_preempt, and the running thread yields at its next function entry.
// WWHD_NO_SCHED=1 lets all threads run freely (the old behaviour).
volatile int g_core_preempt[3];
static const bool g_sched_on = getenv("WWHD_NO_SCHED") == nullptr;

struct CoreSched {
    std::mutex m;
    std::condition_variable cv;
    HostThread* owner = nullptr;
    std::deque<HostThread*> ready;  // FIFO among equal priorities
};
static CoreSched g_sched[3];

static HostThread* best_ready(CoreSched& k) {  // k.m held
    HostThread* b = nullptr;
    for (HostThread* t : k.ready)  // priority, then FIFO
        if (!b || t->prio < b->prio) b = t;
    return b;
}

static void sched_tick_thread();
static void mem_watch_thread();
#ifdef __SWITCH__
void switch_pin_guest_core(uint32_t core);  // platform/switch/switch_host.cpp
extern "C" void switch_set_helper_thread(int core, int priority);
void switch_hold_core(bool holding);
#endif

static void core_acquire(HostThread* t) {
    if (!g_sched_on || t->holds_core) return;
    static std::once_flag tick;
    static std::atomic<bool> tickStarted{false};
    if (!tickStarted.load(std::memory_order_acquire)) {
        std::call_once(tick, [] {
            std::thread(sched_tick_thread).detach();
            if (getenv("WWHD_WATCH_MEM")) std::thread(mem_watch_thread).detach();
        });
        tickStarted.store(true, std::memory_order_release);
    }
    uint32_t core = t->core;
    CoreSched& k = g_sched[core];
    std::unique_lock<std::mutex> lk(k.m);
    // Avoid deque push/erase (and periodic allocation) when the core is free.
    const bool uncontended = !k.owner && k.ready.empty();
    if (!uncontended) k.ready.push_back(t);
    if (k.owner && t->prio < k.owner->prio) g_core_preempt[core] = 1;
    auto start = std::chrono::steady_clock::now();
    t->acquired = start;
    t->ready_since = start;
    bool warned = false;
    while (!uncontended && (k.owner || best_ready(k) != t)) {
        k.cv.wait_for(lk, std::chrono::seconds(2));
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
            warned = true;
            std::string a = mem::read_cstr(ld32(t->guest + osthread::kName));
            std::string b = k.owner ? mem::read_cstr(ld32(k.owner->guest + osthread::kName)) : "-";
            LOG("[sched] \"%s\" (prio %d) waiting >10s for core %u held by \"%s\" (prio %d, pc %08X)", a.c_str(), t->prio, core,
                b.c_str(), k.owner ? k.owner->prio : 0, k.owner ? k.owner->cpu.lr : 0);
        }
    }
    for (auto it = k.ready.begin(); it != k.ready.end(); ++it)
        if (*it == t) { k.ready.erase(it); break; }
    k.owner = t;
    t->holds_core = true;
    t->held_core = core;
#ifdef __SWITCH__
    switch_pin_guest_core(core);  // guest core N runs on Switch CPU core N
    switch_hold_core(true);       // the thread holding a guest core outranks the others on its CPU
#endif
    auto now = std::chrono::steady_clock::now();
    t->wait_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    t->acquired = now;
    t->cpu.core = core;
    HostThread* next = best_ready(k);
    g_core_preempt[core] = next && next->prio < t->prio;
}

static void core_release(HostThread* t) {
    if (!g_sched_on || !t->holds_core) return;
    CoreSched& k = g_sched[t->held_core];
    bool wake;
    {
        std::lock_guard<std::mutex> lk(k.m);
        if (k.owner == t) k.owner = nullptr;
        t->holds_core = false;
        t->held_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t->acquired).count();
        g_core_preempt[t->held_core] = 0;
        wake = !k.ready.empty();
    }
#ifdef __SWITCH__
    switch_hold_core(false);
#endif
    // An acquirer arriving after the unlock sees the free core directly.
    if (wake) k.cv.notify_all();
}

// Like Cafe OS (and Cemu): strict priority, and equal-priority threads round-robin every time slice
// (games busy-wait on other threads of the same core and priority). Lower priorities never preempt.
static constexpr auto kSlice = std::chrono::microseconds(500);

static bool should_yield(CoreSched& k, HostThread* t) {  // k.m held
    HostThread* next = best_ready(k);
    if (!next) return false;
    if (next->prio < t->prio) return true;
    return next->prio == t->prio && std::chrono::steady_clock::now() - t->acquired >= kSlice;
}

// save states: threads that do not reach an HLE wait (pollers) park at a guest function entry.
// 0 off, 1 any thread may (saving), 2 only threads at their saved place (loading)
static std::atomic<int> g_entry_parks{0};
static void try_entry_park(HostThread* t, Cpu* c);
namespace threads { static std::string thread_name(HostThread* t); }

extern "C" void ppc_preempt(Cpu* c) {
    HostThread* t = (HostThread*)c->thread;
    if (__builtin_expect(g_entry_parks.load(std::memory_order_relaxed) != 0, 0) && t) try_entry_park(t, c);
    if (!t || !t->holds_core || t->irq_off) return;
    {
        CoreSched& k = g_sched[t->held_core];
        std::lock_guard<std::mutex> lk(k.m);
        g_core_preempt[t->held_core] = 0;
        if (!should_yield(k, t)) return;
    }
    core_release(t);
    core_acquire(t);
}

// scheduler tick: requests time-slice / starvation preemption on cores that need it
namespace threads { void report_sched(); }
static void report_queue_waits();

// debug: WWHD_WATCH_MEM=addr|*ptr+off[,...] polls guest words and logs every change together with
// where each guest thread is (lr), to find who writes a flag
static void mem_watch_thread() {
    host::set_thread_name("mem watch");
    struct W { bool deref; uint32_t a, off; uint32_t last; bool init = false; };
    std::vector<W> ws;
    const char* e = getenv("WWHD_WATCH_MEM");
    while (e && *e) {
        W w{};
        if (*e == '*') { w.deref = true; e++; }
        char* p;
        w.a = (uint32_t)strtoul(e, &p, 16);
        if (*p == '+') w.off = (uint32_t)strtoul(p + 1, &p, 16);
        ws.push_back(w);
        e = *p == ',' ? p + 1 : p;
        if (*p != ',') break;
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        for (auto& w : ws) {
            uint32_t addr = w.deref ? ld32(w.a) + w.off : w.a + w.off;
            if (!addr || (w.deref && ld32(w.a) == 0)) continue;
            uint32_t v = ld32(addr);
            if (w.init && v == w.last) continue;
            std::string who;
            {
                std::lock_guard<std::mutex> lk(g_threads_mutex);
                for (auto& [g, t] : g_threads) {
                    char b[96];
                    snprintf(b, sizeof b, " %s@%08X", mem::read_cstr(ld32(t->guest + osthread::kName)).c_str(), t->cpu.lr);
                    who += b;
                }
            }
            LOG("[memwatch] t=%.2fs %08X: %08X -> %08X |%s", timebase::now() / (double)timebase::kTicksPerSec, addr, w.last, v,
                who.c_str());
            w.last = v;
            w.init = true;
        }
    }
}
static void sched_tick_thread() {
    host::set_thread_name("sched tick");
#ifdef __SWITCH__
    switch_set_helper_thread(0, 0x2C);
#endif
    static const bool timed_stats = getenv("WWHD_SCHED_STATS") && atoi(getenv("WWHD_SCHED_STATS")) == 2;
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        if (timed_stats && std::chrono::steady_clock::now() >= next_report) {
            next_report += std::chrono::seconds(5);
            threads::report_sched();
        }
        for (int core = 0; core < 3; core++) {
            CoreSched& k = g_sched[core];
            std::lock_guard<std::mutex> lk(k.m);
            if (k.owner && !k.ready.empty() && should_yield(k, k.owner)) g_core_preempt[core] = 1;
        }
    }
}

// debug: WWHD_LOG_LONGWAIT=1 reports blocking calls that took longer than 2 s (who waited, from where)
static const bool g_log_longwait = getenv("WWHD_LOG_LONGWAIT") != nullptr;
static thread_local std::chrono::steady_clock::time_point t_block_start;

namespace threads {
// share of wall time each thread held / waited for its core since the last report
void report_sched() {
    report_queue_waits();
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double span = std::chrono::duration<double, std::nano>(now - last).count();
    last = now;
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    std::string out;
    for (auto& [g, t] : g_threads) {
        uint64_t h = t->held_ns.exchange(0), w = t->wait_ns.exchange(0);
        if (h < span * 0.02 && w < span * 0.02) continue;
        std::string name = mem::read_cstr(ld32(t->guest + osthread::kName));
        // actual CPU time the host gave this thread (vs. time it held its emulated core)
        double cpu = 0;
#ifdef __APPLE__
        if (t->mach) {
            thread_basic_info_data_t info;
            mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mach, THREAD_BASIC_INFO, (thread_info_t)&info, &cnt) == KERN_SUCCESS) {
                uint64_t us = (uint64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds +
                              (uint64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
                cpu = 100.0 * (us - t->last_cpu_us) * 1000.0 / span;
                t->last_cpu_us = us;
            }
        }
 #endif
        char buf[192];
        snprintf(buf, sizeof buf, " [%s c%u p%d run %.0f%% cpu %.0f%% wait %.0f%%]", name.empty() ? "main" : name.c_str(), t->core,
                 t->prio, 100.0 * h / span, cpu, 100.0 * w / span);
        out += buf;
    }
    LOG("[sched]%s", out.c_str());
}
void block_begin() {
    if (frame_trace::on() && frame_trace::is_game_thread()) frame_trace::event('B');
    if (g_log_longwait) t_block_start = std::chrono::steady_clock::now();
    if (t_self) core_release(t_self);
}
void block_end() {
    if (g_log_longwait && t_self) {
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_block_start).count();
        if (sec > 2.0)
            LOG("[longwait] \"%s\" blocked %.1f s (lr %08X)", mem::read_cstr(ld32(t_self->guest + osthread::kName)).c_str(), sec,
                t_self->cpu.lr);
    }
    if (t_self) core_acquire(t_self);
    if (frame_trace::on() && frame_trace::is_game_thread()) frame_trace::event('b');
}
bool ensure_core() {
    if (!t_self || t_self->holds_core || !g_sched_on) return false;
    core_acquire(t_self);
    return true;
}
void release_core() { if (t_self) core_release(t_self); }
void set_service_core(uint32_t core) { if (t_self) t_self->core = t_self->cpu.core = core; }
}  // namespace threads

static HostThread* host_thread(uint32_t t) {
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    auto it = g_threads.find(t);
    return it == g_threads.end() ? nullptr : it->second;
}

struct GuestExit {
    uint32_t value;
};

// ---------------------------------------------------------------- save-state freeze
// A save state is taken (or restored) while every guest thread is parked: blocked in an HLE wait
// whose outcome is decided only by HLE objects (re-checked after waking), or idle (service threads).
// A parked thread that wakes while frozen stops at the gate before it consumes anything, so the HLE
// objects and guest memory fully describe where it will continue.
static std::mutex g_frz_m;
static std::condition_variable g_frz_cv;
static bool g_frozen = false;
static HostThread* g_frz_owner = nullptr;

static void park_gate(HostThread* t) {
    std::unique_lock<std::mutex> l(g_frz_m);
    while (g_frozen && t != g_frz_owner) g_frz_cv.wait(l);
    t->wst.store(kRunning);
}

// wait on a host condition with the core given up; `lk` is held on entry and exit. The predicate is
// re-checked after every wakeup (also after a save state was loaded). False on timeout.
template <class Pred>
static bool park_wait(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, Pred pred, uint8_t kind, uint32_t obj,
                      const std::chrono::steady_clock::time_point* deadline = nullptr) {
    while (!pred()) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline) return false;
        HostThread* t = t_self; // TLS is needed only when this call actually parks
        if (t) {
            t->wait_kind = kind;
            t->wait_obj = obj;
            t->wst.store(kParked);
        }
        lk.unlock();
        threads::block_begin();
        lk.lock();
        if (deadline) cv.wait_until(lk, *deadline, pred);
        else cv.wait(lk, pred);
        lk.unlock();
        if (t) park_gate(t);
        threads::block_end();
        lk.lock();
    }
    return true;
}

// Return addresses on a host stack: 1 = thread/dispatch glue, >1 = recompiled guest function
// (its address), 0 = any other host code.
static std::mutex g_ra_m;
static std::unordered_map<uintptr_t, uint32_t> g_ra_cache;
static uint32_t classify_ra(uintptr_t ra) {
    std::lock_guard<std::mutex> lk(g_ra_m);
    auto it = g_ra_cache.find(ra);
    if (it != g_ra_cache.end()) return it->second;
    uint32_t v = 0;
 #if !defined(_WIN32) && !defined(__SWITCH__)
    Dl_info di;
    if (dladdr((void*)ra, &di) && di.dli_sname) {
        const char* n = di.dli_sname;
        if (n[0] == '_') n++;
        if (n[0] == 'f' && n[1] == '_' && isxdigit((unsigned char)n[2])) {
            v = (uint32_t)strtoul(n + 2, nullptr, 16);
            if (v < 2) v = 0;
        } else if (!strcmp(n, "ppc_dispatch") || !strcmp(n, "ppc_preempt") || strstr(n, "guest_call") || strstr(n, "thread_main") ||
                   strstr(n, "try_entry_park") || strstr(n, "thread_start")) {
            v = 1;
        }
    }
 #endif
    g_ra_cache[ra] = v;
    return v;
}

// Park at the entry of the guest function being entered, if only recompiled guest code is on the
// host stack (no hook or HLE function with host state of its own): the thread's whole state is then
// its Cpu, guest memory and the call chain.
__attribute__((noinline)) static void try_entry_park(HostThread* t, Cpu* c) {
    int mode = g_entry_parks.load();
    if (!mode || t->service || t->irq_off || t->wst.load() != kRunning) return;
    {
        std::lock_guard<std::mutex> l(g_frz_m);
        if (!g_frozen || t == g_frz_owner) return;
    }
    if (mode == 2 && (!t->has_target || c->r[1] != t->tgt_r1 || c->lr != t->tgt_lr)) return;
#if defined(_WIN32) || defined(__SWITCH__)
    // Entry parking requires host-frame symbol classification; Windows stack unwinding needs
    // a dedicated implementation. Ordinary waits remain saveable; never guess a safe frame.
    return;
    uintptr_t hi=0,lo=0;
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(self), lo = hi - pthread_get_stacksize_np(self);
#else
    pthread_attr_t attr; pthread_getattr_np(pthread_self(),&attr);
    void* stack; size_t size; pthread_attr_getstack(&attr,&stack,&size);pthread_attr_destroy(&attr);
    uintptr_t lo=(uintptr_t)stack, hi=lo+size;
#endif
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uint32_t fn = 0;
    for (int depth = 0; fp >= lo && fp < hi; depth++) {
        uintptr_t ra = ((uintptr_t*)fp)[1];
        if (!ra || depth > 8192) break;
        uint32_t k = classify_ra(ra);
        if (k == 0) {
            static const bool dbg = getenv("WWHD_STATE_DEBUG") != nullptr;
            static std::atomic<int> logged{0};
            if (dbg && logged++ < 20) {
#if defined(_WIN32) || defined(__SWITCH__)
                const char* n = "?";
#else
                Dl_info di;
                const char* n = dladdr((void*)ra, &di) && di.dli_sname ? di.dli_sname : "?";
#endif
                LOG("[savestate] %s: no entry park, host frame %s (depth %d)", threads::thread_name(t).c_str(), n, depth);
            }
            return;
        }
        if (k > 1 && !fn) fn = k;  // innermost guest function: the one being entered
        uintptr_t next = ((uintptr_t*)fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    if (!fn || (mode == 2 && fn != t->tgt_fn)) return;
    t->wait_kind = W_ENTRY;
    t->wait_obj = fn;
    bool held = t->holds_core;
    if (held) core_release(t);
    t->wst.store(kParked);
    park_gate(t);
    if (held) core_acquire(t);
}

namespace threads {
void park_sleep_until(std::chrono::steady_clock::time_point tp, bool precise,
                      void (*before_resume)()) {
    HostThread* t = t_self;
    if (t) {
        t->wait_kind = W_SLEEP;
        t->wait_obj = 0;
        t->wst.store(kParked);
    }
    block_begin();
    if (precise) {
        // macOS sleep timers can resume about 1 ms after the requested vsync, so the last part is
        // spun with the guest core released. The spin window follows the measured lateness: the
        // largest of the last 120 wakes plus a margin, 0.5..2 ms; a wake past the deadline goes
        // straight back to 2 ms. (A fixed 2 ms window spun ~1.5 ms per vsync, 15% of Vulkan's CPU.)
        // WWHD_VSYNC_SPIN_US=n fixes the window at n microseconds.
        using us = std::chrono::microseconds;
        static const long fixedUs = getenv("WWHD_VSYNC_SPIN_US") ? atol(getenv("WWHD_VSYNC_SPIN_US")) : -1;
        static thread_local us window{fixedUs >= 0 ? fixedUs : 2000}, peak{0};
        static thread_local int wakes = 0;
        const auto sleepDeadline = tp - window;
        if (std::chrono::steady_clock::now() < sleepDeadline) {
            host::sleep_until(sleepDeadline);
            const auto woke = std::chrono::steady_clock::now();
            peak = std::max(peak, std::chrono::duration_cast<us>(woke - sleepDeadline));
            if (fixedUs < 0 && (woke >= tp || ++wakes == 120)) {
                window = woke >= tp ? us{2000} : std::clamp(peak + us{250}, us{500}, us{2000});
                peak = us{0};
                wakes = 0;
            }
        }
        while (std::chrono::steady_clock::now() < tp) {}
    } else {
        host::sleep_until(tp);
    }
    if (t) park_gate(t);
    // The freeze gate marks the thread busy before host-only completion work.
    // Keep its guest core released until that work finishes; never call guest code.
    if (before_resume) before_resume();
    block_end();
}
void service_begin() {
    if (!t_self) return;
    park_gate(t_self);  // waits while frozen; marks the thread busy
}
void service_end() {
    if (!t_self) return;
    t_self->wait_kind = W_SERVICE;
    t_self->wst.store(kParked);
}
}  // namespace threads

static void* thread_main(void* p) {
    HostThread* ht = (HostThread*)p;
    t_self = ht;
    t_cpu = &ht->cpu;
    std::string name = mem::read_cstr(ld32(ht->guest + osthread::kName));
    host::set_thread_name(name.empty() ? "guest" : name.c_str());
    {
        // the game's main thread (the first, unnamed one) builds every frame's GX2 commands
        static std::atomic<bool> hinted{false};
        if (name.empty() && !hinted.exchange(true)) perf_hint::add_current_thread();
    }
#ifdef __SWITCH__
    ht->throttle = name == "OliveOperationMgrThread";
#endif
#ifdef __APPLE__
    ht->mach = pthread_mach_thread_np(pthread_self());
#endif
    // keep guest threads on performance cores (see host::boost_thread_priority): the default QoS
    // let macOS park them on efficiency cores, which showed up as the main thread holding its core
    // without getting CPU time
    host::boost_thread_priority();
    LOG("[thread] start \"%s\" core %d prio %d affinity %X", name.c_str(), ht->core, (int)ld32(ht->guest + osthread::kBasePrio),
        ld32(ht->guest + osthread::kAffinity));
    uint32_t rv = 0;
    core_acquire(ht);
    try {
        rv = guest_call(&ht->cpu, ht->entry, {ht->argc, ht->argv});
    } catch (const GuestExit& e) {
        rv = e.value;
    }
    core_release(ht);
    {
        std::lock_guard<std::mutex> lk(ht->m);
        ht->exited = true;
        ht->exit_value = rv;
        st32(ht->guest + osthread::kExitValue, rv);
        st8(ht->guest + osthread::kState, osthread::MORIBUND);
    }
    ht->cv.notify_all();
    TRACE("[thread] %s exited with %08X", name.c_str(), rv);
    return nullptr;
}

#ifdef _WIN32
static DWORD WINAPI windows_thread_main(void* p) { thread_main(p); return 0; }
#endif
static void start_host_thread(HostThread* ht) {
#ifdef _WIN32
    ht->pt=CreateThread(nullptr,64<<20,windows_thread_main,ht,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!ht->pt)fatal("cannot create guest thread (error=%lu)",GetLastError());
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef __SWITCH__
    pthread_attr_setstacksize(&attr, 8 << 20);   // Horizon commits thread stacks: 64 MiB each is too much
#else
    pthread_attr_setstacksize(&attr, 64 << 20);  // deep guest call chains recurse on the host stack
#endif
    pthread_create(&ht->pt, &attr, thread_main, ht);
    pthread_attr_destroy(&attr);
#endif
}

static void init_cpu(Cpu& c, uint32_t stack_top) {
    PpcCallCache* icache = c.icache;
    memset(&c, 0, sizeof(c));
    c.icache = icache ? icache : ppc_icache_alloc();
    c.r[1] = (stack_top - 0x20) & ~0xFu;
    c.r[2] = g_sda2_base;
    c.r[13] = g_sda_base;
    // coreinit defaults for new threads: GQR2-5 quantize as u8, u16, s8, s16; FPSCR.NI set
    c.gqr[2] = 0x40004;
    c.gqr[3] = 0x50005;
    c.gqr[4] = 0x60006;
    c.gqr[5] = 0x70007;
    c.fpscr = 4;
}

static HostThread* create_thread(uint32_t t, uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_top,
                                 uint32_t stack_size, int prio, uint32_t attr) {
    auto* ht = new HostThread();
    ht->guest = t;
    ht->entry = entry;
    ht->argc = argc;
    ht->argv = argv;
    memset(mem::ptr(t), 0, osthread::kSize);
    st32(t + 0, 0x4F53436F);  // "OSContxt" tag
    st32(t + 4, 0x6E747874);
    st8(t + osthread::kState, osthread::READY);
    st8(t + osthread::kAttr, attr);
    st16(t + osthread::kId, g_next_id++);
    st32(t + osthread::kSuspend, 1);
    st32(t + osthread::kEffPrio, prio);
    st32(t + osthread::kBasePrio, prio);
    st32(t + osthread::kStackBase, stack_top);
    st32(t + osthread::kStackEnd, stack_top - stack_size);
    st32(t + osthread::kEntry, entry);
    st32(t + osthread::kAffinity, attr & 7);
    init_cpu(ht->cpu, stack_top);
    ht->cpu.thread = ht;
    ht->core = core_from_affinity(attr, t_self ? t_self->core : 1);
    ht->cpu.core = ht->core;
    ht->prio = prio;
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    g_threads[t] = ht;
    return ht;
}

static void thread_dump_loop(int secs);
namespace threads {
void init(const LoadedModule& m) {
    g_sda_base = m.sda_base;
    g_sda2_base = m.sda2_base;
    // debug: WWHD_THREAD_DUMP=s logs every guest thread's state (running/parked, wait object, lr) every s seconds
    if (const char* e = getenv("WWHD_THREAD_DUMP")) {
        int s = atoi(e);
        std::thread([s] { thread_dump_loop(s); }).detach();
    }
}

void run_main(const LoadedModule& m, int argc, uint32_t argv) {
    uint32_t stack_size = std::max<uint32_t>(m.stack_size, 0x100000);
    uint32_t stack = mem::runtime_alloc(stack_size, 0x100);
    uint32_t t = mem::runtime_alloc(osthread::kSize, 8);
    HostThread* ht = create_thread(t, m.entry, argc, argv, stack + stack_size, stack_size, 16, 0x2 /* core 1 */);
    {
        uint32_t nm = mem::runtime_alloc(16);
        mem::write_cstr(nm, "{ Main thread }", 16);
#ifdef __SWITCH__
        st32(t + osthread::kName, nm);  // host thread label (profiler); the game renames it later
#endif
    }
    ht->suspend = 0;
    ht->started = true;
    start_host_thread(ht);
#ifdef _WIN32
    WaitForSingleObject(ht->pt,INFINITE); CloseHandle(ht->pt); ht->pt=nullptr;
#else
    pthread_join(ht->pt, nullptr);
#endif
}

Cpu* make_service_cpu(const char* name, uint32_t stack_size) {
    // host-only memory: service threads start at different times in different sessions
    uint32_t stack = mem::host_alloc(stack_size, 0x100);
    uint32_t t = mem::host_alloc(osthread::kSize, 8);
    HostThread* ht = create_thread(t, 0, 0, 0, stack + stack_size, stack_size, 0, 0x7);
    ht->prio = -1;  // interrupt-like: preempts guest threads
    ht->service = true;
    uint32_t nm = mem::host_alloc((uint32_t)strlen(name) + 1);
    mem::write_cstr(nm, name, (uint32_t)strlen(name) + 1);
    st32(t + osthread::kName, nm);
    t_self = ht;
    t_cpu = &ht->cpu;
    ht->started = true;
    ht->wait_kind = W_SERVICE;
    ht->wst.store(kParked);  // idle until service_begin
    host::boost_thread_priority();
    return &ht->cpu;
}
}  // namespace threads

// ---------------------------------------------------------------- thread API
HLE(coreinit, OSCreateThread) {
    uint32_t t = arg(c, 0), entry = arg(c, 1), argc = arg(c, 2), argv = arg(c, 3), stack = arg(c, 4),
             stack_size = arg(c, 5), prio = arg(c, 6), attr = arg(c, 7);
    TRACE("[thread] OSCreateThread(%08X, entry=%08X, stack=%08X+%X, prio=%d, attr=%X)", t, entry, stack, stack_size,
          prio, attr);
    create_thread(t, entry, argc, argv, stack, stack_size, (int)prio, attr);
    ret(c, 1);
}

HLE(coreinit, OSResumeThread) {
    HostThread* ht = host_thread(arg(c, 0));
    if (!ht) { ret(c, 0); return; }
    int prev;
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(ht->m);
        prev = ht->suspend;
        if (ht->suspend > 0 && --ht->suspend == 0 && !ht->started) {
            ht->started = true;
            start = true;
        }
        st32(ht->guest + osthread::kSuspend, ht->suspend);
    }
    if (start) start_host_thread(ht);
    ht->cv.notify_all();
    ret(c, prev);
}

HLE(coreinit, OSExitThread) {
    throw GuestExit{arg(c, 0)};
}

HLE(coreinit, OSJoinThread) {
    HostThread* ht = host_thread(arg(c, 0));
    if (!ht) { ret(c, 0); return; }
    {
        std::unique_lock<std::mutex> lk(ht->m);
        park_wait(lk, ht->cv, [&] { return ht->exited; }, W_JOIN, arg(c, 0));
    }
    if (arg(c, 1)) st32(arg(c, 1), ht->exit_value);
    ret(c, 1);
}

HLE(coreinit, OSGetCurrentThread) { auto* t = (HostThread*)c->thread; ret(c, t ? t->guest : 0); }
HLE(coreinit, OSGetCoreId) { auto* t = (HostThread*)c->thread; ret(c, t ? t->core : 1); }
HLE(coreinit, OSYieldThread) {
    // let other ready threads of the same (or higher) priority on this core run
    BlockingScope b;
    std::this_thread::yield();
}
HLE(coreinit, OSSleepTicks) {
    threads::park_sleep_until(std::chrono::steady_clock::now() + ticks_to_ns(arg64(c, 3)));
}
HLE(coreinit, OSSetThreadName) { st32(arg(c, 0) + osthread::kName, arg(c, 1)); }
HLE(coreinit, OSSetThreadAffinity) {
    st32(arg(c, 0) + osthread::kAffinity, arg(c, 1));
    if (HostThread* ht = host_thread(arg(c, 0))) ht->core = core_from_affinity(arg(c, 1), ht->core);  // applies at next schedule
    ret(c, 1);
}
HLE(coreinit, OSGetThreadPriority) { ret(c, ld32(arg(c, 0) + osthread::kBasePrio)); }
HLE(coreinit, OSSetThreadSpecific) { st32(threads::current_thread() + osthread::kSpecific + 4 * arg(c, 0), arg(c, 1)); }
HLE(coreinit, OSGetThreadSpecific) { ret(c, ld32(threads::current_thread() + osthread::kSpecific + 4 * arg(c, 0))); }
HLE(coreinit, OSBlockThreadsOnExit) {}
HLE(coreinit, OSSetExceptionCallback) { ret(c, 0); }
// interrupts off = no rescheduling on this core
HLE(coreinit, OSDisableInterrupts) {
    auto* t = (HostThread*)c->thread;
    int prev = t ? (t->irq_off ? 0 : 1) : 1;
    if (t) t->irq_off = 1;
    ret(c, prev);
}
HLE(coreinit, OSRestoreInterrupts) {
    auto* t = (HostThread*)c->thread;
    int prev = t ? (t->irq_off ? 0 : 1) : 1;
    if (t && arg(c, 0)) {
        t->irq_off = 0;
        if (t->holds_core && g_core_preempt[t->held_core]) ppc_preempt(c);
    }
    ret(c, prev);
}
HLE(coreinit, OSMemoryBarrier) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }


// ---------------------------------------------------------------- host objects
template <typename T>
struct ObjTable {
    std::mutex m;
    std::unordered_map<uint32_t, T*> map;  // objects are never deleted (waiters may hold them)
    // Lock-free lookups (every OSLockMutex / OSSendMessage / OSWaitEvent... goes through get): a
    // table-wide lock held by a thread that is then preempted blocked unrelated threads. Open
    // addressing, slots published once (value before key) and never cleared; reset updates the value.
    struct Slot { std::atomic<uint32_t> key{0}; std::atomic<T*> value{nullptr}; };
    static constexpr size_t kSlots = 8192, kProbes = 32;
    std::unique_ptr<Slot[]> slots{new Slot[kSlots]};
    static size_t slot_of(uint32_t addr) { return (addr * 0x9E3779B1u) >> 19; }  // 13 bits
    T* find(uint32_t addr) const {
        for (size_t i = 0, s = slot_of(addr); i < kProbes; ++i, s = (s + 1) & (kSlots - 1)) {
            const uint32_t k = slots[s].key.load(std::memory_order_acquire);
            if (k == addr) return slots[s].value.load(std::memory_order_acquire);
            if (!k) return nullptr;
        }
        return nullptr;
    }
    void publish(uint32_t addr, T* p) {  // under m
        for (size_t i = 0, s = slot_of(addr); i < kProbes; ++i, s = (s + 1) & (kSlots - 1)) {
            const uint32_t k = slots[s].key.load(std::memory_order_relaxed);
            if (k == addr) { slots[s].value.store(p, std::memory_order_release); return; }
            if (!k) {
                slots[s].value.store(p, std::memory_order_relaxed);
                slots[s].key.store(addr, std::memory_order_release);
                return;
            }
        }
        // probe run full: this address stays on the locked path
    }
    T* get(uint32_t addr) {
        if (addr)
            if (T* p = find(addr)) return p;
        std::lock_guard<std::mutex> lk(m);
        T*& p = map[addr];
        if (!p) p = new T();
        if (addr) publish(addr, p);
        return p;
    }
    void reset(uint32_t addr) {
        std::lock_guard<std::mutex> lk(m);
        T*& p = map[addr];
        delete p;
        p = new T();
        if (addr) publish(addr, p);
    }
};

// OSMutex: recursive, owned by a thread
struct HMutex {
    std::mutex m;
    std::condition_variable cv;
    const void* owner = nullptr;
    int count = 0;
};
static ObjTable<HMutex> g_mutexes;

static void mutex_lock(Cpu* c, uint32_t addr) {
    HMutex* mx = g_mutexes.get(addr);
    const void* self = c->thread ? c->thread : (const void*)&t_cpu;
    std::unique_lock<std::mutex> lk(mx->m);
    if (mx->owner == self) { mx->count++; return; }
    park_wait(lk, mx->cv, [&] { return mx->owner == nullptr; }, W_MUTEX, addr);
    mx->owner = self;
    mx->count = 1;
}
static bool mutex_trylock(Cpu* c, uint32_t addr) {
    HMutex* mx = g_mutexes.get(addr);
    const void* self = c->thread ? c->thread : (const void*)&t_cpu;
    std::lock_guard<std::mutex> lk(mx->m);
    if (mx->owner == self) { mx->count++; return true; }
    if (mx->owner) return false;
    mx->owner = self;
    mx->count = 1;
    return true;
}
static void mutex_unlock(uint32_t addr) {
    HMutex* mx = g_mutexes.get(addr);
    {
        std::lock_guard<std::mutex> lk(mx->m);
        if (--mx->count > 0) return;
        mx->owner = nullptr;
        mx->count = 0;
    }
    mx->cv.notify_one();
}

// Init functions reinitialize the host object in place: replacing it would strand threads already
// waiting on the old one (a waiter can get there first, or an object is re-initialized while in use)
HLE(coreinit, OSInitMutex) {
    HMutex* mx = g_mutexes.get(arg(c, 0));
    {
        std::lock_guard<std::mutex> lk(mx->m);
        mx->owner = nullptr;
        mx->count = 0;
    }
    mx->cv.notify_all();
    st32(arg(c, 0), 0x6D557458);  // "mUtX"
}
HLE(coreinit, OSLockMutex) { mutex_lock(c, arg(c, 0)); }
HLE(coreinit, OSTryLockMutex) { ret(c, mutex_trylock(c, arg(c, 0))); }
HLE(coreinit, OSUnlockMutex) { mutex_unlock(arg(c, 0)); }

// GHS C library locks
static uint32_t g_ghs_lock = 0xC0FFEE00;
HLE(coreinit, __ghsLock) { mutex_lock(c, g_ghs_lock); }
HLE(coreinit, __ghsUnlock) { mutex_unlock(g_ghs_lock); }
HLE(coreinit, __ghs_mtx_init) { /* arg: void** handle */ st32(arg(c, 0), mem::runtime_alloc(8)); }
HLE(coreinit, __ghs_mtx_dst) {}
HLE(coreinit, __ghs_mtx_lock) { mutex_lock(c, ld32(arg(c, 0))); }
HLE(coreinit, __ghs_mtx_unlock) { mutex_unlock(ld32(arg(c, 0))); }
HLE(coreinit, __ghs_flock_file) { mutex_lock(c, 0xC0FFEE10); }
HLE(coreinit, __ghs_funlock_file) { mutex_unlock(0xC0FFEE10); }
HLE(coreinit, __ghs_flock_ptr) { ret(c, mem::runtime_alloc(4)); }
HLE(coreinit, __ghs_flock_destroy) {}

// OSEvent. Like Cafe OS, a signal wakes the threads waiting at that moment (a manual-reset event:
// all of them; an auto-reset event: one, without becoming signaled); they return even if the event
// is reset before they run (the game pulses events: signal, then reset at once).
struct HEvent {
    std::mutex m;
    std::condition_variable cv;
    bool signaled = false;
    bool auto_reset = false;
    std::deque<HostThread*> waiters;
};
static ObjTable<HEvent> g_events;

static bool event_wait(Cpu* c, uint32_t addr, const std::chrono::steady_clock::time_point* deadline) {
    HEvent* ev = g_events.get(addr);
    HostThread* t = (HostThread*)c->thread;
    std::unique_lock<std::mutex> lk(ev->m);
    if (ev->signaled) {
        if (ev->auto_reset) ev->signaled = false;
        return true;
    }
    if (!t) {  // not a guest thread
        bool ok = park_wait(lk, ev->cv, [&] { return ev->signaled; }, W_EVENT, addr, deadline);
        if (ok && ev->auto_reset) ev->signaled = false;
        return ok;
    }
    t->woken = false;
    ev->waiters.push_back(t);
    if (park_wait(lk, ev->cv, [&] { return t->woken; }, W_EVENT, addr, deadline)) return true;
    for (auto it = ev->waiters.begin(); it != ev->waiters.end(); ++it)  // timed out
        if (*it == t) { ev->waiters.erase(it); break; }
    return false;
}

HLE(coreinit, OSInitEvent) {
    uint32_t e = arg(c, 0);
    if (trace_msg()) LOG("[evt] init %08X signaled=%u auto=%u lr=%08X", e, arg(c, 1), arg(c, 2), c->lr);
    HEvent* ev = g_events.get(e);
    {
        std::lock_guard<std::mutex> lk(ev->m);
        ev->signaled = arg(c, 1) != 0;
        ev->auto_reset = arg(c, 2) != 0;  // OS_EVENT_MODE_AUTO = 1
    }
    if (ev->signaled) ev->cv.notify_all();
    st32(e, 0x65566E54);              // "eVnT"
}
HLE(coreinit, OSSignalEvent) {
    if (trace_msg()) LOG("[evt] signal %08X lr=%08X thread=%08X", arg(c, 0), c->lr, threads::current_thread());
    HEvent* ev = g_events.get(arg(c, 0));
    {
        std::lock_guard<std::mutex> lk(ev->m);
        if (ev->auto_reset && !ev->waiters.empty()) {
            ev->waiters.front()->woken = true;
            ev->waiters.pop_front();
        } else {
            ev->signaled = true;
            for (HostThread* w : ev->waiters) w->woken = true;
            ev->waiters.clear();
        }
    }
    ev->cv.notify_all();
}
HLE(coreinit, OSResetEvent) {
    if (trace_msg()) LOG("[evt] reset %08X lr=%08X", arg(c, 0), c->lr);
    HEvent* ev = g_events.get(arg(c, 0));
    std::lock_guard<std::mutex> lk(ev->m);
    ev->signaled = false;
}
HLE(coreinit, OSWaitEvent) {
    if (trace_msg()) LOG("[evt] wait %08X lr=%08X thread=%08X", arg(c, 0), c->lr, threads::current_thread());
    event_wait(c, arg(c, 0), nullptr);
}
HLE(coreinit, OSWaitEventWithTimeout) {
    auto deadline = std::chrono::steady_clock::now() + ticks_to_ns(arg64(c, 5));
    ret(c, event_wait(c, arg(c, 0), &deadline));
}

// OSMessageQueue: messages are 16 bytes
// The messages are guarded by a short spin lock, never held across a sleep; m/cv only put threads to
// sleep. With m guarding the data, a woken receiver (on a busy core) owned m before it ran - the
// kernel hands the mutex over at wake-up - and the next sender blocked until it got the CPU
// (the game's main thread lost ~3-4 ms a frame on update_ubo's queue). Sleepers register before
// re-checking (see sleep_pred), so a wake can't be missed; wake() touches m only if someone sleeps.
// Save states run with every thread parked and access msgs under m.
struct HQueue {
    std::atomic<uint64_t> send_wait_ns{0};   // WWHD_QUEUE_STATS: senders blocked on the data lock
    std::atomic<uint32_t> receiver{0};       // last receiving guest thread
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> sleepers{0};
    void lock_data() {
        for (unsigned i = 0; busy.test_and_set(std::memory_order_acquire); ++i)
            if (i > 64) std::this_thread::yield();
    }
    void unlock_data() { busy.clear(std::memory_order_release); }
    template <class F> bool check(F&& f) { lock_data(); const bool ok = f(); unlock_data(); return ok; }
    // park_wait predicate: count this thread as a sleeper first, then test
    template <class F> auto sleep_pred(F f) {
        return [this, f] { sleepers.fetch_add(1, std::memory_order_seq_cst); return check(f); };
    }
    void wake() {
        if (sleepers.exchange(0, std::memory_order_seq_cst)) {
            { std::lock_guard<std::mutex> lk(m); }
            cv.notify_all();
        }
    }
    std::mutex m;
    std::condition_variable cv;
    MessageRing<std::array<uint32_t, 4>> msgs;
    uint32_t capacity = 0;
};
static ObjTable<HQueue> g_queues;

HLE(coreinit, OSInitMessageQueue) {
    uint32_t q = arg(c, 0);
    if (trace_msg()) LOG("[msg] init q=%08X count=%u", q, arg(c, 2));
    HQueue* hq = g_queues.get(q);
    {
        std::lock_guard<std::mutex> lk(hq->m);
        hq->lock_data();
        hq->msgs.reset(arg(c, 2));
        hq->capacity = arg(c, 2);
        hq->unlock_data();
    }
    hq->cv.notify_all();
    st32(q, 0x6D536751);  // "mSgQ"
}
static bool queue_stats() { static const bool on = getenv("WWHD_QUEUE_STATS") != nullptr; return on; }
HLE(coreinit, OSSendMessage) {
    HQueue* q = g_queues.get(arg(c, 0));
    uint32_t m = arg(c, 1), flags = arg(c, 2);
    std::array<uint32_t, 4> msg{ld32(m), ld32(m + 4), ld32(m + 8), ld32(m + 12)};
    if (trace_msg()) LOG("[msg] send q=%08X msg=%08X %08X flags=%X lr=%08X", arg(c, 0), msg[0], msg[1], flags, c->lr);
    for (;;) {
        const auto t0 = queue_stats() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        q->lock_data();
        if (queue_stats()) q->send_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        if (q->msgs.size() < q->capacity) {
            if (flags & 2) q->msgs.push_front(msg); else q->msgs.push_back(msg);  // OS_MESSAGE_FLAG_HIGH_PRIORITY
            q->unlock_data();
            break;
        }
        q->unlock_data();
        if (!(flags & 1)) { ret(c, 0); return; }
        std::unique_lock<std::mutex> lk(q->m);
        park_wait(lk, q->cv, q->sleep_pred([q] { return q->msgs.size() < q->capacity; }), W_MSG_SEND, arg(c, 0));
    }
    q->wake();
    ret(c, 1);
}
static void report_queue_waits() {
    if (!queue_stats()) return;
    std::string out;
    std::lock_guard<std::mutex> lk(g_queues.m);
    for (auto& [addr, q] : g_queues.map) {
        if (!q) continue;
        const uint64_t ns = q->send_wait_ns.exchange(0);
        if (ns < 200000) continue;
        std::string name = "?";
        if (HostThread* t = host_thread(q->receiver.load())) name = threads::thread_name(t);
        char b[160]; snprintf(b, sizeof b, " [q %08X -> %s: senders blocked %.2f ms]", addr, name.c_str(), ns / 1e6);
        out += b;
    }
    if (!out.empty()) LOG("[queues]%s", out.c_str());
}
HLE(coreinit, OSReceiveMessage) {
    // OliveOperationMgrThread re-posts its own state machine step until Miiverse answers (never):
    // ~100k steps a second. The USA build hooks the step function (hooks.txt 0203DEEC); by thread
    // name this works for every region. A 1 ms nap per message, with the guest core released.
    auto* self = (HostThread*)c->thread;
    if (self && self->throttle) threads::park_sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
    HQueue* q = g_queues.get(arg(c, 0));
    uint32_t m = arg(c, 1), flags = arg(c, 2);
    std::array<uint32_t, 4> msg;
    if (trace_msg()) LOG("[msg] recv q=%08X flags=%X lr=%08X thread=%08X", arg(c, 0), flags, c->lr, threads::current_thread());
    for (;;) {
        q->lock_data();
        if (!q->msgs.empty()) {
            msg = q->msgs.front();
            q->msgs.pop_front();
            q->unlock_data();
            break;
        }
        q->unlock_data();
        if (!(flags & 1)) { ret(c, 0); return; }
        std::unique_lock<std::mutex> lk(q->m);
        park_wait(lk, q->cv, q->sleep_pred([q] { return !q->msgs.empty(); }), W_MSG_RECV, arg(c, 0));
    }
    if (queue_stats()) q->receiver = threads::current_thread();
    q->wake();
    for (int i = 0; i < 4; i++) st32(m + 4 * i, msg[i]);
    ret(c, 1);
}

// OSThreadQueue / OSSleepThread: sleepers wait on a condition keyed by the queue; a wakeup sets the
// token of every thread sleeping at that moment
struct HSleep {
    std::mutex m;
    std::condition_variable cv;
    std::vector<HostThread*> waiters;
    uint64_t gen = 0;  // for sleepers that are not guest threads
};
static ObjTable<HSleep> g_sleepq;
HLE(coreinit, OSInitThreadQueue) { g_sleepq.get(arg(c, 0)); }  // sleepers (if any) keep waiting for a wakeup
HLE(coreinit, OSSleepThread) {
    HSleep* q = g_sleepq.get(arg(c, 0));
    std::unique_lock<std::mutex> lk(q->m);
    HostThread* t = t_self;
    if (!t) {
        uint64_t g = q->gen;
        park_wait(lk, q->cv, [&] { return q->gen != g; }, W_SLEEPQ, arg(c, 0));
        return;
    }
    t->woken = false;
    q->waiters.push_back(t);
    park_wait(lk, q->cv, [&] { return t->woken; }, W_SLEEPQ, arg(c, 0));
}
void os_wakeup_thread_queue(uint32_t queue) {
    HSleep* q = g_sleepq.get(queue);
    {
        std::lock_guard<std::mutex> lk(q->m);
        q->gen++;
        for (HostThread* t : q->waiters) t->woken = true;
        q->waiters.clear();
    }
    q->cv.notify_all();
}

// OSRendezvous
struct HRendezvous {
    std::mutex m;
    std::condition_variable cv;
    uint32_t arrived = 0;
};
static ObjTable<HRendezvous> g_rdv;
HLE(coreinit, OSInitRendezvous) {
    HRendezvous* r = g_rdv.get(arg(c, 0));
    std::lock_guard<std::mutex> lk(r->m);
    r->arrived = 0;
}
HLE(coreinit, OSWaitRendezvous) {
    // arg1 is a core mask; we treat each set bit as one participant
    HRendezvous* r = g_rdv.get(arg(c, 0));
    uint32_t need = __builtin_popcount(arg(c, 1) & 7);
    std::unique_lock<std::mutex> lk(r->m);
    r->arrived++;
    r->cv.notify_all();
    park_wait(lk, r->cv, [&] { return r->arrived >= need; }, W_RDV, arg(c, 0));
    ret(c, 1);
}

// ---------------------------------------------------------------- alarms
// One host thread fires all alarms and runs their guest callbacks.
struct Alarm {
    uint32_t core;
    uint32_t guest;
    uint64_t when;
    uint64_t period;
    uint32_t callback;
    uint64_t serial;
};
static std::mutex g_alarm_mutex;
static std::condition_variable g_alarm_cv;
static std::multimap<uint64_t, Alarm> g_alarm_queue;
static std::unordered_map<uint32_t, uint64_t> g_alarm_serial;  // alarm -> active serial (0 = cancelled)
static uint64_t g_next_serial = 1;
static bool g_alarm_thread_started = false;

static void alarm_thread() {
    Cpu* c = threads::make_service_cpu("alarm", 0x20000);
    host::set_thread_name("alarm");
    std::unique_lock<std::mutex> lk(g_alarm_mutex);
    for (;;) {
        if (g_alarm_queue.empty()) { g_alarm_cv.wait(lk); continue; }
        auto it = g_alarm_queue.begin();
        uint64_t now = timebase::now();
        if (it->first > now) {
            g_alarm_cv.wait_for(lk, ticks_to_ns(it->first - now));
            continue;
        }
        // busy from here: the alarm queue changes and the callback runs (a save state waits for it)
        lk.unlock();
        threads::service_begin();
        lk.lock();
        it = g_alarm_queue.begin();
        if (it == g_alarm_queue.end() || it->first > timebase::now()) {  // changed meanwhile (save state loaded)
            threads::service_end();
            continue;
        }
        Alarm a = it->second;
        g_alarm_queue.erase(it);
        if (g_alarm_serial[a.guest] != a.serial) { threads::service_end(); continue; }  // cancelled or re-armed
        if (a.period) {
            a.when += a.period;
            if (a.when < now) a.when = now + a.period;
            g_alarm_queue.emplace(a.when, a);
        } else {
            g_alarm_serial[a.guest] = 0;
        }
        lk.unlock();
        threads::set_service_core(a.core);
        bool took = threads::ensure_core();
        guest_call(c, a.callback, {a.guest, 0});
        if (took) threads::release_core();
        threads::service_end();
        lk.lock();
    }
}

static void arm_alarm(uint32_t alarm, uint64_t when, uint64_t period, uint32_t cb) {
    std::lock_guard<std::mutex> lk(g_alarm_mutex);
    if (!g_alarm_thread_started) {
        g_alarm_thread_started = true;
        std::thread(alarm_thread).detach();
    }
    uint64_t s = g_next_serial++;
    g_alarm_serial[alarm] = s;
    g_alarm_queue.emplace(when, Alarm{t_self ? t_self->core : 1u, alarm, when, period, cb, s});
    g_alarm_cv.notify_all();
}

// OSAlarm layout (Cemu): +0x04 name, +0x0C callback, +0x10 tag, +0x18 nextFire, +0x28 period, +0x30 tick, +0x38 userData
HLE(coreinit, OSCreateAlarm) {
    memset(mem::ptr(arg(c, 0)), 0, 0x58);
    st32(arg(c, 0), 0x614C724D);  // "aLrM"
}
HLE(coreinit, OSSetAlarm) {
    uint32_t alarm = arg(c, 0), cb = c->r[7];
    uint64_t delay = arg64(c, 5);
    st32(alarm + 0x0C, cb);
    arm_alarm(alarm, timebase::now() + delay, 0, cb);
    ret(c, 1);
}
HLE(coreinit, OSSetPeriodicAlarm) {
    uint32_t alarm = arg(c, 0), cb = c->r[9];
    uint64_t start = timebase::to_host(arg64(c, 5)), period = arg64(c, 7);
    st32(alarm + 0x0C, cb);
    uint64_t now = timebase::now();
    uint64_t when = start;
    if (when < now && period) when += ((now - start) / period + 1) * period;
    arm_alarm(alarm, when, period, cb);
    ret(c, 1);
}
HLE(coreinit, OSCancelAlarm) {
    std::lock_guard<std::mutex> lk(g_alarm_mutex);
    g_alarm_serial[arg(c, 0)] = 0;
    ret(c, 1);
}
HLE(coreinit, OSSetAlarmUserData) { st32(arg(c, 0) + 0x38, arg(c, 1)); }
HLE(coreinit, OSGetAlarmUserData) { ret(c, ld32(arg(c, 0) + 0x38)); }

// ---------------------------------------------------------------- time API
HLE(coreinit, OSGetTime) { ret64(c, timebase::guest_now()); }
HLE(coreinit, OSGetSystemTime) { ret64(c, timebase::guest_now()); }
HLE(coreinit, OSGetTick) { ret(c, (uint32_t)timebase::guest_now()); }

HLE(coreinit, OSTicksToCalendarTime) {
    // OSCalendarTime: sec, min, hour, mday, mon, year, wday, yday, msec, usec (int32 each)
    uint64_t ticks = arg64(c, 3);
    uint32_t out = arg(c, 2);
    // guest epoch is 2000-01-01; report the host's wall clock instead of time since boot
    (void)ticks;
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv,&t);
#else
    localtime_r(&t, &tmv);
#endif
    uint32_t v[10] = {(uint32_t)tmv.tm_sec, (uint32_t)tmv.tm_min, (uint32_t)tmv.tm_hour, (uint32_t)tmv.tm_mday,
                      (uint32_t)tmv.tm_mon, (uint32_t)(tmv.tm_year + 1900), (uint32_t)tmv.tm_wday,
                      (uint32_t)tmv.tm_yday, 0, 0};
    for (int i = 0; i < 10; i++) st32(out + 4 * i, v[i]);
}

// ---------------------------------------------------------------- save states

namespace threads {
static std::string thread_name(HostThread* t) {
    uint32_t n = ld32(t->guest + osthread::kName);
    std::string s = n >= 0x10000 ? mem::read_cstr(n) : "";
    char buf[32];
    snprintf(buf, sizeof buf, "%08X", t->guest);
    return (s.empty() ? std::string("?") : s) + "@" + buf;
}

bool quiesce(int timeout_ms, std::string& busy, int entry_mode, int entry_after_ms) {
    HostThread* me = t_self;
    {
        std::lock_guard<std::mutex> l(g_frz_m);
        g_frozen = true;
        g_frz_owner = me;
    }
    if (me) me->wst.store(kRequester);
    block_begin();  // the other threads on our core can run to their waits
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (entry_mode && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(entry_after_ms)) {
            g_entry_parks = entry_mode;
            for (int k = 0; k < 3; k++) g_core_preempt[k] = 1;  // running threads look at their next function entry
        }
        busy.clear();
        {
            std::lock_guard<std::mutex> lk(g_threads_mutex);
            for (auto& [g, t] : g_threads) {
                if (t == me || !t->started || t->exited) continue;
                if (t->wst.load() != kParked) {
                    char buf[64];
                    snprintf(buf, sizeof buf, "(lr %08X)", t->cpu.lr);
                    busy += " " + thread_name(t) + buf;
                }
            }
        }
        if (busy.empty()) { g_entry_parks = 0; return true; }
        if (std::chrono::steady_clock::now() > end) { g_entry_parks = 0; return false; }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void thaw() {
    g_entry_parks = 0;
    {
        std::lock_guard<std::mutex> l(g_frz_m);
        g_frozen = false;
        g_frz_owner = nullptr;
    }
    g_frz_cv.notify_all();
    HostThread* me = t_self;
    block_end();
    if (me) me->wst.store(kRunning);
}
}  // namespace threads

namespace {
struct ThreadRec {
    uint32_t guest;
    uint8_t started, exited, wst, wait_kind, woken, pad[3];
    int32_t suspend, prio, irq_off;
    uint32_t core, exit_value, entry, argc, argv, wait_obj;
    Cpu cpu;
};

// guest call chain of a parked thread: stack pointer, return address and the back chain of saved
// return addresses must be identical, so its host call stack is the same as the saved one
bool same_chain(const Cpu& cur, const Cpu& saved) {
    if (cur.r[1] != saved.r[1] || cur.lr != saved.lr) return false;
    uint32_t a = cur.r[1];
    for (int i = 0; i < 512; i++) {
        uint32_t na = ld32(a), nb = ss::snap_ld32(a);
        if (na != nb) return false;
        if (!na || na <= a || na - a > 0x100000) break;
        if (ld32(na + 4) != ss::snap_ld32(na + 4)) return false;
        a = na;
    }
    return true;
}

uint32_t owner_guest(const void* o, bool& ok) {
    if (!o) return 0;
    for (auto& [g, t] : g_threads)
        if (t == o) return g;
    ok = false;  // held by a host thread that is not a guest thread
    return 0;
}
HostThread* owner_host(uint32_t g) {
    if (!g) return nullptr;
    auto it = g_threads.find(g);
    return it == g_threads.end() ? nullptr : it->second;
}

template <class T>
std::vector<std::pair<uint32_t, T*>> sorted(ObjTable<T>& t) {
    std::vector<std::pair<uint32_t, T*>> v;
    std::lock_guard<std::mutex> lk(t.m);
    for (auto& [a, p] : t.map)
        if (p) v.push_back({a, p});
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.first < y.first; });
    return v;
}
}  // namespace

// all threads are parked (or this is a check without side effects)
bool threads_ss_save(ss::Writer& w, std::string& why) {
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    std::vector<ThreadRec> recs;
    for (auto& [g, t] : g_threads) {
        if (t->service) continue;
        ThreadRec r{};
        r.guest = g;
        r.started = t->started;
        r.exited = t->exited;
        r.wst = (uint8_t)t->wst.load();
        r.wait_kind = t->wait_kind;
        r.woken = t->woken;
        r.suspend = t->suspend;
        r.prio = t->prio;
        r.irq_off = t->irq_off;
        r.core = t->core;
        r.exit_value = t->exit_value;
        r.entry = t->entry;
        r.argc = t->argc;
        r.argv = t->argv;
        r.wait_obj = t->wait_obj;
        r.cpu = t->cpu;
        r.cpu.thread = nullptr;
        r.cpu.icache = nullptr;
        if (r.started && !r.exited && r.irq_off) {
            why = "thread " + threads::thread_name(t) + " has interrupts disabled";
            return false;
        }
        recs.push_back(r);
    }
    std::sort(recs.begin(), recs.end(), [](auto& a, auto& b) { return a.guest < b.guest; });
    w.u32((uint32_t)recs.size());
    for (auto& r : recs) w.pod(r);
    w.u32(g_next_id);

    bool ok = true;
    auto mx = sorted<HMutex>(g_mutexes);
    w.u32((uint32_t)mx.size());
    for (auto& [a, m] : mx) {
        std::lock_guard<std::mutex> l(m->m);
        w.u32(a);
        w.u32(owner_guest(m->owner, ok));
        w.u32((uint32_t)m->count);
        if (!ok) { why = "a guest mutex is held by a host thread"; return false; }
    }
    auto ev = sorted<HEvent>(g_events);
    w.u32((uint32_t)ev.size());
    for (auto& [a, e] : ev) {
        std::lock_guard<std::mutex> l(e->m);
        w.u32(a);
        w.u8(e->signaled);
        w.u8(e->auto_reset);
        w.u32((uint32_t)e->waiters.size());
        for (HostThread* t : e->waiters) w.u32(owner_guest(t, ok));
    }
    auto qs = sorted<HQueue>(g_queues);
    w.u32((uint32_t)qs.size());
    for (auto& [a, q] : qs) {
        std::lock_guard<std::mutex> l(q->m);
        w.u32(a);
        w.u32(q->capacity);
        w.u32((uint32_t)q->msgs.size());
        for (uint32_t i = 0; i < q->msgs.size(); ++i) w.pod(q->msgs[i]);
    }
    auto sq = sorted<HSleep>(g_sleepq);
    w.u32((uint32_t)sq.size());
    for (auto& [a, q] : sq) {
        std::lock_guard<std::mutex> l(q->m);
        w.u32(a);
        w.u32((uint32_t)q->waiters.size());
        for (HostThread* t : q->waiters) w.u32(owner_guest(t, ok));
    }
    auto rv = sorted<HRendezvous>(g_rdv);
    w.u32((uint32_t)rv.size());
    for (auto& [a, r] : rv) {
        std::lock_guard<std::mutex> l(r->m);
        w.u32(a);
        w.u32(r->arrived);
    }
    {
        std::lock_guard<std::mutex> l(g_alarm_mutex);
        w.u32((uint32_t)g_alarm_queue.size());
        for (auto& [when, a] : g_alarm_queue) {
            w.u32(a.core);
            w.u32(a.guest);
            w.u64(timebase::to_guest(a.when));
            w.u64(a.period);
            w.u32(a.callback);
            w.u64(a.serial);
        }
        w.u32((uint32_t)g_alarm_serial.size());
        for (auto& [g, s] : g_alarm_serial) {
            w.u32(g);
            w.u64(s);
        }
        w.u64(g_next_serial);
    }
    w.u64(timebase::guest_now());
    return true;
}

// can the threads of this snapshot be put back into the current process? (all threads parked)
bool threads_ss_check(ss::Reader r, std::string& why) {
    uint32_t n = r.u32();
    std::vector<ThreadRec> recs(n);
    for (auto& t : recs) r.bytes(&t, sizeof t);
    if (!r.ok) { why = "corrupt thread section"; return false; }
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    std::unordered_map<uint32_t, const ThreadRec*> by;
    for (auto& t : recs) by[t.guest] = &t;
    char buf[200];
    for (auto& s : recs) {
        auto it = g_threads.find(s.guest);
        HostThread* t = it == g_threads.end() ? nullptr : it->second;
        if (t && t->service) { snprintf(buf, sizeof buf, "thread %08X is a service thread here", s.guest); why = buf; return false; }
        bool live_s = s.started && !s.exited;
        if (!t) {
            if (!live_s && !s.exited) continue;  // created but not started: recreated on load
            snprintf(buf, sizeof buf, "thread %08X (running in the save) does not exist yet", s.guest);
            why = buf;
            return false;
        }
        bool live_t = t->started && !t->exited;
        if (s.started != t->started || (live_s != live_t)) {
            snprintf(buf, sizeof buf, "thread %s: %s in the save, %s now", threads::thread_name(t).c_str(),
                     !s.started ? "not started" : s.exited ? "exited" : "running", !t->started ? "not started" : t->exited ? "exited" : "running");
            why = buf;
            return false;
        }
        if (!live_s) continue;
        int wst = t->wst.load();
        if (s.wst != wst) {
            snprintf(buf, sizeof buf, "thread %s: %s in the save, %s now", threads::thread_name(t).c_str(),
                     s.wst == kRequester ? "main loop" : "waiting", wst == kRequester ? "main loop" : "waiting");
            why = buf;
            return false;
        }
        if (s.wst == kParked && (s.wait_kind != t->wait_kind || s.wait_obj != t->wait_obj)) {
            snprintf(buf, sizeof buf, "thread %s waits on %u:%08X in the save, %u:%08X now", threads::thread_name(t).c_str(),
                     s.wait_kind, s.wait_obj, t->wait_kind, t->wait_obj);
            why = buf;
            return false;
        }
        if (!same_chain(t->cpu, s.cpu)) {
            snprintf(buf, sizeof buf, "thread %s is at a different place (lr %08X sp %08X, saved lr %08X sp %08X)",
                     threads::thread_name(t).c_str(), t->cpu.lr, t->cpu.r[1], s.cpu.lr, s.cpu.r[1]);
            why = buf;
            return false;
        }
    }
    for (auto& [g, t] : g_threads) {
        if (t->service || !t->started || t->exited || by.count(g)) continue;
        why = "thread " + threads::thread_name(t) + " was created after the save and is running";
        return false;
    }
    return true;
}

// put the threads and HLE objects back (all threads parked, guest memory already restored)
void threads_ss_load(ss::Reader& r) {
    uint32_t n = r.u32();
    std::vector<ThreadRec> recs(n);
    for (auto& t : recs) r.bytes(&t, sizeof t);
    uint16_t next_id = (uint16_t)r.u32();
    {
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        g_next_id = std::max(g_next_id, next_id);
        for (auto& s : recs) {
            auto it = g_threads.find(s.guest);
            HostThread* t;
            if (it == g_threads.end()) {  // created but not started in the save
                t = new HostThread();
                t->guest = s.guest;
                g_threads[s.guest] = t;
            } else {
                t = it->second;
            }
            if (!t->started) {
                t->entry = s.entry;
                t->argc = s.argc;
                t->argv = s.argv;
            }
            std::lock_guard<std::mutex> l(t->m);
            void* self = t->cpu.thread;
            PpcCallCache* icache = t->cpu.icache;  // this process's array, not the saved pointer
            t->cpu = s.cpu;
            t->cpu.thread = t;
            t->cpu.icache = icache ? icache : ppc_icache_alloc();
            t->cpu.core = t->holds_core ? t->held_core : s.core;
            t->suspend = s.suspend;
            t->prio = s.prio;
            t->core = s.core;
            t->exit_value = s.exit_value;
            t->woken = s.woken;
            (void)self;
        }
    }
    uint32_t cnt = r.u32();
    std::unordered_map<uint32_t, std::pair<uint32_t, int>> mxs;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = r.u32(), o = r.u32(), c = r.u32();
        mxs[a] = {o, (int)c};
    }
    for (auto& [a, m] : sorted<HMutex>(g_mutexes)) {
        std::lock_guard<std::mutex> l(m->m);
        m->owner = nullptr;
        m->count = 0;
    }
    for (auto& [a, v] : mxs) {
        HMutex* m = g_mutexes.get(a);
        std::lock_guard<std::mutex> l(m->m);
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        m->owner = owner_host(v.first);
        m->count = m->owner ? v.second : 0;
    }
    cnt = r.u32();
    for (auto& [a, e] : sorted<HEvent>(g_events)) {
        std::lock_guard<std::mutex> l(e->m);
        e->signaled = false;
        e->waiters.clear();
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = r.u32();
        uint8_t sig = r.u8(), ar = r.u8();
        HEvent* e = g_events.get(a);
        std::lock_guard<std::mutex> l(e->m);
        e->signaled = sig;
        e->auto_reset = ar;
        uint32_t nw = r.u32();
        for (uint32_t k = 0; k < nw && r.ok; k++) {
            uint32_t g = r.u32();
            std::lock_guard<std::mutex> lk(g_threads_mutex);
            if (HostThread* t = owner_host(g)) e->waiters.push_back(t);
        }
    }
    cnt = r.u32();
    for (auto& [a, q] : sorted<HQueue>(g_queues)) {
        std::lock_guard<std::mutex> l(q->m);
        q->msgs.clear();
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = r.u32(), cap = r.u32(), nm = r.u32();
        HQueue* q = g_queues.get(a);
        std::lock_guard<std::mutex> l(q->m);
        if (nm > cap) { r.ok = false; return; }
        q->msgs.reset(cap);
        q->capacity = cap;
        for (uint32_t k = 0; k < nm && r.ok; k++) q->msgs.push_back(r.pod<std::array<uint32_t, 4>>());
    }
    cnt = r.u32();
    for (auto& [a, q] : sorted<HSleep>(g_sleepq)) {
        std::lock_guard<std::mutex> l(q->m);
        q->waiters.clear();
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = r.u32(), nw = r.u32();
        HSleep* q = g_sleepq.get(a);
        std::lock_guard<std::mutex> l(q->m);
        for (uint32_t k = 0; k < nw && r.ok; k++) {
            uint32_t g = r.u32();
            std::lock_guard<std::mutex> lk(g_threads_mutex);
            if (HostThread* t = owner_host(g)) q->waiters.push_back(t);
        }
    }
    cnt = r.u32();
    for (auto& [a, x] : sorted<HRendezvous>(g_rdv)) {
        std::lock_guard<std::mutex> l(x->m);
        x->arrived = 0;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = r.u32(), arrived = r.u32();
        HRendezvous* x = g_rdv.get(a);
        std::lock_guard<std::mutex> l(x->m);
        x->arrived = arrived;
    }
    // alarms; the guest clock continues from the saved time
    uint64_t saved_now;
    {
        std::lock_guard<std::mutex> l(g_alarm_mutex);
        std::vector<Alarm> alarms(r.u32());
        for (auto& a : alarms) {
            a.core = r.u32();
            a.guest = r.u32();
            a.when = r.u64();  // guest time (converted below)
            a.period = r.u64();
            a.callback = r.u32();
            a.serial = r.u64();
        }
        g_alarm_serial.clear();
        uint32_t ns = r.u32();
        for (uint32_t i = 0; i < ns && r.ok; i++) {
            uint32_t g = r.u32();
            g_alarm_serial[g] = r.u64();
        }
        g_next_serial = std::max(g_next_serial, r.u64());
        saved_now = r.u64();
        timebase::set_guest_now(saved_now);
        g_alarm_queue.clear();
        for (auto& a : alarms) {
            a.when = timebase::to_host(a.when);
            g_alarm_queue.emplace(a.when, a);
        }
        if (!g_alarm_queue.empty() && !g_alarm_thread_started) {
            g_alarm_thread_started = true;
            std::thread(alarm_thread).detach();
        }
    }
    g_alarm_cv.notify_all();
    // parked threads re-check their conditions against the restored objects
    auto wake = [](auto& table) {
        std::lock_guard<std::mutex> lk(table.m);
        for (auto& [a, o] : table.map)
            if (o) {
                { std::lock_guard<std::mutex> l(o->m); }
                o->cv.notify_all();
            }
    };
    wake(g_mutexes);
    wake(g_events);
    wake(g_queues);
    wake(g_sleepq);
    wake(g_rdv);
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    for (auto& [g, t] : g_threads) {
        { std::lock_guard<std::mutex> l(t->m); }
        t->cv.notify_all();
    }
}

static void thread_dump_loop(int secs) {
    host::set_thread_name("thread dump");
    static const char* kinds[] = {"-", "mutex", "event", "msg-send", "msg-recv", "sleepq", "join", "rdv", "sleep", "service", "entry"};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        std::string out;
        for (auto& [g, t] : g_threads) {
            if (!t->started || t->exited) continue;
            char buf[200];
            int w = t->wst.load();
            snprintf(buf, sizeof buf, "\n   %-32s %s %s %08X lr %08X sp %08X core %u%s", threads::thread_name(t).c_str(),
                     w == kRunning ? "RUN " : w == kParked ? "park" : "REQ ", t->wait_kind < 11 ? kinds[t->wait_kind] : "?", t->wait_obj,
                     t->cpu.lr, t->cpu.r[1], t->core, t->holds_core ? " (holds core)" : "");
            out += buf;
        }
        LOG("[threads]%s", out.c_str());
    }
}

// loading: threads saved at a function entry park when they get there again
void threads_ss_targets(ss::Reader r) {
    uint32_t n = r.u32();
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    for (auto& [g, t] : g_threads) t->has_target = false;
    for (uint32_t i = 0; i < n && r.ok; i++) {
        ThreadRec s = r.pod<ThreadRec>();
        if (s.wst != kParked || s.wait_kind != W_ENTRY) continue;
        auto it = g_threads.find(s.guest);
        if (it == g_threads.end()) continue;
        HostThread* t = it->second;
        t->tgt_fn = s.wait_obj;
        t->tgt_r1 = s.cpu.r[1];
        t->tgt_lr = s.cpu.lr;
        t->has_target = true;
    }
}
