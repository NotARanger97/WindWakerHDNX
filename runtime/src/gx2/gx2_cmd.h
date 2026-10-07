// GX2 command stream.
//
// Every GX2 API call that affects the GPU produces a command. Outside of
// display-list recording, commands execute immediately against the current
// register file and the renderer. While a display list is being recorded
// (GX2BeginDisplayListEx .. GX2EndDisplayList), commands are appended to the
// guest buffer instead and execute whenever the list is called.
//
// The encoding is our own (native-endian 32-bit words):
//   word 0: op | (payload word count << 8)
//   word 1..n: payload
#pragma once
#include <cstdint>
#include <initializer_list>

namespace gx2 {

enum Op : uint32_t {
    OP_NOP = 0,
    OP_SET_REGS,        // first register, values...
    OP_DRAW,            // prim, count, baseVertex, instances
    OP_DRAW_INDEXED,    // prim, count, indexType, indexAddr, baseVertex, instances
    OP_CLEAR_COLOR,     // GX2ColorBuffer*, r, g, b, a (float bits)
    OP_CLEAR_DEPTH,     // GX2DepthBuffer*, depth (float bits), stencil, flags
    OP_CLEAR_BUFFERS,   // GX2ColorBuffer*, GX2DepthBuffer*, r, g, b, a, depth, stencil, flags
    OP_COPY_SURFACE,    // src GX2Surface*, srcMip, srcSlice, dst GX2Surface*, dstMip, dstSlice
    OP_COPY_TO_SCAN,    // GX2ColorBuffer*, target (1 = TV, 4 = DRC)
    OP_CALL,            // display list address, size in bytes
    OP_SET_CONTEXT,     // GX2ContextState* (0 = none)
    OP_INVALIDATE,      // flags, address, size
    OP_EXPAND_COLOR,    // GX2ColorBuffer*
    OP_EXPAND_DEPTH,    // GX2DepthBuffer*
    // host-only (render queue)
    OP_FLUSH,           // submit queued GPU work
    OP_DRAW_DONE,       // wait for the GPU to finish
    OP_SWAP,            // present
    OP_SETUP_CONTEXT,   // GX2ContextState*: allocate shadow state and make it current
    OP_FENCE,           // fence id: the game thread waits until the render thread got here
    // aspect ratio (aspect.cpp), appended to keep the numbers of recorded display lists
    OP_SET_PROJ_REGS,   // first register, 16 values: a layout projection matrix (narrowed when drawing to the TV)
    OP_LAYOUT_ROOT,     // nw::lyt root pane: drawn into the target bound now (which screen it goes to)
    // host-only, Switch GX2 front end -> render thread (gx2_core.cpp)
    OP_RAW_REGS,        // first register, values: store only (the front end classified the change)
    OP_GEN_BUMPS,       // flags, texture slot bits lo/hi, sampler slot bits lo/hi: draw-state generations to bump
    OP_COUNT
};

// emit a command (records into the active display list, or executes now)
void emit(Op op, const uint32_t* payload, uint32_t n);
inline void emit(Op op, std::initializer_list<uint32_t> payload) { emit(op, payload.begin(), (uint32_t)payload.size()); }

// register writes
void set_reg(uint32_t reg, uint32_t value);
void set_regs(uint32_t first, const uint32_t* values, uint32_t count);

// the live register file used for execution
uint32_t* regs();

// execute a sequence of encoded commands
void execute(const uint32_t* words, uint32_t count);

// float <-> bits helpers for command payloads
inline uint32_t fbits(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
inline float bitsf(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }

}  // namespace gx2
