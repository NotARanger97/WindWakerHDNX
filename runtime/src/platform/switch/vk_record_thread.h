#pragma once

#ifdef __SWITCH__
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vkrecord {
bool enabled();
// A drain completes host command replay, not GPU execution. Fence/idle waits still
// execute in NVK on the caller after this host ordering boundary.
VkResult drain();
void report(uint64_t frame);
// Descriptor sets created on the recording thread: allocate + write happen in FIFO order before
// any bind that names the returned placeholder (bit 63 set + a slot index), which the replay of
// vkCmdBindDescriptorSets swaps for the real set. A placeholder is valid until its pool is reset
// (a draining call), like the set it stands for. Writes must use one buffer or image info each.
VkDescriptorSet defer_descriptor_set(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
                                     const VkWriteDescriptorSet* writes, uint32_t count);
[[noreturn]] void unsupported(const char* what);
void result(VkResult value);
struct alignas(16) Header {
    void (*replay)(void*);
    uint32_t size;
};
void* reserve(size_t bytes);
void publish(size_t bytes);
// Make every enqueued record visible to the consumer now (publish batches up to 4 KiB).
void flush();
// Calls after which the consumer must see everything at once: the renderer may go idle after them.
constexpr bool flushes(std::string_view name) {
    return name == "vkQueueSubmit" || name == "vkQueueSubmit2" || name == "vkQueuePresentKHR" ||
           name == "vkEndCommandBuffer";
}
constexpr size_t aligned(size_t n) { return (n + 15) & ~size_t(15); }

// The same traversal sizes and copies each record directly into the ring. No
// scratch allocation, pointer relocation, or producer-owned pointer survives.
struct Copy {
    std::byte* base = nullptr;
    size_t used = 0;
    template<class T> T* array(const T* p, size_t count) {
        if (!p || !count) return nullptr;
        static_assert(alignof(T) <= 16);
        if (count > (8 * 1024 * 1024) / sizeof(T)) unsupported("oversized array");
        const size_t offset = used;
        used += aligned(sizeof(T) * count);
        T* out = base ? reinterpret_cast<T*>(base + offset) : nullptr;
        if (out) std::memcpy(out, p, sizeof(T) * count);
        if constexpr (requires(T v) { v.pNext; }) {
            for (size_t i = 0; i < count; ++i) {
                T value = p[i];
                nested(value);
                if (out) out[i] = value;
            }
        }
        return out;
    }
    // No deferred runtime call currently has a non-null pNext. Fail closed:
    // adding an extension requires an explicit, recursively owned copy policy.
    void chain(const void* p) { if (p) unsupported("non-null pNext"); }
    template<class T> void nested(T& v) {
        if constexpr (requires { v.pNext; }) chain(v.pNext);
    }
    void nested(VkRenderingInfo& v) {
        chain(v.pNext);
        v.pColorAttachments = array(v.pColorAttachments, v.colorAttachmentCount);
        v.pDepthAttachment = array(v.pDepthAttachment, 1);
        v.pStencilAttachment = array(v.pStencilAttachment, 1);
    }
    void nested(VkCommandBufferBeginInfo& v) {
        chain(v.pNext);
        v.pInheritanceInfo = array(v.pInheritanceInfo, 1);
    }
    void nested(VkSubmitInfo& v) {
        chain(v.pNext);
        v.pWaitSemaphores = array(v.pWaitSemaphores, v.waitSemaphoreCount);
        v.pWaitDstStageMask = array(v.pWaitDstStageMask, v.waitSemaphoreCount);
        v.pCommandBuffers = array(v.pCommandBuffers, v.commandBufferCount);
        v.pSignalSemaphores = array(v.pSignalSemaphores, v.signalSemaphoreCount);
    }
    void nested(VkSubmitInfo2& v) {
        chain(v.pNext);
        v.pWaitSemaphoreInfos = array(v.pWaitSemaphoreInfos, v.waitSemaphoreInfoCount);
        v.pCommandBufferInfos = array(v.pCommandBufferInfos, v.commandBufferInfoCount);
        v.pSignalSemaphoreInfos = array(v.pSignalSemaphoreInfos, v.signalSemaphoreInfoCount);
    }
    void nested(VkPresentInfoKHR& v) {
        chain(v.pNext);
        v.pWaitSemaphores = array(v.pWaitSemaphores, v.waitSemaphoreCount);
        v.pSwapchains = array(v.pSwapchains, v.swapchainCount);
        v.pImageIndices = array(v.pImageIndices, v.swapchainCount);
        // An asynchronous API cannot write a caller's output after returning.
        // The renderer leaves this null; reject future synchronous output use.
        if (v.pResults) unsupported("present pResults output");
    }
};
template<class T> struct Array { const T* data; size_t count; };
template<class T> Array<T> array(const T* p, size_t n) { return {p, n}; }
inline auto bytes(const void* p, size_t n) { return array(static_cast<const std::byte*>(p), n); }
template<class T> T materialize(Copy&, T v) {
    // Vulkan handles point to incomplete opaque types. Every other pointer
    // needs Array/bytes, so extending the X-macro cannot leave a shallow copy.
    static_assert(!std::is_pointer_v<T> ||
        (!std::is_void_v<std::remove_pointer_t<T>> &&
         !requires { sizeof(std::remove_pointer_t<T>); }),
        "deferred pointer needs an explicit ownership policy");
    return v;
}
template<class T> const T* materialize(Copy& copy, Array<T> v) { return copy.array(v.data, v.count); }

template<auto Fn, class... Args> void enqueue(Args... args) {
    using Tuple = std::tuple<decltype(materialize(std::declval<Copy&>(), args))...>;
    static_assert(alignof(Tuple) <= 16);
    static_assert(std::is_trivially_destructible_v<Tuple>);
    constexpr size_t prefix = sizeof(Header) + aligned(sizeof(Tuple));
    Copy sizing;
    (materialize(sizing, args), ...);
    const size_t size = prefix + sizing.used;
    auto* memory = static_cast<std::byte*>(reserve(size));
    auto* header = new (memory) Header;
    header->size = uint32_t(size);
    header->replay = [](void* data) {
        auto* tuple = reinterpret_cast<Tuple*>(static_cast<std::byte*>(data) + sizeof(Header));
        if constexpr (std::is_same_v<decltype(std::apply(Fn, *tuple)), VkResult>)
            result(std::apply(Fn, *tuple));
        else
            std::apply(Fn, *tuple);
        tuple->~Tuple();
    };
    Copy copy{memory + prefix};
    new (memory + sizeof(Header)) Tuple{materialize(copy, args)...};
    publish(size);
}
} // namespace vkrecord
#endif
