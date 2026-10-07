#include "frame_trace.h"
#include "runtime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace frame_trace {
namespace {
struct Event { uint64_t ns; char code; };
constexpr uint32_t kEvents = 1 << 14;
Event g_events[kEvents];
std::atomic<uint32_t> g_next{0};
std::atomic<bool> g_on{false};
uint64_t g_first = 0, g_last = 0, g_swap_ns = 0;
std::thread::id g_game;
bool g_slow_only = false;  // WWHD_FRAME_TRACE=slow: always on, print swaps over 36 ms (at most 2000)
uint32_t g_printed = 0;
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// read on the first swap: env.txt is applied after static initialisation on Switch
bool parse_env() {
    const char* e = getenv("WWHD_FRAME_TRACE");
    if (!e) return false;
    unsigned long long first = 0, count = 0;
    if (!strcmp(e, "slow")) { g_slow_only = true; g_first = 0; g_last = ~0ull; return true; }
    if (sscanf(e, "%llu,%llu", &first, &count) != 2 || !count) return false;
    g_first = first; g_last = first + count;
    return true;
}
}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }
void event(char code) {
    if (!on()) return;
    uint32_t i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i < kEvents) g_events[i] = {now_ns(), code};
}
void event_at(char code, uint64_t ns) {
    if (!on()) return;
    uint32_t i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i < kEvents) g_events[i] = {ns, code};
}
void set_game_thread() { g_game = std::this_thread::get_id(); }
bool is_game_thread() { return std::this_thread::get_id() == g_game; }
void swap(uint64_t count) {
    static const bool enabled = parse_env();
    if (!enabled) return;
    const uint64_t t = now_ns();
    if (on() && (!g_slow_only || (t - g_swap_ns > 36000000 && g_printed++ < 2000))) {
        // one line: events since the previous swap, as code+ms from it (lowercase = end of a wait)
        uint32_t n = std::min<uint32_t>(g_next.load(), kEvents);
        std::sort(g_events, g_events + n, [](const Event& a, const Event& b) { return a.ns < b.ns; });
        std::string line;
        char buf[32];
        for (uint32_t i = 0; i < n; i++) {
            snprintf(buf, sizeof buf, " %c%.1f", g_events[i].code, (g_events[i].ns - g_swap_ns) / 1e6);
            line += buf;
        }
        LOG("[ftrace] swap %llu %.1f ms:%s", (unsigned long long)count, (t - g_swap_ns) / 1e6, line.c_str());
    }
    g_next.store(0);
    g_swap_ns = now_ns();
    g_on.store(count >= g_first && count < g_last, std::memory_order_relaxed);
}
}  // namespace frame_trace
