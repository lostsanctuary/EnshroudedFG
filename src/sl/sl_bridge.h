#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
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

// One DLSS-SR evaluation's inputs, captured by the NGX hook (render resolution).
struct ImageRef {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
struct SrInputs {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ImageRef depth, mvec;
    float jitterX = 0, jitterY = 0;    // pixels
    float mvScaleX = 0, mvScaleY = 0;  // NGX MV scale (multiplies MVs into pixels)
    uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
    bool reset = false, depthInverted = false, mvJittered = false;
};
// Called for the game's DLSS-SR evaluate; tags depth/MVs and sets the frame constants.
void OnSrEvaluate(const SrInputs& in);

// HUD-less colour: track the game-facing swapchain images and the game's views of them, and capture
// the image right before the render pass that composites the HUD.
void OnSwapchainImages(uint32_t count, const VkImage* images);
void OnImageViewCreated(VkImageView view, VkImage image);
void OnBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info);

// Snapshot for the overlay.
struct FgStatus {
    bool slActive = false;        // Streamline loaded and device created
    bool fgOn = false;            // DLSS-G currently generating frames
    bool worldFrames = false;     // the game is rendering 3D frames with DLSS-SR inputs
    uint32_t maxMultiplier = 0;   // numFramesToGenerateMax + 1 (6 on RTX 50)
    uint32_t multiplier = 1;      // multiplier in use (Auto's current choice under Auto; 1 = off)
    uint32_t autoMultiplier = 2;  // Auto's current choice
    uint32_t framesPresented = 0; // frames presented per rendered frame (DLSS-G state)
    uint32_t status = 0;          // sl::DLSSGStatus bits
    double baseFps = 0.0;         // rendered (non-generated) frames per second
    uint32_t refreshHz = 0;       // monitor refresh rate
    double outputCapFps = 0.0;    // output cap applied while frame generation runs (0 = none)
};
FgStatus GetFgStatus();

// Frame pacing / latency markers, driven from the game's acquire and present calls.
void OnAcquire();
void BeforePresent();
void AfterPresent(unsigned long long presentCount);

}  // namespace efg::sl_bridge
