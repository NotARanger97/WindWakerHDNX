// debug: WWHD_FRAME_TRACE=first,count logs a timeline of the game/render thread waits for `count`
// swaps starting at swap `first` (one line per swap: event codes with ms offsets from that swap).
#pragma once
#include <cstdint>

namespace frame_trace {
bool on();                    // tracing window active (cheap check)
void event(char code);        // record code at now on the calling thread
void event_at(char code, uint64_t steady_ns);  // record code at a steady_clock time
void swap(uint64_t count);    // game thread, GX2SwapScanBuffers: starts/ends the window, prints a line
void set_game_thread();       // the calling thread is the one that swaps (block events are kept for it)
bool is_game_thread();
}  // namespace frame_trace
