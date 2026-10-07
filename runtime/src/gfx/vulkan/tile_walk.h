// Guest texture detiling in micro-tile order (surfaces.cpp decode_level, tools/tile_walk_test.cpp).
//
// The generic LatteAddrLib address functions cost ~20-50 ns per texel block; a 256x256 texture
// spent milliseconds there on the render thread. Within one 8x8 micro tile (one slice, one sample)
// only the pixel's index inside the micro tile varies: pipe, bank, bank swap, slice and macro tile
// offsets depend on x and y bits 3 and up. So each micro tile needs one generic call; the other 63
// offsets follow from the pixel index exactly as the generic functions combine it:
//   linear: offset(x) = offset(0) + x * bpp / 8
//   micro tiled: base + pixelIndex * bpp / 8
//   macro tiled: v = (macroTileOffset + sliceOffset) / 8 + pixelIndex * bpp / 8, then the low 8 bits
//   (pipe interleave) stay and the rest moves up by the 3 pipe/bank bits; pipe and bank are or-ed in.
// tools/tile_walk_test.cpp checks every element of every tile mode against the generic functions.
#pragma once
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include <cstdint>

namespace gfxvk {
// Calls emit(x, y, byteOffset) for every element (texel or compressed block) of slice z of a level
// bw x bh elements, with the offsets decode_level's per-element calls compute. Returns false (no
// calls) for layouts it does not handle: multisample or thick macro tiling (callers use the generic
// path).
template <class Emit>
bool walk_tiled_slice(uint32_t bw, uint32_t bh, uint32_t z, uint32_t bpp, uint32_t pitch, uint32_t height,
                      uint32_t slices, Latte::E_HWTILEMODE tm, bool depthData, uint32_t pipeSwizzle,
                      uint32_t bankSwizzle, Emit&& emit) {
  using namespace Latte;
  if (tm == E_HWTILEMODE::TM_LINEAR_GENERAL || tm == E_HWTILEMODE::TM_LINEAR_ALIGNED) {
    for (uint32_t y = 0; y < bh; ++y) {
      const uint32_t row = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(0, y, z, 0, bpp, pitch, height, slices);
      for (uint32_t x = 0; x < bw; ++x) emit(x, y, row + x * (bpp >> 3));
    }
    return true;
  }
  const bool macro = TM_IsMacroTiled(tm);
  if (macro && LatteAddrLib::TM_GetThickness(tm) != 1) return false;
  const uint32_t microTileType = depthData ? 1 : 0;
  uint32_t pixelBytes[64];
  for (uint32_t i = 0; i < 64; ++i)
    pixelBytes[i] = LatteAddrLib::_ComputePixelIndexWithinMicroTile(i & 7, i >> 3, z, bpp, tm, microTileType) * bpp / 8;
  LatteAddrLib::CachedSurfaceAddrInfo ci;
  if (macro) LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, slices, 1, tm, depthData, pipeSwizzle, bankSwizzle);
  for (uint32_t yt = 0; yt < bh; yt += 8) {
    for (uint32_t xt = 0; xt < bw; xt += 8) {
      const uint32_t ymax = bh - yt < 8 ? bh - yt : 8, xmax = bw - xt < 8 ? bw - xt : 8;
      if (!macro) {
        const uint32_t a0 = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(xt, yt, z, bpp, pitch, height, tm, depthData);
        const uint32_t base = a0 - pixelBytes[0];
        for (uint32_t ry = 0; ry < ymax; ++ry)
          for (uint32_t rx = 0; rx < xmax; ++rx) emit(xt + rx, yt + ry, base + pixelBytes[ry * 8 + rx]);
        continue;
      }
      // a0 = high(v0) << 3 | low(v0) | pipe/bank bits, with v0 = s + pixelBytes[0]
      const uint32_t a0 = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(xt, yt, &ci);
      const uint32_t pipeBank = a0 & 0x700u;
      const uint32_t s = (((a0 >> 11) << 8) | (a0 & 0xFFu)) - pixelBytes[0];
      for (uint32_t ry = 0; ry < ymax; ++ry)
        for (uint32_t rx = 0; rx < xmax; ++rx) {
          const uint32_t v = s + pixelBytes[ry * 8 + rx];
          emit(xt + rx, yt + ry, ((v & ~0xFFu) << 3) | (v & 0xFFu) | pipeBank);
        }
    }
  }
  return true;
}
}  // namespace gfxvk
