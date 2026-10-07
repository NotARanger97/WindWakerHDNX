#pragma once
#include <cstdint>
#include <initializer_list>
#include "gx2_regs.h"

namespace gx2 {
// Exact union of translation_state's VS/PS words. Program starts and fetch
// inputs select programs, but do not change the state words fed to translation.
inline uint32_t vulkan_translation_mask(uint32_t reg) {
    using Latte::REGADDR;
    for (uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                         uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)})
        if (reg >= base && reg < base + 7 * LATTE_NUM_MAX_TEX_UNITS) {
            switch ((reg - base) % 7) {
            case 0: return 7;
            case 1: return 0x03F00000;
            case 4: return 0x300;
            default: return 0;
            }
        }
    const uint32_t samplers = REGADDR::SQ_TEX_SAMPLER_WORD0_0;
    if (reg >= samplers && reg < samplers + 3 * 3 * LATTE_NUM_MAX_TEX_UNITS)
        return (reg - samplers) % 3 == 0 ? 0xF8000000 : 0;
    if (reg == REGADDR::VGT_PRIMITIVE_TYPE) return 0x3F;
    if (reg == mmSPI_INTERP_CONTROL_0) return 1u << 1;
    if (reg == REGADDR::PA_CL_VTE_CNTL) return 0x3F;
    if (reg == REGADDR::PA_CL_CLIP_CNTL) return 1u << 19;
    if (reg == REGADDR::DB_DEPTH_CONTROL) return 0x83;
    if ((reg >= mmSQ_VTX_SEMANTIC_0 && reg < mmSQ_VTX_SEMANTIC_0 + 32) ||
        (reg >= mmSPI_VS_OUT_ID_0 && reg < mmSPI_VS_OUT_ID_0 + 10) ||
        (reg >= mmSPI_PS_IN_CONTROL_0 && reg < mmSPI_PS_IN_CONTROL_0 + 2) ||
        (reg >= mmSPI_PS_INPUT_CNTL_0 && reg < mmSPI_PS_INPUT_CNTL_0 + 32) ||
        (reg >= mmCB_COLOR0_INFO && reg < mmCB_COLOR0_INFO + 8)) return UINT32_MAX;
    for (uint32_t b = 0; b < 4; ++b)
        if (reg == mmVGT_STRMOUT_VTX_STRIDE_0 + b * 4) return UINT32_MAX;
    for (uint32_t word : {uint32_t(mmSPI_VS_OUT_CONFIG), uint32_t(mmPA_CL_VS_OUT_CNTL),
                         uint32_t(mmVGT_STRMOUT_EN), uint32_t(REGADDR::VGT_GS_MODE),
                         uint32_t(REGADDR::SQ_CONFIG), uint32_t(mmCB_SHADER_MASK),
                         uint32_t(mmCB_SHADER_CONTROL), uint32_t(mmDB_SHADER_CONTROL),
                         uint32_t(mmSPI_INPUT_Z), uint32_t(REGADDR::SX_ALPHA_TEST_CONTROL),
                         uint32_t(REGADDR::CB_COLOR_CONTROL), uint32_t(REGADDR::CB_TARGET_MASK)})
        if (reg == word) return UINT32_MAX;
    return 0;
}
inline bool vulkan_fetch_register(uint32_t reg) {
    return reg == mmSQ_PGM_START_FS || reg == mmSQ_PGM_START_FS + 1 ||
        reg == Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0 ||
        reg == Latte::REGADDR::VGT_INSTANCE_STEP_RATE_1;
}
// color_target/depth_target plus LatteMRT's attachment enable/scissor tests.
inline bool vulkan_target_register(uint32_t reg) {
    using Latte::REGADDR;
    for (uint32_t base : {uint32_t(mmCB_COLOR0_BASE), uint32_t(mmCB_COLOR0_SIZE),
                         uint32_t(mmCB_COLOR0_INFO), uint32_t(mmCB_COLOR0_TILE),
                         uint32_t(mmCB_COLOR0_FRAG), uint32_t(mmCB_COLOR0_VIEW)})
        if (reg >= base && reg < base + 8) return true;
    return reg == mmDB_DEPTH_BASE || reg == mmDB_DEPTH_SIZE || reg == mmDB_DEPTH_INFO ||
        reg == mmDB_DEPTH_VIEW || reg == mmDB_HTILE_DATA_BASE || reg == 0xA002 ||
        reg == REGADDR::CB_COLOR_CONTROL || reg == REGADDR::CB_TARGET_MASK ||
        reg == REGADDR::DB_DEPTH_CONTROL || reg == REGADDR::PA_SC_GENERIC_SCISSOR_TL ||
        reg == REGADDR::PA_SC_GENERIC_SCISSOR_BR;
}
// One-based slot, zero means unrelated. All seven/three words are dependencies.
inline uint8_t vulkan_texture_slot(uint32_t reg) {
    using Latte::REGADDR;
    uint8_t stage = 0;
    for (uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                         uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)}) {
        if (reg >= base && reg < base + 7 * LATTE_NUM_MAX_TEX_UNITS)
            return 1 + stage * 18 + (reg - base) / 7;
        ++stage;
    }
    return 0;
}
inline uint8_t vulkan_sampler_slot(uint32_t reg) {
    const uint32_t base = Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0;
    return reg >= base && reg < base + 3 * 54 ? 1 + (reg - base) / 3 : 0;
}
// Exactly the static register inputs serialized by gfxvk::pipeline. Attachment
// formats, shaders/fetch program and topology are checked separately there.
inline uint32_t vulkan_pipeline_mask(uint32_t reg) {
    using Latte::REGADDR;
    if (reg == REGADDR::DB_STENCILREFMASK || reg == REGADDR::DB_STENCILREFMASK_BF) return 0x00FFFF00;
    if (reg == REGADDR::PA_SU_SC_MODE_CNTL) return 0x807;
    if (reg == REGADDR::PA_CL_CLIP_CNTL) return 1u << 27;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16)
        return (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7 == 2 ? 0x07FFF800 : 0;
    if ((reg >= REGADDR::CB_BLEND0_CONTROL && reg < REGADDR::CB_BLEND0_CONTROL + 8) ||
        reg == REGADDR::CB_COLOR_CONTROL || reg == REGADDR::CB_TARGET_MASK ||
        reg == REGADDR::DB_DEPTH_CONTROL || reg == REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE ||
        reg == REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET || reg == REGADDR::PA_SU_POLY_OFFSET_CLAMP)
        return UINT32_MAX;
    return 0;
}
// Only these proven registers have partial/no influence on Vulkan's current
// shader key. Unknowns, program headers and vertex strides stay conservative.
inline bool vulkan_shader_key_mask(uint32_t reg, uint32_t& mask) {
    using Latte::REGADDR;
    for(uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                         uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)}) {
        if(reg >= base && reg < base + 7 * LATTE_NUM_MAX_TEX_UNITS) {
            switch((reg - base) % 7) {
            case 0: mask = 7; break;
            case 1: mask = 0x03F00000; break;
            case 4: mask = 0x300; break;
            default: mask = 0; break;
            }
            return true;
        }
    }
    constexpr uint32_t samplers = uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0);
    if(reg >= samplers && reg < samplers + 3 * 3 * LATTE_NUM_MAX_TEX_UNITS) {
        mask = (reg - samplers) % 3 == 0 ? 0xF8000000 : 0;
        return true;
    }
    if(reg == REGADDR::VGT_PRIMITIVE_TYPE) { mask = 0x3F; return true; }
    if(reg == mmSPI_INTERP_CONTROL_0) { mask = 1u << 1; return true; }
    if(reg == REGADDR::PA_CL_VTE_CNTL) { mask = 0x3F; return true; }
    if(reg == REGADDR::PA_CL_CLIP_CNTL) { mask = 1u << 19; return true; }
    if(reg == REGADDR::DB_DEPTH_CONTROL) { mask = 0x83; return true; }
    // Alpha-test reference: a uniform (pack_uniforms_into), never part of a shader key.
    if(reg == REGADDR::SX_ALPHA_REF) { mask = 0; return true; }
    if((reg >= REGADDR::PA_CL_VPORT_XSCALE && reg <= REGADDR::PA_CL_VPORT_ZOFFSET) ||
       (reg >= REGADDR::CB_BLEND_RED && reg <= REGADDR::CB_BLEND_ALPHA) ||
       (reg >= REGADDR::CB_BLEND0_CONTROL && reg < REGADDR::CB_BLEND0_CONTROL + 8) ||
       reg == REGADDR::PA_SC_GENERIC_SCISSOR_TL || reg == REGADDR::PA_SC_GENERIC_SCISSOR_BR ||
       reg == REGADDR::DB_STENCILREFMASK || reg == REGADDR::DB_STENCILREFMASK_BF ||
       reg == REGADDR::PA_SU_SC_MODE_CNTL || reg == REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE ||
       reg == REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET || reg == REGADDR::PA_SU_POLY_OFFSET_CLAMP) {
        mask = 0;
        return true;
    }
    return false;
}
} // namespace gx2
