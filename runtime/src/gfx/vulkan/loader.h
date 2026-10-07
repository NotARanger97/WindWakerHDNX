// Vulkan entry points, loaded at run time (loader.cpp). The executable imports nothing from the Vulkan
// loader (vulkan-1.dll, libvulkan.so.1): a missing loader, or one older than the Vulkan version the
// headers describe, cannot stop the program from starting (Windows refuses to start an executable
// whose imports are missing: issue #37, vkCmdBeginRendering on a Vulkan 1.2 system). The build
// defines VK_NO_PROTOTYPES; these pointers, in namespace gfxvk, carry the usual names, so the
// renderer calls vkCmdDraw(...) as before.
#pragma once
#ifdef __SWITCH__
// The Switch has no Vulkan loader: the build links Mesa NVK directly, and a generated shim
// (tools/switch/gen_vk_switch.py) defines the entry points the renderer uses under their usual names.
#include <vulkan/vulkan.h>
namespace gfxvk {
inline void load_global_functions(PFN_vkGetInstanceProcAddr) {}
inline void load_instance_functions(VkInstance) {}
inline void load_device_functions(VkDevice, bool) {}
}  // namespace gfxvk
#else
#ifndef VK_NO_PROTOTYPES
#error "the Vulkan renderer is built with VK_NO_PROTOTYPES (CMakeLists.txt)"
#endif
#include <vulkan/vulkan.h>

// before an instance exists (vkGetInstanceProcAddr(nullptr, name))
#define WWHD_VK_GLOBAL_FUNCTIONS(X) \
  X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
// instance functions: physical devices and window surfaces
#define WWHD_VK_INSTANCE_FUNCTIONS(X) \
  X(vkCreateDevice) X(vkDestroySurfaceKHR) X(vkEnumerateDeviceExtensionProperties) X(vkEnumeratePhysicalDevices) \
  X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)
// device functions (vkGetDeviceProcAddr): Vulkan 1.0 core and VK_KHR_swapchain
#define WWHD_VK_DEVICE_FUNCTIONS(X) \
  X(vkAcquireNextImageKHR) X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) \
  X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkCmdBindDescriptorSets) \
  X(vkCmdBindIndexBuffer) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBlitImage) \
  X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) \
  X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdPipelineBarrier) \
  X(vkCmdPushConstants) X(vkCmdResetQueryPool) X(vkCmdSetBlendConstants) X(vkCmdSetScissor) \
  X(vkCmdSetStencilReference) X(vkCmdSetViewport) X(vkCmdWriteTimestamp) X(vkCreateBuffer) \
  X(vkCreateCommandPool) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkCreateFence) \
  X(vkCreateGraphicsPipelines) X(vkCreateImage) X(vkCreateImageView) X(vkCreatePipelineCache) \
  X(vkCreatePipelineLayout) X(vkCreateQueryPool) X(vkCreateSampler) X(vkCreateSemaphore) \
  X(vkCreateShaderModule) X(vkCreateSwapchainKHR) X(vkDestroyBuffer) X(vkDestroyDescriptorSetLayout) \
  X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) \
  X(vkDestroyQueryPool) X(vkDestroySampler) X(vkDestroySemaphore) X(vkDestroyShaderModule) \
  X(vkDestroySwapchainKHR) X(vkDeviceWaitIdle) X(vkEndCommandBuffer) X(vkFreeMemory) \
  X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) X(vkGetFenceStatus) X(vkGetImageMemoryRequirements) \
  X(vkGetPipelineCacheData) X(vkGetQueryPoolResults) X(vkGetSwapchainImagesKHR) X(vkMapMemory) \
  X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkResetCommandPool) X(vkResetDescriptorPool) \
  X(vkResetFences) X(vkUnmapMemory) X(vkUpdateDescriptorSets) X(vkWaitForFences)
// dynamic rendering: Vulkan 1.3 core, or VK_KHR_dynamic_rendering on Vulkan 1.1 / 1.2 drivers (the
// ...KHR entry points, loaded under these names; the structures are the same)
#define WWHD_VK_RENDERING_FUNCTIONS(X) X(vkCmdBeginRendering) X(vkCmdEndRendering)

namespace gfxvk {
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
extern PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;  // null: a Vulkan 1.0 loader
extern PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;  // optional (log only)
#define WWHD_VK_DECLARE(name) extern PFN_##name name;
WWHD_VK_GLOBAL_FUNCTIONS(WWHD_VK_DECLARE)
WWHD_VK_INSTANCE_FUNCTIONS(WWHD_VK_DECLARE)
WWHD_VK_DEVICE_FUNCTIONS(WWHD_VK_DECLARE)
WWHD_VK_RENDERING_FUNCTIONS(WWHD_VK_DECLARE)
#undef WWHD_VK_DECLARE

// Each step throws std::runtime_error naming what is missing.
// gipa: the loader's vkGetInstanceProcAddr (SDL_Vulkan_GetVkGetInstanceProcAddr, or the macOS
// loader's export)
void load_global_functions(PFN_vkGetInstanceProcAddr gipa);
void load_instance_functions(VkInstance instance);
// khrDynamicRendering: VK_KHR_dynamic_rendering is enabled (the device is older than Vulkan 1.3)
void load_device_functions(VkDevice device, bool khrDynamicRendering);
}  // namespace gfxvk
#endif  // __SWITCH__
