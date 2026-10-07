// Switch-only loaderless Vulkan command recorder. Public deferred entry points
// below and the generator share vk_record_calls.inc as their only call list.
#ifdef __SWITCH__
#include <switch.h>
#include "vk_record_thread.h"
#include "platform/host.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void switch_set_helper_thread(int core, int priority);
extern "C" void switch_note_record_thread();
extern "C" void switch_sample_core_idle();

namespace vkrecord {
namespace {
constexpr size_t capacity = 8 * 1024 * 1024;
alignas(64) std::byte ring[capacity];
// Monotonic byte sequences include padding; tail is also replay completion.
alignas(64) std::atomic<uint64_t> head{0};
alignas(64) std::atomic<uint64_t> tail{0};
alignas(64) std::atomic<bool> parked{false};
std::atomic<VkResult> error{VK_SUCCESS};
// Latched, auto-clearing wake (the borrowed title's permissions do not include svcCreateEvent;
// libnx condition variables use svcWaitProcessWideKeyAtomic like the rest of the runtime).
std::mutex wakeMutex;
std::condition_variable wakeCv;
bool wakePending = false;
void fire_wake() {
    { std::lock_guard<std::mutex> lock(wakeMutex); wakePending = true; }
    wakeCv.notify_one();
}
void wait_wake() {
    std::unique_lock<std::mutex> lock(wakeMutex);
    wakeCv.wait(lock, [] { return wakePending; });
    wakePending = false;
}
uint64_t producer = 0; // single producer, renderer (including its startup smoke)
uint64_t published = 0; // producer-side copy of head
bool stats = false;
struct Stats { uint64_t calls=0, high=0, stalls=0, waits=0, ticks=0; } counters;

void signal() {
    // No syscall unless the consumer has announced that it is parking.
    if (parked.load(std::memory_order_seq_cst) &&
        parked.exchange(false, std::memory_order_seq_cst))
        fire_wake();
}
void consume() {
    host::set_thread_name("VK record");
    const char* coreEnv = std::getenv("WWHD_VK_RECORD_CORE");
    const char* prioEnv = std::getenv("WWHD_VK_RECORD_PRIO");
    const int core = coreEnv ? std::atoi(coreEnv) : 0;
    const int priority = prioEnv ? int(std::strtol(prioEnv, nullptr, 0)) : 0x2D;
    if (core < 0 || core > 2 || priority < 0x20 || priority > 63)
        unsupported("invalid recording core/priority");
    // Naming precedes helper placement, which registers this thread in the sampler.
    switch_set_helper_thread(core, priority);
    switch_note_record_thread();
    uint64_t cursor = 0;
    bool failed = false;
    for (;;) {
        switch_sample_core_idle();
        uint64_t available = head.load(std::memory_order_acquire);
        if (cursor == available) {
            // A bounded spin bridges small producer gaps, but must not starve
            // guests on core 0; then park on the latched wake.
            for (unsigned i=0; i<128 && cursor==available; ++i) {
#ifdef __aarch64__
                __asm__ volatile("yield");
#else
                std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
                available = head.load(std::memory_order_acquire);
            }
            if (cursor == available) {
                // seq_cst pairs this announcement with the producer's exchange.
                // Recheck after announcing; a signal before wait remains latched.
                parked.store(true, std::memory_order_seq_cst);
                if (cursor == head.load(std::memory_order_seq_cst)) wait_wake();
                parked.store(false, std::memory_order_seq_cst);
                continue;
            }
        }
        auto* record = reinterpret_cast<Header*>(ring + cursor % capacity);
        const uint32_t size = record->size;
        if (record->replay && !failed) {
            record->replay(record);
            failed = error.load(std::memory_order_relaxed) != VK_SUCCESS;
        }
        // After a failed begin/end/submit, discard remaining records. In
        // particular, never enter present waiting on a failed submit's signal
        // semaphore: that could prevent the next drain from reporting failure.
        cursor += size;
        // Only release storage and synchronizing callers AFTER NVK returns.
        tail.store(cursor, std::memory_order_release);
    }
}
bool start() {
    const char* e = std::getenv("WWHD_VK_RECORD_THREAD");
    if (e && !std::strcmp(e, "0")) return false;
    stats = std::getenv("WWHD_VK_STATS") != nullptr;
    // Process-lifetime helper, like the sampling profiler. After device destroy
    // it is parked and cannot touch NVK until another record is published.
    (new std::thread(consume))->detach();
    std::fprintf(stderr, "[vulkan record] enabled, 8 MiB ring\n");
    return true;
}
void space(size_t bytes) {
    bool stalled = false;
    if (producer + bytes - tail.load(std::memory_order_acquire) > capacity) flush();
    while (producer + bytes - tail.load(std::memory_order_acquire) > capacity) {
        if (stats && !stalled) { ++counters.stalls; stalled=true; }
        signal();
        // A timed sleep lets a lower-priority recorder run even when both are
        // configured on one core; SleepThread(0) can starve it in that case.
        svcSleepThread(1000);
    }
}
} // namespace
bool enabled() { static const bool on = start(); return on; }
[[noreturn]] void unsupported(const char* what) {
    std::fprintf(stderr, "[vulkan record] unsupported: %s\n", what);
    std::abort();
}
void result(VkResult value) {
    if (value == VK_SUCCESS) return;
    VkResult expected = VK_SUCCESS;
    if (error.compare_exchange_strong(expected, value, std::memory_order_relaxed))
        std::fprintf(stderr, "[vulkan record] deferred Vulkan result %d (reported at next drain)\n", int(value));
}
void* reserve(size_t bytes) {
    if (bytes > capacity || bytes < sizeof(Header) || bytes != aligned(bytes))
        unsupported("record exceeds ring capacity or is misaligned");
    const size_t offset = producer % capacity;
    if (offset + bytes > capacity) {
        const size_t padding = capacity - offset;
        space(padding);
        new (ring + offset) Header{nullptr, uint32_t(padding)};
        producer += padding;
        if (stats) {
            const uint64_t used = producer - tail.load(std::memory_order_acquire);
            if (used > counters.high) counters.high = used;
        }
        flush();
    }
    space(bytes);
    return ring + producer % capacity;
}
void flush() {
    if (published == producer) return;
    published = producer;
    // seq_cst prevents a missed wake between the consumer's parked store and
    // its head recheck. Storage ownership otherwise needs only release/acquire.
    head.store(producer, std::memory_order_seq_cst);
    signal();
}
void publish(size_t bytes) {
    producer += bytes;
    if (stats) {
        ++counters.calls;
        const uint64_t used = producer - tail.load(std::memory_order_acquire);
        if (used > counters.high) counters.high = used;
    }
    // Batched: one barrier and one shared cache-line write per 4 KiB of records, not per call.
    if (producer - published >= 4096) flush();
}
VkResult drain() {
    if (!enabled()) return VK_SUCCESS;
    flush();
    const uint64_t target = head.load(std::memory_order_seq_cst);
    if (tail.load(std::memory_order_acquire) < target) {
        const uint64_t begin = stats ? armGetSystemTick() : 0;
        do { signal(); svcSleepThread(1000); }
        while (tail.load(std::memory_order_acquire) < target);
        if (stats) { ++counters.waits; counters.ticks += armGetSystemTick() - begin; }
    }
    // Sticky: a failed submit must not be swallowed and followed by an infinite
    // fence wait. Result-returning forwarders return it before entering NVK;
    // void forwarders log it here but still perform teardown safely.
    const VkResult value = error.load(std::memory_order_relaxed);
    static bool reported = false;
    if (value != VK_SUCCESS && !reported) {
        reported = true;
        std::fprintf(stderr, "[vulkan record] drain reports sticky Vulkan result %d\n", int(value));
    }
    return value;
}
void report(uint64_t frame) {
    if (!enabled() || !stats) return;
    static uint64_t previous = 0;
    if (frame - previous < 120) return;
    std::fprintf(stderr, "[vulkan record] %.1f calls/frame; ring high %llu bytes; full stalls %llu; drain waits %llu, %.3f ms total\n",
        double(counters.calls) / double(frame - previous),
        (unsigned long long)counters.high, (unsigned long long)counters.stalls,
        (unsigned long long)counters.waits, armTicksToNs(counters.ticks) / 1e6);
    counters = {}; previous = frame;
}
} // namespace vkrecord

// Ordering decisions (also docs/vulkan-record-thread.md):
// vkCmd*, begin/end/reset, queue submit/present: FIFO on the consumer.
// Queue/cmd/pool/fence/semaphore/swapchain/query host accesses: drain, caller.
// Destroy/free/unmap/reset-descriptor-pool: drain, caller; GPU lifetime remains
// the renderer's fence responsibility (retire_submission waits before cleanup).
// Allocate/update descriptors: caller, no drain. NVK reads dynamic_buffers at
// bind time; draw/present/smoke write ONLY fresh sets before enqueueing a bind.
// Allocation never resets existing sets; pool reset/free is a drained operation.
// Create pipelines/images/buffers, allocate/map memory: caller, different objects.
// Flush mapped memory: caller, before enqueueing submit; slot-exclusive uploads.
// GPU readbacks: flush_readback/wait_submission fence waits drain first.
// vkAllocateDescriptorSets / vkUpdateDescriptorSets are plain (non-draining) forwarders.
extern "C" VKAPI_ATTR void VKAPI_CALL nvk_real_vkCmdBindDescriptorSets(
    VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*,
    uint32_t, const uint32_t*);
namespace vkrecord {
namespace {
constexpr uint64_t kPlaceholder = uint64_t{1} << 63;
constexpr uint32_t kSlots = 1u << 16;
// Recording thread only: placeholder slot -> real set.
VkDescriptorSet slots[kSlots];
uint32_t nextSlot = 0;  // producer
struct PackedWrite {
    uint32_t binding, type;
    VkDescriptorBufferInfo buffer;
    VkDescriptorImageInfo image;
};
VkDescriptorSet real_set(VkDescriptorSet set) {
    const uint64_t v = uint64_t(set);
    return v & kPlaceholder ? slots[v & (kSlots - 1)] : set;
}
void replay_defer(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout, uint32_t slot,
                  uint32_t count, const PackedWrite* packed) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult r = vkAllocateDescriptorSets(device, &ai, &set);
    slots[slot] = set;
    if (r != VK_SUCCESS) { result(r); return; }
    VkWriteDescriptorSet writes[64];
    if (count > 64) unsupported("deferred descriptor set with more than 64 writes");
    for (uint32_t i = 0; i < count; ++i) {
        auto& w = writes[i];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = packed[i].binding;
        w.descriptorCount = 1;
        w.descriptorType = VkDescriptorType(packed[i].type);
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            w.pBufferInfo = &packed[i].buffer;
        else
            w.pImageInfo = &packed[i].image;
    }
    if (count) vkUpdateDescriptorSets(device, count, writes, 0, nullptr);
}
void replay_bind_sets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                      uint32_t count, const VkDescriptorSet* sets, uint32_t offsetCount, const uint32_t* offsets) {
    VkDescriptorSet real[8];
    if (count > 8) unsupported("more than 8 descriptor sets in one bind");
    for (uint32_t i = 0; i < count; ++i) real[i] = real_set(sets[i]);
    nvk_real_vkCmdBindDescriptorSets(cmd, point, layout, first, count, real, offsetCount, offsets);
}
}  // namespace
VkDescriptorSet defer_descriptor_set(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
                                     const VkWriteDescriptorSet* writes, uint32_t count) {
    PackedWrite packed[64];
    if (count > 64) unsupported("deferred descriptor set with more than 64 writes");
    for (uint32_t i = 0; i < count; ++i) {
        const auto& w = writes[i];
        if (w.descriptorCount != 1 || w.pNext || w.dstArrayElement) unsupported("deferred descriptor write shape");
        packed[i] = {w.dstBinding, uint32_t(w.descriptorType), {}, {}};
        if (w.pBufferInfo) packed[i].buffer = *w.pBufferInfo;
        else if (w.pImageInfo) packed[i].image = *w.pImageInfo;
        else unsupported("deferred descriptor write without info");
    }
    const uint32_t slot = nextSlot++ & (kSlots - 1);
    enqueue<replay_defer>(device, pool, layout, slot, count, array(packed, count));
    return VkDescriptorSet(kPlaceholder | slot);
}
// vkCmdBindDescriptorSets replays through replay_bind_sets (placeholders); everything else directly.
template <auto Fn> constexpr auto replay_of = Fn;
template <> constexpr auto replay_of<&nvk_real_vkCmdBindDescriptorSets> = &replay_bind_sets;
}  // namespace vkrecord
#define VK_RECORD_CALL(ret, name, params, args, packed) \
    extern "C" VKAPI_ATTR ret VKAPI_CALL nvk_real_##name params; \
    extern "C" VKAPI_ATTR ret VKAPI_CALL name params { \
        if (!vkrecord::enabled()) return nvk_real_##name args; \
        vkrecord::enqueue<vkrecord::replay_of<&nvk_real_##name>> packed; \
        if constexpr (vkrecord::flushes(#name)) vkrecord::flush(); \
        return ret(); \
    }
#include "vk_record_calls.inc"
#undef VK_RECORD_CALL
#endif
