#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace gx2 {
// One bit per 4 KiB guest page (128 KiB total). Pages remain watched for the
// process lifetime: address reuse and overlapping programs cannot lose a watch.
// Only the epoch crosses from guest CPU threads to the render thread; shader
// maps and last-used memos remain render-thread-owned.
//
// Switch: a flush of a watched page does not bump the epoch by itself. It marks the page dirty; the
// render thread (revalidate_programs, before each command batch) compares the programs on dirty
// pages with copies of their bytes and bumps the epoch only if one really changed. The game flushes
// pages holding shader programs ~500 times a frame (programs share pages with per-frame data);
// bumping on every flush revalidated every program and shader lookup all the time.
class ShaderProgramWrites {
    std::array<std::atomic<uint64_t>, 16384> pages{}, dirty{};
    std::array<std::atomic<uint64_t>, 256> dirtyWords{};  // summary: one bit per word of dirty
    std::atomic<uint64_t> epoch{1};
    std::atomic<uint64_t> globalEpoch{1};  // bumped only when any watched byte may have changed unseen
    std::atomic<bool> anyDirty{false};
    void mark_dirty_word(uint64_t word, uint64_t bits) {
        // order: page bits, summary bit, flag (take_dirty clears them in the reverse order)
        dirty[word].fetch_or(bits, std::memory_order_release);
        dirtyWords[word / 64].fetch_or(uint64_t{1} << (word % 64), std::memory_order_release);
        anyDirty.store(true, std::memory_order_release);
    }
public:
    std::atomic<uint64_t> flushBumps{0}, invalidateBumps{0}, contentBumps{0};  // statistics
    uint64_t generation() const { return epoch.load(std::memory_order_acquire); }
    // Content checks of individual programs stay valid across epoch bumps from revalidate_programs
    // (it marks exactly the changed programs stale); a global invalidation voids them all.
    uint64_t global_generation() const { return globalEpoch.load(std::memory_order_acquire); }
    void invalidate() { epoch.fetch_add(1, std::memory_order_release); invalidateBumps.fetch_add(1, std::memory_order_relaxed); }
    void watch(uint32_t address, uint32_t size) {
        if (!size) return;
        const uint64_t end = uint64_t(address) + size;
        for (uint64_t page = address >> 12; page <= (end - 1) >> 12 && page < 1048576; ++page)
            pages[page / 64].fetch_or(uint64_t{1} << (page % 64), std::memory_order_release);
    }
    void notify(uint32_t address, uint64_t size) {
        if (!size) return;
        // A wrapped/whole-window flush is conservatively global. Avoid overflow
        // even for a host size_t range, and never truncate the 4 GiB endpoint.
        if (size > 0x100000000ull - address) {
            globalEpoch.fetch_add(1, std::memory_order_release);
            invalidate();
            return;
        }
        const uint64_t end = uint64_t(address) + size;
        const uint64_t first = address >> 12, last = (end - 1) >> 12;
        // Test up to 64 pages per load; large non-shader flushes stay cheap.
        for (uint64_t word = first / 64; word <= last / 64; ++word) {
            uint64_t mask = UINT64_MAX;
            if (word == first / 64) mask &= UINT64_MAX << (first % 64);
            if (word == last / 64) mask &= UINT64_MAX >> (63 - last % 64);
            if (const uint64_t hit = pages[word].load(std::memory_order_acquire) & mask) {
                mark_dirty_word(word, hit);
                flushBumps.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // Render thread: each dirty watched page since the last call, then cleared.
    template <class F> void take_dirty(F&& f) {
        if (!anyDirty.exchange(false, std::memory_order_acq_rel)) return;
        for (uint64_t summary = 0; summary < dirtyWords.size(); ++summary) {
            for (uint64_t words = dirtyWords[summary].exchange(0, std::memory_order_acq_rel); words; words &= words - 1) {
                const uint64_t word = summary * 64 + __builtin_ctzll(words);
                for (uint64_t bits = dirty[word].exchange(0, std::memory_order_acq_rel); bits; bits &= bits - 1)
                    f(uint32_t(word * 64 + __builtin_ctzll(bits)));
            }
        }
    }
};
inline ShaderProgramWrites shaderProgramWrites;

// CPU-side texture pages (Switch): the render thread watches the guest pages of every texture it
// uploaded from guest memory; DCFlushRange/DCStoreRange (and host writes such as FSReadFile) mark
// watched pages written. On the Wii U the GPU reads memory, not the CPU cache, so the game must
// flush texture data it wrote before the GPU samples it: an unflushed page cannot have new texel
// data the GPU is meant to see. The render thread consumes the written bits once per frame
// (take_written) and re-checks only the textures on those pages.
class TexturePageWrites {
    std::array<std::atomic<uint64_t>, 16384> watched{}, written{};
    std::atomic<bool> any{false};
public:
    void watch(uint32_t address, uint64_t size) {
        if (!size) return;
        const uint64_t end = std::min<uint64_t>(uint64_t(address) + size, 0x100000000ull);
        for (uint64_t page = address >> 12; page <= (end - 1) >> 12; ++page)
            watched[page / 64].fetch_or(uint64_t{1} << (page % 64), std::memory_order_relaxed);
    }
    void notify(uint32_t address, uint64_t size) {
        if (!size) return;
        const uint64_t end = std::min<uint64_t>(uint64_t(address) + size, 0x100000000ull);
        const uint64_t first = address >> 12, last = (end - 1) >> 12;
        for (uint64_t word = first / 64; word <= last / 64; ++word) {
            uint64_t mask = UINT64_MAX;
            if (word == first / 64) mask &= UINT64_MAX << (first % 64);
            if (word == last / 64) mask &= UINT64_MAX >> (63 - last % 64);
            if (const uint64_t hit = watched[word].load(std::memory_order_relaxed) & mask) {
                written[word].fetch_or(hit, std::memory_order_release);
                any.store(true, std::memory_order_release);
            }
        }
    }
    // Render thread: true if a page of [address, address+size) was written since its last take.
    bool written_in(uint32_t address, uint64_t size) const {
        if (!size) return false;
        const uint64_t end = std::min<uint64_t>(uint64_t(address) + size, 0x100000000ull);
        const uint64_t first = address >> 12, last = (end - 1) >> 12;
        for (uint64_t word = first / 64; word <= last / 64; ++word) {
            uint64_t mask = UINT64_MAX;
            if (word == first / 64) mask &= UINT64_MAX << (first % 64);
            if (word == last / 64) mask &= UINT64_MAX >> (63 - last % 64);
            if (written[word].load(std::memory_order_acquire) & mask) return true;
        }
        return false;
    }
    // Render thread: hands each run of written pages to f(address, size) and clears them.
    template <class F> void take_written(F&& f) {
        if (!any.exchange(false, std::memory_order_acq_rel)) return;
        for (uint64_t word = 0; word < written.size(); ++word) {
            if (!written[word].load(std::memory_order_relaxed)) continue;
            uint64_t bits = written[word].exchange(0, std::memory_order_acq_rel);
            while (bits) {
                const int lo = __builtin_ctzll(bits);
                uint64_t run = bits >> lo;
                const int n = run == UINT64_MAX ? 64 : __builtin_ctzll(~run);
                f(uint32_t((word * 64 + lo) << 12), uint64_t(n) << 12);
                bits &= n == 64 ? 0 : ~(((uint64_t{1} << n) - 1) << lo);
            }
        }
    }
};
inline TexturePageWrites texturePageWrites;
} // namespace gx2
