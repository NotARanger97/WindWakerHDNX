// GX2 resources: surfaces, render targets, textures, samplers, shaders,
// uniforms and vertex attribute buffers.
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "gx2_surface_calc.h"
#include "gx2_texture_regs.h"
#include "runtime.h"
#include "../aspect.h"

using namespace Latte;
using gx2::set_reg;
using gx2::set_regs;

template <typename T>
static T* gp(uint32 addr) { return (T*)mem::ptr(addr); }
static float fa(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }

// ---------------------------------------------------------------- surfaces
HLE(gx2, GX2CalcSurfaceSizeAndAlignment) { gx2calc::CalcSurfaceSizeAndAlignment(gp<GX2Surface>(arg(c, 0))); }

HLE(gx2, GX2SetSurfaceSwizzle) {
    GX2Surface* s = gp<GX2Surface>(arg(c, 0));
    s->swizzle = (s->swizzle & 0xFFFF00FF) | ((arg(c, 1) & 0xFF) << 8);
}

HLE(gx2, GX2CalcColorBufferAuxInfo) { st32(arg(c, 1), 0x1000); st32(arg(c, 2), 0x100); }
HLE(gx2, GX2CalcDepthBufferHiZInfo) { st32(arg(c, 1), 0x1000); st32(arg(c, 2), 0x100); }
HLE(gx2, GX2InitDepthBufferHiZEnable) {}

// ---------------------------------------------------------------- render targets
HLE(gx2, GX2InitColorBufferRegs) { GX2::GX2InitColorBufferRegs(gp<GX2::GX2ColorBuffer>(arg(c, 0))); }
HLE(gx2, GX2InitDepthBufferRegs) { GX2::GX2InitDepthBufferRegs(gp<GX2::GX2DepthBuffer>(arg(c, 0))); }

namespace gx2 {
uint32 color_buffer_address(const GX2::GX2ColorBuffer* cb) {
    uint32 mip = cb->viewMip;
    uint32 base = cb->surface.imagePtr;
    if (mip == 1) base = cb->surface.mipPtr;
    else if (mip > 1) base = cb->surface.mipPtr + cb->surface.mipOffset[mip - 1];
    uint32 swizzle = cb->surface.swizzle;
    if (TM_IsMacroTiled(cb->surface.tileMode) && mip < ((swizzle >> 16) & 0xFF)) base ^= (swizzle & 0xFFFF);
    return base;
}
}  // namespace gx2

HLE(gx2, GX2SetColorBuffer) {
    auto* cb = gp<GX2::GX2ColorBuffer>(arg(c, 0));
    uint32 target = arg(c, 1) & 7;
    set_reg(mmCB_COLOR0_BASE + target, gx2::color_buffer_address(cb));  // full address; our renderer's convention
    set_reg(mmCB_COLOR0_SIZE + target, cb->reg_size);
    set_reg(mmCB_COLOR0_VIEW + target, cb->reg_view);
    set_reg(mmCB_COLOR0_INFO + target, cb->reg_info);
    // our convention: the unused TILE/FRAG registers carry the view's real width (| array slices << 16) and height
    uint32 slices = cb->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32>(cb->surface.depth, 1) : 1;
    set_reg(mmCB_COLOR0_TILE + target, std::max<uint32>(cb->surface.width >> cb->viewMip, 1) | (slices << 16));
    set_reg(mmCB_COLOR0_FRAG + target, std::max<uint32>(cb->surface.height >> cb->viewMip, 1));
}

HLE(gx2, GX2SetDepthBuffer) {
    auto* db = gp<GX2::GX2DepthBuffer>(arg(c, 0));
    uint32 info = ((uint32)db->surface.tileMode.value() & 0xF) << 15;
    switch (db->surface.format.value()) {
    case E_GX2SURFFMT::D16_UNORM: info |= 1; break;
    case E_GX2SURFFMT::D24_S8_UNORM: info |= 3; break;
    case E_GX2SURFFMT::D24_S8_FLOAT: info |= 5; break;
    case E_GX2SURFFMT::D32_FLOAT: info |= 6; break;
    case E_GX2SURFFMT::D32_S8_FLOAT: info |= 7; break;
    default: break;
    }
    uint32 view = (db->viewFirstSlice & 0x7FF) | (((db->viewNumSlices + db->viewFirstSlice - 1) & 0x7FF) << 13);
    set_reg(mmDB_DEPTH_SIZE, db->reg_size);
    set_reg(mmDB_DEPTH_BASE, db->surface.imagePtr);  // full address; our renderer's convention
    set_reg(mmDB_DEPTH_INFO, info);
    set_reg(mmDB_DEPTH_VIEW, view);
    set_reg(mmDB_HTILE_DATA_BASE, (uint32)db->surface.width << 16 | (db->surface.height & 0xFFFF));  // our convention
    // our convention: unused register 0xA002 (between DB_DEPTH_VIEW and DB_DEPTH_BASE) = array slices
    set_reg(gx2::kDepthSlicesReg, db->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32>(db->surface.depth, 1) : 1);
}

// ---------------------------------------------------------------- textures and samplers
HLE(gx2, GX2InitTextureRegs) { GX2::GX2InitTextureRegs(gp<GX2::GX2Texture>(arg(c, 0))); }

static void set_texture(uint32 texAddr, uint32 baseReg, uint32 unit) {
    auto* tex = gp<GX2::GX2Texture>(texAddr);
    uint32 image = tex->surface.imagePtr, mip = tex->surface.mipPtr;
    if (!mip) mip = image;
    uint32 swizzle = tex->surface.swizzle;
    if (TM_IsMacroTiled(tex->surface.tileMode)) {
        uint32 stop = (swizzle >> 16) & 0xFF;
        if (stop > 0) image ^= (swizzle & 0xFFFF);
        if (stop > 1) mip ^= (swizzle & 0xFFFF);
    }
    uint32 w[7] = {tex->regTexWord0.value().getRawValue(), tex->regTexWord1.value().getRawValue(), image >> 8, mip >> 8,
                   tex->regTexWord4.value().getRawValue(), tex->regTexWord5.value().getRawValue(),
                   tex->regTexWord6.value().getRawValue()};
    set_regs(baseReg + unit * 7, w, 7);
}
HLE(gx2, GX2SetPixelTexture) { set_texture(arg(c, 0), REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, arg(c, 1)); }
HLE(gx2, GX2SetVertexTexture) { set_texture(arg(c, 0), REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, arg(c, 1)); }
HLE(gx2, GX2SetGeometryTexture) { set_texture(arg(c, 0), REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS, arg(c, 1)); }

using SW0 = LATTE_SQ_TEX_SAMPLER_WORD0_0;
HLE(gx2, GX2InitSampler) { GX2::GX2InitSampler(gp<GX2::GX2Sampler>(arg(c, 0)), (SW0::E_CLAMP)arg(c, 1), (SW0::E_XY_FILTER)arg(c, 2)); }
HLE(gx2, GX2InitSamplerXYFilter) {
    GX2::GX2InitSamplerXYFilter(gp<GX2::GX2Sampler>(arg(c, 0)), (SW0::E_XY_FILTER)arg(c, 1), (SW0::E_XY_FILTER)arg(c, 2), arg(c, 3));
}
HLE(gx2, GX2InitSamplerZMFilter) {
    GX2::GX2InitSamplerZMFilter(gp<GX2::GX2Sampler>(arg(c, 0)), (SW0::E_Z_FILTER)arg(c, 1), (SW0::E_Z_FILTER)arg(c, 2));
}
HLE(gx2, GX2InitSamplerLOD) { GX2::GX2InitSamplerLOD(gp<GX2::GX2Sampler>(arg(c, 0)), fa(c, 0), fa(c, 1), fa(c, 2)); }
HLE(gx2, GX2InitSamplerClamping) {
    GX2::GX2InitSamplerClamping(gp<GX2::GX2Sampler>(arg(c, 0)), (SW0::E_CLAMP)arg(c, 1), (SW0::E_CLAMP)arg(c, 2), (SW0::E_CLAMP)arg(c, 3));
}
HLE(gx2, GX2InitSamplerBorderType) { GX2::GX2InitSamplerBorderType(gp<GX2::GX2Sampler>(arg(c, 0)), (SW0::E_BORDER_COLOR_TYPE)arg(c, 1)); }
HLE(gx2, GX2InitSamplerDepthCompare) { GX2::GX2InitSamplerDepthCompare(gp<GX2::GX2Sampler>(arg(c, 0)), (SW0::E_DEPTH_COMPARE)arg(c, 1)); }

static void set_sampler(uint32 sampler, uint32 index) {
    uint32 w[3] = {ld32(sampler), ld32(sampler + 4), ld32(sampler + 8)};
    set_regs(REGADDR::SQ_TEX_SAMPLER_WORD0_0 + index * 3, w, 3);
}
HLE(gx2, GX2SetPixelSampler) { set_sampler(arg(c, 0), arg(c, 1) + SAMPLER_BASE_INDEX_PIXEL); }
HLE(gx2, GX2SetVertexSampler) { set_sampler(arg(c, 0), arg(c, 1) + SAMPLER_BASE_INDEX_VERTEX); }
HLE(gx2, GX2SetGeometrySampler) { set_sampler(arg(c, 0), arg(c, 1) + SAMPLER_BASE_INDEX_GEOMETRY); }

static void set_border(uint32 base, Cpu* c) {
    uint32 v[4] = {gx2::fbits(fa(c, 0)), gx2::fbits(fa(c, 1)), gx2::fbits(fa(c, 2)), gx2::fbits(fa(c, 3))};
    set_regs(base + arg(c, 0) * 4, v, 4);
}
HLE(gx2, GX2SetPixelSamplerBorderColor) { set_border(REGADDR::TD_PS_SAMPLER0_BORDER_RED, c); }
HLE(gx2, GX2SetVertexSamplerBorderColor) { set_border(REGADDR::TD_VS_SAMPLER0_BORDER_RED, c); }
HLE(gx2, GX2SetGeometrySamplerBorderColor) { set_border(REGADDR::TD_GS_SAMPLER0_BORDER_RED, c); }

// ---------------------------------------------------------------- shaders
HLE(gx2, GX2SetVertexShader) {
    static thread_local uint32 tmp[gx2::kNumRegs];  // only touched regs are forwarded; per thread (display lists are recorded on several cores)
    uint32 vs = arg(c, 0);
    uint32 n = std::min<uint32>(ld32(vs + 0x0C), 10), nsem = std::min<uint32>(ld32(vs + 0x40), 32);
    gx2::bind_vertex_shader_regs(tmp, vs);
    bool gs_mode = ld32(vs + 0xD8) == 2;
    if (gs_mode) {
        set_regs(mmSQ_PGM_START_ES, &tmp[mmSQ_PGM_START_ES], 2);
        set_reg(mmSQ_PGM_RESOURCES_ES, tmp[mmSQ_PGM_RESOURCES_ES]);
    } else {
        set_regs(mmSQ_PGM_START_VS, &tmp[mmSQ_PGM_START_VS], 2);
        set_reg(mmSQ_PGM_RESOURCES_VS, tmp[mmSQ_PGM_RESOURCES_VS]);
        set_reg(mmVGT_PRIMITIVEID_EN, tmp[mmVGT_PRIMITIVEID_EN]);
        set_reg(mmSPI_VS_OUT_CONFIG, tmp[mmSPI_VS_OUT_CONFIG]);
        set_reg(mmPA_CL_VS_OUT_CNTL, tmp[mmPA_CL_VS_OUT_CNTL]);
        if (n) set_regs(mmSPI_VS_OUT_ID_0, &tmp[mmSPI_VS_OUT_ID_0], n);
    }
    if (nsem) {
        set_reg(mmSQ_VTX_SEMANTIC_CLEAR, 0xFFFFFFFF);
        set_regs(mmSQ_VTX_SEMANTIC_0, &tmp[mmSQ_VTX_SEMANTIC_0], nsem);
    }
}

HLE(gx2, GX2SetPixelShader) {
    static thread_local uint32 tmp[gx2::kNumRegs];
    uint32 ps = arg(c, 0);
    uint32 n = std::min<uint32>(ld32(ps + 4 * 4), 0x20);
    gx2::bind_pixel_shader_regs(tmp, ps);
    set_regs(mmSQ_PGM_START_PS, &tmp[mmSQ_PGM_START_PS], 2);
    set_reg(mmSQ_PGM_RESOURCES_PS, tmp[mmSQ_PGM_RESOURCES_PS]);
    set_regs(mmSPI_PS_IN_CONTROL_0, &tmp[mmSPI_PS_IN_CONTROL_0], 2);
    if (n) set_regs(mmSPI_PS_INPUT_CNTL_0, &tmp[mmSPI_PS_INPUT_CNTL_0], n);
    set_reg(mmCB_SHADER_MASK, tmp[mmCB_SHADER_MASK]);
    set_reg(mmCB_SHADER_CONTROL, tmp[mmCB_SHADER_CONTROL]);
    set_reg(mmDB_SHADER_CONTROL, tmp[mmDB_SHADER_CONTROL]);
    set_reg(mmSPI_INPUT_Z, tmp[mmSPI_INPUT_Z]);
}

HLE(gx2, GX2SetGeometryShader) {
    // TODO: geometry shaders (Metal object/mesh shader path)
    static bool warned = false;
    if (!warned) { warned = true; LOG("[gx2] geometry shaders not implemented yet"); }
}
HLE(gx2, GX2SetGeometryShaderInputRingBuffer) {}
HLE(gx2, GX2SetGeometryShaderOutputRingBuffer) {}

// ---------------------------------------------------------------- fetch shader
// Our fetch "program": header (magic, count) followed by 16 bytes per attribute:
//   +0 location | buffer << 8 | indexType << 16 | endianSwap << 24
//   +4 offset, +8 format | aluDivisor << 16, +C destSel
namespace gx2 {
constexpr uint32 kFetchMagic = 0x57574653;  // "WWFS"

static LatteConst::VertexFetchEndianMode default_endian(uint32 fmt) {
    switch (fmt) {
    case 0: case 1: case 4: case 10: return LatteConst::VertexFetchEndianMode::SWAP_NONE;
    case 2: case 3: case 7: case 8: case 14: case 15: return LatteConst::VertexFetchEndianMode::SWAP_U16;
    default: return LatteConst::VertexFetchEndianMode::SWAP_U32;
    }
}
static const uint32 kRawToFetchFormat[] = {1, 2, 5, 6, 7, 0xD, 0xE, 0xF, 0x10, 0x16, 0x1A, 0x19, 0x1D, 0x1E, 0x1F, 0x20, 0x2F, 0x30, 0x22, 0x23};

LatteFetchShader* build_fetch_shader(uint32 program) {
    if (ld32(program) != kFetchMagic) return nullptr;
    uint32 n = ld32(program + 4);
    auto* fs = new LatteFetchShader();
    auto* attrs = new LatteParsedFetchShaderAttribute[n];
    std::vector<std::vector<LatteParsedFetchShaderAttribute*>> groups(16);
    for (uint32 i = 0; i < n; i++) {
        uint32 e = program + 16 + i * 16;
        uint32 w0 = ld32(e), offset = ld32(e + 4), w2 = ld32(e + 8), destSel = ld32(e + 12);
        uint32 location = w0 & 0xFF, buffer = (w0 >> 8) & 0xFF, indexType = (w0 >> 16) & 0xFF, endian = w0 >> 24;
        uint32 fmt = w2 & 0xFFFF, divisor = w2 >> 16;
        LatteParsedFetchShaderAttribute& a = attrs[i];
        a = {};
        a.attributeBufferIndex = (uint8)buffer;
        a.semanticId = (uint8)location;
        a.format = (E_HWFMT)(kRawToFetchFormat[std::min<uint32>(fmt & 0x3F, 19)] & 0x3F);
        a.nfa = (fmt & 0x800) ? 2 : (fmt & 0x100) ? 1 : 0;
        a.isSigned = (fmt & 0x200) ? 1 : 0;
        a.endianSwap = endian == 3 /* SWAP_DEFAULT */ ? default_endian(fmt & 0x3F) : (LatteConst::VertexFetchEndianMode)endian;
        a.fetchType = indexType ? LatteConst::VertexFetchType2::INSTANCE_DATA : LatteConst::VertexFetchType2::VERTEX_DATA;
        a.aluDivisor = indexType ? (sint32)std::max<uint32>(divisor, 1) : 0;
        a.offset = offset;
        for (int k = 0; k < 4; k++) a.ds[k] = (destSel >> (24 - 8 * k)) & 7;
        if (buffer < 16) groups[buffer].push_back(&a);
    }
    // attributes are grouped per buffer, contiguous in the attrs array order
    auto* sorted = new LatteParsedFetchShaderAttribute[n];
    uint32 k = 0;
    for (uint32 b = 0; b < 16; b++) {
        if (groups[b].empty()) continue;
        LatteParsedFetchShaderBufferGroup g{};
        g.attributeBufferIndex = (uint8)b;
        g.attribCount = (sint8)groups[b].size();
        g.attrib = &sorted[k];
        g.minOffset = 0xFFFFFFFF;
        for (auto* a : groups[b]) {
            sorted[k++] = *a;
            g.minOffset = std::min(g.minOffset, a->offset);
            if (a->fetchType == LatteConst::VertexFetchType2::VERTEX_DATA) g.hasVtxIndexAccess = true;
            else g.hasInstanceIndexAccess = true;
        }
        fs->bufferGroups.push_back(g);
        fs->attributeBufferMask |= 1u << b;
    }
    delete[] attrs;
    fs->key = program;
    fs->CalculateFetchShaderVkHash();
    return fs;
}
}  // namespace gx2

HLE(gx2, GX2InitFetchShaderEx) {
    // (GX2FetchShader*, void* program, count, GX2AttribStream*, type, tessMode)
    uint32 fs = arg(c, 0), prog = arg(c, 1), n = arg(c, 2), attrs = arg(c, 3);
    st32(prog, gx2::kFetchMagic);
    st32(prog + 4, n);
    st32(prog + 8, 0);
    st32(prog + 12, 0);
    for (uint32 i = 0; i < n; i++) {
        uint32 s = attrs + i * 0x20, d = prog + 16 + i * 16;
        // GX2AttribStream: +0 location, +4 buffer, +8 offset, +C format, +10 indexType, +14 aluDivisor, +18 destSel, +1C endianSwap
        st32(d, (ld32(s) & 0xFF) | (ld32(s + 4) & 0xFF) << 8 | (ld32(s + 0x10) & 0xFF) << 16 | (ld32(s + 0x1C) & 0xFF) << 24);
        st32(d + 4, ld32(s + 8));
        st32(d + 8, (ld32(s + 0xC) & 0xFFFF) | (ld32(s + 0x14) & 0xFFFF) << 16);
        st32(d + 12, ld32(s + 0x18));
    }
    memset(mem::ptr(fs), 0, 0x20);
    st32(fs + 0x08, 16 + n * 16);  // shaderSize
    st32(fs + 0x0C, prog);         // shaderPtr
    st32(fs + 0x10, n);            // attribCount
}

HLE(gx2, GX2SetFetchShader) {
    uint32 fs = arg(c, 0);
    uint32 w[2] = {ld32(fs + 0x0C) >> 8, ld32(fs + 0x08) >> 3};
    set_regs(mmSQ_PGM_START_FS, w, 2);
}

// ---------------------------------------------------------------- uniforms and attribute buffers
static void uniform_block(uint32 blockStart, uint32 index, uint32 addr, uint32 size) {
    uint32 w[7] = {addr, size - 1, 0, 1, 0, 0, 0xC0000000};
    set_regs(blockStart + index * 7, w, 7);
}
HLE(gx2, GX2SetVertexUniformBlock) { uniform_block(mmSQ_VTX_UNIFORM_BLOCK_START, arg(c, 0), arg(c, 2), arg(c, 1)); }
HLE(gx2, GX2SetPixelUniformBlock) { uniform_block(mmSQ_PS_UNIFORM_BLOCK_START, arg(c, 0), arg(c, 2), arg(c, 1)); }
HLE(gx2, GX2SetGeometryUniformBlock) { uniform_block(mmSQ_GS_UNIFORM_BLOCK_START, arg(c, 0), arg(c, 2), arg(c, 1)); }

static void uniform_regs(uint32 stageBase, uint32 offset, uint32 count, uint32 values) {
    if (offset & 0x8000) return;
    count &= ~3u;
    if (offset + count > 0x400) count = 0x400 - std::min<uint32>(offset, 0x400);
    uint32 tmp[0x400]; // only count words are initialized; no soft-TLS lookup
    for (uint32 i = 0; i < count; i++) tmp[i] = ld32(values + 4 * i);
    if (stageBase == 0x400 && count == 16 && aspect::tagged_projection()) {  // a layout projection (aspect.cpp)
        uint32 w[17];
        w[0] = mmSQ_ALU_CONSTANT0_0 + stageBase + offset;
        memcpy(w + 1, tmp, 16 * 4);
        gx2::emit(gx2::OP_SET_PROJ_REGS, w, 17);
        return;
    }
    set_regs(mmSQ_ALU_CONSTANT0_0 + stageBase + offset, tmp, count);
}
HLE(gx2, GX2SetVertexUniformReg) { uniform_regs(0x400, arg(c, 0), arg(c, 1), arg(c, 2)); }
HLE(gx2, GX2SetPixelUniformReg) { uniform_regs(0, arg(c, 0), arg(c, 1), arg(c, 2)); }

HLE(gx2, GX2SetAttribBuffer) {
    // (index, size, stride, data)
    uint32 w[7] = {arg(c, 3), arg(c, 1) - 1, (arg(c, 2) & 0xFFFF) << 11, 0, 0, 0, 0xC0000000};
    set_regs(mmSQ_VTX_ATTRIBUTE_BLOCK_START + arg(c, 0) * 7, w, 7);
}

// ---------------------------------------------------------------- shader queries
HLE(gx2, GX2GetVertexShaderGPRs) { ret(c, ld32(arg(c, 0)) & 0xFF); }        // SQ_PGM_RESOURCES_VS.NUM_GPRS
HLE(gx2, GX2GetVertexShaderStackEntries) { ret(c, (ld32(arg(c, 0)) >> 8) & 0xFF); }
HLE(gx2, GX2GetPixelShaderGPRs) { ret(c, ld32(arg(c, 0)) & 0xFF); }
HLE(gx2, GX2GetPixelShaderStackEntries) { ret(c, (ld32(arg(c, 0)) >> 8) & 0xFF); }
HLE(gx2, GX2GetGeometryShaderGPRs) { ret(c, ld32(arg(c, 0)) & 0xFF); }
HLE(gx2, GX2GetGeometryShaderStackEntries) { ret(c, (ld32(arg(c, 0)) >> 8) & 0xFF); }
