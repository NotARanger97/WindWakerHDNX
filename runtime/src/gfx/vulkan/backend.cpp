#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#if defined(__APPLE__) && !defined(WWHD_SDL_HOST)
#define VK_USE_PLATFORM_METAL_EXT  // VK_EXT_metal_surface: AppKit views' CAMetalLayers
#endif
#include "backend.h"
#include "present.h"
#include "gfx/display.h"
#include "gfx/display_modes.h"
#include "gx2/gx2.h"
#include "input.h"
#include "mods/mods.h"
#include "overlay/hostui.h"
#include "overlay/overlay.h"
#ifdef WWHD_SDL_HOST
#include "platform/input_sdl.h"
#include <SDL3/SDL_vulkan.h>
#endif
#include "platform/host.h"
#include "platform/perf_hint.h"
#ifdef __SWITCH__
#include "platform/switch/vk_record_thread.h"
#endif
#include "runtime.h"
#include "frame_trace.h"
#include "gx2/shader_program_writes.h"
#include "shaders.h"
#include "settings.h"
#include "sparse_hash_memo.h"
#include "write_watch.h"
#include <algorithm>
#include <functional>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <time.h>
#endif
#include <vulkan/vulkan_beta.h>

namespace gx2 { uint64_t flips_presented(); void checkpoint_vulkan_caches(); }
namespace interp { int mode(); }

#ifdef __SWITCH__
extern "C" uint64_t switch_thread_cpu_ns(int which);  // platform/switch/switch_host.cpp
#endif
extern "C" std::atomic<uint64_t> g_vk_draw_calls;  // draw.cpp: draws received from the game
namespace gfxvk {
extern uint64_t g_stat_full_checks, g_stat_uploads, g_upload_ns, g_full_check_ns;  // surfaces.cpp
Renderer R;
namespace {
bool perf_enabled() {
  static const bool enabled = std::getenv("WWHD_VK_STATS") != nullptr;
  return enabled;
}
bool cpu_only_stats_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWHD_VK_CPU_ONLY_STATS");
    return value && !std::strcmp(value, "1");
  }();
  return enabled;
}
bool asynchronous_submissions() {
#ifdef __SWITCH__
  return true;
#else
  // Desktop opt-in exercises the Switch submission/presentation path with
  // Vulkan validation, without changing normal desktop pacing.
  static const bool enabled = [] {
    const char* value = std::getenv("WWHD_VK_ASYNC");
    return value && !std::strcmp(value, "1");
  }();
  return enabled;
#endif
}
struct WaitTiming { uint64_t count = 0, ns = 0; };
struct PresentTiming { WaitTiming acquire, present, idle; };
WaitTiming submitWait, presentSubmitWait;
PresentTiming screenTiming[2];
constexpr size_t maxPipelineCacheBytes = 64u * 1024u * 1024u;
std::string pipelineCachePath;
uint64_t pipelineCacheAttemptFrame = 0;
bool compatible_pipeline_cache(const std::vector<uint8_t>& bytes) {
  // Vulkan cache header fields are stored little-endian, independent of host.
  if (bytes.size() < 16 + VK_UUID_SIZE) return false;
  auto word = [&](size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset+1]) << 8 |
           uint32_t(bytes[offset+2]) << 16 | uint32_t(bytes[offset+3]) << 24;
  };
  uint32_t headerSize=word(0);
  return headerSize >= 16+VK_UUID_SIZE && headerSize <= bytes.size() &&
         word(4)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
         word(8)==R.properties.vendorID && word(12)==R.properties.deviceID &&
         !std::memcmp(bytes.data()+16,R.properties.pipelineCacheUUID,VK_UUID_SIZE);
}
constexpr std::array<uint8_t,8> pipelineCacheMagic{'W','W','V','K','P','C','0','1'};
constexpr size_t pipelineCacheWrapperSize=24;
uint64_t pipeline_cache_checksum(const uint8_t* bytes,size_t size) {
  uint64_t hash=14695981039346656037ull;
  for (size_t i=0;i<size;++i) hash=(hash^bytes[i])*1099511628211ull;
  return hash;
}
std::array<uint8_t,pipelineCacheWrapperSize> pipeline_cache_wrapper(const std::vector<uint8_t>& bytes) {
  std::array<uint8_t,pipelineCacheWrapperSize> header{};
  std::copy(pipelineCacheMagic.begin(),pipelineCacheMagic.end(),header.begin());
  uint64_t size=bytes.size(),checksum=pipeline_cache_checksum(bytes.data(),bytes.size());
  for (unsigned i=0;i<8;++i) {
    header[8+i]=uint8_t(size>>(i*8));header[16+i]=uint8_t(checksum>>(i*8));
  }
  return header;
}
bool unpack_pipeline_cache(std::vector<uint8_t>& file) {
  if (file.size()<pipelineCacheWrapperSize ||
      !std::equal(pipelineCacheMagic.begin(),pipelineCacheMagic.end(),file.begin())) return false;
  auto word64=[&](size_t offset) {
    uint64_t value=0;
    for (unsigned i=0;i<8;++i) value|=uint64_t(file[offset+i])<<(i*8);
    return value;
  };
  uint64_t size=word64(8);
  if (size<16+VK_UUID_SIZE || size>maxPipelineCacheBytes ||
      size!=file.size()-pipelineCacheWrapperSize) return false;
  if (word64(16)!=pipeline_cache_checksum(file.data()+pipelineCacheWrapperSize,size)) return false;
  file.erase(file.begin(),file.begin()+pipelineCacheWrapperSize);
  return compatible_pipeline_cache(file);
}
void init_pipeline_cache() try {
  if (const char* explicitPath=std::getenv("WWHD_VK_PIPELINE_CACHE")) {
    if (std::strcmp(explicitPath,"0")) pipelineCachePath=explicitPath;
  } else if (const char* shaderPath=std::getenv("WWHD_SHADER_CACHE");
             !shaderPath || std::strcmp(shaderPath,"0")) {
    char ids[32];
    std::snprintf(ids,sizeof ids,"vulkan-%08x-%08x-",R.properties.vendorID,R.properties.deviceID);
    // a WWHD_SHADER_CACHE file (test runs, separate setups) keeps the pipeline cache next to it,
    // so such runs never write the user's own cache in the config folder
    pipelineCachePath=shaderPath ? std::string(shaderPath)+"."+ids
                                 : host::config_dir()+"/shadercache/"+ids;
    for (uint8_t byte : R.properties.pipelineCacheUUID) {
      char hex[3]; std::snprintf(hex,sizeof hex,"%02x",byte); pipelineCachePath+=hex;
    }
    pipelineCachePath+=".bin";
  }
  std::vector<uint8_t> bytes;
  if (!pipelineCachePath.empty()) {
    std::error_code ec;
    uintmax_t size=std::filesystem::file_size(pipelineCachePath,ec);
    if (!ec && size>=pipelineCacheWrapperSize+16+VK_UUID_SIZE &&
        size<=maxPipelineCacheBytes+pipelineCacheWrapperSize) {
      bytes.resize(size);
      FILE* file=std::fopen(pipelineCachePath.c_str(),"rb");
      if (file) {
        if (std::fread(bytes.data(),1,bytes.size(),file)!=bytes.size() ||
            std::fgetc(file)!=EOF || std::ferror(file)) bytes.clear();
        std::fclose(file);
        if (!unpack_pipeline_cache(bytes)) bytes.clear();
      } else bytes.clear();
    }
  }
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize=bytes.size();ci.pInitialData=bytes.empty()?nullptr:bytes.data();
  VkResult result=vkCreatePipelineCache(R.device,&ci,nullptr,&R.pipelineCache);
  if (result!=VK_SUCCESS && !bytes.empty()) {
    ci.initialDataSize=0;ci.pInitialData=nullptr;
    result=vkCreatePipelineCache(R.device,&ci,nullptr,&R.pipelineCache);
    bytes.clear();
  }
  if (result!=VK_SUCCESS) {
    R.pipelineCache=VK_NULL_HANDLE;
    LOG("[vulkan cache] creation failed (%d); continuing without cache",result);
  } else if (perf_enabled()) {
    LOG("[vulkan cache] loaded %zu bytes; persistence %s",bytes.size(),
        pipelineCachePath.empty()?"disabled":pipelineCachePath.c_str());
  }
} catch (const std::exception& error) {
  pipelineCachePath.clear();
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  if (vkCreatePipelineCache(R.device,&ci,nullptr,&R.pipelineCache)!=VK_SUCCESS)
    R.pipelineCache=VK_NULL_HANDLE;
  LOG("[vulkan cache] load disabled after error: %s",error.what());
}
template<class F> VkResult timed_call(WaitTiming& timing, F&& call) {
  if (!perf_enabled()) return call();
  auto start = std::chrono::steady_clock::now();
  VkResult result = call();
  ++timing.count;
  timing.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count();
  return result;
}
} // namespace
namespace {
struct PipelineCacheWriteResult {
  bool written = false;
  uint64_t revision = 0, changedFrame = 0;
  size_t size = 0;
  double elapsedMs = 0, checksumMs = 0;
  std::string error;
};
std::future<PipelineCacheWriteResult> pipelineCacheWrite;
// File work owns immutable bytes and a copied path. It never accesses Vulkan
// objects, renderer state, or render-thread logging.
PipelineCacheWriteResult write_pipeline_cache(std::vector<uint8_t> bytes,
                                              std::string path,
                                              uint64_t revision,
                                              uint64_t changedFrame) {
  PipelineCacheWriteResult result;
  result.revision=revision;result.changedFrame=changedFrame;result.size=bytes.size();
  auto start=std::chrono::steady_clock::now();
  std::string temporary;
  try {
    auto wrapper=pipeline_cache_wrapper(bytes);
    result.checksumMs=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    std::error_code ec;
    auto parent=std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent,ec);
    if (ec) throw std::runtime_error(ec.message());
    temporary=path+".tmp-"+std::to_string(start.time_since_epoch().count());
    FILE* file=std::fopen(temporary.c_str(),"wb");
    if (file) {
      bool written=std::fwrite(wrapper.data(),1,wrapper.size(),file)==wrapper.size();
      if (written) written=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
      if (std::fclose(file)) written=false;
      if (written) result.written=host::replace_file(temporary,path);
    }
    if (!result.written) {
      std::filesystem::remove(temporary,ec);
      result.error="file write or replacement failed";
    }
  } catch (const std::exception& error) {
    result.error=error.what();
    if (!temporary.empty()) {
      std::error_code ec;std::filesystem::remove(temporary,ec);
    }
  }
  result.elapsedMs=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-start).count();
  return result;
}
void poll_pipeline_cache_write(bool wait) {
  if (!pipelineCacheWrite.valid()) return;
  if (!wait && pipelineCacheWrite.wait_for(std::chrono::seconds(0)) !=
                   std::future_status::ready) return;
  auto result=pipelineCacheWrite.get();
  // A newer pipeline, even in the same guest frame, must remain dirty.
  if (result.written && result.revision==R.pipelineCreates &&
      result.changedFrame==R.pipelineCacheChangedFrame)
    R.pipelineCacheDirty=false;
  if (result.written && perf_enabled())
    LOG("[vulkan cache] persisted %zu payload bytes in %.2f ms (checksum %.2f ms)",
        result.size,result.elapsedMs,result.checksumMs);
  else if (!result.written)
    LOG("[vulkan cache] save skipped: %s",result.error.c_str());
}
void start_pipeline_cache_write() {
  if (pipelineCacheWrite.valid() || !R.pipelineCache ||
      !R.pipelineCacheDirty || pipelineCachePath.empty()) return;
  // This is the only Vulkan cache read, serialized with pipeline creation on
  // the render thread. Worker serialization/checksum never holds that thread.
  size_t size=0;
  if (vkGetPipelineCacheData(R.device,R.pipelineCache,&size,nullptr)!=VK_SUCCESS ||
      size<16+VK_UUID_SIZE || size>maxPipelineCacheBytes) return;
  std::vector<uint8_t> bytes(size);
  if (vkGetPipelineCacheData(R.device,R.pipelineCache,&size,bytes.data())!=VK_SUCCESS) return;
  bytes.resize(size);
  if (!compatible_pipeline_cache(bytes)) return;
  pipelineCacheWrite=std::async(std::launch::async,
      [bytes=std::move(bytes),path=pipelineCachePath,creates=R.pipelineCreates,
       changed=R.pipelineCacheChangedFrame]() mutable {
        host::background_thread();  // only time the game and render threads leave idle
        return write_pipeline_cache(std::move(bytes),std::move(path),creates,changed);
      });
}
void checkpoint_pipeline_cache() try {
  poll_pipeline_cache_write(false);
  if (R.pipelineCacheDirty && R.frame-R.pipelineCacheChangedFrame>=120 &&
      R.frame-pipelineCacheAttemptFrame>=120 && !pipelineCacheWrite.valid()) {
    pipelineCacheAttemptFrame=R.frame;
    start_pipeline_cache_write();
  }
} catch (const std::exception& error) {
  LOG("[vulkan cache] checkpoint skipped after error: %s",error.what());
}
} // namespace
void save_pipeline_cache() try {
  // Explicit shutdown/checkpoint waits before _Exit, then persists any newer
  // revision created while the periodic job was running.
  poll_pipeline_cache_write(true);
  start_pipeline_cache_write();
  poll_pipeline_cache_write(true);
} catch (const std::exception& error) {
  LOG("[vulkan cache] save skipped after error: %s",error.what());
}
void vk_check(VkResult r, const char *op) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(op) + ": Vulkan result " +
                             std::to_string(r));
}
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties p;
  vkGetPhysicalDeviceMemoryProperties(R.physicalDevice, &p);
  for (uint32_t i = 0; i < p.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  throw std::runtime_error("No compatible Vulkan memory type");
}
static Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags flags, VkMemoryPropertyFlags preferred) {
  Buffer b;
  b.size = std::max<VkDeviceSize>(size, 16);
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = b.size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vk_check(vkCreateBuffer(R.device, &ci, nullptr, &b.buffer), "create buffer");
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(R.device, b.buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  VkPhysicalDeviceMemoryProperties properties;
  vkGetPhysicalDeviceMemoryProperties(R.physicalDevice, &properties);
  ai.memoryTypeIndex = UINT32_MAX;
  if (preferred)
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & preferred) == preferred) {
        ai.memoryTypeIndex = i;
        break;
      }
  if (ai.memoryTypeIndex == UINT32_MAX)
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, flags);
  b.allocationSize = req.size;
  b.properties = properties.memoryTypes[ai.memoryTypeIndex].propertyFlags;
  vk_check(vkAllocateMemory(R.device, &ai, nullptr, &b.memory),
           "allocate buffer memory");
  vk_check(vkBindBufferMemory(R.device, b.buffer, b.memory, 0),
           "bind buffer memory");
  if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    vk_check(vkMapMemory(R.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped),
             "map buffer");
  return b;
}
Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags flags) {
  return create_buffer(size, usage, flags, 0);
}
UploadSlice allocate_upload(VkDeviceSize size, VkDeviceSize alignment) {
  size = std::max<VkDeviceSize>(size,16);
  alignment = std::max<VkDeviceSize>(alignment,4);
  auto slice = [&](Renderer::UploadBlock& block) {
    const VkDeviceSize end = block.capacity ? block.base + block.capacity : block.buffer.size;
    VkDeviceSize offset = ((block.base + block.used + alignment - 1) / alignment) * alignment;
    if (offset > end || size > end - offset)
      return UploadSlice{};
    block.used = offset + size - block.base;
    // Reserve the entire writable slice as dirty, covering snapshot padding,
    // partial vertex windows and direct mapped writes without per-writer hooks.
    if (!(block.buffer.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      block.dirtyBegin = std::min(block.dirtyBegin, offset);
      block.dirtyEnd = std::max(block.dirtyEnd, offset + size);
    }
    R.uploadBytes += size;
    UploadSlice result{block.buffer.buffer,offset,size,
                       static_cast<uint8_t*>(block.buffer.mapped)+offset};
#ifdef __SWITCH__
    // Coherent fallback memory on Tegra is uncached: skip mapped reuse reads.
    result.cpuReadable = (block.buffer.properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
#endif
    return result;
  };
  for (auto& block : R.uploadBlocks) {
    auto result = slice(block);
    if (result.buffer) return result;
  }
  VkMemoryPropertyFlags preferred = 0;
#ifdef __SWITCH__
  preferred = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  // Each submission slot's first block is its own 32 MiB range of one buffer shared by all slots.
  // Descriptor sets name the uniform buffer, so with a buffer per slot every draw needed one set per
  // slot (draws rotate through the slots: ~3 submissions a frame, 4 slots) and each newly visible
  // material combination created four (600-800 new sets a frame while walking).
  constexpr VkDeviceSize kSlotRange = 32ull << 20;
  static Buffer shared{};
  if (R.uploadBlocks.empty() && size <= kSlotRange) {
    if (!shared.buffer)
      shared = create_buffer(kSlotRange * R.submissions.size(),
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, preferred);
    R.uploadBlocks.push_back({shared, 0, VK_WHOLE_SIZE, 0, kSlotRange * R.activeSubmission, kSlotRange});
    ++R.uploadAllocations;
    if (auto result = slice(R.uploadBlocks.back()); result.buffer) return result;
  }
#endif
  auto buffer = create_buffer(std::max<VkDeviceSize>(32ull<<20,size),
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, preferred);
  R.uploadBlocks.push_back({buffer,0});
  ++R.uploadAllocations;
  return slice(R.uploadBlocks.back());
}
static void flush_uploads() {
  // Retain range capacity across submissions; one Vulkan call flushes all
  // dirty blocks, with one coalesced interval per block.
  static std::vector<VkMappedMemoryRange> ranges;
  ranges.clear();
  const VkDeviceSize atom = R.properties.limits.nonCoherentAtomSize;
  for (auto& block : R.uploadBlocks) {
    if (block.dirtyBegin == VK_WHOLE_SIZE) continue;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = block.buffer.memory;
    range.offset = (block.dirtyBegin / atom) * atom;
    const VkDeviceSize end = std::min(block.buffer.allocationSize,
        ((block.dirtyEnd + atom - 1) / atom) * atom);
    // The whole allocation is mapped. A final partial atom is legal only at
    // the allocation end, which can extend past the buffer's logical size.
    range.size = end - range.offset;
    ranges.push_back(range);
  }
  if (ranges.empty()) return;
  vk_check(vkFlushMappedMemoryRanges(R.device, uint32_t(ranges.size()), ranges.data()),
           "flush upload memory");
  for (auto& block : R.uploadBlocks) {
    block.dirtyBegin = VK_WHOLE_SIZE;
    block.dirtyEnd = 0;
  }
}
void defer_buffer(Buffer b) { R.garbageBuffers.push_back(b); }
void defer_descriptor_pool(VkDescriptorPool pool) { if (pool) R.garbagePools.push_back(pool); }
#ifdef __SWITCH__
extern "C" bool switch_mem2(uint8_t** host);  // platform/switch/switch_host.cpp
// Import guest MEM2 for the GPU (WWHD_ZERO_COPY=0 keeps copying vertex/index data). The memory
// stays CPU-cached; the game's DCFlushRange calls clean the CPU cache for the GPU, as on the Wii U.
static void import_guest_mem2() {
  if (const char* e = std::getenv("WWHD_ZERO_COPY"); e && !std::strcmp(e, "0")) return;
  uint8_t* host = nullptr;
  if (!switch_mem2(&host) || !host) { LOG("[vulkan] MEM2 is not GPU-mappable: vertex data is copied"); return; }
  constexpr VkDeviceSize kSize = 0x40000000;
  auto getProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(R.device, "vkGetMemoryHostPointerPropertiesEXT");
  VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  if (!getProps || getProps(R.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp) != VK_SUCCESS) {
    LOG("[vulkan] MEM2 import: no host pointer properties"); return;
  }
  VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.pNext = &ext;
  ci.size = kSize;
  ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateBuffer(R.device, &ci, nullptr, &R.guestMem2Buffer) != VK_SUCCESS) { LOG("[vulkan] MEM2 import: buffer"); return; }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(R.device, R.guestMem2Buffer, &req);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(R.physicalDevice, &mp);
  uint32_t type = UINT32_MAX, bits = req.memoryTypeBits & hp.memoryTypeBits;
  for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) type = i;
  VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  imp.pHostPointer = host;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.pNext = &imp;
  ai.allocationSize = kSize;
  ai.memoryTypeIndex = type;
  if (type == UINT32_MAX || req.size > kSize ||
      vkAllocateMemory(R.device, &ai, nullptr, &R.guestMem2Memory) != VK_SUCCESS ||
      vkBindBufferMemory(R.device, R.guestMem2Buffer, R.guestMem2Memory, 0) != VK_SUCCESS) {
    LOG("[vulkan] MEM2 import failed (type %u, size %llu): vertex data is copied", type, (unsigned long long)req.size);
    if (R.guestMem2Memory) vkFreeMemory(R.device, R.guestMem2Memory, nullptr);
    vkDestroyBuffer(R.device, R.guestMem2Buffer, nullptr);
    R.guestMem2Buffer = VK_NULL_HANDLE; R.guestMem2Memory = VK_NULL_HANDLE;
    return;
  }
  LOG("[vulkan] guest MEM2 imported for the GPU (memory type %u): vertex/index data is read in place", type);
}
#endif
void defer_surface_image(VkImage image, VkDeviceMemory memory,
                         std::vector<VkImageView> views, SurfaceImageKey key,
                         VkDeviceSize allocationBytes) {
  R.garbageImages.push_back({image, memory, std::move(views), key, allocationBytes});
}
void forget_texture_views() {
} // descriptor sets are rebuilt for every draw and retired on submit
namespace {
struct GpuScopeBucket {
  GpuScopeMetadata metadata{};
  double intervalNs=0,maxNs=0;
  uint64_t count=0,draws=0;
};
std::array<GpuScopeBucket,256> gpuScopeBuckets{};
uint32_t gpuScopeBucketCount=0;
struct GpuScopeTotals { uint64_t available=0,unavailable=0,zero=0,dropped=0,bucketDrops=0; } gpuScopeTotals;
bool same_gpu_scope(const GpuScopeMetadata& a,const GpuScopeMetadata& b) {
  return a.kind==b.kind && a.colors==b.colors && a.depth==b.depth &&
      a.extent.width==b.extent.width && a.extent.height==b.extent.height && a.extent.depth==b.extent.depth &&
      a.mips==b.mips && a.layers==b.layers && a.aspects==b.aspects && a.colorCount==b.colorCount &&
      a.slices==b.slices && a.depthSlice==b.depthSlice && a.depthOnly==b.depthOnly;
}
void add_gpu_scope(const Renderer::Submission::GpuScope& scope,double ns) {
  ++gpuScopeTotals.available;
  gpuScopeTotals.zero += ns==0;
  uint32_t i=0;
  while(i<gpuScopeBucketCount && !same_gpu_scope(gpuScopeBuckets[i].metadata,scope.metadata)) ++i;
  if(i==gpuScopeBucketCount) {
    if(i==gpuScopeBuckets.size()) { ++gpuScopeTotals.bucketDrops; return; }
    gpuScopeBuckets[i]={};gpuScopeBuckets[i].metadata=scope.metadata;++gpuScopeBucketCount;
  }
  auto& bucket=gpuScopeBuckets[i];
  bucket.intervalNs+=ns;bucket.maxNs=std::max(bucket.maxNs,ns);
  ++bucket.count;bucket.draws+=scope.draws;
}
} // namespace
static bool timestamp_capable(uint32_t bits, double period) {
  return bits > 0 && bits <= 64 && period > 0 && std::isfinite(period);
}
static uint64_t timestamp_elapsed_ticks(uint64_t start, uint64_t end, uint32_t bits) {
  const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
  return (end - start) & mask;
}
static void destroy_gpu_timestamp_queries() {
  // Caller has already drained submissions; never destroy in-flight queries.
  R.gpuTimestampsEnabled = false;
  R.gpuPassTimestampsEnabled = false;
  for (auto& slot : R.submissions) {
    if (slot.timestampQueries) vkDestroyQueryPool(R.device, slot.timestampQueries, nullptr);
    slot.timestampQueries = VK_NULL_HANDLE;
    slot.timestampRecorded = false;
    slot.gpuScopeCount = 0; slot.activeRenderScope = UINT32_MAX;
  }
}
static void init_gpu_timestamp_queries() {
  const char* requested = std::getenv("WWHD_VK_GPU_TIMESTAMPS");
  const char* passes = std::getenv("WWHD_VK_GPU_PASS_TIMESTAMPS");
  const bool passRequested = passes && !std::strcmp(passes, "1");
  if ((!requested || std::strcmp(requested, "1")) && !passRequested) return;
  if (!timestamp_capable(R.gpuTimestampValidBits, R.properties.limits.timestampPeriod)) {
    LOG("[vulkan GPU timestamps] unsupported: valid bits %u, period %.9g ns",
        R.gpuTimestampValidBits, double(R.properties.limits.timestampPeriod));
    return;
  }
  for (auto& slot : R.submissions) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = passRequested ? 2 + 2 * Renderer::Submission::maxGpuScopes : 2;
    const VkResult result = vkCreateQueryPool(R.device, &info, nullptr, &slot.timestampQueries);
    if (result != VK_SUCCESS) {
      // Initialization has submitted no work, so partial pools are safe to destroy.
      slot.timestampQueries = VK_NULL_HANDLE;
      destroy_gpu_timestamp_queries();
      LOG("[vulkan GPU timestamps] optional query pools unavailable (%d); disabled", int(result));
      return;
    }
  }
  R.gpuTimestampsEnabled = true;
  R.gpuPassTimestampsEnabled = passRequested;
  if (passRequested) LOG("[vulkan GPU pass timestamps] enabled: 256 bounded scopes/submission; stage intervals may overlap or include stalls; NOT GPU busy time");
  LOG("[vulkan GPU timestamps] enabled: %u valid bits, %.9g ns/tick; submission intervals, NOT GPU busy time",
      R.gpuTimestampValidBits, double(R.properties.limits.timestampPeriod));
}
static uint64_t steady_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
static void collect_gpu_timestamp_queries(Renderer::Submission& slot) {
  if (!slot.timestampRecorded) return;
  slot.timestampRecorded = false; // Consume this submission exactly once.
  if (!R.gpuTimestampsEnabled) return;
  struct Result { uint64_t ticks, available; } values[2]{};
  const VkResult result = vkGetQueryPoolResults(R.device, slot.timestampQueries, 0, 2,
      sizeof(values), values, sizeof(Result),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (result == VK_NOT_READY || (result == VK_SUCCESS &&
      (!values[0].available || !values[1].available))) {
    ++R.gpuTimestampStats.unavailable;
    return;
  }
  if (result != VK_SUCCESS) {
    ++R.gpuTimestampStats.unavailable;
    R.gpuTimestampsEnabled = false; // Retain pools until the shutdown drain.
    LOG("[vulkan GPU timestamps] optional results failed (%d); disabled", int(result));
    return;
  }
  const double ns = double(timestamp_elapsed_ticks(values[0].ticks, values[1].ticks,
      R.gpuTimestampValidBits)) * double(R.properties.limits.timestampPeriod);
  // WWHD_VK_SUBMIT_LOG=n: the first n submissions after frame WWHD_VK_SUBMIT_LOG_FROM (default 0)
  // with CPU begin/submit/collect and GPU start/end times in microseconds (GPU: its own clock base).
  // On the Switch the GPU ticks ran ~1.63x slower than timestampPeriod claims (measured against fence
  // waits), so every GPU timestamp figure here undercounts real GPU time by that factor.
  static int64_t submitLog = [] { const char* e = std::getenv("WWHD_VK_SUBMIT_LOG"); return e ? atoll(e) : 0; }();
  static uint64_t submitLogFrom = [] { const char* e = std::getenv("WWHD_VK_SUBMIT_LOG_FROM"); return e ? strtoull(e, nullptr, 10) : 0; }();
  if (submitLog > 0 && slot.timestampFrame >= submitLogFrom) {
    --submitLog;
    const double period = double(R.properties.limits.timestampPeriod);
    LOG("[vk submit] serial %llu frame %llu cpu begin %.1f submit %.1f collect %.1f gpu start %.1f end %.1f us",
        (unsigned long long)slot.serial, (unsigned long long)slot.timestampFrame,
        slot.beginNs / 1e3, slot.submitNs / 1e3, steady_ns() / 1e3,
        double(values[0].ticks) * period / 1e3, double(values[1].ticks) * period / 1e3);
  }
  auto& stats = R.gpuTimestampStats;
  ++stats.submissions;
  stats.intervalNs += ns;
  stats.maxIntervalNs = std::max(stats.maxIntervalNs, ns);
  stats.zeroIntervals += ns == 0;
}
static void collect_gpu_pass_queries(Renderer::Submission& slot) {
  const uint32_t count=slot.gpuScopeCount;
  slot.gpuScopeCount=0;slot.activeRenderScope=UINT32_MAX;
  if(!count || !R.gpuTimestampsEnabled || !R.gpuPassTimestampsEnabled) return;
  struct Result { uint64_t ticks,available; };
  std::array<Result,2*Renderer::Submission::maxGpuScopes> values{};
  const VkResult result=vkGetQueryPoolResults(R.device,slot.timestampQueries,2,2*count,
      2*count*sizeof(Result),values.data(),sizeof(Result),
      VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if(result!=VK_SUCCESS && result!=VK_NOT_READY) {
    gpuScopeTotals.unavailable+=count;R.gpuTimestampsEnabled=false;
    LOG("[vulkan GPU pass timestamps] optional results failed (%d); disabled",int(result));
    return;
  }
  for(uint32_t i=0;i<count;++i) {
    if(!slot.gpuScopes[i].ended || !values[2*i].available || !values[2*i+1].available) {
      ++gpuScopeTotals.unavailable;continue;
    }
    const double ns=double(timestamp_elapsed_ticks(values[2*i].ticks,values[2*i+1].ticks,
        R.gpuTimestampValidBits))*double(R.properties.limits.timestampPeriod);
    add_gpu_scope(slot.gpuScopes[i],ns);
  }
}
static void report_gpu_pass_timestamps(double frames) {
  if(!R.gpuPassTimestampsEnabled) return;
  LOG("[vulkan GPU pass timestamps] %llu available; %llu unavailable; %llu zero; %llu capacity drops; %llu bucket drops; stage intervals NOT GPU busy time%s",
      (unsigned long long)gpuScopeTotals.available,(unsigned long long)gpuScopeTotals.unavailable,
      (unsigned long long)gpuScopeTotals.zero,(unsigned long long)gpuScopeTotals.dropped,
      (unsigned long long)gpuScopeTotals.bucketDrops,
      gpuScopeTotals.available && gpuScopeTotals.available==gpuScopeTotals.zero
          ? "; all-zero/emulated timing UNUSABLE" : "");
  std::array<uint32_t,256> order{};
  for(uint32_t i=0;i<gpuScopeBucketCount;++i) order[i]=i;
  std::sort(order.begin(),order.begin()+gpuScopeBucketCount,[](uint32_t a,uint32_t b) {
    return gpuScopeBuckets[a].intervalNs>gpuScopeBuckets[b].intervalNs;
  });
  for(uint32_t rank=0;rank<std::min(gpuScopeBucketCount,8u);++rank) {
    const auto& b=gpuScopeBuckets[order[rank]];const auto& m=b.metadata;
    char formats[128]{};size_t used=0;
    for(uint32_t i=0;i<m.colors.size();++i) if(m.colors[i]!=VK_FORMAT_UNDEFINED)
      used+=size_t(std::snprintf(formats+used,sizeof(formats)-used,"%s%u:%u",used?",":"",i,uint32_t(m.colors[i])));
    LOG("[vulkan GPU scope %u] %s %ux%ux%u; colors [%s] depth %u; mips %u layers %u aspects 0x%x depth-only %u; %.3f ms/frame %.3f mean %.3f max ms; %.2f scopes/frame %.2f draws/frame",
        rank+1,m.kind?"feedback":"render",m.extent.width,m.extent.height,m.extent.depth,
        formats,uint32_t(m.depth),m.mips,m.layers,m.aspects,m.depthOnly,
        b.intervalNs/1e6/frames,b.intervalNs/1e6/double(b.count),b.maxNs/1e6,
        b.count/frames,b.draws/frames);
  }
  gpuScopeBuckets={};gpuScopeBucketCount=0;gpuScopeTotals={};
}
static void report_gpu_timestamps() {
  static uint64_t previousFrame = 0;
  if (!R.gpuTimestampsEnabled || R.frame - previousFrame < 120) return;
  const auto& stats = R.gpuTimestampStats;
  const double frames = double(R.frame - previousFrame);
  LOG("[vulkan GPU timestamps] submission interval sum %.3f ms/frame; max %.3f ms/submission; %llu submissions; %llu unavailable; %llu zero; NOT GPU busy time%s",
      stats.intervalNs / 1e6 / frames, stats.maxIntervalNs / 1e6,
      (unsigned long long)stats.submissions, (unsigned long long)stats.unavailable,
      (unsigned long long)stats.zeroIntervals,
      stats.submissions && stats.zeroIntervals == stats.submissions
          ? "; all-zero/emulated timing UNUSABLE" : "");
  report_gpu_pass_timestamps(frames);
  R.gpuTimestampStats = {};
  previousFrame = R.frame;
}
static void wait_submission_traced(uint64_t serial, char trace);
void draw_timing_command_begin(VkCommandBuffer cmd);  // draw.cpp, WWHD_VK_DRAW_TIMING
VkCommandBuffer command_buffer() {
  if (asynchronous_submissions() && R.beginFrame) {
    R.beginFrame=false;
    wait_submission_traced(R.frameSubmissions[R.frame%R.frameSubmissions.size()],'F');
  }
  if (!R.recording) {
#ifdef __SWITCH__
    // Retired slot, no commands queued for it yet. Reset here so the host
    // synchronization boundary does not drain recording at the end of a batch.
    if (vkrecord::enabled())
      vk_check(vkResetFences(R.device, 1, &R.fence), "reset fence");
#endif
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(R.cmd, &bi), "begin command buffer");
    R.recording = true;
    if (R.gpuTimestampsEnabled) {
      auto& slot = R.submissions[R.activeSubmission];
      vkCmdResetQueryPool(R.cmd, slot.timestampQueries, 0,
          R.gpuPassTimestampsEnabled ? 2 + 2 * Renderer::Submission::maxGpuScopes : 2);
      vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestampQueries, 0);
      slot.timestampRecorded = true;
      slot.timestampFrame = R.frame;
      slot.beginNs = steady_ns();
    }
    draw_timing_command_begin(R.cmd);
  }
  return R.cmd;
}
static GpuScopeToken begin_gpu_scope(const GpuScopeMetadata& metadata) {
  if(!R.gpuTimestampsEnabled || !R.gpuPassTimestampsEnabled) return {};
  if(R.rendering) { ++gpuScopeTotals.dropped;return {}; }
  const VkCommandBuffer cmd=command_buffer();
  auto& slot=R.submissions[R.activeSubmission];
  if(slot.gpuScopeCount==Renderer::Submission::maxGpuScopes) { ++gpuScopeTotals.dropped;return {}; }
  const uint32_t index=slot.gpuScopeCount++;
  slot.gpuScopes[index]={metadata,0,R.submissionGeneration,false};
  vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,slot.timestampQueries,2+2*index);
  return {R.submissionGeneration,index};
}
static void end_gpu_scope(GpuScopeToken token) {
  if(token.index==UINT32_MAX || token.generation!=R.submissionGeneration) return;
  auto& slot=R.submissions[R.activeSubmission];
  if(token.index>=slot.gpuScopeCount || slot.gpuScopes[token.index].ended ||
      slot.gpuScopes[token.index].generation!=token.generation) return;
  if(R.rendering) { ++gpuScopeTotals.dropped;return; }
  vkCmdWriteTimestamp(R.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,slot.timestampQueries,3+2*token.index);
  slot.gpuScopes[token.index].ended=true;
}
void gpu_begin_render_scope(const std::array<Surface*,8>& colors,Surface* depth,
                            const uint32_t* slices,uint32_t depthSlice,uint32_t width,uint32_t height) {
  if(!R.gpuTimestampsEnabled || !R.gpuPassTimestampsEnabled) return;
  GpuScopeMetadata metadata;metadata.extent={width,height,1};metadata.depthOnly=true;
  Surface* target=depth;
  for(uint32_t i=0;i<colors.size();++i) if(colors[i]) {
    metadata.colors[i]=colors[i]->fmt.pixel;metadata.slices[i]=slices[i];
    ++metadata.colorCount;metadata.depthOnly=false;if(target==depth) target=colors[i];
  }
  if(depth) { metadata.depth=depth->fmt.pixel;metadata.depthSlice=depthSlice; }
  if(target) { metadata.mips=target->mips;metadata.layers=target->arrayLayers;metadata.aspects=target->aspect; }
  auto token=begin_gpu_scope(metadata);
  R.submissions[R.activeSubmission].activeRenderScope=token.index;
}
void gpu_count_render_draw() {
  if(!R.gpuTimestampsEnabled || !R.gpuPassTimestampsEnabled) return;
  auto& slot=R.submissions[R.activeSubmission];
  if(slot.activeRenderScope<slot.gpuScopeCount) {
    auto& scope=slot.gpuScopes[slot.activeRenderScope];
    if(scope.generation==R.submissionGeneration && !scope.ended) ++scope.draws;
  }
}
GpuScopeToken gpu_begin_feedback_scope(const Surface& source) {
  if(!R.gpuTimestampsEnabled || !R.gpuPassTimestampsEnabled) return {};
  GpuScopeMetadata metadata;metadata.kind=1;metadata.extent=source.extent;
  metadata.mips=source.mips;metadata.layers=source.arrayLayers;metadata.aspects=source.aspect;
  if(source.fmt.depth) { metadata.depth=source.fmt.pixel;metadata.depthOnly=true; }
  else { metadata.colors[0]=source.fmt.pixel;metadata.colorCount=1; }
  return begin_gpu_scope(metadata);
}
void gpu_end_feedback_scope(GpuScopeToken token) { end_gpu_scope(token); }
void end_encoder() {
  if (R.rendering) {
    vkCmdEndRendering(R.cmd);
    R.rendering = false;
    R.passTracked = false;
    if(R.gpuPassTimestampsEnabled) {
      auto& slot=R.submissions[R.activeSubmission];
      end_gpu_scope({R.submissionGeneration,slot.activeRenderScope});
      slot.activeRenderScope=UINT32_MAX;
    }
  }
}
struct ImageSourceScope {
  VkPipelineStageFlags stage;
  VkAccessFlags access;
};
static ImageSourceScope image_source_scope(VkImageLayout layout, bool narrow) {
  if (layout == VK_IMAGE_LAYOUT_UNDEFINED)
    return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
  if (narrow) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      // Current image consumers are VS/PS only. Add any future compute or
      // input-attachment consumers here before using this opt-in path for them.
      return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT};
    default: break; // GENERAL/self-copy and unknown layouts remain conservative.
    }
  }
  return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
}
void transition_image(Surface *s, VkImageLayout layout,
                      VkPipelineStageFlags stage, VkAccessFlags access) {
  // Repeated read-only texture bindings require no barrier or pass break.
  if (s->layout == layout && layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
      access == VK_ACCESS_SHADER_READ_BIT) return;
  end_encoder();
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = s->layout;
  b.newLayout = layout;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = s->image;
  b.subresourceRange = {s->aspect, 0, s->mips, 0, s->arrayLayers};
  static const bool narrow = [] {
    const char* e = std::getenv("WWHD_VK_NARROW_BARRIERS");
    return e && !std::strcmp(e, "1");
  }();
  const auto source = image_source_scope(s->layout, narrow);
  b.srcAccessMask = source.access;
  b.dstAccessMask = access;
  vkCmdPipelineBarrier(command_buffer(), source.stage,
                       stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  s->layout = layout;
}
static void cleanup_submission(Renderer::Submission& slot) {
  // The submit fence has completed; slices can now be overwritten safely.
  for (auto& block : slot.uploadBlocks) {
    block.used = 0;block.dirtyBegin = VK_WHOLE_SIZE;block.dirtyEnd = 0;
  }
  for (auto b : slot.garbageBuffers) {
    if (b.mapped)
      vkUnmapMemory(R.device, b.memory);
    vkDestroyBuffer(R.device, b.buffer, nullptr);
    vkFreeMemory(R.device, b.memory, nullptr);
  }
  slot.garbageBuffers.clear();
  for (auto pool : slot.garbagePools) vkDestroyDescriptorPool(R.device, pool, nullptr);
  slot.garbagePools.clear();
  for (auto &i : slot.garbageImages)
    recycle_surface_image(std::move(i));
  slot.garbageImages.clear();
  vk_check(vkResetCommandPool(R.device, slot.commandPool, 0),
           "reset command pool");
  vk_check(vkResetDescriptorPool(R.device, slot.descriptorPool, 0),
           "reset descriptor pool");
  ++R.submissionGeneration;
}
// trace: WWHD_FRAME_TRACE code of a GPU wait (A slot reuse, F frame N-2, W other waits)
static void retire_submission(Renderer::Submission& slot, WaitTiming& timing=submitWait, char trace='W') {
  if (!slot.pending) return;
  VkResult status=vkGetFenceStatus(R.device,slot.fence);
  if (status==VK_NOT_READY) {
    // frame trace: upper case at the start of a GPU wait, lower case at its end
    frame_trace::event(trace);
    struct TraceEnd { char c; ~TraceEnd() { frame_trace::event(c); } } traceEnd{char(trace+32)};
    vk_check(timed_call(timing,[&] {
      return vkWaitForFences(R.device,1,&slot.fence,VK_TRUE,UINT64_MAX);
    }),"wait submission retirement");
  } else vk_check(status,"submission fence status");
  collect_gpu_timestamp_queries(slot);
  if(R.gpuPassTimestampsEnabled) collect_gpu_pass_queries(slot);
  cleanup_submission(slot);
  slot.pending=false;
}
static void activate_submission(size_t index) {
  auto& slot=R.submissions[index];
  retire_submission(slot,submitWait,'A');
  R.activeSubmission=index;
  slot.serial=++R.submissionSerial;
  slot.gpuScopeCount=0;slot.activeRenderScope=UINT32_MAX;
  ++R.submissionGeneration;
  R.commandPool=slot.commandPool;R.cmd=slot.cmd;R.fence=slot.fence;
  R.descriptorPool=slot.descriptorPool;
  R.uploadBlocks=std::move(slot.uploadBlocks);
  R.garbageBuffers=std::move(slot.garbageBuffers);
  R.garbagePools=std::move(slot.garbagePools);
  R.garbageImages=std::move(slot.garbageImages);
  R.recording=false;R.rendering=false;R.passTracked=false;
}
static void drain_submissions() {
  // A fence signal covers earlier submissions on this same graphics queue.
  // Wait once at the newest pending fence rather than waking at each older
  // fence in ring-array order. Keep each slot's status check and fenced cleanup.
  Renderer::Submission* newest=nullptr;
  for (auto& slot:R.submissions)
    if (slot.pending && (!newest || slot.serial>newest->serial)) newest=&slot;
  if (newest) retire_submission(*newest);
  for (auto& slot:R.submissions) retire_submission(slot);
}
static uint64_t submit(VkSemaphore wait = VK_NULL_HANDLE,
                   VkSemaphore signal = VK_NULL_HANDLE, bool asynchronous=false) {
  if (!R.recording)
    return R.lastSubmittedSerial;
  end_encoder();
  auto& recordingSlot = R.submissions[R.activeSubmission];
  if (recordingSlot.timestampRecorded)
    vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        recordingSlot.timestampQueries, 1);
  vk_check(vkEndCommandBuffer(R.cmd), "end command buffer");
#ifdef __SWITCH__
  if (!vkrecord::enabled())
#endif
  vk_check(vkResetFences(R.device, 1, &R.fence), "reset fence");
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &R.cmd;
  if (wait) {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
  }
  if (signal) {
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signal;
  }
  // Each block belongs exclusively to this submission until fence retirement;
  // atom expansion cannot race another slot's GPU reads or CPU writes.
  flush_uploads();
  frame_trace::event('S');
  R.submissions[R.activeSubmission].submitNs = steady_ns();
  vk_check(vkQueueSubmit(R.queue, 1, &si, R.fence), "submit graphics");
  R.recording=false;
  auto& slot=R.submissions[R.activeSubmission];
  const uint64_t serial=slot.serial;
  R.lastSubmittedSerial=serial;
  slot.uploadBlocks=std::move(R.uploadBlocks);
  slot.garbageBuffers=std::move(R.garbageBuffers);
  slot.garbagePools=std::move(R.garbagePools);
  slot.garbageImages=std::move(R.garbageImages);
  slot.pending=true;
  if (asynchronous) {
    activate_submission((R.activeSubmission+1)%R.submissions.size());
  } else {
    retire_submission(slot,wait ? presentSubmitWait : submitWait);
    activate_submission(R.activeSubmission);
  }
  return serial;
}
uint64_t recording_submission() {
  return R.submissions[R.activeSubmission].serial;
}
static void wait_submission_traced(uint64_t serial, char trace) {
  if (!serial) return;
  // A reader may request a token before its commands have been submitted.
  if (serial==recording_submission() && R.recording) flush_async();
  for (auto& slot:R.submissions)
    if (slot.serial==serial) {
      retire_submission(slot,submitWait,trace);
      return;
    }
  // Absent tokens have already completed and their slot has been reused.
}
void wait_submission(uint64_t serial) { wait_submission_traced(serial,'W'); }
void flush_async() {
  // Deferred objects may reference earlier queued work even if this slot has
  // no draw commands. Submit an empty command buffer to retire them in order.
  if (!R.recording && (!R.garbageBuffers.empty() || !R.garbageImages.empty())) command_buffer();
  submit(VK_NULL_HANDLE,VK_NULL_HANDLE,true);
}
void flush() {
  if (asynchronous_submissions()) {
    flush_async();
    return;
  }
  if (!R.recording && (!R.garbageBuffers.empty() || !R.garbageImages.empty())) command_buffer();
  submit();
  drain_submissions();
}
void flush_readback() {
  if (!R.recording && (!R.garbageBuffers.empty() || !R.garbageImages.empty())) command_buffer();
  // Keep this completed slot active for reuse; unlike flush(), there is no
  // drain of other slots (the readback fence already covers earlier work).
  const uint64_t serial=submit();
  wait_submission(serial);
}
void wait_idle() {
  flush_async();
  drain_submissions();
  vk_check(vkDeviceWaitIdle(R.device), "device idle");
}
void with_autorelease_pool(void (*fn)()) { host::with_autorelease_pool(fn); }
uint64_t frames_completed() { return R.completed.load(); }
uint64_t frame_count() { return std::atomic_ref<uint64_t>(R.frame).load(); }
static bool has_extension(const std::vector<VkExtensionProperties> &es,
                          const char *name) {
  return std::any_of(es.begin(), es.end(),
                     [&](auto &e) { return !strcmp(e.extensionName, name); });
}
static void make_swapchain(Screen &s) {
  wait_idle();
  VkSurfaceCapabilitiesKHR caps;
  vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.physicalDevice,
                                                     s.surface, &caps),
           "surface capabilities");
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(R.physicalDevice, s.surface, &n,
                                       nullptr);
  std::vector<VkSurfaceFormatKHR> fs(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(R.physicalDevice, s.surface, &n,
                                       fs.data());
  if (fs.empty())
    throw std::runtime_error("No presentation formats");
  // Presentation uses image blits, so both advertised surface support and
  // optimal-tiling BLIT_DST format support must hold.
  if (fs.size() == 1 && fs[0].format == VK_FORMAT_UNDEFINED)
    fs[0].format = VK_FORMAT_B8G8R8A8_SRGB;
  VkSurfaceFormatKHR format{};
  bool found = false;
#ifdef WWHD_SDL_HOST
  const VkFormat preferred = VK_FORMAT_B8G8R8A8_SRGB;
#else
  // AppKit windows: like the Metal layer, an sRGB drawable only for an sRGB scan buffer (the
  // composition, overlay blending included, then matches the Metal renderer's)
  const VkFormat preferred = s.srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
#endif
  for (auto f : fs) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(R.physicalDevice, f.format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
      continue;
    if (!found || f.format == preferred) {
      format = f;
      found = true;
    }
    if (f.format == preferred &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      break;
  }
  if (!found)
    throw std::runtime_error("No swapchain format supports image blits");
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX)
    extent = {std::clamp<uint32_t>(std::max(s.width.load(), 1),
                                   caps.minImageExtent.width,
                                   caps.maxImageExtent.width),
              std::clamp<uint32_t>(std::max(s.height.load(), 1),
                                   caps.minImageExtent.height,
                                   caps.maxImageExtent.height)};
#ifdef __ANDROID__
  // Phones report the extent in the display's natural (portrait) orientation together with a 90°
  // or 270° current transform. The pictures are laid out for the landscape window, so the
  // swapchain gets the window's own size and an identity transform: the compositor rotates it.
  // (The window can also still be portrait while the activity turns to landscape: its size
  // changes then and the swapchain is made again for the new size.)
  {
    int pw = 0, ph = 0;
    if (s.window && SDL_GetWindowSizeInPixels(s.window, &pw, &ph) && pw > 0 && ph > 0)
      extent = {uint32_t(pw), uint32_t(ph)};
    else if (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                                      VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
      std::swap(extent.width, extent.height);
  }
  const VkSurfaceTransformFlagBitsKHR transform =
      (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
  LOG("[vulkan] swapchain %ux%u (surface reports %ux%u, transform %u)", extent.width, extent.height,
      caps.currentExtent.width, caps.currentExtent.height, unsigned(caps.currentTransform));
#else
  const VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#endif
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    throw std::runtime_error("Swapchain cannot receive scan-buffer blits");
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = s.surface;
  ci.minImageCount = std::max(caps.minImageCount, 2u);
  if (caps.maxImageCount)
    ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
  ci.imageFormat = format.format;
  ci.imageColorSpace = format.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  VkFormatProperties presentationFormat;
  vkGetPhysicalDeviceFormatProperties(R.physicalDevice,format.format,&presentationFormat);
  const bool shaderPresentation = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
      (presentationFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
  const bool captureTransfer = present_capture_requested() &&
      (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
      (presentationFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      (shaderPresentation ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
      (captureTransfer ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = transform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (auto flag : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
    if (caps.supportedCompositeAlpha & flag) {
      ci.compositeAlpha = flag;
      break;
    }
  uint32_t modeCount = 0;
  vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(R.physicalDevice,
      s.surface, &modeCount, nullptr), "presentation mode count");
  std::vector<VkPresentModeKHR> modes(modeCount);
  vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(R.physicalDevice,
      s.surface, &modeCount, modes.data()), "presentation modes");
  // Presentation (Graphics > Presentation in the settings overlay, WWHD_VK_PRESENT_MODE): FIFO (vsync)
  // by default; mailbox (low latency) or immediate (may tear) when chosen and the surface offers them.
  // Guest GX2 pacing still controls game flips in every mode.
  static const VkPresentModeKHR kModes[kPresentModes] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                                         VK_PRESENT_MODE_IMMEDIATE_KHR};
  unsigned offered = 0;
  std::string offeredNames;
  for (int m = 0; m < kPresentModes; m++)
    if (std::find(modes.begin(), modes.end(), kModes[m]) != modes.end()) {
      offered |= 1u << m;
      offeredNames += std::string(offeredNames.empty() ? "" : ", ") + present_mode_name(m);
    }
  if (&s == &R.tv) set_present_modes_offered(offered);
  const int wanted = present_mode();
  const int chosen = offered >> wanted & 1 ? wanted : kPresentFifo;
  ci.presentMode = kModes[chosen];
  if (chosen != s.presentMode || wanted != s.presentWanted)
    LOG("[vulkan] %s present mode %s (available: %s)%s", &s == &R.tv ? "TV" : "GamePad", present_mode_name(chosen),
        offeredNames.c_str(), chosen != wanted ? " - the requested mode is not offered" : "");
  s.presentMode = chosen;
  s.presentWanted = wanted;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = s.swapchain;
  VkSwapchainKHR sc;
  vk_check(vkCreateSwapchainKHR(R.device, &ci, nullptr, &sc),
           "create swapchain");
  reset_present_screen(s); // Device was drained above; old views are no longer in use.
  for (auto semaphore:s.finished) vkDestroySemaphore(R.device,semaphore,nullptr);
  if (s.swapchain)
    vkDestroySwapchainKHR(R.device, s.swapchain, nullptr);
  s.swapchain = sc;
  s.swapFormat = format.format;
  s.swapExtent = extent;
  vkGetSwapchainImagesKHR(R.device, sc, &n, nullptr);
  s.images.resize(n);
  vkGetSwapchainImagesKHR(R.device, sc, &n, s.images.data());
  s.layouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);
  s.finished.assign(n,VK_NULL_HANDLE);
  VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& finished:s.finished)
    vk_check(vkCreateSemaphore(R.device,&semaphore,nullptr,&finished),
             "create present semaphore");
  prepare_present_screen(s,shaderPresentation,captureTransfer);
  s.resize = false;
}
#ifdef __ANDROID__
// Android destroys an app's surface when it goes to the background (Home, another app) and gives
// it a new one when it comes back. Meanwhile nothing is presented (the game keeps running); then
// the Vulkan surface and swapchain are created again for the window's new native surface.
static std::atomic<bool> surfaceLost{false}, surfaceRecreate{false};
static void recreate_surface(Screen &s) {
  if (!SDL_GetPointerProperty(SDL_GetWindowProperties(s.window),
                              SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) {
    surfaceRecreate = true;  // the new native surface is not there yet: next frame
    return;
  }
  vk_check(vkDeviceWaitIdle(R.device), "surface recreation idle");
  reset_present_screen(s);
  if (s.swapchain)
    vkDestroySwapchainKHR(R.device, s.swapchain, nullptr);
  s.swapchain = VK_NULL_HANDLE;
  if (s.surface)
    vkDestroySurfaceKHR(R.instance, s.surface, nullptr);
  s.surface = VK_NULL_HANDLE;
  if (!SDL_Vulkan_CreateSurface(s.window, R.instance, nullptr, &s.surface)) {
    LOG("[vulkan] surface recreation: %s (retrying)", SDL_GetError());
    s.surface = VK_NULL_HANDLE;
    surfaceRecreate = true;
    return;
  }
  surfaceLost = false;
  s.resize = true;
  LOG("[vulkan] presentation surface recreated");
}
// App lifecycle: called by SDL as the events happen, on the thread that sends them; SDL's
// documentation asks for an event watch here (the queue is not read while the app is paused).
static bool SDLCALL lifecycle_watch(void *, SDL_Event *event) {
  if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
    if (!surfaceLost.exchange(true))
      LOG("[vulkan] app in the background: presentation paused");
  } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
    LOG("[vulkan] app in the foreground: new presentation surface");
    R.tv.visible = true;  // (the window may have reported itself minimized meanwhile)
    surfaceRecreate = true;
  }
  return true;
}
#endif
// Asynchronous presentation (WWHD_VK_ASYNC_PRESENT=1, the default on Android; always on the Switch,
// see asynchronous_submissions): the presentation submission goes into the four-slot ring like
// GX2Flush work instead of waiting for the GPU, and the SDL host's swap() does not drain the queue, so
// the render thread records frame N+1 while the GPU draws frame N. Each submission slot has its own
// acquire semaphores (reused only after that slot retired) and each swapchain image its own
// render-finished semaphore. Captures keep the waiting path.
static bool async_present() {
  static const bool on = [] {
    const char *e = std::getenv("WWHD_VK_ASYNC_PRESENT");
#ifdef __ANDROID__
    return !e || std::atoi(e) != 0;
#else
    return e && std::atoi(e) != 0;
#endif
  }();
  return on;
}
static void present(Screen &s) {
#ifdef __ANDROID__
  if (&s == &R.tv && s.window) {
    if (surfaceRecreate.exchange(false))
      recreate_surface(s);
    if (surfaceLost || !s.surface)
      return;
  }
#endif
  // Presentation changed (settings overlay): a new swapchain, as for a resize (also for a window that
  // is not shown right now, so the next frame it shows uses the new mode)
  if (s.window && s.swapchain && s.presentWanted != present_mode())
    make_swapchain(s);
  if (!s.window || !s.visible || s.width <= 0 || s.height <= 0 || !s.scan ||
      !s.scan->image)
    return;
  VkFormatProperties sourceFormat;
  vkGetPhysicalDeviceFormatProperties(R.physicalDevice, s.scan->fmt.pixel,
                                      &sourceFormat);
  if (!(sourceFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
    throw std::runtime_error(
        "Scan-buffer format does not support presentation blits");
  if (s.scan->fmt.kind != FormatInfo::FLOAT)
    throw std::runtime_error("Integer scan buffers cannot be blitted to "
                             "normalized presentation images");
  VkFilter filter = (sourceFormat.optimalTilingFeatures &
                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                        ? VK_FILTER_LINEAR
                        : VK_FILTER_NEAREST;
#ifndef WWHD_SDL_HOST
  if (s.swapchain && (s.swapFormat == VK_FORMAT_B8G8R8A8_SRGB) != s.srgb.load())
    s.resize = true;  // the scan buffer's encoding changed (GX2SetTVBuffer)
#endif
#ifdef __ANDROID__
  if (s.resize || !s.swapchain) {
    try {
      make_swapchain(s);
    } catch (const std::exception &e) {
      if (&s != &R.tv)
        throw;
      LOG("[vulkan] no swapchain (%s); waiting for a new surface", e.what());
      surfaceLost = true;  // the surface went away while the app is in the background
      return;
    }
  }
#else
  if (s.resize || !s.swapchain)
    make_swapchain(s);
#endif
  uint32_t index;
  const size_t screenIndex=&s == &R.tv ? 0 : 1;
  // asynchronous: the presentation submission joins the four-slot ring instead of waiting for the GPU
  // (the Switch always; elsewhere WWHD_VK_ASYNC_PRESENT); captures keep the waiting path there
  const bool async = asynchronous_submissions() || (async_present() && !present_capture_requested());
  // per submission slot (reused only once the slot retired), so frames in flight never share one
  const VkSemaphore acquired=R.submissions[R.activeSubmission].acquired[screenIndex];
  auto& timing = screenTiming[screenIndex];
  VkResult ar = timed_call(timing.acquire, [&] {
    return vkAcquireNextImageKHR(R.device, s.swapchain, UINT64_MAX,
                                acquired, VK_NULL_HANDLE, &index);
  });
  if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
    s.resize = true;
    return;
  }
#ifdef __ANDROID__
  if (ar == VK_ERROR_SURFACE_LOST_KHR) {
    surfaceLost = true;
    return;
  }
#endif
  if (ar != VK_SUBOPTIMAL_KHR)
    vk_check(ar, "acquire scan image");
  if (!draw_present_screen(s,index)) {
    transition_image(s.scan.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = s.layouts[index];
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.images[index];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    auto cmd = command_buffer();
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkClearColorValue black{};
    vkCmdClearColorImage(cmd, s.images[index],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &b.subresourceRange);
    // Clear and blit overlap the swap image. Order the two transfer writes.
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    const float ratio =
        std::min(float(s.swapExtent.width) / s.scan->extent.width,
                 float(s.swapExtent.height) / s.scan->extent.height);
    int w = int(s.scan->extent.width * ratio),
        h = int(s.scan->extent.height * ratio);
    int x = (s.swapExtent.width - w) / 2, y = (s.swapExtent.height - h) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int(s.scan->extent.width), int(s.scan->extent.height),
                          1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    vkCmdBlitImage(cmd, s.scan->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, filter);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    s.layouts[index] = b.newLayout;
  }
  record_present_capture(s,index);
  const VkSemaphore finished=s.finished[index];
  submit(acquired, finished, async);
  finish_present_capture(s);
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &finished;
  pi.swapchainCount = 1;
  pi.pSwapchains = &s.swapchain;
  pi.pImageIndices = &index;
  VkResult pr = timed_call(timing.present, [&] {
    return vkQueuePresentKHR(R.queue, &pi);
  });
#ifdef __ANDROID__
  // SUBOPTIMAL here only says the compositor rotates the picture (identity pre-transform, see
  // make_swapchain); size changes come as window events. Rebuilding would happen every frame.
  if (pr == VK_ERROR_OUT_OF_DATE_KHR)
    s.resize = true;
  else if (pr == VK_SUBOPTIMAL_KHR) {
  } else if (pr == VK_ERROR_SURFACE_LOST_KHR)
    surfaceLost = true;
  else
    vk_check(pr, "present scan buffer");
#else
  if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
    s.resize = true;
  else
    vk_check(pr, "present scan buffer");
#endif
  if (!async)
    vk_check(timed_call(timing.idle, [&] { return vkQueueWaitIdle(R.queue); }),
             "present completion");
}
void copy_to_scan(uint32_t cb, uint32_t target) {
  copy_to_scan_payload(mem::ptr(cb), target);
}
void copy_to_scan_payload(const void* cb, uint32_t target) {
  Surface *src = surface_from_color_payload(cb);
  if (!src)
    return;
  Screen &s = target == 4 ? R.drc : R.tv;
  if (!s.scan || s.scan->extent.width != src->extent.width ||
      s.scan->extent.height != src->extent.height ||
      s.scan->fmt.pixel != src->fmt.pixel) {
    if (s.scan)
      destroy_surface_image(s.scan.get());
    s.scan = std::make_unique<Surface>();
    auto *d = s.scan.get();
    d->width = src->extent.width;
    d->height = src->extent.height;
    d->format = src->format;
    d->fmt = src->fmt;
    d->scale = 1;
    create_surface_image(d, false);
  }
  resample(src, s.scan.get(), 1);
  if (target & 1)
    draw_mod_overlay(*s.scan);  // HUD of gameplay mods (stamina wheel), as the Metal renderer
  mark_gpu_written(s.scan.get());
}
// debug: WWHD_DUMP_FRAMES=100,300 writes the TV image of those frames to frame_<n>.png (and
// frame_<n>_drc.png; WWHD_DUMP_PRESENT=1 adds the composed window, frame_<n>_present.png), as Metal
static void dump_scan(Screen &s, const std::string &path) {
  if (!s.scan || !s.scan->image)
    return;
  try {
    write_rgba_png(path, s.scan->extent.width, s.scan->extent.height,
                   read_surface_rgba(*s.scan, s.srgb.load()));
    LOG("[gfx] wrote %s (%ux%u)", path.c_str(), s.scan->extent.width, s.scan->extent.height);
  } catch (const std::exception &e) {
    LOG("[gfx] cannot write %s: %s", path.c_str(), e.what());
  }
}
// the composed windows (what present() puts on the screen), written after the frame (swap)
static void request_present_dump(const std::string& path) { gfx::request_present_dump(path); }
static std::atomic<bool> captureRequested{false};
void request_capture() { captureRequested = true; }
static void frame_dumps(uint64_t frame) {
  static const std::vector<uint64_t> frames = [] {
    std::vector<uint64_t> f;
    if (const char *e = getenv("WWHD_DUMP_FRAMES"))
      for (const char *p = e; *p;) {
        f.push_back(strtoull(p, (char **)&p, 10));
        while (*p == ',') p++;
      }
    return f;
  }();
  if (std::find(frames.begin(), frames.end(), frame) != frames.end()) {
    dump_scan(R.tv, "frame_" + std::to_string(frame) + ".png");
    dump_scan(R.drc, "frame_" + std::to_string(frame) + "_drc.png");
    if (getenv("WWHD_DUMP_PRESENT"))
      request_present_dump("frame_" + std::to_string(frame) + "_present.png");
  }
  // P / F12 (Graphics menu): the pictures of this frame in captures/<time>/ (WWHD_CAPTURE=<frame>
  // scripts it when WWHD_CAPTURE_PATH is not used); the Metal renderer also writes a draw log
  static const uint64_t scripted = getenv("WWHD_CAPTURE") && !getenv("WWHD_CAPTURE_PATH")
                                       ? strtoull(getenv("WWHD_CAPTURE"), nullptr, 10) : ~0ull;
  if (captureRequested.exchange(false) || frame == scripted) {
    char dir[64];
    time_t t = time(nullptr);
    strftime(dir, sizeof dir, "captures/%Y%m%d-%H%M%S", localtime(&t));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    dump_scan(R.tv, std::string(dir) + "/tv.png");
    dump_scan(R.drc, std::string(dir) + "/gamepad.png");
    request_present_dump(std::string(dir) + "/present.png");
    LOG("[gfx] capture of frame %llu written to %s", (unsigned long long)frame, dir);
  }
}
void swap() {
  service_captures();
  frame_dumps(R.frame + 1);
  // the layout of both pictures (GamePad window, picture-in-picture, automatic overlay, GamePad only)
  // comes from gfx/display_modes.cpp, which both window hosts and the Metal renderer share
  Surface *tvScan = R.tv.scan && R.tv.scan->image ? R.tv.scan.get() : nullptr;
  Surface *drcScan = R.drc.scan && R.drc.scan->image ? R.drc.scan.get() : nullptr;
#ifdef WWHD_SDL_HOST
  gfx::g_filter = scale_filter();  // the SDL host keeps the scaling filter with the Vulkan settings
  const float layerW = float(R.tv.width.load()), layerH = float(R.tv.height.load());
#else
  const float layerW = float(R.tv.swapExtent.width), layerH = float(R.tv.swapExtent.height);
#endif
  gfx::PresentPlan plan = gfx::display_plan(
      tvScan, tvScan ? float(tvScan->extent.width) : 0, tvScan ? float(tvScan->extent.height) : 0,
      drcScan, drcScan ? float(drcScan->extent.width) : 0, drcScan ? float(drcScan->extent.height) : 0,
      layerW, layerH, R.frame + 1);
#ifdef __ANDROID__
  // the view button's dot: 60 fps chosen (long press), green while drawn, yellow while paused
  if (perf_hint::fps60_chosen())
    plan.button_dot = interp::mode() != 0 ? 1 : 2;
#endif
  set_present_plan(&plan);
  // settings overlay: built once, drawn into the TV window and its present dumps
  set_overlay_draw(overlay::frame(plan.dw > 0 ? plan.dw : layerW, plan.dh > 0 ? plan.dh : layerH, overlay_renderer_init));
  bool sampled[2] = {};
  if (plan.sample_auto && drcScan) {
    sampled[0] = record_signature(0, *drcScan, R.drc.srgb.load());
    sampled[1] = sampled[0] && tvScan && record_signature(1, *tvScan, R.tv.srgb.load());
  }
  present(R.tv);
  if (plan.drc_window)
    present(R.drc);
#ifdef WWHD_SDL_HOST
  // asynchronous presentation: queued like GX2Flush work, the ring's fences retire it (the automatic
  // overlay's signatures are read back right away, so those frames wait)
  if (async_present() && !sampled[0])
    flush_async();
  else
    flush();
#else
  flush();
#endif
  if (sampled[0]) {
    std::vector<float> d = read_signature(0), t = sampled[1] ? read_signature(1) : std::vector<float>{};
    gfx::display_auto_signature(d, sampled[1] ? &t : nullptr, R.frame + 1);
  }
  for (auto &path : gfx::display_take_present_dumps()) {
    if (!tvScan || plan.dw < 1 || plan.dh < 1)
      continue;
    try {
      write_rgba_png(path, uint32_t(plan.dw), uint32_t(plan.dh),
                     compose_offscreen(R.tv, uint32_t(plan.dw), uint32_t(plan.dh), R.tv.srgb.load()));
      // the GamePad window's size: its swapchain, or (SDL host test runs with hidden windows, which
      // present nothing) the window's pixel size
      uint32_t drcW = R.drc.swapchain ? R.drc.swapExtent.width : 0, drcH = R.drc.swapchain ? R.drc.swapExtent.height : 0;
#ifdef WWHD_SDL_HOST
      if (!R.drc.swapchain && R.drc.window)
        drcW = uint32_t(std::max(0, R.drc.width.load())), drcH = uint32_t(std::max(0, R.drc.height.load()));
#endif
      if (plan.drc_window && drcScan && drcW && drcH) {
        std::string p = path;
        size_t dot = p.rfind(".png");
        p.insert(dot == std::string::npos ? p.size() : dot, "_drc");
        write_rgba_png(p, drcW, drcH, compose_offscreen(R.drc, drcW, drcH, R.drc.srgb.load()));
      }
      gfx::display_log_present_dump(path, plan, float(tvScan->extent.width), float(tvScan->extent.height));
    } catch (const std::exception &e) {
      LOG("[gfx] present dump %s failed: %s", path.c_str(), e.what());
    }
  }
  set_present_plan(nullptr);
  if (asynchronous_submissions()) {
    R.frameSubmissions[R.frame%R.frameSubmissions.size()]=R.lastSubmittedSerial;
    R.beginFrame=true;
  }
  std::atomic_ref<uint64_t>(R.frame).fetch_add(1);
  R.completed = R.frame;
#ifdef __SWITCH__
  vkrecord::report(R.frame);
#endif
  report_gpu_timestamps();
  perf_hint::frame_done();
  {
    const auto checkpointStart = std::chrono::steady_clock::now();
    checkpoint_pipeline_cache();
    checkpoint_pipeline_recipes();
    vk::checkpoint_shader_cache(R.frame);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - checkpointStart).count();
    if (ms > 2.0) LOG("[vulkan cache] frame %llu: cache checkpoints took %.1f ms", (unsigned long long)R.frame, ms);
  }
  latch_res_scale();
  sweep_texture_page_writes();
#ifdef __SWITCH__
  vk::revalidate_programs();  // once a frame: programs on flushed pages vs their copies
#endif
#ifdef __SWITCH__
  // WWHD_SLOW_FRAMES=1: one line per swap interval over 40 ms, with what the frame spent its time on
  static const bool slowFrames = std::getenv("WWHD_SLOW_FRAMES") != nullptr;
  if (slowFrames) {
    static uint64_t lastIdle = 0, lastSync = 0;
    const uint64_t idleNs = gx2::g_render_idle_ns.load(), syncNs = gx2::g_main_sync_ns.load();
    static uint64_t lastTick = 0, lastMain = 0, lastRender = 0, lastRecord = 0, lastUploadNs = 0,
                    lastUploads = 0, lastChecks = 0, lastCheckNs = 0, lastCompiles = 0, lastPipes = 0,
                    lastDraws = 0, lastDescAllocs = 0, lastDescLookups = 0, lastLookups = 0,
                    lastEpoch = 0, lastDestroys = 0, lastSurfaces = 0, lastSkipped = 0;
    const uint64_t drawCalls = g_vk_draw_calls.load(std::memory_order_relaxed);
    const uint64_t shaderLookups = vk::shader_stats().lookups;
    const uint64_t tick = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t mainNs = switch_thread_cpu_ns(0), renderNs = switch_thread_cpu_ns(1), recordNs = switch_thread_cpu_ns(2);
    const uint64_t compiles = vk::shader_stats().compiles;
    if (lastTick) {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(tick - lastTick)).count();
      if (ms > 40.0)
        LOG("[slow frame] %llu: %.1f ms; cpu main %.1f render %.1f record %.1f; render idle %.1f, game in DrawDone %.1f; textures: %llu uploads %.1f ms, %llu full checks %.1f ms; %llu shaders, %llu pipelines; draws %llu, descriptor sets %llu new / %llu lookups, shader lookups %llu; surface epoch +%llu, image destroys %llu, surfaces +%llu; async: %llu draws skipped, %zu shaders compiling",
            (unsigned long long)R.frame, ms, (mainNs - lastMain) / 1e6, (renderNs - lastRender) / 1e6,
            (recordNs - lastRecord) / 1e6, (idleNs - lastIdle) / 1e6, (syncNs - lastSync) / 1e6,
            (unsigned long long)(g_stat_uploads - lastUploads),
            (g_upload_ns - lastUploadNs) / 1e6, (unsigned long long)(g_stat_full_checks - lastChecks),
            (g_full_check_ns - lastCheckNs) / 1e6, (unsigned long long)(compiles - lastCompiles),
            (unsigned long long)(R.pipelineCreates - lastPipes), (unsigned long long)(drawCalls - lastDraws),
            (unsigned long long)(R.descriptorAllocations - lastDescAllocs),
            (unsigned long long)(R.descriptorLookups - lastDescLookups), (unsigned long long)(shaderLookups - lastLookups),
            (unsigned long long)(R.surfaceEpoch - lastEpoch), (unsigned long long)(R.surfaceImageDestroys - lastDestroys),
            (unsigned long long)(R.surfaces.size() - lastSurfaces),
            (unsigned long long)(R.asyncSkippedDraws - lastSkipped), vk::pending_shader_compiles());
    }
    lastDraws = drawCalls; lastDescAllocs = R.descriptorAllocations; lastDescLookups = R.descriptorLookups;
    lastLookups = shaderLookups; lastEpoch = R.surfaceEpoch; lastDestroys = R.surfaceImageDestroys;
    lastSurfaces = R.surfaces.size(); lastSkipped = R.asyncSkippedDraws;
    lastIdle = idleNs; lastSync = syncNs;
    lastTick = tick; lastMain = mainNs; lastRender = renderNs; lastRecord = recordNs;
    lastUploads = g_stat_uploads; lastUploadNs = g_upload_ns; lastChecks = g_stat_full_checks;
    lastCheckNs = g_full_check_ns; lastCompiles = compiles; lastPipes = R.pipelineCreates;
  }
#endif
  if (perf_enabled() || cpu_only_stats_enabled()) {
    static auto start = std::chrono::steady_clock::now();
    static auto previousSwap = start;
    static bool havePreviousSwap = false;
    static std::array<double, 120> swapIntervals{};
    static size_t intervalCount = 0, slowIntervals = 0;
    static uint64_t frame=0,draws=0,bytes=0,allocs=0,passes=0;
    static uint64_t imageCreates=0,imageDestroys=0,imagePoolHits=0;
    static uint64_t descriptorLookups=0,descriptorHits=0,descriptorAllocations=0,descriptorFastHits=0;
    static uint64_t vertexDeclaredBytes=0,vertexCopiedBytes=0;
    static uint64_t vertexReuseChecks=0,vertexReuseHits=0,vertexReuseBytes=0,vertexReuseCompareNs=0;
    static uint64_t batchSubmissions=0,fetchLookups=0,fetchLastHits=0;
    static uint64_t stateHashLookups=0,stateHashMemoHits=0,stateHashBytes=0;
    static uint64_t pairLookups=0,pairHits=0,pairStateHits=0,fetchRangeHits=0;
    static uint64_t vertexBindCalls=0,vertexBindSkips=0;
    static uint64_t vertexHistoryRequests=0,vertexHistoryMatches=0,vertexHistoryBytes=0;
    static uint64_t vertexHistoryReuseChecks=0,vertexHistoryReuseHits=0,vertexHistoryReuseBytes=0;
    static std::array<uint64_t,7> vertexHistoryDistances{};
    static Renderer::CpuPreparationStats previousPreparation;
    static SparseHashStats previousSparse;
    static gx2::ShaderKeyDirtyStats previousShaderDirty;
#if defined(__SWITCH__) && !defined(CLOCK_THREAD_CPUTIME_ID)
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC  // statistics only: newlib has no per-thread CPU clock
#endif
#ifndef _WIN32
    auto threadCpuNs = []() -> uint64_t {
      timespec time{};
      if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) return 0;
      return uint64_t(time.tv_sec) * 1000000000ull + uint64_t(time.tv_nsec);
    };
    static uint64_t previousThreadCpu = threadCpuNs();
#endif
    auto now=std::chrono::steady_clock::now();
    if (havePreviousSwap && intervalCount < swapIntervals.size()) {
      double ms = std::chrono::duration<double, std::milli>(now-previousSwap).count();
      swapIntervals[intervalCount++] = ms;
      if (ms > 40.0) ++slowIntervals;
    }
    previousSwap=now;havePreviousSwap=true;
    if (R.frame-frame>=120) {
      double seconds=std::chrono::duration<double>(now-start).count();
#ifndef _WIN32
      const uint64_t currentThreadCpu = threadCpuNs();
      if (previousThreadCpu && currentThreadCpu >= previousThreadCpu)
        LOG("[vulkan thread CPU] %.3f ms/frame (excludes sleeping and GPU waits)",
            double(currentThreadCpu - previousThreadCpu) / 1e6 / (R.frame - frame));
      previousThreadCpu = currentThreadCpu;
#endif
      LOG("[vulkan perf] frame %llu %.2f fps, %.1f draws/frame, %.2f MiB uploads/frame, %llu new upload blocks, %.1f passes/frame",
          (unsigned long long)R.frame,(R.frame-frame)/seconds,
          double(R.drawCount-draws)/(R.frame-frame),double(R.uploadBytes-bytes)/(R.frame-frame)/(1<<20),
          (unsigned long long)(R.uploadAllocations-allocs), double(R.renderPassCount-passes)/(R.frame-frame));
      if (intervalCount) {
        std::sort(swapIntervals.begin(), swapIntervals.begin()+intervalCount);
        // Nearest-rank percentiles; the first report contains 119 intervals.
        size_t p50=(intervalCount+1)/2-1, p95=(intervalCount*95+99)/100-1;
        LOG("[vulkan pacing] %llu intervals p50 %.2f ms p95 %.2f ms max %.2f ms; >40 ms %llu",
            (unsigned long long)intervalCount,swapIntervals[p50],swapIntervals[p95],
            swapIntervals[intervalCount-1],(unsigned long long)slowIntervals);
#ifdef __SWITCH__
        static uint64_t lastFlushBumps = 0, lastInvalidateBumps = 0;
        const uint64_t fb = gx2::shaderProgramWrites.flushBumps.load(), ib = gx2::shaderProgramWrites.invalidateBumps.load();
        static uint64_t lastContent = 0;
        const uint64_t cb = gx2::shaderProgramWrites.contentBumps.load();
        LOG("[vulkan shader epoch] %llu bumps (%llu real program changes); %llu flushes of watched pages",
            (unsigned long long)(ib - lastInvalidateBumps), (unsigned long long)(cb - lastContent), (unsigned long long)(fb - lastFlushBumps));
        lastFlushBumps = fb; lastInvalidateBumps = ib; lastContent = cb;
#endif
        static uint64_t lastEpoch = 0;
        LOG("[vulkan epoch] %llu surface epoch bumps", (unsigned long long)(R.surfaceEpoch - lastEpoch));
        lastEpoch = R.surfaceEpoch;
      }
      intervalCount=0;slowIntervals=0;
      auto ss=vk::shader_stats();
      LOG("[vulkan shaders] lookups %llu last hits %llu variants %llu compiles %llu total compile %.1f ms",
          (unsigned long long)ss.lookups,(unsigned long long)ss.lastHits,(unsigned long long)ss.variantHits,(unsigned long long)ss.compiles,ss.compileNs/1e6);
      LOG("[vulkan shader disk] hits %llu; memory reuses %llu; SPIR-V compiles %llu %.1f ms; decompile %.1f ms; loads %llu %.1f ms; saves %llu worker %.1f ms %.2f MiB; snapshots %.1f ms",
          (unsigned long long)ss.diskHits,(unsigned long long)ss.spirvReuseHits,(unsigned long long)ss.spirvCompiles,ss.spirvCompileNs/1e6,
          ss.decompileNs/1e6,(unsigned long long)ss.diskLoads,ss.diskLoadNs/1e6,
          (unsigned long long)ss.diskSaves,ss.diskSaveNs/1e6,ss.diskSavedBytes/double(1<<20),ss.diskSnapshotNs/1e6);
      LOG("[vulkan pipelines] creates %llu total %.1f ms",
          (unsigned long long)R.pipelineCreates,R.pipelineCreateNs/1e6);
      {
        // texture change detection (surfaces.cpp upload_surface, write_watch.h)
        extern uint64_t g_stat_full_checks, g_stat_uploads;
        static uint64_t checks=0, uploads=0;
        uint64_t faults, protectedPages;
        wwatch::take_stats(faults, protectedPages);
        LOG("[vulkan textures] %llu full checks, %llu uploads, %llu page write faults, %llu pages protected",
            (unsigned long long)(g_stat_full_checks-checks),(unsigned long long)(g_stat_uploads-uploads),
            (unsigned long long)faults,(unsigned long long)protectedPages);
        checks=g_stat_full_checks;uploads=g_stat_uploads;
      }
      // CPU-only reports leave per-draw counters/comparison clocks disabled.
      if (perf_enabled()) {
      double frames = double(R.frame-frame);
      LOG("[vulkan surface images] %.2f creates/frame %.2f destroys/frame %.2f pool hits/frame; %zu retired images %.2f MiB pooled; %zu guest surfaces",
          (R.surfaceImageCreates-imageCreates)/frames,(R.surfaceImageDestroys-imageDestroys)/frames,
          (R.surfaceImagePoolHits-imagePoolHits)/frames,R.surfaceImagePool.size(),
          double(R.surfaceImagePoolBytes)/(1<<20),R.surfaces.size());
      imageCreates=R.surfaceImageCreates;imageDestroys=R.surfaceImageDestroys;imagePoolHits=R.surfaceImagePoolHits;
      LOG("[vulkan fetch memo] %.1f lookups/frame %.1f last hits/frame",
          (ss.fetchLookups-fetchLookups)/frames,(ss.fetchLastHits-fetchLastHits)/frames);
      fetchLookups=ss.fetchLookups;fetchLastHits=ss.fetchLastHits;
      LOG("[vulkan shader pair memo] %.1f lookups/frame %.1f hits/frame %.1f state gathers skipped/frame; %.1f fetch range hits/frame; %llu states %llu pairs %.3f MiB",
          (ss.pairLookups-pairLookups)/frames,(ss.pairHits-pairHits)/frames,
          (ss.pairStateHits-pairStateHits)/frames,(ss.fetchRangeHits-fetchRangeHits)/frames,
          (unsigned long long)ss.pairStates,(unsigned long long)ss.pairEntries,ss.pairCacheBytes/double(1<<20));
      pairLookups=ss.pairLookups;pairHits=ss.pairHits;pairStateHits=ss.pairStateHits;fetchRangeHits=ss.fetchRangeHits;
      static uint64_t supportChecks=0,supportHits=0,supportBytes=0;
      LOG("[vulkan support upload reuse] %.1f checks/frame %.1f hits/frame %.3f MiB avoided/frame",
          (R.cpuPreparation.supportReuseChecks-supportChecks)/frames,
          (R.cpuPreparation.supportReuseHits-supportHits)/frames,
          (R.cpuPreparation.supportReuseBytes-supportBytes)/frames/(1<<20));
      supportChecks=R.cpuPreparation.supportReuseChecks;supportHits=R.cpuPreparation.supportReuseHits;
      supportBytes=R.cpuPreparation.supportReuseBytes;
      LOG("[vulkan state hash memo] %.1f lookups/frame %.1f hits/frame %.3f MiB hashed/frame",
          (ss.stateHashLookups-stateHashLookups)/frames,
          (ss.stateHashMemoHits-stateHashMemoHits)/frames,
          (ss.stateHashBytes-stateHashBytes)/frames/(1<<20));
      stateHashLookups=ss.stateHashLookups;
      stateHashMemoHits=ss.stateHashMemoHits;stateHashBytes=ss.stateHashBytes;
      uint64_t currentBatchSubmissions=draw_batch_submissions();
      LOG("[vulkan draw batches] %.2f extra submissions/frame",
          (currentBatchSubmissions-batchSubmissions)/frames);
      batchSubmissions=currentBatchSubmissions;
      LOG("[vulkan vertex snapshots] declared %.2f MiB/frame; prepared %.2f MiB/frame",
          (R.vertexDeclaredBytes-vertexDeclaredBytes)/frames/(1<<20),
          (R.vertexCopiedBytes-vertexCopiedBytes)/frames/(1<<20));
      vertexDeclaredBytes=R.vertexDeclaredBytes;vertexCopiedBytes=R.vertexCopiedBytes;
      LOG("[vulkan vertex binds] %.1f actual/frame %.1f skipped/frame",
          (R.vertexBindCalls-vertexBindCalls)/frames,
          (R.vertexBindSkips-vertexBindSkips)/frames);
      vertexBindCalls=R.vertexBindCalls;vertexBindSkips=R.vertexBindSkips;
      LOG("[vulkan vertex history] %.1f requests/frame %.1f nonconsecutive keys/frame %.3f MiB potential/frame; distances 1..7 %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
          (R.vertexHistoryRequests-vertexHistoryRequests)/frames,
          (R.vertexHistoryMatches-vertexHistoryMatches)/frames,
          (R.vertexHistoryBytes-vertexHistoryBytes)/frames/(1<<20),
          (R.vertexHistoryDistances[0]-vertexHistoryDistances[0])/frames,
          (R.vertexHistoryDistances[1]-vertexHistoryDistances[1])/frames,
          (R.vertexHistoryDistances[2]-vertexHistoryDistances[2])/frames,
          (R.vertexHistoryDistances[3]-vertexHistoryDistances[3])/frames,
          (R.vertexHistoryDistances[4]-vertexHistoryDistances[4])/frames,
          (R.vertexHistoryDistances[5]-vertexHistoryDistances[5])/frames,
          (R.vertexHistoryDistances[6]-vertexHistoryDistances[6])/frames);
      vertexHistoryRequests=R.vertexHistoryRequests;vertexHistoryMatches=R.vertexHistoryMatches;
      vertexHistoryBytes=R.vertexHistoryBytes;vertexHistoryDistances=R.vertexHistoryDistances;
      LOG("[vulkan vertex history reuse] %.1f checks/frame %.1f hits/frame %.3f MiB reused/frame",
          (R.vertexHistoryReuseChecks-vertexHistoryReuseChecks)/frames,
          (R.vertexHistoryReuseHits-vertexHistoryReuseHits)/frames,
          (R.vertexHistoryReuseBytes-vertexHistoryReuseBytes)/frames/(1<<20));
      vertexHistoryReuseChecks=R.vertexHistoryReuseChecks;
      vertexHistoryReuseHits=R.vertexHistoryReuseHits;vertexHistoryReuseBytes=R.vertexHistoryReuseBytes;
      LOG("[vulkan vertex reuse] %.1f checks/frame; %.1f hits/frame; %.2f MiB reused/frame; compare %.3f ms/frame",
          (R.vertexReuseChecks-vertexReuseChecks)/frames,
          (R.vertexReuseHits-vertexReuseHits)/frames,
          (R.vertexReuseBytes-vertexReuseBytes)/frames/(1<<20),
          (R.vertexReuseCompareNs-vertexReuseCompareNs)/frames/1e6);
      vertexReuseChecks=R.vertexReuseChecks;vertexReuseHits=R.vertexReuseHits;
      vertexReuseBytes=R.vertexReuseBytes;vertexReuseCompareNs=R.vertexReuseCompareNs;
      uint64_t descriptorChecks=R.descriptorLookups-descriptorLookups;
      uint64_t descriptorReuses=R.descriptorCacheHits-descriptorHits;
      LOG("[vulkan descriptors] %.1f lookups/frame; %.1f allocations/frame; %.1f%% reused; %.1f%% consecutive",
          descriptorChecks/frames,(R.descriptorAllocations-descriptorAllocations)/frames,
          descriptorChecks ? 100.0*descriptorReuses/descriptorChecks : 0.0,
          descriptorChecks ? 100.0*(R.descriptorFastHits-descriptorFastHits)/descriptorChecks : 0.0);
      descriptorLookups=R.descriptorLookups;descriptorHits=R.descriptorCacheHits;
      descriptorAllocations=R.descriptorAllocations;descriptorFastHits=R.descriptorFastHits;
      const auto& prep=R.cpuPreparation;
      LOG("[vulkan state reuse] skipped/frame: fetch %.1f shader pairs %.1f targets %.1f textures %.1f views %.1f pipelines %.1f",
          (prep.fetchSkips-previousPreparation.fetchSkips)/frames,
          (prep.shaderSkips-previousPreparation.shaderSkips)/frames,
          (prep.targetSkips-previousPreparation.targetSkips)/frames,
          (prep.textureSkips-previousPreparation.textureSkips)/frames,
          (prep.viewSkips-previousPreparation.viewSkips)/frames,
          (prep.pipelineSkips-previousPreparation.pipelineSkips)/frames);
      const auto sparse=sparse_hash_stats();
      LOG("[vulkan sparse hash memo] %.1f checks/frame %.1f hits/frame %.3f MiB fresh samples/frame %.1f mixed words/frame %.1f overflow/frame",
          (sparse.checks-previousSparse.checks)/frames,
          (sparse.memoHits-previousSparse.memoHits)/frames,
          (sparse.sampleBytes-previousSparse.sampleBytes)/frames/(1<<20),
          (sparse.mixerWords-previousSparse.mixerWords)/frames,
          (sparse.overflows-previousSparse.overflows)/frames);
      previousSparse=sparse;
      gx2::log_bump_regs(uint64_t(frames));
      const auto dirty=gx2::shader_key_dirty_stats();
      LOG("[vulkan shader key dirty] %.1f changed batches/frame %.1f baseline bumps/frame %.1f actual bumps/frame %.1f avoided/frame %.1f masked words/frame",
          (dirty.changedBatches-previousShaderDirty.changedBatches)/frames,
          (dirty.baselineWouldBumps-previousShaderDirty.baselineWouldBumps)/frames,
          (dirty.actualBumps-previousShaderDirty.actualBumps)/frames,
          (dirty.avoidedBumps-previousShaderDirty.avoidedBumps)/frames,
          (dirty.maskedWords-previousShaderDirty.maskedWords)/frames);
      previousShaderDirty=dirty;
      LOG("[vulkan sampler memo] %.1f requests/frame %.1f hits/frame %.1f map lookups/frame",
          (prep.samplerRequests-previousPreparation.samplerRequests)/frames,
          (prep.samplerMemoHits-previousPreparation.samplerMemoHits)/frames,
          (prep.samplerMapLookups-previousPreparation.samplerMapLookups)/frames);
      LOG("[vulkan uniform preparation] %.1f packs/frame %.3f MiB/frame; %llu scratch growths %.3f KiB added; %.1f snapshot requests/frame %.3f MiB/frame",
          (prep.packCalls-previousPreparation.packCalls)/frames,
          (prep.packBytes-previousPreparation.packBytes)/frames/(1<<20),
          (unsigned long long)(prep.packCapacityGrowths-previousPreparation.packCapacityGrowths),
          (prep.packCapacityGrowthBytes-previousPreparation.packCapacityGrowthBytes)/1024.0,
          (prep.uniformSnapshotCalls-previousPreparation.uniformSnapshotCalls)/frames,
          (prep.uniformSnapshotBytes-previousPreparation.uniformSnapshotBytes)/frames/(1<<20));
      LOG("[vulkan uniform reuse] %.1f checks/frame %.1f comparisons/frame %.1f hits/frame %.3f MiB avoided/frame",
          (prep.uniformReuseChecks-previousPreparation.uniformReuseChecks)/frames,
          (prep.uniformReuseComparisons-previousPreparation.uniformReuseComparisons)/frames,
          (prep.uniformReuseHits-previousPreparation.uniformReuseHits)/frames,
          (prep.uniformReuseBytes-previousPreparation.uniformReuseBytes)/frames/(1<<20));
      const uint64_t candidates=prep.descriptorBindCandidates-previousPreparation.descriptorBindCandidates;
      const double percentage=candidates?100.0/candidates:0.0;
      LOG("[vulkan bind opportunities] %.1f candidates/frame; exact whole %.2f%% VS %.2f%% PS %.2f%%; %.1f actual binds/frame %.1f skipped sets/frame",
          candidates/frames,(prep.descriptorWholeMatches-previousPreparation.descriptorWholeMatches)*percentage,
          (prep.descriptorStageMatches[0]-previousPreparation.descriptorStageMatches[0])*percentage,
          (prep.descriptorStageMatches[1]-previousPreparation.descriptorStageMatches[1])*percentage,
          (prep.descriptorBindCalls-previousPreparation.descriptorBindCalls)/frames,
          (prep.descriptorSkippedSets-previousPreparation.descriptorSkippedSets)/frames);
      LOG("[vulkan pipeline lookup] %.1f requests/frame; %.1f last hits; %.1f lookaside hits; %.1f map lookups",
          (prep.pipelineLookups-previousPreparation.pipelineLookups)/frames,
          (prep.pipelineLastHits-previousPreparation.pipelineLastHits)/frames,
          (prep.pipelineLookasideHits-previousPreparation.pipelineLookasideHits)/frames,
          (prep.pipelineMapLookups-previousPreparation.pipelineMapLookups)/frames);
      previousPreparation=prep;
      LOG("[vulkan waits] submit %.2f/frame %.2f ms/frame; present submit %.2f/frame %.2f ms/frame",
          submitWait.count/frames, submitWait.ns/1e6/frames,
          presentSubmitWait.count/frames, presentSubmitWait.ns/1e6/frames);
      for (int screen = 0; screen < 2; ++screen) {
        auto& t = screenTiming[screen];
        LOG("[vulkan waits %s] acquire %.2f/frame %.2f ms/frame; present %.2f/frame %.2f ms/frame; queue idle %.2f/frame %.2f ms/frame",
            screen ? "DRC" : "TV", t.acquire.count/frames, t.acquire.ns/1e6/frames,
            t.present.count/frames, t.present.ns/1e6/frames,
            t.idle.count/frames, t.idle.ns/1e6/frames);
        t = {};
      }
      submitWait = {}; presentSubmitWait = {};
      }
      start=now;frame=R.frame;draws=R.drawCount;bytes=R.uploadBytes;allocs=R.uploadAllocations;passes=R.renderPassCount;
    }
  }
}
void set_tv_format(uint32_t format, bool tv) {
  (tv ? R.tv : R.drc).srgb = (format & 0x400) != 0;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  fprintf(stderr, "[vulkan validation %s] %s\n",
          severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
          data->pMessage);
  return VK_FALSE;
}
// Test aids for the paths of older drivers (docs/vulkan.md): WWHD_VK_FORCE_API=1.2 treats every GPU as
// if it reported that Vulkan version (Vulkan 1.3 GPUs then take the VK_KHR_dynamic_rendering path);
// WWHD_VK_HIDE_EXTENSIONS=VK_KHR_dynamic_rendering,... hides device extensions from the renderer.
static uint32_t device_api_version(const VkPhysicalDeviceProperties &p) {
  static const uint32_t forced = [] {
    unsigned major, minor;
    const char *e = getenv("WWHD_VK_FORCE_API");
    return e && sscanf(e, "%u.%u", &major, &minor) == 2 ? VK_MAKE_API_VERSION(0, major, minor, 0) : ~0u;
  }();
  return forced < VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), 0)
             ? forced : p.apiVersion;
}
static std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> des(n);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &n, des.data());
  des.resize(n);
  if (const char *hide = getenv("WWHD_VK_HIDE_EXTENSIONS"))
    std::erase_if(des, [&](const VkExtensionProperties &e) {
      for (const char *p = hide; *p;) {
        size_t len = strcspn(p, ",");
        if (len == strlen(e.extensionName) && !strncmp(p, e.extensionName, len))
          return true;
        p += len + (p[len] == ',');
      }
      return false;
    });
  return des;
}
static std::string version_text(uint32_t v) {
  return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
         std::to_string(VK_API_VERSION_PATCH(v));
}
// driverVersion: vendor-specific packing (NVIDIA, Intel on Windows); others use Vulkan's
static std::string driver_version_text(const VkPhysicalDeviceProperties &p) {
  const uint32_t v = p.driverVersion;
  if (p.vendorID == 0x10DE)
    return std::to_string(v >> 22) + "." + std::to_string((v >> 14) & 0xff) + "." +
           std::to_string((v >> 6) & 0xff) + "." + std::to_string(v & 0x3f);
#ifdef _WIN32
  if (p.vendorID == 0x8086)
    return std::to_string(v >> 14) + "." + std::to_string(v & 0x3fff);
#endif
  return version_text(v);
}
// How a GPU provides dynamic rendering, the renderer's only way of drawing: Vulkan 1.3 core, or
// VK_KHR_dynamic_rendering on a Vulkan 1.1 / 1.2 driver. Shaders are SPIR-V 1.3 (Vulkan 1.1).
enum class DynamicRendering { None, Core, KHR };
static DynamicRendering dynamic_rendering(VkPhysicalDevice device, const VkPhysicalDeviceProperties &properties,
                                          const std::vector<VkExtensionProperties> &des) {
  const uint32_t api = device_api_version(properties);
  if (api < VK_API_VERSION_1_1)
    return DynamicRendering::None;
  if (api >= VK_API_VERSION_1_3) {
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(device, &f2);
    if (f13.dynamicRendering)
      return DynamicRendering::Core;
  }
  // the extension requires VK_KHR_depth_stencil_resolve (core in 1.2), which requires
  // VK_KHR_create_renderpass2 (core in 1.2)
  if (!has_extension(des, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
      (api < VK_API_VERSION_1_2 && (!has_extension(des, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) ||
                                    !has_extension(des, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))))
    return DynamicRendering::None;
  VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &fdr;
  vkGetPhysicalDeviceFeatures2(device, &f2);
  return fdr.dynamicRendering ? DynamicRendering::KHR : DynamicRendering::None;
}
static const char kUpdateDriver[] =
#ifdef _WIN32
    "Install the newest graphics driver for this GPU (from the AMD, Intel or NVIDIA website, or through "
    "Windows Update) and start the game again. "
#elif defined(__APPLE__)
    "Vulkan on a Mac needs MoltenVK and the Vulkan loader: brew install vulkan-loader molten-vk. "
#else
    "Install the newest graphics driver for this GPU (on Linux: an up-to-date Mesa, or the vendor's "
    "driver) and start the game again. "
#endif
    "If no newer driver exists, this GPU cannot run the game's Vulkan renderer.";
// The Vulkan layers the loader offers, once in the log (so also in a crash log's last lines).
// Overlays install implicit layers that load into every Vulkan program (Steam, Discord, RivaTuner,
// OBS, Overwolf, ReShade...); a crash inside one then shows up in the crash log's module names, and
// this line says which were around. The list holds the explicit layers as well; implicit ones are
// active unless their disable variable is set.
static void log_instance_layers() {
  if (!vkEnumerateInstanceLayerProperties)
    return;
  uint32_t count = 0;
  if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
    return;
  std::vector<VkLayerProperties> layers(count);
  if (count && vkEnumerateInstanceLayerProperties(&count, layers.data()) < VK_SUCCESS)
    return;
  layers.resize(count);
  // one log line holds 240 characters: several lines of up to 200 (each layer name is <= 256)
  std::string line;
  int lines = 0;
  for (const VkLayerProperties &l : layers) {
    char one[300];
    snprintf(one, sizeof one, "%s%s (%u.%u.%u)", line.empty() ? "" : ", ", l.layerName,
             VK_API_VERSION_MAJOR(l.specVersion), VK_API_VERSION_MINOR(l.specVersion),
             VK_API_VERSION_PATCH(l.specVersion));
    if (!line.empty() && line.size() + strlen(one) > 200) {
      LOG("[vulkan] layers: %s,", line.c_str());
      line.clear();
      if (++lines == 4) {
        LOG("[vulkan] layers: ... %u in all", count);
        return;
      }
      snprintf(one, sizeof one, "%s (%u.%u.%u)", l.layerName, VK_API_VERSION_MAJOR(l.specVersion),
               VK_API_VERSION_MINOR(l.specVersion), VK_API_VERSION_PATCH(l.specVersion));
    }
    line += one;
  }
  LOG("[vulkan] layers: %s", count ? line.c_str() : "none");
}

// Instance, device, submission slots and swapchains. `extensions`: the window system's instance
// extensions; `create_surfaces` makes R.tv.surface / R.drc.surface once the instance exists.
// The host has loaded the Vulkan loader's global functions (load_global_functions).
static void init_device(std::vector<const char *> extensions,
                        const std::function<void()> &create_surfaces) {
  reset_pipeline_lookup_cache();
#ifdef __APPLE__
  // This draw-heavy workload is faster with direct Metal resource bindings.
  // Respect an explicit MoltenVK override for devices/scenes needing larger
  // argument-buffer resource limits. Set before the loader loads the driver.
  setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
  // Immediate encoding overlaps recording with submitted GPU work. Every
  // recording entrypoint has a host autorelease pool; explicit overrides win.
  setenv("MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS", "3", 0);
#endif
  uint32_t n;
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion)
    vkEnumerateInstanceVersion(&loaderVersion);
  if (loaderVersion < VK_API_VERSION_1_1)
    throw std::runtime_error("The Vulkan runtime on this computer supports only Vulkan " +
                             version_text(loaderVersion) + "; the game needs Vulkan 1.1 or newer.\n\n" +
                             kUpdateDriver);
  log_instance_layers();
  uint32_t en = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
  std::vector<VkExtensionProperties> ies(en);
  vkEnumerateInstanceExtensionProperties(nullptr, &en, ies.data());
  if (!has_extension(ies, VK_KHR_SURFACE_EXTENSION_NAME))
    throw std::runtime_error("No Vulkan driver found (MoltenVK on a Mac: brew install molten-vk; "
                             "or point VK_DRIVER_FILES at its MoltenVK_icd.json)");
  for (const char *e : extensions)
    if (!has_extension(ies, e))
      throw std::runtime_error(std::string("the Vulkan driver lacks instance extension ") + e);
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  if (has_extension(ies, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  VkDebugUtilsMessengerCreateInfoEXT debug{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  const char *layer = "VK_LAYER_KHRONOS_validation";
  if (getenv("WWHD_VK_VALIDATION")) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = &layer;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debug_message;
    ci.pNext = &debug;
  }
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "WWHD Vulkan";
  ai.apiVersion = VK_API_VERSION_1_3;
  ci.pApplicationInfo = &ai;
  ci.enabledExtensionCount = extensions.size();
  ci.ppEnabledExtensionNames = extensions.data();
  vk_check(vkCreateInstance(&ci, nullptr, &R.instance),
           "create Vulkan instance");
  load_instance_functions(R.instance);
  if (getenv("WWHD_VK_VALIDATION")) {
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        R.instance, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT messenger;
    if (create)
      vk_check(create(R.instance, &debug, nullptr, &messenger),
               "create validation messenger");
  }
  create_surfaces();
  vkEnumeratePhysicalDevices(R.instance, &n, nullptr);
  std::vector<VkPhysicalDevice> devices(n);
  vkEnumeratePhysicalDevices(R.instance, &n, devices.data());
  devices.resize(n);
  // First the GPUs with Vulkan 1.3 (in the driver's order), then those with VK_KHR_dynamic_rendering
  std::string unsuitable;  // the GPUs passed over, for the message when none is left
  for (DynamicRendering want : {DynamicRendering::Core, DynamicRendering::KHR}) {
    for (auto device : devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(device, &properties);
      const DynamicRendering have = dynamic_rendering(device, properties, device_extensions(device));
      if (have != want) {
        if (want == DynamicRendering::KHR && have == DynamicRendering::None)
          unsuitable += std::string("\n") + properties.deviceName + ": Vulkan " +
                        version_text(device_api_version(properties)) + ", driver " +
                        driver_version_text(properties) +
                        (device_api_version(properties) < VK_API_VERSION_1_1 ? "; older than Vulkan 1.1"
                                                                             : "; VK_KHR_dynamic_rendering missing");
        continue;
      }
      uint32_t qn;
      vkGetPhysicalDeviceQueueFamilyProperties(device, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(device, &qn, qs.data());
      for (uint32_t q = 0; q < qn; q++) {
        VkBool32 tv = 0, drc = VK_TRUE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, q, R.tv.surface, &tv);
        if (R.drc.surface)
          vkGetPhysicalDeviceSurfaceSupportKHR(device, q, R.drc.surface, &drc);
        if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && tv && drc) {
          R.physicalDevice = device;
          R.queueFamily = q;
          R.gpuTimestampValidBits = qs[q].timestampValidBits;
          R.dynamicRenderingKHR = have == DynamicRendering::KHR;
          break;
        }
      }
      if (R.physicalDevice)
        break;
      unsuitable += std::string("\n") + properties.deviceName + ": cannot draw to the game window";
    }
    if (R.physicalDevice)
      break;
  }
  if (!R.physicalDevice)
    throw std::runtime_error(
        devices.empty()
            ? std::string("No graphics card with a Vulkan driver was found.\n\n") + kUpdateDriver
            : "The graphics driver does not support the Vulkan features the game needs: Vulkan 1.3, or "
              "Vulkan 1.1 / 1.2 with the VK_KHR_dynamic_rendering extension.\n" + unsuitable +
                  "\n\n" + kUpdateDriver);
  vkGetPhysicalDeviceProperties(R.physicalDevice, &R.properties);
  const uint32_t deviceApi = device_api_version(R.properties);
  LOG("[vulkan] device: %s (Vulkan %s, driver %s%s)", R.properties.deviceName,
      version_text(R.properties.apiVersion).c_str(), driver_version_text(R.properties).c_str(),
      R.dynamicRenderingKHR ? "; dynamic rendering through VK_KHR_dynamic_rendering" : "");
  if (perf_enabled()) {
    const auto& limits=R.properties.limits;
    LOG("[vulkan uniforms] dynamic/set %u; uniform/stage %u; uniform/set %u; resources/stage %u; alignment %llu; range %u",
        limits.maxDescriptorSetUniformBuffersDynamic,limits.maxPerStageDescriptorUniformBuffers,
        limits.maxDescriptorSetUniformBuffers,limits.maxPerStageResources,
        (unsigned long long)limits.minUniformBufferOffsetAlignment,limits.maxUniformBufferRange);
  }
  const std::vector<VkExtensionProperties> des = device_extensions(R.physicalDevice);
  std::vector<const char *> de{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  if (R.dynamicRenderingKHR) {
    de.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    if (deviceApi < VK_API_VERSION_1_2) {
      de.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
      de.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    }
  }
#ifdef __SWITCH__
  const bool hostImport = has_extension(des, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
  if (hostImport) de.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
#endif
  if (has_extension(des, "VK_KHR_portability_subset"))
    de.push_back("VK_KHR_portability_subset");
  R.portabilitySubset =
      has_extension(des, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
  VkPhysicalDevicePortabilitySubsetFeaturesKHR portability{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
  VkPhysicalDeviceVulkan12Features available12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  // VkPhysicalDeviceVulkan12Features: Vulkan 1.2 devices and newer
  const bool vulkan12 = deviceApi >= VK_API_VERSION_1_2;
  void **tail = &features.pNext;
  if (vulkan12) {
    *tail = &available12;
    tail = &available12.pNext;
  }
  if (R.portabilitySubset)
    *tail = &portability;
  vkGetPhysicalDeviceFeatures2(R.physicalDevice, &features);
  if (R.portabilitySubset) {
    R.imageViewSwizzle = portability.imageViewFormatSwizzle;
    R.imageViewReinterpretation = portability.imageViewFormatReinterpretation;
    R.samplerMipLodBias = portability.samplerMipLodBias;
    R.separateStencilMaskRef = portability.separateStencilMaskRef;
    R.constantAlphaColorBlendFactors =
        portability.constantAlphaColorBlendFactors;
    R.vertexAttributeAccessBeyondStride =
        portability.vertexAttributeAccessBeyondStride;
  }
  VkPhysicalDeviceVulkan12Features enabled12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  enabled12.samplerMirrorClampToEdge = available12.samplerMirrorClampToEdge;
  if (!vulkan12 && has_extension(des, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) {
    de.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);  // Vulkan 1.1: the extension
    enabled12.samplerMirrorClampToEdge = VK_TRUE;
  }
  R.samplerMirrorClampToEdge = enabled12.samplerMirrorClampToEdge;
  VkPhysicalDeviceFeatures available;
  vkGetPhysicalDeviceFeatures(R.physicalDevice, &available);
  VkPhysicalDeviceFeatures enabled{};
  enabled.samplerAnisotropy = available.samplerAnisotropy;
  enabled.independentBlend = available.independentBlend;
  enabled.depthClamp = available.depthClamp;
  enabled.depthBiasClamp = available.depthBiasClamp;
  enabled.shaderClipDistance = available.shaderClipDistance;
  enabled.shaderCullDistance = available.shaderCullDistance;
  enabled.fillModeNonSolid = available.fillModeNonSolid;
  enabled.largePoints = available.largePoints;
  enabled.dualSrcBlend = available.dualSrcBlend;
  enabled.logicOp = available.logicOp;
  R.enabledFeatures = enabled;
  VkPhysicalDeviceVulkan13Features f13{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
  f13.dynamicRendering = fdr.dynamicRendering = VK_TRUE;
  void *chain = R.dynamicRenderingKHR ? static_cast<void *>(&fdr) : &f13;
  tail = R.dynamicRenderingKHR ? &fdr.pNext : &f13.pNext;
  if (vulkan12) {
    *tail = &enabled12;
    tail = &enabled12.pNext;
  }
  if (R.portabilitySubset)
    *tail = &portability;
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = R.queueFamily;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.pNext = chain;
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.pEnabledFeatures = &enabled;
  di.enabledExtensionCount = de.size();
  di.ppEnabledExtensionNames = de.data();
  vk_check(vkCreateDevice(R.physicalDevice, &di, nullptr, &R.device),
           "create Vulkan device");
  load_device_functions(R.device, R.dynamicRenderingKHR);
  vkGetDeviceQueue(R.device, R.queueFamily, 0, &R.queue);
#ifdef __SWITCH__
  if (hostImport) import_guest_mem2();
#endif
  init_pipeline_cache();
  for (auto& slot:R.submissions) {
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.queueFamilyIndex = R.queueFamily;
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  vk_check(vkCreateCommandPool(R.device, &pool, nullptr, &slot.commandPool),
           "create command pool");
  VkCommandBufferAllocateInfo ca{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = slot.commandPool;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  vk_check(vkAllocateCommandBuffers(R.device, &ca, &slot.cmd),
           "allocate command buffer");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vk_check(vkCreateFence(R.device, &fi, nullptr, &slot.fence),
           "create frame fence");
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp.maxSets = 32768;
  dp.poolSizeCount = std::size(sizes);
  dp.pPoolSizes = sizes;
  vk_check(vkCreateDescriptorPool(R.device, &dp, nullptr, &slot.descriptorPool),
           "create descriptor pool");
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (size_t screen=0;screen<slot.acquired.size();++screen)
    if ((screen ? R.drc : R.tv).window)
      vk_check(vkCreateSemaphore(R.device,&si,nullptr,&slot.acquired[screen]),
               "create acquire semaphore");
  }
  init_gpu_timestamp_queries();
  activate_submission(0);
  for (Screen *s : {&R.tv, &R.drc})
    if (s->window) make_swapchain(*s);
  vk::select_renderer();
  set_graphics_feature_available(GraphicsFeature::AO);
  set_graphics_feature_available(GraphicsFeature::AOHires);
  set_graphics_feature_available(GraphicsFeature::Anisotropy,
                                 R.enabledFeatures.samplerAnisotropy);
  if (const char* path = getenv("WWHD_CAPTURE_PATH")) {
    const char* frame = getenv("WWHD_CAPTURE");
    request_tv_dump(path, frame ? std::max(0, atoi(frame)) : 120);
  }
}

#ifdef WWHD_SDL_HOST
static bool hidden_windows() {
  static const bool hidden = [] { const char* e = getenv("WWHD_HIDDEN_WINDOWS"); return e && *e && strcmp(e, "0"); }();
  return hidden;
}
// The game's own icon on the windows (title bar, taskbar): meta/iconTex.tga of the game folder, an
// uncompressed 32-bit TGA (128x128 in Wind Waker HD). Nothing happens without it.
static void set_window_icons() {
  FILE *f = std::fopen((config::game_dir + "/meta/iconTex.tga").c_str(), "rb");
  if (!f)
    return;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
    d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  if (d.size() < 18 || d[1] != 0 || d[2] != 2 || d[16] != 32)  // no colour map, true colour, 32 bpp
    return;
  const uint32_t w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, start = 18 + d[0];
  if (!w || !h || w > 1024 || h > 1024 || d.size() < start + size_t(w) * h * 4)
    return;
  SDL_Surface *icon = SDL_CreateSurface(int(w), int(h), SDL_PIXELFORMAT_BGRA32);
  if (!icon)
    return;
  const bool topDown = d[17] & 0x20;  // otherwise the first row is the bottom one
  for (uint32_t y = 0; y < h; y++)
    std::memcpy(static_cast<uint8_t *>(icon->pixels) + size_t(y) * icon->pitch,
                d.data() + start + size_t(topDown ? y : h - 1 - y) * w * 4, size_t(w) * 4);
  for (Screen *s : {&R.tv, &R.drc})
    if (s->window)
      SDL_SetWindowIcon(s->window, icon);
  SDL_DestroySurface(icon);
}
// SDL host (Vulkan-only builds): SDL windows, input and audio
void init() {
#ifdef __ANDROID__
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
  // hidden test runs: no Dock icon, no activation (the app never takes the focus from the user)
  if (hidden_windows())
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
    throw std::runtime_error(SDL_GetError());
  // the Vulkan loader (vulkan-1.dll, libvulkan.so.1), loaded here rather than imported (loader.h); the
  // windows below use the same one
#ifndef __SWITCH__  // the Switch links its Vulkan driver (loader.h)
  if (!SDL_Vulkan_LoadLibrary(nullptr))
    throw std::runtime_error(std::string("Vulkan is not installed on this computer: the Vulkan runtime "
#ifdef _WIN32
                                         "(vulkan-1.dll) "
#endif
                                         "could not be loaded (") + SDL_GetError() + ").\n\n" + kUpdateDriver);
  load_global_functions(reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
#endif
#ifdef __ANDROID__
  SDL_AddEventWatch(lifecycle_watch, nullptr);
  const SDL_WindowFlags windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN;
#else
  // test runs: WWHD_HIDDEN_WINDOWS=1 never puts the windows on screen (nothing pops up or takes the
  // focus); the swapchains still exist, so frame dumps and present dumps work as with visible windows
  const SDL_WindowFlags windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                      (hidden_windows() ? SDL_WINDOW_HIDDEN : 0);
#endif
  R.tv.window = SDL_CreateWindow("Wind Waker HD — Vulkan", 1280, 720, windowFlags);
  if (!R.tv.window)
    throw std::runtime_error(SDL_GetError());
#ifdef __ANDROID__
  // one surface: the GamePad picture is drawn into it (display_modes.h: picture-in-picture, GamePad
  // only), never a window of its own
#else
  if (!getenv("WWHD_NO_GAMEPAD")) {
    // made hidden: the GamePad screen mode (load_saved_options below) shows it in window mode only;
    // the other modes draw the GamePad picture into the TV window (gfx/display_modes.h)
    R.drc.window = SDL_CreateWindow("GamePad — Vulkan", 854, 480, windowFlags | SDL_WINDOW_HIDDEN);
    if (!R.drc.window)
      throw std::runtime_error(SDL_GetError());
    R.drc.visible = false;
    gfx::g_has_drc_window = true;
  }
#endif
  if (hidden_windows())
    R.tv.visible = R.drc.visible = false;  // no drawables: pictures only reach frame / present dumps
  set_window_icons();
  uint32_t n;
  const char *const *se = SDL_Vulkan_GetInstanceExtensions(&n);
  if (!se)
    throw std::runtime_error(SDL_GetError());
  for (Screen *s : {&R.tv, &R.drc})
    if (s->window) {
      int width = 0, height = 0;
      if (!SDL_GetWindowSizeInPixels(s->window, &width, &height))
        throw std::runtime_error(SDL_GetError());
      s->width = width;
      s->height = height;
    }
  init_device(std::vector<const char *>(se, se + n), [] {
    if (!SDL_Vulkan_CreateSurface(R.tv.window, R.instance, nullptr,
                                  &R.tv.surface))
      throw std::runtime_error(SDL_GetError());
    if (R.drc.window && !SDL_Vulkan_CreateSurface(R.drc.window, R.instance,
                                                  nullptr, &R.drc.surface))
      throw std::runtime_error(SDL_GetError());
  });
  mods::mouse_init(R.tv.window);
  input::set_prompt_window(R.tv.window);
  input::init();
  install_graphics_menu(R.tv.window);
  ::hostui::load_saved_options();  // graphics and GamePad screen options saved by the settings overlay (settings.ini)
}
#endif
void save_renderer_caches() {
  // Called during orderly shutdown under the renderer execution lock.
  reset_feedback_images();
  wait_idle();
  reset_surface_image_pool();
  destroy_gpu_timestamp_queries();
  vk::save_shader_cache();
  save_pipeline_cache();
}
#ifdef WWHD_SDL_HOST
// GamePad touch screen: the left mouse button on the GamePad picture, in the GamePad window (window
// mode; the picture fitted as display_layout fits it) or inside the TV window (picture-in-picture,
// GamePad only: display_modes.cpp maps the point as on macOS), mapped to 0..1 touch coordinates. A
// press that starts on the picture keeps touching while it is dragged, clamped to the picture's edge.
// The settings overlay keeps its clicks in the TV window.
enum class TouchIn { None, DrcWindow, TvWindow };
static TouchIn touchHeld = TouchIn::None;
static bool drc_window_point(float x, float y, float &tx, float &ty) {
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(R.drc.window, &pw, &ph);
  const float density = SDL_GetWindowPixelDensity(R.drc.window);
  const float sw = R.drc.scan ? float(R.drc.scan->extent.width) : 854.0f;
  const float sh = R.drc.scan ? float(R.drc.scan->extent.height) : 480.0f;
  const gfx::Box b = gfx::display_layout(float(pw), float(ph), sw, sh);
  if (b.w <= 0 || b.h <= 0) return false;
  tx = (x * density - b.x) / b.w;
  ty = (y * density - b.y) / b.h;
  return tx >= 0 && tx <= 1 && ty >= 0 && ty <= 1;
}
// a point of the TV window (window coordinates) as 0..1 from its top left
static void tv_window_point(float x, float y, float &nx, float &ny) {
  int w = 0, h = 0;
  SDL_GetWindowSize(R.tv.window, &w, &h);
  nx = w > 0 ? x / float(w) : 0;
  ny = h > 0 ? y / float(h) : 0;
}
//
// Touch screens (display_modes.h's view button, on by default on Android): a tap on the button
// switches to the next view (picture-in-picture, GamePad only, TV only), a long press (0.6 s)
// switches 60 fps on Android. On Android fingers are read as fingers (several at once, one of them
// touching the GamePad); the mouse events SDL makes from them only reach the settings overlay.
static uint64_t buttonDown = 0;  // SDL_GetTicks() of the press on the view button, 0: none
static void view_button_released() {
  if (SDL_GetTicks() - buttonDown >= 600) {
#ifdef __ANDROID__
    // the choice only: platform/perf_hint.cpp switches interpolation where the phone keeps up
    perf_hint::set_fps60_chosen(!perf_hint::fps60_chosen());
    ::hostui::graphics_changed();  // saved with the graphics options (fps60)
#endif
  } else {
    ::hostui::set_drc_mode(gfx::next_view());
  }
  buttonDown = 0;
}
#ifdef __ANDROID__
static bool synthHeld = false;                 // a mouse press SDL made from a finger, kept from the game
static SDL_FingerID touchFinger = 0, buttonFinger = 0;  // 0: none
static bool finger_touch(const SDL_Event& event) {
  const SDL_TouchFingerEvent& f = event.tfinger;
  if (f.windowID != SDL_GetWindowID(R.tv.window)) return false;
  float tx = 0, ty = 0;
  switch (event.type) {
  case SDL_EVENT_FINGER_DOWN:
    if (input::touch_from_controller(f.touchID)) return true;  // a controller's own touch pad / buttons
    if (overlay::captures()) return false;
    if (!buttonFinger && gfx::view_button_hit(f.x, f.y)) {
      buttonFinger = f.fingerID;
      buttonDown = std::max<uint64_t>(SDL_GetTicks(), 1);
      return true;
    }
    if (touchFinger || !gfx::overlay_hit(f.x, f.y, &tx, &ty)) return false;
    touchFinger = f.fingerID;
    gfx::display_touched();
    input::set_touch(true, tx, ty);
    return true;
  case SDL_EVENT_FINGER_MOTION:
    if (!touchFinger || f.fingerID != touchFinger) return f.fingerID == buttonFinger && buttonFinger;
    if (!gfx::overlay_hit(f.x, f.y, &tx, &ty, true)) tx = ty = 0;
    input::set_touch(true, tx, ty);
    return true;
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    if (buttonFinger && f.fingerID == buttonFinger) {
      buttonFinger = 0;
      if (event.type == SDL_EVENT_FINGER_UP) view_button_released();
      buttonDown = 0;
      return true;
    }
    if (!touchFinger || f.fingerID != touchFinger) return false;
    touchFinger = 0;
    if (!gfx::overlay_hit(f.x, f.y, &tx, &ty, true)) tx = ty = 0;
    input::set_touch(false, tx, ty);
    gfx::display_touched();
    return true;
  default:
    return false;
  }
}
#endif
static bool gamepad_touch(const SDL_Event& event) {
  const SDL_WindowID drcId = R.drc.window ? SDL_GetWindowID(R.drc.window) : 0;
  const SDL_WindowID tvId = SDL_GetWindowID(R.tv.window);
  float tx = 0, ty = 0;
#ifdef __ANDROID__
  if (finger_touch(event)) return true;
  // the mouse events SDL makes from fingers: those on the GamePad picture or the button are the
  // fingers' (above) and kept from the game's mouse camera; the settings overlay gets them all
  if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
      event.button.which == SDL_TOUCH_MOUSEID) {
    if (overlay::captures() || event.button.windowID != tvId) return false;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) return std::exchange(synthHeld, false);
    float nx, ny;
    tv_window_point(event.button.x, event.button.y, nx, ny);
    synthHeld = gfx::view_button_hit(nx, ny) || gfx::overlay_hit(nx, ny, &tx, &ty);
    return synthHeld;
  }
  if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which == SDL_TOUCH_MOUSEID) return synthHeld;
#endif
  switch (event.type) {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event.button.button != SDL_BUTTON_LEFT) return false;
    if (event.button.windowID == tvId && !overlay::captures() && gfx::view_button_enabled()) {
      float nx, ny;
      tv_window_point(event.button.x, event.button.y, nx, ny);
      if (gfx::view_button_hit(nx, ny)) {
        buttonDown = std::max<uint64_t>(SDL_GetTicks(), 1);
        return true;
      }
    }
    if (drcId && event.button.windowID == drcId) {
      if (!drc_window_point(event.button.x, event.button.y, tx, ty)) return true;  // outside the picture
      touchHeld = TouchIn::DrcWindow;
    } else if (event.button.windowID == tvId && !overlay::captures()) {
      float nx, ny;
      tv_window_point(event.button.x, event.button.y, nx, ny);
      if (!gfx::overlay_hit(nx, ny, &tx, &ty)) return false;  // the game's window: mouse camera etc.
      touchHeld = TouchIn::TvWindow;
      gfx::display_touched();
    } else {
      return false;
    }
    input::set_touch(true, tx, ty);
    return true;
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    const bool up = event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (buttonDown && up && event.button.button == SDL_BUTTON_LEFT) {
      view_button_released();
      return true;
    }
    if (touchHeld == TouchIn::None || (up && event.button.button != SDL_BUTTON_LEFT)) return false;
    const float x = up ? event.button.x : event.motion.x, y = up ? event.button.y : event.motion.y;
    if (touchHeld == TouchIn::DrcWindow) {
      drc_window_point(x, y, tx, ty);
      tx = std::clamp(tx, 0.0f, 1.0f);
      ty = std::clamp(ty, 0.0f, 1.0f);
    } else {
      float nx, ny;
      tv_window_point(x, y, nx, ny);
      if (!gfx::overlay_hit(nx, ny, &tx, &ty, true)) tx = ty = 0;
      gfx::display_touched();
    }
    if (up) touchHeld = TouchIn::None;
    input::set_touch(!up, tx, ty);
    return true;
  }
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    if (touchHeld != TouchIn::None) { touchHeld = TouchIn::None; input::set_touch(false, 0, 0); }
    return false;
  default:
    return false;
  }
}

// Ctrl+G (Cmd+G on macOS, as the AppKit host's Display menu): show / hide the GamePad screen in the
// current mode (the GamePad window, or the picture-in-picture overlay; auto mode: keep it up)
static bool drc_key(const SDL_Event& event) {
  if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat || event.key.scancode != SDL_SCANCODE_G) return false;
#ifdef __APPLE__
  const SDL_Keymod want = SDL_KMOD_GUI;
#else
  const SDL_Keymod want = SDL_KMOD_CTRL;
#endif
  if (!(event.key.mod & want) || (event.key.mod & (SDL_KMOD_ALT | SDL_KMOD_SHIFT))) return false;
  ::hostui::toggle_drc();
  return true;
}
// debug: a test variable's list of TV frames ("3500,3700"); due once per listed frame
static bool test_frame_due(const std::vector<uint64_t> &frames, size_t &i) {
  if (i >= frames.size() || frame_count() < frames[i]) return false;
  i++;
  return true;
}
static std::vector<uint64_t> test_frames(const char *var) {
  std::vector<uint64_t> f;
  if (const char *e = getenv(var))
    for (const char *p = e; *p;) {
      f.push_back(strtoull(p, (char **)&p, 10));
      while (*p == ',') p++;
    }
  return f;
}
// debug: WWHD_TEST_DRC_KEY=3500,3700 simulates Ctrl+G at those frames (as display.mm's Cmd+G)
static void test_drc_key() {
  static const std::vector<uint64_t> frames = test_frames("WWHD_TEST_DRC_KEY");
  static size_t i = 0;
  if (test_frame_due(frames, i)) {
    LOG("[display] test: Ctrl+G at frame %llu", (unsigned long long)frame_count());
    ::hostui::toggle_drc();
  }
}

// full screen (the TV window's is remembered for the next start: hostui::tv_fullscreen_changed)
static void toggle_fullscreen(SDL_Window* window) {
  const bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
  const char* which = window == R.drc.window ? "GamePad window" : "TV window";
  if (!SDL_SetWindowFullscreen(window, !full))
    LOG("[display] %s: switching to %s failed: %s", which, full ? "windowed" : "full screen", SDL_GetError());
  else
    LOG("[display] %s %s", which, full ? "windowed" : "full screen");
  if (window == R.tv.window)
    ::hostui::tv_fullscreen_changed();
}
// F11 or Alt+Enter toggles the focused window (TV or GamePad)
static bool fullscreen_key(const SDL_Event& event) {
  if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) return false;
  const bool f11 = event.key.scancode == SDL_SCANCODE_F11 && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI));
  const bool altEnter = (event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_KP_ENTER) &&
                        (event.key.mod & SDL_KMOD_ALT);
  if (!f11 && !altEnter) return false;
  SDL_Window* window = SDL_GetWindowFromID(event.key.windowID);
  toggle_fullscreen(window ? window : R.tv.window);
  return true;
}
// debug: WWHD_TEST_FULLSCREEN_KEY=3500,3700 simulates F11 on the TV window at those frames (with
// WWHD_HIDDEN_WINDOWS the window stays hidden: SDL only notes the state for when it is shown)
static void test_fullscreen_key() {
  static const std::vector<uint64_t> frames = test_frames("WWHD_TEST_FULLSCREEN_KEY");
  static size_t i = 0;
  if (test_frame_due(frames, i)) {
    LOG("[display] test: F11 at frame %llu", (unsigned long long)frame_count());
    toggle_fullscreen(R.tv.window);
  }
}

// Closing the TV window ends the game. SDL only sends SDL_EVENT_QUIT once every window is closed, so
// with the GamePad window open the close button of the TV window did nothing. Closing the GamePad
// window only hides it (WWHD_NO_GAMEPAD=1 starts without it).
static void quit_game() {
  gx2::checkpoint_vulkan_caches();
  std::_Exit(0);
}
static bool close_request(const SDL_Event& event) {
  if (event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
  if (event.window.windowID == SDL_GetWindowID(R.tv.window)) quit_game();
  if (R.drc.window && event.window.windowID == SDL_GetWindowID(R.drc.window)) {
    ::hostui::drc_window_closed();  // hidden until Ctrl+G or the settings overlay shows it again
    LOG("[display] GamePad window closed (hidden); the game keeps running");
  }
  return true;
}

void run_main_loop() {
  auto titleTime = std::chrono::steady_clock::now();
  uint64_t titleFrames = gx2::flips_presented();
  // Deterministic shutdown hook for cache durability tests.
  const char* exitAt=getenv("WWHD_EXIT_AT_FRAME");
  const uint64_t exitFrame=exitAt ? std::strtoull(exitAt,nullptr,10) : 0;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) quit_game();
      if (close_request(event)) continue;
      if (fullscreen_key(event)) continue;
      if (drc_key(event)) continue;
      if (gamepad_touch(event)) continue;
      input::handle_event(event);
      for (Screen *s : {&R.tv, &R.drc})
        if (s->window && event.window.windowID == SDL_GetWindowID(s->window)) {
          if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            s->width = event.window.data1;
            s->height = event.window.data2;
            s->resize = true;
          }
          if (s == &R.tv && (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
                             event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN))
            ::hostui::tv_fullscreen_changed();  // also when the window manager switched it
          if (event.type == SDL_EVENT_WINDOW_MINIMIZED)
            s->visible = false;
          if (event.type == SDL_EVENT_WINDOW_RESTORED ||
              event.type == SDL_EVENT_WINDOW_SHOWN)
            s->visible = true;
        }
    }
    input::update();
    ::hostui::run_posted();  // option changes from the settings overlay (render thread)
    test_drc_key();
    test_fullscreen_key();
    if (const int m = gfx::display_test_mode(frame_count()); m >= 0)
      ::hostui::set_drc_mode(m);
    overlay::set_density(SDL_GetWindowPixelDensity(R.tv.window));
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - titleTime).count();
    if (elapsed >= 0.5 && !input::text_prompt_active()) {
      uint64_t frames = gx2::flips_presented();
      if (exitFrame && frame_count() >= exitFrame) {
        SDL_Event quit{};quit.type=SDL_EVENT_QUIT;SDL_PushEvent(&quit);
      }
      char title[160];
      int mode = interp::mode();
      std::snprintf(title, sizeof title,
          "The Legend of Zelda: The Wind Waker HD (Vulkan) — %.0f fps%s · %gx%s",
          double(frames - titleFrames) / elapsed,
          mode == 2 ? " · true 60" : mode == 1 ? " · 60 fps" : "",
          double(requested_res_scale()), fxaa_enabled() ? " · FXAA" : "");
      SDL_SetWindowTitle(R.tv.window, title);
      titleFrames = frames;
      titleTime = now;
    }
    SDL_Delay(1);
  }
}
#endif  // WWHD_SDL_HOST

#if defined(__APPLE__) && !defined(WWHD_SDL_HOST)
// AppKit host: the TV / GamePad windows of gfx/display.mm, shared with the Metal renderer. Each view
// has a CAMetalLayer; Vulkan presents to it through VK_EXT_metal_surface (MoltenVK).
static bool image_loaded(const char *part) {
  for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++)
    if (const char *name = _dyld_get_image_name(i))
      if (strstr(name, part))
        return true;
  return false;
}
void init_appkit(void *tvLayer, void *drcLayer) {
  if (const char *e = getenv("WWHD_VK_FORCE_INIT_FAIL"); e && *e && strcmp(e, "0"))
    throw std::runtime_error("Vulkan start-up failure forced for testing (WWHD_VK_FORCE_INIT_FAIL)");
  // weak imports (CMakeLists.txt): a Mac without them still starts the game with Metal
  if (!image_loaded("/libvulkan"))
    throw std::runtime_error("The Vulkan loader (libvulkan) is not installed. "
                             "Install it with: brew install vulkan-loader molten-vk");
  if (!image_loaded("/libglslang"))
    throw std::runtime_error("glslang (shader compiler for Vulkan) is not installed. "
                             "Install it with: brew install glslang");
  load_global_functions(reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")));
  if (!tvLayer)
    throw std::runtime_error("no TV window layer");
  R.tv.window = tvLayer;
  R.drc.window = drcLayer;
  init_device({VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME}, [&] {
    auto create = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(R.instance, "vkCreateMetalSurfaceEXT");
    if (!create)
      throw std::runtime_error("vkCreateMetalSurfaceEXT unavailable");
    for (Screen *s : {&R.tv, &R.drc}) {
      if (!s->window)
        continue;
      VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
      ci.pLayer = static_cast<const CAMetalLayer *>(s->window);
      vk_check(create(R.instance, &ci, nullptr, &s->surface), "create Metal surface");
    }
  });
  LOG("[vulkan] presenting to the AppKit windows through VK_EXT_metal_surface");
}
void screen_changed(int screen, bool visible) {
  Screen &s = screen ? R.drc : R.tv;
  s.visible = visible;
  s.resize = true;  // drawable size or backing scale may have changed
}
#endif
} // namespace gfxvk
