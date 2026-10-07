#!/usr/bin/env python3
"""Host tests using the production gather, pair, rank, descriptor and register functions.

Usage: python tools/switch/test_render_prepare.py <clang++>
Requires Vulkan headers (for types only); no game data, libnx or Vulkan driver.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, marker):
    start = source.index(marker)
    begin = source.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def source():
    shaders = (ROOT / "runtime/src/gfx/vulkan/shaders.cpp").read_text()
    draw = (ROOT / "runtime/src/gfx/vulkan/draw.cpp").read_text()
    core = (ROOT / "runtime/src/gx2/gx2_core.cpp").read_text()
    prelude = r'''
#include "gfx/vulkan/backend.h"
#include "gfx/vulkan/shaders.h"
#include "gfx/vulkan/cpu_snapshot.h"
#include "gfx/vulkan/word_cache.h"
#include "gfx/vulkan/draw_options.h"
#include "gx2/gx2.h"
#include "gx2/gx2_cmd.h"
#include "gx2/shader_key_dirty.h"
#include "gx2/sparse_register_masks.h"
#include "gx2/shader_program_writes.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include <random>
#include <stdexcept>
using Latte::REGADDR;
static void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
static uint32_t compactCount=1; static bool compactFetch=true; static uint64_t fetchCompiles=0;
uint32_t ld32(uint32_t address) { return (address&15)==0 ? (compactFetch?0x57574653:0) : compactCount; }
uint8_t* ppc_ptr(uint32_t) { static uint8_t dummy[16]; return dummy; }
LatteFetchShader* LatteShaderRecompiler_createFetchShader(uint64_t key,uint32_t*,uint32_t*,uint32_t) {
    ++fetchCompiles; return reinterpret_cast<LatteFetchShader*>(uintptr_t(key|16));
}
namespace render { bool vulkan() { return true; } }
namespace gfxvk { void reset_draw_state_cache() {} }
namespace gx2 {
LatteFetchShader* build_fetch_shader(uint32_t address) {
    ++fetchCompiles; return reinterpret_cast<LatteFetchShader*>(uintptr_t(address|16));
}
'''
    registers = core[core.index("static uint32 g_regs["):core.index("static std::recursive_mutex g_exec_mutex;")]
    registers += r'''
uint32* regs() { return g_regs; }
uint64_t g_shader_state_gen=1,g_pipeline_state_gen=1;
DrawStateGenerations drawStateGenerations;
static ShaderKeyDirtyStats shaderKeyDirtyStats;
static std::array<uint32,kNumRegs> g_bump_regs{};
'''
    registers += function(core, "static bool shader_irrelevant(")
    small = function(core, "static void apply_small_regs(")
    small = small.replace("        return result;", """
#ifdef __SWITCH__
        for (uint32 reg=0;reg<kNumRegs;++reg)
            require(result[reg]==classify(reg),"sparse production mask changed classification");
        printf("Production register masks: %zu bytes (dense baseline %zu)\\n",result.bytes(),size_t(kNumRegs)*16);
#endif
        return result;""")
    registers += small
    registers += "static void apply_regs_unmarked(uint32,const uint32*,uint32);\n"
    registers += function(core, "static void apply_regs(uint32")
    registers += function(core, "static void apply_regs_unmarked(uint32 first, const uint32* v, uint32 n) {")
    registers += "}\nnamespace gfxvk {\n"
    descriptors = function(draw,"struct ResolvedTexture {") + ";\n"
    descriptors += draw[draw.index("struct DescriptorIdentity {"):draw.index("// A descriptor cannot sample")]
    descriptors += "}\nnamespace gfxvk::vk {\n"
    shader_helpers = "using TranslationState=std::array<uint32_t,105+5*LATTE_NUM_MAX_TEX_UNITS>;\n"
    shader_helpers += shaders[shaders.index("WordCache<uint64_t> pairStates;"):shaders.index("// The analyzer also annotates")]
    shader_helpers += function(shaders,"struct LastFetch {") + ";\nLastFetch lastFetch;\n"
    shader_helpers += "std::array<LastFetch,64> fetchRangeMemo; std::unordered_map<uint64_t,LatteFetchShader*> fetchShaders;\n"
    shader_helpers += "ShaderStats stats; constexpr uint32_t maxProgramBytes=0x100000;\n"
    shader_helpers += function(shaders,"uint64_t hash_bytes(")
    shader_helpers += function(shaders,"template<bool restore, size_t N>\nsize_t translation_state(")
    shader_helpers += function(shaders,"DescriptorRankPlan make_descriptor_rank_plan(")
    shader_helpers += r'''
uint64_t revision=0,translateCalls=0;
uint64_t program_validation(uint64_t frame) { return frame; }
uint64_t program_hash(uint32_t address,uint32_t size,uint64_t,size_t) { return address*31ull+size+revision; }
std::unordered_map<uint64_t,std::unique_ptr<Shader>> variants;
LatteDecompilerShader dec{LatteConst::ShaderType::Vertex};
Shader* translate(const uint32_t* regs,bool vertex,LatteFetchShader*,uint64_t fsKey,uint64_t,uint64_t) {
    ++translateCalls;
    TranslationState words;
    const size_t count=translation_state<false>(const_cast<uint32_t*>(regs),words,vertex);
    const uint32_t start=vertex?mmSQ_PGM_START_VS:mmSQ_PGM_START_PS;
    uint64_t base=program_hash(regs[start]<<8,regs[start+1]<<3,0,0)^(vertex?0x1111:0x2222);
    const uint64_t key=hash_bytes(words.data(),count*4,base)^(vertex?fsKey*31:0);
    auto& value=variants[key];
    if(!value) { value=std::make_unique<Shader>(); value->key=key; value->dec=&dec;
        value->spirv=std::make_shared<const std::vector<uint32_t>>(1,1); }
    return value.get();
}
'''
    shader_helpers += function(shaders,"ShaderPair translate_pair(")
    shader_helpers += function(shaders,"LatteFetchShader* get_fetch_shader(")
    shader_helpers += "}\n"
    tests = r'''
static void masks() {
    using namespace gfxvk::vk;
    std::array<uint32_t,65536> regs,actual{};
    std::mt19937 random(41);
    for(auto& word:regs) word=random();
    for(bool streamout:{false,true}) {
        regs[mmVGT_STRMOUT_EN]=streamout;
        for(bool vertex:{false,true}) {
            TranslationState baseline,changed;
            const size_t count=translation_state<false>(regs.data(),baseline,vertex);
            for(uint32_t reg=0;reg<regs.size();++reg) {
                const uint32_t original=regs[reg];
                for(uint32_t bit=0;bit<32;++bit) {
                    regs[reg]=original^(1u<<bit);
                    const size_t next=translation_state<false>(regs.data(),changed,vertex);
                    if(next!=count || memcmp(baseline.data(),changed.data(),count*4)) actual[reg]|=1u<<bit;
                }
                regs[reg]=original;
            }
        }
    }
    for(uint32_t reg=0;reg<regs.size();++reg) {
        if(actual[reg]!=gx2::vulkan_translation_mask(reg)) {
            fprintf(stderr,"mask mismatch %04X actual %08X declared %08X\n",reg,actual[reg],gx2::vulkan_translation_mask(reg));
            throw std::runtime_error("translation masks differ from production gather");
        }
    }
}
static void registers() {
    using namespace gx2;
    std::vector<uint32_t> shadow(kNumRegs);
    std::fill_n(g_regs,kNumRegs,0); g_shadow=shadow.data();
    auto& context=new_context_blocks(10); g_shadow_used=&context;
    uint32_t word=0x555;
    const uint32_t reg=mmSQ_PGM_START_VS;
    g_regs[reg]=word; shadow[reg]=0;
    apply_regs(reg,&word,1);
    require(shadow[reg]==word,"equal live register failed to repair shadow");
    equal_add(reg>>8,context.token(reg>>8));
    const auto translation=drawStateGenerations.translation;
    apply_regs(reg,&word,1);
    require(equal_has(reg>>8,context.token(reg>>8)),"identical write lost block equality");
    ++word; apply_regs(reg,&word,1);
    require(drawStateGenerations.translation==translation,"program switch dirtied translation words");
    word=1; apply_regs(REGADDR::SX_ALPHA_TEST_CONTROL,&word,1);
    require(drawStateGenerations.translation>translation,"translation word missed generation");
    auto old=drawStateGenerations.translation;
    word=2; apply_regs(REGADDR::PA_CL_CLIP_CNTL,&word,1);
    require(drawStateGenerations.translation==old,"masked clip bits dirtied translation");
    word=1u<<19; apply_regs(REGADDR::PA_CL_CLIP_CNTL,&word,1);
    require(drawStateGenerations.translation>old,"half-Z bit missed generation");
    std::array<uint32_t,256> bulk; bulk.fill(17);
    old=drawStateGenerations.translation;
    apply_regs(0xA100,bulk.data(),bulk.size());
    require(drawStateGenerations.translation>old,"bulk translation write missed");
    const auto tex=REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    const auto sampler=REGADDR::SQ_TEX_SAMPLER_WORD0_0;
    word=9; auto texGen=drawStateGenerations.textures[0][0]; apply_regs(tex+2,&word,1);
    require(drawStateGenerations.textures[0][0]>texGen,"texture address stamp missed");
    old=drawStateGenerations.translation; word=31; apply_regs(tex+2,&word,1);
    require(drawStateGenerations.translation==old,"texture address changed translation");
    auto samplerGen=drawStateGenerations.samplers[0]; apply_regs(sampler,&word,1);
    require(drawStateGenerations.samplers[0]>samplerGen,"sampler stamp missed");
    // A small packet can straddle block boundaries with different equality.
    std::array<uint32_t,2> cross{33,44};
    apply_regs(0xA1FF,cross.data(),2);
    require(shadow[0xA1FF]==33 && shadow[0xA200]==44,"cross-block shadow write failed");
    old=drawStateGenerations.translation;
    apply_regs(kNumRegs-1,cross.data(),2); apply_regs(kNumRegs+1,&word,1);
    require(drawStateGenerations.translation==old,"invalid packet changed state");
    g_shadow=nullptr; g_shadow_used=nullptr;
    drawStateGenerations.invalidate(); require(drawStateGenerations.translation>old,"global reset missed translation");
}
static void pairs() {
    using namespace gfxvk::vk;
    auto* regs=gx2::regs(); std::fill_n(regs,gx2::kNumRegs,0);
    regs[mmSQ_PGM_START_VS]=17; regs[mmSQ_PGM_START_VS+1]=8;
    regs[mmSQ_PGM_START_PS]=19; regs[mmSQ_PGM_START_PS+1]=8;
    auto* fetch=reinterpret_cast<LatteFetchShader*>(uintptr_t(16));
    auto check=[&](bool tracked=true) {
        auto pair=translate_pair(regs,fetch,55,1,1,tracked);
        require(pair.vs==translate(regs,true,fetch,55,1,1) && pair.ps==translate(regs,false,fetch,55,1,1),"pair differs from original translation key");
        return pair;
    };
    check(); auto calls=translateCalls; translate_pair(regs,fetch,55,1,1,true);
    require(translateCalls==calls,"identical pair missed memo");
    for(uint32_t i=0;i<3000;++i) {
        uint32_t value=17+i%37; gx2::apply_regs(mmSQ_PGM_START_VS,&value,1);
        value=i%4; gx2::apply_regs(REGADDR::SX_ALPHA_TEST_CONTROL,&value,1);
        value=(i%8)<<20; gx2::apply_regs(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS+1,&value,1);
        regs[REGADDR::VGT_PRIMITIVE_TYPE]=i%3; // draw's direct store
        check();
    }
    ++revision; check(); // same addresses, changed program hashes
    regs[mmSPI_VS_OUT_CONFIG]^=17; check(false); // untracked caller must gather
    check(true);
    // Exercise both bounds, including ID reuse only after all pair entries
    // have been cleared, then return to an earlier state/program pair.
    for(uint32_t i=0;i<8300;++i) { uint32_t value=100+i; gx2::apply_regs(mmSQ_PGM_START_VS,&value,1); check(); }
    for(uint32_t i=0;i<2200;++i) { uint32_t value=1000+i; gx2::apply_regs(mmSPI_VS_OUT_CONFIG,&value,1); check(); }
    uint32_t value=17; gx2::apply_regs(mmSQ_PGM_START_VS,&value,1); value=0; gx2::apply_regs(mmSPI_VS_OUT_CONFIG,&value,1); check();
    require(stats.pairHits && stats.pairStateHits,"pair cache paths not exercised");
}
static void descriptors() {
    using namespace gfxvk;
    VkDescriptorBufferInfo buffer{reinterpret_cast<VkBuffer>(uintptr_t(16)),4,32};
    VkDescriptorImageInfo image{reinterpret_cast<VkSampler>(uintptr_t(32)),reinterpret_cast<VkImageView>(uintptr_t(48)),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet,2> writes{};
    writes[0].dstBinding=1; writes[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; writes[0].pBufferInfo=&buffer;
    writes[1].dstBinding=7; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo=&image;
    auto layout=reinterpret_cast<VkDescriptorSetLayout>(uintptr_t(64));
    auto set=reinterpret_cast<VkDescriptorSet>(uintptr_t(80));
    LastDescriptorSet last; remember_descriptors(last,layout,set,writes.data(),2);
    auto key=[&](bool complete) { DescriptorKey out; descriptor_key(out,layout,writes.data(),2,complete); return out; };
    auto equal=[](const auto& a,const auto& b) { return a.count==b.count && !memcmp(a.words.data(),b.words.data(),a.count*8); };
    auto a=key(true); buffer.offset=256;
    require(equal(a,key(true)) && descriptor_matches(last,layout,writes.data(),2),"dynamic offset entered descriptor identity");
    buffer.range=33; require(!equal(a,key(true)) && !descriptor_matches(last,layout,writes.data(),2),"range change hit"); buffer.range=32;
    image.imageLayout=VK_IMAGE_LAYOUT_GENERAL; require(!equal(a,key(true)),"image layout change hit"); image.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.imageView=reinterpret_cast<VkImageView>(uintptr_t(96)); require(!equal(a,key(true)),"image view change hit"); image.imageView=reinterpret_cast<VkImageView>(uintptr_t(48));
    require(!equal(key(false),key(true)),"partial and complete keys alias");
    a=key(false); writes[0].dstBinding=2; require(!equal(a,key(false)),"partial binding change hit"); writes[0].dstBinding=1;
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    a=key(true); buffer.offset+=256; require(!equal(a,key(true)),"regular offset omitted");
    ResolvedTexture texture;
    auto device=reinterpret_cast<VkDevice>(uintptr_t(128));
    texture.descriptor=image; texture.descriptorDevice=device; texture.descriptorEpoch=7;
    texture.samplerSlot=3; texture.samplerGeneration=9;
    require(texture.image_matches(device,7,image.imageView,3,9,false,false,false,false),"equal image memo missed");
    require(!texture.image_matches(device,8,image.imageView,3,9,false,false,false,false),"surface epoch hit");
    require(!texture.image_matches(device,7,image.imageView,3,10,false,false,false,false),"sampler write hit");
    require(!texture.image_matches(device,7,image.imageView,4,9,false,false,false,false),"sampler assignment hit");
    require(!texture.image_matches(device,7,image.imageView,3,9,true,false,false,false),"AO patch hit");
    require(!texture.image_matches(device,7,image.imageView,3,9,false,true,false,false),"compare mode hit");
    require(!texture.image_matches(device,7,image.imageView,3,9,false,false,true,false),"integer mode hit");
    require(!texture.image_matches(device,7,image.imageView,3,9,false,false,false,true),"anisotropy change hit");
    require(!texture.image_matches(reinterpret_cast<VkDevice>(uintptr_t(256)),7,image.imageView,3,9,false,false,false,false),"device change hit");
    require(!texture.image_matches(device,7,reinterpret_cast<VkImageView>(uintptr_t(256)),3,9,false,false,false,false),"view change hit");
    LatteDecompilerShaderResourceMapping mapping{};
    std::fill_n(mapping.uniformBuffersBindingPoint,16,-1);
    std::fill_n(mapping.textureUnitToBindingPoint,LATTE_NUM_MAX_TEX_UNITS,-1);
    mapping.uniformVarsBufferBindingPoint=9; mapping.uniformBuffersBindingPoint[0]=5;
    mapping.textureUnitToBindingPoint[0]=2; mapping.textureUnitToBindingPoint[1]=7;
    LatteDecompilerShader shader{LatteConst::ShaderType::Pixel}; shader.textureUnitListCount=2;
    shader.textureUnitList[0]=1; shader.textureUnitList[1]=0;
    auto plan=vk::make_descriptor_rank_plan(mapping,shader);
    require(plan.valid && plan.count==4 && plan.uniformCount==2 && plan.uniformRanks[0]==1 && plan.uniformRanks[1]==3,"uniform ranks differ from binding order");
    mapping.uniformVarsBufferBindingPoint=5; require(!vk::make_descriptor_rank_plan(mapping,shader).valid,"duplicate bindings accepted");
}
static void fetches() {
    using namespace gfxvk::vk;
    auto* regs=gx2::regs(); uint64_t key=0;
    regs[mmSQ_PGM_START_FS+1]=8;
    regs[mmSQ_PGM_START_FS]=0x101; get_fetch_shader(regs,&key,1);
    regs[mmSQ_PGM_START_FS]=0x102; get_fetch_shader(regs,&key,1);
    const auto compiles=fetchCompiles;
    regs[mmSQ_PGM_START_FS]=0x101; get_fetch_shader(regs,&key,1);
#ifdef __SWITCH__
    require(stats.fetchRangeHits>0,"alternating fetch range did not hit");
#endif
    require(fetchCompiles==compiles,"unchanged fetch recompiled");
    ++revision; get_fetch_shader(regs,&key,2);
    require(fetchCompiles>compiles,"new validation stamp returned old fetch");
    compactFetch=false; ++revision;
    const auto previous=fetchCompiles; get_fetch_shader(regs,&key,3);
    ++regs[REGADDR::VGT_INSTANCE_STEP_RATE_0]; get_fetch_shader(regs,&key,3);
    require(fetchCompiles==previous+2,"raw fetch divisor change missed");
    ++regs[mmSQ_PGM_START_FS+1]; get_fetch_shader(regs,&key,3);
    require(fetchCompiles==previous+3,"raw fetch size change missed");
    get_fetch_shader(regs,&key); // standalone clears outer range entries
    const auto hits=stats.fetchRangeHits; get_fetch_shader(regs,&key,3);
    require(stats.fetchRangeHits==hits,"standalone left a stale range entry");
}
int main() {
    try { masks(); registers(); pairs(); descriptors(); fetches(); puts("Production render preparation regressions passed"); }
    catch(const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
'''
    return prelude + registers + descriptors + shader_helpers + tests


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "clang++"
    build = ROOT / "build/render-check"
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=build) as temp:
        src = Path(temp) / "prepare.cpp"
        src.write_text(source())
        for switch in (False, True):
            exe = Path(temp) / ("switch.exe" if switch else "desktop.exe")
            cmd = [compiler, "-std=c++20", "-O2", "-DWWHD_HAS_VULKAN", "-DENABLE_VULKAN",
                   "-Iruntime/src", "-Iruntime/include", "-Iruntime/third_party/cemu",
                   "-Iruntime/third_party/cemu/Cafe", "-Iruntime/third_party/fmt/include",
                   "-include", "runtime/third_party/cemu/cemu_shim.h"]
            if switch:
                cmd += ["-D__SWITCH__"]
            subprocess.run(cmd + [str(src), "-o", str(exe)], cwd=ROOT, check=True)
            env = dict(os.environ, WWHD_VK_SHADER_PAIR_MEMO="1", WWHD_VK_SHADER_KEY_DIRTY="1", WWHD_VK_FETCH_MEMO="1")
            for fused in ("1", "0"):
                subprocess.run([str(exe)], env=dict(env, WWHD_VK_FUSE_SMALL_REGS=fused), cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
