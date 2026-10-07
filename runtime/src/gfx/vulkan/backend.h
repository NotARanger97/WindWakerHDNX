#pragma once
#include "loader.h"
#ifdef WWHD_SDL_HOST
#include <SDL3/SDL.h>
#endif
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "formats.h"
#include "api.h"
namespace gfxvk {
struct Buffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; void* mapped=nullptr; VkDeviceSize size=0,allocationSize=0; VkMemoryPropertyFlags properties=0; };
struct UploadSlice { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceSize offset=0,size=0; void* mapped=nullptr; bool cpuReadable=true; };
struct CachedGuestLayout;
// Shared by guest surfaces with the same normalized base address. Sampling
// ranks aliases by writeSeq; rendering only tests identity/gpuWritten.
struct SurfaceAddressState { uint64_t generation=1; bool ambiguous=false; };
struct Surface {
 VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE;
 VkDeviceSize allocationBytes=0;
 VkImageType imageType=VK_IMAGE_TYPE_2D; VkImageViewType viewType=VK_IMAGE_VIEW_TYPE_2D;
 VkExtent3D extent{}; VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED; VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT; VkImageUsageFlags usage=0;
 uint32_t arrayLayers=1; std::vector<VkImageView> layerViews;
 std::unordered_map<uint32_t,VkImageView> sampledViews;
 uint32_t addr=0,mipAddr=0,width=0,height=0,slices=1,pitch=0,mips=1,format=0,dim=1,tileMode=0,swizzle=0;
 bool isDepth=false,gpuWritten=false,dirty=true;
 bool pagesWritten=false,fullCheckPending=false; // Switch: a watched guest page was flushed (sweep_texture_page_writes)
 uint64_t writeSeq=0,contentHash=0,lastCheckedFrame=~0ull,sparseHash=0;
 // CPU textures: write stamp (write_watch.h) of all levels' pages at the last full check
 uint64_t watchStamp=0; bool watched=false;
 uint32_t dataSize=0; FormatInfo fmt;
 mutable std::shared_ptr<CachedGuestLayout> guestLayout;
 std::shared_ptr<SurfaceAddressState> addressState;
 float scale=1,sx=1,sy=1;
 float ax=1,ay=1; // aspect-ratio factors of screen-shaped targets (surfaces.cpp)
};
struct SurfaceDesc { uint32_t addr=0,mipAddr=0,width=0,height=0,slices=1,pitch=0,mips=1,format=0,dim=1,tileMode=0,swizzle=0; bool isDepth=false; };
// All variable VkImageCreateInfo fields; tiling/samples/sharing are fixed.
struct SurfaceImageKey {
 VkFormat format=VK_FORMAT_UNDEFINED; VkExtent3D extent{};
 uint32_t mips=0,layers=0; VkImageUsageFlags usage=0;
 VkImageType type=VK_IMAGE_TYPE_2D; VkImageCreateFlags flags=0;
 bool operator==(const SurfaceImageKey& b) const {
  return format==b.format && extent.width==b.extent.width && extent.height==b.extent.height &&
   extent.depth==b.extent.depth && mips==b.mips && layers==b.layers && usage==b.usage && type==b.type && flags==b.flags;
 }
};
struct Screen {
#ifdef WWHD_SDL_HOST
 SDL_Window* window=nullptr;
#else
 void* window=nullptr; // CAMetalLayer of the AppKit view (display.mm)
#endif
 VkSurfaceKHR surface=VK_NULL_HANDLE; VkSwapchainKHR swapchain=VK_NULL_HANDLE;
 VkFormat swapFormat=VK_FORMAT_UNDEFINED; VkExtent2D swapExtent{};
 std::vector<VkImage> images; std::vector<VkImageLayout> layouts;
 // Present waits can outlive a graphics fence. Reuse only on reacquisition
 // of the same swapchain image, never merely on submission-slot retirement.
 std::vector<VkSemaphore> finished;
 std::unique_ptr<Surface> scan;
 std::atomic<bool> visible{true},srgb{false},resize{false};
 int presentMode=-1,presentWanted=-1; // present mode of the swapchain, and the setting it was made for
 std::atomic<int> width{1280},height{720};
};
struct GpuScopeMetadata {
 uint32_t kind=0; // 0 render pass, 1 feedback copy.
 std::array<VkFormat,8> colors{};
 VkFormat depth=VK_FORMAT_UNDEFINED;
 VkExtent3D extent{};
 uint32_t mips=0,layers=0,aspects=0,colorCount=0;
 std::array<uint32_t,8> slices{};
 uint32_t depthSlice=0;
 bool depthOnly=false;
};
struct GpuScopeToken { uint64_t generation=0; uint32_t index=UINT32_MAX; };
struct Renderer {
 // Switch: guest MEM2 (0x10000000, 1 GiB) imported as one buffer; draws bind guest vertex/index
 // data in place (gfxvk::guest_buffer_slice), like the Wii U GPU reading MEM2 directly
 VkBuffer guestMem2Buffer=VK_NULL_HANDLE; VkDeviceMemory guestMem2Memory=VK_NULL_HANDLE;
 VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physicalDevice=VK_NULL_HANDLE; VkDevice device=VK_NULL_HANDLE;
 VkPhysicalDeviceFeatures enabledFeatures{};
 bool dynamicRenderingKHR=false; // VK_KHR_dynamic_rendering (device older than Vulkan 1.3)
 bool portabilitySubset=false,imageViewSwizzle=true,imageViewReinterpretation=true;
 bool samplerMipLodBias=true,separateStencilMaskRef=true,constantAlphaColorBlendFactors=true,vertexAttributeAccessBeyondStride=true,samplerMirrorClampToEdge=false;
 VkPhysicalDeviceProperties properties{}; VkQueue queue=VK_NULL_HANDLE; uint32_t queueFamily=0;
 bool gpuTimestampsEnabled=false,gpuPassTimestampsEnabled=false;
 uint32_t gpuTimestampValidBits=0;
 struct GpuTimestampStats {
  double intervalNs=0,maxIntervalNs=0;
  uint64_t submissions=0,unavailable=0,zeroIntervals=0;
 } gpuTimestampStats;
 VkPipelineCache pipelineCache=VK_NULL_HANDLE;
 bool pipelineCacheDirty=false;
 uint64_t pipelineCacheChangedFrame=0;
 VkCommandPool commandPool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
 VkDescriptorPool descriptorPool=VK_NULL_HANDLE; bool recording=false,rendering=false;
 // Descriptor reuse must include this epoch: pool handles repeat after reset.
 uint64_t submissionGeneration=0;
 uint64_t vertexBindCalls=0,vertexBindSkips=0;
 uint64_t descriptorLookups=0,descriptorCacheHits=0,descriptorAllocations=0,descriptorFastHits=0;
 struct CpuPreparationStats {
  uint64_t fetchSkips=0,shaderSkips=0,targetSkips=0,textureSkips=0,viewSkips=0,pipelineSkips=0;
  uint64_t packCalls=0,packBytes=0,packCapacityGrowths=0,packCapacityGrowthBytes=0;
  uint64_t uniformSnapshotCalls=0,uniformSnapshotBytes=0;
  uint64_t samplerRequests=0,samplerMemoHits=0,samplerMapLookups=0;
  uint64_t uniformReuseChecks=0,uniformReuseComparisons=0,uniformReuseHits=0,uniformReuseBytes=0;
  uint64_t supportReuseChecks=0,supportReuseHits=0,supportReuseBytes=0;
  uint64_t descriptorBindCandidates=0,descriptorWholeMatches=0;
  uint64_t descriptorBindCalls=0,descriptorSkippedSets=0;
  uint64_t pipelineLookups=0,pipelineLastHits=0,pipelineLookasideHits=0,pipelineMapLookups=0;
  std::array<uint64_t,2> descriptorStageMatches{};
 } cpuPreparation;
 uint64_t pipelineCreates=0,pipelineCreateNs=0;
 uint64_t asyncSkippedDraws=0;  // draws skipped while their shaders compile (WWHD_VK_ASYNC_SHADERS)
 uint64_t surfaceImageCreates=0,surfaceImageDestroys=0,surfaceImagePoolHits=0;
 std::array<Surface*,8> passColors{};
 std::array<uint32_t,8> passSlices{};
 Surface* passDepth=nullptr;
 uint32_t passDepthSlice=0,passWidth=0,passHeight=0;
 bool passTracked=false;
 uint64_t renderPassCount=0;
 uint64_t frame=0,drawCount=0; std::atomic<uint64_t> completed{0};
 uint64_t surfaceEpoch=1,surfaceTargetGeneration=1;
 // bumped only when image views can die (destroy_surface_image, save-state reset), not when a surface
 // is created: persistent descriptor sets name views, samplers and buffers only
 uint64_t viewEpoch=1;
 // base/capacity: a block can be a range of a buffer shared by all submission slots (Switch); 0 = whole buffer
 struct UploadBlock { Buffer buffer; VkDeviceSize used=0,dirtyBegin=VK_WHOLE_SIZE,dirtyEnd=0; VkDeviceSize base=0,capacity=0; };
 std::vector<UploadBlock> uploadBlocks;
 uint64_t uploadAllocations=0,uploadBytes=0;
 uint64_t vertexHistoryReuseChecks=0,vertexHistoryReuseHits=0,vertexHistoryReuseBytes=0;
 uint64_t vertexHistoryRequests=0,vertexHistoryMatches=0,vertexHistoryBytes=0;
 std::array<uint64_t,7> vertexHistoryDistances{};
 uint64_t vertexDeclaredBytes=0,vertexCopiedBytes=0;
 uint64_t vertexReuseChecks=0,vertexReuseHits=0,vertexReuseBytes=0,vertexReuseCompareNs=0;
 std::vector<Buffer> garbageBuffers; std::vector<VkDescriptorPool> garbagePools;
 struct RetiredImage {
  VkImage image;VkDeviceMemory memory;std::vector<VkImageView> views;
  SurfaceImageKey key{}; VkDeviceSize allocationBytes=0;
 }; std::vector<RetiredImage> garbageImages;
 // Only fence-retired allocations enter this pool, oldest first.
 std::vector<RetiredImage> surfaceImagePool;
 VkDeviceSize surfaceImagePoolBytes=0;
 // Each submission retains its pools, upload bytes and deferred objects until
 // its fence completes. The fields above alias the active recording slot.
 struct Submission {
  VkCommandPool commandPool=VK_NULL_HANDLE;
  VkCommandBuffer cmd=VK_NULL_HANDLE;
  VkFence fence=VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
  // Acquire waits are consumed by this slot's graphics submission/fence.
  std::array<VkSemaphore,2> acquired{}; // TV, DRC
  VkQueryPool timestampQueries=VK_NULL_HANDLE;
  bool timestampRecorded=false;
  uint64_t timestampFrame=0;
  uint64_t beginNs=0,submitNs=0; // steady clock, for WWHD_VK_SUBMIT_LOG
  static constexpr uint32_t maxGpuScopes=256;
  struct GpuScope { GpuScopeMetadata metadata{}; uint64_t draws=0,generation=0; bool ended=false; };
  std::array<GpuScope,maxGpuScopes> gpuScopes{};
  uint32_t gpuScopeCount=0,activeRenderScope=UINT32_MAX;
  bool pending=false;
  uint64_t serial=0; // Submission order on the single graphics queue.
  std::vector<UploadBlock> uploadBlocks;
  std::vector<Buffer> garbageBuffers; std::vector<VkDescriptorPool> garbagePools;
  std::vector<RetiredImage> garbageImages;
 };
 std::array<Submission,4> submissions{};
 size_t activeSubmission=0;
 uint64_t submissionSerial=0,lastSubmittedSerial=0;
 // Fence markers at the end of each guest frame; before recording frame N+2,
 // wait for frame N. Mid-frame submissions still use the same bounded ring.
 std::array<uint64_t,2> frameSubmissions{};
 bool beginFrame=false;
 Screen tv,drc;
 std::unordered_multimap<uint32_t,std::unique_ptr<Surface>> surfaces;
};
extern Renderer R;
// Render-thread checkpoint; failures leave the cache dirty for a later retry.
void save_pipeline_cache();
// Tokens identify submission order even after a fence/slot has been recycled.
uint64_t recording_submission();
void wait_submission(uint64_t serial);
// draw.cpp: a depth clear ends the pre-pass meshes whose depth writes later draws may skip
void prepass_depth_cleared(const Surface* depth);
void reset_pipeline_lookup_cache();
void checkpoint_pipeline_recipes();  // draw.cpp: pipeline warm-up recipes, quiet-frame save
void warm_up_pipelines();            // draw.cpp: boot only, after the shader warm-up
uint64_t draw_batch_submissions();
void vk_check(VkResult result,const char* operation);
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags properties);
Buffer create_buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags properties);
UploadSlice allocate_upload(VkDeviceSize size,VkDeviceSize alignment);
// Renderer smoke tests exercise the production snapshot helper with host data.
UploadSlice vertex_window_smoke_snapshot(uint32_t binding,uint32_t address,
    uint32_t reservation,uint32_t windowOffset,uint32_t windowLength,
    const void* data,bool poisonUnused);
// Borrow a retained production feedback image; smoke tests never destroy it.
Surface* feedback_image_smoke_snapshot(Surface&,bool vertex,uint32_t unit);
void defer_buffer(Buffer buffer);
// Destroy a descriptor pool once the submission now being recorded has completed.
void defer_descriptor_pool(VkDescriptorPool pool);
void defer_surface_image(VkImage image,VkDeviceMemory memory,std::vector<VkImageView> views,
    SurfaceImageKey key = {},VkDeviceSize allocationBytes = 0);
void recycle_surface_image(Renderer::RetiredImage);
void reset_surface_image_pool(); // After draining submissions, before device teardown.
VkCommandBuffer command_buffer();
void end_encoder();
void gpu_begin_render_scope(const std::array<Surface*,8>&,Surface*,const uint32_t*,uint32_t,uint32_t,uint32_t);
void gpu_count_render_draw();
GpuScopeToken gpu_begin_feedback_scope(const Surface&);
void gpu_end_feedback_scope(GpuScopeToken);
void transition_image(Surface*,VkImageLayout,VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VkAccessFlags access=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
void forget_texture_views();
void service_captures();
void request_tv_dump(const std::string&, int);
void create_surface_image(Surface*,bool forRendering,VkExtent3D explicitExtent = {});
void reset_ao_private_cache();
void destroy_surface_image(Surface*);
VkImageView layer_view(Surface*,uint32_t slice);
VkImageView sampled_texture_view(Surface*,const uint32_t* texWords);
Surface* find_or_create_surface(const SurfaceDesc&,bool forRendering);
Surface* color_target(const uint32_t*,int,uint32_t* slice=nullptr);
Surface* depth_target(const uint32_t*,uint32_t* slice=nullptr);
Surface* surface_from_color_buffer(uint32_t,uint32_t* firstSlice=nullptr,uint32_t* numSlices=nullptr);
Surface* surface_from_depth_buffer(uint32_t,uint32_t* firstSlice=nullptr,uint32_t* numSlices=nullptr);
Surface* surface_from_color_payload(const void*,uint32_t* firstSlice=nullptr,uint32_t* numSlices=nullptr);
Surface* surface_from_depth_payload(const void*,uint32_t* firstSlice=nullptr,uint32_t* numSlices=nullptr);
Surface* sampled_texture(const uint32_t*,bool);
void upload_surface(Surface*);
void resample(Surface*,Surface*,uint32_t slices,float uMax=1,float vMax=1,uint32_t dstW=0,uint32_t dstH=0);
float res_scale();void set_res_scale(float);void latch_res_scale();
void sweep_texture_page_writes();
uint64_t next_write_seq();
inline void mark_gpu_written(Surface* s){
 if(s->addressState) {
  if(!s->gpuWritten) ++R.surfaceTargetGeneration;
  if(!s->gpuWritten || s->addressState->ambiguous) ++s->addressState->generation;
 }
 s->gpuWritten=true;s->writeSeq=next_write_seq();
}
}

namespace gfxvk { void reset_feedback_images(); } // Call before device teardown, then drain retirements.
