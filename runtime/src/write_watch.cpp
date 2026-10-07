// Exact CPU-write tracking for guest memory (see write_watch.h for the protocol and its race argument).
#include "write_watch.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "platform/host.h"

#ifdef _WIN32
#include <windows.h>
#elif !defined(__SWITCH__)
#include <signal.h>
#include <sys/mman.h>
#endif

namespace wwatch {
namespace {

// per page: kProt = read-only by us right now; kEver = armed at least once, so a write fault on the page
// is ours (guest memory is otherwise all read-write; the null-page guard is never armed)
constexpr uint8_t kProt = 1, kEver = 2;

uint8_t* g_base = nullptr;
uint64_t g_size = 0;
unsigned g_shift = 0;
uint64_t g_pages = 0;
std::atomic<uint64_t>* g_stamp = nullptr;  // newest write stamp per page
std::atomic<uint8_t>* g_flags = nullptr;
std::atomic<uint64_t> g_clock{1};
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_faults{0}, g_protected{0};
// arm() vs host writes: a page inside a pending kernel write must stay writable until the write ends
std::mutex g_m;
std::vector<std::pair<uint64_t, uint64_t>> g_pins;  // page ranges [first, last] of pending host writes

bool set_rw(uint64_t first, uint64_t count, bool writable) {
    void* p = g_base + (first << g_shift);
    size_t n = (size_t)(count << g_shift);
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(p, n, writable ? PAGE_READWRITE : PAGE_READONLY, &old) != 0;
#elif defined(__SWITCH__)
    (void)p; (void)n; (void)writable;
    return false;
#else
    return mprotect(p, n, writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0;
#endif
}

void stamp_page(uint64_t page, uint64_t v) {
    // monotonic: two threads faulting on one page may store out of order
    uint64_t cur = g_stamp[page].load(std::memory_order_relaxed);
    while (cur < v && !g_stamp[page].compare_exchange_weak(cur, v, std::memory_order_release, std::memory_order_relaxed)) {}
}

// a write fault at host address a: ours if the page was ever armed. Unprotect first, then stamp (the
// order the race argument in write_watch.h relies on). Async-signal-safe: atomics and one syscall.
bool on_write_fault(uintptr_t a) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    uintptr_t off = a - (uintptr_t)g_base;
    if (a < (uintptr_t)g_base || off >= g_size) return false;
    uint64_t page = off >> g_shift;
    if (!(g_flags[page].load(std::memory_order_acquire) & kEver)) return false;
    if (!set_rw(page, 1, true)) return false;
    g_flags[page].fetch_and((uint8_t)~kProt, std::memory_order_acq_rel);
    stamp_page(page, g_clock.fetch_add(1, std::memory_order_acq_rel) + 1);
    g_faults.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#ifdef _WIN32
LONG CALLBACK veh(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    // ExceptionInformation[0] == 1: a write; [1]: the address
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
        on_write_fault((uintptr_t)r->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
bool install_handler() { return AddVectoredExceptionHandler(1, veh) != nullptr; }
#elif defined(__SWITCH__)
// Horizon gives homebrew no page protection with fault handling: the Switch renderer narrows texture
// checks with the pages the game flushes from its data cache instead (gx2::texturePageWrites)
bool install_handler() { return false; }
#else
struct sigaction g_old_segv, g_old_bus;
void handler(int sig, siginfo_t* si, void* uc) {
    // macOS reports a write to a read-only page as SIGBUS, Linux/Android as SIGSEGV
    if (on_write_fault((uintptr_t)si->si_addr)) return;
    // not ours: the crash handler (main.cpp) or whatever was installed before
    struct sigaction& o = sig == SIGBUS ? g_old_bus : g_old_segv;
    if (o.sa_flags & SA_SIGINFO) o.sa_sigaction(sig, si, uc);
    else if (o.sa_handler != SIG_DFL && o.sa_handler != SIG_IGN) o.sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}
bool install_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGSEGV, &sa, &g_old_segv) == 0 && sigaction(SIGBUS, &sa, &g_old_bus) == 0;
}
#endif

bool pinned(uint64_t page) {
    for (auto& [a, b] : g_pins)
        if (page >= a && page <= b) return true;
    return false;
}

bool page_range(uint32_t addr, uint32_t size, uint64_t& first, uint64_t& last) {
    if (!size || !g_active.load(std::memory_order_acquire)) return false;
    uint64_t end = std::min<uint64_t>((uint64_t)addr + size, g_size);
    if (addr >= end) return false;
    first = (uint64_t)addr >> g_shift;
    last = (end - 1) >> g_shift;
    return true;
}

}  // namespace

bool init(uint8_t* base, uint64_t size) {
    static std::once_flag once;
    std::call_once(once, [&] {
#ifdef __SWITCH__
        return;  // see install_handler
#endif
        size_t ps = host::page_size();
        if (!ps || (ps & (ps - 1))) return;
        unsigned shift = 0;
        while (((size_t)1 << shift) < ps) shift++;
        g_base = base;
        g_size = size;
        g_shift = shift;
        g_pages = size >> shift;
        // zero-filled on demand by the OS (calloc of this size maps fresh pages); 9 bytes per host page
        g_stamp = (std::atomic<uint64_t>*)calloc(g_pages, sizeof(std::atomic<uint64_t>));
        g_flags = (std::atomic<uint8_t>*)calloc(g_pages, sizeof(std::atomic<uint8_t>));
        if (!g_stamp || !g_flags || !install_handler()) return;
        g_active.store(true, std::memory_order_release);
    });
    return active();
}

bool active() { return g_active.load(std::memory_order_acquire); }

uint64_t arm(uint32_t addr, uint32_t size) {
    uint64_t t0 = g_clock.load(std::memory_order_acquire), first, last;
    if (!page_range(addr, size, first, last)) return t0;
    // fast path: every page is still protected (nothing written since some earlier arm)
    uint64_t p = first;
    while (p <= last && (g_flags[p].load(std::memory_order_acquire) & kProt)) p++;
    if (p > last) return t0;
    std::lock_guard<std::mutex> lk(g_m);
    uint64_t run = 0, runStart = 0;
    auto flush = [&] {
        if (!run) return;
        set_rw(runStart, run, false);
        g_protected.fetch_add(run, std::memory_order_relaxed);
        run = 0;
    };
    for (; p <= last; p++) {
        // flag before protecting: a protected-flagged page is never writable (see write_watch.h)
        if ((g_flags[p].load(std::memory_order_acquire) & kProt) || pinned(p)) { flush(); continue; }
        g_flags[p].fetch_or(kProt | kEver, std::memory_order_acq_rel);
        if (!run) runStart = p;
        run++;
    }
    flush();
    return t0;
}

bool written_since(uint32_t addr, uint32_t size, uint64_t stamp) {
    uint64_t first, last;
    if (!page_range(addr, size, first, last)) return false;
    for (uint64_t p = first; p <= last; p++)
        if (g_stamp[p].load(std::memory_order_acquire) > stamp) return true;
    return false;
}

void host_write_begin(uint32_t addr, uint32_t size) {
    uint64_t first, last;
    if (!page_range(addr, size, first, last)) return;
    std::lock_guard<std::mutex> lk(g_m);
    g_pins.push_back({first, last});
    for (uint64_t p = first; p <= last; p++)
        if (g_flags[p].load(std::memory_order_acquire) & kProt) {
            set_rw(p, 1, true);
            g_flags[p].fetch_and((uint8_t)~kProt, std::memory_order_acq_rel);
        }
    uint64_t v = g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (uint64_t p = first; p <= last; p++) stamp_page(p, v);
}

void host_write_end(uint32_t addr, uint32_t size) {
    uint64_t first, last;
    if (!page_range(addr, size, first, last)) return;
    std::lock_guard<std::mutex> lk(g_m);
    auto it = std::find(g_pins.begin(), g_pins.end(), std::make_pair(first, last));
    if (it != g_pins.end()) g_pins.erase(it);
    // stamped again after the data is in: a texture checked during the write is checked once more
    uint64_t v = g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (uint64_t p = first; p <= last; p++) stamp_page(p, v);
}

void take_stats(uint64_t& faults, uint64_t& protectedPages) {
    faults = g_faults.exchange(0, std::memory_order_relaxed);
    protectedPages = g_protected.exchange(0, std::memory_order_relaxed);
}

}  // namespace wwatch
