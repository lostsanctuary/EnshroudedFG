#pragma once

#include <cstddef>
#include <cstdint>

#include "sl/sl_bridge.h"

namespace efg::render {

// A compute pass that copies a sampled source image into a ring of images we own (one per frame, so a
// frame's copy is never overwritten while DLSS-G may still read it). Recorded directly into the game's
// command buffer. Used for:
//  - motion vectors: R16G16_SNORM (not in Streamline's Vulkan format table) -> RG16F
//  - HUD-less colour: the swapchain image right before the game composites its HUD -> RGBA8
//  - depth ([FrameGen] QueueParallelism=parallel only): the game's depth -> R32F
class CopyPass {
public:
    static constexpr uint32_t kMaxSlots = 3;
    // Blocks until DLSS-G has finished reading the inputs it was given with this fence value.
    using WaitFn = void (*)(uint64_t fenceValue);

    // `slots`: 2 is enough while Streamline blocks the presenting queue; parallel mode uses 3 plus
    // `wait`, which is called before a slot that DLSS-G may still be reading is rewritten.
    // `timing` ([Debug] VerboseLog): GPU timestamps around each copy, read back a few frames later.
    bool Init(const char* name, VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
              VkFormat dstFormat, const uint32_t* spirv, size_t spirvBytes, uint32_t slots = 2,
              WaitFn wait = nullptr, bool timing = false);
    void Shutdown();
    bool Ready() const { return ready_; }

    // `srcView` must be readable as a sampled image in `srcLayout`. Returns the copy (layout GENERAL),
    // or an empty ImageRef on failure.
    sl_bridge::ImageRef Copy(VkCommandBuffer cmd, VkImageView srcView, uint32_t width, uint32_t height,
                             VkImageLayout srcLayout);
    // After a present: DLSS-G's completion-fence value for that frame's inputs. It's recorded on the slot
    // written since the previous call (if any), and waited for before that slot is written again.
    void MarkPresented(uint64_t fenceValue);
    // GPU time of the copies measured since the last call (timing only). Returns false if none.
    bool TakeTiming(double& avgUs, double& maxUs, uint32_t& count);

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool initialized = false;  // has left VK_IMAGE_LAYOUT_UNDEFINED
        uint64_t fenceValue = 0;   // DLSS-G is done with this copy once its fence reaches this (0 = no wait)
    };
    bool EnsureTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    void ReadTiming(uint32_t frame);  // collects a finished timestamp pair, if available

    static constexpr uint32_t kTimingFrames = 4;  // timestamp pairs in flight

    const char* name_ = "";
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Target targets_[kMaxSlots];
    uint32_t slots_ = 2;
    WaitFn wait_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    uint32_t next_ = 0;
    int written_ = -1;  // slot written since the last MarkPresented (-1 = none)
    bool ready_ = false;

    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    double timestampPeriodNs_ = 0;
    uint64_t timestampMask_ = ~0ull;
    bool queryWritten_[kTimingFrames] = {};
    uint32_t timingFrame_ = 0;
    double timingSumUs_ = 0, timingMaxUs_ = 0;
    uint32_t timingCount_ = 0;

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
    PFN_vkDestroyQueryPool vkDestroyQueryPool_ = nullptr;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool_ = nullptr;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp_ = nullptr;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults_ = nullptr;
};

}  // namespace efg::render
