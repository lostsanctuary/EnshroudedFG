#pragma once

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace efg::sl_bridge {

// Loads sl.interposer.dll (signature-checked) and calls slInit in manual-hooking mode.
// Called from the vkCreateInstance hook for the game's real (surface-capable) instance.
bool Initialize();
bool Initialized();

// SL proxies. Valid only after Initialize() (instance/device level ones after the
// corresponding object exists). Null when SL is not active.
PFN_vkCreateInstance CreateInstanceProxy();
PFN_vkCreateDevice CreateDeviceProxy(VkInstance instance);
PFN_vkVoidFunction InstanceProxy(VkInstance instance, const char* name);

void OnInstanceCreated(VkInstance instance);
void OnDeviceCreated(VkPhysicalDevice physicalDevice, VkDevice device);
void OnDeviceDestroying(VkDevice device);

// True once the SL device exists; swapchain/present calls on it go through SL.
bool DeviceActive(VkDevice device);
bool QueueActive();

struct DeviceProxies {
    PFN_vkCreateSwapchainKHR createSwapchain;
    PFN_vkDestroySwapchainKHR destroySwapchain;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages;
    PFN_vkAcquireNextImageKHR acquireNextImage;
    PFN_vkQueuePresentKHR queuePresent;
    PFN_vkDeviceWaitIdle deviceWaitIdle;
};
const DeviceProxies& Proxies();

// Frame pacing / latency markers, driven from the game's acquire and present calls.
void OnAcquire();
void BeforePresent();
void AfterPresent(unsigned long long presentCount);

}  // namespace efg::sl_bridge
