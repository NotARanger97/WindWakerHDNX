#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
#include <vector>

namespace gfxvk::vk {
// Emit the same uint32 stream as the original draw conversion. Native guest
// indices remain on their existing upload path; this helper owns no GPU state.
inline size_t converted_index_count(uint32_t prim, uint32_t count, bool indexed) {
  size_t n = indexed ? size_t(count) : 0;
  switch (prim) {
  case 5: n = count > 2 ? size_t(count - 2) * 3 : 0; break;
  case 0x13: n = size_t(count / 4) * 6; break;
  case 0x14: n = count >= 4 ? size_t((count - 2) / 2) * 6 : 0; break;
  case 0x12: n = size_t(count) + 1; break;
  case 1: case 2: case 3: case 4: case 6: break;
  default: throw std::runtime_error("unsupported Vulkan primitive");
  }
  // Preserve the original empty-conversion indexed fallback (short fan).
  if (!n && indexed) n = count;
  return n;
}
inline void resize_indices(std::vector<uint32_t>& out, size_t n) { out.resize(n); }
inline void resize_indices(std::span<uint32_t>& out, size_t n) {
  if (out.size() != n) throw std::runtime_error("incorrect index output size");
}
// A span writes directly to an owned upload slice, without value-initializing
// or copying an intermediate vector. Both outputs use exactly the same reader.
template<class Read, class Output>
void expand_indices(uint32_t prim, uint32_t count, bool indexed,
                    Read read, Output& out) {
  const size_t n = converted_index_count(prim, count, indexed);
  resize_indices(out, n);
  size_t o = 0;
  if (prim == 5 && count > 2) {
    const auto a = read(0); auto b = read(1);
    for (uint32_t i = 2; i < count; ++i) {
      auto c = read(i); out[o++] = a; out[o++] = b; out[o++] = c; b = c;
    }
  } else if (prim == 0x13 && count >= 4) {
    for (uint32_t i = 0; i + 3 < count; i += 4) {
      auto a = read(i), b = read(i+1), c = read(i+2), d = read(i+3);
      out[o++] = a; out[o++] = b; out[o++] = c;
      out[o++] = a; out[o++] = c; out[o++] = d;
    }
  } else if (prim == 0x14 && count >= 4) {
    auto a = read(0), b = read(1);
    for (uint32_t i = 2; i + 1 < count; i += 2) {
      auto c = read(i), d = read(i+1);
      out[o++] = a; out[o++] = b; out[o++] = c;
      out[o++] = b; out[o++] = d; out[o++] = c;
      a = c; b = d;
    }
  } else if (prim == 0x12) {
    for (uint32_t i = 0; i < count; ++i) out[i] = read(i);
    out[count] = read(0);
  } else {
    for (size_t i = 0; i < n; ++i) out[i] = read(uint32_t(i));
  }
}
template<class Word, bool BigEndian, bool Restart, class Output>
void convert_index_words(const void* data, uint32_t prim, uint32_t count,
                         uint32_t marker, Output& out) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  auto read = [&](uint32_t i) {
    Word word; std::memcpy(&word, bytes + size_t(i) * sizeof(Word), sizeof(Word));
    if constexpr (BigEndian) {
      if constexpr (sizeof(Word) == 2) word = __builtin_bswap16(word);
      else word = __builtin_bswap32(word);
    }
    uint32_t value = word;
    if constexpr (Restart) if (value == marker) return UINT32_MAX;
    return value;
  };
  expand_indices(prim, count, true, read, out);
}
template<class Output>
inline void convert_indices(const void* data, uint32_t prim, uint32_t count,
                            uint32_t type, bool restart, uint32_t marker,
                            Output& out) {
  if (!data) {
    expand_indices(prim, count, false, [](uint32_t i) { return i; }, out);
    return;
  }
  // Empty indexed conversions never invoked the original reader, except
  // line-loop closure, which still reads index zero when count is zero.
  if (!count && prim != 0x12) {
    expand_indices(prim, count, true, [](uint32_t) -> uint32_t {
      throw std::runtime_error("unsupported index type");
    }, out);
    return;
  }
#define WWHD_INDEX_CASE(T, W, BE) case T: \
  if (restart) convert_index_words<W, BE, true>(data, prim, count, marker, out); \
  else convert_index_words<W, BE, false>(data, prim, count, marker, out); break
  switch (type) {
    WWHD_INDEX_CASE(0, uint16_t, false);
    WWHD_INDEX_CASE(1, uint32_t, false);
    WWHD_INDEX_CASE(4, uint16_t, true);
    WWHD_INDEX_CASE(9, uint32_t, true);
    default: throw std::runtime_error("unsupported index type");
  }
#undef WWHD_INDEX_CASE
}
} // namespace gfxvk::vk
