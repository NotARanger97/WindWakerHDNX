#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace gfxvk {
// Render-thread-only intervals. Append on resource publication, sort once
// after growth, and use prefix maximum ends to skip disjoint earlier ranges.
template<class Value>
class AddressRangeIndex {
    struct Entry { uint64_t begin, end, maxEnd; Value value; };
    std::vector<Entry> entries;
    bool sorted = true;
public:
    void clear() { entries.clear(); sorted = true; }
    void add(uint64_t begin, uint64_t end, Value value) {
        entries.push_back({begin, end, 0, value});
        sorted = false;
    }
    template<class Visit>
    void overlaps(uint64_t begin, uint64_t end, Visit visit) {
        if (!sorted) {
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) { return a.begin < b.begin; });
            uint64_t maxEnd = 0;
            for (auto& entry : entries) entry.maxEnd = maxEnd = std::max(maxEnd, entry.end);
            sorted = true;
        }
        auto it = std::lower_bound(entries.begin(), entries.end(), end,
            [](const Entry& entry, uint64_t address) { return entry.begin < address; });
        while (it != entries.begin()) {
            --it;
            if (it->maxEnd <= begin) break;
            if (begin < it->end) visit(it->value);
        }
    }
};
} // namespace gfxvk
