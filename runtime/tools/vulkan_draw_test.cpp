// Standalone CPU regressions; see docs/vulkan-draw-cpu.md for build commands.
#include "gfx/vulkan/address_range_index.h"
#include "gfx/vulkan/cache_key.h"
#include "gfx/vulkan/draw_options.h"
#include "gfx/vulkan/index_conversion.h"
#include "gx2/shader_program_writes.h"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <thread>

static size_t allocations = 0;
void* operator new(size_t bytes) {
    ++allocations;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void ranges() {
    struct Range { uint64_t begin, end; };
    std::vector<Range> reference;
    gfxvk::AddressRangeIndex<size_t> index;
    std::mt19937 random(42);
    auto add = [&](uint64_t begin, uint64_t end) {
        index.add(begin, end, reference.size());
        reference.push_back({begin, end});
    };
    // Nested, duplicate, empty, adjoining, disjoint mip and 32-bit overflow.
    add(0, 100000); add(100, 200); add(100, 200); add(200, 300);
    add(150, 150); add(4000, 4500); add(UINT32_MAX - 20, uint64_t(UINT32_MAX) + 100);
    for (size_t i = 0; i < 1000; ++i) {
        const uint64_t begin = random() % 100000;
        add(begin, begin + random() % 10000);
    }
    auto check = [&](uint64_t begin, uint64_t end) {
        std::vector<size_t> actual, expected;
        index.overlaps(begin, end, [&](size_t value) { actual.push_back(value); });
        for (size_t i = 0; i < reference.size(); ++i)
            if (reference[i].begin < end && begin < reference[i].end) expected.push_back(i);
        std::sort(actual.begin(), actual.end());
        require(actual == expected, "interval index differs from original overlap predicate");
    };
    for (uint64_t boundary : {0ull, 100ull, 150ull, 200ull, 300ull, 4000ull, 4500ull, 4294967295ull}) {
        check(boundary, boundary); check(boundary, boundary + 1);
        if (boundary) check(boundary - 1, boundary);
    }
    for (size_t i = 0; i < 10000; ++i) {
        const uint64_t begin = random() % 110000;
        check(begin, begin + random() % 1000);
        if (i % 200 == 0) add(begin, begin + 2000); // Sort again after growth.
    }
    index.clear();
    bool visited = false;
    index.overlaps(0, UINT64_MAX, [&](size_t) { visited = true; });
    require(!visited, "cleared range index retained resources");
    index.add(42, 43, 17);
    index.overlaps(42, 43, [&](size_t value) { visited = value == 17; });
    require(visited, "range index failed after reset");
}
static void keys() {
    std::unordered_map<std::string, int, gfxvk::CacheKeyHash, std::equal_to<>> cache;
    const std::string binary("\0a\0b", 4);
    cache.emplace(binary, 7);
    gfxvk::CacheKeyBytes<3> scratch;
    scratch.append(binary.data(), binary.size());
    require(!scratch.bounded() && scratch.view() == binary, "key overflow lost binary bytes");
    require(cache.find(scratch.view())->second == 7, "borrowed key hash differs from owning hash");
    const size_t capacity = scratch.overflow.capacity(), before = allocations;
    for (size_t i = 0; i < 1000; ++i) {
        scratch.reset(); scratch.append(binary.data(), 2); scratch.append(binary.data() + 2, 2);
        require(cache.find(scratch.view())->second == 7, "warm binary key lookup failed");
    }
    require(allocations == before && scratch.overflow.capacity() == capacity,
        "warm borrowed key lookup allocated");
    scratch.reset(); scratch.append("x", 1);
    require(scratch.bounded() && scratch.view() == "x", "short key retained old overflow bytes");
    // Force collisions to ensure comparison includes all active binary bytes.
    struct CollisionHash {
        using is_transparent = void;
        size_t operator()(std::string_view) const { return 0; }
    };
    std::unordered_map<std::string, int, CollisionHash, std::equal_to<>> collisions;
    collisions.emplace(binary, 1); collisions.emplace(std::string("\0a\0c", 4), 2);
    require(collisions.find(std::string_view(binary))->second == 1, "binary key collision was not checked");
}
static std::vector<uint32_t> reference_indices(uint32_t prim, const std::vector<uint32_t>& input, bool indexed) {
    std::vector<uint32_t> out;
    const uint32_t count = uint32_t(input.size());
    auto at = [&](uint32_t i) { return input.empty() ? 0 : input[i]; };
    switch (prim) {
    case 5:
        for (uint32_t i = 1; i + 1 < count; ++i) out.insert(out.end(), {at(0), at(i), at(i + 1)});
        break;
    case 0x13:
        for (uint32_t i = 0; i + 3 < count; i += 4)
            out.insert(out.end(), {at(i), at(i+1), at(i+2), at(i), at(i+2), at(i+3)});
        break;
    case 0x14:
        for (uint32_t i = 0; i + 3 < count; i += 2)
            out.insert(out.end(), {at(i), at(i+1), at(i+2), at(i+1), at(i+3), at(i+2)});
        break;
    case 0x12: out = input; out.push_back(at(0)); break;
    }
    if (out.empty() && indexed) out = input;
    return out;
}
static void indices() {
    std::mt19937 random(17);
    std::vector<uint32_t> scratch;
    scratch.reserve(4096);
    for (uint32_t prim : {1u, 2u, 3u, 4u, 5u, 6u, 0x12u, 0x13u, 0x14u})
        for (uint32_t type : {0u, 1u, 4u, 9u})
            for (bool restart : {false, true})
                for (uint32_t count = 0; count < 65; ++count) {
                    const uint32_t width = type == 0 || type == 4 ? 2 : 4;
                    const bool bigEndian = type == 4 || type == 9;
                    const uint32_t marker = count % 2 ? (width == 2 ? UINT16_MAX : UINT32_MAX) : 7;
                    // Unaligned payload, restart markers and nontrivial high bytes.
                    std::vector<uint8_t> bytes(1 + std::max(count, 1u) * width);
                    std::vector<uint32_t> normalized;
                    for (uint32_t i = 0; i < std::max(count, 1u); ++i) {
                        const uint32_t raw = i % 3 ? random() & (width == 2 ? UINT16_MAX : UINT32_MAX) : marker;
                        for (uint32_t b = 0; b < width; ++b)
                            bytes[1 + i * width + b] = uint8_t(raw >> (8 * (bigEndian ? width - 1 - b : b)));
                        if (i < count) normalized.push_back(restart && raw == marker ? UINT32_MAX : raw);
                    }
                    const size_t before = allocations;
                    gfxvk::vk::convert_indices(bytes.data() + 1, prim, count, type, restart, marker, scratch);
                    require(allocations == before, "warm index conversion allocated");
                    auto expected = reference_indices(prim, normalized, true);
                    if (!count && prim == 0x12) expected[0] = restart ? UINT32_MAX : marker;
                    require(scratch == expected, "specialized index stream differs from original draw loop");
                    std::array<uint32_t, 260> mapped;
                    mapped.fill(0xABCD1234);
                    const size_t outputCount = gfxvk::vk::converted_index_count(prim, count, true);
                    std::span<uint32_t> output(mapped.data() + 1, outputCount);
                    gfxvk::vk::convert_indices(bytes.data() + 1, prim, count, type, restart, marker, output);
                    require(std::equal(output.begin(), output.end(), expected.begin(), expected.end()),
                        "mapped conversion differs from reference");
                    require(mapped[0] == 0xABCD1234 && mapped[outputCount + 1] == 0xABCD1234,
                        "mapped conversion wrote outside slice");
                    for (uint32_t i = 0; i < count; ++i) normalized[i] = i;
                    gfxvk::vk::convert_indices(nullptr, prim, count, type, restart, marker, scratch);
                    require(scratch == reference_indices(prim, normalized, false), "nonindexed expansion differs");
                    std::span<uint32_t> generated(mapped.data() + 1,
                        gfxvk::vk::converted_index_count(prim, count, false));
                    gfxvk::vk::convert_indices(nullptr, prim, count, type, restart, marker, generated);
                    require(std::equal(generated.begin(), generated.end(), scratch.begin(), scratch.end()),
                        "mapped nonindexed expansion differs");
                }
}
static void shader_writes() {
    gx2::ShaderProgramWrites writes;
    auto epoch = writes.generation();
    writes.notify(0x10000000, 0x10000); // Writes before first lookup need no invalidation.
    require(writes.generation() == epoch, "unwatched writes changed shader epoch");
    writes.watch(0x10001FF0, 32); // Straddles two pages.
    writes.watch(0x10001FF0, 16); // A smaller reused range must retain both watches.
    writes.notify(0x10001000, 0);
    writes.notify(0x10000000, 4096);
    writes.notify(0x10003000, 4096);
    require(writes.generation() == epoch, "empty/disjoint shader writes invalidated");
    writes.notify(0x10001FFF, 1);
    require(writes.generation() == ++epoch, "first shader page write missed");
    writes.notify(0x10002000, 1);
    require(writes.generation() == ++epoch, "reused program lost second page watch");
    writes.notify(0x10001000, 1); // Conservative false positive within a watched page.
    require(writes.generation() == ++epoch, "page-level shader invalidation missed");
    writes.watch(UINT32_MAX - 7, 8);
    writes.notify(UINT32_MAX, 1);
    require(writes.generation() == ++epoch, "4 GiB shader endpoint truncated");
    writes.notify(UINT32_MAX, UINT64_MAX);
    require(writes.generation() == ++epoch, "overflowing shader flush missed");
    writes.notify(0, 0x100000000ull);
    require(writes.generation() == ++epoch, "whole-window shader flush missed");
    writes.invalidate(); // Shader-bit GX2Invalidate is global, independent of its range.
    require(writes.generation() == ++epoch, "explicit shader invalidation missed");
    std::thread producer([&] {
        for (unsigned i = 0; i < 10000; ++i) writes.notify(0x10002000, 16);
    });
    for (unsigned i = 0; i < 10000; ++i) writes.watch(0x20000000 + i * 4096, 16);
    producer.join();
    require(writes.generation() == epoch + 10000, "concurrent shader notifications lost epochs");
    gx2::ShaderProgramWrites randomWrites;
    std::array<bool, 512> watched{};
    std::mt19937 random(91);
    constexpr uint32_t base = 0x30000000;
    for (unsigned i = 0; i < 2000; ++i) {
        const uint32_t page = random() % watched.size();
        watched[page] = true;
        randomWrites.watch(base + page * 4096 + 13, 1);
        const uint32_t begin = random() % (watched.size() * 4096);
        const uint32_t size = random() % 0x50000;
        bool overlap = false;
        if (size)
            for (size_t p = begin / 4096; p <= (begin + size - 1) / 4096 && p < watched.size(); ++p)
                overlap |= watched[p];
        const auto before = randomWrites.generation();
        randomWrites.notify(base + begin, size);
        require(randomWrites.generation() == before + overlap,
            "shader page bitmap masks differ from reference overlap");
    }
}
int main() {
    try {
        ranges(); keys(); indices(); shader_writes();
        const bool option = gfxvk::draw_option_enabled("WWHD_VK_TEST_OPTION");
        const char* value = std::getenv("WWHD_VK_TEST_OPTION");
        bool expected = value && std::string_view(value) == "1";
#ifdef __SWITCH__
        if (!value) expected = true;
#endif
        require(option == expected, "draw option default or override changed");
        std::puts("Vulkan draw CPU regressions passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
