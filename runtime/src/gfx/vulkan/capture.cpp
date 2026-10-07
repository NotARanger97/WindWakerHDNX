#include "backend.h"
#include "gx2/gx2.h"
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace gfxvk {
uint64_t frame_count();
namespace {
struct Request {uint64_t frame;std::string path;};
std::mutex captureMutex;
std::vector<Request> captures;
void be32(std::vector<uint8_t>& output,uint32_t value){for(int shift=24;shift>=0;shift-=8)output.push_back(uint8_t(value>>shift));}
void chunk(std::vector<uint8_t>& png,const char* type,const std::vector<uint8_t>& data) {
 if(data.size()>std::numeric_limits<uint32_t>::max())throw std::runtime_error("PNG chunk too large");
 be32(png,uint32_t(data.size()));size_t begin=png.size();png.insert(png.end(),type,type+4);png.insert(png.end(),data.begin(),data.end());
 uLong crc=crc32(0,Z_NULL,0);crc=crc32(crc,png.data()+begin,uInt(data.size()+4));be32(png,uint32_t(crc));
}
void write_png(const std::string& path,uint32_t width,uint32_t height,const std::vector<uint8_t>& rgba) {
 size_t row=size_t(width)*4;std::vector<uint8_t> scanlines((row+1)*height);
 for(uint32_t y=0;y<height;++y){scanlines[y*(row+1)]=0;memcpy(scanlines.data()+y*(row+1)+1,rgba.data()+y*row,row);}
 uLongf compressedSize=compressBound(uLong(scanlines.size()));std::vector<uint8_t> compressed(compressedSize);
 if(compress2(compressed.data(),&compressedSize,scanlines.data(),uLong(scanlines.size()),Z_BEST_SPEED)!=Z_OK)throw std::runtime_error("PNG zlib compression failed");compressed.resize(compressedSize);
 std::vector<uint8_t> png{137,80,78,71,13,10,26,10},header;be32(header,width);be32(header,height);header.insert(header.end(),{8,6,0,0,0});chunk(png,"IHDR",header);chunk(png,"IDAT",compressed);chunk(png,"IEND",{});
 FILE* file=fopen(path.c_str(),"wb");if(!file)throw std::runtime_error("cannot open PNG "+path);size_t written=fwrite(png.data(),1,png.size(),file);int result=fclose(file);if(written!=png.size()||result)throw std::runtime_error("cannot write PNG "+path);
}
float half(uint16_t value) {
 unsigned exponent=(value>>10)&31,mantissa=value&1023;float magnitude;
 if(exponent==0)magnitude=std::ldexp(float(mantissa),-24);
 else if(exponent==31)magnitude=mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity();
 else magnitude=std::ldexp(1.0f+float(mantissa)/1024,int(exponent)-15);
 return value&0x8000?-magnitude:magnitude;
}
uint8_t quantize(float value) {if(std::isnan(value))return 0;return uint8_t(std::clamp(value,0.0f,1.0f)*255+0.5f);}
std::vector<uint8_t> read_rgba(Surface& source,bool encodeSrgb) {
 uint32_t bytes=0;bool alreadySrgb=false;
 switch(source.fmt.pixel) {
 case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_B8G8R8A8_SRGB:alreadySrgb=true;[[fallthrough]];
 case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:case VK_FORMAT_B10G11R11_UFLOAT_PACK32:bytes=4;break;
 case VK_FORMAT_R8_UNORM:bytes=1;break;
 case VK_FORMAT_R16G16B16A16_SFLOAT:bytes=8;break;
 case VK_FORMAT_R32G32B32A32_SFLOAT:bytes=16;break;
 default:throw std::runtime_error("unsupported TV PNG capture VkFormat "+std::to_string(source.fmt.pixel));
 }
 uint32_t width=source.extent.width,height=source.extent.height;
 if(!width||!height||size_t(width)>std::numeric_limits<size_t>::max()/height/bytes)throw std::runtime_error("invalid capture dimensions");
 size_t count=size_t(width)*height;
 Buffer buffer=create_buffer(count*bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 try {
  transition_image(&source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={width,height,1};auto cmd=command_buffer();vkCmdCopyImageToBuffer(cmd,source.image,source.layout,buffer.buffer,1,&region);
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=buffer.buffer;barrier.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);flush_readback();
  const auto* raw=static_cast<const uint8_t*>(buffer.mapped);std::vector<uint8_t> rgba(count*4);
  for(size_t i=0;i<count;++i) {
   float color[3]{};const uint8_t* pixel=raw+i*bytes;
   switch(source.fmt.pixel) {
   case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SRGB:for(unsigned c=0;c<3;++c)color[c]=pixel[c]/255.0f;break;
   case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_B8G8R8A8_SRGB:for(unsigned c=0;c<3;++c)color[c]=pixel[2-c]/255.0f;break;
   case VK_FORMAT_R8_UNORM:color[0]=color[1]=color[2]=pixel[0]/255.0f;break;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:{uint32_t v;memcpy(&v,pixel,4);for(unsigned c=0;c<3;++c)color[c]=float((v>>(c*10))&1023)/1023;break;}
   case VK_FORMAT_R16G16B16A16_SFLOAT:for(unsigned c=0;c<3;++c){uint16_t v;memcpy(&v,pixel+c*2,2);color[c]=half(v);}break;
   case VK_FORMAT_R32G32B32A32_SFLOAT:memcpy(color,pixel,12);break;
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:{uint32_t v;memcpy(&v,pixel,4);for(unsigned c=0;c<3;++c){unsigned bits=c==2?5:6,offset=c==2?22:c*11,m=(v>>offset)&((1u<<bits)-1),e=(v>>(offset+bits))&31;color[c]=e==31?(m?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity()):e?std::ldexp(1.0f+float(m)/(1u<<bits),int(e)-15):std::ldexp(float(m)/(1u<<bits),-14);}break;}
   default:break;
   }
   for(unsigned c=0;c<3;++c){float v=color[c];if(encodeSrgb&&!alreadySrgb)v=v<=0.0031308f?v*12.92f:1.055f*std::pow(v,1/2.4f)-0.055f;rgba[i*4+c]=quantize(v);}rgba[i*4+3]=255;
  }
  defer_buffer(buffer);return rgba;
 }catch(...){defer_buffer(buffer);throw;}
}
}
std::vector<uint8_t> read_surface_rgba(Surface& source,bool encodeSrgb) { return read_rgba(source,encodeSrgb); }
void write_rgba_png(const std::string& path,uint32_t width,uint32_t height,const std::vector<uint8_t>& rgba) {
 write_png(path,width,height,rgba);
}
void request_tv_dump(const std::string& path,int frames_ahead) {
 std::lock_guard lock(captureMutex);captures.push_back({frame_count()+uint64_t(std::max(frames_ahead,0)),path});
}
// Called by the GX2 renderer thread, before swapchain presentation.
void service_captures() {
 if(!R.tv.scan||!R.tv.scan->image)return;
 std::vector<std::string> paths;
 {std::lock_guard lock(captureMutex);for(auto it=captures.begin();it!=captures.end();)if(it->frame<=frame_count()){paths.push_back(std::move(it->path));it=captures.erase(it);}else ++it;}
 if(paths.empty())return;
 try {
  auto& source=*R.tv.scan;auto rgba=read_rgba(source,R.tv.srgb.load());
  for(const auto& path:paths)try {write_png(path,source.extent.width,source.extent.height,rgba);fprintf(stderr,"[gfx] wrote %s (%ux%u)\n",path.c_str(),source.extent.width,source.extent.height);}catch(const std::exception& e){fprintf(stderr,"[gfx] PNG capture failed: %s\n",e.what());}
 }catch(const std::exception& e){fprintf(stderr,"[gfx] TV capture failed: %s\n",e.what());}
}
} // namespace gfxvk
