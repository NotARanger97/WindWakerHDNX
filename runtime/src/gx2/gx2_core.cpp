#include "../frame_trace.h"
#include "../platform/host.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "gx2_texture_regs.h"
#ifdef __SWITCH__
#include "command_ring.h"
#endif
#ifdef WWHD_HAS_VULKAN
#include "shader_key_dirty.h"
#include "sparse_register_masks.h"
#include "gfx/vulkan/api.h"
#include "gfx/vulkan/draw_options.h"
#endif
#include "runtime.h"
#include "../aspect.h"
#include "gfx/renderer.h"
#include "platform/perf_hint.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
// set_context touches only the 1 KiB register blocks that can differ (the game switches context
// states dozens of times per frame; their 256 KiB shadows are cold in the cache).
//  - A block of g_regs holding a non-zero word is set in g_live; a block of a context's shadow
//    holding a non-zero word is set in its ContextBlocks::used. Blocks clear in both are all zero.
//  - Each context block has a version, bumped on every write to it. g_equal[b] lists tokens
//    (context id, block version) whose shadow block g_regs block b is known to equal.
// Every write goes through apply_regs / set_context / OP_SETUP_CONTEXT / the save-state load,
// which keep both; the renderer's own VGT_PRIMITIVE_TYPE store is covered by always processing
// (and keeping live) that block. Context ids are never reused.
constexpr uint32 kRegBlocks = kNumRegs / 256;
struct RegBlocks {
    std::array<uint64_t, kRegBlocks / 64> bits{};
    void set(uint32 block) { bits[block / 64] |= uint64_t{1} << (block % 64); }
    bool test(uint32 block) const { return bits[block / 64] >> (block % 64) & 1; }
    void fill() { bits.fill(UINT64_MAX); }
};
struct ContextBlocks {
    RegBlocks used;
    uint64_t id = 0;
    std::array<uint32, kRegBlocks> version{};
    uint64_t token(uint32 block) const { return id << 32 | version[block]; }
};
static uint64_t g_next_context_id = 1;
static RegBlocks g_live = [] { RegBlocks b; b.fill(); return b; }();
static std::unordered_map<uint32, ContextBlocks> g_context_used;
static ContextBlocks* g_shadow_used = nullptr;
static std::array<std::array<uint64_t, 4>, kRegBlocks> g_equal{};
constexpr uint32 kPrimBlock = REGADDR::VGT_PRIMITIVE_TYPE >> 8;  // also written by the renderer's draw()
static bool equal_has(uint32 block, uint64_t token) {
    for (uint64_t t : g_equal[block]) if (t == token) return true;
    return false;
}
static void equal_add(uint32 block, uint64_t token) {
    auto& e = g_equal[block];
    if (equal_has(block, token)) return;
    e[3] = e[2]; e[2] = e[1]; e[1] = e[0]; e[0] = token;
}
static ContextBlocks& new_context_blocks(uint32 ctx) {
    auto& c = g_context_used[ctx];
    c = {};
    c.id = g_next_context_id++;
    return c;
}
static inline void mark_written(uint32 first, uint32 n) {
    for (uint32 b = first >> 8, last = (first + n - 1) >> 8; b <= last; ++b) {
        g_live.set(b);
        if (g_shadow_used) {
            // g_regs and the shadow receive the same words: if they were equal they still are
            const bool same = equal_has(b, g_shadow_used->token(b));
            g_shadow_used->used.set(b);
            ++g_shadow_used->version[b];
            g_equal[b] = {};
            if (same) g_equal[b][0] = g_shadow_used->token(b);
        } else {
            g_equal[b] = {};
        }
    }
}
static std::recursive_mutex g_exec_mutex;

uint32* regs() { return g_regs; }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }
extern "C" { uint64_t g_pipeline_state_gen = 1; }
DrawStateGenerations drawStateGenerations;

static bool shader_irrelevant(uint32 reg) {
#ifdef WWHD_HAS_VULKAN
    // Vulkan resolves texture addresses freshly in bind_stage. These words do
    // not participate in shader translation; keep Metal's broader dirty gate.
    static const bool addressMemo = [] {
        const char* e = getenv("WWHD_VK_SHADER_ADDRESS_MEMO");
        return render::vulkan() && e && !strcmp(e, "1");
    }();
    if (addressMemo)
        for (uint32 base : {uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
            if (reg >= base && reg < base + 7 * 18) {
                uint32 word = (reg - base) % 7;
                if (word == 2 || word == 3) return true;
            }
#endif
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32 base : {(uint32)mmSQ_VTX_UNIFORM_BLOCK_START, (uint32)mmSQ_PS_UNIFORM_BLOCK_START, (uint32)mmSQ_GS_UNIFORM_BLOCK_START})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32 w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}
static ShaderKeyDirtyStats shaderKeyDirtyStats;
// WWHD_VK_STATS: shader-generation bumps per register, logged by log_bump_regs
static std::array<uint32, kNumRegs> g_bump_regs{};
void log_bump_regs(uint64_t frames) {
    std::vector<std::pair<uint32, uint32>> top;
    for (uint32 r = 0; r < kNumRegs; ++r) if (g_bump_regs[r]) top.push_back({g_bump_regs[r], r});
    std::sort(top.rbegin(), top.rend());
    std::string line;
    for (size_t i = 0; i < top.size() && i < 16; ++i) {
        char b[48]; snprintf(b, sizeof b, " %04X:%.0f", top[i].second, top[i].first / double(frames ? frames : 1));
        line += b;
    }
    LOG("[gx2] shader-gen bumps/frame by register:%s", line.c_str());
    g_bump_regs.fill(0);
}
ShaderKeyDirtyStats shader_key_dirty_stats() { return shaderKeyDirtyStats; }

#ifdef WWHD_HAS_VULKAN
// How a change of each register affects the renderer's draw-state memos (render thread, and the
// Switch GX2 front end, which classifies changes for it)
struct RegisterMasks {
    uint32 shader, pipeline, translation;
    uint8_t texture, sampler;
    bool baseline, fetch, targets;
    bool operator==(const RegisterMasks&) const = default;
};
static const auto& register_masks() {
    static const bool keyDirty = [] {
        return gfxvk::draw_option_enabled("WWHD_VK_SHADER_KEY_DIRTY");
    }();
    using Masks = RegisterMasks;
    // Classification is immutable for the selected renderer/options. Keep the
    // conservative unknown-register mask; only proven irrelevant bits drop out.
    static const auto masks = [] {
        auto classify = [](uint32 reg) {
            Masks m{};
            m.pipeline = vulkan_pipeline_mask(reg);
            m.translation = vulkan_translation_mask(reg);
            m.fetch = vulkan_fetch_register(reg);
            m.targets = vulkan_target_register(reg);
            m.texture = vulkan_texture_slot(reg);
            m.sampler = vulkan_sampler_slot(reg);
            m.baseline = !shader_irrelevant(reg);
            m.shader = m.baseline ? UINT32_MAX : 0;
            uint32 mask;
            if (m.baseline && keyDirty && vulkan_shader_key_mask(reg, mask)) m.shader = mask;
            return m;
        };
#ifdef __SWITCH__
        SparseRegisterMasks<Masks, kNumRegs> result({UINT32_MAX, 0, 0, 0, 0, true, false, false}, classify);
#else
        std::array<Masks, kNumRegs> result;
        for (uint32 reg = 0; reg < kNumRegs; ++reg) result[reg] = classify(reg);
#endif
        return result;
    }();
    return masks;
}
static void apply_small_regs(uint32 first, const uint32* v, uint32 n) {
    static const bool collectStats = getenv("WWHD_VK_STATS") != nullptr;
    const auto& masks = register_masks();
    bool changed = false, baselineBump = false, actualBump = false, pipelineBump = false;
    bool fetchBump = false, targetBump = false, translationBump = false;
    uint64_t maskedWords = 0;
    // mark_written preserves equality tokens when both copies were equal.
    // At most two blocks occur in a small packet; avoid cold shadow stores on
    // identical rewrites only when a token proves the whole block still equal.
    std::array<bool, 2> shadowEqual{};
    if (n && g_shadow_used)
        for (uint32 b = first >> 8; b <= (first + n - 1) >> 8; ++b)
            shadowEqual[b - (first >> 8)] = equal_has(b, g_shadow_used->token(b));
    for (uint32 i = 0; i < n; ++i) {
        const uint32 reg = first + i, value = v[i], old = g_regs[reg];
        if (old != value) {
            changed = true;
            const auto& m = masks[reg];
            fetchBump |= m.fetch;
            targetBump |= m.targets;
            if (m.texture) {
                const uint32_t slot = m.texture - 1;
                ++drawStateGenerations.textures[slot / 18][slot % 18];
            }
            if (m.sampler) ++drawStateGenerations.samplers[m.sampler - 1];
            const uint32 diff = old ^ value;
            translationBump |= bool(diff & m.translation);
            if (diff & m.pipeline) pipelineBump = true;
            if ((collectStats || !actualBump) && m.baseline) {
                baselineBump = true;
                if (!(diff & m.shader)) {
                    if (collectStats) ++maskedWords;
                } else { actualBump = true; if (collectStats) ++g_bump_regs[reg]; }
            }
            g_regs[reg] = value;
        }
        // Shadow can differ even when registers already match: draw mutates
        // primitive type directly, and context setup initializes only shadow.
        if (g_shadow && (old != value || !shadowEqual[(reg >> 8) - (first >> 8)])) g_shadow[reg] = value;
    }
    if (actualBump) ++g_shader_state_gen;
    if (pipelineBump) ++g_pipeline_state_gen;
    if (fetchBump) ++drawStateGenerations.fetch;
    if (targetBump) ++drawStateGenerations.targets;
    if (translationBump) ++drawStateGenerations.translation;
    if (changed && collectStats) {
        ++shaderKeyDirtyStats.changedBatches;
        shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
        shaderKeyDirtyStats.actualBumps += actualBump;
        shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
        shaderKeyDirtyStats.maskedWords += maskedWords;
    }
}
#endif

static void apply_regs_unmarked(uint32 first, const uint32* v, uint32 n);
static void apply_regs(uint32 first, const uint32* v, uint32 n) {
    if (first > kNumRegs || n > kNumRegs - first) return;
    if (n) mark_written(first, n);
    apply_regs_unmarked(first, v, n);
}
static void apply_regs_unmarked(uint32 first, const uint32* v, uint32 n) {
    // ALU constants affect uniform contents only. No comparison, classification
    // or generation scan is needed, even for a full 16-register matrix command.
    if (first >= mmSQ_ALU_CONSTANT0_0 && first <= mmSQ_ALU_CONSTANT0_0 + 0x1000 &&
        n <= mmSQ_ALU_CONSTANT0_0 + 0x1000 - first) {
#ifdef WWHD_HAS_VULKAN
        static const bool collectStats = render::vulkan() && getenv("WWHD_VK_STATS") != nullptr;
        if (collectStats && memcmp(g_regs + first, v, n * sizeof(uint32)))
            ++shaderKeyDirtyStats.changedBatches;
#endif
        memcpy(g_regs + first, v, n * sizeof(uint32));
        if (g_shadow && g_shadow + first != v) memcpy(g_shadow + first, v, n * sizeof(uint32));
        return;
    }
#ifdef WWHD_HAS_VULKAN
    // Vulkan renderer only (the Metal renderer keeps the original bulk path)
    static const bool fusedSmall = [] {
        const char* e = getenv("WWHD_VK_FUSE_SMALL_REGS");
        // Enabled by default; explicit zero retains the original bulk path.
        return render::vulkan() && (!e || strcmp(e, "0") != 0);
    }();
    if (fusedSmall && n <= 16) {
        apply_small_regs(first, v, n);
        return;
    }
#endif
    if (memcmp(&g_regs[first], v, n * 4) != 0) {
#ifdef WWHD_HAS_VULKAN
        static const bool keyDirty = [] {
            return render::vulkan() && gfxvk::draw_option_enabled("WWHD_VK_SHADER_KEY_DIRTY");
        }();
        static const bool collectStats = render::vulkan() && getenv("WWHD_VK_STATS") != nullptr;
        for (uint32 i = 0; i < n; ++i) {
            const uint32 reg = first + i;
            if (g_regs[reg] == v[i]) continue;
            if ((g_regs[reg] ^ v[i]) & vulkan_translation_mask(reg)) ++drawStateGenerations.translation;
            if (vulkan_fetch_register(reg)) ++drawStateGenerations.fetch;
            if (vulkan_target_register(reg)) ++drawStateGenerations.targets;
            if (const uint32 slot = vulkan_texture_slot(reg))
                ++drawStateGenerations.textures[(slot - 1) / 18][(slot - 1) % 18];
            if (const uint32 slot = vulkan_sampler_slot(reg)) ++drawStateGenerations.samplers[slot - 1];
        }
        for (uint32 i = 0; i < n; ++i)
            if ((g_regs[first + i] ^ v[i]) & vulkan_pipeline_mask(first + i)) {
                ++g_pipeline_state_gen;
                break;
            }
        if(keyDirty || collectStats) {
            bool baselineBump = false, actualBump = false;
            uint64_t maskedWords = 0;
            for(uint32 i = 0; i < n; ++i) {
                uint32 reg = first + i;
                if(g_regs[reg] == v[i] || shader_irrelevant(reg)) continue;
                baselineBump = true;
                uint32 mask;
                if(keyDirty && vulkan_shader_key_mask(reg, mask) && !((g_regs[reg] ^ v[i]) & mask)) {
                    if(collectStats) ++maskedWords;
                } else { actualBump = true; if (collectStats) ++g_bump_regs[reg]; }
                if(actualBump && !collectStats) break;
            }
            if(actualBump) ++g_shader_state_gen;
            if(collectStats) {
                ++shaderKeyDirtyStats.changedBatches;
                shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
                shaderKeyDirtyStats.actualBumps += actualBump;
                shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
                shaderKeyDirtyStats.maskedWords += maskedWords;
            }
        } else
#endif
        for (uint32 i = 0; i < n; i++)
            if (g_regs[first + i] != v[i] && !shader_irrelevant(first + i)) { g_shader_state_gen++; break; }
        memcpy(&g_regs[first], v, n * 4);
    }
    if (g_shadow && g_shadow + first != v) memcpy(&g_shadow[first], v, n * 4);
}

// ---------------------------------------------------------------- display list recording
struct Recording {
    uint32 start = 0, pos = 0, end = 0;
};
static thread_local Recording t_rec;

static void execute_one(Op op, const uint32* p, uint32 n);

// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into Metal work. WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::mutex g_fence_issue_mutex;
static std::condition_variable g_q_cv, g_q_done_cv;
#ifdef __SWITCH__
static CommandRing<1 << 20> g_q_ring; // 4 MiB, no per-command allocation
#else
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;
#endif
static uint64_t g_fence_issued = 0, g_fence_done = 0;

#ifdef __SWITCH__
extern "C" void switch_set_helper_thread(int core, int priority);
extern "C" void switch_log_clocks();
extern "C" void switch_log_thread_cpu(uint64_t frames);
extern "C" void switch_note_render_thread();
extern "C" void switch_sample_core_idle();
#endif
// slow-frame diagnostics: render thread waiting for commands; game thread waiting in render_sync
std::atomic<uint64_t> g_render_idle_ns{0}, g_main_sync_ns{0};
static uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
#ifdef __SWITCH__
// ---------------------------------------------------------------- GX2 front end (Switch)
// WWHD_GX2_FRONTEND (default on): a thread on CPU core 0 (~40% idle in heavy views, while the render
// thread on core 2 is the frame's critical path) consumes the game's command queue. It flattens
// display-list calls, owns the register file and the context shadows, drops register writes that
// change nothing (the game re-sends its render state before every draw: ~60% of ~50,000 register
// packets a frame) and classifies the remaining changes for the renderer's draw-state memos. The
// render thread receives only changed register words (OP_RAW_REGS: plain stores into g_regs), the
// pending generation bumps (OP_GEN_BUMPS, before the next command that can read them) and every other
// command unchanged and in order. Both register files receive the same effective writes, so g_regs
// is what the single-thread path would have at every command. Display-list memory is read when the
// front end reaches the call, which is earlier than the render thread would, never later.
static bool frontend_enabled() {
    static const bool on = [] { const char* e = getenv("WWHD_GX2_FRONTEND"); return !e || atoi(e) != 0; }();
    return on && g_render_thread;
}
static CommandRing<1 << 20> g_rt_ring;  // front end -> render thread
namespace fe {
static uint32 regs[kNumRegs];
static uint32* shadow = nullptr;
static std::unordered_map<uint32, std::vector<uint32>> contexts;
static RegBlocks live = [] { RegBlocks b; b.fill(); return b; }();
static std::unordered_map<uint32, ContextBlocks> contextUsed;
static ContextBlocks* shadowUsed = nullptr;
static std::array<std::array<uint64_t, 4>, kRegBlocks> equal{};
static uint64_t nextContextId = 1;
// registers whose render-thread value may differ from ours (OP_SET_PROJ_REGS: the renderer patches
// the projection for wider screens): writes to them are always forwarded, as OP_SET_REGS
static std::array<uint64_t, kNumRegs / 64> volatileRegs{};
static bool anyVolatile = false;
struct Bumps { uint32 flags = 0; uint64_t tex = 0, smp = 0; };  // flags: shader pipeline fetch targets translation
static Bumps bumps;
static std::mutex mutex;  // front-end state: the thread's batches vs save states

static void forward(Op op, const uint32* payload, uint32 n, const uint32* prefix = nullptr) {
    const uint32 count = n + (prefix != nullptr);
    g_rt_ring.lock();
    uint32* w = g_rt_ring.reserve(count + 1);
    w[0] = op | (count << 8);
    if (prefix) w[1] = *prefix;
    if (n) memcpy(w + 1 + (prefix != nullptr), payload, n * sizeof(uint32));
    const bool boundary = op == OP_FLUSH || op == OP_DRAW_DONE || op == OP_SWAP || op == OP_FENCE;
    g_rt_ring.commit(count + 1, boundary);
    g_rt_ring.unlock();
}
static void flush_bumps() {
    if (!bumps.flags && !bumps.tex && !bumps.smp) return;
    const uint32 w[5] = {bumps.flags, uint32(bumps.tex), uint32(bumps.tex >> 32), uint32(bumps.smp), uint32(bumps.smp >> 32)};
    forward(OP_GEN_BUMPS, w, 5);
    bumps = {};
}
static bool volatile_in(uint32 first, uint32 n) {
    if (!anyVolatile) return false;
    for (uint32 r = first; r < first + n; ++r)
        if (volatileRegs[r / 64] >> (r % 64) & 1) return true;
    return false;
}
// the context-shadow block bookkeeping of the single-thread path (equal_has, mark_written, ...)
static bool equal_has(uint32 block, uint64_t token) {
    for (uint64_t t : equal[block]) if (t == token) return true;
    return false;
}
static void equal_add(uint32 block, uint64_t token) {
    auto& e = equal[block];
    if (equal_has(block, token)) return;
    e[3] = e[2]; e[2] = e[1]; e[1] = e[0]; e[0] = token;
}
static ContextBlocks& new_context_blocks(uint32 ctx) {
    auto& c = contextUsed[ctx];
    c = {};
    c.id = nextContextId++;
    return c;
}
static void mark_written(uint32 first, uint32 n) {
    for (uint32 b = first >> 8, last = (first + n - 1) >> 8; b <= last; ++b) {
        live.set(b);
        if (shadowUsed) {
            const bool same = equal_has(b, shadowUsed->token(b));
            shadowUsed->used.set(b);
            ++shadowUsed->version[b];
            equal[b] = {};
            if (same) equal[b][0] = shadowUsed->token(b);
        } else {
            equal[b] = {};
        }
    }
}
// store + classify (apply_regs_unmarked of the single-thread path), forward the changed words
static void apply_unmarked(uint32 first, const uint32* v, uint32 n) {
    bool changed = false;
    if (first >= mmSQ_ALU_CONSTANT0_0 && first <= mmSQ_ALU_CONSTANT0_0 + 0x1000 &&
        n <= mmSQ_ALU_CONSTANT0_0 + 0x1000 - first) {
        changed = memcmp(regs + first, v, n * sizeof(uint32)) != 0;  // uniforms only: no generations
        if (changed) memcpy(regs + first, v, n * sizeof(uint32));
    } else {
        const auto& masks = register_masks();
        for (uint32 i = 0; i < n; ++i) {
            const uint32 reg = first + i, value = v[i], old = regs[reg];
            if (old == value) continue;
            changed = true;
            const auto& m = masks[reg];
            if (m.fetch) bumps.flags |= 4;
            if (m.targets) bumps.flags |= 8;
            if (m.texture) bumps.tex |= uint64_t{1} << (m.texture - 1);
            if (m.sampler) bumps.smp |= uint64_t{1} << (m.sampler - 1);
            const uint32 diff = old ^ value;
            if (diff & m.translation) bumps.flags |= 16;
            if (diff & m.pipeline) bumps.flags |= 2;
            if (m.baseline && (diff & m.shader)) bumps.flags |= 1;
            regs[reg] = value;
        }
    }
    if (shadow && shadow + first != v) memcpy(shadow + first, v, n * sizeof(uint32));
    if (!changed) return;
    if (volatile_in(first, n)) forward(OP_SET_REGS, v, n, &first);  // the renderer classifies against its value
    else forward(OP_RAW_REGS, v, n, &first);
}
static void set_regs(uint32 first, const uint32* v, uint32 n) {
    if (!n || first > kNumRegs || n > kNumRegs - first) return;
    // no change to the live registers or the active context shadow: nothing to do or forward
    if (!volatile_in(first, n) && !memcmp(regs + first, v, n * sizeof(uint32)) &&
        (!shadow || !memcmp(shadow + first, v, n * sizeof(uint32))))
        return;
    mark_written(first, n);
    apply_unmarked(first, v, n);
}
static void set_context(uint32 ctx) {
    auto it = ctx ? contexts.find(ctx) : contexts.end();
    if (it == contexts.end()) {
        shadow = nullptr;
        shadowUsed = nullptr;
        return;
    }
    shadow = it->second.data();
    auto found = contextUsed.find(ctx);
    if (found != contextUsed.end()) shadowUsed = &found->second;
    else { shadowUsed = &new_context_blocks(ctx); shadowUsed->used.fill(); }
    for (uint32 block = 0; block < kRegBlocks; ++block) {
        const uint64_t token = shadowUsed->token(block);
        if (block != kPrimBlock) {
            if (equal_has(block, token)) continue;
            if (!live.test(block) && !shadowUsed->used.test(block)) { equal_add(block, token); continue; }
        }
        const uint32* saved = shadow + block * 256;
        if (memcmp(regs + block * 256, saved, 256 * sizeof(uint32)) == 0) {
            equal_add(block, token);
            continue;
        }
        apply_unmarked(block * 256, saved, 256);
        equal[block] = {};
        equal[block][0] = token;
    }
    live = shadowUsed->used;
    live.set(kPrimBlock);
}
static void execute(const uint32* words, uint32 count, int depth) {
    for (uint32 i = 0; i < count;) {
        const uint32 hdr = words[i];
        const Op op = Op(hdr & 0xFF);
        const uint32 n = hdr >> 8;
        if (op >= OP_COUNT || n > count - i - 1 || (op == OP_SET_REGS && !n)) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        const uint32* p = words + i + 1;
        switch (op) {
        case OP_NOP: break;
        case OP_SET_REGS: set_regs(p[0], p + 1, n - 1); break;
        case OP_CALL:
            if (depth < 16) execute((const uint32*)mem::ptr(p[0]), p[1] / 4, depth + 1);
            else LOG("[gx2] display list nesting too deep at %08X", p[0]);
            break;
        case OP_SET_CONTEXT: set_context(p[0]); break;
        case OP_SETUP_CONTEXT:
            contexts[p[0]].assign(kNumRegs, 0);
            shadow = contexts[p[0]].data();
            shadowUsed = &new_context_blocks(p[0]);
            break;
        case OP_SET_PROJ_REGS:
            // the renderer applies the (possibly patched) values; keep ours unpatched and treat
            // these registers as unknown from now on
            if (n >= 2 && p[0] < kNumRegs) {
                const uint32 m = std::min<uint32>(n - 1, std::min<uint32>(16, kNumRegs - p[0]));
                mark_written(p[0], m);
                memcpy(regs + p[0], p + 1, m * sizeof(uint32));
                if (shadow) memcpy(shadow + p[0], p + 1, m * sizeof(uint32));
                for (uint32 r = p[0]; r < p[0] + m; ++r) volatileRegs[r / 64] |= uint64_t{1} << (r % 64);
                anyVolatile = true;
            }
            flush_bumps();
            forward(op, p, n);
            break;
        case OP_DRAW: case OP_DRAW_INDEXED: {
            flush_bumps();
            // gfxvk::draw stores the primitive type into the register file (not the shadow)
            const uint32 prim = p[0], cnt = n > 1 ? p[1] : 0, instances = op == OP_DRAW ? (n > 3 ? p[3] : 0) : (n > 5 ? p[5] : 0);
            if (cnt && instances && !((prim == 0x13 || prim == 0x14) && cnt < 4) &&
                !(regs[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22)))
                regs[REGADDR::VGT_PRIMITIVE_TYPE] = prim;
            forward(op, p, n);
            break;
        }
        default:
            flush_bumps();
            forward(op, p, n);
            break;
        }
        i += 1 + n;
    }
}
}  // namespace fe

static void frontend_thread_main() {
    host::set_thread_name("GX2 front end");
    switch_set_helper_thread(0, 0x2C);
    for (;;) {
        uint32 count;
        const uint32* words = g_q_ring.wait(count);
        {
            std::lock_guard<std::mutex> lk(fe::mutex);
            fe::execute(words, count, 0);
            fe::flush_bumps();
        }
        g_q_ring.complete(count);
        g_rt_ring.lock();
        g_rt_ring.flush();  // publish what this batch produced
        g_rt_ring.unlock();
    }
}
#endif

static void render_thread_main() {
    host::set_thread_name("GX2 render");
#ifdef __SWITCH__
    // CPU core 0 (the guest main thread is on core 1), above guest threads
    switch_set_helper_thread(getenv("WWHD_RENDER_CORE") ? atoi(getenv("WWHD_RENDER_CORE")) : 0, 0x2C);
    switch_note_render_thread();
#else
    host::boost_thread_priority();
#endif
    perf_hint::add_current_thread();
    for (;;) {
#ifdef __SWITCH__
        uint32 count;
        const uint64_t idleStart = now_ns();
        static auto& ring = frontend_enabled() ? g_rt_ring : g_q_ring;
        const uint32* words = ring.wait(count);
        const uint64_t idleEnd = now_ns();
        g_render_idle_ns.fetch_add(idleEnd - idleStart, std::memory_order_relaxed);
        if (frame_trace::on() && idleEnd - idleStart > 200000) {
            frame_trace::event_at('I', idleStart);
            frame_trace::event_at('i', idleEnd);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            host::with_autorelease_pool([&] { execute(words, count); });
        }
        ring.complete(count);
        switch_sample_core_idle();
#else
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            const uint64_t idleStart = now_ns();
            g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
            g_render_idle_ns.fetch_add(now_ns() - idleStart, std::memory_order_relaxed);
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            render::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
#endif
    }
}

// A prefix lets set_regs copy directly to the queue/display list, without TLS scratch.
static void enqueue(Op op, const uint32* payload, uint32 n, const uint32* prefix = nullptr) {
    static std::once_flag once;
    static std::atomic<bool> started{false};
    // call_once uses TLS/callback machinery even after startup on some hosts.
    // Keep it on the cold path while preserving safe concurrent first emission.
    if (!started.load(std::memory_order_acquire)) {
        std::call_once(once, [] {
            std::thread(render_thread_main).detach();
#ifdef __SWITCH__
            if (frontend_enabled()) std::thread(frontend_thread_main).detach();
#endif
        });
        started.store(true, std::memory_order_release);
    }
#ifdef __SWITCH__
    const uint32 count = n + (prefix != nullptr);
    g_q_ring.lock();
    uint32* w = g_q_ring.reserve(count + 1);
    w[0] = op | (count << 8);
    if (prefix) w[1] = *prefix;
    if (n) memcpy(w + 1 + (prefix != nullptr), payload, n * sizeof(uint32));
    // These commands can be followed by CPU sleeps/waits with no more emission.
    const bool boundary = op == OP_FLUSH || op == OP_DRAW_DONE || op == OP_SWAP || op == OP_FENCE;
    g_q_ring.commit(count + 1, boundary);
    g_q_ring.unlock();
#else
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_q_pending.push_back(op | ((n + (prefix != nullptr)) << 8));
    if (prefix) g_q_pending.push_back(*prefix);
    if (n) g_q_pending.insert(g_q_pending.end(), payload, payload + n);
    if (g_q_waiting) g_q_cv.notify_one();
#endif
}

// queue a fence; the render thread marks it done when it gets there
static uint64_t issue_fence() {
    // IDs must follow queue order even with concurrent sync callers.
    // The consumer never takes this lock (enqueue can wait for ring space).
    std::lock_guard<std::mutex> lk(g_fence_issue_mutex);
    const uint64_t id = ++g_fence_issued;
    uint32 w[2] = {uint32(id), uint32(id >> 32)};
    enqueue(OP_FENCE, w, 2);
    return id;
}
static void wait_fence(uint64_t id) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    const uint64_t syncStart = now_ns();
    frame_trace::event('Y');
    g_q_done_cv.wait(lk, [&] { return g_fence_done >= id; });
    frame_trace::event('y');
    g_main_sync_ns.fetch_add(now_ns() - syncStart, std::memory_order_relaxed);
}
// GX2DrawDone with one frame of render-thread lag (WWHD_DRAWDONE_LAG=1): the game waits for the
// frame before the last swap instead of everything queued, so it computes frame N+1 while the render
// thread still draws frame N (as with a GPU working a frame behind). Fences queued after each swap.
static bool drawdone_lag() {
    static const bool on = [] { const char* e = getenv("WWHD_DRAWDONE_LAG"); return e && atoi(e) > 0; }();
    return on && g_render_thread;
}
static std::mutex g_swap_fence_mutex;
static uint64_t g_swap_fences[2] = {0, 0};  // [0] older, [1] latest swap
// block the game thread until the render thread has executed everything queued so far
// debug: WWHD_SYNC_STATS=1 logs, every 5 s, how often each caller waited for the render thread to
// catch up (render_sync) and for how long
enum SyncSite { kSyncShutdown, kSyncFlip, kSyncDrawDone, kSyncVsyncUncapped, kSyncVsyncFlip, kSyncSaveState, kSyncCopySurface, kSyncSites };
static void sync_stat(int site, std::chrono::steady_clock::duration waited) {
    static const bool on = getenv("WWHD_SYNC_STATS") != nullptr;
    if (!on) return;
    static std::mutex mu;
    static uint64_t count[kSyncSites], ns[kSyncSites];
    static auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    count[site]++;
    ns[site] += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - t0).count();
    static const char* names[kSyncSites] = {"shutdown", "flip", "DrawDone", "vsync-uncapped", "vsync-flip", "savestate", "CopySurface"};
    char buf[400];
    int k = snprintf(buf, sizeof buf, "[gx2] render_sync per second:");
    for (int i = 0; i < kSyncSites; i++)
        if (count[i]) k += snprintf(buf + k, sizeof buf - k, " %s %.1f x %.2f ms", names[i], count[i] / secs, ns[i] / 1e6 / count[i]);
    LOG("%s", buf);
    memset(count, 0, sizeof count); memset(ns, 0, sizeof ns);
    t0 = now;
}
static void render_sync(int site = kSyncShutdown) {
    if (!g_render_thread) return;
    const auto started = std::chrono::steady_clock::now();
    struct Done { int site; std::chrono::steady_clock::time_point t; ~Done() { sync_stat(site, std::chrono::steady_clock::now() - t); } } done{site, started};
    wait_fence(issue_fence());
}

void emit(Op op, const uint32* payload, uint32 n) {
    Recording& rec = t_rec;
    if (rec.start) {
        uint64_t bytes = 4 * (uint64_t(n) + 1);
        if (uint64_t(rec.pos) + bytes > rec.end) {
            LOG("[gx2] display list overflow at %08X", rec.start);
            return;
        }
        uint32* w = (uint32*)mem::ptr(rec.pos);
        w[0] = op | (n << 8);
        if (n) memcpy(w + 1, payload, n * 4);
        rec.pos += uint32(bytes);
        return;
    }
    if (g_render_thread) {
        enqueue(op, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload, n); });
}

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload.begin(), (uint32)payload.size()); });
}

#ifdef WWHD_HAS_VULKAN
void checkpoint_vulkan_caches() {
    // Wait for queued work, then exclude further renderer mutations while the
    // SDL thread writes the final cache checkpoint during orderly shutdown.
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] {
        gfxvk::wait_idle();
        gfxvk::save_renderer_caches();
    });
}
#endif

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (!count) return;
    Recording& rec = t_rec; // one TLS lookup for this path
    if (rec.start) {
        const uint64_t bytes = (uint64_t(count) + 2) * sizeof(uint32);
        if (uint64_t(rec.pos) + bytes > rec.end) {
            LOG("[gx2] display list overflow at %08X", rec.start);
            return;
        }
        uint32* w = (uint32*)mem::ptr(rec.pos);
        w[0] = OP_SET_REGS | ((count + 1) << 8);
        w[1] = first;
        memcpy(w + 2, values, count * sizeof(uint32));
        rec.pos += uint32(bytes);
    } else if (g_render_thread) {
        enqueue(OP_SET_REGS, values, count, &first);
    } else {
        std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
        host::with_autorelease_pool([&] { apply_regs(first, values, count); });
    }
}
void set_reg(uint32 reg, uint32 value) { emit(OP_SET_REGS, {reg, value}); }

// WWHD_OP_STATS: per-frame command stream composition, logged with the FPS line (render thread counts)
struct OpStats { uint64_t noopPackets = 0, sameCalls = 0, sameAddrCalls = 0, draws[2] = {}, calls = 0, callWords = 0, regPackets[2] = {}, regWords[2] = {}, aluWords[2] = {}, words = 0; };
static OpStats g_op_stats;
static int g_call_depth = 0;
void log_op_stats(uint64_t frames) {
    OpStats s = g_op_stats;
    g_op_stats = {};
    if (!frames) return;
    LOG("[gx2 ops] per frame: display list calls identical to last frame %llu, same address+size %llu; no-op register packets %llu",
        (unsigned long long)(s.sameCalls / frames), (unsigned long long)(s.sameAddrCalls / frames),
        (unsigned long long)(s.noopPackets / frames));
    LOG("[gx2 ops] per frame: draws %llu top + %llu in display lists; %llu calls (%llu words); reg packets %llu/%llu "
        "(%llu/%llu words, ALU const %llu/%llu); words %llu",
        (unsigned long long)(s.draws[0] / frames), (unsigned long long)(s.draws[1] / frames),
        (unsigned long long)(s.calls / frames), (unsigned long long)(s.callWords / frames),
        (unsigned long long)(s.regPackets[0] / frames), (unsigned long long)(s.regPackets[1] / frames),
        (unsigned long long)(s.regWords[0] / frames), (unsigned long long)(s.regWords[1] / frames),
        (unsigned long long)(s.aluWords[0] / frames), (unsigned long long)(s.aluWords[1] / frames),
        (unsigned long long)(s.words / frames));
}
void execute(const uint32* words, uint32 count) {
    if (g_call_depth == 0) g_op_stats.words += count;
    uint32 i = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || n > count - i - 1 || (op == OP_SET_REGS && !n)) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        // Register packets dominate the stream; bypass the large command switch.
        if (op == OP_SET_REGS) {
            static const bool opStats = getenv("WWHD_OP_STATS") != nullptr;
            const uint32 first = words[i + 1], cnt = n - 1;
            const uint32* v = &words[i + 2];
            if (opStats) {
                const bool inCall = g_call_depth > 0;
                g_op_stats.regPackets[inCall]++; g_op_stats.regWords[inCall] += cnt;
                if (first >= mmSQ_ALU_CONSTANT0_0 && first < mmSQ_ALU_CONSTANT0_0 + 0x1000) g_op_stats.aluWords[inCall] += cnt;
            }
            // The game re-sends the same render state before every draw (~50,000 mostly single-register
            // packets a frame, most of them rewriting the value already there). A write that leaves both
            // the live registers and the active context shadow unchanged changes nothing: no generation
            // bump, and the block bookkeeping of mark_written (live/used bits, versions, equality tokens)
            // stays true as it is. Skip it.
            if (cnt <= 8 && first < kNumRegs && cnt <= kNumRegs - first &&
                !memcmp(g_regs + first, v, cnt * sizeof(uint32)) &&
                (!g_shadow || !memcmp(g_shadow + first, v, cnt * sizeof(uint32)))) {
                if (opStats) g_op_stats.noopPackets++;
            } else apply_regs(first, v, cnt);
        }
        else execute_one(op, &words[i + 1], n);
        i += 1 + n;
    }
}

static void set_context(uint32 ctx) {
    if (!ctx) {
        g_shadow = nullptr;
        g_shadow_used = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        g_shadow_used = nullptr;
        return;
    }
    g_shadow = it->second.data();
    auto found = g_context_used.find(ctx);
    if (found != g_context_used.end()) g_shadow_used = &found->second;
    else { g_shadow_used = &new_context_blocks(ctx); g_shadow_used->used.fill(); }  // unknown contents
    // Apply the saved registers through the classifying write path (exact change detection for
    // the draw-state generations), skipping blocks known equal or zero on both sides.
    for (uint32 block = 0; block < kRegBlocks; ++block) {
        const uint64_t token = g_shadow_used->token(block);
        if (block != kPrimBlock) {
            if (equal_has(block, token)) continue;
            if (!g_live.test(block) && !g_shadow_used->used.test(block)) { equal_add(block, token); continue; }
        }
        const uint32* saved = g_shadow + block * 256;
        if (memcmp(g_regs + block * 256, saved, 256 * sizeof(uint32)) == 0) {
            equal_add(block, token);  // unchanged g_regs: earlier tokens stay valid
            continue;
        }
        apply_regs_unmarked(block * 256, saved, 256);
        g_equal[block] = {};
        g_equal[block][0] = token;
    }
    g_live = g_shadow_used->used;
    g_live.set(kPrimBlock);
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}
static void clear_color_payload(const uint32* p, const float rgba[4]) {
#ifdef WWHD_HAS_VULKAN
    if (render::vulkan()) { gfxvk::clear_color_payload(p, rgba); return; }
#endif
    render::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
}
static void clear_depth_payload(const uint32* p, float depth, uint32 stencil, uint32 flags) {
#ifdef WWHD_HAS_VULKAN
    if (render::vulkan()) { gfxvk::clear_depth_stencil_payload(p, depth, stencil, flags); return; }
#endif
    render::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 1), depth, stencil, flags);
}

static bool lazy_draw_done() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_LAZY_DRAW_DONE");
#ifdef __ANDROID__
        return render::vulkan() && (!e || atoi(e) != 0);
#else
        return render::vulkan() && e && atoi(e) != 0;
#endif
    }();
    return on;
}

static void execute_one(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS: if (n) apply_regs(p[0], p + 1, n - 1); break;
    case OP_RAW_REGS:  // Switch GX2 front end: a classified change, store only
        if (n && p[0] <= kNumRegs && n - 1 <= kNumRegs - p[0]) memcpy(g_regs + p[0], p + 1, (n - 1) * sizeof(uint32));
        break;
    case OP_GEN_BUMPS:
        if (n >= 5) {
            if (p[0] & 1) ++g_shader_state_gen;
            if (p[0] & 2) ++g_pipeline_state_gen;
            if (p[0] & 4) ++drawStateGenerations.fetch;
            if (p[0] & 8) ++drawStateGenerations.targets;
            if (p[0] & 16) ++drawStateGenerations.translation;
            for (uint64_t bits = uint64_t(p[1]) | uint64_t(p[2]) << 32; bits; bits &= bits - 1) {
                const uint32 slot = __builtin_ctzll(bits);
                if (slot < 36) ++drawStateGenerations.textures[slot / 18][slot % 18];
            }
            for (uint64_t bits = uint64_t(p[3]) | uint64_t(p[4]) << 32; bits; bits &= bits - 1) {
                const uint32 slot = __builtin_ctzll(bits);
                if (slot < 54) ++drawStateGenerations.samplers[slot];
            }
        }
        break;
    case OP_DRAW: g_op_stats.draws[g_call_depth > 0]++; render::draw(g_regs, p[0], p[1], 0, 0, p[2], p[3]); break;
    case OP_DRAW_INDEXED: g_op_stats.draws[g_call_depth > 0]++; render::draw(g_regs, p[0], p[1], p[2], p[3], p[4], p[5]); break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        clear_color_payload(p, rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        clear_depth_payload(p, bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        clear_color_payload(p, rgba);
        clear_depth_payload(p + kColorBufferWords, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        // debug: WWHD_GX2_DELAY_COPY=ms stalls the render thread before each surface copy (a slow
        // or busy render thread; reproduced the agl boot crash every time before GX2CopySurface waited)
        static const int delay = getenv("WWHD_GX2_DELAY_COPY") ? atoi(getenv("WWHD_GX2_DELAY_COPY")) : 0;
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const uint32* q = p + kSurfaceWords;
        const uint32* r = q + 2 + kSurfaceWords;
#ifdef WWHD_HAS_VULKAN
        if (render::vulkan()) gfxvk::copy_surface_payload(p, q[0], q[1], q + 2, r[0], r[1]);
        else
#endif
        render::copy_surface(unpack_struct(p, kSurfaceWords, 0), q[0], q[1],
                             unpack_struct(q + 2, kSurfaceWords, 1), r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN:
#ifdef WWHD_HAS_VULKAN
        if (render::vulkan()) gfxvk::copy_to_scan_payload(p, p[kColorBufferWords]);
        else
#endif
        render::copy_to_scan(unpack_struct(p, kColorBufferWords, 0), p[kColorBufferWords]);
        break;
    case OP_CALL:
        g_op_stats.calls++; g_op_stats.callWords += p[1] / 4;
        {   // WWHD_OP_STATS: is this display list byte-identical to one called last frame?
            static const bool stats = getenv("WWHD_OP_STATS") != nullptr;
            static std::unordered_map<uint64_t, uint64_t> prev, cur;  // (address<<32|size) -> content hash
            static uint64_t curFrame = ~0ull;
            if (stats) {
                const uint64_t f = render::frame_count();
                if (f != curFrame) { prev.swap(cur); cur.clear(); curFrame = f; }
                const uint32* w = (const uint32*)mem::ptr(p[0]);
                uint64_t h = 1469598103934665603ull;
                for (uint32 k = 0; k < p[1] / 4; ++k) h = (h ^ w[k]) * 1099511628211ull;
                const uint64_t key = uint64_t(p[0]) << 32 | p[1];
                auto it = prev.find(key);
                if (it != prev.end()) { g_op_stats.sameAddrCalls++; if (it->second == h) g_op_stats.sameCalls++; }
                cur[key] = h;
            }
        }
        g_call_depth++;
        execute((const uint32*)mem::ptr(p[0]), p[1] / 4);
        g_call_depth--;
        break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: render::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_FLUSH: render::guest_flush(); break;  // Vulkan: asynchronous submission
    case OP_DRAW_DONE: {
        // The Vulkan renderer never writes GPU results back to guest memory (guest data is copied
        // into fenced upload slices when work is recorded; surface/feedback copies stay on the GPU;
        // CPU readers wait on their own submission fences), so GX2DrawDone needs this op executed
        // (render_sync in the HLE), not an idle GPU: the work is only queued. An idle GPU still
        // comes with n (save states) or without WWHD_DRAWDONE_CPU (desktop default; the Switch and,
        // with WWHD_VK_LAZY_DRAW_DONE, Android only queue).
#ifdef __SWITCH__
        static const bool cpu_only = [] {
            const char* value = getenv("WWHD_DRAWDONE_CPU");
            return !value || strcmp(value, "0");
        }();
#else
        static const bool cpu_only = getenv("WWHD_DRAWDONE_CPU") != nullptr || lazy_draw_done();
#endif
        if (n || !cpu_only) render::wait_idle();
        else render::guest_flush();
        break;
    }
    case OP_SWAP:
        if (n) render::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        frame_trace::event('P');
        render::swap();
        frame_trace::event('p');
        break;
    case OP_SET_PROJ_REGS: {
        float kx, ky;
        if (n == 17 && render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky)) {
            uint32 v[16];
            memcpy(v, p + 1, sizeof v);
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = fbits(bitsf(v[i]) / kx);
                v[4 + i] = fbits(bitsf(v[4 + i]) / ky);
            }
            apply_regs(p[0], v, 16);
        } else if (n) apply_regs(p[0], p + 1, std::min<uint32>(n - 1, 16));
        break;
    }
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        aspect::layout_root_target(p[0], render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky));
        break;
    }
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        g_shadow_used = &new_context_blocks(p[0]);  // g_regs is not loaded: no equality token
        break;
    case OP_FENCE: {
        if (!n) break;
        std::lock_guard<std::mutex> lk(g_q_mutex);
        const uint64_t id = uint64_t(p[0]) | (n > 1 ? uint64_t(p[1]) << 32 : 0);
        g_fence_done = std::max(g_fence_done, id);
        g_q_done_cv.notify_all();
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
static uint64_t g_swap_count = 0, g_flip_count = 0;
namespace gx2 { uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); } }  // live fps in the title
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); }
static std::mutex g_flip_mutex;
static const auto g_vsync_epoch = std::chrono::steady_clock::now();
static constexpr std::chrono::nanoseconds kVsyncPeriod(16683333);  // 59.94 Hz
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
struct PendingFlip { uint64_t vsync, swap; };
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

// Late-swap grace (WWHD_VSYNC_GRACE_MS, Switch default 4): a swap a little after the vsync its flip
// was due at delays the vsync clock instead of losing a whole vsync, like a variable refresh display.
// A game whose frames take 31-35 ms then runs at ~30 fps instead of dropping to 20-25 (every late
// frame cost 50 ms). The clock only ever moves later: swaps and flips are never early.
static std::atomic<int64_t> g_vsync_delay_ns{0};
static uint64_t g_grace_delays = 0, g_grace_ns = 0;  // late swaps absorbed, and by how much (statistics)
static std::chrono::steady_clock::time_point vsync_origin() {
    return g_vsync_epoch + std::chrono::nanoseconds(g_vsync_delay_ns.load(std::memory_order_relaxed));
}
static uint64_t vsync_index() { return (std::chrono::steady_clock::now() - vsync_origin()) / kVsyncPeriod; }
#ifdef WWHD_HAS_VULKAN
// Vulkan diagnostics (docs/vulkan.md); never with the Metal renderer
static bool uncapped_benchmark() {
    static const bool enabled = [] {
        const char* value = getenv("WWHD_VK_UNCAPPED");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    return enabled;
}
#endif

static void update_flips() {  // g_flip_mutex held
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        uint64_t at = std::max(g_pending_flips.front().vsync + 1, g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval));
#ifdef WWHD_HAS_VULKAN
        if ((!uncapped_benchmark() && at > now) || render::frames_completed() < g_pending_flips.front().swap) break;
#else
        if (at > now || render::frames_completed() < g_pending_flips.front().swap) break;
#endif
        at = now;
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_time = timebase::now();
        g_flip_count++;
    }
}
// Before queueing a swap (g_flip_mutex held): if the previous frame has flipped and this swap missed
// the vsync its flip was due at by at most the grace, delay the vsync clock so that vsync is just
// ahead. Swaps late by more (or with flips pending) keep the hardware timing.
static void late_swap_grace() {
    static const int64_t graceNs = [] {
        const char* value = getenv("WWHD_VSYNC_GRACE_MS");
#ifdef __SWITCH__
        const double ms = value ? atof(value) : 4.0;
#else
        const double ms = value ? atof(value) : 0.0;
#endif
        return int64_t(std::max(0.0, std::min(ms, 8.0)) * 1e6);
    }();
#ifdef WWHD_HAS_VULKAN
    if (uncapped_benchmark()) return;
#endif
    // debug: WWHD_VSYNC_GRACE_AB=1 applies the grace only in every other 60-frame window (A/B test)
    static const bool ab = getenv("WWHD_VSYNC_GRACE_AB") != nullptr;
    const uint32_t interval = interp::effective_swap_interval(g_swap_interval);
    if (!graceNs || interval < 2 || !g_flip_count || !g_pending_flips.empty() || (ab && (g_swap_count / 60) % 2))
        return;
    const int64_t since = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - vsync_origin()).count();
    const uint64_t vsync = uint64_t(since) / uint64_t(kVsyncPeriod.count());
    const int64_t late = since - int64_t(vsync) * int64_t(kVsyncPeriod.count());
    if (g_last_flip_vsync + interval != vsync || late > graceNs) return;
    // vsync `vsync` moves to 0.1 ms from now; the clock reads vsync - 1 until then
    g_vsync_delay_ns.fetch_add(late + 100000, std::memory_order_relaxed);
    ++g_grace_delays;
    g_grace_ns += uint64_t(late);
}
#ifdef WWHD_HAS_VULKAN
static void ready_flip_before_resume() {
    bool needsSync;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        if(g_pending_flips.empty()) return;
        const auto& front = g_pending_flips.front();
        const uint64_t at = std::max(front.vsync + 1,
            g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval));
        if(at > vsync_index()) return;
        needsSync = render::frames_completed() < front.swap;
    }
    if(needsSync) render_sync(kSyncFlip); // Core already released; no flip lock held.
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
}
#endif

HLE(gx2, GX2Init) {
    set_default_state();
    LOG("[gx2] initialized (native GX2 -> %s)", render::api_name(render::active()));
#ifdef WWHD_HAS_VULKAN
    if(uncapped_benchmark())
        LOG("[gx2 benchmark] uncapped guest flips; GPU completion ordering retained; frame-based simulation accelerates while timebase/audio clocks remain real-time");
#endif
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    t_rec.start = t_rec.pos = arg(c, 0);
    t_rec.end = arg(c, 0) + arg(c, 1);
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = t_rec.pos - t_rec.start;
    t_rec = Recording{};
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    if (arg(c, 0)) st32(arg(c, 0), t_rec.start);
    if (arg(c, 1)) st32(arg(c, 1), t_rec.start ? t_rec.end - t_rec.start : 0);
    ret(c, t_rec.start != 0);
}
HLE(gx2, GX2CallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }
HLE(gx2, GX2DirectCallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(uint32* p, uint32 addr, uint32 words) {
    if (addr) memcpy(p, mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::array<uint32, kColorBufferWords + 4> p{};
    put_struct(p.data(), arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p[kColorBufferWords + i] = fbits(farg(c, i));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::array<uint32, kDepthBufferWords + 3> p{};
    put_struct(p.data(), arg(c, 0), kDepthBufferWords);
    p[kDepthBufferWords] = fbits(farg(c, 0));
    p[kDepthBufferWords + 1] = arg(c, 1) & 0xFF;
    p[kDepthBufferWords + 2] = arg(c, 2);
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::array<uint32, kColorBufferWords + kDepthBufferWords + 7> p{};
    put_struct(p.data(), arg(c, 0), kColorBufferWords);
    put_struct(p.data() + kColorBufferWords, arg(c, 1), kDepthBufferWords);
    constexpr uint32 at = kColorBufferWords + kDepthBufferWords;
    for (int i = 0; i < 5; i++) p[at + i] = fbits(farg(c, i));
    p[at + 5] = arg(c, 2) & 0xFF;
    p[at + 6] = arg(c, 3);
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
HLE(gx2, GX2CopySurface) {
    // debug: WWHD_COPYDBG=1 logs each copy as issued (thread, caller, source and destination images)
    static const bool dbg = getenv("WWHD_COPYDBG") != nullptr;
    if (dbg)
        LOG("[copydbg] issue t=%.3f thread %08X lr %08X src %08X img %08X dst %08X img %08X size %X", timebase::now() / (double)timebase::kTicksPerSec,
            threads::current_thread(), c->lr, arg(c, 0), ld32(arg(c, 0) + 0x24), arg(c, 3), ld32(arg(c, 3) + 0x24), ld32(arg(c, 3) + 0x20));
    std::array<uint32, 2 * kSurfaceWords + 4> p{};
    put_struct(p.data(), arg(c, 0), kSurfaceWords);
    p[kSurfaceWords] = arg(c, 1);
    p[kSurfaceWords + 1] = arg(c, 2);
    put_struct(p.data() + kSurfaceWords + 2, arg(c, 3), kSurfaceWords);
    p[2 * kSurfaceWords + 2] = arg(c, 4);
    p[2 * kSurfaceWords + 3] = arg(c, 5);
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // The copy is complete when GX2CopySurface returns: the game uses the result (and frees the
    // surfaces) right away. agl's tile-mode conversion (027B5EEC) copies into a temporary surface,
    // OSBlockMoves it back and frees it at once; executed later on the render thread, the copy
    // wrote into the freed memory after the heap had reused it (boot crash: agl shader program
    // array 21EFE28C, program 0's +0x7c zeroed). Not for display lists (they run when called).
    if (!t_rec.start) {
        BlockingScope b;
        render_sync(kSyncCopySurface);
    }
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::array<uint32, kColorBufferWords + 1> p{};
    put_struct(p.data(), arg(c, 0), kColorBufferWords);
    p[kColorBufferWords] = arg(c, 1);
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) {
    static const bool trace = getenv("WWHD_TRACE_INVALIDATE") != nullptr;
    static uint32_t traced = 0;
    if (trace && (arg(c, 0) & 8) && traced < 200) {
        ++traced;
        LOG("[gx2] GX2Invalidate flags %X addr %08X size %X lr %08X", arg(c, 0), arg(c, 1), arg(c, 2), c->lr);
    }
    emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)});
}

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { emit_host(OP_FLUSH, {}); }
HLE(gx2, GX2DrawDone) {
    frame_trace::event('D');
    {
        BlockingScope b;
        emit_host(OP_DRAW_DONE, {});
        uint64_t older = 0;
        if (drawdone_lag()) {
            std::lock_guard<std::mutex> lk(g_swap_fence_mutex);
            older = g_swap_fences[0];
        }
        if (older) wait_fence(older);
        else render_sync(kSyncDrawDone);
    }
    frame_trace::event('d');
    ret(c, 1);
}
HLE(gx2, GX2SwapScanBuffers) {
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    uint32 ab;
    memcpy(&ab, &a, 4);
    emit_host(OP_SWAP, {ab});
    if (drawdone_lag()) {
        const uint64_t id = issue_fence();
        std::lock_guard<std::mutex> lk(g_swap_fence_mutex);
        g_swap_fences[0] = g_swap_fences[1];
        g_swap_fences[1] = id;
    }
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        late_swap_grace();
        g_swap_count++;
        g_pending_flips.push_back({vsync_index(), g_swap_count});
    }
    // debug: WWHD_LOG_SLOW_SWAP=ms logs swaps that came more than ms after the previous one
    static const double slow_ms = getenv("WWHD_LOG_SLOW_SWAP") ? atof(getenv("WWHD_LOG_SLOW_SWAP")) : 0;
    if (slow_ms > 0) {
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        if (ms > slow_ms) LOG("[gx2] slow swap %llu: %.1f ms", (unsigned long long)g_swap_count, ms);
    }
    if (g_swap_count == 1) frame_trace::set_game_thread();
    frame_trace::swap(g_swap_count);
    static const uint64_t fps_every = getenv("WWHD_FPS_EVERY") ? std::max(1, atoi(getenv("WWHD_FPS_EVERY"))) : 300;
    if (g_swap_count % fps_every == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u; late swaps absorbed %llu (%.1f ms)",
            (unsigned long long)g_swap_count, fps_every / s, g_swap_interval,
            (unsigned long long)g_grace_delays, g_grace_ns / 1e6);
        g_grace_delays = 0; g_grace_ns = 0;
#ifdef __SWITCH__
        switch_log_clocks();
        switch_log_thread_cpu(fps_every);
#endif
        if (getenv("WWHD_SCHED_STATS")) threads::report_sched();
        if (getenv("WWHD_OP_STATS")) log_op_stats(fps_every);  // racy read of render-thread counters: statistics only
    }
}
HLE(gx2, GX2GetSwapStatus) {
    frame_trace::event('G');
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(g_flip_count + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
HLE(gx2, GX2WaitForVsync) {
    frame_trace::event('V');
    struct VsyncTraceEnd { ~VsyncTraceEnd() { frame_trace::event('v'); } } vsyncTraceEnd;
#ifdef WWHD_HAS_VULKAN
    if(uncapped_benchmark()) {
        BlockingScope b;
        // The queue fence follows earlier swaps, whose presentation path waits
        // for GPU completion. Never wait while holding the flip mutex.
        render_sync(kSyncVsyncUncapped);
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        return;
    }
    static const bool readyFlipWait = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_WAIT");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    if(readyFlipWait) {
        bool eligible = false, needsSync = false;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            if(!g_pending_flips.empty()) {
                const auto& front = g_pending_flips.front();
                const uint64_t at = std::max(front.vsync + 1,
                    g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval));
                eligible = at <= vsync_index();
                if(eligible) needsSync = render::frames_completed() < front.swap;
            }
        }
        if(eligible) {
            if(needsSync) {
                BlockingScope b;
                render_sync(kSyncVsyncFlip); // Queued swap completion; never hold flip mutex.
            }
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips(); // Retains minimum interval and FIFO GPU guards.
            return;
        }
    }
#endif
    static const bool preciseSleep = [] {
#ifdef WWHD_HAS_VULKAN
        // Vulkan renderer's pacing (docs/vulkan.md); the Metal renderer keeps plain sleeping
        if (!render::vulkan()) return false;
        const char* value = getenv("WWHD_VSYNC_PRECISE");
#ifdef __APPLE__
        // Avoid the measured macOS sleep overshoot; explicit zero opts out.
        return !value || atoi(value) != 0;
#else
        return value && atoi(value) != 0;
#endif
#else
        return false;
#endif
    }();
    const auto deadline = vsync_origin() + kVsyncPeriod * (vsync_index() + 1);
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipPark = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_PARK");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    threads::park_sleep_until(deadline, preciseSleep,
        readyFlipPark ? ready_flip_before_resume : nullptr);
#else
    threads::park_sleep_until(deadline, preciseSleep);
#endif
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    static uint64_t calls = 0;
    if (getenv("WWHD_LOG_VSYNC") && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    LOG("[gx2] TV buffer format %X", arg(c, 3));
    render::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { render::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states
#include "../savestate.h"

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {1}); // Save-state replacement requires an explicit GPU drain.
    render_sync(kSyncSaveState);
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    // the register file and context shadows live in the GX2 front end when it runs (Switch)
    const uint32* regsSrc = g_regs;
    auto* contexts = &g_contexts;
    const uint32* shadowSrc = g_shadow;
#ifdef __SWITCH__
    std::unique_lock<std::mutex> feLock;
    if (frontend_enabled()) {
        feLock = std::unique_lock<std::mutex>(fe::mutex);
        regsSrc = fe::regs; contexts = &fe::contexts; shadowSrc = fe::shadow;
    }
#endif
    w.u32(kNumRegs);
    w.bytes(regsSrc, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : *contexts) {
        keys.push_back(k);
        if (shadowSrc == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)(*contexts)[k].size());
        w.bytes((*contexts)[k].data(), (*contexts)[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    r.bytes(g_regs, sizeof g_regs);
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_live.fill();
    g_equal = {};
    g_context_used.clear();
    for (auto& [k, v] : g_contexts) {
        if (v.size() != kNumRegs) v.resize(kNumRegs);  // set_context copies whole blocks
        new_context_blocks(k).used.fill();
    }
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_shadow_used = g_shadow ? &g_context_used[active] : nullptr;
#ifdef __SWITCH__
    if (frontend_enabled()) {
        // hand the loaded state to the front end; the render thread keeps only its register file
        std::lock_guard<std::mutex> feLock(fe::mutex);
        memcpy(fe::regs, g_regs, sizeof g_regs);
        fe::contexts = std::move(g_contexts);
        g_contexts.clear();
        fe::contextUsed.clear();
        fe::live.fill();
        fe::equal = {};
        fe::volatileRegs = {};
        fe::anyVolatile = false;
        for (auto& [k, v] : fe::contexts) fe::new_context_blocks(k).used.fill();
        auto feIt = fe::contexts.find(active);
        fe::shadow = active && feIt != fe::contexts.end() ? feIt->second.data() : nullptr;
        fe::shadowUsed = fe::shadow ? &fe::contextUsed[active] : nullptr;
        fe::bumps = {};
        g_shadow = nullptr;
        g_shadow_used = nullptr;
        g_context_used.clear();
    }
#endif
    g_shader_state_gen++;
    g_pipeline_state_gen++;
    drawStateGenerations.invalidate();
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    render::ss_reset();  // the renderer forgets surface contents and shader memos (Vulkan)
}
