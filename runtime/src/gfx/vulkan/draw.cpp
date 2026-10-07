// Vulkan draw submission. Guest state conventions follow Cemu (MPL-2.0).
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "backend.h"
#include "runtime.h"
#include "shaders.h"
#include "settings.h"
#include "vertex_formats.h"
#include "uniform_snapshot.h"
#include "cpu_snapshot.h"
#include "word_cache.h"
#include "index_conversion.h"
#include "vertex_history.h"
#include "vertex_snapshot_history.h"
#include "cache_key.h"
#include "draw_options.h"
#ifdef __SWITCH__
#include "platform/switch/vk_record_thread.h"
#endif
#include "gx2/gx2_cmd.h"
#include "gx2/gx2.h"
#include "gx2/gx2_regs.h"
#ifdef __SWITCH__
#include "gx2/shader_program_writes.h"
#endif
#include <map>
#include <tuple>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <filesystem>
#include <type_traits>
#include "platform/host.h"
extern "C" uint64_t g_shader_state_gen;
// draws received from the game (Switch CPU log: per-frame count next to the thread times)
extern "C" { std::atomic<uint64_t> g_vk_draw_calls{0}; }
extern "C" uint64_t g_pipeline_state_gen;
using namespace Latte;
namespace gfxvk {
namespace {
[[gnu::always_inline]] inline bool preparation_stats_enabled() {
  static const bool enabled=std::getenv("WWHD_VK_STATS")!=nullptr;
  return enabled;
}
// Lazy: env.txt is applied after static initialization on the Switch.
[[gnu::always_inline]] inline bool state_reuse_enabled() {
  static const bool enabled=draw_option_enabled("WWHD_VK_STATE_REUSE");
  return enabled;
}
struct ResolvedShaders {
  const uint32_t* regs = nullptr;
  uint64_t validation=0, fetchGeneration=0, shaderGeneration=0, fsKey=0;
  uint32_t primitive=0;
  LatteFetchShader* fetch=nullptr;
  vk::Shader *vs=nullptr,*ps=nullptr;
};
ResolvedShaders resolvedShaders;
struct ResolvedTargets {
  const uint32_t* regs=nullptr;
  vk::Shader* ps=nullptr;
  uint64_t generation=0,epoch=0,selection=0,frame=0;
  std::array<Surface*,8> colors{};
  std::array<uint32_t,8> slices{};
  Surface* depth=nullptr;
  uint32_t depthSlice=0;
};
ResolvedTargets resolvedTargets;
struct ResolvedTexture {
  const uint32_t* regs=nullptr;
  uint64_t generation=0,epoch=0,addressGeneration=0;
  bool compare=false;
  Surface* surface=nullptr;
  Surface* viewSurface=nullptr;
  uint64_t viewEpoch=0,viewGeneration=0;
  VkImageView view=VK_NULL_HANDLE;
  VkDescriptorImageInfo descriptor{};
  VkDevice descriptorDevice=VK_NULL_HANDLE;
  uint64_t descriptorEpoch=0,samplerGeneration=0;
  uint32_t samplerSlot=0;
  bool samplerPatched=false,samplerCompare=false,samplerInteger=false,samplerAniso=false;
  bool image_matches(VkDevice device, uint64_t epoch, VkImageView sampledView,
                     uint32_t slot, uint64_t samplerGen, bool patched,
                     bool compareMode, bool integer, bool forceAniso) const {
    return descriptor.sampler && descriptor.imageView == sampledView && descriptorDevice == device &&
        descriptorEpoch == epoch && samplerSlot == slot && samplerGeneration == samplerGen &&
        samplerPatched == patched && samplerCompare == compareMode && samplerInteger == integer && samplerAniso == forceAniso;
  }
};
std::array<std::array<ResolvedTexture,18>,2> resolvedTextures;
// Bounded overlap: the default on every platform but the Switch (measured on macOS and Windows; the
// Switch's submission ring and frame pacing were measured without it); an explicit zero/invalid
// WWHD_VK_DRAW_BATCH disables it.
uint32_t parse_draw_batch(const char* text) {
#ifdef __SWITCH__
  if (!text) return 0;
#else
  if (!text) return 2048;
#endif
  if (!*text) return 0;
  uint32_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return 0;
    uint32_t digit = uint32_t(*p - '0');
    if (value > (1048576u - digit) / 10) return 0;
    value = value * 10 + digit;
  }
  return value;
}
uint32_t parse_draw_batch_cap(const char* text) {
  if (!text) return 3;
  // Explicit invalid caps retain the original conservative fallback.
  const uint32_t value = parse_draw_batch(text);
  return value >= 1 && value <= 3 ? value : 2;
}
struct DrawBatchState {
  uint64_t frame = ~uint64_t{0};
  uint32_t draws = 0, submissions = 0;
  bool after_draw(uint64_t currentFrame, uint32_t batch, uint32_t cap) {
    if (frame != currentFrame) {
      frame = currentFrame;
      draws = submissions = 0;
    }
    if (!batch || submissions >= cap) return false;
    if (++draws < batch) return false;
    draws = 0;
    ++submissions;
    return true;
  }
};
DrawBatchState drawBatchState;
uint64_t drawBatchSubmissions = 0;
constexpr uint32_t kDepthDownsamplePS = 0x3BB9DE00, kOcclusionPS = 0x44BDFD00;
constexpr uint32_t kOcclusionVS = 0x44BDF900;
bool aoPrivateReplay = false;
uint32_t aoPrivateSource = 0;
uint64_t aoPrivateFrame = ~0ull;
Surface aoPrivateColor, aoPrivateDepth;
Surface* private_ao_surface(Surface& dst, const Surface* like) {
  const uint64_t logicalWidth = uint64_t(like->width) * 3 / 2;
  const uint64_t logicalHeight = uint64_t(like->height) * 3 / 2;
  const uint64_t physicalWidth = uint64_t(like->extent.width) * 3 / 2;
  const uint64_t physicalHeight = uint64_t(like->extent.height) * 3 / 2;
  const auto limit = R.properties.limits.maxImageDimension2D;
  if (!logicalWidth || !logicalHeight || logicalWidth > UINT32_MAX ||
      logicalHeight > UINT32_MAX || !physicalWidth || !physicalHeight ||
      physicalWidth > limit || physicalHeight > limit)
    throw std::runtime_error("private AO image dimensions exceed device limits");
  const uint32_t width = uint32_t(logicalWidth), height = uint32_t(logicalHeight);
  const VkExtent3D extent{uint32_t(physicalWidth), uint32_t(physicalHeight), 1};
  if (!dst.image || dst.width != width || dst.height != height ||
      dst.extent.width != extent.width || dst.extent.height != extent.height ||
      dst.fmt.pixel != like->fmt.pixel) {
    end_encoder();
    destroy_surface_image(&dst);
    dst = *like;
    dst.addressState.reset();
    dst.image = VK_NULL_HANDLE; dst.memory = VK_NULL_HANDLE;
    dst.view = VK_NULL_HANDLE;
    dst.layerViews.clear(); dst.sampledViews.clear(); dst.guestLayout.reset();
    dst.addr = dst.mipAddr = 0;
    dst.width = width; dst.height = height; dst.slices = dst.mips = 1;
    dst.dim = uint32_t(Latte::E_DIM::DIM_2D);
    dst.gpuWritten = true; // Private render image: never upload guest address 0.
    dst.dirty = false;
    create_surface_image(&dst, false, extent);
  }
  return &dst;
}

float f32(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// Guest data in MEM2, bound in place when MEM2 is imported for the GPU (Switch); an empty slice
// means: copy it as before.
UploadSlice guest_buffer_slice(uint32_t address, uint64_t size, uint32_t alignment) {
  constexpr uint32_t kMem2 = 0x10000000, kMem2Size = 0x40000000;
  if (!R.guestMem2Buffer || address < kMem2 || uint64_t(address) - kMem2 + size > kMem2Size ||
      (address & (alignment - 1)))
    return UploadSlice{};
  UploadSlice s{};
  s.buffer = R.guestMem2Buffer;
  s.offset = address - kMem2;
  s.size = size;
  return s;
}
UploadSlice snapshot(const void *data, size_t size, VkDeviceSize alignment) {
  auto slice = allocate_upload(std::max<size_t>(size, 16), alignment);
  // Every allocation owns its bytes until the submission fence completes.
  // Zero padding also defines shader reads for empty/small uniform payloads.
  if (data && size) {
    std::memcpy(slice.mapped, data, size);
    if (slice.size > size)
      std::memset(static_cast<uint8_t *>(slice.mapped) + size, 0,
                  slice.size - size);
  } else {
    std::memset(slice.mapped, 0, slice.size);
  }
  return slice;
}

UploadSlice vertex_snapshot(uint32_t binding, uint32_t address, uint32_t size,
                            bool bounded) {
  static const bool enabled = [] {
    const char *value = std::getenv("WWHD_VK_REUSE_VERTEX_SNAPSHOTS");
    return value && std::strcmp(value, "1") == 0;
  }();
  static const bool probeEnabled = [] {
    const char* e = std::getenv("WWHD_VK_VERTEX_HISTORY_PROBE");
    return e && !std::strcmp(e, "1") && preparation_stats_enabled();
  }();
  if (probeEnabled) {
    static VertexHistoryProbe probe;
    const uint32_t distance = probe.observe(reinterpret_cast<uintptr_t>(R.device),
        R.submissionGeneration, binding, address, size, bounded);
    if (bounded && binding < probe.bindings.size()) {
      ++R.vertexHistoryRequests;
      if (distance) {
        ++R.vertexHistoryMatches;
        R.vertexHistoryBytes += size;
        ++R.vertexHistoryDistances[distance - 1];
      }
    }
  }
  if (!enabled)
    return snapshot(mem::ptr(address), size, 4);
  static const bool historyEnabled = [] {
    const char* e = std::getenv("WWHD_VK_VERTEX_HISTORY_REUSE");
    return e && !std::strcmp(e, "1");
  }();
  static std::array<VertexSnapshotHistory<UploadSlice>, 16> cache{};
  static uint64_t generation = ~0ull;
  static VkDevice device = VK_NULL_HANDLE;
  if (device != R.device || generation != R.submissionGeneration) {
    cache = {};
    device = R.device;
    generation = R.submissionGeneration;
  }
  if (binding >= cache.size())
    return snapshot(mem::ptr(address), size, 4);
  auto &history = cache[binding];
  auto &last = history.last;
  if (!bounded) {
    history = {};
    return snapshot(mem::ptr(address), size, 4);
  }
  // One exact matching payload per draw; secondary is consulted only when
  // the original consecutive key differs, never after a changed-byte miss.
  auto* candidate = VertexSnapshotHistory<UploadSlice>::matches(last, address, size)
      ? &last : historyEnabled ? history.secondary(address, size) : nullptr;
  const bool secondary = candidate && candidate != &last;
  if (candidate && candidate->slice.cpuReadable) {
    ++R.vertexReuseChecks;
    if (secondary) ++R.vertexHistoryReuseChecks;
    static const bool timed = std::getenv("WWHD_VK_STATS") != nullptr;
    std::chrono::steady_clock::time_point start;
    if (timed) start = std::chrono::steady_clock::now();
    const bool equal = std::memcmp(mem::ptr(address), candidate->slice.mapped, size) == 0;
    if (timed)
      R.vertexReuseCompareNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count();
    if (equal) {
      ++R.vertexReuseHits;
      R.vertexReuseBytes += size;
      if (secondary) {
        ++R.vertexHistoryReuseHits;
        R.vertexHistoryReuseBytes += size;
        history.promote();
      }
      return history.last.slice;
    }
  }
  auto slice = snapshot(mem::ptr(address), size, 4);
  history.remember(address, size, slice, historyEnabled);
  return slice;
}

// Separate immutable window cache: a partially initialized reservation must
// never be accepted by the full-prefix snapshot cache.
UploadSlice vertex_window_snapshot(uint32_t binding,uint32_t address,uint32_t reservation,
                                   uint32_t begin,uint32_t length,
                                   const void* source=nullptr,bool poisonUnused=false) {
  struct Entry { uint32_t address=0,reservation=0,begin=0,length=0; UploadSlice slice{}; };
  static std::array<Entry,16> entries{};
  static VkDevice device=VK_NULL_HANDLE;
  static uint64_t generation=~0ull;
  if(device!=R.device || generation!=R.submissionGeneration) {
    entries={};device=R.device;generation=R.submissionGeneration;
  }
  static const bool reuse=[] {const char* e=std::getenv("WWHD_VK_REUSE_VERTEX_SNAPSHOTS");
    return e && !std::strcmp(e,"1");}();
  Entry* entry=binding<entries.size()?&entries[binding]:nullptr;
  const auto* fresh=static_cast<const uint8_t*>(source?source:mem::ptr(address))+begin;
  if(reuse && entry && entry->slice.mapped && entry->slice.cpuReadable && entry->address==address &&
     entry->reservation==reservation && entry->begin==begin && entry->length==length) {
    ++R.vertexReuseChecks;
    if(!std::memcmp(fresh,static_cast<const uint8_t*>(entry->slice.mapped)+begin,length)) {
      ++R.vertexReuseHits;R.vertexReuseBytes+=length;return entry->slice;
    }
  }
  auto slice=allocate_upload(std::max<uint32_t>(reservation,16),4);
  if(poisonUnused) std::memset(slice.mapped,0xCD,slice.size);
  std::memcpy(static_cast<uint8_t*>(slice.mapped)+begin,fresh,length);
  // Bytes before the proven minimum are never fetched. Preserve the previous
  // small-allocation padding contract without reading that unused prefix.
  if(slice.size>reservation)
    std::memset(static_cast<uint8_t*>(slice.mapped)+reservation,0,slice.size-reservation);
  if(entry) *entry={address,reservation,begin,length,slice};
  return slice;
}

// Exact widths of the raw UINT formats used by the fetch pipeline.
uint32_t vertex_format_bytes(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8_UINT: return 1;
  case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16_UINT: return 2;
  case VK_FORMAT_R8G8B8_UINT: return 3;
  case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16_UINT:
  case VK_FORMAT_R32_UINT: return 4;
  case VK_FORMAT_R16G16B16_UINT: return 6;
  case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32_UINT: return 8;
  case VK_FORMAT_R32G32B32_UINT: return 12;
  case VK_FORMAT_R32G32B32A32_UINT: return 16;
  default: return 0;
  }
}
struct VertexExtent {
  bool valid = false;
  uint32_t maximum = 0;
  uint32_t minimum = 0;
};
bool vertex_copy_window_enabled() {
  static const bool enabled=[] {const char* e=std::getenv("WWHD_VK_VERTEX_COPY_WINDOW");
    return e && !std::strcmp(e,"1");}();
  return enabled;
}
template<class Index, bool Restart, bool ZeroBase, bool Window=false>
VertexExtent index_extent_reduction(const void* data, size_t count, int32_t baseVertex) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  constexpr uint32_t marker = sizeof(Index) == 2 ? UINT16_MAX : UINT32_MAX;
  uint32_t minimum = UINT32_MAX, maximum = 0;
  bool any = Restart ? false : count != 0;
  for (size_t i = 0; i < count; ++i) {
    Index index;
    // Fixed-size memcpy supports unaligned guest snapshots and compiles to a
    // native load. Branches on width, restart and zero base stay outside here.
    std::memcpy(&index, bytes + i * sizeof(Index), sizeof(Index));
    const uint32_t value = index;
    if constexpr (Restart) {
      const bool included = value != marker;
      any |= included;
      maximum = std::max(maximum, included ? value : 0);
      if constexpr (!ZeroBase || Window)
        minimum = std::min(minimum, included ? value : UINT32_MAX);
    } else {
      maximum = std::max(maximum, value);
      if constexpr (!ZeroBase || Window) minimum = std::min(minimum, value);
    }
  }
  if (!any) return {}; // Empty/all-restart draws retain the full binding.
  if constexpr (ZeroBase) return {true, maximum, Window ? minimum : 0};
  const int64_t low = int64_t(minimum) + baseVertex;
  const int64_t high = int64_t(maximum) + baseVertex;
  if (low < 0 || high > UINT32_MAX) return {};
  return {true, uint32_t(high), Window ? uint32_t(low) : 0};
}
template<class Index, bool Window=false>
VertexExtent index_extent_typed(const void* data, size_t count, bool restart, int32_t baseVertex) {
  if (!baseVertex) {
    if constexpr (Window) {
      if(count) {
        Index first;
        std::memcpy(&first,data,sizeof(first));
        // A host index zero proves the unsigned global minimum. Zero is
        // never a restart marker; use the original max-only SIMD reduction.
        if(first==0)
          return restart ? index_extent_reduction<Index,true,true,false>(data,count,baseVertex)
                         : index_extent_reduction<Index,false,true,false>(data,count,baseVertex);
      }
    }
    return restart ? index_extent_reduction<Index, true, true, Window>(data, count, baseVertex)
                   : index_extent_reduction<Index, false, true, Window>(data, count, baseVertex);
  }
  return restart ? index_extent_reduction<Index, true, false, Window>(data, count, baseVertex)
                 : index_extent_reduction<Index, false, false, Window>(data, count, baseVertex);
}
VertexExtent indexed_vertex_extent(const void* data, size_t count,
                                   uint32_t width, bool restart, int32_t baseVertex) {
  if(vertex_copy_window_enabled())
    return width == 2 ? index_extent_typed<uint16_t,true>(data,count,restart,baseVertex)
                      : index_extent_typed<uint32_t,true>(data,count,restart,baseVertex);
  return width == 2 ? index_extent_typed<uint16_t>(data, count, restart, baseVertex)
                    : index_extent_typed<uint32_t>(data, count, restart, baseVertex);
}
uint32_t vertex_prefix_size(uint32_t declared, uint32_t stride,
                            uint64_t attributeEnd, VertexExtent extent) {
  if (!extent.valid || !attributeEnd)
    return declared;
  return uint32_t(std::min<uint64_t>(declared,
      uint64_t(extent.maximum) * stride + attributeEnd));
}

// Metadata-only opportunity measurement. It never changes the upload extent.
struct VertexWindowExtent { bool valid=false; uint32_t minimum=0, maximum=0; };
template<class Index>
VertexWindowExtent vertex_window_extent(const void* data, size_t count,
                                       bool restart, int32_t base) {
  uint32_t lo=UINT32_MAX,hi=0; bool any=false;
  const auto* bytes=static_cast<const uint8_t*>(data);
  for(size_t i=0;i<count;++i) {
    Index value; std::memcpy(&value,bytes+i*sizeof(Index),sizeof(value));
    if(restart && value==std::numeric_limits<Index>::max()) continue;
    any=true;lo=std::min(lo,uint32_t(value));hi=std::max(hi,uint32_t(value));
  }
  const int64_t low=int64_t(lo)+base,high=int64_t(hi)+base;
  if(!any || low<0 || high>UINT32_MAX) return {};
  return {true,uint32_t(low),uint32_t(high)};
}
bool vertex_window_stats_enabled() {
  static const bool enabled=[] {const char* e=std::getenv("WWHD_VK_VERTEX_WINDOW_STATS");
    return e && !std::strcmp(e,"1");}();
  return enabled;
}
struct VertexWindowStats {
  uint64_t prefix=0,window=0,bindings=0,eligible=0,fallback=0,zeroStride=0;
} vertexWindowStats;
uint32_t vertex_window_unused(uint32_t copied,uint32_t stride,uint64_t attributeEnd,
                             VertexWindowExtent extent,bool supported) {
  if(!supported || !extent.valid || !attributeEnd || !stride) return 0;
  const uint64_t begin=uint64_t(extent.minimum)*stride;
  const uint64_t end=uint64_t(extent.maximum)*stride+attributeEnd;
  // Out-of-declaration fetches cannot justify changing a snapshot contract.
  if(end>copied || begin>end) return 0;
  return uint32_t(begin);
}
void report_vertex_window_stats() {
  static uint64_t previousFrame=0;
  if(R.frame<previousFrame) {previousFrame=R.frame;vertexWindowStats={};}
  if(R.frame-previousFrame<120) return;
  const double frames=double(R.frame-previousFrame);
  const auto& t=vertexWindowStats;
  LOG("[vulkan vertex window opportunity] %.3f MiB prefix/frame %.3f MiB reachable window/frame %.3f MiB unused prefix/frame; bindings/eligible/fallback/zero-stride %llu/%llu/%llu/%llu; metadata only, uploads unchanged",
    t.prefix/frames/1048576.,t.window/frames/1048576.,(t.prefix-t.window)/frames/1048576.,
    (unsigned long long)t.bindings,(unsigned long long)t.eligible,
    (unsigned long long)t.fallback,(unsigned long long)t.zeroStride);
  vertexWindowStats={};previousFrame=R.frame;
}

VkBlendFactor blend(uint32_t v) {
  static const VkBlendFactor t[] = {VK_BLEND_FACTOR_ZERO,
                                    VK_BLEND_FACTOR_ONE,
                                    VK_BLEND_FACTOR_SRC_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                                    VK_BLEND_FACTOR_SRC_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                    VK_BLEND_FACTOR_DST_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                    VK_BLEND_FACTOR_DST_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
                                    VK_BLEND_FACTOR_SRC_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                    VK_BLEND_FACTOR_CONSTANT_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
                                    VK_BLEND_FACTOR_SRC1_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
                                    VK_BLEND_FACTOR_SRC1_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
                                    VK_BLEND_FACTOR_CONSTANT_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
  if (v >= std::size(t))
    throw std::runtime_error("unsupported blend factor");
  return t[v];
}
VkBlendOp blendop(uint32_t v) {
  static const VkBlendOp t[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT,
                                VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                VK_BLEND_OP_REVERSE_SUBTRACT};
  if (v >= 5)
    throw std::runtime_error("unsupported blend operation");
  return t[v];
}
VkStencilOp stencil(uint32_t v) {
  static const VkStencilOp t[] = {VK_STENCIL_OP_KEEP,
                                  VK_STENCIL_OP_ZERO,
                                  VK_STENCIL_OP_REPLACE,
                                  VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                  VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                  VK_STENCIL_OP_INVERT,
                                  VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                  VK_STENCIL_OP_DECREMENT_AND_WRAP};
  if (v >= 8)
    throw std::runtime_error("unsupported stencil operation");
  return t[v];
}
struct BindingTrimMetadata {
  uint64_t attributeEnd = 0;
  uint32_t stride = 0;
  bool supported = true;
  std::optional<LatteConst::VertexFetchType2> rate;
};
template<class Group>
BindingTrimMetadata binding_trim_metadata(const uint32_t* r, vk::Shader* vs, const Group& g) {
  BindingTrimMetadata trim;
  for (int j = 0; j < g.attribCount; ++j) {
    const auto &a = g.attrib[j];
    if (vs->mapping.attributeMapping[a.semanticId] < 0)
      continue;
    const uint32_t bytes = vertex_format_bytes(vk::vertex_format(a.format));
    if (!bytes || (trim.rate && *trim.rate != a.fetchType) ||
      (a.fetchType != LatteConst::VertexFetchType2::VERTEX_DATA &&
       a.fetchType != LatteConst::VertexFetchType2::INSTANCE_DATA) ||
      (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA &&
       a.aluDivisor != 1)) {
      trim.supported = false;
      break;
    }
    trim.rate = a.fetchType;
    trim.attributeEnd = std::max(trim.attributeEnd, uint64_t(a.offset) + bytes);
  }
  trim.stride =
    (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >>
     11) & 0xFFFF;
  return trim;
}
struct Pipeline {
  VkPipeline pipeline{};
  VkPipelineLayout layout{};
  VkDescriptorSetLayout sets[2]{};
  bool dynamicUniforms = false;
  const LatteFetchShader* trimFetch = nullptr;
  const vk::Shader* trimVertex = nullptr;
  std::vector<BindingTrimMetadata> bindingTrims;
};
std::unordered_map<std::string, Pipeline, CacheKeyHash, std::equal_to<>> pipelines;
struct PipelineStateMemo {
  const uint32_t* regs = nullptr;
  const LatteFetchShader* fetchShader = nullptr;
  uint64_t generation = 0, vs = 0, ps = 0, fetch = 0;
  VkPrimitiveTopology topology{};
  std::array<VkFormat, 8> colors{};
  VkFormat depth = VK_FORMAT_UNDEFINED;
  uint32_t depthControl = 0;  // pre-pass twins change it without a register write
  bool operator==(const PipelineStateMemo&) const = default;
};
PipelineStateMemo lastPipelineState;
struct ResolvedPipeline {
  const uint32_t* regs=nullptr;
  vk::Shader *vs=nullptr,*ps=nullptr;
  LatteFetchShader* fetch=nullptr;
  uint64_t generation=0,epoch=0;
  VkPrimitiveTopology topology{};
  std::array<Surface*,8> colors{};
  Surface* depth=nullptr;
  Pipeline* value=nullptr;
  uint32_t depthControl=0;  // pre-pass twins change it without a register write
};
// [1]: draws whose depth write pre-pass twins dropped, so alternating draws keep both memos
ResolvedPipeline resolvedPipelines[2];
struct LastPipelineLookup {
  std::array<char, 256> key{};
  size_t size = 0;
  VkDevice device = VK_NULL_HANDLE;
  Pipeline* value = nullptr;
};
LastPipelineLookup lastPipelineLookup;
struct PipelineLookaside {
  std::array<LastPipelineLookup, 8> entries{};
  uint32_t next = 0;
  Pipeline* find(const char* key, size_t size, VkDevice device) const {
    if (size > entries[0].key.size()) return nullptr;
    for (const auto& entry : entries)
      if (entry.value && entry.device == device && entry.size == size &&
          !std::memcmp(entry.key.data(), key, size))
        return entry.value;
    return nullptr;
  }
  void remember(const LastPipelineLookup& entry) {
    if (!entry.value || entry.size > entry.key.size()) return;
    entries[next] = entry;
    next = (next + 1) % entries.size();
  }
};
PipelineLookaside pipelineLookaside;
bool pipeline_lookaside_enabled() {
  static const bool enabled = [] {
    const char* e = std::getenv("WWHD_VK_PIPELINE_LOOKASIDE");
    return e && !std::strcmp(e, "1");
  }();
  return enabled;
}
// Pipeline warm-up. Every pipeline created during play is recorded as its exact lookup key plus the
// target details the key leaves out (color data kinds, depth stencil aspect); the next boot recreates
// them all (warm_up_pipelines) so first sight of a view no longer builds hundreds of pipelines.
// Render thread only (boot replay runs before the render and game threads start).
struct PipelineRecipe {
  std::string key;
  uint64_t fsKey = 0;  // exact fetch shader (the key's layout hash is 0 for every compact one)
  std::array<uint8_t, 8> kinds{};
  uint8_t stencil = 0;
};
std::vector<PipelineRecipe> pipelineRecipes;
// pipelines.bin payload of pipelineRecipes[0, recipeSerialized): extended at each save, so a save copies
// one buffer instead of every recipe (render thread)
std::vector<uint8_t> recipePayload;
size_t recipeSerialized = 0;
std::unordered_set<std::string> pipelineRecipeKeys;
bool pipelineRecipesDirty = false;
uint64_t pipelineRecipesChangedFrame = 0, pipelineRecipesAttemptFrame = 0;
constexpr size_t maxPipelineRecipes = 32768;
void record_pipeline_recipe(std::string_view key, const LatteFetchShader *fs,
                            const std::array<Surface *, 8> &colors, Surface *depth) {
  uint64_t fsKey;
  if (pipelineRecipes.size() >= maxPipelineRecipes || key.size() > 0xFFFF ||
      !vk::fetch_shader_key(fs, fsKey)) return;
  if (!pipelineRecipeKeys.emplace(key).second) return;
  PipelineRecipe recipe;
  recipe.key = std::string(key);
  recipe.fsKey = fsKey;
  for (int i = 0; i < 8; i++)
    recipe.kinds[i] = colors[i] ? uint8_t(colors[i]->fmt.kind) : 0;
  recipe.stencil = depth && depth->fmt.stencil;
  pipelineRecipes.push_back(std::move(recipe));
  pipelineRecipesDirty = true;
  pipelineRecipesChangedFrame = R.frame;
}
Pipeline &pipeline(const uint32_t *r, vk::Shader *vs, vk::Shader *ps,
                   LatteFetchShader *fs, VkPrimitiveTopology topology,
                   const std::array<Surface *, 8> &colors, Surface *depth) {
  if(preparation_stats_enabled())++R.cpuPreparation.pipelineLookups;
  if (lastPipelineLookup.device != R.device || pipelines.empty()) {
    lastPipelineLookup = {};
    pipelineLookaside = {};
    lastPipelineState = {};
  }
  std::array<VkFormat, 8> formats{};
  uint32_t ncolor = 0;
  for (int i = 0; i < 8; i++) {
    formats[i] = colors[i] ? colors[i]->fmt.pixel : VK_FORMAT_UNDEFINED;
    if (colors[i]) ncolor = i + 1;
  }
  VkFormat df = depth ? depth->fmt.pixel : VK_FORMAT_UNDEFINED;
  const PipelineStateMemo state{r, fs, g_pipeline_state_gen, vs->key, ps->key,
      fs->vkPipelineHashFragment, topology, formats, df, r[REGADDR::DB_DEPTH_CONTROL]};
  // Only GX2's register file has tracked writes. Standalone callers always
  // serialize their state, even if their register pointer remains unchanged.
  const bool tracked = r == gx2::regs();
  if (tracked && lastPipelineLookup.value && state == lastPipelineState) {
    if(preparation_stats_enabled())++R.cpuPreparation.pipelineLastHits;
    return *lastPipelineLookup.value;
  }
  static thread_local CacheKeyBytes<256> bytes;
  bytes.reset(); // Retain overflow capacity for unusually large fetch programs.
  bytes.append( &vs->key, 8);
  bytes.append( &ps->key, 8);
  bytes.append( &fs->vkPipelineHashFragment, 8);
  bytes.append( &topology, sizeof topology);
  const uint32_t fields[] = {REGADDR::CB_COLOR_CONTROL,
                             REGADDR::CB_TARGET_MASK,
                             REGADDR::DB_DEPTH_CONTROL,
                             REGADDR::DB_STENCILREFMASK,
                             REGADDR::DB_STENCILREFMASK_BF,
                             REGADDR::PA_SU_SC_MODE_CNTL,
                             REGADDR::PA_CL_CLIP_CNTL,
                             REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE,
                             REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET,
                             REGADDR::PA_SU_POLY_OFFSET_CLAMP};
  for (auto a : fields) {
    uint32_t value=r[a];
    if (a==REGADDR::DB_STENCILREFMASK || a==REGADDR::DB_STENCILREFMASK_BF) value &= 0x00FFFF00;
    else if (a==REGADDR::PA_SU_SC_MODE_CNTL) value &= 0x807;
    else if (a==REGADDR::PA_CL_CLIP_CNTL) value &= 1u<<27;
    bytes.append(&value,4);
  }
  bytes.append( r + REGADDR::CB_BLEND0_CONTROL, 32);
  bytes.append( formats.data(), sizeof formats);
  bytes.append( &df, sizeof df);
  for (auto &g : fs->bufferGroups) {
    uint32_t stride=(r[mmSQ_VTX_ATTRIBUTE_BLOCK_START+g.attributeBufferIndex*7+2]>>11)&0xFFFF;
    bytes.append(&stride,4);
  }
  if (bytes.bounded() && lastPipelineLookup.value && lastPipelineLookup.size == bytes.size &&
      !std::memcmp(lastPipelineLookup.key.data(), bytes.data(), bytes.size)) {
    if(preparation_stats_enabled())++R.cpuPreparation.pipelineLastHits;
    lastPipelineState = tracked ? state : PipelineStateMemo{};
    return *lastPipelineLookup.value;
  }
  const bool useLookaside = pipeline_lookaside_enabled() && bytes.bounded();
  if (useLookaside)
    if (auto* value = pipelineLookaside.find(bytes.data(), bytes.size, R.device)) {
      if(preparation_stats_enabled())++R.cpuPreparation.pipelineLookasideHits;
      std::memcpy(lastPipelineLookup.key.data(), bytes.data(), bytes.size);
      lastPipelineLookup.size = bytes.size;
      lastPipelineLookup.device = R.device;
      lastPipelineLookup.value = value;
      lastPipelineState = tracked ? state : PipelineStateMemo{};
      return *value;
    }
  auto remember = [&](Pipeline& value) -> Pipeline& {
    lastPipelineState = tracked ? state : PipelineStateMemo{};
    lastPipelineLookup = {};
    if (bytes.bounded()) {
      std::memcpy(lastPipelineLookup.key.data(), bytes.data(), bytes.size);
      lastPipelineLookup.size = bytes.size;
      lastPipelineLookup.device = R.device;
      lastPipelineLookup.value = &value;
      if (useLookaside) pipelineLookaside.remember(lastPipelineLookup);
    }
    return value;
  };
  if(preparation_stats_enabled())++R.cpuPreparation.pipelineMapLookups;
  if (auto it = pipelines.find(bytes.view()); it != pipelines.end())
    return remember(it->second);
  Pipeline p;
  vk::Shader *shaders[] = {vs, ps};
  uint32_t uniformBindings = 0;
  for (auto *shader : shaders) {
    uniformBindings += shader->mapping.uniformVarsBufferBindingPoint >= 0;
    for (int binding : shader->mapping.uniformBuffersBindingPoint)
      uniformBindings += binding >= 0;
  }
  // Dynamic UBO limits apply across both sets in the pipeline layout. Keep
  // both stages on the existing regular path if the complete layout exceeds it.
  p.dynamicUniforms = uniformBindings <=
      R.properties.limits.maxDescriptorSetUniformBuffersDynamic;
  const auto uniformType = p.dynamicUniforms
      ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
      : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  for (int st = 0; st < 2; st++) {
    auto &m = shaders[st]->mapping;
    std::vector<VkDescriptorSetLayoutBinding> b;
    auto add = [&](int i, VkDescriptorType t) {
      if (i >= 0)
        b.push_back(
            {uint32_t(i), t, 1,
             VkShaderStageFlags(st ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT),
             nullptr});
    };
    add(m.uniformVarsBufferBindingPoint, uniformType);
    for (auto i : m.uniformBuffersBindingPoint)
      add(i, uniformType);
    for (int i = 0; i < m.textureUnitCount; i++)
      add(m.textureUnitBaseBindingPoint + i,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    if (m.tfStorageBindingPoint >= 0)
      throw std::runtime_error("Vulkan transform feedback unsupported");
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = b.size();
    ci.pBindings = b.data();
    vk_check(vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &p.sets[st]),
             "descriptor layout");
  }
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.setLayoutCount = 2;
  lc.pSetLayouts = p.sets;
  vk_check(vkCreatePipelineLayout(R.device, &lc, nullptr, &p.layout),
           "pipeline layout");
  VkShaderModule modules[2]{};
  VkPipelineShaderStageCreateInfo stages[2]{};
  for (int i = 0; i < 2; i++) {
    VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mc.codeSize = shaders[i]->spirv->size() * 4;
    mc.pCode = shaders[i]->spirv->data();
    vk_check(vkCreateShaderModule(R.device, &mc, nullptr, &modules[i]),
             "shader module");
    stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[i].stage =
        i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    stages[i].module = modules[i];
    stages[i].pName = "main";
  }
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  p.trimFetch = fs;
  p.trimVertex = vs;
  p.bindingTrims.reserve(fs->bufferGroups.size());
  for (auto &g : fs->bufferGroups) {
    bool instance = false;
    BindingTrimMetadata trim;
    std::optional<LatteConst::VertexFetchType2> fetchRate;
    for (int j = 0; j < g.attribCount; j++) {
      auto &a = g.attrib[j];
      int loc = vs->mapping.attributeMapping[a.semanticId];
      if (loc < 0)
        continue;
      auto fmt = vk::vertex_format(a.format);
      if (fmt == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("unsupported vertex format");
      attributes.push_back(
          {uint32_t(loc), g.attributeBufferIndex, fmt, a.offset});
      // Mirror the conservative prefix rules using the actual pipeline format.
      if (trim.supported) {
        const uint32_t width = vertex_format_bytes(fmt);
        if (!width || (trim.rate && *trim.rate != a.fetchType) ||
            (a.fetchType != LatteConst::VertexFetchType2::VERTEX_DATA &&
             a.fetchType != LatteConst::VertexFetchType2::INSTANCE_DATA) ||
            (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA && a.aluDivisor != 1))
          trim.supported = false;
        else {
          trim.rate = a.fetchType;
          trim.attributeEnd = std::max(trim.attributeEnd, uint64_t(a.offset) + width);
        }
      }
      if (fetchRate && *fetchRate != a.fetchType)
        throw std::runtime_error("mixed vertex/instance rate in one buffer");
      fetchRate = a.fetchType;
      if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) {
        if (a.aluDivisor != 1)
          throw std::runtime_error("instance divisor unsupported");
        instance = true;
      }
    }
    uint32_t stride =
        (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >>
         11) &
        0xFFFF;
    bindings.push_back({g.attributeBufferIndex, stride,
                        instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                 : VK_VERTEX_INPUT_RATE_VERTEX});
    trim.stride = stride;
    p.bindingTrims.push_back(trim);
  }
  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = bindings.size();
  vi.pVertexBindingDescriptions = bindings.data();
  vi.vertexAttributeDescriptionCount = attributes.size();
  vi.pVertexAttributeDescriptions = attributes.data();
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topology;
  // Strip restart is native on Metal and required by MoltenVK portability.
  ia.primitiveRestartEnable = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                              topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  LATTE_PA_SU_SC_MODE_CNTL pm;
  std::memcpy(&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  LATTE_PA_CL_CLIP_CNTL pipelineClip;
  std::memcpy(&pipelineClip, r + REGADDR::PA_CL_CLIP_CNTL, 4);
  if (pipelineClip.get_ZCLIP_FAR_DISABLE()) {
    if (!R.enabledFeatures.depthClamp)
      throw std::runtime_error("device lacks depth clamp requested by guest");
    rs.depthClampEnable = VK_TRUE;
  }
  rs.lineWidth = 1;
  rs.cullMode = (pm.get_CULL_FRONT() ? VK_CULL_MODE_FRONT_BIT : 0) |
                (pm.get_CULL_BACK() ? VK_CULL_MODE_BACK_BIT : 0);
  rs.frontFace =
      pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW
          ? VK_FRONT_FACE_COUNTER_CLOCKWISE
          : VK_FRONT_FACE_CLOCKWISE;
  rs.depthBiasEnable = pm.get_OFFSET_FRONT_ENABLED();
  rs.depthBiasConstantFactor = f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]);
  rs.depthBiasSlopeFactor = f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16;
  if (rs.depthBiasEnable) {
    rs.depthBiasClamp = f32(r[REGADDR::PA_SU_POLY_OFFSET_CLAMP]);
    if (rs.depthBiasClamp != 0 && !R.enabledFeatures.depthBiasClamp)
      throw std::runtime_error("device lacks depth bias clamp requested by guest");
  }
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  LATTE_DB_DEPTH_CONTROL dc;
  std::memcpy(&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = depth && dc.get_Z_ENABLE();
  ds.depthWriteEnable = depth && dc.get_Z_WRITE_ENABLE();
  ds.depthCompareOp = VkCompareOp(dc.get_Z_FUNC());
  ds.stencilTestEnable = depth && depth->fmt.stencil && dc.get_STENCIL_ENABLE();
  ds.front = {stencil(static_cast<uint32_t>(dc.get_STENCIL_FAIL_F())),
              stencil(static_cast<uint32_t>(dc.get_STENCIL_ZPASS_F())),
              stencil(static_cast<uint32_t>(dc.get_STENCIL_ZFAIL_F())),
              VkCompareOp(dc.get_STENCIL_FUNC_F()),
              (r[REGADDR::DB_STENCILREFMASK] >> 8) & 255,
              (r[REGADDR::DB_STENCILREFMASK] >> 16) & 255,
              r[REGADDR::DB_STENCILREFMASK] & 255};
  ds.back = ds.front;
  if (dc.get_BACK_STENCIL_ENABLE())
    ds.back = {stencil(static_cast<uint32_t>(dc.get_STENCIL_FAIL_B())),
               stencil(static_cast<uint32_t>(dc.get_STENCIL_ZPASS_B())),
               stencil(static_cast<uint32_t>(dc.get_STENCIL_ZFAIL_B())),
               VkCompareOp(dc.get_STENCIL_FUNC_B()),
               (r[REGADDR::DB_STENCILREFMASK_BF] >> 8) & 255,
               (r[REGADDR::DB_STENCILREFMASK_BF] >> 16) & 255,
               r[REGADDR::DB_STENCILREFMASK_BF] & 255};
  std::array<VkPipelineColorBlendAttachmentState, 8> cb{};
  for (uint32_t i = 0; i < ncolor; i++) {
    auto &b = cb[i];
    b.colorWriteMask = (r[REGADDR::CB_TARGET_MASK] >> (4 * i)) & 15;
    b.blendEnable = colors[i] && colors[i]->fmt.kind == FormatInfo::FLOAT &&
                    ((r[REGADDR::CB_COLOR_CONTROL] >> (8 + i)) & 1);
    LATTE_CB_BLENDN_CONTROL raw;
    std::memcpy(&raw, r + REGADDR::CB_BLEND0_CONTROL + i, 4);
    b.srcColorBlendFactor =
        blend(static_cast<uint32_t>(raw.get_COLOR_SRCBLEND()));
    b.dstColorBlendFactor =
        blend(static_cast<uint32_t>(raw.get_COLOR_DSTBLEND()));
    b.colorBlendOp = blendop(static_cast<uint32_t>(raw.get_COLOR_COMB_FCN()));
    b.srcAlphaBlendFactor =
        raw.get_SEPARATE_ALPHA_BLEND()
            ? blend(static_cast<uint32_t>(raw.get_ALPHA_SRCBLEND()))
            : b.srcColorBlendFactor;
    b.dstAlphaBlendFactor =
        raw.get_SEPARATE_ALPHA_BLEND()
            ? blend(static_cast<uint32_t>(raw.get_ALPHA_DSTBLEND()))
            : b.dstColorBlendFactor;
    b.alphaBlendOp =
        raw.get_SEPARATE_ALPHA_BLEND()
            ? blendop(static_cast<uint32_t>(raw.get_ALPHA_COMB_FCN()))
            : b.colorBlendOp;
  }
  VkPipelineColorBlendStateCreateInfo bs{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  bs.attachmentCount = ncolor;
  bs.pAttachments = cb.data();
  uint32_t rop = (r[REGADDR::CB_COLOR_CONTROL] >> 16) & 255;
  bs.logicOp = VK_LOGIC_OP_COPY;
  if (rop != 0xCC) {
    if (!R.enabledFeatures.logicOp)
      throw std::runtime_error("color logic operation unsupported by device");
    bs.logicOpEnable = VK_TRUE;
    switch (rop) {
    case 0x00:
      bs.logicOp = VK_LOGIC_OP_CLEAR;
      break;
    case 0x88:
      bs.logicOp = VK_LOGIC_OP_AND;
      break;
    case 0x44:
      bs.logicOp = VK_LOGIC_OP_AND_REVERSE;
      break;
    case 0x22:
      bs.logicOp = VK_LOGIC_OP_AND_INVERTED;
      break;
    case 0xAA:
      bs.logicOp = VK_LOGIC_OP_NO_OP;
      break;
    case 0x66:
      bs.logicOp = VK_LOGIC_OP_XOR;
      break;
    case 0xEE:
      bs.logicOp = VK_LOGIC_OP_OR;
      break;
    case 0x11:
      bs.logicOp = VK_LOGIC_OP_NOR;
      break;
    case 0x99:
      bs.logicOp = VK_LOGIC_OP_EQUIVALENT;
      break;
    case 0x55:
      bs.logicOp = VK_LOGIC_OP_INVERT;
      break;
    case 0xDD:
      bs.logicOp = VK_LOGIC_OP_OR_REVERSE;
      break;
    case 0x33:
      bs.logicOp = VK_LOGIC_OP_COPY_INVERTED;
      break;
    case 0xBB:
      bs.logicOp = VK_LOGIC_OP_OR_INVERTED;
      break;
    case 0x77:
      bs.logicOp = VK_LOGIC_OP_NAND;
      break;
    case 0xFF:
      bs.logicOp = VK_LOGIC_OP_SET;
      break;
    default:
      throw std::runtime_error("unsupported color logic operation");
    }
  }
  for (uint32_t i = 0; i < ncolor; ++i)
    if (cb[i].blendEnable) {
      auto &b = cb[i];
      if (!R.enabledFeatures.dualSrcBlend &&
          (b.srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
           b.dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
           b.srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
           b.dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR))
        throw std::runtime_error("dual-source blend unsupported by device");
      if (!R.constantAlphaColorBlendFactors &&
          (b.srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
           b.srcColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA ||
           b.dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
           b.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA))
        throw std::runtime_error(
            "constant alpha color blend unsupported by device");
    }
  if (ds.stencilTestEnable && !R.separateStencilMaskRef &&
      (ds.front.compareMask != ds.back.compareMask ||
       ds.front.writeMask != ds.back.writeMask ||
       ds.front.reference != ds.back.reference))
    throw std::runtime_error("separate stencil state unsupported by device");
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                          VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                          VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dy{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = std::size(dyn);
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo rc{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rc.colorAttachmentCount = ncolor;
  rc.pColorAttachmentFormats = formats.data();
  rc.depthAttachmentFormat = df;
  rc.stencilAttachmentFormat =
      depth && depth->fmt.stencil ? df : VK_FORMAT_UNDEFINED;
  VkGraphicsPipelineCreateInfo ci{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rc;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &bs;
  ci.pDynamicState = &dy;
  ci.layout = p.layout;
  const auto pipelineStarted = std::chrono::steady_clock::now();
  auto result = vkCreateGraphicsPipelines(R.device, R.pipelineCache, 1, &ci,
                                          nullptr, &p.pipeline);
  R.pipelineCreateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-pipelineStarted).count();
  ++R.pipelineCreates;
  if (result == VK_ERROR_UNKNOWN) {
    // WORKAROUND (PR #30, see TODO.md): patches the GLSL text after the driver
    // refused the pipeline. The proper fix is to link the pixel shader's inputs
    // to the vertex shader's outputs in the shader translation, so inputs
    // without an output read as zero up front on every driver.
    // Pixel shader inputs that this vertex shader does not write (the Latte
    // translation declares every input of the pixel shader): read as zero.
    std::string glsl = ps->glsl;
    size_t replaced = 0;
    for (size_t at = 0; (at = glsl.find("layout(location = ", at)) != std::string::npos;) {
      size_t end = glsl.find(';', at);
      size_t name = glsl.find("in vec4 passParameterSem", at);
      if (end == std::string::npos || name == std::string::npos || name > end) { at++; continue; }
      std::string var = glsl.substr(name + 8, end - name - 8);
      if (vs->glsl.find("out vec4 " + var + ";") == std::string::npos) {
        std::string zero = "const vec4 " + var + " = vec4(0.0)";
        glsl.replace(at, end - at, zero);
        at += zero.size();
        replaced++;
      } else {
        at = end;
      }
    }
    std::string error;
    auto spirv = replaced ? vk::compile_glsl(glsl, false, &error) : std::vector<uint32_t>{};
    VkShaderModule zeroed = VK_NULL_HANDLE;
    if (!spirv.empty()) {
      VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      mc.codeSize = spirv.size() * 4;
      mc.pCode = spirv.data();
      if (vkCreateShaderModule(R.device, &mc, nullptr, &zeroed) == VK_SUCCESS) {
        stages[1].module = zeroed;
        result = vkCreateGraphicsPipelines(R.device, R.pipelineCache, 1, &ci, nullptr, &p.pipeline);
        vkDestroyShaderModule(R.device, zeroed, nullptr);
      }
    }
    LOG("[vulkan] graphics pipeline vs %016llX ps %016llX: VK_ERROR_UNKNOWN; %zu pixel shader inputs without a vertex output read as zero: %s",
        (unsigned long long)vs->key, (unsigned long long)ps->key, replaced,
        result == VK_SUCCESS ? "built" : error.empty() ? "still fails" : error.c_str());
  }
  for (auto m : modules)
    vkDestroyShaderModule(R.device, m, nullptr);
  if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_HOST_MEMORY &&
      result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_DEVICE_LOST) {
    // A driver that cannot build one pipeline (Adreno: VK_ERROR_UNKNOWN on the
    // boat ride after the sword and shield) used to end the game. The pipeline
    // stays empty, its draws are skipped, and the two shaders go to captures/
    // (GLSL and SPIR-V) to look at.
    p.pipeline = VK_NULL_HANDLE;
    LOG("[vulkan] graphics pipeline failed (Vulkan result %d): vs %016llX ps %016llX; its draws are skipped",
        int(result), (unsigned long long)vs->key, (unsigned long long)ps->key);
    std::error_code captureDirError;  // this path must not throw: it replaces a crash
    std::filesystem::create_directories("captures", captureDirError);
    for (auto *shader : shaders) {
      char name[96];
      snprintf(name, sizeof name, "captures/pipeline-failed-%s-%016llX",
               shader->vertex ? "vs" : "ps", (unsigned long long)shader->key);
      if (FILE *f = fopen((std::string(name) + ".glsl").c_str(), "wb")) {
        fwrite(shader->glsl.data(), 1, shader->glsl.size(), f);
        fclose(f);
      }
      if (FILE *f = fopen((std::string(name) + ".spv").c_str(), "wb")) {
        if (shader->spirv) fwrite(shader->spirv->data(), 4, shader->spirv->size(), f);
        fclose(f);
      }
    }
    return remember(pipelines.emplace(std::string(bytes.view()), p).first->second);
  }
  vk_check(result, "graphics pipeline");
  R.pipelineCacheDirty = true;
  R.pipelineCacheChangedFrame = R.frame;
  record_pipeline_recipe(bytes.view(), fs, colors, depth);
  return remember(pipelines.emplace(std::string(bytes.view()), std::move(p)).first->second);
}
struct SamplerMemo {
  VkDevice device = VK_NULL_HANDLE;
  std::array<uint32_t, 3> words{};
  bool compare = false, integer = false, forceAniso = false;
  VkSampler value = VK_NULL_HANDLE;
  uint64_t generation = ~uint64_t{0};
  bool patched = false;
  bool matches(VkDevice nextDevice, const uint32_t* nextWords,
               bool nextCompare, bool nextInteger, bool nextForceAniso) const {
    return value && device == nextDevice && compare == nextCompare &&
        integer == nextInteger && forceAniso == nextForceAniso &&
        words[0] == nextWords[0] && words[1] == nextWords[1] && words[2] == nextWords[2];
  }
  void remember(VkDevice nextDevice, const uint32_t* nextWords,
                bool nextCompare, bool nextInteger, bool nextForceAniso, VkSampler nextValue) {
    device = nextDevice;
    std::memcpy(words.data(), nextWords, sizeof(words));
    compare = nextCompare; integer = nextInteger; forceAniso = nextForceAniso;
    value = nextValue;
  }
};
std::array<SamplerMemo, 36> samplerMemos;
struct SamplerKeyHash {
  size_t operator()(const std::array<uint32_t, 4>& key) const noexcept {
    size_t hash = 0;
    for (uint32_t word : key) hash = (hash * 16777619) ^ word;
    return hash;
  }
};
std::unordered_map<std::array<uint32_t, 4>, VkSampler, SamplerKeyHash> samplerCache;
VkSampler sampler(const uint32_t *words, bool compare, bool integer, bool forceAniso,
                  uint32_t slot, uint64_t generation, bool patched) {
  static const bool memo = draw_option_enabled("WWHD_VK_SAMPLER_MEMO");
  auto& samplerMemo = samplerMemos.at(slot);
  const bool stats = preparation_stats_enabled();
  if (stats) ++R.cpuPreparation.samplerRequests;
  const bool generationHit = generation != ~uint64_t{0} && samplerMemo.value &&
      samplerMemo.device == R.device && samplerMemo.generation == generation &&
      samplerMemo.patched == patched && samplerMemo.compare == compare &&
      samplerMemo.integer == integer && samplerMemo.forceAniso == forceAniso;
  if (memo && (generationHit || samplerMemo.matches(R.device, words, compare, integer, forceAniso))) {
    samplerMemo.generation = generation; samplerMemo.patched = patched;
    if (stats) ++R.cpuPreparation.samplerMemoHits;
    return samplerMemo.value;
  }
  if (stats) ++R.cpuPreparation.samplerMapLookups;
  auto remember = [&](VkSampler value) {
    if (memo) {
      samplerMemo.remember(R.device, words, compare, integer, forceAniso, value);
      samplerMemo.generation = generation; samplerMemo.patched = patched;
    }
    return value;
  };
  const std::array<uint32_t, 4> key{words[0], words[1], words[2],
      uint32_t(compare) | (uint32_t(integer) << 1) | (uint32_t(forceAniso) << 2)};
  if (auto it = samplerCache.find(key); it != samplerCache.end())
    return remember(it->second);
  LATTE_SQ_TEX_SAMPLER_WORD0_0 w;
  LATTE_SQ_TEX_SAMPLER_WORD1_0 w1;
  std::memcpy(&w, words, 4);
  std::memcpy(&w1, words + 1, 4);
  auto filter = [](uint32_t v) {
    return v == 0 || v == 4 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  };
  auto addr = [](uint32_t v) {
    switch (v) {
    case 0:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:
      if (!R.samplerMirrorClampToEdge)
        throw std::runtime_error("mirror-once sampler unsupported by device");
      return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5:
    case 7:
      throw std::runtime_error(
          "mirror-once border sampling is not implemented");
    case 2:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
  };
  VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  ci.magFilter = filter(static_cast<uint32_t>(w.get_XY_MAG_FILTER()));
  ci.minFilter = filter(static_cast<uint32_t>(w.get_XY_MIN_FILTER()));
  if (integer)
    ci.minFilter = ci.magFilter = VK_FILTER_NEAREST;
  ci.mipmapMode = static_cast<uint32_t>(w.get_MIP_FILTER()) == 2
                      ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                      : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU = addr(static_cast<uint32_t>(w.get_CLAMP_X()));
  ci.addressModeV = addr(static_cast<uint32_t>(w.get_CLAMP_Y()));
  ci.addressModeW = addr(static_cast<uint32_t>(w.get_CLAMP_Z()));
  ci.mipLodBias = std::clamp(float(w1.get_LOD_BIAS()) / 64.f,
                             -R.properties.limits.maxSamplerLodBias,
                             R.properties.limits.maxSamplerLodBias);
  if (!R.samplerMipLodBias && ci.mipLodBias != 0) {
    // MoltenVK cannot apply sampler LOD bias. Match the existing Metal renderer
    // there; native Vulkan devices retain the requested bias.
    static bool reported = false;
    if (!reported) {
      LOG("[vulkan] device lacks sampler LOD bias; using zero bias (Metal-compatible fallback)");
      reported = true;
    }
    ci.mipLodBias = 0;
  }
  ci.minLod = w1.get_MIN_LOD() / 64.f;
  ci.maxLod =
      static_cast<uint32_t>(w.get_MIP_FILTER()) ? w1.get_MAX_LOD() / 64.f : 0;
  ci.compareEnable = compare;
  ci.compareOp = VkCompareOp(w.get_DEPTH_COMPARE_FUNCTION());
  uint32_t anisotropy = w.get_MAX_ANISO_RATIO()
      ? std::min(1u << std::min(uint32_t(w.get_MAX_ANISO_RATIO()), 4u), 16u) : 1;
  if (forceAniso && uint32_t(w.get_MIP_FILTER()) != 0 && ci.minFilter == VK_FILTER_LINEAR &&
      uint32_t(w.get_DEPTH_COMPARE_FUNCTION()) == 0)
    anisotropy = 16;
  if (R.enabledFeatures.samplerAnisotropy && !integer && anisotropy > 1) {
    ci.anisotropyEnable = VK_TRUE;
    ci.maxAnisotropy = std::min(float(anisotropy), R.properties.limits.maxSamplerAnisotropy);
  }

  ci.borderColor = static_cast<uint32_t>(w.get_BORDER_COLOR_TYPE()) == 1
                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                   : static_cast<uint32_t>(w.get_BORDER_COLOR_TYPE()) == 2
                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                       : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if (integer) {
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.borderColor = VkBorderColor(uint32_t(ci.borderColor) + 1);
  }
  VkSampler s;
  vk_check(vkCreateSampler(R.device, &ci, nullptr, &s), "sampler");
  samplerCache.emplace(key, s);
  return remember(s);
}
struct StageResources {
  VkDescriptorSet set{};
  std::array<uint32_t, 17> dynamicOffsets{};
  uint32_t dynamicOffsetCount = 0;
};
// Exact consecutive descriptor state, optionally used to omit redundant binds.
// Lifetime is the tracked draw pass; explicit generation/command/layout keys
// also exclude repeated handles after command/descriptor pool resets.
struct DescriptorBindProbe {
  struct Matches { bool whole=false;std::array<bool,2> stage{}; };
  struct BindRange { uint32_t first=0,count=2,offsetBegin=0,offsetCount=0; };
  static BindRange bind_range(Matches matches,bool skip,uint32_t vsCount,uint32_t psCount) {
    if(skip && matches.whole) return {0,0,0,0};
    if(skip && matches.stage[0]) return {1,1,vsCount,psCount};
    if(skip && matches.stage[1]) return {0,1,0,vsCount};
    return {0,2,0,vsCount+psCount};
  }
  VkCommandBuffer command=VK_NULL_HANDLE;
  uint64_t generation=0;
  VkPipelineLayout layout=VK_NULL_HANDLE;
  std::array<VkDescriptorSet,2> sets{};
  std::array<uint32_t,2> counts{};
  std::array<uint32_t,34> offsets{};
  bool valid=false;
  Matches observe(VkCommandBuffer nextCommand,uint64_t nextGeneration,
                  VkPipelineLayout nextLayout,const VkDescriptorSet* nextSets,
                  const uint32_t* nextOffsets,uint32_t vsCount,uint32_t psCount) {
    Matches matches;
    if(vsCount>17 || psCount>17) { *this={};return matches; }
    const std::array<uint32_t,2> nextCounts{vsCount,psCount};
    if(valid && command==nextCommand && generation==nextGeneration && layout==nextLayout) {
      for(size_t stage=0;stage<2;++stage) {
        const size_t oldBegin=stage?counts[0]:0,nextBegin=stage?vsCount:0;
        matches.stage[stage]=sets[stage]==nextSets[stage] && counts[stage]==nextCounts[stage] &&
            (!nextCounts[stage] || !std::memcmp(offsets.data()+oldBegin,
                nextOffsets+nextBegin,nextCounts[stage]*sizeof(uint32_t)));
      }
      matches.whole=matches.stage[0]&&matches.stage[1];
    }
    command=nextCommand;generation=nextGeneration;layout=nextLayout;
    std::copy_n(nextSets,2,sets.begin());counts=nextCounts;
    if(vsCount+psCount)std::copy_n(nextOffsets,vsCount+psCount,offsets.begin());
    valid=true;
    return matches;
  }
};
struct DescriptorIdentity {
  uint32_t binding = 0;
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
  VkDescriptorBufferInfo buffer{};
  VkDescriptorImageInfo image{};
  bool isBuffer = false;
};
struct LastDescriptorSet {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  uint32_t count = 0;
  std::array<DescriptorIdentity, 17 + LATTE_NUM_MAX_TEX_UNITS> identities{};
};
bool persistentSetsReset = false;  // drop the persistent descriptor sets (samplers/state reset)
LastDescriptorSet lastDescriptors[2];
uint64_t descriptorCacheGeneration = ~uint64_t{0};
#ifdef __SWITCH__
CpuSnapshotCache<UploadSlice, VkDevice> supportSnapshotCache;
#endif
inline bool descriptor_identity_matches(const DescriptorIdentity& identity,
                                       const VkWriteDescriptorSet& write) {
  if (identity.binding != write.dstBinding || identity.type != write.descriptorType ||
      identity.isBuffer != bool(write.pBufferInfo)) return false;
  if (write.pBufferInfo) {
    const auto& info = *write.pBufferInfo;
    return identity.buffer.buffer == info.buffer && identity.buffer.range == info.range &&
        (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
         identity.buffer.offset == info.offset);
  }
  const auto& info = *write.pImageInfo;
  return identity.image.sampler == info.sampler && identity.image.imageView == info.imageView &&
      identity.image.imageLayout == info.imageLayout;
}
bool descriptor_matches(const LastDescriptorSet &last,
                        VkDescriptorSetLayout layout,
                        const VkWriteDescriptorSet *writes, uint32_t count) {
  if (!last.set || last.layout != layout || last.count != count) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (!descriptor_identity_matches(last.identities[i], writes[i])) return false;
  return true;
}
void remember_descriptors(LastDescriptorSet &last, VkDescriptorSetLayout layout,
                          VkDescriptorSet set, const VkWriteDescriptorSet *writes,
                          uint32_t count) {
  last.layout = layout;
  last.set = set;
  last.count = count;
  for (uint32_t i = 0; i < count; ++i) {
    auto &identity = last.identities[i];
    const auto &write = writes[i];
    identity.binding = write.dstBinding;
    identity.type = write.descriptorType;
    identity.isBuffer = write.pBufferInfo != nullptr;
    if (identity.isBuffer) identity.buffer = *write.pBufferInfo;
    else identity.image = *write.pImageInfo;
  }
}
struct DescriptorKey {
  std::array<uint64_t, 2 + (17 + LATTE_NUM_MAX_TEX_UNITS) * 4> words;
  size_t count = 0;
  template<class T> void append(T value) {
    if constexpr (std::is_pointer_v<T>) words[count++] = reinterpret_cast<uintptr_t>(value);
    else words[count++] = uint64_t(value);
  }
  std::span<const uint64_t> view() const { return {words.data(), count}; }
};
void descriptor_key(DescriptorKey& key, VkDescriptorSetLayout layout,
                    const VkWriteDescriptorSet *writes, uint32_t count, bool completeRanks) {
  key.append(layout);
  key.append(uint64_t(count) | (uint64_t(completeRanks) << 32));
  for (uint32_t i = 0; i < count; ++i) {
    const auto &write = writes[i];
    // A complete rank plan writes every binding in increasing order. Its
    // layout already specifies bindings/types; partial plans name them too.
    if (!completeRanks)
      key.append(uint64_t(write.dstBinding) | (uint64_t(write.descriptorType) << 32));
    if (write.pBufferInfo) {
      const auto &info = *write.pBufferInfo;
      key.append(info.buffer);
      key.append(info.range);
      if (write.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        key.append(info.offset);
    } else {
      const auto &info = *write.pImageInfo;
      key.append(info.sampler);
      key.append(info.imageView);
      key.append(info.imageLayout);
    }
  }
}
// A descriptor cannot sample the same subresource being written as an
// attachment without a feedback-loop extension. Snapshot before rendering.
// Separate stage/unit slots preserve every descriptor selected for one draw.
constexpr uint32_t kFeedbackUnitsPerStage = 16;
constexpr VkDeviceSize kFeedbackRetainedBudget = 128ull << 20;
struct FeedbackScratch {
  Surface surface;
  VkDevice device = VK_NULL_HANDLE;
  VkDeviceSize allocationBytes = 0;
};
// A unit alternates between e.g. full-size color and reduced AO targets.
// Retain a few shapes per unit instead of replacing its image on every change.
constexpr size_t kFeedbackShapesPerSlot = 4;
std::array<std::array<FeedbackScratch, kFeedbackShapesPerSlot>, kFeedbackUnitsPerStage * 2> feedbackScratch;
VkDeviceSize feedbackRetainedBytes = 0;
bool feedback_compatible(const Surface& copy, const Surface& source) {
  return copy.image && copy.fmt.pixel == source.fmt.pixel &&
         copy.aspect == source.aspect && copy.imageType == source.imageType &&
         copy.viewType == source.viewType && copy.dim == source.dim &&
         copy.extent.width == source.extent.width &&
         copy.extent.height == source.extent.height &&
         copy.extent.depth == source.extent.depth &&
         copy.mips == source.mips && copy.arrayLayers == source.arrayLayers;
}
void make_feedback_image(Surface& copy, const Surface& source) {
  copy = source;
  copy.addressState.reset();
  copy.image = VK_NULL_HANDLE;
  copy.memory = VK_NULL_HANDLE;
  copy.view = VK_NULL_HANDLE;
  copy.layerViews.clear();
  copy.sampledViews.clear();
  copy.guestLayout.reset();
  copy.addr = copy.mipAddr = 0;
  copy.gpuWritten = true;
  copy.dirty = false;
  // The snapshot is already at physical resolution. Shader texture-scale
  // uniforms continue to use source metadata in bind_stage, never this copy.
  copy.width = source.extent.width;
  copy.height = source.extent.height;
  if (source.imageType == VK_IMAGE_TYPE_3D) copy.slices = source.extent.depth;
  create_surface_image(&copy, false);
}
void reset_feedback_scratch() {
  for (auto& shapes : feedbackScratch) for (auto& slot : shapes) {
    // Reset must run before replacing the device. Foreign handles cannot be
    // placed into this device's deferred retirement lists.
    if (slot.surface.image && slot.device != R.device) continue;
    destroy_surface_image(&slot.surface);
    feedbackRetainedBytes -= slot.allocationBytes;
    slot = {};
  }
}
struct FeedbackStatsProbe {
  struct Key {
    VkDevice device; VkImage image; uint64_t version;
    VkFormat format; VkImageAspectFlags aspect;
    VkImageType imageType; VkImageViewType viewType;
    VkExtent3D extent; uint32_t mips,layers;
  };
  std::array<Key,2*LATTE_NUM_MAX_TEX_UNITS> keys;
  size_t count=0;
  static bool equal(const Key& a,const Key& b) {
    return a.device==b.device && a.image==b.image && a.version==b.version &&
        a.format==b.format && a.aspect==b.aspect && a.imageType==b.imageType &&
        a.viewType==b.viewType && a.extent.width==b.extent.width &&
        a.extent.height==b.extent.height && a.extent.depth==b.extent.depth &&
        a.mips==b.mips && a.layers==b.layers;
  }
  bool observe(const Surface& source) {
    const Key key{R.device,source.image,source.writeSeq,source.fmt.pixel,
        source.aspect,source.imageType,source.viewType,source.extent,
        source.mips,source.arrayLayers};
    for(size_t i=0;i<count;++i) if(equal(keys[i],key)) return true;
    if(count<keys.size()) keys[count++]=key;
    return false;
  }
};
struct FeedbackStats {
  uint64_t aliases=0,duplicates=0,texels=0,duplicateTexels=0;
  uint64_t color=0,depth=0,stencil=0,compressed=0,saturated=0;
};
FeedbackStats feedbackStats;
[[gnu::always_inline]] inline bool feedback_stats_enabled() {
  static const bool enabled=[] {
    const char* e=std::getenv("WWHD_VK_FEEDBACK_STATS");
    return e && !std::strcmp(e,"1");
  }();
  return enabled;
}
uint64_t feedback_texels(const Surface& source) {
  auto multiply=[](uint64_t a,uint64_t b) {
    return b && a>UINT64_MAX/b ? UINT64_MAX : a*b;
  };
  uint64_t total=0;
  for(uint32_t mip=0;mip<source.mips;++mip) {
    const auto dimension=[&](uint32_t d) { return mip<32?std::max(1u,d>>mip):1u; };
    uint64_t n=multiply(dimension(source.extent.width),dimension(source.extent.height));
    n=multiply(n,dimension(source.extent.depth));n=multiply(n,source.arrayLayers);
    total=n>UINT64_MAX-total?UINT64_MAX:total+n;
  }
  return total;
}
void report_feedback_stats() {
  static uint64_t previousFrame=0;
  if(R.frame-previousFrame<120) return;
  const double frames=double(R.frame-previousFrame);
  const auto& t=feedbackStats;
  LOG("[vulkan feedback copies] %.2f aliases/frame %.2f duplicate source versions/frame; %.3f M physical texels/frame %.3f M duplicate texels/frame; color/depth/stencil/compressed %llu/%llu/%llu/%llu; %llu saturated; copies unchanged",
      t.aliases/frames,t.duplicates/frames,t.texels/frames/1e6,t.duplicateTexels/frames/1e6,
      (unsigned long long)t.color,(unsigned long long)t.depth,(unsigned long long)t.stencil,
      (unsigned long long)t.compressed,(unsigned long long)t.saturated);
  feedbackStats={};previousFrame=R.frame;
}
VkImageView feedback_view(Surface *source, const uint32_t *textureWords,
                          bool vertex, uint32_t unit, FeedbackStatsProbe* probe) {
  if(probe) {
    const bool duplicate=probe->observe(*source);
    const uint64_t texels=feedback_texels(*source);
    if(texels==UINT64_MAX) ++feedbackStats.saturated;
    auto add=[&](uint64_t& sum) { if(texels>UINT64_MAX-sum) {sum=UINT64_MAX;++feedbackStats.saturated;} else sum+=texels; };
    ++feedbackStats.aliases;feedbackStats.duplicates+=duplicate;
    add(feedbackStats.texels);if(duplicate)add(feedbackStats.duplicateTexels);
    feedbackStats.color+=bool(source->aspect&VK_IMAGE_ASPECT_COLOR_BIT);
    feedbackStats.depth+=bool(source->aspect&VK_IMAGE_ASPECT_DEPTH_BIT);
    feedbackStats.stencil+=bool(source->aspect&VK_IMAGE_ASPECT_STENCIL_BIT);
    feedbackStats.compressed+=source->fmt.compressed;
  }
  static const bool reuse = draw_option_enabled("WWHD_VK_REUSE_FEEDBACK_IMAGES");
  Surface temporary;
  Surface* copy = &temporary;
  FeedbackScratch* slot = nullptr;
  if (reuse && unit < kFeedbackUnitsPerStage) {
    auto& shapes = feedbackScratch[(vertex ? kFeedbackUnitsPerStage : 0) + unit];
    for (auto& candidate : shapes) {
      if (candidate.device == R.device && feedback_compatible(candidate.surface, *source)) {
        slot = &candidate;break;
      }
      if (!slot && !candidate.surface.image) slot = &candidate;
    }
    // More than four shapes or the byte budget uses fence-retired temporaries;
    // don't evict a recurring shape merely because a one-off target appeared.
  }
  // Device recreation is not a supported lifecycle today; refuse to reuse or
  // retire foreign handles if a caller nevertheless changes the device.
  if (slot && slot->surface.image && slot->device != R.device) slot = nullptr;
  if (slot && feedback_compatible(slot->surface, *source)) {
    copy = &slot->surface;
  } else {
    if (slot && slot->surface.image) {
      end_encoder();
      destroy_surface_image(&slot->surface);
      feedbackRetainedBytes -= slot->allocationBytes;
      *slot = {};
    }
    make_feedback_image(temporary, *source);
    // debug: WWHD_VK_FEEDBACK_ALLOC_LOG=1 reports feedback image creations every 120 frames
    static const bool allocLog = getenv("WWHD_VK_FEEDBACK_ALLOC_LOG") != nullptr;
    if (allocLog) {
      static uint64_t creates = 0, unslotted = 0, overBudget = 0, lastFrame = 0;
      ++creates;
      if (!slot) ++unslotted;
      if (R.frame - lastFrame >= 120) {
        LOG("[vulkan feedback allocs] %.1f/frame (%.1f without a slot: unit %u, %.1f over budget), retained %.1f MiB, last %ux%u fmt %d mips %u",
            creates / double(R.frame - lastFrame), unslotted / double(R.frame - lastFrame), unit,
            overBudget / double(R.frame - lastFrame), feedbackRetainedBytes / 1048576.0, source->extent.width,
            source->extent.height, int(source->fmt.pixel), source->mips);
        creates = unslotted = overBudget = 0;
        lastFrame = R.frame;
      }
      if (slot) {
        VkMemoryRequirements rq{};
        vkGetImageMemoryRequirements(R.device, temporary.image, &rq);
        if (rq.size > kFeedbackRetainedBudget - feedbackRetainedBytes) ++overBudget;
      }
    }
    if (slot) {
      if (temporary.allocationBytes <= kFeedbackRetainedBudget - feedbackRetainedBytes) {
        slot->surface = std::move(temporary);
        temporary = {};
        slot->device = R.device;
        slot->allocationBytes = slot->surface.allocationBytes;
        feedbackRetainedBytes += slot->allocationBytes;
        copy = &slot->surface;
      }
    }
  }
  transition_image(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  // Retained layout preserves the read→write dependency on preceding draws,
  // including draws in an earlier submission on this same graphics queue.
  transition_image(copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT);
  static thread_local std::vector<VkImageCopy> regions;
  regions.clear();
  for (auto aspect : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_ASPECT_STENCIL_BIT}) {
    if (!(source->aspect & aspect))
      continue;
    for (uint32_t mip = 0; mip < source->mips; ++mip) {
      VkImageCopy region{};
      region.srcSubresource = {VkImageAspectFlags(aspect), mip, 0,
                               source->arrayLayers};
      region.dstSubresource = region.srcSubresource;
      region.extent = {std::max(1u, source->extent.width >> mip),
                       std::max(1u, source->extent.height >> mip),
                       std::max(1u, source->extent.depth >> mip)};
      regions.push_back(region);
    }
  }
  GpuScopeToken feedbackTiming;
  if(R.gpuPassTimestampsEnabled) feedbackTiming=gpu_begin_feedback_scope(*source);
  vkCmdCopyImage(command_buffer(), source->image,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions.size(),
                 regions.data());
  if(R.gpuPassTimestampsEnabled) gpu_end_feedback_scope(feedbackTiming);
  transition_image(copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
  VkImageView view = sampled_texture_view(copy, textureWords);
  destroy_surface_image(&temporary); // Temporary snapshots remain fence-retired.
  return view;
}
// debug: WWHD_VK_PHASES=1 also splits bind_stage (us per frame, both stages, every 120 frames)
namespace bphases {
inline uint64_t now() {
#if defined(__aarch64__)
  uint64_t t; asm volatile("mrs %0, cntpct_el0" : "=r"(t)); return t;
#else
  return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}
inline bool enabled() { static const bool v = std::getenv("WWHD_VK_PHASES") != nullptr; return v; }
uint64_t acc[13] = {}, last = 0, frame = 0;
const char* const names[13] = {"pack", "ublocks", "textures", "support", "ranks", "desc-hit", "desc-new",
                               "sh-early", "sh-fetch", "sh-translate", "sh-tail", "#translated", "#fetched"};
inline void start() { if (enabled()) last = now(); }
inline void mark(int i) { if (enabled()) { uint64_t t = now(); acc[i] += t - last; last = t; } }
inline void report() {
  if (!enabled() || R.frame == frame || R.frame % 120) return;
  frame = R.frame;
#if defined(__aarch64__)
  const double k = 1e6 / 19200000.0 / 120.0;
#else
  const double k = 1e-3 / 120.0;
#endif
  char line[512]; int n = 0;
  for (int i = 0; i < 11; ++i) n += snprintf(line + n, sizeof line - n, "%s %.0f | ", names[i], acc[i] * k);
  for (int i = 11; i < 13; ++i) n += snprintf(line + n, sizeof line - n, "%s %.0f | ", names[i], acc[i] / 120.0);
  LOG("[vulkan bind phases] us/frame: %s", line);
  for (auto& a : acc) a = 0;
}
}  // namespace bphases
StageResources bind_stage(const uint32_t *r, vk::Shader *sh,
                          VkDescriptorSetLayout layout, bool dynamicUniforms, float sx, float sy,
                          const std::array<Surface *, 8> &colors,
                          Surface *depth, FeedbackStatsProbe* feedbackProbe, bool tracked) {
  StageResources out;
  bphases::start();
  auto &m = sh->mapping;
  // Descriptor info pointers must remain stable until this stage's one update.
  std::array<VkDescriptorBufferInfo, 17> bufferInfos;
  std::array<VkDescriptorImageInfo, LATTE_NUM_MAX_TEX_UNITS> imageInfos;
  std::array<VkWriteDescriptorSet, 17 + LATTE_NUM_MAX_TEX_UNITS> writes;
  uint32_t bufferCount = 0, imageCount = 0, writeCount = 0;
  std::array<std::pair<uint32_t, uint32_t>, 17> dynamicBindings;
  uint32_t dynamicCount = 0;
  static const bool ranksEnabled = draw_option_enabled("WWHD_VK_DESCRIPTOR_RANKS");
  const auto& rankPlan = sh->descriptorRanks;
  const bool useRanks = ranksEnabled && rankPlan.valid;
  auto& last = lastDescriptors[sh->vertex ? 0 : 1];
  const uint64_t preparationGeneration = R.submissionGeneration;
  // Compare fresh identities while constructing them. Only a complete rank
  // plan has the same dense ordering as the previously remembered descriptor.
  bool rankDescriptorMatch = tracked && useRanks && last.set && last.layout == layout &&
      last.count == rankPlan.count && descriptorCacheGeneration == preparationGeneration;
  auto observeDescriptor = [&](const VkWriteDescriptorSet& write, uint8_t rank) {
    if (rankDescriptorMatch &&
        (rank >= last.count || !descriptor_identity_matches(last.identities[rank], write)))
      rankDescriptorMatch = false;
  };
  std::array<bool, 17 + LATTE_NUM_MAX_TEX_UNITS> occupied;
  std::array<uint32_t, 17 + LATTE_NUM_MAX_TEX_UNITS> rankedOffsets;
  if (useRanks) occupied.fill(false);
  auto appendWrite = [&](uint8_t rank) -> VkWriteDescriptorSet & {
    if (writeCount >= writes.size())
      throw std::runtime_error("stage descriptor write capacity exceeded");
    const size_t slot = useRanks ? size_t(rank) : size_t(writeCount);
    if (useRanks && (slot >= rankPlan.count || occupied[slot]))
      throw std::runtime_error("stage descriptor rank metadata mismatch");
    ++writeCount;
    if (useRanks) occupied[slot] = true;
    auto &write = writes[slot];
    write = {}; // Initialize every active field; unused capacity is never read.
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = out.set;
    write.descriptorCount = 1;
    return write;
  };
  auto uniform = [&](int binding, const void *bytes, size_t size, size_t logicalSlot,
                     const UploadSlice* packed = nullptr) {
    if (binding < 0)
      return;
    if (size > R.properties.limits.maxUniformBufferRange)
      throw std::runtime_error("uniform buffer exceeds device range");
    if(preparation_stats_enabled()) {
      ++R.cpuPreparation.uniformSnapshotCalls;
      R.cpuPreparation.uniformSnapshotBytes+=size;
    }
    static const bool reuseUniforms = [] {
      const char* value = std::getenv("WWHD_VK_REUSE_UNIFORM_SNAPSHOTS");
      return value && std::strcmp(value, "1") == 0;
    }();
    static UniformSnapshotCache<UploadSlice, VkDevice> uniformCache;
    auto fresh = [&](const void* source, size_t length) {
      return snapshot(source, length, R.properties.limits.minUniformBufferOffsetAlignment);
    };
    UploadSlice b;
#ifdef __SWITCH__
    static const bool reuseShaderSupport = draw_option_enabled("WWHD_VK_SUPPORT_UPLOAD_REUSE");
    if (!packed && logicalSlot == 16 && reuseShaderSupport) {
      b = supportSnapshotCache.get(sh, R.device, R.submissionGeneration, bytes, size, fresh);
      if (preparation_stats_enabled()) {
        R.cpuPreparation.supportReuseChecks = supportSnapshotCache.checks;
        R.cpuPreparation.supportReuseHits = supportSnapshotCache.hits;
        R.cpuPreparation.supportReuseBytes = supportSnapshotCache.reusedBytes;
      }
    } else
#endif
    b = packed ? *packed : reuseUniforms
        ? uniformCache.get(R.device, R.submissionGeneration,
                           (sh->vertex ? 0 : 17) + logicalSlot, bytes, size, fresh)
        : fresh(bytes, size);
    if (reuseUniforms && preparation_stats_enabled()) {
      const auto& counts = uniformCache.counters;
      R.cpuPreparation.uniformReuseChecks = counts.checks;
      R.cpuPreparation.uniformReuseComparisons = counts.comparisons;
      R.cpuPreparation.uniformReuseHits = counts.hits;
      R.cpuPreparation.uniformReuseBytes = counts.reusedBytes;
    }
    if (bufferCount >= bufferInfos.size())
      throw std::runtime_error("stage uniform descriptor capacity exceeded");
    auto &info = bufferInfos[bufferCount++];
    info = {b.buffer, dynamicUniforms ? 0 : b.offset, b.size};
    if (dynamicUniforms) {
      if (b.offset > UINT32_MAX)
        throw std::runtime_error("dynamic uniform offset exceeds uint32 range");
      if (useRanks) {
        const auto rank = logicalSlot == 16 ? rankPlan.support : rankPlan.blocks[logicalSlot];
        if (rank >= rankPlan.count)
          throw std::runtime_error("stage uniform rank metadata mismatch");
        rankedOffsets[rank] = uint32_t(b.offset);
      } else dynamicBindings[dynamicCount++] = {uint32_t(binding), uint32_t(b.offset)};
    }
    auto &write = appendWrite(logicalSlot == 16 ? rankPlan.support : rankPlan.blocks[logicalSlot]);
    write.dstBinding = binding;
    write.descriptorType = dynamicUniforms
        ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
        : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    observeDescriptor(write, logicalSlot == 16 ? rankPlan.support : rankPlan.blocks[logicalSlot]);
  };
  // CPU-only scratch supplies an immutable upload slice below.
  // Separate stages retain capacity without retaining guest payload contents.
  static thread_local std::array<std::vector<uint8_t>, 2> supportScratch;
  auto& supportUniforms = supportScratch[sh->vertex ? 0 : 1];
  // Switch: pack straight into a cached upload slice (no scratch copy). The texture-scale and AO
  // patches below then write (and the AO patch reads) the slice: cached memory, owned by this draw.
  UploadSlice supportSlice{};
  uint8_t* supportData = nullptr;
  size_t supportSize = 0;
#ifdef __SWITCH__
  static const bool reuseSupport = [] {
    const char* value = std::getenv("WWHD_VK_REUSE_UNIFORM_SNAPSHOTS");
    return value && std::strcmp(value, "1") == 0;
  }();
  static const bool reuseShaderSupport = draw_option_enabled("WWHD_VK_SUPPORT_UPLOAD_REUSE");
  if (m.uniformVarsBufferBindingPoint >= 0 && !reuseSupport && !reuseShaderSupport) {
    supportSize = vk::support_uniform_size(*sh);
    if (supportSize > R.properties.limits.maxUniformBufferRange)
      throw std::runtime_error("uniform buffer exceeds device range");
    supportSlice = allocate_upload(supportSize, R.properties.limits.minUniformBufferOffsetAlignment);
    if (supportSlice.cpuReadable) {
      supportData = static_cast<uint8_t*>(supportSlice.mapped);
      vk::pack_uniforms_raw(r, *sh, supportData, sx, sy);
      if (supportSlice.size > supportSize)
        std::memset(supportData + supportSize, 0, supportSlice.size - supportSize);
      supportSlice.size = supportSize;
      if (preparation_stats_enabled()) { ++R.cpuPreparation.packCalls; R.cpuPreparation.packBytes += supportSize; }
    } else supportSlice = {};  // uncached fallback memory: the scratch path below
  }
#endif
  if (supportData) {
  } else if (m.uniformVarsBufferBindingPoint >= 0) {
    const size_t capacity=preparation_stats_enabled()?supportUniforms.capacity():0;
    vk::pack_uniforms_into(r, *sh, supportUniforms, sx, sy);
    if(preparation_stats_enabled()) {
      ++R.cpuPreparation.packCalls;
      R.cpuPreparation.packBytes+=supportUniforms.size();
      if(supportUniforms.capacity()>capacity) {
        ++R.cpuPreparation.packCapacityGrowths;
        R.cpuPreparation.packCapacityGrowthBytes+=supportUniforms.capacity()-capacity;
      }
    }
  }
  else
    supportUniforms.clear();
  if (!supportData) { supportData = supportUniforms.data(); supportSize = supportUniforms.size(); }
  // Metal AO mode 2 tiles noise per 960x540 output pixel rather than 640x360.
  const int remapped = sh->uniforms.offset_remapped;
  if (sh->vertex && ao_mode() == 2 &&
      (r[mmSQ_PGM_START_VS] << 8) == kOcclusionVS && remapped >= 0 &&
      size_t(remapped) + 16 <= supportSize) {
    float noiseScale;
    memcpy(&noiseScale, supportData + remapped + 12, sizeof noiseScale);
    noiseScale *= 1.5f;
    memcpy(supportData + remapped + 12, &noiseScale, sizeof noiseScale);
  }

  bphases::mark(0);
  uint32_t block =
      sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
  for (int i = 0; i < 16; i++)
    if (m.uniformBuffersBindingPoint[i] >= 0) {
      uint32_t addr = r[block + i * 7];
      uint32_t size = std::min<uint32_t>(r[block + i * 7 + 1] + 1, 0x10000);
      uniform(m.uniformBuffersBindingPoint[i], addr ? mem::ptr(addr) : nullptr,
              size, size_t(i));
    }
  bphases::mark(1);
  uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS
                                : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
  for (int i = 0; i < sh->dec->textureUnitListCount; i++) {
    uint32_t unit = sh->dec->textureUnitList[i];
    if (unit >= LATTE_NUM_MAX_TEX_UNITS)
      throw std::runtime_error("sampled texture unit exceeds stage capacity");
    int binding = m.textureUnitToBindingPoint[unit];
    if (binding < 0)
      continue;
    const bool compare = sh->dec->textureUsesDepthCompare[unit];
    const uint32_t stage = sh->vertex ? 1 : 0;
    const uint64_t textureGeneration = gx2::drawStateGenerations.textures[stage][unit];
    auto& texture = resolvedTextures[stage][unit];
    Surface* s = nullptr;
    // Same descriptor and alias-selection epoch; content checks remain below.
    if (tracked && texture.regs == r && texture.generation == textureGeneration &&
        texture.epoch == R.surfaceEpoch && texture.compare == compare && texture.surface &&
        texture.surface->addressState &&
        texture.addressGeneration == texture.surface->addressState->generation) {
      s = texture.surface;
      // sampled_texture also checks the original guest surface before any AO
      // substitution. Keep that check on hits, including when AO redirects it.
      if (s->image && !s->gpuWritten && s->lastCheckedFrame != R.frame) upload_surface(s);
      if (preparation_stats_enabled()) ++R.cpuPreparation.textureSkips;
    } else {
      s = sampled_texture(r + texbase + unit * 7, compare);
      if (tracked && s) {
        texture.regs=r;texture.generation=textureGeneration;texture.epoch=R.surfaceEpoch;
        texture.compare=compare;texture.surface=s;
        texture.addressGeneration=s->addressState ? s->addressState->generation : 0;
      }
    }
    if (!s)
      throw std::runtime_error("missing sampled texture");
    auto* guestSurface = s;
    if (!sh->vertex && ao_hires_enabled() && aoPrivateSource &&
        s->addr == aoPrivateSource && aoPrivateFrame == R.frame &&
        (r[mmSQ_PGM_START_PS] << 8) == kOcclusionPS)
      s = &aoPrivateColor;
    // Both sampled_texture and the memo-hit branch validated the guest surface.
    // An AO substitution is the only new surface requiring a second check.
    if (s != guestSurface) upload_surface(s);
    int scaleOffset = sh->uniforms.offset_texScale[unit];
    if (scaleOffset >= 0 && size_t(scaleOffset) + 8 <= supportSize) {
      const float scale[] = {s->sx, s->sy};
      memcpy(supportData + scaleOffset, scale, sizeof scale);
    }
    bool aliases = depth && s->image == depth->image;
    for (auto *color : colors)
      if (color && s->image == color->image)
        aliases = true;
    VkImageView sampledView;
    if (aliases)
      sampledView = feedback_view(s, r + texbase + unit * 7, sh->vertex, unit, feedbackProbe);
    else if (tracked && texture.view && texture.viewSurface == s &&
             texture.viewEpoch == R.surfaceEpoch && texture.viewGeneration == textureGeneration) {
      sampledView = texture.view;
      if (preparation_stats_enabled()) ++R.cpuPreparation.viewSkips;
    } else {
      sampledView = sampled_texture_view(s, r + texbase + unit * 7);
      if (tracked) {
        texture.view=sampledView;texture.viewSurface=s;
        texture.viewEpoch=R.surfaceEpoch;texture.viewGeneration=textureGeneration;
      }
    }
    if (!aliases && s->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      transition_image(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
    uint32_t samplerId = sh->dec->textureUnitSamplerAssignment[unit];
    if (samplerId >= 18)
      throw std::runtime_error("missing texture sampler");
    uint32_t samplerBase = sh->vertex ? 18 : 0;
    if (imageCount >= imageInfos.size())
      throw std::runtime_error("stage image descriptor capacity exceeded");
    auto &info = imageInfos[imageCount++];
    const uint32_t* samplerWords = r + REGADDR::SQ_TEX_SAMPLER_WORD0_0 +
                                    (samplerBase + samplerId) * 3;
    uint32_t patchedSampler[3];
    const bool patched = !sh->vertex && ao_mode() >= 1 && unit == 0 &&
        (r[mmSQ_PGM_START_PS] << 8) == kOcclusionPS;
    const uint32_t samplerSlot = samplerBase + samplerId;
    const uint64_t samplerGeneration = gx2::drawStateGenerations.samplers[samplerSlot];
    const bool integer = s->fmt.kind != FormatInfo::FLOAT;
    const bool allowAniso = !s->gpuWritten && s->mips > 1;
    const bool forceAniso = allowAniso && aniso_enabled();
    static const bool imageMemo = draw_option_enabled("WWHD_VK_IMAGE_DESCRIPTOR_MEMO");
    if (tracked && imageMemo && !aliases && texture.image_matches(R.device, R.surfaceEpoch, sampledView,
        samplerSlot, samplerGeneration, patched, compare, integer, forceAniso)) {
      info = texture.descriptor;
      if (preparation_stats_enabled()) {
        ++R.cpuPreparation.samplerRequests; ++R.cpuPreparation.samplerMemoHits;
      }
    } else {
      if (patched) {
        memcpy(patchedSampler, samplerWords, sizeof patchedSampler);
        patchedSampler[0] = (patchedSampler[0] & ~0x7E00u) | (1u << 9) | (1u << 12);
        samplerWords = patchedSampler;
      }
      info = {sampler(samplerWords, compare, integer, forceAniso, samplerSlot,
                     tracked ? samplerGeneration : ~uint64_t{0}, patched),
              sampledView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      if (tracked && imageMemo && !aliases) {
        texture.descriptor=info; texture.descriptorDevice=R.device; texture.descriptorEpoch=R.surfaceEpoch;
        texture.samplerSlot=samplerSlot; texture.samplerGeneration=samplerGeneration;
        texture.samplerPatched=patched; texture.samplerCompare=compare;
        texture.samplerInteger=integer; texture.samplerAniso=forceAniso;
      }
    }
    auto &write = appendWrite(rankPlan.textures[unit]);
    write.dstBinding = binding;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    observeDescriptor(write, rankPlan.textures[unit]);
  }
  bphases::mark(2);
  if (m.uniformVarsBufferBindingPoint >= 0)
    uniform(m.uniformVarsBufferBindingPoint, supportData, supportSize, 16,
            supportSlice.buffer ? &supportSlice : nullptr);
  bphases::mark(3);
  const bool completeRanks = useRanks && writeCount == rankPlan.count;
  if (completeRanks) {
    // Preparation wrote directly into the dense rank slots. Only dynamic
    // offsets need gathering; their binding order was computed at translation.
    if (dynamicUniforms)
      for (uint32_t i = 0; i < rankPlan.uniformCount; ++i)
        out.dynamicOffsets[dynamicCount++] = rankedOffsets[rankPlan.uniformRanks[i]];
  } else if (useRanks) {
    uint32_t dense = 0;
    for (uint32_t rank = 0; rank < rankPlan.count; ++rank) {
      if (!occupied[rank]) continue;
      const auto type = writes[rank].descriptorType;
      writes[dense++] = writes[rank];
      if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        dynamicBindings[dynamicCount++] = {writes[dense-1].dstBinding, rankedOffsets[rank]};
    }
  } else {
    std::sort(writes.begin(), writes.begin() + writeCount,
        [](const auto &a, const auto &b) { return a.dstBinding < b.dstBinding; });
    std::sort(dynamicBindings.begin(), dynamicBindings.begin() + dynamicCount);
  }
  out.dynamicOffsetCount = dynamicCount;
  if (!completeRanks)
    for (uint32_t i = 0; i < dynamicCount; ++i)
      out.dynamicOffsets[i] = dynamicBindings[i].second;
  // Resource preparation above must always run, even when the descriptor set
  // itself is reusable. Pool reset/slot activation invalidates every old set.
  static WordCache<VkDescriptorSet> descriptorCache;
#ifdef __SWITCH__
  // Persistent sets (Switch, dynamic uniform offsets): sets live in a long-lived pool and survive
  // submissions, so the ~600 sets a frame are neither re-allocated/written nor destroyed by the
  // per-submission pool reset. A set names image views, samplers and upload buffers: views die
  // only through destroy_surface_image (R.viewEpoch), samplers and upload blocks live as long as
  // the device (reset_pipeline_lookup_cache drops the sampler cache: it also clears this one). On an
  // epoch change, or when the pool fills, the pool goes to the recording submission's garbage
  // (destroyed after its fence: earlier binds are on the same queue) and a fresh one starts.
  struct PersistentSets {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint64_t epoch = 0;
    uint32_t sets = 0, ubos = 0, dynamicUbos = 0, images = 0;
    WordCache<VkDescriptorSet> cache;
  };
  static PersistentSets persistent;
  static const bool persistentEnabled = draw_option_enabled("WWHD_VK_PERSISTENT_SETS");
  const bool usePersistent = persistentEnabled && dynamicUniforms;
  constexpr uint32_t kPoolSets = 16384, kPoolUbos = 16384, kPoolDynamic = 32768, kPoolImages = 65536;
  if (usePersistent && (!persistent.pool || persistent.epoch != R.viewEpoch || persistentSetsReset)) {
    if (persistent.pool) defer_descriptor_pool(persistent.pool);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPoolUbos},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kPoolDynamic},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPoolImages}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kPoolSets;
    dp.poolSizeCount = std::size(sizes);
    dp.pPoolSizes = sizes;
    persistent.pool = VK_NULL_HANDLE;
    vk_check(vkCreateDescriptorPool(R.device, &dp, nullptr, &persistent.pool), "create persistent descriptor pool");
    persistent.epoch = R.viewEpoch;
    persistent.sets = persistent.ubos = persistent.dynamicUbos = persistent.images = 0;
    persistent.cache.clear();
    lastDescriptors[0] = {}; lastDescriptors[1] = {};
    persistentSetsReset = false;
  }
  auto& cache = usePersistent ? persistent.cache : descriptorCache;
#else
  auto& cache = descriptorCache;
#endif
  if (descriptorCacheGeneration != R.submissionGeneration) {
    descriptorCache.clear();
    lastDescriptors[0] = {}; lastDescriptors[1] = {};
    descriptorCacheGeneration = R.submissionGeneration;
  }
  ++R.descriptorLookups;
  if ((rankDescriptorMatch && writeCount == rankPlan.count &&
       preparationGeneration == R.submissionGeneration) ||
      descriptor_matches(last, layout, writes.data(), writeCount)) {
    ++R.descriptorCacheHits;
    ++R.descriptorFastHits;
    out.set = last.set;
    bphases::mark(4);
    return out;
  }
  bphases::mark(4);
  DescriptorKey key;
  descriptor_key(key, layout, writes.data(), writeCount, completeRanks);
  if (auto found = cache.find(key.view())) {
    ++R.descriptorCacheHits;
    out.set = found;
    remember_descriptors(last, layout, out.set, writes.data(), writeCount);
    bphases::mark(5);
    return out;
  }
#ifdef __SWITCH__
  VkDescriptorPool allocationPool = R.descriptorPool;
  if (usePersistent) {
    uint32_t ubos = 0, dynamicUbos = 0, images = 0;
    for (uint32_t i = 0; i < writeCount; ++i) {
      const auto type = writes[i].descriptorType;
      ubos += type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      dynamicUbos += type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      images += type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    if (persistent.sets + 1 > kPoolSets || persistent.ubos + ubos > kPoolUbos ||
        persistent.dynamicUbos + dynamicUbos > kPoolDynamic || persistent.images + images > kPoolImages) {
      // Full: the next lookup starts a fresh pool. This set uses the per-submission pool.
      persistentSetsReset = true;
    } else {
      allocationPool = persistent.pool;
      ++persistent.sets; persistent.ubos += ubos; persistent.dynamicUbos += dynamicUbos; persistent.images += images;
    }
  }
  if (vkrecord::enabled()) {
    // Allocated and written on the recording thread, in order before any bind of this set.
    out.set = vkrecord::defer_descriptor_set(R.device, allocationPool, layout, writes.data(), writeCount);
    ++R.descriptorAllocations;
  } else
#endif
  {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
#ifdef __SWITCH__
  ai.descriptorPool = allocationPool;
#else
  ai.descriptorPool = R.descriptorPool;
#endif
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  vk_check(vkAllocateDescriptorSets(R.device, &ai, &out.set), "descriptor set");
  ++R.descriptorAllocations;
  for (uint32_t i = 0; i < writeCount; ++i) writes[i].dstSet = out.set;
  if (writeCount)
    vkUpdateDescriptorSets(R.device, writeCount, writes.data(), 0, nullptr);
  }
#ifdef __SWITCH__
  if (allocationPool == R.descriptorPool) descriptorCache.insert(key.view(), out.set);
  else cache.insert(key.view(), out.set);
#else
  descriptorCache.insert(key.view(), out.set);
#endif
  remember_descriptors(last, layout, out.set, writes.data(), writeCount);
  bphases::mark(6);
  bphases::report();
  return out;
}
} // namespace
void reset_feedback_images() { reset_feedback_scratch(); }
Surface* feedback_image_smoke_snapshot(Surface& source,bool vertex,uint32_t unit) {
  if(unit >= kFeedbackUnitsPerStage) throw std::runtime_error("feedback smoke unit is out of range");
  uint32_t words[7]={source.dim,0,0,0,(1u<<19)|(2u<<22)|(3u<<25),0,0};
  const auto view=feedback_view(&source,words,vertex,unit,nullptr);
  for(auto& slot : feedbackScratch[(vertex?kFeedbackUnitsPerStage:0)+unit])
    for(auto& [key,candidate] : slot.surface.sampledViews)
      if(candidate==view) return &slot.surface;
  throw std::runtime_error("feedback smoke requires retained images within budget");
}
UploadSlice vertex_window_smoke_snapshot(uint32_t binding,uint32_t address,
    uint32_t reservation,uint32_t windowOffset,uint32_t windowLength,
    const void* data,bool poisonUnused) {
  if(!data || !windowLength || windowOffset>reservation ||
     windowLength>reservation-windowOffset)
    throw std::runtime_error("invalid vertex window smoke bounds");
  return vertex_window_snapshot(binding,address,reservation,windowOffset,
                                windowLength,data,poisonUnused);
}

namespace {
// pipelines.bin: "WWVKPR02", u64 count, u64 payload checksum (FNV-1a), payload of
// {u16 key size, key bytes, u64 fetch key, 8 color kinds, u8 stencil} records.
constexpr char kRecipeMagic[8] = {'W','W','V','K','P','R','0','2'};
std::string pipeline_recipe_path() {
  const char* shaderPath = std::getenv("WWHD_SHADER_CACHE");
  if (shaderPath && !std::strcmp(shaderPath, "0")) return {};
  return host::config_dir() + "/shadercache/pipelines.bin";
}
uint64_t recipe_checksum(const uint8_t* p, size_t n) {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
void serialize_recipes() {
  if (recipeSerialized > pipelineRecipes.size()) { recipePayload.clear(); recipeSerialized = 0; }
  for (; recipeSerialized < pipelineRecipes.size(); ++recipeSerialized) {
    const auto& r = pipelineRecipes[recipeSerialized];
    uint16_t n = uint16_t(r.key.size());
    recipePayload.push_back(uint8_t(n)); recipePayload.push_back(uint8_t(n >> 8));
    recipePayload.insert(recipePayload.end(), r.key.begin(), r.key.end());
    for (int i = 0; i < 8; i++) recipePayload.push_back(uint8_t(r.fsKey >> (i * 8)));
    recipePayload.insert(recipePayload.end(), r.kinds.begin(), r.kinds.end());
    recipePayload.push_back(r.stencil);
  }
}
bool write_pipeline_recipes(std::vector<uint8_t> payload, uint64_t count, std::string path) {
  try {
    std::vector<uint8_t> header(kRecipeMagic, kRecipeMagic + 8);
    auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) header.push_back(uint8_t(v >> (i * 8))); };
    put64(count);
    put64(recipe_checksum(payload.data(), payload.size()));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string temporary = path + ".tmp";
    FILE* f = std::fopen(temporary.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(header.data(), 1, header.size(), f) == header.size() &&
              std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    if (std::fclose(f)) ok = false;
    if (ok) ok = host::replace_file(temporary, path);
    if (!ok) std::filesystem::remove(temporary, ec);
    return ok;
  } catch (...) { return false; }
}
std::vector<PipelineRecipe> read_pipeline_recipes(const std::string& path) {
  std::vector<PipelineRecipe> result;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return result;
  std::vector<uint8_t> file;
  std::vector<uint8_t> buffer(65536);
  for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0;)
    file.insert(file.end(), buffer.begin(), buffer.begin() + n);
  std::fclose(f);
  auto get64 = [&](size_t o) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= uint64_t(file[o + i]) << (i * 8); return v; };
  if (file.size() < 24 || std::memcmp(file.data(), kRecipeMagic, 8)) return result;
  const uint64_t count = get64(8);
  if (get64(16) != recipe_checksum(file.data() + 24, file.size() - 24) || count > maxPipelineRecipes) return result;
  size_t o = 24;
  for (uint64_t i = 0; i < count; i++) {
    if (file.size() - o < 2) return {};
    size_t n = file[o] | size_t(file[o + 1]) << 8;
    o += 2;
    if (file.size() - o < n + 17) return {};
    PipelineRecipe r;
    r.key.assign(reinterpret_cast<const char*>(file.data() + o), n);
    o += n;
    r.fsKey = get64(o);
    o += 8;
    std::memcpy(r.kinds.data(), file.data() + o, 8);
    r.stencil = file[o + 8];
    o += 9;
    result.push_back(std::move(r));
  }
  if (o != file.size()) result.clear();
  return result;
}
std::future<bool> pipelineRecipeWrite;
} // namespace
// Render thread, once per frame: write the recipes after 120 quiet frames (worker thread).
void checkpoint_pipeline_recipes() {
  if (pipelineRecipeWrite.valid()) {
    if (pipelineRecipeWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    if (!pipelineRecipeWrite.get()) LOG("[vulkan pipelines] recipe save failed");
  }
  if (!pipelineRecipesDirty || R.frame - pipelineRecipesChangedFrame < 120 ||
      R.frame - pipelineRecipesAttemptFrame < 120) return;
  const std::string path = pipeline_recipe_path();
  if (path.empty()) return;
  pipelineRecipesAttemptFrame = R.frame;
  pipelineRecipesDirty = false;
  try {
    serialize_recipes();
    pipelineRecipeWrite = std::async(std::launch::async, [payload = recipePayload, count = uint64_t(recipeSerialized),
                                                          path]() mutable {
      host::background_thread();  // only time the game and render threads leave idle
      return write_pipeline_recipes(std::move(payload), count, std::move(path));
    });
  } catch (...) { pipelineRecipesDirty = true; }
}
// Boot only, after warm_up_shader_cache and before the render and game threads start: recreate the
// recorded pipelines through the normal pipeline() path from a scratch register file.
void warm_up_pipelines() {
  static bool attempted = false;
  if (attempted) return;
  attempted = true;
  const std::string path = pipeline_recipe_path();
  if (path.empty()) return;
  const auto started = std::chrono::steady_clock::now();
  auto recipes = read_pipeline_recipes(path);
  // Variants with equal translations share one Shader and its key (shaders.cpp): a recipe recorded
  // under another variant's key is the shared shader's recipe, often a duplicate of one already kept.
  size_t renamed = 0;
  for (auto& r : recipes) {
    if (r.key.size() >= 16) {
      uint64_t keys[2];
      std::memcpy(keys, r.key.data(), 16);
      const vk::Shader* vs = vk::find_shader(keys[0]);
      const vk::Shader* ps = vk::find_shader(keys[1]);
      if (vs && ps && (vs->key != keys[0] || ps->key != keys[1])) {
        keys[0] = vs->key; keys[1] = ps->key;
        std::memcpy(r.key.data(), keys, 16);
        ++renamed;
      }
    }
    if (pipelineRecipeKeys.insert(r.key).second) pipelineRecipes.push_back(std::move(r));
  }
  if (renamed) {
    pipelineRecipesDirty = true;  // rewritten at the first quiet checkpoint
    std::fprintf(stderr, "[vulkan] pipeline recipes: %zu renamed to shared shaders, %zu of %zu kept\n",
                 renamed, pipelineRecipes.size(), recipes.size());
  }
  const char* enabled = std::getenv("WWHD_VK_PIPELINE_WARMUP");
  if (enabled && !std::strcmp(enabled, "0")) return;
  // key layout (pipeline()): vs, ps, fetch layout (u64 each), topology, 10 masked fields,
  // 8 blend controls, 8 color formats, depth format, one stride per fetch buffer group
  constexpr size_t kFixed = 24 + 4 + 40 + 32 + 32 + 4;
  const uint32_t fields[] = {REGADDR::CB_COLOR_CONTROL, REGADDR::CB_TARGET_MASK,
                             REGADDR::DB_DEPTH_CONTROL, REGADDR::DB_STENCILREFMASK,
                             REGADDR::DB_STENCILREFMASK_BF, REGADDR::PA_SU_SC_MODE_CNTL,
                             REGADDR::PA_CL_CLIP_CNTL, REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE,
                             REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET, REGADDR::PA_SU_POLY_OFFSET_CLAMP};
  std::vector<uint32_t> regs(gx2::kNumRegs);
  size_t created = 0, present = 0, missing = 0, failed = 0, mismatched = 0, noVs = 0, noPs = 0, noFetch = 0;
  bool timedOut = false;
  const uint64_t before = R.pipelineCreates;
  for (const auto& recipe : pipelineRecipes) {
    if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(20)) { timedOut = true; break; }
    const auto* k = reinterpret_cast<const uint8_t*>(recipe.key.data());
    if (recipe.key.size() < kFixed || (recipe.key.size() - kFixed) % 4) { ++failed; continue; }
    if (pipelines.count(recipe.key)) { ++present; continue; }
    uint64_t vsKey, psKey, layout;
    std::memcpy(&vsKey, k, 8); std::memcpy(&psKey, k + 8, 8); std::memcpy(&layout, k + 16, 8);
    vk::Shader* vs = vk::find_shader(vsKey);
    vk::Shader* ps = vk::find_shader(psKey);
    LatteFetchShader* fs = vk::find_fetch_shader(recipe.fsKey);
    noVs += !vs; noPs += !ps; noFetch += !fs;
    if (!vs || !ps || !fs) { ++missing; continue; }
    // the same fetch object the vertex shader was translated with (its key includes fsKey)
    if (fs->vkPipelineHashFragment != layout ||
        fs->bufferGroups.size() != (recipe.key.size() - kFixed) / 4) { ++mismatched; continue; }
    VkPrimitiveTopology topology;
    std::memcpy(&topology, k + 24, 4);
    std::fill(regs.begin(), regs.end(), 0);
    for (int i = 0; i < 10; i++) std::memcpy(&regs[fields[i]], k + 28 + 4 * i, 4);
    std::memcpy(&regs[REGADDR::CB_BLEND0_CONTROL], k + 68, 32);
    std::array<VkFormat, 8> formats;
    VkFormat df;
    std::memcpy(formats.data(), k + 100, 32);
    std::memcpy(&df, k + 132, 4);
    for (size_t g = 0; g < fs->bufferGroups.size(); g++) {
      uint32_t stride;
      std::memcpy(&stride, k + kFixed + 4 * g, 4);
      regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + fs->bufferGroups[g].attributeBufferIndex * 7 + 2] = stride << 11;
    }
    std::array<Surface, 8> colorTargets;
    std::array<Surface *, 8> colors{};
    for (int i = 0; i < 8; i++)
      if (formats[i] != VK_FORMAT_UNDEFINED) {
        colorTargets[i].fmt.pixel = formats[i];
        colorTargets[i].fmt.kind = FormatInfo::Kind(recipe.kinds[i]);
        colors[i] = &colorTargets[i];
      }
    Surface depthTarget;
    depthTarget.fmt.pixel = df;
    depthTarget.fmt.depth = true;
    depthTarget.fmt.stencil = recipe.stencil;
    try {
      pipeline(regs.data(), vs, ps, fs, topology, colors, df != VK_FORMAT_UNDEFINED ? &depthTarget : nullptr);
      if (pipelines.count(recipe.key)) ++created;
      else ++mismatched;
    } catch (const std::exception&) { ++failed; }
  }
  reset_pipeline_lookup_cache();
  // A recipe whose fetch layout hash no longer matches its fetch shader can never be recreated
  // (recorded by an older build): drop it. Missing shaders may still be warmed by a later session.
  pipelineRecipesDirty = renamed != 0;
  if (mismatched && !timedOut) {
    std::vector<PipelineRecipe> kept;
    kept.reserve(pipelineRecipes.size());
    for (auto& recipe : pipelineRecipes) {
      uint64_t layout;
      std::memcpy(&layout, recipe.key.data() + 16, 8);
      LatteFetchShader* fs = recipe.key.size() >= kFixed ? vk::find_fetch_shader(recipe.fsKey) : nullptr;
      if (fs && (fs->vkPipelineHashFragment != layout ||
                 fs->bufferGroups.size() != (recipe.key.size() - kFixed) / 4)) {
        pipelineRecipeKeys.erase(recipe.key);
        continue;
      }
      kept.push_back(std::move(recipe));
    }
    pipelineRecipes = std::move(kept);
    recipePayload.clear();
    recipeSerialized = 0;
    pipelineRecipesDirty = true;  // rewritten at the first quiet checkpoint
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::fprintf(stderr, "[vulkan] pipeline warm-up: %zu/%zu created (%llu Vulkan creates), %zu missing shaders (vs %zu ps %zu fetch %zu), "
               "%zu mismatched, %zu failed, %zu already present, %.1f ms%s\n",
               created, pipelineRecipes.size(), (unsigned long long)(R.pipelineCreates - before), missing, noVs, noPs, noFetch,
               mismatched, failed, present, ms, timedOut ? " (20 s budget)" : "");
}

void reset_ao_private_cache() { aoPrivateFrame = ~0ull; aoPrivateSource = 0; }
void reset_draw_state_cache() {
  persistentSetsReset=true;
  resolvedShaders={};resolvedTargets={};resolvedTextures={};resolvedPipelines[0]={};resolvedPipelines[1]={};
  lastDescriptors[0]={};lastDescriptors[1]={};descriptorCacheGeneration=~uint64_t{0};
#ifdef __SWITCH__
  supportSnapshotCache.reset();
#endif
}
void reset_pipeline_lookup_cache() { reset_draw_state_cache(); lastPipelineLookup = {}; lastPipelineState = {}; pipelineLookaside = {}; samplerMemos = {}; samplerCache.clear(); drawBatchState = {}; }
uint64_t draw_batch_submissions() { return drawBatchSubmissions; }
// debug: WWHD_VK_PHASES=1 logs the CPU time of gfxvk::draw's phases (us per frame, every 120 frames)
namespace phases {
inline uint64_t now() {
#if defined(__aarch64__)
  uint64_t t; asm volatile("mrs %0, cntpct_el0" : "=r"(t)); return t;
#else
  return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}
inline double tick_us() {
#if defined(__aarch64__)
  return 1e6 / 19200000.0;  // Horizon: cntpct_el0 at 19.2 MHz
#else
  return 1e-3;
#endif
}
inline bool enabled() { static const bool v = std::getenv("WWHD_VK_PHASES") != nullptr; return v; }
uint64_t acc[12] = {}, draws = 0, last = 0, frame = 0;
const char* const names[12] = {"shaders", "indices", "targets", "pipeline", "vs-bind", "ps-bind", "pass", "desc-bind", "dyn-state", "vtx-bind", "draw-call", "outside"};
uint64_t prev_end = 0;
inline void mark(int i) { if (enabled()) { uint64_t t = now(); acc[i] += t - last; last = t; } }
struct Guard {
  Guard() { if (enabled()) { uint64_t t = now(); if (prev_end) acc[11] += t - prev_end; last = t; } }
  ~Guard() {
    if (!enabled()) return;
    uint64_t t = now(); acc[10] += t - last; prev_end = t; ++draws;
    bphases::report();
    if (R.frame != frame && R.frame % 120 == 0) {
      frame = R.frame;
      const double k = tick_us() / 120.0;
      char line[512]; int n = 0;
      for (int i = 0; i < 12; ++i) n += snprintf(line + n, sizeof line - n, "%s %.0f | ", names[i], acc[i] * k);
      LOG("[vulkan phases] us/frame: %sdraws/frame %.0f", line, draws / 120.0);
      for (auto& a : acc) a = 0;
      draws = 0;
    }
  }
};
}  // namespace phases
// Pre-pass twins (WWHD_VK_PREPASS_TWINS=0 turns it off). The game draws its opaque and alpha-tested
// geometry twice per frame into the same depth buffer: a depth + normals pre-pass for the ambient
// occlusion, then the color pass with depth test LEQUAL and depth writes still on. Alpha-tested color
// draws end in a discard, so with depth writes on the GPU tests depth only after the whole (expensive)
// pixel shader ran: every hidden layer of grass is shaded (Outset grass close up: 28 ms of GPU per frame).
// The pre-pass already wrote exactly these depths, so a color draw whose mesh the pre-pass drew drops
// its redundant depth write; the GPU then tests depth before shading. Matching is by mesh (index and
// vertex buffers, counts) and alpha test, as many color draws per mesh as the pre-pass had (grass clumps
// share one mesh), within one frame and depth buffer.
namespace prepass {
struct Entry { const void* target = nullptr; uint32_t writers = 0; };
struct Mesh {
  std::array<uint32_t, 16> words{};
  bool operator==(const Mesh& o) const { return words == o.words; }
};
// open addressing, emptied each frame by a new stamp (no allocation per draw)
struct Slot { Mesh mesh; Entry entry; uint32_t stamp = 0; };
constexpr size_t slotCount = 8192;
std::vector<Slot> slots(slotCount);
uint32_t stamp = 1;
// depth buffer clear generations: part of the mesh key, so a clear forgets that buffer's meshes
std::vector<std::pair<const void*, uint32_t>> depthGenerations;
uint32_t depth_generation(const void* depth) {
  for (auto& [d, g] : depthGenerations) if (d == depth) return g;
  return 0;
}
Entry* find(const Mesh& mesh) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (uint32_t w : mesh.words) h = (h ^ w) * 0xBF58476D1CE4E5B9ull, h ^= h >> 31;
  for (size_t i = 0; i < 32; ++i) {
    Slot& slot = slots[(h + i) & (slotCount - 1)];
    if (slot.stamp != stamp) { slot = {mesh, {}, stamp}; return &slot.entry; }
    if (slot.mesh == mesh) return &slot.entry;
  }
  return nullptr;  // crowded: treat as unknown
}
uint64_t frame = ~0ull;
uint64_t stats[2] = {};  // draws without / with their depth write
bool enabled() {
  static const bool on = draw_option_enabled("WWHD_VK_PREPASS_TWINS");
  return on;
}
// true: this draw's depth write is redundant (the caller clears it)
bool redundant_depth_write(const uint32_t* r, const Surface* color, const Surface* depth, const LatteFetchShader* fs,
                           uint32_t prim, uint32_t count, uint32_t instances, uint32_t indexAddr, uint32_t indexType,
                           uint32_t baseVertex) {
  const uint32_t dc = r[REGADDR::DB_DEPTH_CONTROL], alpha = r[REGADDR::SX_ALPHA_TEST_CONTROL];
  // depth test and write, no stencil, alpha test on
  if (!depth || (dc & 7) != 6 || !(alpha & 8)) return false;
  if (R.frame != frame) {
    if (++stamp == 0) { for (auto& slot : slots) slot.stamp = 0; stamp = 1; }
    depthGenerations.clear();
    if (preparation_stats_enabled() && frame != ~0ull && R.frame / 120 != frame / 120) {
      LOG("[vulkan pre-pass twins] %.1f draws/frame without their depth write, %.1f with",
          stats[0] / 120.0, stats[1] / 120.0);
      stats[0] = stats[1] = 0;
    }
    frame = R.frame;
  }
  Mesh mesh;
  mesh.words = {prim, count, instances, indexAddr, indexType, baseVertex, alpha, r[REGADDR::SX_ALPHA_REF],
                uint32_t(uintptr_t(depth)), uint32_t(uint64_t(uintptr_t(depth)) >> 32), depth_generation(depth)};
  uint32_t n = 11;
  for (auto& g : fs->bufferGroups) {
    if (n == 16) break;
    mesh.words[n++] = r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7];
  }
  Entry* found = find(mesh);
  if (!found) return false;
  Entry& e = *found;
  const uint32_t func = (dc >> 4) & 7;  // LEQUAL or EQUAL: equal depths pass, as the game relies on
  if (e.target != color && e.writers && (func == 3 || func == 2)) {
    --e.writers;
    ++stats[0];
    return true;
  }
  if (e.target != color) e = {color, 0};
  ++e.writers;
  ++stats[1];
  return false;
}
// a depth clear ends the pre-pass of that depth buffer only (others, like the ambient occlusion
// pass's, are cleared between the game's pre-pass and color pass)
void depth_cleared(const Surface* depth) {
  for (auto& [d, g] : depthGenerations) if (d == depth) { ++g; return; }
  depthGenerations.push_back({depth, 1});
}
} // namespace prepass
void prepass_depth_cleared(const Surface* depth) { prepass::depth_cleared(depth); }

// WWHD_VK_DRAW_TIMING=<frame>: a GPU timestamp after every draw of that renderer frame; a few frames
// later the costliest draws, shaders and targets are logged (the GPU overlaps draws, so a draw's time is
// its completion step after the previous one). Every draw goes to sdmc/drawlist.txt; pixel shaders of the
// top draws translated this session go to sdmc/ps_<key>.glsl (cached ones keep no GLSL: read it from
// spirv.bin). GPU times are in the GPU timer's reported units (see WWHD_VK_SUBMIT_LOG in backend.cpp).
namespace drawtiming {
struct Info { const vk::Shader *vs, *ps; uint32_t count, instances, prim, w, h, cbColor, blend0, depthCtl, alphaTest; const void* target;
  const void* depth = nullptr; uint32_t alphaRef = 0, indexAddr = 0, indexType = 0, baseVertex = 0; std::array<uint32_t, 4> vb{}; };
constexpr uint32_t cap = 8192;
VkQueryPool pool = VK_NULL_HANDLE;
bool armed = false, reported = false;
uint32_t used = 0;
std::vector<Info> infos;
uint64_t target_frame() {
  static const uint64_t t = [] { const char* e = std::getenv("WWHD_VK_DRAW_TIMING"); return e ? strtoull(e, nullptr, 10) : ~0ull; }();
  return t;
}
void command_begin(VkCommandBuffer cmd) {
  if (armed || reported || R.frame != target_frame()) return;
  if (!pool) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = cap;
    if (vkCreateQueryPool(R.device, &info, nullptr, &pool) != VK_SUCCESS) { reported = true; return; }
  }
  vkCmdResetQueryPool(cmd, pool, 0, cap);
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
  armed = true; used = 1; infos.clear();
}
void after_draw(VkCommandBuffer cmd, const uint32_t* r, const vk::Shader* vs, const vk::Shader* ps,
                uint32_t count, uint32_t instances, uint32_t prim, const Surface* target, const Surface* depth,
                const LatteFetchShader* fs, uint32_t indexAddr, uint32_t indexType, uint32_t baseVertex) {
  if (!armed || R.frame != target_frame() || used >= cap) return;
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, used++);
  infos.push_back({vs, ps, count, instances, prim, target ? target->extent.width : 0, target ? target->extent.height : 0,
                   r[REGADDR::CB_COLOR_CONTROL], r[REGADDR::CB_BLEND0_CONTROL], r[REGADDR::DB_DEPTH_CONTROL],
                   r[REGADDR::SX_ALPHA_TEST_CONTROL], target});
  Info& d = infos.back();
  d.depth = depth; d.alphaRef = r[REGADDR::SX_ALPHA_REF]; d.indexAddr = indexAddr; d.indexType = indexType; d.baseVertex = baseVertex;
  size_t n = 0;
  for (auto& g : fs->bufferGroups)
    if (n < d.vb.size()) d.vb[n++] = r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7];
}
void report() {
  if (!armed || reported || R.frame < target_frame() + 4) return;
  reported = true;
  std::vector<uint64_t> ticks(used);
  if (vkGetQueryPoolResults(R.device, pool, 0, used, used * 8, ticks.data(), 8,
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
    LOG("[draw timing] results unavailable");
    return;
  }
  const double period = double(R.properties.limits.timestampPeriod);
  struct Row { uint32_t i; double ms; };
  std::vector<Row> rows;
  std::unordered_map<uint64_t, std::pair<double, uint32_t>> byPs;
  std::map<std::tuple<uint32_t, uint32_t, const void*>, std::pair<double, uint32_t>> byTarget;
  double total = 0;
  for (uint32_t i = 0; i < infos.size(); ++i) {
    const double ms = ticks[i + 1] > ticks[i] ? double(ticks[i + 1] - ticks[i]) * period / 1e6 : 0;
    rows.push_back({i, ms}); total += ms;
    auto& p = byPs[infos[i].ps ? infos[i].ps->key : 0]; p.first += ms; ++p.second;
    auto& t = byTarget[{infos[i].w, infos[i].h, infos[i].target}]; t.first += ms; ++t.second;
  }
  LOG("[draw timing] frame %llu: %zu draws, %.3f ms of draw steps (GPU timer units as reported)",
      (unsigned long long)target_frame(), infos.size(), total);
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
  std::vector<const vk::Shader*> dump;
  for (size_t k = 0; k < std::min<size_t>(rows.size(), 40); ++k) {
    const Info& d = infos[rows[k].i];
    LOG("[draw timing] #%u %.3f ms: vs %016llx ps %016llx count %u x%u prim %u target %ux%u cb %08x blend %08x depth %08x alpha %08x",
        rows[k].i, rows[k].ms, (unsigned long long)(d.vs ? d.vs->key : 0), (unsigned long long)(d.ps ? d.ps->key : 0),
        d.count, d.instances, d.prim, d.w, d.h, d.cbColor, d.blend0, d.depthCtl, d.alphaTest);
    if (d.ps && dump.size() < 12 && std::find(dump.begin(), dump.end(), d.ps) == dump.end()) dump.push_back(d.ps);
  }
  std::vector<std::pair<uint64_t, std::pair<double, uint32_t>>> ps(byPs.begin(), byPs.end());
  std::sort(ps.begin(), ps.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
  for (size_t k = 0; k < std::min<size_t>(ps.size(), 20); ++k)
    LOG("[draw timing] ps %016llx: %.3f ms in %u draws", (unsigned long long)ps[k].first, ps[k].second.first, ps[k].second.second);
  for (auto& [key, v] : byTarget)
    if (v.first > 0.2) LOG("[draw timing] target %ux%u %p: %.3f ms in %u draws", std::get<0>(key), std::get<1>(key), std::get<2>(key), v.first, v.second);
#ifdef __SWITCH__
  if (FILE* f = fopen("sdmc:/switch/wwhd/drawlist.txt", "w")) {
#else
  if (FILE* f = fopen("drawlist.txt", "w")) {
#endif
    for (uint32_t i = 0; i < infos.size(); ++i) {
      const Info& d = infos[i];
      const double ms = ticks[i + 1] > ticks[i] ? double(ticks[i + 1] - ticks[i]) * period / 1e6 : 0;
      fprintf(f, "%u %.4f ps %016llx vs %016llx t %p d %p dc %08x at %08x ref %08x prim %u n %u x%u ib %08x it %u bv %u vb %08x %08x %08x %08x\n",
              i, ms, (unsigned long long)(d.ps ? d.ps->key : 0), (unsigned long long)(d.vs ? d.vs->key : 0),
              d.target, d.depth, d.depthCtl, d.alphaTest, d.alphaRef, d.prim, d.count, d.instances, d.indexAddr, d.indexType,
              d.baseVertex, d.vb[0], d.vb[1], d.vb[2], d.vb[3]);
    }
    fclose(f);
  }
  for (const vk::Shader* sh : dump) {
    if (sh->glsl.empty()) continue;
#ifdef __SWITCH__
    char path[96]; snprintf(path, sizeof path, "sdmc:/switch/wwhd/ps_%016llx.glsl", (unsigned long long)sh->key);
#else
    char path[96]; snprintf(path, sizeof path, "ps_%016llx.glsl", (unsigned long long)sh->key);
#endif
    if (FILE* f = fopen(path, "w")) { fwrite(sh->glsl.data(), 1, sh->glsl.size(), f); fclose(f); }
  }
}
} // namespace drawtiming
void draw_timing_command_begin(VkCommandBuffer cmd) { drawtiming::command_begin(cmd); }

void draw(const uint32_t *r, uint32_t prim, uint32_t count, uint32_t indexType,
          uint32_t indexAddr, uint32_t baseVertex, uint32_t instances) {
  phases::Guard phaseGuard;
  g_vk_draw_calls.fetch_add(1, std::memory_order_relaxed);
  if (!count || !instances || ((prim == 0x13 || prim == 0x14) && count < 4))
    return;
  if (r[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22))
    return;
  ((uint32_t *)r)[REGADDR::VGT_PRIMITIVE_TYPE] = prim;
  vk::poll_shader_compiles(R.frame);
  bphases::start();
  const bool tracked = state_reuse_enabled() && r == gx2::regs();
  uint64_t validation = R.frame;
#ifdef __SWITCH__
  validation = gx2::shaderProgramWrites.generation();
#endif
  auto& shaders = resolvedShaders;
  uint64_t fsKey = shaders.fsKey;
  auto* fs = shaders.fetch;
  bool reuseFetch = false;
#ifdef __SWITCH__
  // Desktop must still read compact fetch headers freshly, as get_fetch_shader does.
  static const bool fetchMemo = draw_option_enabled("WWHD_VK_FETCH_MEMO");
  reuseFetch = tracked && fetchMemo && shaders.regs == r && fs && shaders.validation == validation &&
      shaders.fetchGeneration == gx2::drawStateGenerations.fetch;
#endif
  bphases::mark(7);
  if (!reuseFetch) fs = vk::get_fetch_shader(r, &fsKey, R.frame);
  else if (preparation_stats_enabled()) ++R.cpuPreparation.fetchSkips;
  bphases::mark(8);
  if (!fs)
    throw std::runtime_error("missing Vulkan fetch shader");
  auto *vs = shaders.vs, *ps = shaders.ps;
  const bool reuseShaders = tracked && shaders.regs == r && shaders.validation == validation &&
      shaders.shaderGeneration == g_shader_state_gen && shaders.primitive == (prim & 0x3F) &&
      shaders.fetch == fs && shaders.fsKey == fsKey && vs && ps;
  if (!reuseShaders) {
    const auto pair = vk::translate_pair(r, fs, fsKey, R.frame, g_shader_state_gen, tracked);
    vs = pair.vs; ps = pair.ps;
    ++bphases::acc[11];
  } else if (preparation_stats_enabled()) ++R.cpuPreparation.shaderSkips;
  if (!reuseFetch) ++bphases::acc[12];
  bphases::mark(9);
  const bool pendingShaders = (vs && vs->pending) || (ps && ps->pending);
  if (pendingShaders) shaders = {};  // a postponed translation is never stored: look it up again
  else if (tracked && (!reuseShaders || shaders.fetchGeneration != gx2::drawStateGenerations.fetch))
    shaders = {r,validation,gx2::drawStateGenerations.fetch,g_shader_state_gen,fsKey,prim & 0x3F,fs,vs,ps};
  else if (!tracked) shaders = {};
  bphases::mark(10);
  phases::mark(0);
  if (pendingShaders) {  // SPIR-V compiling or translation postponed (WWHD_VK_ASYNC_SHADERS)
    ++R.asyncSkippedDraws;
    return;
  }
  if (!vs || !vs->ready() || !ps || !ps->ready())
    throw std::runtime_error("Vulkan shader translation failed: " +
                             (vs && !vs->ready() ? vs->error
                              : ps               ? ps->error
                                                 : "missing shader"));
  const bool stripRestart = indexAddr &&
      (prim == 3 || prim == 6) &&
      (r[REGADDR::VGT_MULTI_PRIM_IB_RESET_EN] & 1);
  const uint32_t restartIndex = r[REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX];
  // Native guest index bytes need no widening or temporary vector. Strip
  // pipelines always enable restart for portability, so 16-bit 0xffff is only
  // safe when it is also the guest's enabled marker. Preserve other markers
  // through the existing uint32 normalization path.
  const bool nativeIndices = indexAddr && (indexType == 0 || indexType == 1) &&
      (prim == 1 || prim == 2 || prim == 3 || prim == 4 || prim == 6) &&
      ((prim != 3 && prim != 6) ||
       (indexType == 0 ? stripRestart && restartIndex == UINT16_MAX
                       : !stripRestart || restartIndex == UINT32_MAX));
  // Draw uploads copy the converted bytes before the optional AO replay calls
  // draw again. Retain CPU capacity; queued GPU work owns separate arena slices.
  static thread_local std::vector<uint32_t> indices;
  indices.clear();
  // Conversion emits a known number of indices. Allocate once rather than
  // repeatedly growing and copying the vector for every indexed draw.
  size_t convertedCount = indexAddr ? size_t(count) : 0;
  switch (prim) {
  case 5: convertedCount = count > 2 ? size_t(count - 2) * 3 : 0; break;
  case 0x13: convertedCount = size_t(count / 4) * 6; break;
  case 0x14: convertedCount = count >= 4 ? size_t((count - 2) / 2) * 6 : 0; break;
  case 0x12: convertedCount = size_t(count) + 1; break;
  default: break;
  }
  static const bool specializeIndices = draw_option_enabled("WWHD_VK_SPECIALIZE_INDICES");
  // ld16/ld32 use uint32 guest-EA arithmetic. A wrapped BE range stays on
  // that original path instead of replacing it with linear host addressing.
  const uint64_t guestReads = prim == 0x12 ? std::max(count, 1u) : count;
  const uint64_t guestBytes = guestReads * (indexType == 4 ? 2 : 4);
  const bool guestWrap = indexAddr && (indexType == 4 || indexType == 9) &&
      uint64_t(indexAddr) + guestBytes > 0x100000000ull;
  // Prefix/window bounds are used only by CPU vertex snapshots and diagnostic
  // counters. When every declared binding fits imported MEM2, bind the full
  // ranges and avoid reading indices back solely to compute unused bounds.
  const bool windowStats = vertex_window_stats_enabled();
  bool needVertexExtent = !R.guestMem2Buffer || windowStats;
  if (!needVertexExtent)
    for (const auto& g : fs->bufferGroups) {
      const uint32_t slot = mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7;
      if (!guest_buffer_slice(r[slot], uint32_t(r[slot + 1] + 1), 1).buffer) {
        needVertexExtent = true;
        break;
      }
    }
  // Allocate after texture/pass preparation below, so any intervening submit
  // cannot retire the converted slice before this draw consumes it.
  const bool convertInUpload = specializeIndices && !nativeIndices && !guestWrap && !needVertexExtent;
  // Big-endian guest indices that the native rule above would accept as little-endian: a
  // byte swap at their own width yields exactly the indices (and 16-bit restart marker) of the
  // uint32 expansion, at half the bytes for 16-bit data.
  const bool swappedIndices = convertInUpload && (indexType == 4 || indexType == 9) &&
      (prim == 1 || prim == 2 || prim == 3 || prim == 4 || prim == 6) &&
      ((prim != 3 && prim != 6) ||
       (indexType == 4 ? stripRestart && restartIndex == UINT16_MAX
                       : !stripRestart || restartIndex == UINT32_MAX));
  VkPrimitiveTopology topology;
  if (specializeIndices && !nativeIndices && !guestWrap) {
    switch (prim) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: case 0x12: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: case 5: case 0x13: case 0x14:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: throw std::runtime_error("unsupported Vulkan primitive");
    }
    if (!convertInUpload)
      vk::convert_indices(indexAddr ? mem::ptr(indexAddr) : nullptr, prim, count,
                          indexType, stripRestart, restartIndex, indices);
    else convertedCount = vk::converted_index_count(prim, count, indexAddr != 0);
  } else {
  if (!nativeIndices) indices.reserve(convertedCount);
  auto idx = [&](uint32_t i) {
    if (!indexAddr)
      return i;
    uint32_t value;
    switch (indexType) {
    case 0:
      value = ((uint16_t *)mem::ptr(indexAddr))[i];
      break;
    case 1:
      value = ((uint32_t *)mem::ptr(indexAddr))[i];
      break;
    case 4:
      value = ld16(indexAddr + i * 2);
      break;
    case 9:
      value = ld32(indexAddr + i * 4);
      break;
    default:
      throw std::runtime_error("unsupported index type");
    }
    // All host index buffers use uint32, whose native restart marker differs
    // from the guest's configurable marker (including 16-bit 0xffff).
    return stripRestart && value == restartIndex ? UINT32_MAX : value;
  };
  switch (prim) {
  case 1:
    topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    break;
  case 2:
    topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    break;
  case 3:
    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    break;
  case 4:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    break;
  case 5:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (uint32_t i = 1; i + 1 < count; i++)
      indices.insert(indices.end(), {idx(0), idx(i), idx(i + 1)});
    break;
  case 6:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    break;
  case 0x13:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (uint32_t i = 0; i + 3 < count; i += 4)
      indices.insert(indices.end(), {idx(i), idx(i + 1), idx(i + 2), idx(i),
                                     idx(i + 2), idx(i + 3)});
    break;
  case 0x14:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (uint32_t i = 0; i + 3 < count; i += 2)
      indices.insert(indices.end(), {idx(i), idx(i + 1), idx(i + 2), idx(i + 1),
                                     idx(i + 3), idx(i + 2)});
    break;
  case 0x12:
    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    for (uint32_t i = 0; i < count; i++)
      indices.push_back(idx(i));
    indices.push_back(idx(0));
    break;
  default:
    throw std::runtime_error("unsupported Vulkan primitive");
  }
  if (!nativeIndices && indices.empty() && indexAddr) {
    indices.resize(count);
    for (uint32_t i = 0; i < count; i++)
      indices[i] = idx(i);
  }
  }
  phases::mark(1);
  const auto &lcr = *reinterpret_cast<const LatteContextRegister *>(r);
  std::array<Surface *, 8> colors{};
  uint32_t slices[8]{}, depthSlice = 0;
  Surface* depth=nullptr;
  auto& targets=resolvedTargets;
  if (tracked && targets.regs == r && targets.ps == ps && targets.frame == R.frame &&
      targets.generation == gx2::drawStateGenerations.targets && targets.epoch == R.surfaceEpoch &&
      targets.selection == R.surfaceTargetGeneration) {
    if (preparation_stats_enabled()) ++R.cpuPreparation.targetSkips;
    colors=targets.colors;std::copy(targets.slices.begin(),targets.slices.end(),slices);
    depth=targets.depth;depthSlice=targets.depthSlice;
  } else {
    auto mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
    for (int i = 0; i < 8; i++)
      if (mask & (1 << i)) colors[i] = color_target(r, i, &slices[i]);
    depth = LatteMRT::GetActiveDepthBufferMask(lcr) ? depth_target(r, &depthSlice) : nullptr;
    if (tracked) {
      targets={r,ps,gx2::drawStateGenerations.targets,R.surfaceEpoch,R.surfaceTargetGeneration,R.frame,colors};
      std::copy(std::begin(slices),std::end(slices),targets.slices.begin());
      targets.depth=depth;targets.depthSlice=depthSlice;
    } else targets={};
  }
  const uint32_t guestWidth = colors[0] ? colors[0]->width : 0;
  const uint32_t guestHeight = colors[0] ? colors[0]->height : 0;
  if (aoPrivateReplay && colors[0]) {
    aoPrivateSource = colors[0]->addr;
    colors[0] = private_ao_surface(aoPrivateColor, colors[0]);
    slices[0] = 0;
    if (depth) {
      depth = private_ao_surface(aoPrivateDepth, depth);
      depthSlice = 0;
    }
  }
  Surface *target = depth;
  for (auto *c : colors)
    if (c) {
      target = c;
      break;
    }
  if (!target)
    return;
  uint32_t width = target->extent.width, height = target->extent.height;
  float sx = target->sx, sy = target->sy;
  if (aoPrivateReplay && guestWidth) {
    // the viewport registers describe the game's smaller buffer (x and y differ at other aspect ratios)
    sx = float(colors[0]->extent.width) / guestWidth;
    sy = guestHeight ? float(colors[0]->extent.height) / guestHeight : sx;
  }
  for (auto *&c : colors)
    if (c && (c->extent.width != width || c->extent.height != height)) {
      if (aoPrivateReplay) c = nullptr;
      else throw std::runtime_error("mismatched Vulkan attachments");
    }
  if (depth && (depth->extent.width < width || depth->extent.height < height))
    depth = nullptr;
  struct DepthRestore { uint32_t* reg = nullptr; uint32_t value = 0; ~DepthRestore() { if (reg) *reg = value; } } depthRestore;
  bool redundant = false;
  if (prepass::enabled()) {
    redundant = prepass::redundant_depth_write(r, colors[0], depth, fs, prim, count, instances,
                                               indexAddr, indexType, baseVertex);
    if (redundant) {
      // the register file is restored after this draw; pipeline lookups key on its depth control
      depthRestore.reg = (uint32_t*)r + REGADDR::DB_DEPTH_CONTROL;
      depthRestore.value = *depthRestore.reg;
      *depthRestore.reg &= ~4u;
    }
  }
  phases::mark(2);
  auto& previousPipeline=resolvedPipelines[redundant];
  Pipeline* selectedPipeline=nullptr;
  if (tracked && previousPipeline.value && previousPipeline.regs == r &&
      previousPipeline.generation == g_pipeline_state_gen && previousPipeline.epoch == R.surfaceEpoch &&
      previousPipeline.vs == vs && previousPipeline.ps == ps && previousPipeline.fetch == fs &&
      previousPipeline.topology == topology && previousPipeline.colors == colors && previousPipeline.depth == depth &&
      previousPipeline.depthControl == r[REGADDR::DB_DEPTH_CONTROL]) {
    selectedPipeline=previousPipeline.value;
    if (preparation_stats_enabled()) ++R.cpuPreparation.pipelineSkips;
  } else {
    selectedPipeline=&pipeline(r, vs, ps, fs, topology, colors, depth);
    if (tracked) previousPipeline={r,vs,ps,fs,g_pipeline_state_gen,R.surfaceEpoch,topology,colors,depth,selectedPipeline,
                                   r[REGADDR::DB_DEPTH_CONTROL]};
    else previousPipeline={};
  }
  auto& p=*selectedPipeline;
  if (!p.pipeline)
    return;  // the driver could not build it (logged once in pipeline())
  phases::mark(3);
  if(feedback_stats_enabled()) report_feedback_stats();
  if(vertex_window_stats_enabled()) report_vertex_window_stats();
  FeedbackStatsProbe feedbackProbe;
  auto* probe=feedback_stats_enabled()?&feedbackProbe:nullptr;
  auto vres = bind_stage(r, vs, p.sets[0], p.dynamicUniforms, sx, sy, colors, depth, probe, tracked);
  phases::mark(4);
  auto pres = bind_stage(r, ps, p.sets[1], p.dynamicUniforms, sx, sy, colors, depth, probe, tracked);
  phases::mark(5);
  // Texture uploads, feedback copies, and sampling transitions above may have
  // ended the previous pass. Reuse only its exact active attachment set.
  bool reusePass = R.rendering && R.passTracked && R.passColors == colors &&
                   std::equal(std::begin(slices), std::end(slices),
                              R.passSlices.begin()) &&
                   R.passDepth == depth && R.passDepthSlice == depthSlice &&
                   R.passWidth == width && R.passHeight == height;
  for (auto *color : colors)
    if (color && color->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
      reusePass = false;
  if (depth && depth->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    reusePass = false;
  struct DrawStateCache {
    VkPipeline pipeline = VK_NULL_HANDLE;
    DescriptorBindProbe descriptorProbe;
    struct VertexBind { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceSize offset=0; bool valid=false; };
    std::array<VertexBind,16> vertexBinds{};
    VkCommandBuffer vertexCommand=VK_NULL_HANDLE;
    uint64_t vertexGeneration=0;
    bool vertex_bind_matches(VkCommandBuffer command,uint64_t generation,
                             uint32_t binding,VkBuffer buffer,VkDeviceSize offset) {
      if(vertexCommand!=command || vertexGeneration!=generation) {
        vertexBinds={};vertexCommand=command;vertexGeneration=generation;
      }
      if(binding>=vertexBinds.size()) return false;
      auto& old=vertexBinds[binding];
      const bool match=old.valid && old.buffer==buffer && old.offset==offset;
      old={buffer,offset,true};
      return match;
    }
    VkViewport viewport{};
    VkRect2D scissor{};
    std::array<uint32_t, 4> blend{};
    uint32_t stencilFront = 0, stencilBack = 0;
    bool viewportValid = false, scissorValid = false;
    bool blendValid = false, stencilValid = false;
  };
  static DrawStateCache state;
  // A tracked active pass can only contain our draw commands. Every pass end,
  // including submit/fence/reset, makes reusePass false on the next draw.
  // Reset conservatively on a new pass so external command recording cannot
  // leave this cache claiming dynamic state that was never set in this buffer.
  if (!reusePass) state = {};
  auto cmd = command_buffer();
  if (!reusePass) {
    end_encoder();
    std::array<VkRenderingAttachmentInfo, 8> attachments{};
    uint32_t ncolor = 0;
    for (int i = 0; i < 8; i++) {
      attachments[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      if (colors[i]) {
        upload_surface(colors[i]);
        transition_image(colors[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        attachments[i].imageView = layer_view(colors[i], slices[i]);
        ncolor = i + 1;
      }
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
      upload_surface(depth);
      transition_image(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
      da.imageView = layer_view(depth, depthSlice);
      da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = {width, height};
    ri.layerCount = 1;
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachments = attachments.data();
    ri.pDepthAttachment = depth ? &da : nullptr;
    ri.pStencilAttachment = depth && depth->fmt.stencil ? &da : nullptr;
    if(R.gpuPassTimestampsEnabled) gpu_begin_render_scope(colors,depth,slices,depthSlice,width,height);
    vkCmdBeginRendering(cmd, &ri);
    ++R.renderPassCount;
    R.rendering = true;
    R.passTracked = true;
    R.passColors = colors;
    std::copy(std::begin(slices), std::end(slices), R.passSlices.begin());
    R.passDepth = depth;
    R.passDepthSlice = depthSlice;
    R.passWidth = width;
    R.passHeight = height;
  }
  phases::mark(6);
  if (state.pipeline != p.pipeline) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    state.pipeline = p.pipeline;
  }
  VkDescriptorSet sets[] = {vres.set, pres.set};
  std::array<uint32_t, 34> dynamicOffsets{};
  std::copy_n(vres.dynamicOffsets.begin(), vres.dynamicOffsetCount, dynamicOffsets.begin());
  std::copy_n(pres.dynamicOffsets.begin(), pres.dynamicOffsetCount,
              dynamicOffsets.begin() + vres.dynamicOffsetCount);
  static const bool skipRedundantBinds = draw_option_enabled("WWHD_VK_SKIP_REDUNDANT_BINDS");
  DescriptorBindProbe::Matches matches;
  const bool preparationStats=preparation_stats_enabled();
  if(skipRedundantBinds || preparationStats)
    matches=state.descriptorProbe.observe(cmd,R.submissionGeneration,p.layout,sets,
        dynamicOffsets.data(),vres.dynamicOffsetCount,pres.dynamicOffsetCount);
  const auto bind=DescriptorBindProbe::bind_range(matches,skipRedundantBinds,
      vres.dynamicOffsetCount,pres.dynamicOffsetCount);
  if(preparationStats) {
    ++R.cpuPreparation.descriptorBindCandidates;
    R.cpuPreparation.descriptorWholeMatches+=matches.whole;
    for(size_t stage=0;stage<2;++stage)R.cpuPreparation.descriptorStageMatches[stage]+=matches.stage[stage];
    R.cpuPreparation.descriptorBindCalls+=bind.count!=0;
    R.cpuPreparation.descriptorSkippedSets+=2-bind.count;
  }
  if(bind.count)
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p.layout,
        bind.first,bind.count,sets+bind.first,bind.offsetCount,
        dynamicOffsets.data()+bind.offsetBegin);
  float xs = f32(r[REGADDR::PA_CL_VPORT_XSCALE]),
        xo = f32(r[REGADDR::PA_CL_VPORT_XOFFSET]),
        ys = f32(r[REGADDR::PA_CL_VPORT_YSCALE]),
        yo = f32(r[REGADDR::PA_CL_VPORT_YOFFSET]),
        zs = f32(r[REGADDR::PA_CL_VPORT_ZSCALE]),
        zo = f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]);
  LATTE_PA_CL_CLIP_CNTL clip;
  phases::mark(7);
  std::memcpy(&clip, r + REGADDR::PA_CL_CLIP_CNTL, 4);
  VkViewport vp{(xo - xs) * sx,
                (yo - ys) * sy,
                xs * 2 * sx,
                ys * 2 * sy,
                clip.get_DX_CLIP_SPACE_DEF() ? zo : zo - zs,
                zo + zs};
  // Bitwise comparison preserves distinct signed zeros and exact float state.
  if (!state.viewportValid || std::memcmp(&state.viewport, &vp, sizeof(vp))) {
    vkCmdSetViewport(cmd, 0, 1, &vp);
    state.viewport = vp;
    state.viewportValid = true;
  }
  uint32_t tl = r[REGADDR::PA_SC_GENERIC_SCISSOR_TL],
           br = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
  auto clamp = [](uint32_t v, float k, uint32_t limit) {
    return uint32_t(std::clamp(double(v) * k, 0.0, double(limit)));
  };
  uint32_t x = clamp(tl & 0x7fff, sx, width),
           y = clamp((tl >> 16) & 0x7fff, sy, height),
           ex = clamp(br & 0x7fff, sx, width),
           ey = clamp((br >> 16) & 0x7fff, sy, height);
  if (ex <= x || ey <= y) {
    end_encoder();
    return;
  }
  VkRect2D sc{{int32_t(x), int32_t(y)}, {ex - x, ey - y}};
  if (!state.scissorValid || state.scissor.offset.x != sc.offset.x ||
      state.scissor.offset.y != sc.offset.y ||
      state.scissor.extent.width != sc.extent.width ||
      state.scissor.extent.height != sc.extent.height) {
    vkCmdSetScissor(cmd, 0, 1, &sc);
    state.scissor = sc;
    state.scissorValid = true;
  }
  std::array<uint32_t, 4> blendBits;
  std::copy_n(r + REGADDR::CB_BLEND_RED, blendBits.size(), blendBits.begin());
  if (!state.blendValid || state.blend != blendBits) {
    vkCmdSetBlendConstants(
        cmd, reinterpret_cast<const float *>(r + REGADDR::CB_BLEND_RED));
    state.blend = blendBits;
    state.blendValid = true;
  }
  uint32_t stencilFront = r[REGADDR::DB_STENCILREFMASK] & 255;
  uint32_t stencilBack = r[REGADDR::DB_STENCILREFMASK_BF] & 255;
  if (!state.stencilValid || state.stencilFront != stencilFront)
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencilFront);
  if (!state.stencilValid || state.stencilBack != stencilBack)
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, stencilBack);
  state.stencilFront = stencilFront;
  state.stencilBack = stencilBack;
  state.stencilValid = true;
  // Scan the cached CPU source, avoiding reads back from mapped GPU uploads.
  UploadSlice nativeIndexSlice{};
  UploadSlice convertedIndexSlice{};
  if (swappedIndices && count) {
    const uint8_t* source = mem::ptr(indexAddr);
    if (indexType == 4) {
      convertedIndexSlice = allocate_upload(size_t(count) * 2, 4);
      auto* out = static_cast<uint16_t*>(convertedIndexSlice.mapped);
      uint32_t i = 0;
#if defined(__aarch64__)
      for (; i + 8 <= count; i += 8)
        vst1q_u8(reinterpret_cast<uint8_t*>(out + i), vrev16q_u8(vld1q_u8(source + size_t(i) * 2)));
#endif
      for (; i < count; ++i) {
        uint16_t v; std::memcpy(&v, source + size_t(i) * 2, 2); out[i] = __builtin_bswap16(v);
      }
    } else {
      convertedIndexSlice = allocate_upload(size_t(count) * 4, 4);
      auto* out = static_cast<uint32_t*>(convertedIndexSlice.mapped);
      uint32_t i = 0;
#if defined(__aarch64__)
      for (; i + 4 <= count; i += 4)
        vst1q_u8(reinterpret_cast<uint8_t*>(out + i), vrev32q_u8(vld1q_u8(source + size_t(i) * 4)));
#endif
      for (; i < count; ++i) {
        uint32_t v; std::memcpy(&v, source + size_t(i) * 4, 4); out[i] = __builtin_bswap32(v);
      }
    }
    convertedCount = count;
  } else if (convertInUpload && convertedCount) {
    convertedIndexSlice = allocate_upload(convertedCount * sizeof(uint32_t), 4);
    std::span<uint32_t> output(static_cast<uint32_t*>(convertedIndexSlice.mapped), convertedCount);
    vk::convert_indices(indexAddr ? mem::ptr(indexAddr) : nullptr, prim, count,
                        indexType, stripRestart, restartIndex, output);
    if (convertedIndexSlice.size > output.size_bytes())
      memset(static_cast<uint8_t*>(convertedIndexSlice.mapped) + output.size_bytes(), 0,
             convertedIndexSlice.size - output.size_bytes());
  }
  const void* nativeIndexSource = nativeIndices ? mem::ptr(indexAddr) : nullptr;
  const bool hostRestart = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                           topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VertexExtent vertexExtent;
  phases::mark(8);
  if (nativeIndices) {
    const uint32_t indexBytes = indexType == 0 ? 2 : 4;
    nativeIndexSlice = guest_buffer_slice(indexAddr, uint64_t(count) * indexBytes, indexBytes);
    if (!nativeIndexSlice.buffer)
      nativeIndexSlice = snapshot(nativeIndexSource, size_t(count) * indexBytes, 4);
    if (needVertexExtent) vertexExtent = indexed_vertex_extent(nativeIndexSource, count,
                                         indexBytes, hostRestart,
                                         int32_t(baseVertex));
  } else if (needVertexExtent && !indices.empty()) {
    vertexExtent = indexed_vertex_extent(indices.data(), indices.size(), 4,
                                         hostRestart, int32_t(baseVertex));
  } else if (needVertexExtent) {
    const uint64_t maximum = uint64_t(baseVertex) + count - 1;
    vertexExtent = {maximum <= UINT32_MAX, uint32_t(maximum)};
  }
  VertexWindowExtent windowExtent;
  if(windowStats && vertex_copy_window_enabled()) {
    windowExtent={vertexExtent.valid,vertexExtent.minimum,vertexExtent.maximum};
  } else if(windowStats) {
    if(nativeIndices) {
      windowExtent=indexType==0
        ? vertex_window_extent<uint16_t>(nativeIndexSource,count,hostRestart,int32_t(baseVertex))
        : vertex_window_extent<uint32_t>(nativeIndexSource,count,hostRestart,int32_t(baseVertex));
    } else if(!indices.empty()) {
      windowExtent=vertex_window_extent<uint32_t>(indices.data(),indices.size(),hostRestart,int32_t(baseVertex));
    } else if(count && uint64_t(baseVertex)+count-1<=UINT32_MAX) {
      windowExtent={true,baseVertex,uint32_t(uint64_t(baseVertex)+count-1)};
    }
  }
  const bool cachedTrims = p.trimFetch == fs && p.trimVertex == vs &&
                           p.bindingTrims.size() == fs->bufferGroups.size();
  size_t trimGroup = 0;
  for (auto &g : fs->bufferGroups) {
    uint32_t addr =
        r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7];
    uint32_t size =
        r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 1] + 1;
    if (!addr || !size)
      throw std::runtime_error("missing vertex buffer");
    const BindingTrimMetadata trim = cachedTrims
        ? p.bindingTrims[trimGroup] : binding_trim_metadata(r, vs, g);
    ++trimGroup;
    const auto attributeEnd = trim.attributeEnd;
    const auto stride = trim.stride;
    const auto supported = trim.supported;
    const auto rate = trim.rate;
    VertexExtent extent = vertexExtent;
    if (rate && *rate == LatteConst::VertexFetchType2::INSTANCE_DATA)
      extent = {true, instances - 1};
    const uint32_t copied = supported
        ? vertex_prefix_size(size, stride, attributeEnd, extent) : size;
    if(windowStats) {
      auto window=windowExtent;
      if(rate && *rate==LatteConst::VertexFetchType2::INSTANCE_DATA)
        window={instances!=0,0,instances-1};
      const bool eligible=supported && attributeEnd && stride && window.valid &&
          uint64_t(window.maximum)*stride+attributeEnd<=copied;
      const uint32_t unused=vertex_window_unused(copied,stride,attributeEnd,window,supported);
      auto& t=vertexWindowStats;
      ++t.bindings;t.eligible+=eligible;t.fallback+=!eligible;t.zeroStride+=stride==0;
      t.prefix+=copied;t.window+=copied-unused;
    }
    R.vertexDeclaredBytes += size;
    R.vertexCopiedBytes += copied;
    const uint64_t windowBegin=uint64_t(extent.minimum)*stride;
    const uint64_t windowEnd=uint64_t(extent.maximum)*stride+attributeEnd;
    const bool copyWindow=vertex_copy_window_enabled() &&
        (nativeIndices || !indices.empty()) && supported && rate &&
        attributeEnd && stride && extent.valid && vertexExtent.valid &&
        windowEnd<=copied && windowBegin<windowEnd && windowBegin!=0;
    UploadSlice direct = guest_buffer_slice(addr, size, 1);
    {  // debug: WWHD_VK_COPY_REGIONS=1 logs which guest regions vertex data is still copied from
      static const bool regions = std::getenv("WWHD_VK_COPY_REGIONS") != nullptr;
      static uint64_t bytes[16] = {}, frame = 0;
      if (regions && !direct.buffer) bytes[addr >> 28] += size;
      if (regions && R.frame != frame && (R.frame % 120) == 0) {
        frame = R.frame;
        LOG("[vulkan] vertex bytes copied per 120 frames by region (addr>>28): 1:%llu 2:%llu 3:%llu 4:%llu 6:%llu E:%llu F:%llu 0:%llu",
            (unsigned long long)bytes[1], (unsigned long long)bytes[2], (unsigned long long)bytes[3], (unsigned long long)bytes[4],
            (unsigned long long)bytes[6], (unsigned long long)bytes[14], (unsigned long long)bytes[15], (unsigned long long)bytes[0]);
        for (auto& b : bytes) b = 0;
      }
    }
    auto b = direct.buffer ? direct : copyWindow
        ? vertex_window_snapshot(g.attributeBufferIndex,addr,copied,
                                 uint32_t(windowBegin),uint32_t(windowEnd-windowBegin))
        : vertex_snapshot(g.attributeBufferIndex, addr, copied,
                          supported && attributeEnd && extent.valid && vertexExtent.valid);
    VkDeviceSize offset = b.offset;
    static const bool skipVertexBinds = [] {
      const char* e=std::getenv("WWHD_VK_SKIP_VERTEX_BINDS");
      return e && !std::strcmp(e,"1");
    }();
    // Snapshot preparation above stays fresh even when its immutable slice is
    // unchanged. Compare host binding identity, never guest addresses alone.
    const bool skip=skipVertexBinds && state.vertex_bind_matches(cmd,
        R.submissionGeneration,g.attributeBufferIndex,b.buffer,offset);
    if(preparation_stats_enabled()) {
      R.vertexBindCalls+=!skip;
      R.vertexBindSkips+=skip;
    }
    if(!skip)
      vkCmdBindVertexBuffers(cmd,g.attributeBufferIndex,1,&b.buffer,&offset);
  }
  phases::mark(9);
  if (nativeIndices) {
    vkCmdBindIndexBuffer(cmd, nativeIndexSlice.buffer, nativeIndexSlice.offset,
                         indexType == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, count, instances, 0, int32_t(baseVertex), 0);
  } else if (convertedIndexSlice.buffer) {
    vkCmdBindIndexBuffer(cmd, convertedIndexSlice.buffer, convertedIndexSlice.offset,
                         swappedIndices && indexType == 4 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, convertedCount, instances, 0, int32_t(baseVertex), 0);
  } else if (indices.empty())
    vkCmdDraw(cmd, count, instances, baseVertex, 0);
  else {
    auto b = snapshot(indices.data(), indices.size() * 4, 4);
    vkCmdBindIndexBuffer(cmd, b.buffer, b.offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, indices.size(), instances, 0, int32_t(baseVertex), 0);
  }
  if(R.gpuPassTimestampsEnabled) gpu_count_render_draw();
  if (drawtiming::target_frame() != ~0ull) {
    drawtiming::after_draw(cmd, r, vs, ps, count, instances, prim, colors[0] ? colors[0] : depth, depth, fs,
                           indexAddr, indexType, baseVertex);
    drawtiming::report();
  }
  for (auto *color : colors)
    if (color) mark_gpu_written(color);
  if (depth) mark_gpu_written(depth);
  R.drawCount++;
  if (preparation_stats_enabled()) {
    // WWHD_VK_STATS: draws per frame by render target (first color target, else depth) guest size
    static std::unordered_map<uint64_t, uint64_t> byTarget;
    static uint64_t frame = 0;
    const Surface* t = colors[0] ? colors[0] : depth;
    uint64_t k = t ? (uint64_t(t->width) << 32 | uint64_t(t->height) << 1 | (colors[0] ? 0 : 1)) : 0;
    ++byTarget[k];
    if (R.frame != frame && R.frame % 120 == 0) {
      frame = R.frame;
      std::vector<std::pair<uint64_t, uint64_t>> v(byTarget.begin(), byTarget.end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::string line;
      for (size_t i = 0; i < v.size() && i < 12; ++i) {
        char b[64];
        snprintf(b, sizeof b, " %ux%u%s:%.0f", unsigned(v[i].first >> 32), unsigned(v[i].first >> 1 & 0x7FFFFFFF),
                 v[i].first & 1 ? "d" : "", v[i].second / 120.0);
        line += b;
      }
      LOG("[vulkan draws by target]%s", line.c_str());
      byTarget.clear();
    }
  }
  if (aoPrivateReplay) {
    aoPrivateFrame = R.frame;
    return;
  }
  if (ao_hires_enabled() && colors[0] &&
      (r[mmSQ_PGM_START_PS] << 8) == kDepthDownsamplePS) {
    struct ReplayGuard {
      ReplayGuard() { aoPrivateReplay = true; aoPrivateFrame = ~0ull; }
      ~ReplayGuard() { aoPrivateReplay = false; }
    } guard;
    draw(r, prim, count, indexType, indexAddr, baseVertex, instances);
  }
  static const uint32_t drawBatch = parse_draw_batch(std::getenv("WWHD_VK_DRAW_BATCH"));
  static const uint32_t drawBatchCap =
      parse_draw_batch_cap(std::getenv("WWHD_VK_DRAW_BATCH_CAP"));
  if (drawBatchState.after_draw(R.frame, drawBatch, drawBatchCap)) {
    // Submit only after this draw owns all its upload slices and deferred
    // resources. The next draw reopens attachments with LOAD and rebinds state.
    flush_async();
    ++drawBatchSubmissions;
  }
}
} // namespace gfxvk
