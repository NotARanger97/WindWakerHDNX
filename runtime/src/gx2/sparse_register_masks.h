#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gx2 {
// Most register pages share the conservative unknown-register classification.
// Retain exact per-word masks only on pages that differ from that default.
template<class Masks, size_t RegCount> class SparseRegisterMasks {
    static_assert(RegCount % 256 == 0 && RegCount / 256 < UINT16_MAX);
    std::array<uint16_t, RegCount / 256> index{};
    std::vector<std::array<Masks, 256>> pages;
    Masks fallback;
public:
    template<class Classify> SparseRegisterMasks(Masks defaults, Classify&& classify) : fallback(defaults) {
        for (size_t page = 0; page < index.size(); ++page) {
            std::array<Masks, 256> words;
            bool different = false;
            for (size_t word = 0; word < words.size(); ++word) {
                words[word] = classify(uint32_t(page * 256 + word));
                different |= !(words[word] == fallback);
            }
            if (different) {
                pages.push_back(words);
                index[page] = uint16_t(pages.size());
            }
        }
    }
    const Masks& operator[](uint32_t reg) const {
        const auto page = index[reg >> 8];
        return page ? pages[page - 1][reg & 255] : fallback;
    }
    size_t bytes() const { return sizeof(index) + pages.size() * sizeof(pages[0]) + sizeof(fallback); }
};
} // namespace gx2
