#pragma once

#include "sl/sl_bridge.h"

namespace efg::render {

// Converts the game's R16G16_SNORM motion vectors (unsupported by Streamline's format table) into an
// RG16F image we own. Recorded into the game's command buffer right before its DLSS-SR evaluate.
// Two images alternate per frame, so a frame's copy is never overwritten while DLSS-G may still read it.
class MvConverter {
public:
    bool Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
    void Shutdown();

    // Returns the converted image (layout GENERAL), or an empty ImageRef on failure.
    sl_bridge::ImageRef Convert(VkCommandBuffer cmd, const sl_bridge::ImageRef& src);

    static bool NeedsConversion(VkFormat format) { return format == VK_FORMAT_R16G16_SNORM; }

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool initialized = false;  // has left VK_IMAGE_LAYOUT_UNDEFINED
    };
    bool EnsureTargets(uint32_t width, uint32_t height);
    void DestroyTargets();

    VkDevice device_ = VK_NULL_HANDLE;
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
