#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace gfxvk {
// Render-thread-only, exact variable-length word keys. Warm finds allocate
// nothing and probe a dense metadata array before touching the key arena.
template<class Value> class WordCache {
  struct Slot {
    uint64_t hash = 0;
    Value value{};
    size_t begin = 0, count = 0;
  };
  std::vector<Slot> slots;
  std::vector<uint64_t> words;
  size_t used = 0;
  static uint64_t hash_key(std::span<const uint64_t> key) {
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    for (uint64_t word : key) {
      hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
      hash ^= hash >> 32;
    }
    hash ^= hash >> 29;
    return hash ? hash : 1; // zero denotes an empty slot
  }
  void grow() {
    std::vector<Slot> next(slots.empty() ? 1024 : slots.size() * 2);
    for (const auto& slot : slots) if (slot.hash) {
      size_t i = size_t(slot.hash) & (next.size() - 1);
      while (next[i].hash) i = (i + 1) & (next.size() - 1);
      next[i] = slot;
    }
    slots.swap(next);
  }
public:
  size_t bytes() const { return slots.capacity() * sizeof(Slot) + words.capacity() * sizeof(uint64_t); }
  Value find(std::span<const uint64_t> key) const {
    if (slots.empty()) return {};
    const uint64_t hash = hash_key(key);
    size_t i = size_t(hash) & (slots.size() - 1);
    while (slots[i].hash) {
      const auto& slot = slots[i];
      if (slot.hash == hash && slot.count == key.size() &&
          (!slot.count || !std::memcmp(words.data() + slot.begin, key.data(), slot.count * 8)))
        return slot.value;
      i = (i + 1) & (slots.size() - 1);
    }
    return {};
  }
  void insert(std::span<const uint64_t> key, Value value) {
    if (slots.empty() || (used + 1) * 2 > slots.size()) grow();
    const uint64_t hash = hash_key(key);
    size_t i = size_t(hash) & (slots.size() - 1);
    while (slots[i].hash) {
      auto& slot = slots[i];
      if (slot.hash == hash && slot.count == key.size() &&
          (!slot.count || !std::memcmp(words.data() + slot.begin, key.data(), slot.count * 8))) {
        slot.value = value;
        return;
      }
      i = (i + 1) & (slots.size() - 1);
    }
    const size_t begin = words.size();
    words.insert(words.end(), key.begin(), key.end());
    slots[i] = {hash, value, begin, key.size()};
    ++used;
  }
  void clear() {
    std::fill(slots.begin(), slots.end(), Slot{});
    words.clear(); used = 0;
  }
};
} // namespace gfxvk
