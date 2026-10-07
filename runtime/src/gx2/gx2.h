// Internal interfaces between the GX2 layer and the renderer.
#pragma once
#include <atomic>
#include <cstdint>

struct LatteFetchShader;
namespace GX2 {
struct GX2ColorBuffer;
struct GX2DepthBuffer;
}

namespace gx2 {
// Render-thread-owned stamps for the tracked GX2 register file only.
struct DrawStateGenerations {
    uint64_t fetch = 1, targets = 1, translation = 1;
    uint64_t textures[2][18]{};
    uint64_t samplers[54]{};
    void invalidate() {
        ++fetch; ++targets; ++translation;
        for (auto& stage : textures) for (auto& unit : stage) ++unit;
        for (auto& sampler : samplers) ++sampler;
    }
};
extern DrawStateGenerations drawStateGenerations;
struct ShaderKeyDirtyStats {
    uint64_t changedBatches = 0, baselineWouldBumps = 0, actualBumps = 0;
    uint64_t avoidedBumps = 0, maskedWords = 0;
};
ShaderKeyDirtyStats shader_key_dirty_stats();
void log_bump_regs(uint64_t frames);
extern std::atomic<uint64_t> g_render_idle_ns, g_main_sync_ns;  // slow-frame diagnostics // Render-thread diagnostics.
uint32_t color_buffer_address(const GX2::GX2ColorBuffer* cb);
LatteFetchShader* build_fetch_shader(uint32_t program);  // from our encoded fetch "program"
}  // namespace gx2

// The renderer backend (Metal). All calls come from the thread executing GX2
// commands, in submission order. Guest structures are passed by guest address.
namespace gx2 {
constexpr uint32_t kDepthSlicesReg = 0xA002;  // our convention (unused register): depth buffer array size
}

namespace gfx {
void init();                     // create device; call on the main thread before the game starts
void run_main_loop();            // window/event loop; runs on the main thread, never returns
void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances);
void clear_color(const uint32_t* regs, uint32_t colorBuffer, const float rgba[4]);
void clear_depth_stencil(const uint32_t* regs, uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice);
void copy_to_scan(uint32_t colorBuffer, uint32_t target);  // target: 1 = TV, 4 = DRC (GamePad)
void swap();                     // present the TV scan buffer
void set_frame_aspect(float a);  // aspect ratio of the TV picture from the next frame on (aspect.cpp)
// render thread: a target of this guest size is made wider/taller this frame (kx, ky != 1)
bool target_aspect_factors(uint32_t w, uint32_t h, float& kx, float& ky);
uint64_t frames_completed();     // swaps whose GPU work has finished
void with_autorelease_pool(void (*fn)());  // render thread: drain Objective-C temporaries per batch
void set_tv_format(uint32_t gx2Format, bool tv);  // GX2SetTVBuffer / GX2SetDRCBuffer
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
void flush();                    // submit queued GPU work
void wait_idle();                // GX2DrawDone: block until the GPU finished
}  // namespace gfx
