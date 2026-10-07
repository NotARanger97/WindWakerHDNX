#pragma once
#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace gfxvk {
// Borrow key bytes during lookups; only a new cache entry needs an owning key.
// Both owning and borrowed keys MUST use the same hash for heterogeneous find.
struct CacheKeyHash {
    using is_transparent = void;
    size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};
template<size_t Capacity>
struct CacheKeyBytes {
    std::array<char, Capacity> stack;
    size_t size = 0;
    bool spilled = false;
    std::string overflow;
    void reset() { size = 0; spilled = false; overflow.clear(); }
    void append(const void* source, size_t bytes) {
        if (!spilled && bytes <= stack.size() - size) {
            std::memcpy(stack.data() + size, source, bytes);
        } else {
            if (!spilled) {
                overflow.assign(stack.data(), size);
                spilled = true;
            }
            overflow.append(static_cast<const char*>(source), bytes);
        }
        size += bytes;
    }
    const char* data() const { return spilled ? overflow.data() : stack.data(); }
    bool bounded() const { return !spilled; }
    std::string_view view() const { return {data(), size}; }
};
} // namespace gfxvk
