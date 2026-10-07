// walk_tiled_slice (gfx/vulkan/tile_walk.h) against the per-element LatteAddrLib calls of
// decode_level: every element of every tile mode, bpp, depth flag, swizzle, slice and a range of
// level sizes. Build (host):
//   g++ -std=gnu++20 -O1 -include runtime/third_party/cemu/cemu_shim.h -Iruntime/src -Iruntime/include
//       -Iruntime/third_party/cemu -Iruntime/third_party/cemu/Cafe runtime/tools/tile_walk_test.cpp
//       runtime/third_party/cemu/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib*.cpp -o tile_walk_test
#include "gfx/vulkan/tile_walk.h"
#include <cstdio>
#include <vector>

using Latte::E_HWTILEMODE;

static uint32_t generic(uint32_t x, uint32_t y, uint32_t z, uint32_t bpp, uint32_t pitch, uint32_t height,
                        uint32_t slices, E_HWTILEMODE tm, bool depth, LatteAddrLib::CachedSurfaceAddrInfo* ci) {
  if (tm == E_HWTILEMODE::TM_LINEAR_GENERAL || tm == E_HWTILEMODE::TM_LINEAR_ALIGNED)
    return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, pitch, height, slices);
  if (!Latte::TM_IsMacroTiled(tm))
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, pitch, height, tm, depth);
  return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, ci);
}

int main() {
  const E_HWTILEMODE modes[] = {
      E_HWTILEMODE::TM_LINEAR_GENERAL, E_HWTILEMODE::TM_LINEAR_ALIGNED, E_HWTILEMODE::TM_1D_TILED_THIN1,
      E_HWTILEMODE::TM_1D_TILED_THICK, E_HWTILEMODE::TM_2D_TILED_THIN1, E_HWTILEMODE::TM_2D_TILED_THIN2,
      E_HWTILEMODE::TM_2D_TILED_THIN4, E_HWTILEMODE::TM_2D_TILED_THICK, E_HWTILEMODE::TM_2B_TILED_THIN1,
      E_HWTILEMODE::TM_2B_TILED_THIN2, E_HWTILEMODE::TM_2B_TILED_THIN4, E_HWTILEMODE::TM_2B_TILED_THICK,
      E_HWTILEMODE::TM_3D_TILED_THIN1, E_HWTILEMODE::TM_3D_TILED_THICK, E_HWTILEMODE::TM_3B_TILED_THIN1,
      E_HWTILEMODE::TM_3B_TILED_THICK};
  const uint32_t bpps[] = {8, 16, 32, 64, 128};
  const uint32_t sizes[][2] = {{1, 1}, {3, 5}, {8, 8}, {13, 7}, {32, 16}, {64, 64}, {100, 37}, {256, 96}, {257, 130}};
  uint64_t checked = 0, failures = 0, walked = 0, skipped = 0;
  for (auto tm : modes)
    for (uint32_t bpp : bpps)
      for (int depth = 0; depth < 2; ++depth)
        for (uint32_t swz = 0; swz < 8; ++swz)
          for (auto& sz : sizes)
            for (uint32_t z = 0; z < 3; ++z) {
              const uint32_t bw = sz[0], bh = sz[1];
              const uint32_t pitch = (bw + 255) & ~255u, height = (bh + 127) & ~127u, slices = 4;
              const uint32_t pipeSwizzle = swz & 1, bankSwizzle = swz >> 1;
              std::vector<uint32_t> fast(size_t(bw) * bh, ~0u);
              const bool handled = gfxvk::walk_tiled_slice(bw, bh, z, bpp, pitch, height, slices, tm, depth != 0,
                                                           pipeSwizzle, bankSwizzle,
                                                           [&](uint32_t x, uint32_t y, uint32_t off) { fast[size_t(y) * bw + x] = off; });
              if (!handled) { ++skipped; continue; }
              ++walked;
              LatteAddrLib::CachedSurfaceAddrInfo ci;
              if (Latte::TM_IsMacroTiled(tm))
                LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, slices, 1, tm, depth != 0,
                                                         pipeSwizzle, bankSwizzle);
              for (uint32_t y = 0; y < bh; ++y)
                for (uint32_t x = 0; x < bw; ++x) {
                  ++checked;
                  const uint32_t want = generic(x, y, z, bpp, pitch, height, slices, tm, depth != 0, &ci);
                  if (fast[size_t(y) * bw + x] != want && ++failures <= 20)
                    std::printf("MISMATCH tm %u bpp %u depth %d swz %u size %ux%u z %u at %u,%u: %08X != %08X\n",
                                unsigned(tm), bpp, depth, swz, bw, bh, z, x, y, fast[size_t(y) * bw + x], want);
                }
            }
  std::printf("%llu layouts walked, %llu skipped, %llu elements checked, %llu mismatches\n",
              (unsigned long long)walked, (unsigned long long)skipped, (unsigned long long)checked,
              (unsigned long long)failures);
  return failures != 0;
}
