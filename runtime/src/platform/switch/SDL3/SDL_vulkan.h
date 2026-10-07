// Minimal SDL3 Vulkan surface API on libnx (see SDL.h in this directory).
#pragma once
#include "SDL.h"
#include <vulkan/vulkan.h>

char const* const* SDL_Vulkan_GetInstanceExtensions(Uint32* count);
bool SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);
