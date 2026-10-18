#include "overlay/overlay.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "hooks/hook_util.h"
#include "debug/frame_trace.h"
#include "hooks/hooks.h"
#include "overlay/theme.h"
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
    namespace th = theme;
    const sl_bridge::FgStatus st = sl_bridge::GetFgStatus();
    const Config& cfg = LoadConfig();
    const bool autoMode = cfg.multiplier == kMultiplierAuto;
    const uint32_t maxMult = st.maxMultiplier ? st.maxMultiplier : 6;

    static float lastHeight = 0.0f;
    const float width = th::S(640);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, lastHeight > 0 ? lastHeight : th::S(720)));
    ImGui::Begin("##efgmenu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 wp = ImGui::GetWindowPos();
    th::PanelChrome(ImGui::GetWindowDrawList(), wp, ImVec2(wp.x + width, wp.y + ImGui::GetWindowHeight()));

    char version[32];
    snprintf(version, sizeof(version), "v%s", EFG_VERSION_STRING);
    th::TitleBar("FRAME GENERATION", version);

    // ---- Output: the settings rows ------------------------------------------------------------
    th::SectionHeader("Output");
    int row = 0;

    // Multiplier: Off, 2x .. max, Auto.
    int options[8];
    int count = 0;
    options[count++] = 1;
    for (int m = 2; m <= static_cast<int>(maxMult) && m <= 6; ++m) options[count++] = m;
    options[count++] = kMultiplierAuto;
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if (options[i] == cfg.multiplier) current = i;
    }
    char multValue[32];
    if (cfg.multiplier == 1) snprintf(multValue, sizeof(multValue), "Off");
    else if (autoMode) snprintf(multValue, sizeof(multValue), "Auto (%ux)", st.autoMultiplier);
    else snprintf(multValue, sizeof(multValue), "%dx", cfg.multiplier);
    char multTip[256];
    snprintf(multTip, sizeof(multTip),
             "DLSS Multi Frame Generation: Off, 2x..%ux, or Auto.\nAuto picks the multiplier so that rendered fps x "
             "multiplier just fills your display (%.0f fps).",
             maxMult, st.refreshHz * 0.97);
    if (int d = th::StepperRow("mult", "Frame Generation", multValue, current > 0, current + 1 < count, row++, multTip,
                               st.slActive)) {
        SetMultiplier(options[current + d]);
    }

    // Output cap: Off / Auto (a custom value from the ini is shown but not offered).
    char capValue[48];
    if (cfg.outputCap == 0) snprintf(capValue, sizeof(capValue), "Off");
    else if (cfg.outputCap < 0 && st.refreshHz) snprintf(capValue, sizeof(capValue), "Auto (%.0f fps)", st.refreshHz * 0.97);
    else if (cfg.outputCap < 0) snprintf(capValue, sizeof(capValue), "Auto");
    else snprintf(capValue, sizeof(capValue), "%d fps (ini)", cfg.outputCap);
    if (int d = th::StepperRow("cap", "Output Cap", capValue, cfg.outputCap != 0, cfg.outputCap == 0, row++,
                               "Keeps the displayed frame rate at or below your refresh rate (Reflex limiter).\n"
                               "Rendering then runs at cap / multiplier, which adds latency at high multipliers.")) {
        SetOutputCap(d > 0 ? -1 : 0);
    }

    static const char* kReflex[] = {"Off", "On", "On + Boost"};
    char reflexValue[40];
    if (cfg.reflexMode == 0 && st.reflexMode != 0) snprintf(reflexValue, sizeof(reflexValue), "On (FG needs it)");
    else snprintf(reflexValue, sizeof(reflexValue), "%s", kReflex[cfg.reflexMode]);
    if (int d = th::StepperRow("reflex", "NVIDIA Reflex", reflexValue, cfg.reflexMode > 0, cfg.reflexMode < 2, row++,
                               "Reflex keeps the render queue short for lower input latency.\n"
                               "On + Boost also keeps GPU clocks up when the CPU is the bottleneck.\n"
                               "Frame generation requires Reflex, so Off only applies while it is off.",
                               st.slActive)) {
        SetReflexMode(cfg.reflexMode + d);
    }

    if (int d = th::StepperRow("camera", "Camera Data", cfg.useGameCamera ? "Game" : "Approximate", !cfg.useGameCamera,
                               cfg.useGameCamera, row++,
                               "Game: DLSS-G gets the game's real camera (position, rotation, FOV).\n"
                               "Approximate: generic camera, for comparison only.")) {
        SetUseGameCamera(d < 0);
    }

    if (int d = th::StepperRow("readout", "Frame Counter", cfg.readout ? "On" : "Off", cfg.readout, !cfg.readout, row++,
                               "Small always-on counter: multiplier, output and rendered fps.\n"
                               "While this menu is open you can drag it; right-click puts it back in the corner.")) {
        SetReadout(d > 0);
    }

    // What the counter shows: a dropdown of check items.
    struct CounterItem {
        unsigned bit;
        const char* name;
        const char* shortName;
    };
    static const CounterItem kCounterItems[] = {
        {kReadoutFg, "Frame Generation", "FG"},   {kReadoutOutput, "Output fps", "Output"},
        {kReadoutFps, "Rendered fps", "FPS"},     {kReadoutLatency, "Latency", "Latency"},
        {kReadoutFrameTime, "Frame Time", "Frame time"}, {kReadoutReflex, "Reflex Mode", "Reflex"}};
    const unsigned items = cfg.readoutItems;
    int selected = 0;
    char preview[64] = "";
    for (const auto& it : kCounterItems) {
        if (!(items & it.bit)) continue;
        if (selected++) strncat_s(preview, ", ", _TRUNCATE);
        strncat_s(preview, it.shortName, _TRUNCATE);
    }
    if (selected == static_cast<int>(std::size(kCounterItems))) snprintf(preview, sizeof(preview), "Everything");
    else if (selected > 3) snprintf(preview, sizeof(preview), "%d items", selected);
    if (th::DropdownRow("counteritems", "Counter Shows", preview, row++,
                        "Choose what the frame counter displays.", cfg.readout)) {
        for (const auto& it : kCounterItems) {
            bool on = (items & it.bit) != 0;
            const bool canToggle = !(on && selected == 1);
            if (th::DropdownCheck(it.name, &on, canToggle)) SetReadoutItems(on ? (items | it.bit) : (items & ~it.bit));
        }
        th::EndDropdown();
    }

    // ---- Status: the game's big stat numbers ---------------------------------------------------
    ImGui::Dummy(ImVec2(0, th::S(6)));
    th::SectionHeader("Status");
    ImGui::Dummy(ImVec2(0, th::S(2)));
    const unsigned shownMult = st.multiplier > 1 ? st.multiplier : 1;
    char rendered[24], output[24], mode[24];
    snprintf(rendered, sizeof(rendered), "%.0f", st.baseFps);
    // Expected output only means something in the world (FG is off in menus and loading screens).
    const bool expectOutput = st.worldFrames && st.multiplier > 1;
    snprintf(output, sizeof(output), "%.0f", st.baseFps * (expectOutput ? shownMult : 1));
    if (st.multiplier <= 1) snprintf(mode, sizeof(mode), "Off");
    else snprintf(mode, sizeof(mode), "%s%ux", autoMode ? "Auto " : "", st.multiplier);
    char latency[24];
    if (st.latencyMs > 0) snprintf(latency, sizeof(latency), "%.0f ms", st.latencyMs);
    else snprintf(latency, sizeof(latency), "-");
    const float statW = (width - th::S(56)) / 4.0f;
    const float statX = wp.x + th::S(28);
    const float statY = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(statX, statY));
    th::Stat("RENDERED", rendered, theme::col::kText, statW);
    ImGui::SetCursorScreenPos(ImVec2(statX + statW, statY));
    th::Stat("OUTPUT", output, expectOutput ? theme::col::kGold : theme::col::kTextMuted, statW);
    ImGui::SetCursorScreenPos(ImVec2(statX + 2 * statW, statY));
    th::Stat("MULTIPLIER", mode, st.multiplier > 1 ? theme::col::kAccent : theme::col::kTextMuted, statW);
    ImGui::SetCursorScreenPos(ImVec2(statX + 3 * statW, statY));
    th::Stat("LATENCY", latency, st.latencyMs > 0 ? theme::col::kText : theme::col::kTextMuted, statW);
    ImGui::SetCursorScreenPos(ImVec2(wp.x, ImGui::GetCursorScreenPos().y + th::S(12)));

    char line[160];
    if (!st.slActive) {
        th::StatusLine(theme::col::kRed, "Streamline is not active - see EnshroudedFG\\log.txt");
    } else if (st.multiplier <= 1) {
        th::StatusLine(theme::col::kTextDim, "Frame generation is off.");
    } else if (!st.worldFrames) {
        th::StatusLine(theme::col::kAccent, "Waiting for the world - frame generation is off in menus and loading screens.");
    } else {
        if (st.fgOn)
            snprintf(line, sizeof(line), "Generating at %ux - changes apply live.", st.multiplier);
        else
            snprintf(line, sizeof(line), "Starting at %ux...", st.multiplier);
        th::StatusLine(theme::col::kGreen, line);
    }
    if (st.status) {
        snprintf(line, sizeof(line), "DLSS-G reports status flags 0x%x.", st.status);
        th::StatusLine(theme::col::kRed, line);
    }
    if (st.outputCapFps > 0) {
        snprintf(line, sizeof(line), "Output capped at %.0f fps: rendering limited to %.0f fps.", st.outputCapFps,
                 st.outputCapFps / shownMult);
        th::StatusLine(theme::col::kAccent, line);
    }

    // Quieter details in italics.
    ImGui::Dummy(ImVec2(0, th::S(4)));
    ImGui::PushFont(theme::GetFonts().serifItalic, theme::kSmall + 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::col::kTextDim));
    const float textX = wp.x + th::S(48);
    ImGui::PushTextWrapPos(width - th::S(34));  // window-local x
    if (st.refreshHz && st.baseFps > 1 && cfg.outputCap == 0 && !autoMode) {
        const int fit = static_cast<int>(st.refreshHz / st.baseFps);
        ImGui::SetCursorScreenPos(ImVec2(textX, ImGui::GetCursorScreenPos().y));
        ImGui::Text("At %.0f rendered fps, %dx already fills your %u Hz display.", st.baseFps,
                    fit < 2 ? 2 : (fit > 6 ? 6 : fit), st.refreshHz);
    }
    static const char* kModes[] = {"as the game requests", "immediate", "mailbox", "fifo (vsync)"};
    ImGui::SetCursorScreenPos(ImVec2(textX, ImGui::GetCursorScreenPos().y));
    ImGui::Text("This GPU supports up to %ux. Present mode: %s.", maxMult, kModes[cfg.presentModeOverride & 3]);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // ---- Footer: key hints like the game's ("ESC Close") ----------------------------------------
    const float ruleY = ImGui::GetCursorScreenPos().y + th::S(14);
    th::GoldRule(ImGui::GetWindowDrawList(), wp.x + th::S(18), wp.x + width - th::S(18), ruleY, true);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + th::S(28), ruleY + th::S(16)));
    if (th::KeyHint(KeyName(cfg.overlayKey), "Close")) SetVisible(false);
    ImGui::SameLine(0, th::S(26));
    ImGui::PushFont(theme::GetFonts().serifItalic, theme::kSmall + 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::col::kTextMuted));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + th::S(3)));
    ImGui::TextUnformatted("Changes are saved to EnshroudedFG.ini");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    lastHeight = ImGui::GetCursorScreenPos().y - wp.y + th::S(26);
    ImGui::End();
}

// Always-on frame counter, a miniature of the menu panel: indigo plate with a gold hairline frame and
// diamond end-caps, spaced parchment captions, bold serif values (multiplier in the menu's amber, output
// in gold), gold diamonds between segments. Drawn onto the final image like the game's HUD, so the
// HUD-less colour keeps DLSS-G from interpolating it. While the menu is open it takes the mouse: drag it
// anywhere (saved on release), right-click puts it back in the top-right corner.
void DrawReadout(bool movable) {
    namespace th = theme;
    static ImVec2 lastSize(0, 0);
    static bool dragged = false;  // the user is dragging it (menu open)
    static bool snap = false;     // re-place it next frame even while movable (after a reset)
    const Config& cfg = LoadConfig();
    const sl_bridge::FgStatus st = sl_bridge::GetFgStatus();
    const bool autoMode = cfg.multiplier == kMultiplierAuto;

    // Segments in a fixed order, filtered by [Overlay] ReadoutItems.
    struct Segment {
        const char* caption;
        char value[24];
        ImU32 color;
        const char* suffix;
    };
    Segment segs[6];
    int n = 0;
    const unsigned items = cfg.readoutItems;
    const bool fgActive = st.slActive && st.multiplier > 1;
    if (items & kReadoutFg) {
        Segment& g = segs[n++];
        g.caption = "FG";
        if (!st.slActive) snprintf(g.value, sizeof(g.value), "N/A");
        else if (st.multiplier <= 1) snprintf(g.value, sizeof(g.value), "Off");
        else snprintf(g.value, sizeof(g.value), "%s%ux", autoMode ? "Auto " : "", st.multiplier);
        g.color = fgActive ? th::col::kAccent : th::col::kTextMuted;
        // FG pauses in the game's menus and loading screens: say so in muted italics.
        g.suffix = fgActive && !st.fgOn ? "paused" : nullptr;
    }
    if (items & kReadoutOutput) {
        Segment& g = segs[n++];
        g.caption = "OUTPUT";
        snprintf(g.value, sizeof(g.value), "%.0f", st.baseFps * (st.fgOn ? st.multiplier : 1));
        g.color = th::col::kGold;
        g.suffix = nullptr;
    }
    if (items & kReadoutFps) {
        Segment& g = segs[n++];
        g.caption = "FPS";
        snprintf(g.value, sizeof(g.value), "%.0f", st.baseFps);
        g.color = th::col::kText;
        g.suffix = nullptr;
    }
    if (items & kReadoutLatency) {
        Segment& g = segs[n++];
        g.caption = "LATENCY";
        if (st.latencyMs > 0) snprintf(g.value, sizeof(g.value), "%.0fms", st.latencyMs);
        else snprintf(g.value, sizeof(g.value), "-");
        g.color = st.latencyMs > 0 ? th::col::kText : th::col::kTextMuted;
        g.suffix = nullptr;
    }
    if (items & kReadoutFrameTime) {
        Segment& g = segs[n++];
        g.caption = "FRAME";
        if (st.baseFps > 1) snprintf(g.value, sizeof(g.value), "%.1fms", 1000.0 / st.baseFps);
        else snprintf(g.value, sizeof(g.value), "-");
        g.color = th::col::kText;
        g.suffix = nullptr;
    }
    if (items & kReadoutReflex) {
        Segment& g = segs[n++];
        g.caption = "REFLEX";
        static const char* kReflex[] = {"Off", "On", "Boost"};
        snprintf(g.value, sizeof(g.value), "%s", kReflex[st.reflexMode < 0 || st.reflexMode > 2 ? 1 : st.reflexMode]);
        g.color = st.reflexMode ? th::col::kGreen : th::col::kTextMuted;
        g.suffix = nullptr;
    }

    const th::Fonts& fonts = th::GetFonts();
    ImFont* capFont = fonts.serif ? fonts.serif : ImGui::GetFont();
    ImFont* valFont = fonts.serifBold ? fonts.serifBold : ImGui::GetFont();
    ImFont* itFont = fonts.serifItalic ? fonts.serifItalic : ImGui::GetFont();
    const float capSize = th::S(12.5f), valSize = th::S(18.0f), itSize = th::S(14.0f);
    const float capSpacing = th::S(1.4f);
    const float pad = th::S(20), gap = th::S(7), sep = th::S(22);
    const float h = th::S(32);

    float w = pad * 2;
    for (int i = 0; i < n; ++i) {
        w += th::SpacedTextWidth(capFont, capSize, segs[i].caption, capSpacing) + gap +
             valFont->CalcTextSizeA(valSize, FLT_MAX, 0, segs[i].value).x;
        if (segs[i].suffix) w += th::S(5) + itFont->CalcTextSizeA(itSize, FLT_MAX, 0, segs[i].suffix).x;
        if (i + 1 < n) w += sep;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiCond cond = movable && !snap ? ImGuiCond_Appearing : ImGuiCond_Always;
    snap = false;
    if (cfg.readoutX >= 0) {
        const float x = std::min(float(cfg.readoutX), vp->WorkPos.x + vp->WorkSize.x - lastSize.x);
        const float y = std::min(float(cfg.readoutY), vp->WorkPos.y + vp->WorkSize.y - lastSize.y);
        ImGui::SetNextWindowPos(ImVec2(std::max(x, vp->WorkPos.x), std::max(y, vp->WorkPos.y)), cond);
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - th::S(12), vp->WorkPos.y + th::S(12)), cond,
                                ImVec2(1, 0));
    }
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (!movable) flags |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##efgreadout", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos();
    const ImVec2 b(a.x + w, a.y + h);

    // Plate: soft shadow, the menu's indigo gradient, gold hairline inset with diamond end-caps.
    for (int i = 1; i <= 3; ++i) {
        const float o = th::S(2.0f * i);
        dl->AddRectFilled(ImVec2(a.x - o, a.y - o + th::S(2)), ImVec2(b.x + o, b.y + o + th::S(2)),
                          IM_COL32(0, 0, 0, 30 - 8 * i), o);
    }
    dl->AddRectFilledMultiColor(a, b, th::col::kPanelTop, th::col::kPanelTop, th::col::kPanelBottom,
                                th::col::kPanelBottom);
    const float in = th::S(3);
    const ImU32 frame = movable ? th::col::kGold : th::col::kGoldDim;
    dl->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), frame, 0.0f, 0, th::S(1));
    const float cy = (a.y + b.y) * 0.5f;
    for (float ex : {a.x + in, b.x - in}) {
        th::Diamond(dl, ImVec2(ex, cy), th::S(5.0f), th::col::kGold);
        th::Diamond(dl, ImVec2(ex, cy), th::S(2.5f), th::col::kPanelBottom | 0xFF000000);
    }

    float x = a.x + pad;
    for (int i = 0; i < n; ++i) {
        const Segment& g = segs[i];
        const float cw = th::SpacedTextWidth(capFont, capSize, g.caption, capSpacing);
        th::SpacedText(dl, capFont, capSize, ImVec2(x, cy - capSize * 0.5f), th::col::kParchment, g.caption, capSpacing);
        x += cw + gap;
        dl->AddText(valFont, valSize, ImVec2(x, cy - valSize * 0.55f), g.color, g.value);
        x += valFont->CalcTextSizeA(valSize, FLT_MAX, 0, g.value).x;
        if (g.suffix) {
            x += th::S(5);
            dl->AddText(itFont, itSize, ImVec2(x, cy - itSize * 0.5f), th::col::kTextMuted, g.suffix);
            x += itFont->CalcTextSizeA(itSize, FLT_MAX, 0, g.suffix).x;
        }
        if (i + 1 < n) {
            th::Diamond(dl, ImVec2(x + sep * 0.5f, cy), th::S(3.0f), th::col::kGoldDim);
            x += sep;
        }
    }
    ImGui::Dummy(ImVec2(w, h));
    lastSize = ImVec2(w, h);
    if (movable) {
        if (ImGui::IsWindowHovered() && !dragged) {
            ImGui::SetTooltip("Drag to move. Right-click: back to the top-right corner.");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                SetReadoutPos(-1, -1);
                snap = true;
            }
        }
        if (ImGui::IsWindowFocused() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) dragged = true;
        if (dragged && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragged = false;
            const ImVec2 pos = ImGui::GetWindowPos();
            SetReadoutPos(static_cast<int>(pos.x + 0.5f), static_cast<int>(pos.y + 0.5f));
        }
    } else {
        dragged = false;
    }
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
        theme::Init();
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

    theme::UpdateScale(static_cast<float>(g_extent.height));
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (menu) {
        DrawMenu();
        if (LoadConfig().readout) DrawReadout(true);  // on top of the menu, draggable
    } else {
        DrawReadout(false);
    }
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
