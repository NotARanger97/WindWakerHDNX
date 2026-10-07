// Shader presentation matches Metal's source-grid FXAA and scaling filters.
#include "present.h"
#include "backend.h"
#include "shaders.h"
#include "settings.h"
#include "mods/climb.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace gfxvk {
namespace {
const char* vertexSource = R"glsl(#version 450
layout(location=0) out vec2 uv;
void main() {
 vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
 uv=p;
 gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";
const char* fragmentSource = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Params { int aa; int conv; float sharp; float foot; float alpha; } params;
float luma(vec3 c) { return dot(c,vec3(0.299,0.587,0.114)); }
vec3 fxaa(vec2 at) {
 vec2 rcp=1.0/vec2(textureSize(image,0));
 vec3 nw=texture(image,at+vec2(-1,-1)*rcp).rgb, ne=texture(image,at+vec2(1,-1)*rcp).rgb;
 vec3 sw=texture(image,at+vec2(-1,1)*rcp).rgb, se=texture(image,at+vec2(1,1)*rcp).rgb;
 vec3 m=texture(image,at).rgb;
 float lnw=luma(nw),lne=luma(ne),lsw=luma(sw),lse=luma(se),lm=luma(m);
 float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));
 if(lmax-lmin<max(0.0312,lmax*0.125))return m;
 vec2 dir=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));
 float reduce=max((lnw+lne+lsw+lse)*(0.25/8.0),1.0/128.0);
 dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-8.0),vec2(8.0))*rcp;
 vec3 a=0.5*(texture(image,at+dir*(1.0/3.0-0.5)).rgb+texture(image,at+dir*(2.0/3.0-0.5)).rgb);
 vec3 b=a*0.5+0.25*(texture(image,at-dir*0.5).rgb+texture(image,at+dir*0.5).rgb);
 float lb=luma(b);return (lb<lmin||lb>lmax)?a:b;
}
vec3 toLinear(vec3 c) {
 return mix(pow((c+0.055)/1.055,vec3(2.4)),c/12.92,lessThanEqual(c,vec3(0.04045)));
}
vec3 toSrgb(vec3 c) {
 return mix(1.055*pow(c,vec3(1.0/2.4))-0.055,c*12.92,lessThanEqual(c,vec3(0.0031308)));
}
void main() {
 vec2 size=vec2(textureSize(image,0)),at=uv;
 if(params.sharp>1.0) {
  vec2 t=at*size-0.5,i=floor(t),f=t-i;
  f=clamp((f-0.5)*params.sharp+0.5,0.0,1.0);at=(i+f+0.5)/size;
 }
 vec3 c;
 if(params.aa!=0)c=fxaa(at);
 else if(params.foot>1.25) {
  vec2 d=0.25*min(params.foot,4.0)/size;
  c=0.25*(texture(image,at+vec2(-d.x,-d.y)).rgb+texture(image,at+vec2(d.x,-d.y)).rgb+
          texture(image,at+vec2(-d.x,d.y)).rgb+texture(image,at+vec2(d.x,d.y)).rgb);
 }else c=texture(image,at).rgb;
 if(params.conv==1)c=toLinear(clamp(c,0.0,1.0));
 else if(params.conv==2)c=toSrgb(clamp(c,0.0,1.0));
 result=vec4(c,params.alpha);
}
)glsl";
// a filled rectangle (GamePad overlay frame)
const char* solidSource = R"glsl(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Params { vec4 color; } params;
void main() { result=params.color; }
)glsl";
// stamina wheel of the "climb any wall" mod (as mods/climb_hud.mm), premultiplied
const char* hudSource = R"glsl(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Params { float stamina; float alpha; float exhausted; float pad; } u;
void main() {
 vec2 p=vec2(uv.x*2.0-1.0,1.0-uv.y*2.0);
 float r=length(p);
 float aa=max(fwidth(r),1e-4);
 float ring=smoothstep(0.58-aa,0.58+aa,r)*(1.0-smoothstep(0.92-aa,0.92+aa,r));
 float outline=smoothstep(0.50-aa,0.50+aa,r)*(1.0-smoothstep(1.0-2.0*aa,1.0,r));
 float ang=atan(p.x,p.y);
 float frac=(ang<0.0?ang+6.28318530718:ang)/6.28318530718;
 float filled=1.0-smoothstep(u.stamina-0.004,u.stamina+0.004,frac);
 vec3 green=vec3(0.30,0.90,0.35),yellow=vec3(1.0,0.80,0.15),red=vec3(0.95,0.20,0.15);
 vec3 full=u.exhausted>0.5?red:(u.stamina<0.3?mix(red,yellow,u.stamina/0.3):green);
 vec3 empty=u.exhausted>0.5?vec3(0.55,0.08,0.06):vec3(0.10,0.10,0.10);
 vec3 col=mix(empty,full,filled);
 float fa=ring*mix(0.45,0.95,filled);
 float oa=outline*0.45;
 float a=fa+oa*(1.0-fa);
 result=vec4(col*fa,a)*u.alpha;
}
)glsl";
struct PresentRect { float x,y,width,height,scale; };
struct PresentParams { int32_t aa,conv; float sharp,foot; float alpha=1,pad[3]{}; };
static_assert(sizeof(PresentParams)==32);
PresentRect present_rect(VkExtent3D source,VkExtent2D target,int filter) {
 float scale=std::min(float(target.width)/source.width,float(target.height)/source.height);
 if(filter==2&&scale>=1)scale=std::floor(scale+1e-3f);
 const float width=source.width*scale,height=source.height*scale;
 return {(target.width-width)*0.5f,(target.height-height)*0.5f,width,height,scale};
}
PresentParams present_params(bool aa,int filter,float scale,bool sourceLinear,bool targetLinear) {
 return {aa?1:0,sourceLinear==targetLinear?0:targetLinear?1:2,
  filter==0||scale<=1?1:filter==1?scale:1e4f,scale<1?1/scale:1};
}
// pipeline kinds: the scaled picture (opaque / with opacity), a filled rectangle, the mod HUD
enum Kind : uint32_t { kImage, kImageBlend, kSolid, kHud };
struct ScreenResources { bool drawable=false,captureTransfer=false; std::vector<VkImageView> views; };
struct PresentCapture {
 std::string path;
 uint64_t frame=0;
 bool enabled=false,done=false;
 Buffer buffer{};
 uint64_t submission=0;
 VkExtent2D extent{};
 VkFormat format=VK_FORMAT_UNDEFINED;
};
PresentCapture& present_capture() {
 static PresentCapture capture=[] {
  PresentCapture result;
  const char* path=std::getenv("WWHD_PRESENT_CAPTURE_PATH");
  const char* frame=std::getenv("WWHD_PRESENT_CAPTURE_FRAME");
  if(!path||!*path||!frame||!*frame)return result;
  uint64_t value=0;
  for(const char* p=frame;*p;++p) {
   if(*p<'0'||*p>'9')return result;
   const uint64_t digit=uint64_t(*p-'0');
   if(value>(UINT64_MAX-digit)/10)return result;
   value=value*10+digit;
  }
  result.path=path;result.frame=value;result.enabled=true;return result;
 }();
 return capture;
}
struct PresentResources {
 VkDevice device=VK_NULL_HANDLE;
 VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
 VkPipelineLayout layout=VK_NULL_HANDLE;
 VkSampler linear=VK_NULL_HANDLE,nearest=VK_NULL_HANDLE;
 std::unordered_map<uint64_t,VkPipeline> pipelines; // (format, kind)
 std::unordered_map<Screen*,ScreenResources> screens;
};
PresentResources resources;
bool srgb_format(VkFormat format) {
 return format==VK_FORMAT_R8G8B8A8_SRGB || format==VK_FORMAT_B8G8R8A8_SRGB ||
        format==VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
VkShaderModule module(const char* source,bool vertex) {
 std::string error;auto words=vk::compile_glsl(source,vertex,&error);
 if(words.empty()||!error.empty())throw std::runtime_error("Vulkan presentation shader: "+error);
 VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();
 VkShaderModule result;vk_check(vkCreateShaderModule(R.device,&ci,nullptr,&result),"presentation shader");return result;
}
void ensure_resources() {
 if(resources.device && resources.device!=R.device)
  throw std::runtime_error("Vulkan presentation resources must be reset before device replacement");
 resources.device=R.device;
 if(!resources.descriptors) {
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=1;ci.pBindings=&binding;
  vk_check(vkCreateDescriptorSetLayout(R.device,&ci,nullptr,&resources.descriptors),"presentation descriptors");
 }
 if(!resources.layout) {
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PresentParams)};
  VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};ci.setLayoutCount=1;ci.pSetLayouts=&resources.descriptors;ci.pushConstantRangeCount=1;ci.pPushConstantRanges=&push;
  vk_check(vkCreatePipelineLayout(R.device,&ci,nullptr,&resources.layout),"presentation layout");
 }
 for(auto filter:{VK_FILTER_LINEAR,VK_FILTER_NEAREST}) {
  auto& sampler=filter==VK_FILTER_LINEAR?resources.linear:resources.nearest;
  if(sampler)continue;
  VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};ci.minFilter=ci.magFilter=filter;ci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU=ci.addressModeV=ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;ci.maxLod=0;
  vk_check(vkCreateSampler(R.device,&ci,nullptr,&sampler),"presentation sampler");
 }
}
VkPipeline pipeline(VkFormat format,Kind kind=kImage) {
 ensure_resources();const uint64_t key=uint64_t(format)<<8|kind;
 if(auto it=resources.pipelines.find(key);it!=resources.pipelines.end())return it->second;
 VkShaderModule vs=VK_NULL_HANDLE,fs=VK_NULL_HANDLE;VkPipeline result=VK_NULL_HANDLE;
 try {
  vs=module(vertexSource,true);fs=module(kind==kSolid?solidSource:kind==kHud?hudSource:fragmentSource,false);
  VkPipelineShaderStageCreateInfo stages[2]{};
  for(int i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?fs:vs;stages[i].pName="main";}
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
  if(kind==kImageBlend||kind==kSolid) {
   // overlay opacity: source alpha over the picture underneath
   blend.blendEnable=VK_TRUE;blend.colorBlendOp=blend.alphaBlendOp=VK_BLEND_OP_ADD;
   blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
   blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  } else if(kind==kHud) {
   // premultiplied colour; the image keeps its alpha
   blend.blendEnable=VK_TRUE;blend.colorBlendOp=blend.alphaBlendOp=VK_BLEND_OP_ADD;
   blend.srcColorBlendFactor=VK_BLEND_FACTOR_ONE;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
  }
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&blend;
  VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&format;
  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&rendering;ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pColorBlendState=&bs;ci.pDynamicState=&ds;ci.layout=resources.layout;
  const auto started=std::chrono::steady_clock::now();
  vk_check(vkCreateGraphicsPipelines(R.device,R.pipelineCache,1,&ci,nullptr,&result),"presentation pipeline");
  R.pipelineCreateNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
  resources.pipelines.emplace(key,result);++R.pipelineCreates;R.pipelineCacheDirty=true;R.pipelineCacheChangedFrame=R.frame;
 }catch(...){if(result)vkDestroyPipeline(R.device,result,nullptr);if(vs)vkDestroyShaderModule(R.device,vs,nullptr);if(fs)vkDestroyShaderModule(R.device,fs,nullptr);throw;}
 vkDestroyShaderModule(R.device,vs,nullptr);vkDestroyShaderModule(R.device,fs,nullptr);return result;
}
}
void reset_present_screen(Screen& screen) {
 auto found=resources.screens.find(&screen);if(found==resources.screens.end())return;
 for(auto view:found->second.views)vkDestroyImageView(resources.device,view,nullptr);
 resources.screens.erase(found);
}
void prepare_present_screen(Screen& screen,bool colorAttachmentSupported,bool captureTransferSupported) {
 ensure_resources();reset_present_screen(screen);auto& state=resources.screens[&screen];state.drawable=colorAttachmentSupported;state.captureTransfer=captureTransferSupported;
 // Capability publication happens at draw time, after a guest scan image
 // exists, and refreshes when the guest replaces that image's format.
 if(!state.drawable)return;
 state.views.reserve(screen.images.size());
 try {
  for(auto image:screen.images) {
   VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.format=screen.swapFormat;ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
   VkImageView view;vk_check(vkCreateImageView(R.device,&ci,nullptr,&view),"presentation image view");state.views.push_back(view);
  }
 }catch(...){reset_present_screen(screen);throw;}
}
void reset_present_resources() {
 if(present_capture().buffer.buffer) { defer_buffer(present_capture().buffer);present_capture().buffer={}; }
 // Caller has drained the device; command/descriptor pools must not execute
 // references to these process-lifetime presentation objects afterwards.
 for(auto& [screen,state]:resources.screens)for(auto view:state.views)vkDestroyImageView(resources.device,view,nullptr);
 for(auto [key,p]:resources.pipelines)vkDestroyPipeline(resources.device,p,nullptr);
 if(resources.linear)vkDestroySampler(resources.device,resources.linear,nullptr);
 if(resources.nearest)vkDestroySampler(resources.device,resources.nearest,nullptr);
 if(resources.layout)vkDestroyPipelineLayout(resources.device,resources.layout,nullptr);
 if(resources.descriptors)vkDestroyDescriptorSetLayout(resources.device,resources.descriptors,nullptr);
 resources={};
 reset_overlay_resources();
}
// ---------------------------------------------------------------- composition
namespace {
const gfx::PresentPlan* currentPlan=nullptr;
bool linear_filtering(VkFormat format) {
 VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(R.physicalDevice,format,&properties);
 return properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}
bool sampleable(const Surface& source) {
 VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(R.physicalDevice,source.fmt.pixel,&properties);
 return (properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (source.usage&VK_IMAGE_USAGE_SAMPLED_BIT);
}
// draw the quads into `view` (cleared to black first); `layout` is the target image's current layout
void compose(VkImage image,VkImageView view,VkImageLayout& layout,VkExtent2D extent,VkFormat format,
             const std::vector<ComposeQuad>& quads,VkImageLayout finalLayout,int filter,bool fxaa,ImDrawData* overlay=nullptr) {
 end_encoder();
 if(overlay)overlay_prepare(overlay);
 for(auto& q:quads)
  if(q.image)transition_image(q.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
 auto cmd=command_buffer();VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=layout;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
 VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=view;attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 attachment.clearValue.color.float32[3]=1;
 VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.renderArea.extent=extent;rendering.layerCount=1;rendering.colorAttachmentCount=1;rendering.pColorAttachments=&attachment;
 vkCmdBeginRendering(cmd,&rendering);R.rendering=true;R.passTracked=false;
 VkRect2D scissor{{0,0},extent};vkCmdSetScissor(cmd,0,1,&scissor);
 const bool targetLinear=srgb_format(format);
 for(auto& q:quads) {
  if(q.box.w<=0||q.box.h<=0)continue;
  VkViewport viewport{q.box.x,q.box.y,q.box.w,q.box.h,0,1};vkCmdSetViewport(cmd,0,1,&viewport);
  if(q.solid) {
   vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline(format,kSolid));
   PresentParams params{};memcpy(&params,q.color,16);
   vkCmdPushConstants(cmd,resources.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(params),&params);vkCmdDraw(cmd,3,1,0,0);
   continue;
  }
  if(!q.image||!q.image->view)continue;
  const bool blend=q.alpha<0.999f;
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline(format,blend?kImageBlend:kImage));
  VkDescriptorSet set;VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=R.descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&resources.descriptors;
  vk_check(vkAllocateDescriptorSets(R.device,&allocation,&set),"presentation descriptor set");
  const bool linear=linear_filtering(q.image->fmt.pixel);
  VkDescriptorImageInfo info{linear?resources.linear:resources.nearest,q.image->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&info;
  vkUpdateDescriptorSets(R.device,1,&write,0,nullptr);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,resources.layout,0,1,&set,0,nullptr);
  // target pixels per source texel, as the Metal composition (display.mm draw_image)
  const float scale=std::min(q.box.w/q.image->extent.width,q.box.h/q.image->extent.height);
  // sRGB texture views decode on sampling; the scan flag also identifies linear
  // scan values held in a UNORM image. sRGB targets encode shader linear output.
  const bool sourceLinear=q.sourceLinear||srgb_format(q.image->fmt.pixel);
  auto params=present_params(fxaa&&linear,filter,scale,sourceLinear,targetLinear);params.alpha=q.alpha;
  vkCmdPushConstants(cmd,resources.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(params),&params);vkCmdDraw(cmd,3,1,0,0);
 }
 if(overlay)overlay_draw(overlay,cmd,format,extent,targetLinear);  // settings overlay on top
 vkCmdEndRendering(cmd);
 barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=finalLayout;barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=0;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);layout=finalLayout;
 R.rendering=false;R.passTracked=false;
}
}  // namespace

void set_present_plan(const gfx::PresentPlan* plan) { currentPlan=plan; }
namespace { ImDrawData* overlayDraw=nullptr; }
void set_overlay_draw(ImDrawData* draw) { overlayDraw=draw; }

std::vector<ComposeQuad> screen_quads(Screen& screen,VkExtent2D target,int& filter) {
 std::vector<ComposeQuad> quads;
 filter=scale_filter();
 if(!screen.scan||!screen.scan->image)return quads;
 if(currentPlan) {
  // both window hosts (gfx/display_modes.cpp): the same layout as the Metal renderer
  filter=currentPlan->filter;
  if(&screen==&R.drc) {
   ComposeQuad q;q.image=R.drc.scan.get();q.sourceLinear=R.drc.srgb.load();
   q.box=gfx::display_layout(float(target.width),float(target.height),float(q.image->extent.width),float(q.image->extent.height));
   quads.push_back(q);return quads;
  }
  gfx::PresentPlan p=*currentPlan;
  Surface* drc=R.drc.scan&&R.drc.scan->image?R.drc.scan.get():nullptr;
  if(p.dw!=float(target.width)||p.dh!=float(target.height))
   p=gfx::display_plan_for(p,float(target.width),float(target.height),float(screen.scan->extent.width),float(screen.scan->extent.height),
                          drc?float(drc->extent.width):0,drc?float(drc->extent.height):0);
  ComposeQuad tv;tv.image=screen.scan.get();tv.sourceLinear=screen.srgb.load();tv.box=p.tv;quads.push_back(tv);
  if(p.pip_on&&drc) {
   const float op=p.pip_opacity,bw=std::max(1.0f,std::round(p.pip.w/200.0f));
   ComposeQuad frame;frame.solid=true;frame.color[3]=0.6f*op;
   frame.box={p.pip.x-bw,p.pip.y-bw,p.pip.w+2*bw,p.pip.h+2*bw};quads.push_back(frame);
   ComposeQuad pip;pip.image=drc;pip.sourceLinear=R.drc.srgb.load();pip.box=p.pip;pip.alpha=op;quads.push_back(pip);
  }
  if(p.button.w>0) {
   // touch screens' view button (display_modes.h): a dark square with two light "screens" (rhemfur's
   // Android design) and the host's state dot (Android: 60 fps on / paused)
   const gfx::Box& t=p.button;
   ComposeQuad b;b.solid=true;b.color[3]=0.45f;b.box={t.x,t.y,t.w,t.h};quads.push_back(b);
   for(int i=0;i<2;i++) {
    ComposeQuad s;s.solid=true;s.color[0]=s.color[1]=s.color[2]=0.9f;s.color[3]=0.8f;
    s.box={t.x+t.w*(i?0.45f:0.15f),t.y+t.h*(i?0.45f:0.2f),t.w*0.4f,t.h*0.32f};quads.push_back(s);
   }
   if(p.button_dot) {
    const bool on=p.button_dot==1;
    ComposeQuad d;d.solid=true;d.color[0]=on?0.2f:0.95f;d.color[1]=on?0.85f:0.8f;d.color[2]=on?0.3f:0.15f;d.color[3]=0.95f;
    d.box={t.x+t.w*0.72f,t.y+t.h*0.06f,t.w*0.22f,t.h*0.22f};quads.push_back(d);
   }
  }
  return quads;
 }
 // no plan (outside swap): the picture scaled to fit (Codex's presentation)
 const auto rect=present_rect(screen.scan->extent,target,filter);
 ComposeQuad q;q.image=screen.scan.get();q.sourceLinear=screen.srgb.load();q.box={rect.x,rect.y,rect.width,rect.height};quads.push_back(q);
 return quads;
}

bool draw_present_screen(Screen& screen,uint32_t imageIndex) {
 auto found=resources.screens.find(&screen);
 if(found==resources.screens.end()||!found->second.drawable) {
  if(&screen==&R.tv) {
   set_graphics_feature_available(GraphicsFeature::FXAA,false);
   set_graphics_feature_available(GraphicsFeature::ScaleFilter,false);
  }
  if(fxaa_enabled())throw std::runtime_error("FXAA requires color-attachment presentation support");
  if(scale_filter()!=0)throw std::runtime_error("Sharp/integer filtering requires shader presentation support");
  return false;
 }
 auto& source=*screen.scan;
 if(!sampleable(source)) {
  if(&screen==&R.tv) {
   set_graphics_feature_available(GraphicsFeature::FXAA,false);
   set_graphics_feature_available(GraphicsFeature::ScaleFilter,false);
  }
  throw std::runtime_error("Scan-buffer format cannot be sampled for shader presentation");
 }
 const bool linear=linear_filtering(source.fmt.pixel);
 if(&screen==&R.tv) {
  set_graphics_feature_available(GraphicsFeature::FXAA,linear);
  set_graphics_feature_available(GraphicsFeature::ScaleFilter,linear);
 }
 if(fxaa_enabled()&&!linear)throw std::runtime_error("FXAA requires linear scan-buffer filtering");
 int filter=0;auto quads=screen_quads(screen,screen.swapExtent,filter);
 compose(screen.images.at(imageIndex),found->second.views.at(imageIndex),screen.layouts.at(imageIndex),screen.swapExtent,screen.swapFormat,
         quads,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,filter,fxaa_enabled(),&screen==&R.tv?overlayDraw:nullptr);
 return true;
}

// the composition into an offscreen RGBA8 image, read back (present dumps, captures)
std::vector<uint8_t> compose_offscreen(Screen& screen,uint32_t width,uint32_t height,bool srgb) {
 if(!width||!height)return {};
 Surface target;target.width=width;target.height=height;target.format=srgb?0x41a:0x1a;target.fmt=format_info(target.format,false);
 create_surface_image(&target,true,VkExtent3D{width,height,1});
 std::vector<uint8_t> rgba;
 try {
  int filter=0;auto quads=screen_quads(screen,VkExtent2D{width,height},filter);
  compose(target.image,target.view,target.layout,VkExtent2D{width,height},target.fmt.pixel,quads,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,filter,fxaa_enabled(),&screen==&R.tv?overlayDraw:nullptr);
  rgba=read_surface_rgba(target,false);  // display-encoded already (sRGB target, or encoded values)
 }catch(...){destroy_surface_image(&target);throw;}
 destroy_surface_image(&target);
 return rgba;
}

// ---------------------------------------------------------------- the climb mod's stamina wheel
void draw_mod_overlay(Surface& scan) {
 if(!mods::climb_enabled()||!scan.image||!scan.view||!(scan.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))return;
 const mods::ClimbHud hud=mods::climb_hud();
 if(hud.alpha<=0.0f)return;
 transition_image(&scan,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
 auto cmd=command_buffer();
 VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=scan.view;attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 const VkExtent2D extent{scan.extent.width,scan.extent.height};
 VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.renderArea.extent=extent;rendering.layerCount=1;rendering.colorAttachmentCount=1;rendering.pColorAttachments=&attachment;
 vkCmdBeginRendering(cmd,&rendering);R.rendering=true;R.passTracked=false;
 // upper right of the screen centre, where the follow camera keeps Link (climb_hud.mm)
 const float w=float(extent.width),h=float(extent.height),cx=0.60f*w,cy=0.38f*h,rad=0.05f*h;
 VkViewport viewport{cx-rad,cy-rad,2*rad,2*rad,0,1};VkRect2D scissor{{0,0},extent};
 vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline(scan.fmt.pixel,kHud));
 PresentParams params{};const float u[4]={hud.stamina,hud.alpha,hud.exhausted?1.0f:0.0f,0};memcpy(&params,u,16);
 vkCmdPushConstants(cmd,resources.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(params),&params);vkCmdDraw(cmd,3,1,0,0);
 vkCmdEndRendering(cmd);R.rendering=false;R.passTracked=false;
}

// ---------------------------------------------------------------- automatic overlay signatures
// A picture reduced to 32x18 like the Metal renderer does it (display.mm Sig): a box-filtered mip
// chain, then a linear reduction of the first level at least 32 wide; read back after the frame's fence.
namespace {
struct Signature {
 VkImage mip=VK_NULL_HANDLE,signatureImage=VK_NULL_HANDLE;VkDeviceMemory mipMemory=VK_NULL_HANDLE,smallMemory=VK_NULL_HANDLE;
 uint32_t width=0,height=0,levels=0;Buffer buffer{};bool pending=false,linear=false;
 uint64_t submission=0;
};
Signature signatures[2];
void make_image(VkImage& image,VkDeviceMemory& memory,uint32_t w,uint32_t h,uint32_t levels) {
 VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={w,h,1};ci.mipLevels=levels;ci.arrayLayers=1;
 ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
 vk_check(vkCreateImage(R.device,&ci,nullptr,&image),"signature image");
 VkMemoryRequirements req;vkGetImageMemoryRequirements(R.device,image,&req);
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
 vk_check(vkAllocateMemory(R.device,&ai,nullptr,&memory),"signature memory");vk_check(vkBindImageMemory(R.device,image,memory,0),"signature bind");
}
void level_barrier(VkCommandBuffer cmd,VkImage image,uint32_t level,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst) {
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=from;b.newLayout=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,1,0,1};b.srcAccessMask=src;b.dstAccessMask=dst;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
}
}  // namespace

bool record_signature(int slot,Surface& source,bool sourceLinear) {
 VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(R.physicalDevice,source.fmt.pixel,&properties);
 if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||source.fmt.kind!=FormatInfo::FLOAT)return false;
 auto& g=signatures[slot];
 const uint32_t w=source.extent.width,h=source.extent.height;
 if(g.width!=w||g.height!=h||!g.mip) {
  if(g.mip){defer_surface_image(g.mip,g.mipMemory,{});g.mip=VK_NULL_HANDLE;}
  uint32_t levels=1;while((w>>levels)>=uint32_t(gfx::kSignatureW)&&(h>>levels)>=1)levels++;
  make_image(g.mip,g.mipMemory,w,h,levels);g.width=w;g.height=h;g.levels=levels;
 }
 if(!g.signatureImage) {
  make_image(g.signatureImage,g.smallMemory,gfx::kSignatureW,gfx::kSignatureH,1);
  g.buffer=create_buffer(gfx::kSignatureW*gfx::kSignatureH*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 }
 transition_image(&source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
 auto cmd=command_buffer();
 const VkAccessFlags W=VK_ACCESS_TRANSFER_WRITE_BIT,Rd=VK_ACCESS_TRANSFER_READ_BIT;
 level_barrier(cmd,g.mip,0,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,W);
 VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
 blit.srcOffsets[1]={int(w),int(h),1};blit.dstOffsets[1]={int(w),int(h),1};
 vkCmdBlitImage(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.mip,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
 for(uint32_t l=1;l<g.levels;l++) {
  level_barrier(cmd,g.mip,l-1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,W,Rd);
  level_barrier(cmd,g.mip,l,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,W);
  VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l-1,0,1};b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1};
  b.srcOffsets[1]={int(std::max(w>>(l-1),1u)),int(std::max(h>>(l-1),1u)),1};b.dstOffsets[1]={int(std::max(w>>l,1u)),int(std::max(h>>l,1u)),1};
  vkCmdBlitImage(cmd,g.mip,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.mip,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,VK_FILTER_LINEAR);
 }
 const uint32_t last=g.levels-1;
 level_barrier(cmd,g.mip,last,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,W,Rd);
 level_barrier(cmd,g.signatureImage,0,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,W);
 VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,last,0,1};b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
 b.srcOffsets[1]={int(std::max(w>>last,1u)),int(std::max(h>>last,1u)),1};b.dstOffsets[1]={int(gfx::kSignatureW),int(gfx::kSignatureH),1};
 vkCmdBlitImage(cmd,g.mip,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.signatureImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,VK_FILTER_LINEAR);
 level_barrier(cmd,g.signatureImage,0,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,W,Rd);
 VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={gfx::kSignatureW,gfx::kSignatureH,1};
 vkCmdCopyImageToBuffer(cmd,g.signatureImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.buffer.buffer,1,&copy);
 VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=W;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=g.buffer.buffer;host.size=VK_WHOLE_SIZE;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
 // the blits decode sRGB images to linear values: encode them like the Metal path does
 g.linear=sourceLinear||srgb_format(source.fmt.pixel);g.pending=true;
 g.submission=recording_submission();
 return true;
}
// Wait only for the submission containing the signature's GPU-to-host copy.
std::vector<float> read_signature(int slot) {
 auto& g=signatures[slot];
 if(!g.pending)return {};
 wait_submission(g.submission);
 g.pending=false;
 const auto* px=static_cast<const uint8_t*>(g.buffer.mapped);
 std::vector<float> v(gfx::kSignatureW*gfx::kSignatureH);
 auto enc=[&](uint8_t c) {
  float x=c/255.0f;
  if(g.linear)x=x<=0.0031308f?x*12.92f:1.055f*std::pow(x,1/2.4f)-0.055f;
  return x;
 };
 for(size_t i=0;i<v.size();i++)v[i]=0.299f*enc(px[i*4])+0.587f*enc(px[i*4+1])+0.114f*enc(px[i*4+2]);
 return v;
}
void reset_signatures() {
 for(auto& g:signatures) {
  if(g.mip)defer_surface_image(g.mip,g.mipMemory,{});
  if(g.signatureImage)defer_surface_image(g.signatureImage,g.smallMemory,{});
  if(g.buffer.buffer)defer_buffer(g.buffer);
  g=Signature{};
 }
}

bool present_capture_requested() { return present_capture().enabled; }
void record_present_capture(Screen& screen,uint32_t imageIndex) {
 auto& capture=present_capture();
 if(!capture.enabled||capture.done||&screen!=&R.tv||R.frame<capture.frame)return;
 capture.done=true;
 auto found=resources.screens.find(&screen);
 if(found==resources.screens.end()||!found->second.captureTransfer) {
  std::fprintf(stderr,"[vulkan present] capture unavailable: swapchain lacks transfer-source support\n");return;
 }
 if(screen.swapFormat!=VK_FORMAT_R8G8B8A8_UNORM && screen.swapFormat!=VK_FORMAT_R8G8B8A8_SRGB &&
    screen.swapFormat!=VK_FORMAT_B8G8R8A8_UNORM && screen.swapFormat!=VK_FORMAT_B8G8R8A8_SRGB) {
  std::fprintf(stderr,"[vulkan present] capture unsupported swap format %d\n",int(screen.swapFormat));return;
 }
 try {
  const size_t width=screen.swapExtent.width,height=screen.swapExtent.height;
  if(!width||!height||width>std::numeric_limits<size_t>::max()/height/4)
   throw std::runtime_error("invalid swap-image capture dimensions");
  capture.buffer=create_buffer(width*height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  capture.extent=screen.swapExtent;capture.format=screen.swapFormat;
  auto cmd=command_buffer();VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  image.oldLayout=screen.layouts.at(imageIndex);image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  image.image=screen.images.at(imageIndex);image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  image.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={uint32_t(width),uint32_t(height),1};
  vkCmdCopyImageToBuffer(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,capture.buffer.buffer,1,&copy);
  VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=capture.buffer.buffer;host.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
  image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=0;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&image);
  screen.layouts[imageIndex]=image.newLayout;
  capture.submission=recording_submission();
 }catch(const std::exception& error) {
  if(capture.buffer.buffer){defer_buffer(capture.buffer);capture.buffer={};}
  std::fprintf(stderr,"[vulkan present] capture failed: %s\n",error.what());
 }
}
void finish_present_capture(Screen& screen) {
 auto& capture=present_capture();if(&screen!=&R.tv||!capture.buffer.buffer)return;
 // The normal present submission is asynchronous on Switch. Its fence covers
 // this host copy (and the acquire wait), but not the later present wait.
 wait_submission(capture.submission);
 try {
  const size_t count=size_t(capture.extent.width)*capture.extent.height;
  const auto* raw=static_cast<const uint8_t*>(capture.buffer.mapped);std::vector<uint8_t> rgba(count*4);
  const bool bgra=capture.format==VK_FORMAT_B8G8R8A8_UNORM||capture.format==VK_FORMAT_B8G8R8A8_SRGB;
  for(size_t i=0;i<count;++i){rgba[4*i]=raw[4*i+(bgra?2:0)];rgba[4*i+1]=raw[4*i+1];rgba[4*i+2]=raw[4*i+(bgra?0:2)];rgba[4*i+3]=255;}
  // Swap-image bytes are already display encoded; never apply capture gamma.
  write_rgba_png(capture.path,capture.extent.width,capture.extent.height,rgba);
  std::fprintf(stderr,"[vulkan present] wrote actual swap image %s (%ux%u, frame %llu, FXAA %d, filter %d)\n",
      capture.path.c_str(),capture.extent.width,capture.extent.height,(unsigned long long)R.frame,int(fxaa_enabled()),scale_filter());
 }catch(const std::exception& error){std::fprintf(stderr,"[vulkan present] capture failed: %s\n",error.what());}
 defer_buffer(capture.buffer);capture.buffer={};
}

}
