#include "render/copy_pass.h"

#include "core/log.h"
#include "hooks/hooks.h"

namespace efg::render {
namespace {

template <class T>
bool Load(T& out, VkDevice device, const char* name) {
    out = reinterpret_cast<T>(RealDeviceProc(device, name));
    if (!out) EFG_LOG("CopyPass: missing device function %s", name);
    return out != nullptr;
}

}  // namespace

bool CopyPass::Init(const char* name, VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                    VkFormat dstFormat, const uint32_t* spirv, size_t spirvBytes) {
    name_ = name;
    device_ = device;
    format_ = dstFormat;
    auto getMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        RealInstanceProc(instance, "vkGetPhysicalDeviceMemoryProperties"));
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout createSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    bool ok = getMemProps && Load(createShaderModule, device, "vkCreateShaderModule") &&
              Load(destroyShaderModule, device, "vkDestroyShaderModule") &&
              Load(createSetLayout, device, "vkCreateDescriptorSetLayout") &&
              Load(createPipelineLayout, device, "vkCreatePipelineLayout") &&
              Load(createComputePipelines, device, "vkCreateComputePipelines") &&
              Load(vkCreateImage_, device, "vkCreateImage") && Load(vkDestroyImage_, device, "vkDestroyImage") &&
              Load(vkGetImageMemoryRequirements_, device, "vkGetImageMemoryRequirements") &&
              Load(vkAllocateMemory_, device, "vkAllocateMemory") && Load(vkFreeMemory_, device, "vkFreeMemory") &&
              Load(vkBindImageMemory_, device, "vkBindImageMemory") &&
              Load(vkCreateImageView_, device, "vkCreateImageView") &&
              Load(vkDestroyImageView_, device, "vkDestroyImageView") &&
              Load(vkDestroyPipeline_, device, "vkDestroyPipeline") &&
              Load(vkDestroyPipelineLayout_, device, "vkDestroyPipelineLayout") &&
              Load(vkDestroyDescriptorSetLayout_, device, "vkDestroyDescriptorSetLayout") &&
              Load(vkCmdPipelineBarrier_, device, "vkCmdPipelineBarrier") &&
              Load(vkCmdBindPipeline_, device, "vkCmdBindPipeline") &&
              Load(vkCmdPushDescriptorSetKHR_, device, "vkCmdPushDescriptorSetKHR") &&
              Load(vkCmdPushConstants_, device, "vkCmdPushConstants") && Load(vkCmdDispatch_, device, "vkCmdDispatch");
    if (!ok) return false;
    getMemProps(physicalDevice, &memProps_);

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (createSetLayout(device, &setInfo, nullptr, &setLayout_) != VK_SUCCESS) return false;

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(int32_t)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (createPipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo smInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smInfo.codeSize = spirvBytes;
    smInfo.pCode = spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (createShaderModule(device, &smInfo, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pInfo.stage.module = module;
    pInfo.stage.pName = "main";
    pInfo.layout = pipelineLayout_;
    VkResult r = createComputePipelines(device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_);
    destroyShaderModule(device, module, nullptr);
    if (r != VK_SUCCESS) {
        EFG_LOG("CopyPass %s: compute pipeline creation failed (%d)", name_, r);
        return false;
    }
    ready_ = true;
    EFG_LOG("CopyPass %s ready (-> format %d)", name_, format_);
    return true;
}

bool CopyPass::EnsureTargets(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_ && targets_[0].image) return true;
    DestroyTargets();
    for (auto& t : targets_) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format_;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage_(device_, &ci, nullptr, &t.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements_(device_, t.image, &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory_(device_, &ai, nullptr, &t.memory) != VK_SUCCESS ||
            vkBindImageMemory_(device_, t.image, t.memory, 0) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView_(device_, &vi, nullptr, &t.view) != VK_SUCCESS) return false;
        t.initialized = false;
    }
    width_ = width;
    height_ = height;
    EFG_LOG("CopyPass %s: allocated 2x %ux%u targets", name_, width, height);
    return true;
}

void CopyPass::DestroyTargets() {
    for (auto& t : targets_) {
        if (t.view) vkDestroyImageView_(device_, t.view, nullptr);
        if (t.image) vkDestroyImage_(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory_(device_, t.memory, nullptr);
        t = Target{};
    }
    width_ = height_ = 0;
}

void CopyPass::Shutdown() {
    if (!device_ || !vkDestroyPipeline_) return;
    DestroyTargets();
    if (pipeline_) vkDestroyPipeline_(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout_(device_, pipelineLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout_(device_, setLayout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ready_ = false;
}

sl_bridge::ImageRef CopyPass::Copy(VkCommandBuffer cmd, VkImageView srcView, uint32_t width, uint32_t height,
                                   VkImageLayout srcLayout) {
    if (!ready_ || !srcView || !width || !height) return {};
    if (!EnsureTargets(width, height)) {
        EFG_LOG("CopyPass %s: target allocation failed", name_);
        ready_ = false;
        return {};
    }
    Target& t = targets_[next_];
    next_ ^= 1;

    VkImageMemoryBarrier toWrite{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toWrite.srcAccessMask = 0;  // earlier DLSS-G reads are ordered by Streamline's queue synchronization
    toWrite.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toWrite.oldLayout = t.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toWrite.srcQueueFamilyIndex = toWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toWrite.image = t.image;
    toWrite.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &toWrite);
    t.initialized = true;

    VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, srcView, srcLayout};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &srcInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &dstInfo;

    // Both insertion points (before DLSS-SR, before the HUD pass) are followed by work that binds its own
    // pipeline and descriptors (DLSS-SR / a new render pass), so the game's bound state does not leak.
    vkCmdBindPipeline_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdPushDescriptorSetKHR_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, writes);
    int32_t size[2] = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
    vkCmdPushConstants_(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vkCmdDispatch_(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    VkImageMemoryBarrier toRead = toWrite;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &toRead);

    return {t.image, t.view, format_, width, height};
}

}  // namespace efg::render
