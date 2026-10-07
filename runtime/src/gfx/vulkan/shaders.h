#pragma once
#include <memory>
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"

struct LatteFetchShader;
namespace gfxvk::vk {
struct DescriptorRankPlan {
    static constexpr uint8_t unused = 255;
    std::array<uint8_t, 16> blocks;
    std::array<uint8_t, LATTE_NUM_MAX_TEX_UNITS> textures;
    uint8_t support = unused, count = 0;
    std::array<uint8_t, 17> uniformRanks{};
    uint8_t uniformCount = 0;
    bool valid = false;
    DescriptorRankPlan() { blocks.fill(unused); textures.fill(unused); }
};
DescriptorRankPlan make_descriptor_rank_plan(const LatteDecompilerShaderResourceMapping& mapping,
                                             const LatteDecompilerShader& shader);
// Metadata uses the decompiler's original binding numbers and std140 byte offsets.
// Vertex descriptors occupy set 0, pixel descriptors set 1. Device modules are
// created by the draw backend; this cache owns only translation and SPIR-V.
// pack_uniforms_raw's copies for one shader, merged into contiguous runs and bounds-checked once.
struct UniformPackPlan {
    struct Run { uint32_t dst, src, bytes; };         // src: byte offset from the ALU constants / block
    struct Group { uint32_t bankWord; std::vector<Run> runs; };
    bool built = false;
    std::vector<Run> regs;      // from the stage's ALU constant registers
    std::vector<Group> groups;  // from guest uniform blocks
    std::vector<int> texScales;
};
struct Shader {
    mutable UniformPackPlan packPlan;  // render thread, built on first pack
    uint64_t key = 0;
    bool vertex = false;
    LatteDecompilerShader* dec = nullptr;
    LatteDecompilerShaderResourceMapping mapping;
    DescriptorRankPlan descriptorRanks;
    LatteDecompilerOutputUniformOffsets uniforms;
    std::shared_ptr<const std::vector<uint32_t>> spirv;  // shared with the disk cache entry
    std::string glsl;
    std::string error;
    // two independent hashes of everything the renderer reads from this translation (GLSL, resource
    // mapping, uniform offsets, decompiler metadata): equal outputs make variants interchangeable
    std::array<uint64_t, 2> output{};
    // its SPIR-V is compiling on a worker (WWHD_VK_ASYNC_SHADERS): draws using it are skipped until
    // poll_shader_compiles completes it in place
    bool pending = false;
    bool ready() const { return dec && spirv && !spirv->empty() && error.empty(); }
};
void select_renderer();
#ifdef __SWITCH__
void revalidate_programs();  // render thread, before each command batch (shaders.cpp)
#endif
// Framed desktop calls revalidate bytes once per frame. Switch framed calls
// reuse across frames until a watched DC store or shader invalidation. Omitting
// the frame keeps immediate revalidation for standalone callers/shader tools.
LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame = ~uint64_t{0});
Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetchShader, uint64_t fsKey,
    uint64_t frame = ~uint64_t{0}, uint64_t stateGeneration = ~uint64_t{0});
struct ShaderPair { Shader *vs = nullptr, *ps = nullptr; };
ShaderPair translate_pair(const uint32_t* regs, LatteFetchShader* fetchShader, uint64_t fsKey,
                          uint64_t frame, uint64_t stateGeneration, bool tracked = false);
// Generation must advance for every shader-relevant register write. Primitive
// mode is checked separately because draw submission writes it directly.
struct ShaderStats {
    uint64_t stateHashLookups = 0, stateHashMemoHits = 0, stateHashBytes = 0;
    uint64_t fetchLookups = 0, fetchLastHits = 0;
    uint64_t lookups = 0, lastHits = 0, variantHits = 0, compiles = 0, compileNs = 0;
    uint64_t pairLookups = 0, pairHits = 0, pairStateHits = 0, fetchRangeHits = 0;
    uint64_t pairStates = 0, pairEntries = 0, pairCacheBytes = 0;
    uint64_t decompileNs = 0, spirvCompiles = 0, spirvCompileNs = 0, diskHits = 0, spirvReuseHits = 0;
    uint64_t deferredTranslations = 0;  // async mode: lookups postponed by the per-frame translation budget
    uint64_t diskLoads = 0, diskLoadNs = 0, diskSaves = 0, diskSaveNs = 0, diskSavedBytes = 0, diskSnapshotNs = 0;
};
ShaderStats shader_stats();
// Boot only, after guest memory/device init and before game/render threads start.
// Rebuilds metadata through translate; creates no Vulkan modules or pipelines.
void warm_up_shader_cache();
// Render thread: completes the pending shaders whose SPIR-V compile finished (cheap when none are).
void poll_shader_compiles(uint64_t frame);
// Shaders waiting for an asynchronous compile (statistics).
size_t pending_shader_compiles();
// Pipeline warm-up (draw.cpp): ready Shader by key; fetch shader by its content key (the fsKey that
// vertex shader keys include) and back. Compact GX2 fetch shaders all have vkPipelineHashFragment 0,
// so the layout hash cannot identify them.
Shader* find_shader(uint64_t key);
LatteFetchShader* find_fetch_shader(uint64_t fsKey);
bool fetch_shader_key(const LatteFetchShader* fetch, uint64_t& fsKey);
// Render-thread-only checkpoint schedules one immutable disk worker after quiet frames.
// Explicit save joins that worker and synchronously saves the latest revision.
// Disk cache retains SPIR-V and bounded translation input recipes.
void checkpoint_shader_cache(uint64_t frame);
bool shader_cache_dirty();
uint64_t shader_cache_changed_frame();
bool save_shader_cache();
std::vector<uint32_t> compile_glsl(const std::string& source, bool vertex, std::string* error = nullptr);
// Caller-owned CPU scratch; all active bytes are freshly zeroed and packed.
// Size in bytes of a shader's support uniform block (pack_uniforms_into's output size).
size_t support_uniform_size(const Shader& shader);
// Packs the support uniform block into out[0, support_uniform_size(shader)): every byte is written.
void pack_uniforms_raw(const uint32_t* regs, const Shader& shader, uint8_t* out,
                       float scaleX, float scaleY);
void pack_uniforms_into(const uint32_t* regs, const Shader& shader,
    std::vector<uint8_t>& data, float scaleX = 1.0f, float scaleY = 1.0f);
std::vector<uint8_t> pack_uniforms(const uint32_t* regs, const Shader& shader,
    float scaleX = 1.0f, float scaleY = 1.0f);
// After guest memory replacement and render drain; compiled variants stay alive.
void reset_shader_memoization();
// Call only after draw code has discarded pipelines and references to Shader.
void clear_shader_cache();
} // namespace gfxvk::vk
