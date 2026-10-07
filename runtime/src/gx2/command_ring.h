#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace gx2 {
// Single consumer; the producer gate also permits guest/host threads other than
// the main thread to emit. The consumer tries it only when published work runs out.
// Payloads are owned until consumption completes. Sequences include wrap NOPs.
template <uint32_t Capacity> class CommandRing {
    static_assert(Capacity >= 1024 && (Capacity & (Capacity - 1)) == 0);
    alignas(64) uint32_t words[Capacity];
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) std::atomic_flag producing = ATOMIC_FLAG_INIT;
    uint64_t producer = 0, published = 0, consumed = 0;
    uint64_t cachedTail = 0; // producer only, while holding producing
    std::atomic<bool> parked{false};
    std::mutex wakeMutex;
    std::condition_variable wakeCv;
    bool wakePending = false;
    static void pause() {
        // Let a lower-priority consumer run even when mapped to our CPU core.
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    void signal() {
        if (parked.load(std::memory_order_seq_cst) &&
            parked.exchange(false, std::memory_order_seq_cst)) {
            { std::lock_guard<std::mutex> lk(wakeMutex); wakePending = true; }
            wakeCv.notify_one();
        }
    }
    void space(uint32_t count) {
        if (producer + count - cachedTail <= Capacity) return;
        cachedTail = tail.load(std::memory_order_acquire);
        if (producer + count - cachedTail <= Capacity) return;
        flush();
        do {
            signal();
            pause();
            cachedTail = tail.load(std::memory_order_acquire);
        } while (producer + count - cachedTail > Capacity);
    }
public:
    void lock() {
        while (producing.test_and_set(std::memory_order_acquire)) pause();
    }
    void unlock() { producing.clear(std::memory_order_release); }
    // reserve/commit/flush require the producer gate. Header is the GX2 encoding.
    uint32_t* reserve(uint32_t count) {
        if (!count || count > Capacity) std::abort();
        const uint32_t offset = uint32_t(producer) & (Capacity - 1);
        if (offset + count > Capacity) {
            const uint32_t padding = Capacity - offset;
            space(padding);
            words[offset] = (padding - 1) << 8; // OP_NOP, skip uninitialized padding
            producer += padding;
            flush();
        }
        space(count);
        return words + (uint32_t(producer) & (Capacity - 1));
    }
    void flush() {
        if (published == producer) return;
        published = producer;
        // Together with the consumer's parked announcement/recheck this closes
        // the missed-wake window. The CV token stays latched before wait starts.
        head.store(producer, std::memory_order_seq_cst);
        signal();
    }
    void commit(uint32_t count, bool boundary) {
        producer += count;
        if (boundary || producer - published >= 1024 || parked.load(std::memory_order_seq_cst)) flush();
    }
    // One contiguous published span; consumer holds storage through complete().
    const uint32_t* wait(uint32_t& count) {
        for (;;) {
            uint64_t available = head.load(std::memory_order_acquire);
            if (available != consumed) {
                const uint32_t offset = uint32_t(consumed) & (Capacity - 1);
                count = uint32_t(std::min<uint64_t>(available - consumed, Capacity - offset));
                return words + offset;
            }
            parked.store(true, std::memory_order_seq_cst);
            // A producer can stop after a short batch while we are executing
            // the previous span. Publish that batch before sleeping. Never
            // wait for the gate: a full producer may need us to consume first.
            if (producing.test_and_set(std::memory_order_acquire)) {
                parked.store(false, std::memory_order_seq_cst);
                pause();
                continue;
            }
            flush();
            unlock();
            if (consumed == head.load(std::memory_order_seq_cst)) {
                std::unique_lock<std::mutex> lk(wakeMutex);
                wakeCv.wait(lk, [&] { return wakePending; });
                wakePending = false;
            }
            parked.store(false, std::memory_order_seq_cst);
        }
    }
    void complete(uint32_t count) {
        consumed += count;
        tail.store(consumed, std::memory_order_release);
    }
};
} // namespace gx2
