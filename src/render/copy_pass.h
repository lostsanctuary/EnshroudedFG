#pragma once

#include <cstddef>
#include <cstdint>

#include "sl/sl_bridge.h"

namespace efg::render {

// A compute pass that copies a sampled source image into one of two images we own (alternating per
// frame, so a frame's copy is never overwritten while DLSS-G may still read it). Recorded directly
// into the game's command buffer. Used for:
//  - motion vectors: R16G16_SNORM (not in Streamline's Vulkan format table) -> RG16F
//  - HUD-less colour: the swapchain image right before the game composites its HUD -> RGBA8
class CopyPass {
public:
    bool Init(const char* name, VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
              VkFormat dstFormat, const uint32_t* spirv, size_t spirvBytes);
    void Shutdown();
    bool Ready() const { return ready_; }

    // `srcView` must be readable as a sampled image in `srcLayout`. Returns the copy (layout GENERAL),
    // or an empty ImageRef on failure.
    sl_bridge::ImageRef Copy(VkCommandBuffer cmd, VkImageView srcView, uint32_t width, uint32_t height,
                             VkImageLayout srcLayout);

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool initialized = false;  // has left VK_IMAGE_LAYOUT_UNDEFINED
    };
    bool EnsureTargets(uint32_t width, uint32_t height);
    void DestroyTargets();

    const char* name_ = "";
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Target targets_[2];
    uint32_t width_ = 0, height_ = 0;
    uint32_t next_ = 0;
    bool ready_ = false;

    PFN_vkCreateImage vkCreateImage_ = nullptr;
    PFN_vkDestroyImage vkDestroyImage_ = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements_ = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory_ = nullptr;
    PFN_vkFreeMemory vkFreeMemory_ = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory_ = nullptr;
    PFN_vkCreateImageView vkCreateImageView_ = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView_ = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline_ = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout_ = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout_ = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier_ = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline_ = nullptr;
    PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR_ = nullptr;
    PFN_vkCmdPushConstants vkCmdPushConstants_ = nullptr;
    PFN_vkCmdDispatch vkCmdDispatch_ = nullptr;
};

}  // namespace efg::render
