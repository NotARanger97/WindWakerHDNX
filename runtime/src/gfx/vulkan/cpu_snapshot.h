#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gfxvk {
// Compare freshly prepared bytes with a CPU copy, never with mapped upload
// memory. Identity selects a shader, generation bounds the slice's lifetime.
template<class Slice, class Device, size_t Slots = 256> class CpuSnapshotCache {
  struct Entry {
    const void* identity = nullptr;
    Device device{};
    uint64_t generation = 0;
    Slice slice{};
    std::vector<uint8_t> bytes;
  };
  std::array<Entry, Slots> entries;
public:
  uint64_t checks = 0, hits = 0, reusedBytes = 0;
  template<class Factory>
  Slice get(const void* identity, Device device, uint64_t generation,
            const void* source, size_t size, Factory&& factory) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(identity);
    auto& entry = entries[(p / 16 ^ p / 4096) % Slots];
    ++checks;
    if (entry.identity == identity && entry.device == device && entry.generation == generation &&
        entry.slice.buffer && entry.bytes.size() == size &&
        (!size || !std::memcmp(entry.bytes.data(), source, size))) {
      ++hits; reusedBytes += entry.slice.size;
      return entry.slice;
    }
    const Slice slice = factory(source, size);
    entry.bytes.resize(size);
    if (size) std::memcpy(entry.bytes.data(), source, size);
    entry.identity = identity; entry.device = device;
    entry.generation = generation; entry.slice = slice;
    return slice;
  }
  void reset() {
    for (auto& entry : entries) { entry.identity = nullptr; entry.slice = {}; }
  }
};
} // namespace gfxvk
