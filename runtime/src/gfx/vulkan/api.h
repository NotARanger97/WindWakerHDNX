// The Vulkan renderer's entry points (namespace gfxvk), the counterpart of the Metal renderer's
// gfx:: functions in gx2/gx2.h. The GX2 layer reaches them through the renderer table
// (gfx/renderer.h, backend_table.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace gfxvk {
void init();                     // SDL host: windows + device; AppKit host: see init_appkit
void run_main_loop();            // SDL host only (the AppKit host runs [NSApp run])
void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances);
void clear_color(const uint32_t* regs, uint32_t colorBuffer, const float rgba[4]);
void clear_depth_stencil(const uint32_t* regs, uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice);
void copy_to_scan(uint32_t colorBuffer, uint32_t target);  // target: 1 = TV, 4 = DRC (GamePad)
// Borrow immutable command payloads synchronously; no guest scratch copy.
void clear_color_payload(const void* colorBuffer, const float rgba[4]);
void clear_depth_stencil_payload(const void* depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface_payload(const void* src, uint32_t srcMip, uint32_t srcSlice,
                          const void* dst, uint32_t dstMip, uint32_t dstSlice);
void copy_to_scan_payload(const void* colorBuffer, uint32_t target);
void swap();
void set_frame_aspect(float a);  // aspect ratio of the TV picture from the next frame on (aspect.cpp)
bool target_aspect_factors(uint32_t w, uint32_t h, float& kx, float& ky);
uint64_t frames_completed();
void with_autorelease_pool(void (*fn)());
void set_tv_format(uint32_t gx2Format, bool tv);
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
void flush();                    // Switch / WWHD_VK_ASYNC=1: submit; otherwise drain
void flush_async();              // GX2Flush: submit without waiting
void flush_readback();           // submit and wait for the CPU readback's fence
void wait_idle();                // explicit drain: shutdown, resize, save states
uint64_t frame_count();
void request_tv_dump(const std::string& path, int frames_ahead);
void request_capture();
void ss_reset_surfaces();
void save_renderer_caches();
int renderer_smoke_test();
namespace vk { void reset_shader_memoization(); }
void reset_draw_state_cache();  // Also called when shader memoization is invalidated.

#if defined(__APPLE__) && !defined(WWHD_SDL_HOST)
// AppKit host (gfx/display.mm): the TV and GamePad views' CAMetalLayers (drc may be null)
void init_appkit(void* tvLayer, void* drcLayer);
void screen_changed(int screen, bool visible);  // 0 TV, 1 GamePad: resized / occluded
#endif
}  // namespace gfxvk
#ifdef __SWITCH__
namespace gfxvk::vk { void revalidate_programs(); }  // shaders.cpp
#endif
