#include "overlay/overlay.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <cstdio>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "hooks/hook_util.h"
#include "debug/frame_trace.h"
#include "hooks/hooks.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace efg::overlay {
namespace {

// ---- state ----------------------------------------------------------------------------------

std::atomic<bool> g_visible{false};
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
bool g_imguiContext = false;

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
uint32_t g_queueFamily = 0;
VkCommandPool g_pool = VK_NULL_HANDLE;
bool g_vkReady = false;  // device functions loaded, command pool created

VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;
VkExtent2D g_extent{};
uint32_t g_minImageCount = 2;
struct PerImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
};
std::vector<PerImage> g_images;

bool g_imguiVulkan = false;      // ImGui Vulkan backend initialized
VkFormat g_imguiFormat = VK_FORMAT_UNDEFINED;

// Device functions (unhooked).
PFN_vkCreateCommandPool p_CreateCommandPool;
PFN_vkDestroyCommandPool p_DestroyCommandPool;
PFN_vkAllocateCommandBuffers p_AllocateCommandBuffers;
PFN_vkFreeCommandBuffers p_FreeCommandBuffers;
PFN_vkBeginCommandBuffer p_BeginCommandBuffer;
PFN_vkEndCommandBuffer p_EndCommandBuffer;
PFN_vkResetCommandBuffer p_ResetCommandBuffer;
PFN_vkCreateFence p_CreateFence;
PFN_vkDestroyFence p_DestroyFence;
PFN_vkWaitForFences p_WaitForFences;
PFN_vkResetFences p_ResetFences;
PFN_vkCreateSemaphore p_CreateSemaphore;
PFN_vkDestroySemaphore p_DestroySemaphore;
PFN_vkCreateImageView p_CreateImageView;
PFN_vkDestroyImageView p_DestroyImageView;
PFN_vkCmdPipelineBarrier p_CmdPipelineBarrier;
PFN_vkCmdBeginRenderingKHR p_CmdBeginRendering;
PFN_vkCmdEndRenderingKHR p_CmdEndRendering;
PFN_vkQueueSubmit p_QueueSubmit;
PFN_vkDeviceWaitIdle p_DeviceWaitIdle;

template <class T>
bool Load(T& out, const char* name, const char* alt = nullptr) {
    out = reinterpret_cast<T>(RealDeviceProc(g_device, name));
    if (!out && alt) out = reinterpret_cast<T>(RealDeviceProc(g_device, alt));
    if (!out) EFG_LOG("overlay: missing device function %s", name);
    return out != nullptr;
}

PFN_vkVoidFunction ImGuiLoader(const char* name, void*) {
    if (auto f = RealDeviceProc(g_device, name)) return reinterpret_cast<PFN_vkVoidFunction>(f);
    return reinterpret_cast<PFN_vkVoidFunction>(RealInstanceProc(g_instance, name));
}

// ---- window / input -------------------------------------------------------------------------

bool IsInputMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUT || msg == WM_NCMOUSEMOVE || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE;
}

void SetVisible(bool v) {
    if (g_visible.exchange(v) == v) return;
    if (v) ClipCursor(nullptr);  // release the game's cursor capture while the menu is open
    if (g_imguiContext) ImGui::GetIO().MouseDrawCursor = v;
    EFG_LOG("overlay %s", v ? "opened" : "closed");
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int key = LoadConfig().overlayKey;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && static_cast<int>(wp) == key) {
        if (!(lp & (1 << 30))) SetVisible(!g_visible.load());  // ignore auto-repeat
        return 0;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && static_cast<int>(wp) == key) return 0;
    if (msg == WM_KEYDOWN && wp == VK_F11 && !(lp & (1 << 30)) && frame_trace::Enabled()) frame_trace::Arm();
    if (g_visible.load() && g_imguiContext) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp);  // required cleanup, but hide from the game
        if (IsInputMessage(msg)) return 0;
    }
    if (msg == WM_KILLFOCUS && g_visible.load()) SetVisible(false);
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
SetCursorPos_t g_origSetCursorPos = nullptr;
ClipCursor_t g_origClipCursor = nullptr;

// While the menu is open the game must not recentre or confine the cursor.
BOOL WINAPI HookSetCursorPos(int x, int y) { return g_visible.load() ? TRUE : g_origSetCursorPos(x, y); }
BOOL WINAPI HookClipCursor(const RECT* rect) {
    return (g_visible.load() && rect) ? TRUE : g_origClipCursor(rect);
}

// ---- Vulkan resources -----------------------------------------------------------------------

bool InitVulkan() {
    if (g_vkReady) return true;
    if (!g_device) return false;
    bool ok = Load(p_CreateCommandPool, "vkCreateCommandPool") && Load(p_DestroyCommandPool, "vkDestroyCommandPool") &&
              Load(p_AllocateCommandBuffers, "vkAllocateCommandBuffers") &&
              Load(p_FreeCommandBuffers, "vkFreeCommandBuffers") && Load(p_BeginCommandBuffer, "vkBeginCommandBuffer") &&
              Load(p_EndCommandBuffer, "vkEndCommandBuffer") && Load(p_ResetCommandBuffer, "vkResetCommandBuffer") &&
              Load(p_CreateFence, "vkCreateFence") && Load(p_DestroyFence, "vkDestroyFence") &&
              Load(p_WaitForFences, "vkWaitForFences") && Load(p_ResetFences, "vkResetFences") &&
              Load(p_CreateSemaphore, "vkCreateSemaphore") && Load(p_DestroySemaphore, "vkDestroySemaphore") &&
              Load(p_CreateImageView, "vkCreateImageView") && Load(p_DestroyImageView, "vkDestroyImageView") &&
              Load(p_CmdPipelineBarrier, "vkCmdPipelineBarrier") &&
              Load(p_CmdBeginRendering, "vkCmdBeginRendering", "vkCmdBeginRenderingKHR") &&
              Load(p_CmdEndRendering, "vkCmdEndRendering", "vkCmdEndRenderingKHR") &&
              Load(p_QueueSubmit, "vkQueueSubmit") && Load(p_DeviceWaitIdle, "vkDeviceWaitIdle");
    if (!ok) return false;

    // Draw on the first graphics-capable queue family (the game presents from family 0).
    auto getQueueProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        RealInstanceProc(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    uint32_t n = 0;
    getQueueProps(g_physicalDevice, &n, nullptr);
    std::vector<VkQueueFamilyProperties> props(n);
    getQueueProps(g_physicalDevice, &n, props.data());
    for (uint32_t i = 0; i < n; ++i) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_queueFamily = i;
            break;
        }
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_queueFamily;
    if (p_CreateCommandPool(g_device, &pci, nullptr, &g_pool) != VK_SUCCESS) return false;
    g_vkReady = true;
    EFG_LOG("overlay: Vulkan ready (queue family %u)", g_queueFamily);
    return true;
}

void DestroyImages() {
    if (!g_vkReady) return;
    std::vector<VkFence> fences;
    for (auto& pi : g_images) {
        if (pi.fence) fences.push_back(pi.fence);
    }
    if (!fences.empty()) p_WaitForFences(g_device, (uint32_t)fences.size(), fences.data(), VK_TRUE, 1000000000ull);
    for (auto& pi : g_images) {
        if (pi.view) p_DestroyImageView(g_device, pi.view, nullptr);
        if (pi.cmd) p_FreeCommandBuffers(g_device, g_pool, 1, &pi.cmd);
        if (pi.fence) p_DestroyFence(g_device, pi.fence, nullptr);
        if (pi.done) p_DestroySemaphore(g_device, pi.done, nullptr);
    }
    g_images.clear();
}

bool EnsureImageResources(PerImage& pi) {
    if (pi.view) return true;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = pi.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_CreateImageView(g_device, &vi, nullptr, &pi.view) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (p_AllocateCommandBuffers(g_device, &ai, &pi.cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (p_CreateFence(g_device, &fi, nullptr, &pi.fence) != VK_SUCCESS) return false;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    return p_CreateSemaphore(g_device, &si, nullptr, &pi.done) == VK_SUCCESS;
}

bool EnsureImGuiVulkan(VkQueue queue) {
    if (g_imguiVulkan && g_imguiFormat == g_format) return true;
    if (g_imguiVulkan) {
        p_DeviceWaitIdle(g_device);
        ImGui_ImplVulkan_Shutdown();
        g_imguiVulkan = false;
    }
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, &ImGuiLoader)) {
        EFG_LOG("overlay: ImGui_ImplVulkan_LoadFunctions failed");
        return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_2;
    ii.Instance = g_instance;
    ii.PhysicalDevice = g_physicalDevice;
    ii.Device = g_device;
    ii.QueueFamily = g_queueFamily;
    ii.Queue = queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = g_minImageCount < 2 ? 2 : g_minImageCount;
    ii.ImageCount = static_cast<uint32_t>(g_images.size()) < ii.MinImageCount ? ii.MinImageCount
                                                                               : static_cast<uint32_t>(g_images.size());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        EFG_LOG("overlay: ImGui_ImplVulkan_Init failed");
        return false;
    }
    g_imguiVulkan = true;
    g_imguiFormat = g_format;
    EFG_LOG("overlay: ImGui Vulkan backend ready (format %d, %u images)", g_format, ii.ImageCount);
    return true;
}

// ---- menu -----------------------------------------------------------------------------------

const char* KeyName(int vk) {
    static char buf[64];
    UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lparam = static_cast<LONG>(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT)
        lparam |= 1 << 24;  // extended key
    if (!GetKeyNameTextA(lparam, buf, sizeof(buf))) snprintf(buf, sizeof(buf), "key 0x%02X", vk);
    return buf;
}

void DrawMenu() {
    const sl_bridge::FgStatus st = sl_bridge::GetFgStatus();
    const Config& cfg = LoadConfig();

    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::Begin("Enshrouded Frame Generation", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextDisabled("EnshroudedFG %s  -  DLSS Multi Frame Generation", EFG_VERSION_STRING);
    ImGui::Separator();

    ImGui::TextUnformatted("Frame generation");
    static const char* kLabels[] = {"Off", "2x", "3x", "4x", "5x", "6x"};
    for (int i = 0; i < 6; ++i) {
        const int value = i == 0 ? 1 : i + 1;
        const bool supported = value == 1 || st.maxMultiplier == 0 || static_cast<uint32_t>(value) <= st.maxMultiplier;
        if (i) ImGui::SameLine();
        ImGui::BeginDisabled(!supported || !st.slActive);
        ImGui::PushID(i);
        if (ImGui::RadioButton(kLabels[i], cfg.multiplier == value)) SetMultiplier(value);
        ImGui::PopID();
        ImGui::EndDisabled();
        if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Not supported on this GPU (max %ux)", st.maxMultiplier);
    }
    ImGui::SameLine();
    char fgAutoLabel[48];
    snprintf(fgAutoLabel, sizeof(fgAutoLabel), "Auto (%ux)###fgauto", st.autoMultiplier);
    ImGui::BeginDisabled(!st.slActive);
    if (ImGui::RadioButton(fgAutoLabel, cfg.multiplier == kMultiplierAuto)) SetMultiplier(kMultiplierAuto);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Picks 2x..%ux by itself so that rendered fps x multiplier just fills your display\n"
                          "(%.0f fps). Re-checked every 2 s while you play; the menu shows its current choice.",
                          st.maxMultiplier ? st.maxMultiplier : 6, st.refreshHz * 0.97);
    ImGui::TextUnformatted("Output cap");
    if (ImGui::RadioButton("Off##outputcap", cfg.outputCap == 0)) SetOutputCap(0);
    ImGui::SameLine();
    char autoLabel[64];
    if (st.refreshHz)
        snprintf(autoLabel, sizeof(autoLabel), "Auto (%.0f fps, %u Hz display)", st.refreshHz * 0.97, st.refreshHz);
    else
        snprintf(autoLabel, sizeof(autoLabel), "Auto (display refresh)");
    if (ImGui::RadioButton(autoLabel, cfg.outputCap < 0)) SetOutputCap(-1);
    if (cfg.outputCap > 0) {
        ImGui::SameLine();
        ImGui::RadioButton("Custom (ini)", true);
        ImGui::SameLine();
        ImGui::TextDisabled("%d fps", cfg.outputCap);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keeps the displayed frame rate at or below the cap (Reflex limiter).\n"
                          "Rendering then runs at cap / multiplier, which adds latency at high multipliers.\n"
                          "Usually better: pick the multiplier whose output matches your refresh rate.");
    ImGui::Spacing();

    bool gameCamera = cfg.useGameCamera;
    if (ImGui::Checkbox("Use game camera data", &gameCamera)) SetUseGameCamera(gameCamera);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Feed DLSS-G the game's real camera (position, rotation, FOV).\nOff = approximate camera.");
    bool readout = cfg.readout;
    if (ImGui::Checkbox("Corner readout", &readout)) SetReadout(readout);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Small always-on readout in the top-right corner while this menu is closed:\n"
                          "multiplier (Auto's current choice), rendered fps and output fps.");
    ImGui::Spacing();

    if (!st.slActive) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Streamline is not active - see EnshroudedFG\\log.txt");
    } else if (st.multiplier == 1) {
        ImGui::TextUnformatted("Status: off");
    } else if (!st.worldFrames) {
        ImGui::TextUnformatted("Status: waiting for the game world (off in menus and loading screens)");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "Status: %ux%s - paused while this menu is open", st.multiplier,
                           cfg.multiplier == kMultiplierAuto ? " (Auto)" : "");
    }
    if (st.status) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "DLSS-G status flags: 0x%x", st.status);
    ImGui::Text("Rendered: %.0f fps   Expected output: ~%.0f fps", st.baseFps, st.baseFps * st.multiplier);
    if (st.outputCapFps > 0)
        ImGui::Text("Output capped at %.0f fps -> rendering limited to %.0f fps at %ux", st.outputCapFps,
                    st.outputCapFps / st.multiplier, st.multiplier);
    if (st.maxMultiplier) ImGui::TextDisabled("This GPU supports up to %ux.", st.maxMultiplier);
    if (st.refreshHz && st.baseFps > 1 && cfg.outputCap == 0 && cfg.multiplier != kMultiplierAuto) {
        const int fit = static_cast<int>(st.refreshHz / st.baseFps);
        ImGui::TextDisabled("Tip: at %.0f rendered fps, %dx already reaches your %u Hz display.", st.baseFps,
                            fit < 2 ? 2 : (fit > 6 ? 6 : fit), st.refreshHz);
    }
    static const char* kModes[] = {"as the game requests", "immediate (no vsync)", "mailbox", "fifo (vsync)"};
    ImGui::TextDisabled("Present mode: %s - [Display] PresentMode in the ini, applies after restart.",
                        kModes[cfg.presentModeOverride]);

    ImGui::Separator();
    ImGui::TextDisabled("%s closes this menu. Changes are saved to EnshroudedFG.ini.", KeyName(cfg.overlayKey));
    ImGui::End();
}

// Always-on corner readout (menu closed). Drawn onto the final image like the game's HUD, so the
// HUD-less colour keeps DLSS-G from interpolating it.
void DrawReadout() {
    const sl_bridge::FgStatus st = sl_bridge::GetFgStatus();
    const bool autoMode = LoadConfig().multiplier == kMultiplierAuto;
    char mode[32];
    if (!st.slActive)
        snprintf(mode, sizeof(mode), "FG unavailable");
    else if (st.multiplier <= 1)
        snprintf(mode, sizeof(mode), "FG off");
    else
        snprintf(mode, sizeof(mode), "FG %s%ux%s", autoMode ? "Auto " : "", st.multiplier, st.fgOn ? "" : " paused");
    const double output = st.baseFps * (st.fgOn ? st.multiplier : 1);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12, vp->WorkPos.y + 12), ImGuiCond_Always,
                            ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##efgreadout", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::Text("%s  |  %.0f rendered  |  %.0f output fps", mode, st.baseFps, output);
    ImGui::End();
}

void RecordAndSubmit(VkQueue queue, PerImage& pi, const VkPresentInfoKHR* info) {
    ImDrawData* dd = ImGui::GetDrawData();

    p_ResetCommandBuffer(pi.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_BeginCommandBuffer(pi.cmd, &bi);

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = pi.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_CmdPipelineBarrier(pi.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkRenderingAttachmentInfoKHR color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    color.imageView = pi.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, g_extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    p_CmdBeginRendering(pi.cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, pi.cmd);
    p_CmdEndRendering(pi.cmd);

    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    p_CmdPipelineBarrier(pi.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    p_EndCommandBuffer(pi.cmd);

    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &pi.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &pi.done;
    p_QueueSubmit(queue, 1, &si, pi.fence);
}

}  // namespace

// ---- public API -----------------------------------------------------------------------------

void OnWindow(HWND hwnd) {
    if (!hwnd || g_hwnd == hwnd) return;
    if (!g_imguiContext) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().WindowRounding = 6.0f;
        ImGui::GetStyle().FrameRounding = 4.0f;
        ImGui_ImplWin32_Init(hwnd);
        g_imguiContext = true;
    }
    g_hwnd = hwnd;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
    EFG_LOG("overlay: window %p subclassed (toggle key: %s)", (void*)hwnd, KeyName(LoadConfig().overlayKey));
}

void OnDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device) {
    g_instance = instance;
    g_physicalDevice = physicalDevice;
    g_device = device;
    InitVulkan();
}

void OnSwapchainCreated(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info) {
    if (!g_vkReady) return;
    DestroyImages();
    g_swapchain = swapchain;
    g_format = info.imageFormat;
    g_extent = info.imageExtent;
    g_minImageCount = info.minImageCount;
}

void OnSwapchainImages(VkSwapchainKHR swapchain, uint32_t count, const VkImage* images) {
    if (!g_vkReady || swapchain != g_swapchain || !images) return;
    if (g_images.size() == count) {
        bool same = true;
        for (uint32_t i = 0; i < count; ++i) same &= g_images[i].image == images[i];
        if (same) return;
    }
    DestroyImages();
    g_images.resize(count);
    for (uint32_t i = 0; i < count; ++i) g_images[i].image = images[i];
    EFG_LOG("overlay: tracking %u swapchain images (%ux%u, format %d)", count, g_extent.width, g_extent.height, g_format);
}

void OnSwapchainDestroyed(VkSwapchainKHR swapchain) {
    if (swapchain != g_swapchain) return;
    DestroyImages();
    g_swapchain = VK_NULL_HANDLE;
}

void OnDeviceDestroying(VkDevice device) {
    if (device != g_device || !g_vkReady) return;
    p_DeviceWaitIdle(g_device);
    DestroyImages();
    if (g_imguiVulkan) ImGui_ImplVulkan_Shutdown();
    g_imguiVulkan = false;
    if (g_pool) p_DestroyCommandPool(g_device, g_pool, nullptr);
    g_pool = VK_NULL_HANDLE;
    g_vkReady = false;
    g_device = VK_NULL_HANDLE;
}

bool Visible() { return g_visible.load(); }
HWND Window() { return g_hwnd; }

const VkPresentInfoKHR* BeforePresent(VkQueue queue, const VkPresentInfoKHR* info, VkPresentInfoKHR& storage) {
    const bool menu = g_visible.load();
    if (!(menu || LoadConfig().readout) || !g_vkReady || !g_imguiContext || !info || info->swapchainCount != 1 ||
        info->pSwapchains[0] != g_swapchain) {
        return info;
    }
    const uint32_t index = info->pImageIndices[0];
    if (index >= g_images.size()) return info;
    PerImage& pi = g_images[index];
    if (!EnsureImageResources(pi) || !EnsureImGuiVulkan(queue)) return info;
    if (p_WaitForFences(g_device, 1, &pi.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return info;
    p_ResetFences(g_device, 1, &pi.fence);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (menu)
        DrawMenu();
    else
        DrawReadout();
    ImGui::Render();
    RecordAndSubmit(queue, pi, info);

    storage = *info;
    storage.waitSemaphoreCount = 1;
    storage.pWaitSemaphores = &pi.done;
    return &storage;
}

void InstallInputHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")), &HookSetCursorPos, &g_origSetCursorPos,
                "user32!SetCursorPos");
    InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")), &HookClipCursor, &g_origClipCursor,
                "user32!ClipCursor");
}

}  // namespace efg::overlay
