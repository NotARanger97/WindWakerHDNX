#pragma once
#include <cassert>
#include <cstdint>
#include <vector>

// Object lock held by callers. Allocation happens only at init/load, never on
// send/receive. Keep logical indexing for the existing save-state wire format.
template <class T> class MessageRing {
    std::vector<T> storage;
    uint32_t first = 0, count = 0;
    uint32_t next(uint32_t i) const { return i + 1 == storage.size() ? 0 : i + 1; }
public:
    void reset(uint32_t capacity) { storage.resize(capacity); clear(); }
    void clear() { first = count = 0; }
    uint32_t size() const { return count; }
    bool empty() const { return !count; }
    const T& operator[](uint32_t i) const {
        uint64_t at = uint64_t(first) + i;
        if (at >= storage.size()) at -= storage.size();
        return storage[size_t(at)];
    }
    const T& front() const { return storage[first]; }
    void pop_front() { assert(count); first = next(first); --count; }
    void push_back(const T& value) {
        assert(count < storage.size());
        uint64_t at = uint64_t(first) + count;
        if (at >= storage.size()) at -= storage.size();
        storage[size_t(at)] = value;
        ++count;
    }
    void push_front(const T& value) {
        assert(count < storage.size());
        first = first ? first - 1 : uint32_t(storage.size() - 1);
        storage[first] = value;
        ++count;
    }
};
