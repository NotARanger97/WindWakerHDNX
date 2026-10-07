#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace gfxvk {
// CPU-only metadata for the last immutable snapshot in each VS/PS logical
// uniform slot (16 guest blocks and one support block per stage).
// Slice provides buffer, mapped and size; Device supports equality. The factory
// owns allocation, alignment and zero padding exactly as the normal snapshot
// path does. Returned bytes must remain immutable until the generation retires.
template<class Slice, class Device>
class UniformSnapshotCache {
public:
  static constexpr size_t slotCount = 34;
  struct Counters {
    uint64_t lookups = 0, checks = 0, comparisons = 0, hits = 0;
    // Avoided allocation bytes, including the snapshot's minimum-size padding.
    uint64_t reusedBytes = 0;
  } counters;

  // Caller must validate device uniform range limits before lookup. The factory
  // receives the fresh source and requested length, including nullptr/zero-size.
  template<class Factory>
  Slice get(Device device, uint64_t generation, size_t slot,
            const void* bytes, size_t size, Factory&& factory) {
    if (!initialized_ || device_ != device || generation_ != generation) {
      entries_ = {};
      device_ = device;
      generation_ = generation;
      initialized_ = true;
    }
    ++counters.lookups;
    if (slot >= slotCount)
      return std::forward<Factory>(factory)(bytes, size);
    auto& last = entries_[slot];
    if (last.valid && last.size == size) {
      ++counters.checks;
      bool equal;
      if (!size) equal = true;
      else if (!bytes) equal = last.knownZero;
      else {
        ++counters.comparisons;
        equal = std::memcmp(bytes, last.slice.mapped, size) == 0;
      }
      if (equal) {
        ++counters.hits;
        counters.reusedBytes += last.slice.size;
        return last.slice;
      }
    }
    auto slice = std::forward<Factory>(factory)(bytes, size);
    // Upload arenas may fall back to uncached memory on Switch. Never retain
    // those slices for mapped comparisons; CPU-only fixture slices need no flag.
    if constexpr (requires { slice.cpuReadable; }) {
      if (!slice.cpuReadable) { last = {}; return slice; }
    }
    // Failed/empty factories never publish an entry whose mapped pointer could
    // be dereferenced. Allocation padding is not compared as guest memory.
    if (slice.buffer && slice.mapped && slice.size == (size < 16 ? 16 : size))
      last = {slice, size, true, !bytes || !size};
    else
      last = {};
    return slice;
  }

  void reset() {
    entries_ = {};
    initialized_ = false;
  }

private:
  struct Entry {
    Slice slice{};
    size_t size = 0;
    bool valid = false, knownZero = false;
  };
  std::array<Entry, slotCount> entries_{};
  Device device_{};
  uint64_t generation_ = 0;
  bool initialized_ = false;
};
} // namespace gfxvk
