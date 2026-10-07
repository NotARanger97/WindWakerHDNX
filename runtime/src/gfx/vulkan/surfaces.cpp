#include "mods/cemu_pack.h"
// Guest surfaces backed by Vulkan images. LatteAddrLib supplies guest tiling geometry.
#include "backend.h"
#include "settings.h"
#include "sparse_hash_memo.h"
#include "write_watch.h"
#include "address_range_index.h"
#include "draw_options.h"
#define XXH_INLINE_ALL
#include "../../../third_party/xxhash/xxhash.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "tile_walk.h"
#include "gx2/gx2.h"
#include "shaders.h"
#ifdef __SWITCH__
#include "gx2/shader_program_writes.h"
#endif
#include "gx2_texture_regs.h"
#include "runtime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);
namespace gfxvk {
// Queued payloads are uint32 words. These guest structs need no stronger alignment.
static_assert(alignof(GX2::GX2ColorBuffer) <= alignof(uint32_t));
static_assert(alignof(GX2::GX2DepthBuffer) <= alignof(uint32_t));
static_assert(alignof(GX2Surface) <= alignof(uint32_t));
// Guest layout is immutable between descriptor changes. Invalidation ranges,
// sparse checks and uploads share this metadata; image scale/layout are unrelated.
struct CachedGuestLevel {
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    uint32_t address = 0;
    bool infoValid = false, addressValid = false;
};
struct CachedGuestLayout {
    std::array<uint32_t, 10> descriptor{};
    std::vector<CachedGuestLevel> levels;
};
static CachedGuestLevel& guest_level(const Surface* s, uint32_t level) {
    std::array<uint32_t, 10> descriptor{s->addr,s->mipAddr,s->width,s->height,s->slices,
        s->format,s->dim,s->tileMode,s->swizzle,s->mips};
    // Surface copies may share unchanged guest geometry. Descriptor changes
    // get a fresh cache, preserving metadata belonging to the original surface.
    if (!s->guestLayout || s->guestLayout->descriptor != descriptor ||
        s->guestLayout->levels.empty()) {
        auto layout = std::make_shared<CachedGuestLayout>();
        layout->descriptor = descriptor;
        layout->levels.resize(s->mips);
        s->guestLayout = std::move(layout);
    }
    if (level >= s->guestLayout->levels.size()) throw std::runtime_error("GX2 mip level exceeds surface");
    return s->guestLayout->levels[level];
}
static const LatteAddrLib::AddrSurfaceInfo_OUT& guest_info(const Surface* s, uint32_t level) {
    auto& cached = guest_level(s, level);
    if (!cached.infoValid) {
        LatteAddrLib::GX2CalculateSurfaceInfo(static_cast<Latte::E_GX2SURFFMT>(s->format),
            s->width,s->height,s->slices,static_cast<Latte::E_DIM>(s->dim),
            Latte::MakeGX2TileMode(static_cast<Latte::E_HWTILEMODE>(s->tileMode)),0,level,&cached.info);
        cached.infoValid = true;
    }
    return cached.info;
}
static void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan surfaces: ")+what+" failed ("+std::to_string(result)+")");
}
static uint64_t content_hash(const uint8_t* p, size_t n) {
    // Full-byte coverage, including unaligned guest ranges and the final tail.
    // Only transient surface hashes use XXH3; disk cache checksums stay stable.
    return XXH3_64bits(p, n);
}
uint64_t next_write_seq() { static uint64_t seq=0; return ++seq; }
static float parse_scale(const char* e) {
    float f = e ? (float)atof(e) : 1.0f;
    return std::clamp(f > 0 ? f : 1.0f, 1.0f, 4.0f);
}
static std::atomic<float> g_res_requested{parse_scale(getenv("WWHD_RES_SCALE"))};
static float g_res_frame = g_res_requested.load();  // render thread: the factor for this frame
static void latch_aspect();
float res_scale() { return g_res_frame; }
float requested_res_scale() { return g_res_requested.load(std::memory_order_relaxed); }
void set_res_scale(float f) {
    g_res_requested = std::clamp(std::isfinite(f) ? f : 1.0f, 1.0f, 4.0f);
    LOG("[gfx] internal resolution %gx", g_res_requested.load());
}
void latch_res_scale() {
    // test aid: WWHD_RES_SCALE_AT=frame:factor,... switches the factor at those frames
    static std::vector<std::pair<uint64_t, float>> at = [] {
        std::vector<std::pair<uint64_t, float>> v;
        if (const char* e = getenv("WWHD_RES_SCALE_AT"))
            for (char* p = (char*)e; *p;) {
                uint64_t f = strtoull(p, &p, 10);
                if (*p++ != ':') break;
                v.push_back({f, (float)strtod(p, &p)});
                while (*p == ',') p++;
            }
        return v;
    }();
    for (auto& [f, v] : at)
        if (R.frame == f) set_res_scale(v);
    g_res_frame = g_res_requested.load(std::memory_order_relaxed);
    latch_aspect();
}

// ---------------------------------------------------------------- aspect ratio
// As the Metal renderer (metal_surfaces.mm): at another aspect ratio (aspect.cpp) the game's
// projections are widened (or made taller) and every screen-shaped render target is allocated that
// much wider (taller) than its guest size: draws keep their guest viewports, which cover the whole
// image, so the picture comes out at the new shape. kx/ky: image pixels per guest pixel on top of
// the internal resolution, (A / (16/9), 1) for wider screens, (1, (16/9) / A) for narrower ones.
// Latched at the frame boundary like the resolution.
static std::atomic<float> g_aspect_requested{16.0f / 9.0f};
static float g_aspect_kx = 1.0f, g_aspect_ky = 1.0f;  // render thread: factors for this frame
void set_frame_aspect(float a) { g_aspect_requested.store(a, std::memory_order_relaxed); }
static void latch_aspect() {
    float a = g_aspect_requested.load(std::memory_order_relaxed), base = 16.0f / 9.0f;
    float kx = a >= base ? a / base : 1.0f, ky = a >= base ? 1.0f : base / a;
    if (kx != g_aspect_kx || ky != g_aspect_ky) LOG("[gfx] aspect %.4f: screen targets x%.4f wide, x%.4f tall", a, kx, ky);
    g_aspect_kx = kx;
    g_aspect_ky = ky;
}
// the game's screen-sized buffers and their reductions (1920x1080 ... 60x33); not the GamePad's
// (854x480, shown in its own window), not shadow maps, mip chains or textures
static bool screen_shaped(uint32_t width, uint32_t height, bool compressed, uint32_t mips, uint32_t slices) {
    if (compressed || mips > 1 || slices > 1 || width < 32) return false;
    for (uint32_t w = 854, h = 480; w >= 32; w >>= 1, h >>= 1)
        if ((width == w || width == w + 1) && height == h) return false;
    float r = (float)width * 9.0f / ((float)height * 16.0f);
    return r > 0.97f && r < 1.03f;
}
bool target_aspect_factors(uint32_t w, uint32_t h, float& kx, float& ky) {
    bool on = screen_shaped(w, h, false, 1, 1) && (g_aspect_kx != 1.0f || g_aspect_ky != 1.0f);
    kx = on ? g_aspect_kx : 1.0f;
    ky = on ? g_aspect_ky : 1.0f;
    return on;
}
static void target_aspect(const Surface* s, float& kx, float& ky) {
    uint32_t width,height;
    if(!s->fmt.compressed&&s->mips==1&&mods::cemu::texture_extent(s->width,s->height,s->format,s->slices,s->tileMode,width,height)){
        kx=float(width)/s->width;ky=float(height)/s->height;return;
    }
    bool on = screen_shaped(s->width, s->height, s->fmt.compressed, s->mips, s->slices);
    kx = on ? g_aspect_kx : 1.0f;
    ky = on ? g_aspect_ky : 1.0f;
}

// the factor a render target gets. Shadow maps (depth arrays: the game's cascades) can have their
// own factor (WWHD_SHADOW_SCALE=n; default: the same as everything else).
static float target_scale(const Surface* s) {
    uint32_t width,height;
    if(!s->fmt.compressed&&s->mips==1&&mods::cemu::texture_extent(s->width,s->height,s->format,s->slices,s->tileMode,width,height))return 1.0f;
    if (s->fmt.compressed || s->mips > 1) return 1.0f;
    static const float shadow = getenv("WWHD_SHADOW_SCALE") ? parse_scale(getenv("WWHD_SHADOW_SCALE")) : 0.0f;
    if (shadow && s->isDepth && s->slices > 1) return shadow;
    return res_scale();
}


// ---------------------------------------------------------------- render targets
// CB_COLORn_BASE holds the full guest address; CB_COLORn_TILE/FRAG hold width/height (our convention).
constexpr uint32_t kDim2D = 1, kDim2DArray = 5;

Surface* color_target(const uint32_t* regs, int i, uint32_t* slice) {
    uint32_t base = regs[mmCB_COLOR0_BASE + i];
    if (!base) return nullptr;
    uint32_t size = regs[mmCB_COLOR0_SIZE + i], info = regs[mmCB_COLOR0_INFO + i];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    // our convention (GX2SetColorBuffer): TILE = width | array slices << 16, FRAG = height
    uint32_t w = regs[mmCB_COLOR0_TILE + i] & 0xFFFF, h = regs[mmCB_COLOR0_FRAG + i];
    uint32_t slices = std::max<uint32_t>(regs[mmCB_COLOR0_TILE + i] >> 16, 1);
    if (slice) *slice = slices > 1 ? std::min<uint32_t>(regs[mmCB_COLOR0_VIEW + i] & 0x7FF, slices - 1) : 0;
    static const uint32_t numberBits[8] = {0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800};
    SurfaceDesc d;
    d.addr = base;
    d.width = w ? w : pitch;
    d.height = h ? h : height;
    d.pitch = pitch;
    d.format = ((info >> 2) & 0x3F) | numberBits[(info >> 12) & 7];
    d.tileMode = (info >> 8) & 0xF;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    return find_or_create_surface(d, true);
}

Surface* depth_target(const uint32_t* regs, uint32_t* slice) {
    uint32_t base = regs[mmDB_DEPTH_BASE];
    if (!base) return nullptr;
    uint32_t slices = std::max<uint32_t>(regs[gx2::kDepthSlicesReg], 1);
    if (slice) *slice = slices > 1 ? std::min<uint32_t>(regs[mmDB_DEPTH_VIEW] & 0x7FF, slices - 1) : 0;
    uint32_t size = regs[mmDB_DEPTH_SIZE], info = regs[mmDB_DEPTH_INFO];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    uint32_t wh = regs[mmDB_HTILE_DATA_BASE];  // our convention: width << 16 | height
    static const uint32_t fmts[8] = {0, 0x005, 0, 0x011, 0, 0x811, 0x80E, 0x81C};
    SurfaceDesc d;
    d.addr = base;
    d.width = wh ? (wh >> 16) : pitch;
    d.height = wh ? (wh & 0xFFFF) : height;
    d.pitch = pitch;
    d.format = fmts[info & 7];
    d.tileMode = (info >> 15) & 0xF;
    d.isDepth = true;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    return find_or_create_surface(d, true);
}

Surface* surface_from_color_buffer(uint32_t addr, uint32_t* firstSlice, uint32_t* numSlices) {
    return surface_from_color_payload(mem::ptr(addr), firstSlice, numSlices);
}
Surface* surface_from_color_payload(const void* payload, uint32_t* firstSlice, uint32_t* numSlices) {
    auto* cb = static_cast<const GX2::GX2ColorBuffer*>(payload);
    SurfaceDesc d;
    uint32_t slices = cb->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(cb->surface.depth, 1) : 1;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    if (firstSlice) *firstSlice = std::min<uint32_t>(cb->viewFirstSlice, slices - 1);
    if (numSlices) *numSlices = std::clamp<uint32_t>(cb->viewNumSlices, 1, slices - std::min<uint32_t>(cb->viewFirstSlice, slices - 1));
    d.addr = gx2::color_buffer_address(cb);
    d.width = std::max<uint32_t>(cb->surface.width >> cb->viewMip, 1);
    d.height = std::max<uint32_t>(cb->surface.height >> cb->viewMip, 1);
    d.pitch = cb->surface.pitch;
    d.format = (uint32_t)cb->surface.format.value();
    d.tileMode = (uint32_t)cb->surface.tileMode.value();
    return find_or_create_surface(d, true);
}

Surface* surface_from_depth_buffer(uint32_t addr, uint32_t* firstSlice, uint32_t* numSlices) {
    return surface_from_depth_payload(mem::ptr(addr), firstSlice, numSlices);
}
Surface* surface_from_depth_payload(const void* payload, uint32_t* firstSlice, uint32_t* numSlices) {
    auto* db = static_cast<const GX2::GX2DepthBuffer*>(payload);
    SurfaceDesc d;
    uint32_t slices = db->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(db->surface.depth, 1) : 1;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    if (firstSlice) *firstSlice = std::min<uint32_t>(db->viewFirstSlice, slices - 1);
    if (numSlices) *numSlices = std::clamp<uint32_t>(db->viewNumSlices, 1, slices - std::min<uint32_t>(db->viewFirstSlice, slices - 1));
    d.addr = db->surface.imagePtr;
    d.width = db->surface.width;
    d.height = db->surface.height;
    d.pitch = db->surface.pitch;
    d.format = (uint32_t)db->surface.format.value();
    d.tileMode = (uint32_t)db->surface.tileMode.value();
    d.isDepth = true;
    return find_or_create_surface(d, true);
}

// ---------------------------------------------------------------- sampled textures
static uint64_t sparse_hash(Surface* s);
uint64_t g_stat_full_checks, g_stat_uploads, g_stat_invalidates, g_stat_invalidated_surfaces;
uint64_t g_upload_ns, g_full_check_ns;  // CPU time of full texture checks and uploads (slow-frame log)

Surface* sampled_texture(const uint32_t* w, bool isDepthSampler) {
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N w1;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N w5;
    memcpy(&w0, &w[0], 4);
    memcpy(&w1, &w[1], 4);
    memcpy(&w4, &w[4], 4);
    memcpy(&w5, &w[5], 4);
    uint32_t addr = w[2] << 8, mipAddr = w[3] << 8;
    if (!addr) return nullptr;
    auto dim = w0.get_DIM();
    uint32_t pitch = (w0.get_PITCH() + 1) << 3;
    uint32_t width = w0.get_WIDTH() + 1;
    uint32_t height = w1.get_HEIGHT() + 1;
    uint32_t depth = w1.get_DEPTH();
    if (dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA ||
        dim == Latte::E_DIM::DIM_1D_ARRAY)
        depth += 1;
    else {
        if (dim == Latte::E_DIM::DIM_CUBEMAP) depth = 6 * (depth + 1);
        if (depth == 0) depth = 1;
    }
    if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY) height = 1;
    auto tileMode = w0.get_TILE_MODE();
    if (Latte::IsCompressedFormat(w1.get_DATA_FORMAT())) pitch /= 4;
    uint32_t swizzle = 0;
    if (Latte::TM_IsMacroTiled(tileMode)) {
        swizzle = addr & 0x700;
        addr &= ~0x700u;
    }
    SurfaceDesc d;
    d.addr = addr;
    d.mipAddr = mipAddr;
    d.width = width;
    d.height = height;
    d.slices = depth;
    d.pitch = pitch;
    d.mips = w5.get_LAST_LEVEL() + 1;
    d.format = (uint32_t)LatteTexture_ReconstructGX2Format(w1, w4);
    d.dim = (uint32_t)dim;
    d.tileMode = (uint32_t)tileMode;
    d.swizzle = swizzle;
    d.isDepth = isDepthSampler;
    Surface* s = find_or_create_surface(d, false);
    upload_surface(s);
    return s;
}


static void decode_level(Surface* s, uint32_t level, uint32_t base, std::vector<uint8_t>& out, uint32_t& outW,
                         uint32_t& outH, uint32_t& outSlices) {
    const FormatInfo& f = s->fmt;
    uint32_t w = std::max(s->width >> level, 1u), h = std::max(s->height >> level, 1u);
    uint32_t slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    outW = w;
    outH = h;
    outSlices = slices;

    // level geometry from the address library
    const auto& info = guest_info(s, level);
    uint32_t pitch = level == 0 && s->pitch ? s->pitch : info.pitch, height = info.height;
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    bool depthData = s->isDepth || f.convert == Convert::D24_R32F;
    uint32_t pipeSwizzle = (s->swizzle >> 8) & 1, bankSwizzle = (s->swizzle >> 9) & 3;
    // small mips of macro-tiled surfaces drop the swizzle
    out.assign((size_t)bw * bh * slices * f.hostBytesPerBlock, 0);
    const uint8_t* src = mem::ptr(base);
    // micro-tile walk (tile_walk.h): one address computation per 8x8 elements
    {
        const uint32_t B = f.bytesPerBlock;
        auto copy = [B](uint8_t* d, const uint8_t* s) {
            switch (B) {
            case 1: *d = *s; break;
            case 2: memcpy(d, s, 2); break;
            case 4: memcpy(d, s, 4); break;
            case 8: memcpy(d, s, 8); break;
            case 16: memcpy(d, s, 16); break;
            default: memcpy(d, s, B); break;
            }
        };
        const bool direct = f.convert == Convert::NONE && f.hostBytesPerBlock == B;
        std::vector<uint8_t> linear(direct ? 0 : (size_t)bw * bh * B);
        bool walked = true;
        for (uint32_t z = 0; z < slices && walked; z++) {
            uint8_t* dst = direct ? out.data() + (size_t)z * bh * bw * B : linear.data();
            walked = walk_tiled_slice(bw, bh, z, bpp, pitch, height, slices, tm, depthData, pipeSwizzle, bankSwizzle,
                                      [&](uint32_t x, uint32_t y, uint32_t off) {
                                          copy(dst + ((size_t)y * bw + x) * B, src + off);
                                      });
            if (walked && !direct)
                for (uint32_t y = 0; y < bh; y++) {
                    uint8_t* row = &out[((size_t)z * bh + y) * bw * f.hostBytesPerBlock];
                    const uint8_t* in = linear.data() + (size_t)y * bw * B;
                    if (f.convert == Convert::NONE) memcpy(row, in, (size_t)bw * B);
                    else convert_row(f.convert, in, row, bw);
                }
        }
        if (walked) return;
    }
    std::vector<uint8_t> row(bw * f.bytesPerBlock);
    for (uint32_t z = 0; z < slices; z++) {
        LatteAddrLib::CachedSurfaceAddrInfo ci;
        bool macro = Latte::TM_IsMacroTiled(tm);
        if (macro)
            LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, slices, 1, tm, depthData, pipeSwizzle, bankSwizzle);
        for (uint32_t y = 0; y < bh; y++) {
            for (uint32_t x = 0; x < bw; x++) {
                uint32_t off;
                if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, pitch, height, slices);
                else if (!macro)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, pitch, height, tm, depthData);
                else
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
                memcpy(&row[x * f.bytesPerBlock], src + off, f.bytesPerBlock);
            }
            uint8_t* dst = &out[((size_t)z * bh + y) * bw * f.hostBytesPerBlock];
            if (f.convert == Convert::NONE) memcpy(dst, row.data(), row.size());
            else convert_row(f.convert, row.data(), dst, bw);
        }
    }
}

static uint32_t mip_base(Surface* s, uint32_t level);

// Fallback change check when write tracking (write_watch.h) is unavailable.
// Sample every mip so CPU changes confined to the mip chain get the same
// immediate detection as base-level changes. Periodic full checks catch writes
// outside these samples when guest code omits a texture invalidation.
static SparseHashStats sparseStats;
SparseHashStats sparse_hash_stats() { return sparseStats; }
static uint64_t sparse_hash(Surface* s) {
    static const bool enabled = [] {
        const char* value = getenv("WWHD_VK_SPARSE_HASH_MEMO");
        return value && !strcmp(value,"1");
    }();
    static const bool collectStats = getenv("WWHD_VK_STATS") != nullptr;
    if(!enabled && !collectStats) {
        // Keep the default path's original streaming loop, without diagnostics.
        uint64_t h = 0xcbf29ce484222325ull;
        for(uint32_t level = 0; level < s->mips; ++level) {
            const auto& info = guest_info(s, level);
            const auto* bytes = mem::ptr(mip_base(s, level));
            size_t size = size_t(info.surfSize), step = std::max<size_t>((size / 256) & ~size_t(7), 8);
            size_t offset = 0;
            for(; offset < size && size - offset >= 8; offset += step) {
                uint64_t value;
                memcpy(&value, bytes + offset, 8);
                h = (h ^ value) * 0x100000001b3ull;
            }
            if(offset < size) {
                uint64_t value = 0;
                memcpy(&value, bytes + offset, size - offset);
                h = (h ^ value) * 0x100000001b3ull;
            }
        }
        return h;
    }
    if(collectStats) ++sparseStats.checks;
    if(!enabled) {
        uint64_t hash = SparseHashMemo<>::basis;
        for(uint32_t level = 0; level < s->mips; ++level) {
            const auto& info = guest_info(s, level);
            const auto counts = sparse_sample_counts(size_t(info.surfSize));
            sparseStats.sampleBytes += counts.bytes;
            sparseStats.mixerWords += counts.words;
            sparse_sample_words(mem::ptr(mip_base(s, level)), size_t(info.surfSize),
                [&](uint64_t value, size_t) {
                    hash = (hash ^ value) * 0x100000001b3ull;
                });
        }
        return hash;
    }
    static const bool largerMemo = [] {
        const char* value = getenv("WWHD_VK_SPARSE_HASH_ENTRIES");
        return value && !strcmp(value,"256");
    }();
    auto runMemo = [&](auto& memo) {
        memo.begin();
        for(uint32_t level = 0; level < s->mips; ++level) {
            const auto& info = guest_info(s, level);
            if(collectStats) sparseStats.sampleBytes += sparse_sample_counts(size_t(info.surfSize)).bytes;
            sparse_sample_words(mem::ptr(mip_base(s, level)), size_t(info.surfSize),
                [&](uint64_t value, size_t) { memo.add(value); });
        }
        SparseHashStats discarded;
        return memo.finish(reinterpret_cast<uintptr_t>(s), collectStats ? sparseStats : discarded);
    };
    if(largerMemo) {
        // Allocate only the selected larger cache; payload storage is bounded
        // to 16 MiB plus 64 KiB scratch and small identity metadata.
        static std::unique_ptr<SparseHashMemo<256>> memo(new SparseHashMemo<256>);
        return runMemo(*memo);
    }
    static SparseHashMemo<> memo;
    return runMemo(memo);
}


static uint32_t level_address(const GX2Surface* s, uint32_t level) {
    if (level == 0) return s->imagePtr;
    if (level == 1) return s->mipPtr;
    return s->mipPtr + s->mipOffset[level - 1];
}

static uint32_t element_offset(const LatteAddrLib::AddrSurfaceInfo_OUT& info, Latte::E_HWTILEMODE tm, uint32_t x, uint32_t y,
                               uint32_t slice, uint32_t bpp, uint32_t swizzle, LatteAddrLib::CachedSurfaceAddrInfo* ci, bool depth) {
    if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, slice, 0, bpp, info.pitch, info.height, info.depth);
    if (!Latte::TM_IsMacroTiled(tm))
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, slice, bpp, info.pitch, info.height, tm, depth);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, ci);
}


} // namespace gfxvk

namespace gfxvk {
namespace {
bool surface_pool_enabled() {
    static const bool enabled = draw_option_enabled("WWHD_VK_SURFACE_IMAGE_POOL");
    return enabled;
}
SurfaceImageKey image_key(const Surface& s) {
    const bool cube = s.viewType == VK_IMAGE_VIEW_TYPE_CUBE || s.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    return {s.fmt.pixel,s.extent,s.mips,s.arrayLayers,s.usage,s.imageType,
        VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0))};
}
void free_surface_allocation(const Renderer::RetiredImage& image) {
    if(image.image) {
        vkDestroyImage(R.device,image.image,nullptr);
        if(image.key.format != VK_FORMAT_UNDEFINED) ++R.surfaceImageDestroys;
    }
    if(image.memory) vkFreeMemory(R.device,image.memory,nullptr);
}
void evict_pooled_image(size_t index) {
    auto& image = R.surfaceImagePool[index];
    R.surfaceImagePoolBytes -= image.allocationBytes;
    free_surface_allocation(image);
    R.surfaceImagePool.erase(R.surfaceImagePool.begin() + index);
}
}
void recycle_surface_image(Renderer::RetiredImage image) {
    // Called only after this submission's fence: it also covers all earlier
    // uses on our single queue. Views/descriptors never survive pool admission.
    for(auto view : image.views) if(view) vkDestroyImageView(R.device,view,nullptr);
    image.views.clear();
    constexpr VkDeviceSize budget = 64ull << 20;
    constexpr size_t maxImages = 32, maxPerKey = 4;
    if(!surface_pool_enabled() || !image.image || !image.memory || !image.allocationBytes ||
       image.key.format == VK_FORMAT_UNDEFINED || image.allocationBytes > budget) {
        free_surface_allocation(image);return;
    }
    size_t matches = 0, oldest = 0;
    for(size_t i=0;i<R.surfaceImagePool.size();++i) if(R.surfaceImagePool[i].key == image.key) {
        if(!matches) oldest=i;
        ++matches;
    }
    if(matches >= maxPerKey) evict_pooled_image(oldest);
    while(!R.surfaceImagePool.empty() && (R.surfaceImagePool.size() >= maxImages ||
        R.surfaceImagePoolBytes > budget - image.allocationBytes)) evict_pooled_image(0);
    R.surfaceImagePoolBytes += image.allocationBytes;
    R.surfaceImagePool.push_back(std::move(image));
}
void reset_surface_image_pool() {
    for(auto& image : R.surfaceImagePool) free_surface_allocation(image);
    R.surfaceImagePool.clear();R.surfaceImagePoolBytes=0;
}
void create_surface_image(Surface* s, bool forRendering, VkExtent3D explicitExtent) {
    if (!s || !s->width || !s->height || !s->slices || !s->mips)
        throw std::runtime_error("Vulkan surface has empty dimensions");
    if (s->image) throw std::runtime_error("Vulkan surface image must be retired before replacement");
    if (s->fmt.pixel == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("Unsupported GX2 surface format " + std::to_string(s->format));
    auto dim=static_cast<Latte::E_DIM>(s->dim);
    if (dim==Latte::E_DIM::DIM_2D_MSAA || dim==Latte::E_DIM::DIM_2D_ARRAY_MSAA)
        throw std::runtime_error("Vulkan GX2 multisample surfaces require an explicit sample count");
    bool oneD=dim==Latte::E_DIM::DIM_1D || dim==Latte::E_DIM::DIM_1D_ARRAY;
    bool threeD=dim==Latte::E_DIM::DIM_3D;
    bool cube=dim==Latte::E_DIM::DIM_CUBEMAP;
    s->imageType=oneD?VK_IMAGE_TYPE_1D:threeD?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;
    s->arrayLayers=threeD?1:s->slices;
    s->viewType=threeD?VK_IMAGE_VIEW_TYPE_3D:oneD?(s->slices>1?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):
        cube?(s->slices>6?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE):
        (s->slices>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D);
    s->scale=forRendering&&!oneD&&!threeD&&!cube?target_scale(s):1.0f;
    s->ax=s->ay=1.0f;
    if(forRendering&&!oneD&&!threeD&&!cube)target_aspect(s,s->ax,s->ay);
    s->extent={uint32_t(std::ceil(s->width*s->scale*s->ax-0.01f)),oneD?1:uint32_t(std::ceil(s->height*s->scale*s->ay-0.01f)),threeD?s->slices:1};
    if(explicitExtent.width || explicitExtent.height || explicitExtent.depth) {
        if(s->imageType!=VK_IMAGE_TYPE_2D || !explicitExtent.width ||
           !explicitExtent.height || explicitExtent.depth!=1 ||
           explicitExtent.width>R.properties.limits.maxImageDimension2D ||
           explicitExtent.height>R.properties.limits.maxImageDimension2D)
            throw std::runtime_error("Invalid explicit private surface extent");
        s->extent=explicitExtent;
    }
    s->sx=float(s->extent.width)/s->width; s->sy=float(s->extent.height)/s->height;
    s->aspect=s->fmt.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    if(s->fmt.stencil)s->aspect|=VK_IMAGE_ASPECT_STENCIL_BIT;
    if(cube&&(s->extent.width!=s->extent.height||s->slices%6))throw std::runtime_error("Invalid GX2 cube surface dimensions");
    uint32_t maxDim=std::max({s->extent.width,s->extent.height,s->extent.depth});
    uint32_t maxMips=1; while(maxDim>1){maxDim>>=1;++maxMips;}
    if(s->mips>maxMips)throw std::runtime_error("GX2 surface requests too many mip levels");
    VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(R.physicalDevice,s->fmt.pixel,&properties);
    auto features=properties.optimalTilingFeatures;
    VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    auto attachment=s->fmt.depth?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if(forRendering)required|=attachment;
    if((features&required)!=required)throw std::runtime_error("Vulkan device lacks required features for GX2 format "+std::to_string(s->format));
    VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(!s->fmt.compressed&&(features&attachment))usage|=s->fmt.depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.flags=(cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0)|VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    imageInfo.imageType=s->imageType; imageInfo.format=s->fmt.pixel; imageInfo.extent=s->extent;
    imageInfo.mipLevels=s->mips;imageInfo.arrayLayers=s->arrayLayers;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.usage=usage;s->usage=usage;imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    try {
        const auto key = image_key(*s);
        auto pooled = std::find_if(R.surfaceImagePool.begin(),R.surfaceImagePool.end(),
            [&](const auto& image) { return image.key == key; });
        if(pooled != R.surfaceImagePool.end()) {
            s->image=pooled->image;s->memory=pooled->memory;s->allocationBytes=pooled->allocationBytes;
            R.surfaceImagePoolBytes -= pooled->allocationBytes;
            R.surfaceImagePool.erase(pooled);++R.surfaceImagePoolHits;
        } else {
            check_vk(vkCreateImage(R.device,&imageInfo,nullptr,&s->image),"create image");
            ++R.surfaceImageCreates;
            VkMemoryRequirements needs{};vkGetImageMemoryRequirements(R.device,s->image,&needs);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=needs.size;
            allocation.memoryTypeIndex=memory_type(needs.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check_vk(vkAllocateMemory(R.device,&allocation,nullptr,&s->memory),"allocate image memory");
            check_vk(vkBindImageMemory(R.device,s->image,s->memory,0),"bind image memory");
            s->allocationBytes=needs.size;
        }
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};viewInfo.image=s->image;
        viewInfo.viewType=s->viewType;viewInfo.format=s->fmt.pixel;
        viewInfo.subresourceRange={VkImageAspectFlags(s->fmt.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,s->mips,0,s->arrayLayers};
        check_vk(vkCreateImageView(R.device,&viewInfo,nullptr,&s->view),"create sampling image view");
        // Pool entries have no guest content identity. Discard all old texels;
        // the caller uploads, copies, clears or renders just as for a new image.
        s->layout=VK_IMAGE_LAYOUT_UNDEFINED;
    } catch(...) {
        if(s->view)vkDestroyImageView(R.device,s->view,nullptr);
        if(s->image){vkDestroyImage(R.device,s->image,nullptr);++R.surfaceImageDestroys;}
        if(s->memory)vkFreeMemory(R.device,s->memory,nullptr);
        s->view=VK_NULL_HANDLE;s->image=VK_NULL_HANDLE;s->memory=VK_NULL_HANDLE;
        s->allocationBytes=0;
        throw;
    }
}
void destroy_surface_image(Surface* s) {
    if(!s||!s->image)return;
    ++R.surfaceEpoch;++R.viewEpoch;
    auto views=std::move(s->layerViews);if(s->view)views.push_back(s->view);
    for(auto& [key,view]:s->sampledViews)if(view)views.push_back(view);s->sampledViews.clear();
    defer_surface_image(s->image,s->memory,std::move(views),image_key(*s),s->allocationBytes);
    s->image=VK_NULL_HANDLE;s->memory=VK_NULL_HANDLE;s->view=VK_NULL_HANDLE;s->layout=VK_IMAGE_LAYOUT_UNDEFINED;
    s->layerViews.clear();
    s->allocationBytes=0;
}
VkImageView layer_view(Surface* s,uint32_t layer) {
    if(!s||!s->image||layer>=s->arrayLayers)throw std::runtime_error("Vulkan attachment layer is out of range");
    if(s->imageType==VK_IMAGE_TYPE_3D)throw std::runtime_error("Rendering a GX2 volume slice is unsupported");
    if(s->layerViews.size()<s->arrayLayers)s->layerViews.resize(s->arrayLayers,VK_NULL_HANDLE);
    if(!s->layerViews[layer]) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};info.image=s->image;
        info.viewType=s->imageType==VK_IMAGE_TYPE_1D?VK_IMAGE_VIEW_TYPE_1D:VK_IMAGE_VIEW_TYPE_2D;info.format=s->fmt.pixel;
        info.subresourceRange={s->aspect,0,1,layer,1};
        check_vk(vkCreateImageView(R.device,&info,nullptr,&s->layerViews[layer]),"create attachment layer view");
    }
    return s->layerViews[layer];
}
VkImageView sampled_texture_view(Surface* s,const uint32_t* texWords) {
    if(!s||!s->image||!texWords)throw std::runtime_error("Vulkan sampled view requires a surface and texture descriptor");
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    memcpy(&w0,texWords,4);memcpy(&w4,texWords+4,4);
    auto dim=w0.get_DIM();VkImageViewType type;
    switch(dim) {
    case Latte::E_DIM::DIM_1D:type=VK_IMAGE_VIEW_TYPE_1D;break;
    case Latte::E_DIM::DIM_1D_ARRAY:type=VK_IMAGE_VIEW_TYPE_1D_ARRAY;break;
    case Latte::E_DIM::DIM_2D:type=VK_IMAGE_VIEW_TYPE_2D;break;
    case Latte::E_DIM::DIM_2D_ARRAY:type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
    case Latte::E_DIM::DIM_3D:type=VK_IMAGE_VIEW_TYPE_3D;break;
    case Latte::E_DIM::DIM_CUBEMAP:type=s->arrayLayers>6?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE;break;
    default:throw std::runtime_error("Unsupported Vulkan sampled texture dimension");
    }
    bool oneD=type==VK_IMAGE_VIEW_TYPE_1D||type==VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    bool threeD=type==VK_IMAGE_VIEW_TYPE_3D;
    bool cube=type==VK_IMAGE_VIEW_TYPE_CUBE||type==VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    if((oneD&&s->imageType!=VK_IMAGE_TYPE_1D)||(threeD!=(s->imageType==VK_IMAGE_TYPE_3D))||(!oneD&&!threeD&&s->imageType!=VK_IMAGE_TYPE_2D))
        throw std::runtime_error("Vulkan sampled texture dimension does not match the backing image");
    if(cube&&(s->viewType!=VK_IMAGE_VIEW_TYPE_CUBE&&s->viewType!=VK_IMAGE_VIEW_TYPE_CUBE_ARRAY))
        throw std::runtime_error("Vulkan sampled cube requires a cube-compatible backing image");
    uint32_t selectors[4]={uint32_t(w4.get_DST_SEL_X()),uint32_t(w4.get_DST_SEL_Y()),uint32_t(w4.get_DST_SEL_Z()),uint32_t(w4.get_DST_SEL_W())};
    static const VkComponentSwizzle mapping[8]={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO};
    uint32_t key=uint32_t(type)<<12;for(unsigned i=0;i<4;++i)key|=(s->fmt.depth?i:selectors[i])<<(i*3);
    auto found=s->sampledViews.find(key);if(found!=s->sampledViews.end())return found->second;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};info.image=s->image;info.viewType=type;info.format=s->fmt.pixel;
    // Comparison samplers require a depth-only, identity-component view.
    if(!s->fmt.depth)info.components={mapping[selectors[0]],mapping[selectors[1]],mapping[selectors[2]],mapping[selectors[3]]};
    uint32_t layers=(type==VK_IMAGE_VIEW_TYPE_1D||type==VK_IMAGE_VIEW_TYPE_2D||threeD)?1:type==VK_IMAGE_VIEW_TYPE_CUBE?6:s->arrayLayers;
    info.subresourceRange={VkImageAspectFlags(s->fmt.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,s->mips,0,layers};
    VkImageView view=VK_NULL_HANDLE;check_vk(vkCreateImageView(R.device,&info,nullptr,&view),"create sampled texture view");s->sampledViews.emplace(key,view);return view;
}
void resample(Surface* src,Surface* dst,uint32_t slices,float uMax,float vMax,uint32_t dstW,uint32_t dstH) {
    if(!src||!dst||!src->image||!dst->image)throw std::runtime_error("Vulkan resample requires allocated surfaces");
    if(src->image==dst->image)throw std::runtime_error("Vulkan resample source and destination alias");
    if(src->fmt.pixel!=dst->fmt.pixel||src->imageType!=dst->imageType||src->fmt.compressed)
        throw std::runtime_error("Vulkan resample requires matching uncompressed surface formats and dimensions");
    VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(R.physicalDevice,src->fmt.pixel,&properties);
    auto features=properties.optimalTilingFeatures;
    if(!(features&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(features&VK_FORMAT_FEATURE_BLIT_DST_BIT))
        throw std::runtime_error("Vulkan device cannot blit this surface format");
    if(slices>src->arrayLayers||slices>dst->arrayLayers)throw std::runtime_error("Vulkan resample layer range exceeds surface");
    if(!dstW)dstW=dst->extent.width;if(!dstH)dstH=dst->extent.height;
    if(dstW>dst->extent.width||dstH>dst->extent.height||!(uMax>0&&uMax<=1&&vMax>0&&vMax<=1))
        throw std::runtime_error("Invalid Vulkan resample extent");
    end_encoder();transition_image(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    transition_image(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit region{};region.srcSubresource={VkImageAspectFlags(src->fmt.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,0,slices};
    region.dstSubresource=region.srcSubresource;
    region.srcOffsets[1]={int32_t(std::max(1u,uint32_t(std::lround(src->extent.width*uMax)))),int32_t(std::max(1u,uint32_t(std::lround(src->extent.height*vMax)))),int32_t(src->extent.depth)};
    region.dstOffsets[1]={int32_t(dstW),int32_t(dstH),int32_t(dst->extent.depth)};
    VkFilter filter=!src->fmt.depth&&src->fmt.kind==FormatInfo::FLOAT&&(features&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    vkCmdBlitImage(command_buffer(),src->image,src->layout,dst->image,dst->layout,1,&region,filter);
    if(src->fmt.stencil) {
        region.srcSubresource.aspectMask=region.dstSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdBlitImage(command_buffer(),src->image,src->layout,dst->image,dst->layout,1,&region,VK_FILTER_NEAREST);
    }
    transition_image(src,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition_image(dst,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
static Surface* rescale(Surface* s) {
    float wanted=target_scale(s),ax,ay;
    target_aspect(s,ax,ay);
    auto attachment=s->fmt.depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(s->scale==wanted&&s->ax==ax&&s->ay==ay&&(s->usage&attachment))return s;
    if(s->imageType!=VK_IMAGE_TYPE_2D)throw std::runtime_error("Vulkan internal-resolution scaling requires a 2D render target");
    Surface replacement;
    replacement.width=s->width;replacement.height=s->height;replacement.slices=s->slices;replacement.mips=s->mips;
    replacement.dim=s->dim;replacement.format=s->format;replacement.isDepth=s->isDepth;replacement.fmt=s->fmt;
    create_surface_image(&replacement,true);
    try { resample(s,&replacement,s->arrayLayers,1,1,0,0); }
    catch(...) { destroy_surface_image(&replacement);throw; }
    destroy_surface_image(s);
    s->image=replacement.image;s->memory=replacement.memory;s->view=replacement.view;
    s->allocationBytes=replacement.allocationBytes;
    s->extent=replacement.extent;s->layout=replacement.layout;s->usage=replacement.usage;s->scale=replacement.scale;s->ax=replacement.ax;s->ay=replacement.ay;s->sx=replacement.sx;s->sy=replacement.sy;
    forget_texture_views();return s;
}
Surface* find_or_create_surface(const SurfaceDesc& descriptor,bool forRendering) {
    SurfaceDesc d = descriptor;
    d.width=std::max(d.width,1u);d.height=std::max(d.height,1u);d.slices=std::max(d.slices,1u);
    d.mips=forRendering?1:std::max(d.mips,1u);
    if(forRendering)d.mipAddr=0;
    if(!d.addr)return nullptr;
    // Macro-tiled register addresses include the same swizzle bits that the
    // texture descriptor decodes separately. All owners use the real base.
    if(Latte::TM_IsMacroTiled(static_cast<Latte::E_HWTILEMODE>(d.tileMode))) {
        d.swizzle |= d.addr & 0x700;
        d.addr &= ~0x700u;
    }
    // Memo of lookups that found exactly one surface at the address. R.surfaces only grows, so
    // while nothing was inserted (surfaceInserts) the range is still that one surface, and the
    // selection below depends only on its gpuWritten flag, checked on every hit.
    struct Memo { SurfaceDesc d; bool forRendering; bool gpuWritten; uint64_t inserts; Surface* s; };
    static std::array<Memo, 128> memos{};
    static uint64_t surfaceInserts = 1;
    auto& memo = memos[(d.addr >> 8 ^ d.addr >> 17 ^ d.format ^ (forRendering ? 0x55 : 0)) & 127];
    if (memo.inserts == surfaceInserts && memo.forRendering == forRendering &&
        !std::memcmp(&memo.d, &d, offsetof(SurfaceDesc, isDepth)) && memo.d.isDepth == d.isDepth && memo.s->gpuWritten == memo.gpuWritten)
        return forRendering ? rescale(memo.s) : memo.s;
    auto range=R.surfaces.equal_range(d.addr);
    const bool single = range.first != range.second && std::next(range.first) == range.second;
    auto remember = [&](Surface* result) {
        if (single && result == range.first->second.get())
            memo = {d, forRendering, result->gpuWritten, surfaceInserts, result};
        return result;
    };
    Surface* exact=nullptr;Surface* rendered=nullptr;
    auto score=[&](Surface* s){return std::make_tuple(s->width==d.width&&s->height==d.height,s->slices==d.slices,s->writeSeq);};
    auto consider=[&](Surface* s){if(!rendered||score(s)>score(rendered))rendered=s;};
    for(auto it=range.first;it!=range.second;++it) {
        auto* s=it->second.get();
        if(!forRendering&&s->isDepth&&!d.isDepth&&s->gpuWritten&&s->width==d.width&&s->height==d.height)consider(s);
        if(s->isDepth!=d.isDepth)continue;
        const bool sameShape=s->width==d.width&&s->height==d.height&&s->format==d.format&&s->slices==d.slices;
        const bool sameIdentity=sameShape&&s->mipAddr==d.mipAddr&&s->pitch==d.pitch&&s->mips==d.mips&&
            s->dim==d.dim&&s->tileMode==d.tileMode&&s->swizzle==d.swizzle;
        // GPU-written aliases are deliberately separate from identity: their
        // contents live on the GPU, and guest bytes cannot reconstruct them.
        // Preserve the existing render-target/texture alias selection below.
        if(sameIdentity || (sameShape&&s->gpuWritten)) {
            if(forRendering)return remember(rescale(s));
            if(!exact||s->writeSeq>exact->writeSeq)exact=s;
        } else if(!forRendering&&s->gpuWritten&&(s->format&0x3f)==(d.format&0x3f))consider(s);
    }
    if(exact&&(exact->gpuWritten||!rendered||exact->writeSeq>rendered->writeSeq))return remember(exact);
    if(rendered)return remember(rendered);if(exact)return remember(exact);
    ++surfaceInserts;
    auto s=std::make_unique<Surface>();
    s->addr=d.addr;s->mipAddr=d.mipAddr;s->width=d.width;s->height=d.height;
    s->slices=d.slices;s->pitch=d.pitch;s->mips=d.mips;
    s->format=d.format;s->dim=d.dim;s->tileMode=d.tileMode;s->swizzle=d.swizzle;s->isDepth=d.isDepth;
    s->fmt=format_info(d.format,d.isDepth);create_surface_image(s.get(),forRendering);
    if(range.first != range.second) {
        s->addressState=range.first->second->addressState;
        s->addressState->ambiguous=true;
        ++s->addressState->generation;
    } else s->addressState=std::make_shared<SurfaceAddressState>();
    ++R.surfaceEpoch;
    auto* raw=s.get();R.surfaces.emplace(d.addr,std::move(s));return raw;
}
static uint32_t mip_base(Surface* s,uint32_t level) {
    if(!level)return s->addr;
    if(!s->mipAddr)throw std::runtime_error("GX2 texture mip chain address is missing");
    if(level==1)return s->mipAddr;
    auto& cached = guest_level(s, level);
    if (cached.addressValid) return cached.address;
    uint32_t address=0,size=0;sint32 sub=0;
    LatteAddrLib::CalculateMipAndSliceAddr(s->addr,s->mipAddr,static_cast<Latte::E_GX2SURFFMT>(s->format),s->width,s->height,s->slices,
        static_cast<Latte::E_DIM>(s->dim),static_cast<Latte::E_HWTILEMODE>(s->tileMode),s->swizzle,0,level,0,&address,&size,&sub);
    cached.address=address;cached.addressValid=true;
    return cached.address;
}
static AddressRangeIndex<Surface*> surfaceRanges;
static std::unordered_set<Surface*> indexedSurfaces;
static void publish_surface_ranges(Surface* s) {
    if (indexedSurfaces.contains(s)) return;
    // Private feedback/AO images are not owned by the guest surface cache and
    // may be destroyed independently. Never retain their pointers here.
    const auto owners = R.surfaces.equal_range(s->addr);
    if (std::none_of(owners.first, owners.second,
        [&](const auto& owner) { return owner.second.get() == s; })) return;
    // Cached guest descriptors are immutable after find_or_create_surface.
    // dataSize is published by the first full check before reaching here, so
    // its base interval and the address-library mip intervals stay valid.
    const uint64_t bytes = std::max<uint64_t>(s->dataSize,
        uint64_t(s->pitch) * s->height * s->fmt.bytesPerBlock);
    surfaceRanges.add(s->addr, uint64_t(s->addr) + bytes, s);
#ifdef __SWITCH__
    gx2::texturePageWrites.watch(s->addr, bytes);
#endif
    if (s->mipAddr)
        for (uint32_t level = 1; level < s->mips; ++level) {
            const uint64_t mip = mip_base(s, level);
            surfaceRanges.add(mip, mip + guest_info(s, level).surfSize, s);
#ifdef __SWITCH__
            gx2::texturePageWrites.watch(uint32_t(mip), guest_info(s, level).surfSize);
#endif
        }
    indexedSurfaces.insert(s);
}
#ifdef __SWITCH__
// Pages of s written since the last sweep (the sweep turns older writes into pagesWritten).
static bool page_writes_since_sweep(Surface* s) {
    const uint64_t bytes = std::max<uint64_t>(s->dataSize, uint64_t(s->pitch) * s->height * s->fmt.bytesPerBlock);
    if (gx2::texturePageWrites.written_in(s->addr, bytes)) return true;
    if (s->mipAddr)
        for (uint32_t level = 1; level < s->mips; ++level)
            if (gx2::texturePageWrites.written_in(mip_base(s, level), guest_info(s, level).surfSize)) return true;
    return false;
}
#endif
// Frame boundary: textures on pages flushed since the last sweep get one check at their next use.
void sweep_texture_page_writes() {
#ifdef __SWITCH__
    gx2::texturePageWrites.take_written([](uint32_t address, uint64_t size) {
        surfaceRanges.overlaps(address, uint64_t(address) + size, [](Surface* s) { s->pagesWritten = true; });
    });
#endif
}
void upload_surface(Surface* s) {
    if(!s||!s->image||s->gpuWritten)return;
    // All callers, including render attachments, share the once-per-frame
    // check. Invalidation resets lastCheckedFrame for writes within a frame.
    if (s->lastCheckedFrame == R.frame) return;
    s->lastCheckedFrame = R.frame;
    // Has the CPU changed it since the last check? Exact with write tracking (write_watch.h): the
    // pages of every level were write-protected at that check, so any write since (guest code, HLE
    // copies, a save-state restore) has stamped them. Changes the game announces (GX2Invalidate on the
    // range, GX2CopySurface into it, save-state loads) set dirty. Without write tracking (page
    // protection unavailable on the host, as on the Switch): sampled words of every level each frame
    // plus a full check every 64 frames, which can show a changed texture late; the Switch narrows
    // both with the pages the game flushed from its data cache (gx2::texturePageWrites).
    uint32_t levels=s->mips;
    std::array<std::pair<uint32_t,uint32_t>,16> ranges{};
    if(levels>ranges.size())throw std::runtime_error("GX2 texture has too many mip levels");
    for(uint32_t level=0;level<levels;++level) {
        uint32_t base=mip_base(s,level);
        ranges[level]={base,uint32_t(std::min<uint64_t>(guest_info(s,level).surfSize,0x100000000ull-base))};
    }
    s->dataSize=ranges[0].second;
    bool full = s->dirty || !s->watched;
    if (wwatch::active()) {
        for(uint32_t level=0;level<levels&&!full;++level)full=wwatch::written_since(ranges[level].first,ranges[level].second,s->watchStamp);
        if (!full) return;
        // arm before reading: a write from now on faults and stamps the pages after this stamp
        uint64_t stamp=~0ull;
        for(uint32_t level=0;level<levels;++level)stamp=std::min(stamp,wwatch::arm(ranges[level].first,ranges[level].second));
        s->watchStamp=stamp;
    } else {
        bool periodic = ((R.frame + (s->addr >> 12)) & 63) == 0;
#ifdef __SWITCH__
        // The periodic full hash (a multi-ms spike for a large texture) catches partial writes the sparse
        // samples miss; only textures with flushed pages since their last full check can have any.
        periodic = periodic && s->fullCheckPending;
#endif
        full = full || periodic;
#ifdef __SWITCH__
        // Pages watched since this surface's first full check (publish_surface_ranges) and not
        // flushed since: the guest bytes the GPU may see are unchanged. The sparse hash still runs
        // every 8th frame as a backstop for host-side writes that bypass the flush notification.
        if (!full) {
            const bool written = s->pagesWritten || page_writes_since_sweep(s);
            s->pagesWritten = false;
            if (written) s->fullCheckPending = true;
            if (!written && ((R.frame + (s->addr >> 12)) & 7) != 0) return;
        }
#endif
        uint64_t sparse = sparse_hash(s);
        if (!full && sparse == s->sparseHash) return;
        s->sparseHash = sparse;
    }
    s->watched = true;
    ++g_stat_full_checks;
    s->fullCheckPending = false;
    const auto checkStart = std::chrono::steady_clock::now();
    // Watch the pages before reading them, so a flush during the hash is not lost.
    publish_surface_ranges(s);
    uint64_t hash=1469598103934665603ull;
    for(uint32_t level=0;level<levels;++level)
        hash=(hash^content_hash(mem::ptr(ranges[level].first),size_t(ranges[level].second)))*1099511628211ull;
    const auto checkEnd = std::chrono::steady_clock::now();
    g_full_check_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(checkEnd - checkStart).count();
    if(!s->dirty&&hash==s->contentHash)return;
    struct UploadTimer { std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~UploadTimer() { g_upload_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); } } uploadTimer;
    end_encoder();transition_image(s,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    for(uint32_t level=0;level<s->mips;++level) {
        std::vector<uint8_t> data;uint32_t w,h,slices;decode_level(s,level,mip_base(s,level),data,w,h,slices);
        std::vector<VkBufferImageCopy> copies;std::vector<uint8_t> packed;
        bool threeD=s->imageType==VK_IMAGE_TYPE_3D;
        uint32_t layers=threeD?1:slices;
        if(s->fmt.stencil) {
            // Vulkan buffer/image copies use separate tightly packed depth and stencil aspects.
            size_t count=size_t(w)*h*slices;
            packed.resize(count*5);
            for(size_t i=0;i<count;++i) {memcpy(packed.data()+i*4,data.data()+i*8,4);packed[count*4+i]=data[i*8+4];}
            VkBufferImageCopy depth{};depth.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,level,0,layers};depth.imageExtent={w,h,threeD?slices:1};
            copies.push_back(depth);auto stencil=depth;stencil.bufferOffset=count*4;stencil.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;copies.push_back(stencil);
        } else {
            packed=std::move(data);
            VkBufferImageCopy copy{};copy.imageSubresource={s->aspect,level,0,layers};copy.imageExtent={w,h,threeD?slices:1};copies.push_back(copy);
        }
        // Color/compressed offsets must align to the host texel block size;
        // depth/stencil offsets require four bytes (including the stencil plane).
        auto staging=allocate_upload(packed.size(),std::max(4u,s->fmt.hostBytesPerBlock));
        memcpy(staging.mapped,packed.data(),packed.size());
        for(auto& copy:copies)copy.bufferOffset+=staging.offset;
        vkCmdCopyBufferToImage(command_buffer(),staging.buffer,s->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
    }
    transition_image(s,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s->contentHash=hash;s->writeSeq=next_write_seq();s->dirty=false;++g_stat_uploads;
    if(s->addressState && s->addressState->ambiguous) ++s->addressState->generation;
}
void clear_color(const uint32_t*,uint32_t cb,const float rgba[4]) {
    clear_color_payload(mem::ptr(cb), rgba);
}
void clear_color_payload(const void* cb,const float rgba[4]) {
    uint32_t first,num;auto* s=surface_from_color_payload(cb,&first,&num);if(!s)return;
    if(s->fmt.depth||s->fmt.compressed)throw std::runtime_error("GX2 clear color requires an uncompressed color surface");
    VkClearColorValue value{};
    for(unsigned i=0;i<4;++i) {
        double integerValue = std::isnan(rgba[i]) ? 0.0 : double(rgba[i]);
        if(s->fmt.kind==FormatInfo::UINT)value.uint32[i]=uint32_t(std::clamp(integerValue,0.0,4294967295.0));
        else if(s->fmt.kind==FormatInfo::SINT)value.int32[i]=int32_t(std::clamp(integerValue,-2147483648.0,2147483647.0));
        else value.float32[i]=rgba[i];
    }
    end_encoder();transition_image(s,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,first,num};
    vkCmdClearColorImage(command_buffer(),s->image,s->layout,&value,1,&range);mark_gpu_written(s);
}
void clear_depth_stencil(const uint32_t*,uint32_t db,float depth,uint32_t stencil,uint32_t flags) {
    clear_depth_stencil_payload(mem::ptr(db), depth, stencil, flags);
}
void clear_depth_stencil_payload(const void* db,float depth,uint32_t stencil,uint32_t flags) {
    uint32_t first,num;auto* s=surface_from_depth_payload(db,&first,&num);if(!s)return;
    VkImageAspectFlags aspects=0;if(flags&1)aspects|=VK_IMAGE_ASPECT_DEPTH_BIT;if((flags&2)&&s->fmt.stencil)aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;
    if(!aspects)return;
    if(aspects&VK_IMAGE_ASPECT_DEPTH_BIT)prepass_depth_cleared(s);
    end_encoder();transition_image(s,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearDepthStencilValue value{depth,stencil};VkImageSubresourceRange range{aspects,0,1,first,num};
    vkCmdClearDepthStencilImage(command_buffer(),s->image,s->layout,&value,1,&range);mark_gpu_written(s);
}
void copy_surface_payload(const void* src,uint32_t srcMip,uint32_t srcSlice,const void* dstPayload,uint32_t dstMip,uint32_t dstSlice) {
    auto* s=static_cast<const GX2Surface*>(src);auto* d=static_cast<const GX2Surface*>(dstPayload);
    if(srcMip>=uint32_t(s->numLevels)||dstMip>=uint32_t(d->numLevels))throw std::runtime_error("GX2CopySurface mip is out of range");
    uint32_t sbase=level_address(s,srcMip),dbase=level_address(d,dstMip);
    uint32_t w=std::max<uint32_t>(uint32_t(s->width)>>srcMip,1),h=std::max<uint32_t>(uint32_t(s->height)>>srcMip,1);
    uint32_t dw=std::max<uint32_t>(uint32_t(d->width)>>dstMip,1),dh=std::max<uint32_t>(uint32_t(d->height)>>dstMip,1);
    uint32_t cw=std::min(w,dw),ch=std::min(h,dh);
    Surface* gpuSrc=nullptr;uint32_t gpuLevel=0;
    for(auto& [addr,image]:R.surfaces) {
        if(!image->gpuWritten)continue;
        if(addr==sbase&&image->width==w&&image->height==h) {if(!gpuSrc||image->writeSeq>gpuSrc->writeSeq){gpuSrc=image.get();gpuLevel=0;}}
        else if(addr==uint32_t(s->imagePtr)&&image->mips>srcMip&&std::max(image->width>>srcMip,1u)==w&&std::max(image->height>>srcMip,1u)==h)
            if(!gpuSrc||image->writeSeq>gpuSrc->writeSeq){gpuSrc=image.get();gpuLevel=srcMip;}
    }
    if(gpuSrc) {
        if(gpuSrc->imageType==VK_IMAGE_TYPE_3D)throw std::runtime_error("Vulkan GPU GX2CopySurface volume slices are unsupported");
        SurfaceDesc dd;dd.addr=dbase;dd.width=dw;dd.height=dh;dd.pitch=d->pitch;dd.format=uint32_t(d->format.value());
        dd.tileMode=uint32_t(d->tileMode.value());dd.swizzle=d->swizzle;dd.isDepth=gpuSrc->isDepth;
        dd.dim=uint32_t(d->dim.value());dd.slices=std::max<uint32_t>(d->depth,1);
        if(dd.dim==uint32_t(Latte::E_DIM::DIM_2D)||dd.dim==uint32_t(Latte::E_DIM::DIM_1D))dd.slices=1;
        auto* dst=find_or_create_surface(dd,true);
        if(!dst||dst->fmt.pixel!=gpuSrc->fmt.pixel)throw std::runtime_error("Vulkan GPU GX2CopySurface format conversion is unsupported");
        if(srcSlice>=gpuSrc->arrayLayers||dstSlice>=dst->arrayLayers)throw std::runtime_error("GX2CopySurface array slice is out of range");
        if(gpuSrc==dst&&gpuLevel==0&&srcSlice==dstSlice)return;
        end_encoder();
        bool self=gpuSrc->image==dst->image;
        transition_image(gpuSrc,self?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|(self?VK_ACCESS_TRANSFER_WRITE_BIT:0));
        if(!self)transition_image(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        uint32_t sw=std::min(uint32_t(std::lround(cw*gpuSrc->sx)),std::max(gpuSrc->extent.width>>gpuLevel,1u));
        uint32_t sh=std::min(uint32_t(std::lround(ch*gpuSrc->sy)),std::max(gpuSrc->extent.height>>gpuLevel,1u));
        uint32_t tw=std::min(uint32_t(std::lround(cw*dst->sx)),dst->extent.width),th=std::min(uint32_t(std::lround(ch*dst->sy)),dst->extent.height);
        for(auto aspect:{VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_ASPECT_STENCIL_BIT}) {
            if(!(gpuSrc->aspect&aspect))continue;
            if(sw==tw&&sh==th) {
                VkImageCopy region{};region.srcSubresource={VkImageAspectFlags(aspect),gpuLevel,srcSlice,1};region.dstSubresource={VkImageAspectFlags(aspect),0,dstSlice,1};region.extent={sw,sh,1};
                vkCmdCopyImage(command_buffer(),gpuSrc->image,gpuSrc->layout,dst->image,dst->layout,1,&region);
            } else {
                VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(R.physicalDevice,dst->fmt.pixel,&fp);
                if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))throw std::runtime_error("Vulkan device cannot scale GX2CopySurface format");
                VkImageBlit region{};region.srcSubresource={VkImageAspectFlags(aspect),gpuLevel,srcSlice,1};region.dstSubresource={VkImageAspectFlags(aspect),0,dstSlice,1};
                region.srcOffsets[1]={int32_t(sw),int32_t(sh),1};region.dstOffsets[1]={int32_t(tw),int32_t(th),1};
                VkFilter filter=aspect==VK_IMAGE_ASPECT_COLOR_BIT&&dst->fmt.kind==FormatInfo::FLOAT&&(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
                vkCmdBlitImage(command_buffer(),gpuSrc->image,gpuSrc->layout,dst->image,dst->layout,1,&region,filter);
            }
        }
        transition_image(gpuSrc,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);if(!self)transition_image(dst,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        mark_gpu_written(dst);return;
    }
    auto sf=format_info(uint32_t(s->format.value()),bool(uint32_t(s->format.value())&0x800));
    auto df=format_info(uint32_t(d->format.value()),bool(uint32_t(d->format.value())&0x800));
    if(sf.pixel==VK_FORMAT_UNDEFINED||df.pixel==VK_FORMAT_UNDEFINED||sf.bytesPerBlock!=df.bytesPerBlock||sf.compressed!=df.compressed)
        throw std::runtime_error("Unsupported CPU GX2CopySurface format layout");
    LatteAddrLib::AddrSurfaceInfo_OUT si{},di{};
    LatteAddrLib::GX2CalculateSurfaceInfo(s->format,s->width,s->height,s->depth,s->dim,s->tileMode,s->aa,srcMip,&si);
    LatteAddrLib::GX2CalculateSurfaceInfo(d->format,d->width,d->height,d->depth,d->dim,d->tileMode,d->aa,dstMip,&di);
    if(srcSlice>=si.depth||dstSlice>=di.depth)throw std::runtime_error("CPU GX2CopySurface slice is out of range");
    auto stm=static_cast<Latte::E_HWTILEMODE>(si.hwTileMode),dtm=static_cast<Latte::E_HWTILEMODE>(di.hwTileMode);
    uint32_t bpp=sf.bytesPerBlock*8,bw=sf.compressed?(cw+3)/4:cw,bh=sf.compressed?(ch+3)/4:ch;
    uint32_t sswz=s->swizzle,dswz=d->swizzle;LatteAddrLib::CachedSurfaceAddrInfo sci{},dci{};
    if(Latte::TM_IsMacroTiled(stm))LatteAddrLib::SetupCachedSurfaceAddrInfo(&sci,srcSlice,0,bpp,si.pitch,si.height,si.depth,1,stm,(sf.depth||sf.convert==Convert::D24_R32F),(sswz>>8)&1,(sswz>>9)&3);
    if(Latte::TM_IsMacroTiled(dtm))LatteAddrLib::SetupCachedSurfaceAddrInfo(&dci,dstSlice,0,bpp,di.pitch,di.height,di.depth,1,dtm,(df.depth||df.convert==Convert::D24_R32F),(dswz>>8)&1,(dswz>>9)&3);
    // Gather before storing so overlapping guest ranges survive a change in tiling geometry.
    std::vector<uint8_t> rows(size_t(bw)*bh*sf.bytesPerBlock);
    for(uint32_t y=0;y<bh;++y)for(uint32_t x=0;x<bw;++x) {
        uint32_t offset=element_offset(si,stm,x,y,srcSlice,bpp,sswz,&sci,(sf.depth||sf.convert==Convert::D24_R32F));
        memcpy(rows.data()+(size_t(y)*bw+x)*sf.bytesPerBlock,mem::ptr(sbase+offset),sf.bytesPerBlock);
    }
    for(uint32_t y=0;y<bh;++y)for(uint32_t x=0;x<bw;++x) {
        uint32_t offset=element_offset(di,dtm,x,y,dstSlice,bpp,dswz,&dci,(df.depth||df.convert==Convert::D24_R32F));
        memcpy(mem::ptr(dbase+offset),rows.data()+(size_t(y)*bw+x)*sf.bytesPerBlock,sf.bytesPerBlock);
    }
    for(auto& [address,image]:R.surfaces)if(address==dbase){
        if(image->gpuWritten) ++R.surfaceTargetGeneration;
        if(image->addressState) ++image->addressState->generation;
        image->gpuWritten=false;image->dirty=true;image->lastCheckedFrame=~0ull;
    }
}
void copy_surface(uint32_t src,uint32_t srcMip,uint32_t srcSlice,uint32_t dst,uint32_t dstMip,uint32_t dstSlice) {
    copy_surface_payload(mem::ptr(src),srcMip,srcSlice,mem::ptr(dst),dstMip,dstSlice);
}
void invalidate(uint32_t flags,uint32_t addr,uint32_t size) {
    if(flags & (1u << 3)) { // GX2_INVALIDATE_MODE_SHADER, including all-range invalidates.
#ifdef __SWITCH__
        // Checked now, in command order: programs on pages the game flushed (a CPU write the GPU may
        // see always goes through DCFlushRange/DCStoreRange, which marks watched pages dirty) are
        // compared with their copies; only a real change advances the shader epoch. The game
        // invalidates all of memory (flags F, size FFFFFFFF) ~13 times a frame: a global bump each
        // time discarded every program hash and shader memo.
        if(size && size<0x10000000) gx2::shaderProgramWrites.notify(addr,size);
        vk::revalidate_programs();
#else
        vk::reset_shader_memoization();
#endif
    }
    ++g_stat_invalidates;if(!(flags&2)||size>=0x10000000)return;
    uint64_t end=uint64_t(addr)+size;
    surfaceRanges.overlaps(addr, end, [&](Surface* s) {
        const uint32_t base = s->addr;
        if(s->gpuWritten||(base>=0xF4000000&&base<0xF6000000))return;
        // Already pending a fresh upload/check: another write cannot further
        // invalidate it. Count only range checks that actually reset state.
        if(s->dirty&&s->lastCheckedFrame==~0ull)return;
        s->dirty=true;s->lastCheckedFrame=~0ull;++g_stat_invalidated_surfaces;
    });
}
void ss_reset_surfaces() {
    ++R.surfaceEpoch;++R.viewEpoch;
    reset_ao_private_cache();
    surfaceRanges.clear();indexedSurfaces.clear();
    for(auto& [addr,s]:R.surfaces){s->guestLayout.reset();s->dirty=true;s->lastCheckedFrame=~0ull;}
}
} // namespace gfxvk
