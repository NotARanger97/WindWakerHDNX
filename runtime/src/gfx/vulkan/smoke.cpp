// No game assets: assertions inspect data returned by the actual Vulkan device.
#include "backend.h"
#include "present.h"
#include "shaders.h"
#include "draw_options.h"
#include "gx2/gx2.h"
#include "runtime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <zlib.h>
#include <stdexcept>
#include <string>

namespace gfxvk {
extern uint64_t g_stat_full_checks, g_stat_uploads;
void request_tv_dump(const std::string&,int);
namespace {
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
struct Image {
 Surface s;
 Image(uint32_t w,uint32_t h,uint32_t format,bool depth=false,uint32_t layers=1,uint32_t mips=1) {
  s.width=w;s.height=h;s.pitch=w;s.format=format;s.isDepth=depth;s.slices=layers;s.mips=mips;s.dim=layers>1?5:1;s.fmt=format_info(format,depth);create_surface_image(&s,true);
 }
 ~Image(){destroy_surface_image(&s);}
};
std::vector<uint8_t> read_image(Surface& s,VkImageAspectFlags aspect,uint32_t bytes,uint32_t mip=0,uint32_t layer=0) {
 uint32_t w=std::max(1u,s.extent.width>>mip),h=std::max(1u,s.extent.height>>mip);
 Buffer b=create_buffer(size_t(w)*h*bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 transition_image(&s,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
 VkBufferImageCopy copy{};copy.imageSubresource={aspect,mip,layer,1};copy.imageExtent={w,h,1};
 auto cmd=command_buffer();vkCmdCopyImageToBuffer(cmd,s.image,s.layout,b.buffer,1,&copy);
 VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=b.buffer;barrier.size=VK_WHOLE_SIZE;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
 try {flush_readback();std::vector<uint8_t> result(size_t(w)*h*bytes);memcpy(result.data(),b.mapped,result.size());defer_buffer(b);return result;}
 catch(...){defer_buffer(b);throw;}
}
void rgba_is(const std::vector<uint8_t>& data,const uint8_t rgba[4],const char* message) {
 for(size_t i=0;i<data.size();++i)require(data[i]==rgba[i%4],message);
}
void clear_image(Surface& s,const float rgba[4]) {
 transition_image(&s,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
 VkClearColorValue value{};std::copy(rgba,rgba+4,value.float32);VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,s.mips,0,s.arrayLayers};
 vkCmdClearColorImage(command_buffer(),s.image,s.layout,&value,1,&range);mark_gpu_written(&s);
}
void surface_pool_check() {
 if(!draw_option_enabled("WWHD_VK_SURFACE_IMAGE_POOL"))return;
 wait_idle();reset_surface_image_pool();
 {
  Image original(16,16,0x1a);
  const VkImage image=original.s.image;const VkDeviceMemory memory=original.s.memory;
  const float green[4]={0,1,0,1},blue[4]={0,0,1,1};
  const uint8_t blueBytes[4]={0,0,255,255},freshBytes[4]={17,34,51,255};
  clear_image(original.s,green);
  destroy_surface_image(&original.s);
  // A release in the current recording cannot be reissued, even though the
  // CPU has already dropped its Surface. Its GPU commands have not retired.
  Image pending(16,16,0x1a);
  require(pending.s.image!=image&&pending.s.memory!=memory,"surface pool reused an unretired allocation");
  clear_image(pending.s,blue);
  const uint64_t serial=recording_submission();flush_async();wait_submission(serial);
  const uint64_t creates=R.surfaceImageCreates,hits=R.surfaceImagePoolHits;
  Image reused(16,16,0x1a);
  require(reused.s.image==image&&reused.s.memory==memory,"surface pool failed fenced image/memory reuse");
  require(R.surfaceImageCreates==creates&&R.surfaceImagePoolHits==hits+1,"surface pool allocated on a warm hit");
  require(reused.s.layout==VK_IMAGE_LAYOUT_UNDEFINED,"pooled surface retained content layout");
  reused.s.addr=mem::host_alloc(65536,256);
  for(uint32_t i=0;i<65536;++i)mem::ptr(reused.s.addr)[i]=freshBytes[i%4];
  upload_surface(&reused.s);
  rgba_is(read_image(reused.s,VK_IMAGE_ASPECT_COLOR_BIT,4),freshBytes,"pooled image showed previous owner's texels");
  rgba_is(read_image(pending.s,VK_IMAGE_ASPECT_COLOR_BIT,4),blueBytes,"pooled image overwrote a live allocation");
 }
 wait_idle();
 // Alternating allocation classes must warm independently. Mips, layers,
 // extent, color space and dimensionality all participate in the pool key.
 auto cycle=[] {
  Image base(16,16,0x1a),size(32,16,0x1a),mips(16,16,0x1a,false,1,2),
      layers(16,16,0x1a,false,2),srgb(16,16,0x41a),flat(16,1,0x1a);
  Surface oneD;oneD.width=16;oneD.height=1;oneD.dim=0;oneD.format=0x1a;oneD.fmt=format_info(oneD.format,false);
  create_surface_image(&oneD,false);destroy_surface_image(&oneD);
 };
 cycle();wait_idle();
 const uint64_t creates=R.surfaceImageCreates,destroys=R.surfaceImageDestroys;
 for(unsigned i=0;i<8;++i){cycle();wait_idle();}
 require(R.surfaceImageCreates==creates&&R.surfaceImageDestroys==destroys,"steady surface allocation classes failed to warm");
 reset_surface_image_pool();
 fprintf(stderr,"[renderer smoke] surface pool fence exclusion, fresh upload and allocation-free warm classes passed\n");
}
void feedback_image_check() {
 if(!draw_option_enabled("WWHD_VK_REUSE_FEEDBACK_IMAGES"))return;
 reset_feedback_images();wait_idle();
 {
  Image a(16,16,0x1a),b(32,16,0x1a),c(8,8,0x1a);
  Surface* sources[3]={&a.s,&b.s,&c.s};
  Surface* copies[2][3]{};
  const float green[4]={0,1,0,1};
  for(auto* source:sources)clear_image(*source,green);
  for(unsigned stage=0;stage<2;++stage)for(unsigned shape=0;shape<3;++shape)
   copies[stage][shape]=feedback_image_smoke_snapshot(*sources[shape],stage!=0,0);
  for(unsigned shape=0;shape<3;++shape)
   require(copies[0][shape]->image!=copies[1][shape]->image,"VS/PS feedback descriptors share an overwrite slot");
  const uint64_t creates=R.surfaceImageCreates,destroys=R.surfaceImageDestroys;
  // Alternate shapes in one texture unit, mutate the source every time and
  // inspect the actual retained copies. A cache hit must still copy new texels.
  for(unsigned round=0;round<8;++round)for(unsigned stage=0;stage<2;++stage)for(unsigned shape=0;shape<3;++shape) {
   const float rgba[4]={float(round&1),float(stage),float(shape&1),1};
   const uint8_t bytes[4]={uint8_t((round&1)*255),uint8_t(stage*255),uint8_t((shape&1)*255),255};
   clear_image(*sources[shape],rgba);
   auto* copy=feedback_image_smoke_snapshot(*sources[shape],stage!=0,0);
   require(copy==copies[stage][shape],"alternating feedback shape was replaced");
   rgba_is(read_image(*copy,VK_IMAGE_ASPECT_COLOR_BIT,4),bytes,"feedback reuse skipped a fresh source copy");
  }
  require(R.surfaceImageCreates==creates&&R.surfaceImageDestroys==destroys,"warm feedback shapes allocated or destroyed images");
 }
 reset_feedback_images();wait_idle();
 fprintf(stderr,"[renderer smoke] alternating feedback shapes, separate VS/PS slots and fresh copies passed\n");
}
void upload_arena_check() {
 const uint64_t before=R.uploadAllocations;
 auto a=allocate_upload(16,256),b=allocate_upload(16,256);
 require(a.buffer==b.buffer&&a.offset!=b.offset,"arena slices alias or fail pooling");
 require(a.offset%256==0&&b.offset%256==0,"arena alignment failed");
 memset(a.mapped,0x31,16);memset(b.mapped,0x72,16);
 Buffer out=create_buffer(32,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 VkBufferCopy ca{a.offset,0,16},cb{b.offset,16,16};
 auto cmd=command_buffer();vkCmdCopyBuffer(cmd,a.buffer,out.buffer,1,&ca);vkCmdCopyBuffer(cmd,b.buffer,out.buffer,1,&cb);
 VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=out.buffer;barrier.size=VK_WHOLE_SIZE;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);flush_readback();
 auto bytes=static_cast<uint8_t*>(out.mapped);for(int i=0;i<32;i++)require(bytes[i]==(i<16?0x31:0x72),"arena GPU snapshots corrupted");
 auto reuse=allocate_upload(16,256);require(reuse.buffer==a.buffer&&reuse.offset==a.offset,"arena failed fence reuse");
 require(R.uploadAllocations<=before+1,"arena allocated per slice");defer_buffer(out);command_buffer();flush_readback();
 fprintf(stderr,"[renderer smoke] immutable upload arena GPU snapshots and fence reuse passed\n");
}
void asynchronous_submission_check() {
 constexpr uint32_t submissions=10, payloadSize=16, regionSize=payloadSize*3;
 Buffer out=create_buffer(submissions*regionSize,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 std::array<UploadSlice,4> firstSlices{};
 for(uint32_t submission=0;submission<submissions;++submission) {
  auto a=allocate_upload(payloadSize,256),b=allocate_upload(payloadSize,256);
  require(a.buffer==b.buffer&&a.offset!=b.offset,"async submission slices alias");
  if(submission<firstSlices.size())firstSlices[submission]=a;
  else {
   const auto& previous=firstSlices[submission%firstSlices.size()];
   require(a.buffer==previous.buffer&&a.offset==previous.offset,"async slot failed fenced arena reuse");
  }
  for(uint32_t byte=0;byte<payloadSize;++byte) {
   static_cast<uint8_t*>(a.mapped)[byte]=uint8_t(submission*19+byte);
   static_cast<uint8_t*>(b.mapped)[byte]=uint8_t(255-submission*13-byte);
  }
  // A temporary buffer is referenced twice and retired with this submission.
  // Freeing it while queued, or recycling its upload bytes early, corrupts the
  // third output region (and should also trigger Vulkan lifetime validation).
  Buffer temporary=create_buffer(payloadSize,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  auto cmd=command_buffer();
  VkBufferCopy first{a.offset,submission*regionSize,payloadSize};
  VkBufferCopy second{b.offset,submission*regionSize+payloadSize,payloadSize};
  VkBufferCopy toTemporary{a.offset,0,payloadSize};
  vkCmdCopyBuffer(cmd,a.buffer,out.buffer,1,&first);
  vkCmdCopyBuffer(cmd,b.buffer,out.buffer,1,&second);
  vkCmdCopyBuffer(cmd,a.buffer,temporary.buffer,1,&toTemporary);
  VkBufferMemoryBarrier transfer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  transfer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;transfer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  transfer.srcQueueFamilyIndex=transfer.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  transfer.buffer=temporary.buffer;transfer.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&transfer,0,nullptr);
  VkBufferCopy fromTemporary{0,submission*regionSize+payloadSize*2,payloadSize};
  vkCmdCopyBuffer(cmd,temporary.buffer,out.buffer,1,&fromTemporary);
  defer_buffer(temporary);
  flush_async();
 }
 // Waiting on an earlier readback token must not drain unrelated later work.
 uint64_t oldest=UINT64_MAX,newest=0;
 for(const auto& slot:R.submissions)if(slot.pending) {
  oldest=std::min(oldest,slot.serial);newest=std::max(newest,slot.serial);
 }
 require(oldest<newest,"async submits did not retain multiple pending slots");
 const uint64_t recording=recording_submission();
 wait_submission(oldest);
 wait_submission(oldest); // A completed token remains safe to request again.
 require(recording_submission()==recording,"token wait changed the recording slot");
 for(const auto& slot:R.submissions) {
  if(slot.serial==oldest)require(!slot.pending,"token wait failed to retire its submission");
  if(slot.serial==newest)require(slot.pending,"token wait drained a later submission");
 }
 VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
 host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 host.buffer=out.buffer;host.size=VK_WHOLE_SIZE;
 vkCmdPipelineBarrier(command_buffer(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
 flush_readback();
 auto* bytes=static_cast<uint8_t*>(out.mapped);
 for(uint32_t submission=0;submission<submissions;++submission)
  for(uint32_t byte=0;byte<payloadSize;++byte) {
   uint32_t offset=submission*regionSize+byte;
   require(bytes[offset]==uint8_t(submission*19+byte),"async upload first snapshot differs");
   require(bytes[offset+payloadSize]==uint8_t(255-submission*13-byte),"async upload second snapshot differs");
   require(bytes[offset+payloadSize*2]==uint8_t(submission*19+byte),"async deferred temporary copy differs");
  }
 wait_idle();
 for(const auto& slot:R.submissions)
  require(!slot.pending&&slot.garbageBuffers.empty()&&slot.garbageImages.empty(),"async drain left pending resources");
 defer_buffer(out);flush_readback();
 fprintf(stderr,"[renderer smoke] ten async submissions, immutable snapshots, slot wrap and deferred retirement passed\n");
}
void triangle(Surface& s) {
 struct Resources {
  VkShaderModule vs=VK_NULL_HANDLE,ps=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
  ~Resources(){if(pipeline)vkDestroyPipeline(R.device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(R.device,layout,nullptr);if(vs)vkDestroyShaderModule(R.device,vs,nullptr);if(ps)vkDestroyShaderModule(R.device,ps,nullptr);}
 } objects;
 auto module=[&](const char* glsl,bool vertex,VkShaderModule& result) {
  std::string error;auto words=vk::compile_glsl(glsl,vertex,&error);if(words.empty())throw std::runtime_error("smoke GLSL compilation: "+error);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();vk_check(vkCreateShaderModule(R.device,&ci,nullptr,&result),"smoke shader module");
 };
 module("#version 450\nvoid main(){vec2 p[3]=vec2[3](vec2(-0.8,-0.8),vec2(0.8,-0.8),vec2(0,0.8));gl_Position=vec4(p[gl_VertexIndex],0,1);}",true,objects.vs);
 module("#version 450\nlayout(location=0) out vec4 color;void main(){color=vec4(1,0,0,1);}",false,objects.ps);
 VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};vk_check(vkCreatePipelineLayout(R.device,&layout,nullptr,&objects.layout),"smoke pipeline layout");
 VkPipelineShaderStageCreateInfo stages[2]{};
 for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
 stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=objects.vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=objects.ps;
 VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport viewport{0,0,float(s.extent.width),float(s.extent.height),0,1};VkRect2D scissor{{0,0},{s.extent.width,s.extent.height}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=0xf;
 VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
 VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&s.fmt.pixel;
 VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pNext=&rendering;info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&vp;info.pRasterizationState=&raster;info.pMultisampleState=&ms;info.pColorBlendState=&blend;info.layout=objects.layout;
 vk_check(vkCreateGraphicsPipelines(R.device,R.pipelineCache,1,&info,nullptr,&objects.pipeline),"smoke triangle pipeline");
 R.pipelineCacheDirty=true;
 R.pipelineCacheChangedFrame=R.frame;
 transition_image(&s,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
 VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};target.imageView=layer_view(&s,0);target.imageLayout=s.layout;target.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;target.storeOp=VK_ATTACHMENT_STORE_OP_STORE;target.clearValue.color.float32[3]=1;
 VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea=scissor;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&target;
 auto cmd=command_buffer();vkCmdBeginRendering(cmd,&ri);R.rendering=true;vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,objects.pipeline);vkCmdDraw(cmd,3,1,0,0);end_encoder();mark_gpu_written(&s);flush_readback();
 auto pixels=read_image(s,VK_IMAGE_ASPECT_COLOR_BIT,4);
 size_t center=(size_t(s.extent.height/2)*s.extent.width+s.extent.width/2)*4;
 require(pixels[center]==255&&pixels[center+1]==0&&pixels[center+2]==0&&pixels[center+3]==255,"triangle center pixel differs");
 require(pixels[0]==0&&pixels[1]==0&&pixels[2]==0&&pixels[3]==255,"triangle background pixel differs");
 size_t red=0;for(size_t i=0;i<pixels.size();i+=4)if(pixels[i]==255)++red;
 require(red>size_t(s.extent.width)*s.extent.height/5&&red<size_t(s.extent.width)*s.extent.height/2,"triangle rasterized area differs");
 fprintf(stderr,"[renderer smoke] generated GLSL triangle/readback passed (%zu red pixels)\n",red);
}
void vertex_window_check(Surface& s) {
 struct Resources {
  VkShaderModule vs=VK_NULL_HANDLE,ps=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
  ~Resources(){if(pipeline)vkDestroyPipeline(R.device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(R.device,layout,nullptr);if(vs)vkDestroyShaderModule(R.device,vs,nullptr);if(ps)vkDestroyShaderModule(R.device,ps,nullptr);}
 } objects;
 auto module=[&](const char* glsl,bool vertex,VkShaderModule& result) {
  std::string error;auto words=vk::compile_glsl(glsl,vertex,&error);if(words.empty())throw std::runtime_error("smoke GLSL compilation: "+error);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();vk_check(vkCreateShaderModule(R.device,&ci,nullptr,&result),"smoke shader module");
 };
 module("#version 450\nlayout(location=0) in vec3 position;layout(location=1) in vec4 tint;layout(location=0) out vec4 vertexColor;void main(){gl_Position=vec4(position.xy,0,1);vertexColor=gl_VertexIndex==int(position.z)?tint:vec4(0,0,1,1);}",true,objects.vs);
 module("#version 450\nlayout(location=0) in vec4 vertexColor;layout(location=0) out vec4 color;void main(){color=vertexColor;}",false,objects.ps);
 VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};vk_check(vkCreatePipelineLayout(R.device,&layout,nullptr,&objects.layout),"smoke pipeline layout");
 VkPipelineShaderStageCreateInfo stages[2]{};
 for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
 stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=objects.vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=objects.ps;
 struct Vertex {float x,y,id,r,g,b,a;};
 VkVertexInputBindingDescription binding{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
 VkVertexInputAttributeDescription attributes[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32A32_SFLOAT,12}};
 VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vertex.vertexBindingDescriptionCount=1;vertex.pVertexBindingDescriptions=&binding;vertex.vertexAttributeDescriptionCount=2;vertex.pVertexAttributeDescriptions=attributes;
 VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;assembly.primitiveRestartEnable=VK_TRUE;
 VkViewport viewport{0,0,float(s.extent.width),float(s.extent.height),0,1};VkRect2D scissor{{0,0},{s.extent.width,s.extent.height}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=0xf;
 VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
 VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&s.fmt.pixel;
 VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pNext=&rendering;info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&vp;info.pRasterizationState=&raster;info.pMultisampleState=&ms;info.pColorBlendState=&blend;info.layout=objects.layout;
 vk_check(vkCreateGraphicsPipelines(R.device,R.pipelineCache,1,&info,nullptr,&objects.pipeline),"smoke triangle pipeline");
 R.pipelineCacheDirty=true;
 R.pipelineCacheChangedFrame=R.frame;
 std::array<Vertex,128> data{};
 for(uint32_t i=100;i<108;++i) {const float xy[3][2]={{-0.8f,-0.8f},{0.8f,-0.8f},{0,0.8f}};data[i]={xy[(i-100)%3][0],xy[(i-100)%3][1],float(i),1,0,0,1};}
 const uint32_t address=0x73510000; // Fixture cache identity; data is supplied directly.
 auto sameSlice=[](const UploadSlice& a,const UploadSlice& b){return a.buffer==b.buffer && a.offset==b.offset;};
 const char* reuseEnv=std::getenv("WWHD_VK_REUSE_VERTEX_SNAPSHOTS");
 const bool reuse=reuseEnv && !std::strcmp(reuseEnv,"1");
 const uint32_t reserved=106*sizeof(Vertex),offset=100*sizeof(Vertex);
 auto original=vertex_window_smoke_snapshot(0,address,reserved,offset,reserved-offset,data.data(),true);
 for(uint32_t i=0;i<offset;++i)require(static_cast<uint8_t*>(original.mapped)[i]==0xCD,"vertex window leading poison missing");
 auto repeated=vertex_window_smoke_snapshot(0,address,reserved,offset,reserved-offset,data.data(),true);
 require(sameSlice(original,repeated)==reuse,"vertex window exact reuse mismatch");
 data[1].r=0.25f;
 auto outside=vertex_window_smoke_snapshot(0,address,reserved,offset,reserved-offset,data.data(),true);
 require(sameSlice(repeated,outside)==reuse,"vertex window outside mutation reuse mismatch");
 data[101].g=0.25f;
 auto changed=vertex_window_smoke_snapshot(0,address,reserved,offset,reserved-offset,data.data(),true);
 require(!sameSlice(outside,changed),"vertex window fetched mutation reused stale slice");
 auto shifted=vertex_window_smoke_snapshot(0,address,reserved,offset+sizeof(Vertex),reserved-offset-sizeof(Vertex),data.data(),true);
 require(!sameSlice(changed,shifted),"vertex window shifted key reused stale slice");
 data[101].g=0;
 auto render=[&](bool window,uint32_t first,int32_t base,bool narrow,bool restart,bool converted) {
  const uint32_t end=restart?first+6:first+3;
  const uint32_t reservation=end*sizeof(Vertex),begin=window?first*sizeof(Vertex):0;
  auto vertices=vertex_window_smoke_snapshot(0,address,reservation,begin,reservation-begin,data.data(),true);
  // A freshly copied slice must preserve every fetched byte, including IDs.
  require(!memcmp(static_cast<uint8_t*>(vertices.mapped)+begin,reinterpret_cast<uint8_t*>(data.data())+begin,reservation-begin),"vertex window snapshot bytes differ");
  std::vector<uint32_t> wide={uint32_t(int64_t(first)-base),uint32_t(int64_t(first+1)-base),uint32_t(int64_t(first+2)-base)};
  if(restart){wide.push_back(UINT32_MAX);for(uint32_t i=3;i<6;++i)wide.push_back(uint32_t(int64_t(first+i)-base));}
  // Custom guest marker and endian conversion are normalized before Vulkan.
  if(converted){
   auto swap=[](uint32_t v){return (v<<24)|((v&0xFF00)<<8)|((v>>8)&0xFF00)|(v>>24);};
   if(restart)wide[3]=0x12345678;
   for(auto& value:wide){const uint32_t guestEndian=swap(value);value=swap(guestEndian);if(restart && value==0x12345678)value=UINT32_MAX;}
  }
  auto indices=allocate_upload(wide.size()*(narrow?2:4),4);
  for(size_t i=0;i<wide.size();++i) {if(narrow){uint16_t value=wide[i]==UINT32_MAX?UINT16_MAX:uint16_t(wide[i]);memcpy(static_cast<uint8_t*>(indices.mapped)+i*2,&value,2);}else memcpy(static_cast<uint8_t*>(indices.mapped)+i*4,&wide[i],4);}
  transition_image(&s,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};target.imageView=layer_view(&s,0);target.imageLayout=s.layout;target.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;target.storeOp=VK_ATTACHMENT_STORE_OP_STORE;target.clearValue.color.float32[3]=1;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea=scissor;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&target;
  auto cmd=command_buffer();vkCmdBeginRendering(cmd,&ri);R.rendering=true;vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,objects.pipeline);
  vkCmdBindVertexBuffers(cmd,0,1,&vertices.buffer,&vertices.offset);vkCmdBindIndexBuffer(cmd,indices.buffer,indices.offset,narrow?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(cmd,wide.size(),1,0,base,0);end_encoder();mark_gpu_written(&s);flush_async();
 };
 for(int test=0;test<12;++test) {
  const uint32_t first=test>=8?101:100;const int32_t base=test%3==0?-5:test%3==1?5:0;
  const bool narrow=test%2==0,restart=test%4>=2,converted=test%4==3;
  if(test==6)data[101].g=0.5f; // Same key, freshly changed fetched bytes.
  if(test==7)data[1].r=0.3f;   // Never fetched; must not affect pixels.
  render(false,first,base,narrow,restart,converted);auto full=read_image(s,VK_IMAGE_ASPECT_COLOR_BIT,4);
  render(true,first,base,narrow,restart,converted);auto window=read_image(s,VK_IMAGE_ASPECT_COLOR_BIT,4);
  require(full==window,"vertex window/full GPU pixels differ");
  const size_t center=(size_t(s.extent.height/2)*s.extent.width+s.extent.width/2)*4;
  require(window[center]>200 && window[center+2]==0,"vertex window position/VertexIndex invariant failed");
 }
 // More than four queued submissions exercise slot retirement and epoch reset.
 for(int i=0;i<10;++i)render(true,100,i%2?-5:5,i%2,false,false);
 auto final=read_image(s,VK_IMAGE_ASPECT_COLOR_BIT,4);
 require(final[(size_t(s.extent.height/2)*s.extent.width+s.extent.width/2)*4]>200,"vertex window ring retirement failed");
 fprintf(stderr,"[renderer smoke] vertex copy windows/full pixels, poison, signed base, restart, mutation and ring retirement passed\n");
}
void dynamic_uniform_check(Surface& s) {
 if(R.properties.limits.maxDescriptorSetUniformBuffersDynamic<3) {
  fprintf(stderr,"[renderer smoke] dynamic UBO ordering fixture skipped (device limit <3)\n");return;
 }
 struct Resources {
  VkShaderModule vs=VK_NULL_HANDLE,ps=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
  VkDescriptorSetLayout sets[2]{};
  ~Resources(){for(auto set:sets)if(set)vkDestroyDescriptorSetLayout(R.device,set,nullptr);if(pipeline)vkDestroyPipeline(R.device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(R.device,layout,nullptr);if(vs)vkDestroyShaderModule(R.device,vs,nullptr);if(ps)vkDestroyShaderModule(R.device,ps,nullptr);}
 } objects;
 auto module=[&](const char* glsl,bool vertex,VkShaderModule& result) {
  std::string error;auto words=vk::compile_glsl(glsl,vertex,&error);if(words.empty())throw std::runtime_error("smoke GLSL compilation: "+error);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();vk_check(vkCreateShaderModule(R.device,&ci,nullptr,&result),"smoke shader module");
 };
 module("#version 450\nlayout(set=0,binding=7,std140) uniform Transform{vec4 transform;} ;layout(set=0,binding=1,std140) uniform Tint{vec4 tint;};layout(location=0) out vec4 vcolor;void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexIndex]*transform.xy+transform.zw,0,1);vcolor=tint;}",true,objects.vs);
 module("#version 450\nlayout(set=1,binding=3,std140) uniform Factor{vec4 factor;};layout(location=0) in vec4 vcolor;layout(location=0) out vec4 color;void main(){color=vcolor*factor;}",false,objects.ps);
 VkDescriptorSetLayoutBinding vsBindings[2]={{7,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}};
 VkDescriptorSetLayoutBinding psBinding{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
 VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=2;setInfo.pBindings=vsBindings;
 vk_check(vkCreateDescriptorSetLayout(R.device,&setInfo,nullptr,&objects.sets[0]),"smoke dynamic VS layout");
 setInfo.bindingCount=1;setInfo.pBindings=&psBinding;vk_check(vkCreateDescriptorSetLayout(R.device,&setInfo,nullptr,&objects.sets[1]),"smoke dynamic PS layout");
 VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=2;layout.pSetLayouts=objects.sets;
 vk_check(vkCreatePipelineLayout(R.device,&layout,nullptr,&objects.layout),"smoke dynamic pipeline layout");
 VkPipelineShaderStageCreateInfo stages[2]{};
 for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
 stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=objects.vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=objects.ps;
 VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport viewport{0,0,float(s.extent.width),float(s.extent.height),0,1};VkRect2D scissor{{0,0},{s.extent.width,s.extent.height}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=0xf;
 VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
 VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&s.fmt.pixel;
 VkDynamicState dynamicState=VK_DYNAMIC_STATE_SCISSOR;
 VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&dynamicState;
 VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pNext=&rendering;info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&vp;info.pRasterizationState=&raster;info.pMultisampleState=&ms;info.pColorBlendState=&blend;info.layout=objects.layout;info.pDynamicState=&dynamic;
 vk_check(vkCreateGraphicsPipelines(R.device,R.pipelineCache,1,&info,nullptr,&objects.pipeline),"smoke triangle pipeline");
 R.pipelineCacheDirty=true;
 R.pipelineCacheChangedFrame=R.frame;
 // Two rounds force a pool reset between freshly allocated descriptor sets.
 for(uint32_t round=0;round<2;++round) {
  uint64_t generation=R.submissionGeneration;
  const float transform[4]={1,1,0,0},factor[4]={1,1,1,1};
  const float tints[2][2][4]={{{1,0,0,1},{0,1,0,1}},{{0,0,1,1},{1,1,0,1}}};
  std::array<UploadSlice,2> transforms{},tintSlices{},factors{};
  for(uint32_t draw=0;draw<2;++draw) {
   // Prepare binding7 before binding1, matching support uniforms prepared last.
   transforms[draw]=allocate_upload(16,R.properties.limits.minUniformBufferOffsetAlignment);
   factors[draw]=allocate_upload(16,R.properties.limits.minUniformBufferOffsetAlignment);
   tintSlices[draw]=allocate_upload(16,R.properties.limits.minUniformBufferOffsetAlignment);
   memcpy(transforms[draw].mapped,transform,16);memcpy(factors[draw].mapped,factor,16);memcpy(tintSlices[draw].mapped,tints[round][draw],16);
  }
  require(transforms[0].buffer==tintSlices[1].buffer&&transforms[0].buffer==factors[1].buffer,"dynamic UBO fixture requires pooled slices");
  VkDescriptorSet sets[2]{};VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=R.descriptorPool;allocation.descriptorSetCount=2;allocation.pSetLayouts=objects.sets;
  vk_check(vkAllocateDescriptorSets(R.device,&allocation,sets),"smoke dynamic descriptor sets");
  VkDescriptorBufferInfo buffers[3]={{transforms[0].buffer,0,16},{tintSlices[0].buffer,0,16},{factors[0].buffer,0,16}};
  VkWriteDescriptorSet writes[3]{};
  for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=sets[i==2?1:0];writes[i].dstBinding=i==0?7:i==1?1:3;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;writes[i].pBufferInfo=&buffers[i];}
  vkUpdateDescriptorSets(R.device,3,writes,0,nullptr);
  transition_image(&s,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};target.imageView=layer_view(&s,0);target.imageLayout=s.layout;target.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;target.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea=scissor;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&target;
  auto cmd=command_buffer();vkCmdBeginRendering(cmd,&ri);R.rendering=true;vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,objects.pipeline);
  for(uint32_t draw=0;draw<2;++draw) {
   // Dynamic offsets are ordered by binding within VS set0, then PS set1.
   uint32_t offsets[3]={uint32_t(tintSlices[draw].offset),uint32_t(transforms[draw].offset),uint32_t(factors[draw].offset)};
   vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,objects.layout,0,2,sets,3,offsets);
   VkRect2D half{{int32_t(draw*s.extent.width/2),0},{s.extent.width/2,s.extent.height}};vkCmdSetScissor(cmd,0,1,&half);vkCmdDraw(cmd,3,1,0,0);
  }
  end_encoder();mark_gpu_written(&s);flush_readback();
  require(R.submissionGeneration!=generation,"dynamic descriptor pool reset did not change generation");
  auto pixels=read_image(s,VK_IMAGE_ASPECT_COLOR_BIT,4);
  for(uint32_t y=0;y<s.extent.height;++y)for(uint32_t x=0;x<s.extent.width;++x)for(uint32_t channel=0;channel<4;++channel)
   require(pixels[(size_t(y)*s.extent.width+x)*4+channel]==uint8_t(tints[round][x<s.extent.width/2?0:1][channel]*255),"dynamic UBO reused-set colors or offset order differ");
 }
 fprintf(stderr,"[renderer smoke] dynamic UBO immutable draws, VS/PS binding order and fresh sets after pool reset passed\n");
}
}
int renderer_smoke_test() {
 try {
  mem::init();upload_arena_check();asynchronous_submission_check();set_res_scale(1);latch_res_scale();surface_pool_check();feedback_image_check();
  {
   Image upload(16,16,0x1a,false,2,2);
   upload.s.addr=mem::host_alloc(65536,256);upload.s.mipAddr=mem::host_alloc(65536,256);
   const uint8_t rgba[4]={17,34,51,255};
   for(uint32_t i=0;i<65536;++i){mem::ptr(upload.s.addr)[i]=rgba[i%4];mem::ptr(upload.s.mipAddr)[i]=rgba[i%4];}
   upload_surface(&upload.s);
   uint32_t textureWords[7]={5,0,0,0,(2u<<16)|(1u<<19)|(0u<<22)|(5u<<25),0,0};
   auto swizzled=sampled_texture_view(&upload.s,textureWords);require(swizzled!=VK_NULL_HANDLE&&swizzled==sampled_texture_view(&upload.s,textureWords),"sampled array/swizzle view cache differs");
   for(uint32_t mip=0;mip<2;++mip)for(uint32_t layer=0;layer<2;++layer)rgba_is(read_image(upload.s,VK_IMAGE_ASPECT_COLOR_BIT,4,mip,layer),rgba,"guest mip/layer upload differs");
   fprintf(stderr,"[renderer smoke] guest upload, two mips and two layers passed\n");
   // Use the public surface cache so the public invalidation path sees this fixture.
   SurfaceDesc cacheDesc;
   cacheDesc.addr=upload.s.addr;cacheDesc.mipAddr=upload.s.mipAddr;
   cacheDesc.width=16;cacheDesc.height=16;cacheDesc.pitch=16;cacheDesc.slices=2;
   cacheDesc.mips=2;cacheDesc.format=0x1a;cacheDesc.dim=5;
   auto* cached=find_or_create_surface(cacheDesc,false);
   require(find_or_create_surface(cacheDesc,false)==cached,"identical guest descriptor missed surface cache");
   // CPU-owned aliases cannot borrow old content hashes or decode geometry.
   auto identity=cacheDesc;identity.pitch=32;
   auto* pitched=find_or_create_surface(identity,false);
   require(pitched!=cached&&find_or_create_surface(identity,false)==pitched,"guest pitch identity was not retained");
   identity=cacheDesc;identity.tileMode=1;
   require(find_or_create_surface(identity,false)!=cached,"guest tile mode was ignored in surface identity");
   identity=cacheDesc;identity.mipAddr=mem::host_alloc(65536,256);
   require(find_or_create_surface(identity,false)!=cached,"guest mip address was ignored in surface identity");
   upload_surface(cached);
   uint64_t checks=g_stat_full_checks,uploads=g_stat_uploads;
   upload_surface(cached);
   require(g_stat_full_checks==checks&&g_stat_uploads==uploads,"same-frame upload bypassed texture cache");
   ++R.frame;
   if(((R.frame+(cached->addr>>12))&63)==0)++R.frame;
   upload_surface(cached);
   require(g_stat_full_checks==checks&&g_stat_uploads==uploads,"unchanged next-frame texture performed a full check");
   const uint8_t changedMip[4]={85,102,119,255};
   for(uint32_t i=0;i<65536;++i)mem::ptr(cached->mipAddr)[i]=changedMip[i%4];
   // The invalidated range starts inside the mip, rather than crossing its base.
   invalidate(2,cached->mipAddr+16,4);
   require(cached->dirty&&cached->lastCheckedFrame!=R.frame,"interior mip invalidation did not reset the upload gate");
   upload_surface(cached);
   require(g_stat_full_checks==checks+1&&g_stat_uploads==uploads+1,"same-frame invalidated mip was not uploaded");
   for(uint32_t layer=0;layer<2;++layer) {
    rgba_is(read_image(*cached,VK_IMAGE_ASPECT_COLOR_BIT,4,0,layer),rgba,"mip invalidation changed the base image");
    rgba_is(read_image(*cached,VK_IMAGE_ASPECT_COLOR_BIT,4,1,layer),changedMip,"invalidated mip guest upload differs");
   }
   fprintf(stderr,"[renderer smoke] upload cache and interior mip invalidation/readback passed\n");
   {
    // A texel changed in place, unannounced, between the 256 words the former sampled check read
    // (step 1 KiB here): page write tracking must still upload it on the next frame.
    SurfaceDesc bigDesc;bigDesc.addr=mem::host_alloc(256*256*4,256);
    bigDesc.width=256;bigDesc.height=256;bigDesc.pitch=256;bigDesc.slices=1;bigDesc.mips=1;bigDesc.format=0x1a;bigDesc.dim=1;
    for(uint32_t i=0;i<256*256*4;++i)mem::ptr(bigDesc.addr)[i]=rgba[i%4];
    auto* big=find_or_create_surface(bigDesc,false);
    upload_surface(big);
    ++R.frame;
    if(((R.frame+(big->addr>>12))&63)==0)++R.frame;
    upload_surface(big);
    uint64_t bigUploads=g_stat_uploads;
    const uint8_t texel[4]={200,10,20,255};
    memcpy(mem::ptr(bigDesc.addr)+(256+5)*4,texel,4);  // texel (5,1): bytes 1044..1047, not sampled
    ++R.frame;
    if(((R.frame+(big->addr>>12))&63)==0)++R.frame;
    upload_surface(big);
    require(g_stat_uploads==bigUploads+1,"unannounced in-place texel change was not uploaded");
    auto px=read_image(*big,VK_IMAGE_ASPECT_COLOR_BIT,4);
    require(!memcmp(px.data()+(256+5)*4,texel,4)&&!memcmp(px.data(),rgba,4),"unannounced texel change readback differs");
    fprintf(stderr,"[renderer smoke] unannounced in-place texel change between sampled words uploaded next frame\n");
   }
   Image color(16,16,0x1a);const float green[4]={0,1,0,1};const uint8_t greenBytes[4]={0,255,0,255};clear_image(color.s,green);rgba_is(read_image(color.s,VK_IMAGE_ASPECT_COLOR_BIT,4),greenBytes,"color clear differs");
   Image scaled(32,32,0x1a);resample(&color.s,&scaled.s,1);rgba_is(read_image(scaled.s,VK_IMAGE_ASPECT_COLOR_BIT,4),greenBytes,"scaled blit differs");
   // Retire an image while its commands are pending; replacement must not destroy it early.
   clear_image(color.s,green);destroy_surface_image(&color.s);create_surface_image(&color.s,true);const float blue[4]={0,0,1,1};const uint8_t blueBytes[4]={0,0,255,255};clear_image(color.s,blue);rgba_is(read_image(color.s,VK_IMAGE_ASPECT_COLOR_BIT,4),blueBytes,"deferred image replacement differs");
   fprintf(stderr,"[renderer smoke] clear, scaled blit and deferred image replacement passed\n");
   Image depth(16,16,0x11,true);depth.s.addr=mem::host_alloc(65536,256);
   const uint32_t packedDepth=0x5a800000;for(uint32_t i=0;i<65536;i+=4)memcpy(mem::ptr(depth.s.addr)+i,&packedDepth,4);
   upload_surface(&depth.s);
   textureWords[0]=1;require(sampled_texture_view(&depth.s,textureWords)!=VK_NULL_HANDLE,"sampled depth-only view creation failed");
   auto depths=read_image(depth.s,VK_IMAGE_ASPECT_DEPTH_BIT,4);auto stencils=read_image(depth.s,VK_IMAGE_ASPECT_STENCIL_BIT,1);
   for(size_t i=0;i<stencils.size();++i){float value;memcpy(&value,depths.data()+i*4,4);require(std::abs(value-0.5f)<0.00001f&&stencils[i]==0x5a,"depth/stencil guest upload differs");}
   transition_image(&depth.s,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
   VkClearDepthStencilValue dv{0.25f,0xa5};VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};vkCmdClearDepthStencilImage(command_buffer(),depth.s.image,depth.s.layout,&dv,1,&range);
   depths=read_image(depth.s,VK_IMAGE_ASPECT_DEPTH_BIT,4);stencils=read_image(depth.s,VK_IMAGE_ASPECT_STENCIL_BIT,1);
   for(size_t i=0;i<stencils.size();++i){float value;memcpy(&value,depths.data()+i*4,4);require(value==0.25f&&stencils[i]==0xa5,"depth/stencil clear differs");}
   fprintf(stderr,"[renderer smoke] depth/stencil upload and clear passed\n");
   Image rendered(64,64,0x1a);dynamic_uniform_check(rendered.s);vertex_window_check(rendered.s);triangle(rendered.s);
   require(record_signature(0,rendered.s,false),"signature GPU copy could not be recorded");
   flush_async(); // The signature reader must wait on its recorded token.
   auto luma=read_signature(0);
   require(luma.size()==gfx::kSignatureW*gfx::kSignatureH,"signature readback size differs");
   require(std::abs(luma[(gfx::kSignatureH/2)*gfx::kSignatureW+gfx::kSignatureW/2]-0.299f)<0.002f,
       "signature readback center differs");
   reset_signatures();
   fprintf(stderr,"[renderer smoke] submitted signature fence/readback and deferred reset passed\n");
   if(R.tv.scan)destroy_surface_image(R.tv.scan.get());R.tv.scan=std::make_unique<Surface>();auto& scan=*R.tv.scan;scan.width=64;scan.height=64;scan.format=0x1a;scan.fmt=format_info(scan.format,false);create_surface_image(&scan,false);resample(&rendered.s,&scan,1);mark_gpu_written(&scan);
   auto capturePath=std::filesystem::temp_directory_path()/("wwhd-vulkan-smoke-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".png");
   request_tv_dump(capturePath.string(),0);swap();
   std::ifstream capture(capturePath,std::ios::binary);std::vector<uint8_t> png((std::istreambuf_iterator<char>(capture)),std::istreambuf_iterator<char>());
   const uint8_t signature[8]={137,80,78,71,13,10,26,10};require(png.size()>8&&memcmp(png.data(),signature,8)==0,"PNG capture signature differs");
   auto get32=[&](size_t offset){return (uint32_t(png[offset])<<24)|(uint32_t(png[offset+1])<<16)|(uint32_t(png[offset+2])<<8)|png[offset+3];};
   std::vector<uint8_t> compressed;bool header=false;
   for(size_t offset=8;offset+12<=png.size();) {
    uint32_t size=get32(offset);require(size<=png.size()-offset-12,"PNG capture chunk length differs");
    const uint8_t* type=png.data()+offset+4;const uint8_t* data=type+4;
    require(uint32_t(crc32(0,type,size+4))==get32(offset+8+size),"PNG capture chunk CRC differs");
    if(memcmp(type,"IHDR",4)==0){require(size==13&&get32(offset+8)==64&&get32(offset+12)==64,"PNG capture dimensions differ");header=true;}
    if(memcmp(type,"IDAT",4)==0)compressed.insert(compressed.end(),data,data+size);
    offset+=size+12;
   }
   std::vector<uint8_t> decoded((64*4+1)*64);uLongf decodedSize=decoded.size();require(header&&uncompress(decoded.data(),&decodedSize,compressed.data(),compressed.size())==Z_OK&&decodedSize==decoded.size(),"PNG capture decompression differs");
   size_t pngCenter=32*(64*4+1)+1+32*4;require(decoded[pngCenter]==255&&decoded[pngCenter+1]==0&&decoded[pngCenter+2]==0,"PNG capture triangle center differs");
   fprintf(stderr,"[renderer smoke] queued GPU PNG capture passed: %s\n",capturePath.string().c_str());
   require(R.tv.swapchain!=VK_NULL_HANDLE,"smoke presentation did not create a swapchain");fprintf(stderr,"[renderer smoke] scan-buffer swapchain presentation passed\n");
   // Wrap submission slots and reacquire swap images in the normal swap path.
   // WWHD_VK_ASYNC=1 exercises the Switch path on a desktop validation driver.
   for(uint32_t frame=0;frame<6;++frame) {
    const float color[4]={float(frame&1),float((frame>>1)&1),0,1};
    clear_image(scan,color);swap();
   }
   fprintf(stderr,"[renderer smoke] repeated frame submission and swap-image semaphore reuse passed\n");
  }
  // Ensure deferred objects left by readback and stack-owned images are actually reclaimed.
  command_buffer();wait_idle();require(R.garbageBuffers.empty()&&R.garbageImages.empty(),"deferred Vulkan resources were not reclaimed");
  save_pipeline_cache();
  fprintf(stderr,"[renderer smoke] PASS: actual device upload/clear/blit/depth/triangle/present\n");return 0;
 }catch(const std::exception& e){fprintf(stderr,"[renderer smoke] FAIL: %s\n",e.what());try {command_buffer();wait_idle();}catch(...){}return 1;}
}
} // namespace gfxvk
