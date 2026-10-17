#include "debug/frame_trace.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"

namespace efg::frame_trace {
namespace {

struct ImageInfo {
    uint32_t id = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkImageUsageFlags usage = 0;
};
struct CmdLog {
    std::string text;
    uint32_t draws = 0, dispatches = 0;
};

std::mutex g_mutex;
std::unordered_map<VkImage, ImageInfo> g_images;
std::unordered_map<VkImageView, VkImage> g_views;
std::unordered_map<VkFramebuffer, std::vector<VkImageView>> g_framebuffers;
std::unordered_map<VkImage, std::string> g_labels;
std::unordered_map<VkCommandBuffer, CmdLog> g_cmds;
uint32_t g_nextImageId = 1;
std::string g_out;
uint32_t g_submitIndex = 0;

enum State { kIdle, kArmed, kRecording };
std::atomic<int> g_state{kIdle};
bool Recording() { return g_state.load(std::memory_order_relaxed) == kRecording; }

const char* FormatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "BGR10A2";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10F";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
        case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
        case VK_FORMAT_R16G16_SNORM: return "RG16_SNORM";
        case VK_FORMAT_R32_SFLOAT: return "R32F";
        case VK_FORMAT_R16_SFLOAT: return "R16F";
        case VK_FORMAT_R8_UNORM: return "R8";
        case VK_FORMAT_R8G8_UNORM: return "RG8";
        case VK_FORMAT_D32_SFLOAT: return "D32";
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
        case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case VK_FORMAT_D16_UNORM: return "D16";
        default: return nullptr;
    }
}

const char* LayoutName(VkImageLayout l) {
    switch (l) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEF";
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_RO";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_RO";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "XFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "XFER_DST";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT";
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL: return "ATT";
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL: return "RO";
        default: return "other";
    }
}

// Caller holds g_mutex.
std::string Describe(VkImage img) {
    char buf[160];
    auto it = g_images.find(img);
    if (it == g_images.end()) {
        snprintf(buf, sizeof(buf), "img?%p", (void*)img);
    } else {
        const ImageInfo& i = it->second;
        const char* fn = FormatName(i.format);
        char fmt[16];
        if (!fn) {
            snprintf(fmt, sizeof(fmt), "fmt%d", i.format);
            fn = fmt;
        }
        snprintf(buf, sizeof(buf), "img#%u %s %ux%u", i.id, fn, i.extent.width, i.extent.height);
    }
    std::string s = buf;
    auto lab = g_labels.find(img);
    if (lab != g_labels.end()) s += " [" + lab->second + "]";
    return s;
}

std::string DescribeView(VkImageView v) {
    auto it = g_views.find(v);
    return it == g_views.end() ? "view?" : Describe(it->second);
}

// Caller holds g_mutex. Returns the log for `cmd` if it is being recorded during the trace.
CmdLog* Log(VkCommandBuffer cmd) {
    auto it = g_cmds.find(cmd);
    return it == g_cmds.end() ? nullptr : &it->second;
}

void FlushCounts(CmdLog& l) {
    if (!l.draws && !l.dispatches) return;
    char buf[96];
    snprintf(buf, sizeof(buf), "    %u draws, %u dispatches\n", l.draws, l.dispatches);
    l.text += buf;
    l.draws = l.dispatches = 0;
}

void Line(VkCommandBuffer cmd, const std::string& s) {
    std::lock_guard lock(g_mutex);
    if (CmdLog* l = Log(cmd)) {
        FlushCounts(*l);
        l->text += "  " + s + "\n";
    }
}

// ---- real functions -------------------------------------------------------------------------

PFN_vkCreateImage r_CreateImage;
PFN_vkCreateImageView r_CreateImageView;
PFN_vkCreateFramebuffer r_CreateFramebuffer;
PFN_vkBeginCommandBuffer r_BeginCommandBuffer;
PFN_vkCmdBeginRenderPass r_CmdBeginRenderPass;
PFN_vkCmdBeginRenderPass2 r_CmdBeginRenderPass2;
PFN_vkCmdBeginRenderPass2 r_CmdBeginRenderPass2KHR;
PFN_vkCmdEndRenderPass r_CmdEndRenderPass;
PFN_vkCmdBeginRendering r_CmdBeginRendering;
PFN_vkCmdBeginRendering r_CmdBeginRenderingKHR;
PFN_vkCmdEndRendering r_CmdEndRendering;
PFN_vkCmdEndRendering r_CmdEndRenderingKHR;
PFN_vkCmdDraw r_CmdDraw;
PFN_vkCmdDrawIndexed r_CmdDrawIndexed;
PFN_vkCmdDrawIndirect r_CmdDrawIndirect;
PFN_vkCmdDrawIndexedIndirect r_CmdDrawIndexedIndirect;
PFN_vkCmdDispatch r_CmdDispatch;
PFN_vkCmdDispatchIndirect r_CmdDispatchIndirect;
PFN_vkCmdPipelineBarrier r_CmdPipelineBarrier;
PFN_vkCmdPipelineBarrier2 r_CmdPipelineBarrier2;
PFN_vkCmdPipelineBarrier2 r_CmdPipelineBarrier2KHR;
PFN_vkCmdCopyImage r_CmdCopyImage;
PFN_vkCmdBlitImage r_CmdBlitImage;
PFN_vkCmdClearColorImage r_CmdClearColorImage;
PFN_vkCmdCopyBufferToImage r_CmdCopyBufferToImage;
PFN_vkQueueSubmit r_QueueSubmit;
PFN_vkQueueSubmit2 r_QueueSubmit2;
PFN_vkQueueSubmit2 r_QueueSubmit2KHR;

// ---- wrappers -------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL W_CreateImage(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a,
                                             VkImage* out) {
    VkResult r = r_CreateImage(d, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mutex);
        g_images[*out] = {g_nextImageId++, ci->format, ci->extent, ci->usage};
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL W_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci,
                                                 const VkAllocationCallbacks* a, VkImageView* out) {
    VkResult r = r_CreateImageView(d, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mutex);
        g_views[*out] = ci->image;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL W_CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* ci,
                                                   const VkAllocationCallbacks* a, VkFramebuffer* out) {
    VkResult r = r_CreateFramebuffer(d, ci, a, out);
    if (r == VK_SUCCESS && !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
        std::lock_guard lock(g_mutex);
        g_framebuffers[*out].assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL W_BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* bi) {
    if (Recording()) {
        std::lock_guard lock(g_mutex);
        g_cmds[cmd] = CmdLog{};
    }
    return r_BeginCommandBuffer(cmd, bi);
}

void LogRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* bi) {
    std::lock_guard lock(g_mutex);
    CmdLog* l = Log(cmd);
    if (!l) return;
    FlushCounts(*l);
    std::string s = "BeginRenderPass " + std::to_string(bi->renderArea.extent.width) + "x" +
                    std::to_string(bi->renderArea.extent.height) + ":";
    const VkRenderPassAttachmentBeginInfo* imageless = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(bi->pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
            imageless = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(p);
    }
    if (imageless) {
        for (uint32_t i = 0; i < imageless->attachmentCount; ++i) s += " {" + DescribeView(imageless->pAttachments[i]) + "}";
    } else if (auto it = g_framebuffers.find(bi->framebuffer); it != g_framebuffers.end()) {
        for (VkImageView v : it->second) s += " {" + DescribeView(v) + "}";
    }
    l->text += "  " + s + "\n";
}

VKAPI_ATTR void VKAPI_CALL W_CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    if (Recording()) LogRenderPass(cmd, bi);
    r_CmdBeginRenderPass(cmd, bi, c);
}
VKAPI_ATTR void VKAPI_CALL W_CmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* bi,
                                                 const VkSubpassBeginInfo* sb) {
    if (Recording()) LogRenderPass(cmd, bi);
    r_CmdBeginRenderPass2(cmd, bi, sb);
}
VKAPI_ATTR void VKAPI_CALL W_CmdBeginRenderPass2KHR(VkCommandBuffer cmd, const VkRenderPassBeginInfo* bi,
                                                    const VkSubpassBeginInfo* sb) {
    if (Recording()) LogRenderPass(cmd, bi);
    r_CmdBeginRenderPass2KHR(cmd, bi, sb);
}
VKAPI_ATTR void VKAPI_CALL W_CmdEndRenderPass(VkCommandBuffer cmd) {
    if (Recording()) Line(cmd, "EndRenderPass");
    r_CmdEndRenderPass(cmd);
}

const char* LoadOpName(VkAttachmentLoadOp op) {
    return op == VK_ATTACHMENT_LOAD_OP_LOAD ? "load" : op == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear" : "dontcare";
}

void LogRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri) {
    std::lock_guard lock(g_mutex);
    CmdLog* l = Log(cmd);
    if (!l) return;
    FlushCounts(*l);
    std::string s = "BeginRendering " + std::to_string(ri->renderArea.extent.width) + "x" +
                    std::to_string(ri->renderArea.extent.height) + ":";
    for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) {
        const auto& a = ri->pColorAttachments[i];
        if (!a.imageView) continue;
        s += " color{" + DescribeView(a.imageView) + " " + LoadOpName(a.loadOp) + "}";
    }
    if (ri->pDepthAttachment && ri->pDepthAttachment->imageView)
        s += " depth{" + DescribeView(ri->pDepthAttachment->imageView) + " " + LoadOpName(ri->pDepthAttachment->loadOp) + "}";
    l->text += "  " + s + "\n";
}

VKAPI_ATTR void VKAPI_CALL W_CmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri) {
    if (Recording()) LogRendering(cmd, ri);
    r_CmdBeginRendering(cmd, ri);
}
VKAPI_ATTR void VKAPI_CALL W_CmdBeginRenderingKHR(VkCommandBuffer cmd, const VkRenderingInfo* ri) {
    if (Recording()) LogRendering(cmd, ri);
    r_CmdBeginRenderingKHR(cmd, ri);
}
VKAPI_ATTR void VKAPI_CALL W_CmdEndRendering(VkCommandBuffer cmd) {
    if (Recording()) Line(cmd, "EndRendering");
    r_CmdEndRendering(cmd);
}
VKAPI_ATTR void VKAPI_CALL W_CmdEndRenderingKHR(VkCommandBuffer cmd) {
    if (Recording()) Line(cmd, "EndRendering");
    r_CmdEndRenderingKHR(cmd);
}

void CountDraw(VkCommandBuffer cmd) {
    std::lock_guard lock(g_mutex);
    if (CmdLog* l = Log(cmd)) ++l->draws;
}
void CountDispatch(VkCommandBuffer cmd) {
    std::lock_guard lock(g_mutex);
    if (CmdLog* l = Log(cmd)) ++l->dispatches;
}

VKAPI_ATTR void VKAPI_CALL W_CmdDraw(VkCommandBuffer cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (Recording()) CountDraw(cmd);
    r_CmdDraw(cmd, a, b, c, d);
}
VKAPI_ATTR void VKAPI_CALL W_CmdDrawIndexed(VkCommandBuffer cmd, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) {
    if (Recording()) CountDraw(cmd);
    r_CmdDrawIndexed(cmd, a, b, c, d, e);
}
VKAPI_ATTR void VKAPI_CALL W_CmdDrawIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (Recording()) CountDraw(cmd);
    r_CmdDrawIndirect(cmd, b, o, n, s);
}
VKAPI_ATTR void VKAPI_CALL W_CmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (Recording()) CountDraw(cmd);
    r_CmdDrawIndexedIndirect(cmd, b, o, n, s);
}
VKAPI_ATTR void VKAPI_CALL W_CmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    if (Recording()) CountDispatch(cmd);
    r_CmdDispatch(cmd, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL W_CmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o) {
    if (Recording()) CountDispatch(cmd);
    r_CmdDispatchIndirect(cmd, b, o);
}

void LogImageBarrier(std::string& s, VkImage img, VkImageLayout from, VkImageLayout to) {
    if (from == to) return;  // memory-only barriers are noise here
    s += " " + Describe(img) + " " + LayoutName(from) + "->" + LayoutName(to) + ";";
}

VKAPI_ATTR void VKAPI_CALL W_CmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                                VkDependencyFlags df, uint32_t mc, const VkMemoryBarrier* mb, uint32_t bc,
                                                const VkBufferMemoryBarrier* bb, uint32_t ic,
                                                const VkImageMemoryBarrier* ib) {
    if (Recording() && ic) {
        std::lock_guard lock(g_mutex);
        if (CmdLog* l = Log(cmd)) {
            std::string s;
            for (uint32_t i = 0; i < ic; ++i) LogImageBarrier(s, ib[i].image, ib[i].oldLayout, ib[i].newLayout);
            if (!s.empty()) {
                FlushCounts(*l);
                l->text += "  barrier:" + s + "\n";
            }
        }
    }
    r_CmdPipelineBarrier(cmd, ss, ds, df, mc, mb, bc, bb, ic, ib);
}

void LogBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* di) {
    std::lock_guard lock(g_mutex);
    CmdLog* l = Log(cmd);
    if (!l || !di) return;
    std::string s;
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) {
        const auto& b = di->pImageMemoryBarriers[i];
        LogImageBarrier(s, b.image, b.oldLayout, b.newLayout);
    }
    if (!s.empty()) {
        FlushCounts(*l);
        l->text += "  barrier2:" + s + "\n";
    }
}
VKAPI_ATTR void VKAPI_CALL W_CmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* di) {
    if (Recording()) LogBarrier2(cmd, di);
    r_CmdPipelineBarrier2(cmd, di);
}
VKAPI_ATTR void VKAPI_CALL W_CmdPipelineBarrier2KHR(VkCommandBuffer cmd, const VkDependencyInfo* di) {
    if (Recording()) LogBarrier2(cmd, di);
    r_CmdPipelineBarrier2KHR(cmd, di);
}

void LogTransfer(VkCommandBuffer cmd, const char* op, VkImage src, VkImage dst) {
    std::lock_guard lock(g_mutex);
    if (CmdLog* l = Log(cmd)) {
        FlushCounts(*l);
        l->text += std::string("  ") + op + " " + (src ? Describe(src) + " -> " : std::string()) + Describe(dst) + "\n";
    }
}
VKAPI_ATTR void VKAPI_CALL W_CmdCopyImage(VkCommandBuffer cmd, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                          uint32_t n, const VkImageCopy* r) {
    if (Recording()) LogTransfer(cmd, "CopyImage", s, d);
    r_CmdCopyImage(cmd, s, sl, d, dl, n, r);
}
VKAPI_ATTR void VKAPI_CALL W_CmdBlitImage(VkCommandBuffer cmd, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                          uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (Recording()) LogTransfer(cmd, "BlitImage", s, d);
    r_CmdBlitImage(cmd, s, sl, d, dl, n, r, f);
}
VKAPI_ATTR void VKAPI_CALL W_CmdClearColorImage(VkCommandBuffer cmd, VkImage i, VkImageLayout l, const VkClearColorValue* c,
                                                uint32_t n, const VkImageSubresourceRange* r) {
    if (Recording()) LogTransfer(cmd, "ClearColorImage", VK_NULL_HANDLE, i);
    r_CmdClearColorImage(cmd, i, l, c, n, r);
}
VKAPI_ATTR void VKAPI_CALL W_CmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n,
                                                  const VkBufferImageCopy* r) {
    if (Recording()) LogTransfer(cmd, "CopyBufferToImage", VK_NULL_HANDLE, i);
    r_CmdCopyBufferToImage(cmd, b, i, l, n, r);
}

void LogSubmit(VkQueue q, uint32_t count, const VkCommandBuffer* cmds) {
    std::lock_guard lock(g_mutex);
    char buf[96];
    snprintf(buf, sizeof(buf), "== submit #%u queue %p, %u command buffers\n", g_submitIndex++, (void*)q, count);
    g_out += buf;
    for (uint32_t i = 0; i < count; ++i) {
        snprintf(buf, sizeof(buf), "-- cmd %p\n", (void*)cmds[i]);
        g_out += buf;
        auto it = g_cmds.find(cmds[i]);
        if (it == g_cmds.end()) {
            g_out += "  (recorded before the trace started)\n";
            continue;
        }
        FlushCounts(it->second);
        g_out += it->second.text;
        g_cmds.erase(it);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL W_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    if (Recording()) {
        for (uint32_t i = 0; i < n; ++i) LogSubmit(q, s[i].commandBufferCount, s[i].pCommandBuffers);
    }
    return r_QueueSubmit(q, n, s, f);
}

void LogSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s) {
    for (uint32_t i = 0; i < n; ++i) {
        std::vector<VkCommandBuffer> cmds;
        for (uint32_t j = 0; j < s[i].commandBufferInfoCount; ++j) cmds.push_back(s[i].pCommandBufferInfos[j].commandBuffer);
        LogSubmit(q, static_cast<uint32_t>(cmds.size()), cmds.data());
    }
}
VKAPI_ATTR VkResult VKAPI_CALL W_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (Recording()) LogSubmit2(q, n, s);
    return r_QueueSubmit2(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL W_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (Recording()) LogSubmit2(q, n, s);
    return r_QueueSubmit2KHR(q, n, s, f);
}

struct Entry {
    const char* name;
    void* wrapper;
    void** real;
};
#define EFG_TRACE(fn) {"vk" #fn, reinterpret_cast<void*>(&W_##fn), reinterpret_cast<void**>(&r_##fn)}
const Entry kEntries[] = {
    EFG_TRACE(CreateImage), EFG_TRACE(CreateImageView), EFG_TRACE(CreateFramebuffer), EFG_TRACE(BeginCommandBuffer),
    EFG_TRACE(CmdBeginRenderPass), EFG_TRACE(CmdBeginRenderPass2), EFG_TRACE(CmdBeginRenderPass2KHR),
    EFG_TRACE(CmdEndRenderPass), EFG_TRACE(CmdBeginRendering), EFG_TRACE(CmdBeginRenderingKHR),
    EFG_TRACE(CmdEndRendering), EFG_TRACE(CmdEndRenderingKHR), EFG_TRACE(CmdDraw), EFG_TRACE(CmdDrawIndexed),
    EFG_TRACE(CmdDrawIndirect), EFG_TRACE(CmdDrawIndexedIndirect), EFG_TRACE(CmdDispatch),
    EFG_TRACE(CmdDispatchIndirect), EFG_TRACE(CmdPipelineBarrier), EFG_TRACE(CmdPipelineBarrier2),
    EFG_TRACE(CmdPipelineBarrier2KHR), EFG_TRACE(CmdCopyImage), EFG_TRACE(CmdBlitImage),
    EFG_TRACE(CmdClearColorImage), EFG_TRACE(CmdCopyBufferToImage), EFG_TRACE(QueueSubmit), EFG_TRACE(QueueSubmit2),
    EFG_TRACE(QueueSubmit2KHR),
};
#undef EFG_TRACE

}  // namespace

bool Enabled() {
    static const bool enabled = [] {
        std::wstring ini = GameDir() + L"EnshroudedFG.ini";
        bool on = GetPrivateProfileIntW(L"Debug", L"FrameTrace", 0, ini.c_str()) != 0;
        if (on) EFG_LOG("frame trace enabled (F11 traces the next frame; one automatic trace in the world)");
        return on;
    }();
    return enabled;
}

PFN_vkVoidFunction Intercept(const char* name, PFN_vkVoidFunction real) {
    if (!real || !Enabled()) return real;
    for (const auto& e : kEntries) {
        if (strcmp(name, e.name) == 0) {
            *e.real = reinterpret_cast<void*>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(e.wrapper);
        }
    }
    return real;
}

void Arm() {
    int expected = kIdle;
    if (Enabled() && g_state.compare_exchange_strong(expected, kArmed)) EFG_LOG("frame trace armed");
}

void OnSwapchainImages(uint32_t count, const VkImage* images) {
    if (!Enabled() || !images) return;
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < count; ++i) g_labels[images[i]] = "SWAPCHAIN " + std::to_string(i);
}

void OnSrEvaluate(VkCommandBuffer cmd, VkImage color, VkImage output, VkImage depth, VkImage mvec) {
    if (!Enabled()) return;
    std::lock_guard lock(g_mutex);
    g_labels[color] = "SR color";
    g_labels[output] = "SR output";
    g_labels[depth] = "SR depth";
    g_labels[mvec] = "SR mvec";
    if (Recording()) {
        if (CmdLog* l = Log(cmd)) {
            FlushCounts(*l);
            l->text += "  >>> DLSS-SR evaluate: " + Describe(color) + " -> " + Describe(output) + "\n";
        }
    }
}

void OnPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    if (!Enabled()) return;
    int state = g_state.load();
    if (state == kArmed) {
        std::lock_guard lock(g_mutex);
        g_cmds.clear();
        g_out = "Enshrouded frame trace (one frame, present to present)\n";
        g_submitIndex = 0;
        g_state.store(kRecording);
        return;
    }
    if (state != kRecording) return;
    g_state.store(kIdle);
    std::string out;
    {
        std::lock_guard lock(g_mutex);
        char buf[128];
        snprintf(buf, sizeof(buf), "== present queue %p image index %u\n", (void*)queue,
                 info && info->pImageIndices ? info->pImageIndices[0] : 0);
        g_out += buf;
        out.swap(g_out);
        g_cmds.clear();
    }
    std::wstring path = ModDir() + L"frame_trace.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        EFG_LOG("frame trace written: %zu bytes -> %ls", out.size(), path.c_str());
    }
}

}  // namespace efg::frame_trace
