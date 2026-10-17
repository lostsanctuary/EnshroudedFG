// Vulkan interception. Core entry points are hooked at their vulkan-1.dll exports; lookups through
// vkGet{Instance,Device}ProcAddr hand out those patched exports so every call path reaches the same
// detour once. When Streamline is active, the detours forward the functions SL must intercept
// (sl_hooks.h, "Mandatory - Vulkan", plus the optional instance/device proxies) to SL's proxies.
//
// Re-entry: SL's proxies call back into Vulkan through the same exports. A detour therefore calls
// the real function directly when we are already inside an SL call on this thread, or when its
// caller lives in SL / DLSS-G / our own module (SL worker threads, tail calls).

#include "sl/sl_bridge.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "core/module_ranges.h"
#include "hooks/hook_util.h"
#include "debug/frame_trace.h"
#include "hooks/hooks.h"
#include "overlay/overlay.h"

namespace efg {
namespace {

namespace sl = sl_bridge;

PFN_vkGetInstanceProcAddr g_origGIPA = nullptr;  // MinHook trampolines
PFN_vkGetDeviceProcAddr g_origGDPA = nullptr;

// Trampolines to the original vulkan-1 exports.
PFN_vkCreateInstance r_CreateInstance = nullptr;
PFN_vkCreateDevice r_CreateDevice = nullptr;
PFN_vkDestroyDevice r_DestroyDevice = nullptr;
PFN_vkCreateWin32SurfaceKHR r_CreateWin32Surface = nullptr;
PFN_vkDestroySurfaceKHR r_DestroySurface = nullptr;
PFN_vkCreateSwapchainKHR r_CreateSwapchain = nullptr;
PFN_vkDestroySwapchainKHR r_DestroySwapchain = nullptr;
PFN_vkGetSwapchainImagesKHR r_GetSwapchainImages = nullptr;
PFN_vkAcquireNextImageKHR r_AcquireNextImage = nullptr;
PFN_vkQueuePresentKHR r_QueuePresent = nullptr;
PFN_vkDeviceWaitIdle r_DeviceWaitIdle = nullptr;

// Non-exported extension functions, wrapped at lookup time (logging only).
std::atomic<PFN_vkSetLatencySleepModeNV> g_realSetLatencySleepMode{nullptr};
std::atomic<PFN_vkLatencySleepNV> g_realLatencySleep{nullptr};
std::atomic<PFN_vkSetLatencyMarkerNV> g_realSetLatencyMarker{nullptr};

VkInstance g_mainInstance = VK_NULL_HANDLE;
std::atomic<unsigned long long> g_frame{0};
std::atomic<int> g_markerLogBudget{48};
std::atomic<int> g_presentLogBudget{3};

// Route this call to Streamline? Only for calls coming from the game itself.
bool FromGame(const void* returnAddress) { return !InSlCall() && !IsInternalAddress(returnAddress); }

std::string ChainTypes(const void* pNext) {
    std::string s;
    for (auto* p = static_cast<const VkBaseInStructure*>(pNext); p; p = p->pNext) {
        if (!s.empty()) s += ",";
        s += std::to_string(static_cast<int>(p->sType));
    }
    return s.empty() ? "-" : s;
}

const char* PresentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "other";
    }
}

bool HasExtension(const VkInstanceCreateInfo* ci, const char* name) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        if (strcmp(ci->ppEnabledExtensionNames[i], name) == 0) return true;
    }
    return false;
}

// ---- instance / device ----------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL HookCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                                  VkInstance* out) {
    if (!FromGame(_ReturnAddress())) return r_CreateInstance(ci, alloc, out);

    const VkApplicationInfo* app = ci->pApplicationInfo;
    EFG_LOG("vkCreateInstance: app '%s' engine '%s' apiVersion %u.%u.%u, %u layers, %u extensions, pNext [%s]",
            app && app->pApplicationName ? app->pApplicationName : "", app && app->pEngineName ? app->pEngineName : "",
            app ? VK_API_VERSION_MAJOR(app->apiVersion) : 0, app ? VK_API_VERSION_MINOR(app->apiVersion) : 0,
            app ? VK_API_VERSION_PATCH(app->apiVersion) : 0, ci->enabledLayerCount, ci->enabledExtensionCount,
            ChainTypes(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) EFG_LOG("  ext %s", ci->ppEnabledExtensionNames[i]);

    // The game first creates a throwaway instance (no extensions) to probe devices; only the real,
    // surface-capable instance goes through Streamline.
    const bool mainInstance = HasExtension(ci, VK_KHR_SURFACE_EXTENSION_NAME);
    VkResult r;
    if (mainInstance && sl::Initialize()) {
        PFN_vkCreateInstance proxy = sl::CreateInstanceProxy();
        {
            SlCallScope scope;
            r = proxy ? proxy(ci, alloc, out) : r_CreateInstance(ci, alloc, out);
        }
        EFG_LOG("vkCreateInstance via %s -> %d, instance %p", proxy ? "SL proxy" : "native", r,
                r == VK_SUCCESS ? (void*)*out : nullptr);
        if (r == VK_SUCCESS && proxy) {
            g_mainInstance = *out;
            sl::OnInstanceCreated(*out);
        }
        return r;
    }
    r = r_CreateInstance(ci, alloc, out);
    EFG_LOG("vkCreateInstance -> %d, instance %p", r, r == VK_SUCCESS ? (void*)*out : nullptr);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateWin32Surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                      const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
    if (FromGame(_ReturnAddress())) {
        if (auto proxy = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(sl::InstanceProxy(instance, "vkCreateWin32SurfaceKHR"))) {
            SlCallScope scope;
            VkResult r = proxy(instance, ci, alloc, out);
            EFG_LOG("vkCreateWin32SurfaceKHR via SL -> %d, surface %p", r, r == VK_SUCCESS ? (void*)*out : nullptr);
            if (r == VK_SUCCESS) overlay::OnWindow(ci->hwnd);
            return r;
        }
    }
    return r_CreateWin32Surface(instance, ci, alloc, out);
}

VKAPI_ATTR void VKAPI_CALL HookDestroySurface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* alloc) {
    if (FromGame(_ReturnAddress())) {
        if (auto proxy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(sl::InstanceProxy(instance, "vkDestroySurfaceKHR"))) {
            SlCallScope scope;
            proxy(instance, surface, alloc);
            return;
        }
    }
    r_DestroySurface(instance, surface, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                                const VkAllocationCallbacks* alloc, VkDevice* out) {
    if (!FromGame(_ReturnAddress())) return r_CreateDevice(pd, ci, alloc, out);

    EFG_LOG("vkCreateDevice: physicalDevice %p, %u queue infos, %u extensions, pNext [%s]", pd,
            ci->queueCreateInfoCount, ci->enabledExtensionCount, ChainTypes(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
        const auto& q = ci->pQueueCreateInfos[i];
        EFG_LOG("  queue family %u count %u flags 0x%x", q.queueFamilyIndex, q.queueCount, q.flags);
    }
    PFN_vkCreateDevice proxy = sl::CreateDeviceProxy(g_mainInstance);
    VkResult r;
    {
        SlCallScope scope;
        // SL's vkCreateDevice finds the instance through a physical-device map that only its own
        // vkEnumeratePhysicalDevices proxy fills. The game enumerates natively, so do it here.
        if (proxy) {
            auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                sl::InstanceProxy(g_mainInstance, "vkEnumeratePhysicalDevices"));
            uint32_t count = 0;
            VkPhysicalDevice devices[16]{};
            VkResult er = VK_ERROR_INITIALIZATION_FAILED;
            if (enumerate && enumerate(g_mainInstance, &count, nullptr) == VK_SUCCESS) {
                if (count > 16) count = 16;
                er = enumerate(g_mainInstance, &count, devices);
            }
            bool found = false;
            for (uint32_t i = 0; i < count; ++i) found |= devices[i] == pd;
            EFG_LOG("SL vkEnumeratePhysicalDevices -> %d, %u devices, target device %s", er, count,
                    found ? "found" : "NOT FOUND");
            if (!found) proxy = nullptr;  // SL could not map this device to its instance
        }
        r = proxy ? proxy(pd, ci, alloc, out) : r_CreateDevice(pd, ci, alloc, out);
    }
    EFG_LOG("vkCreateDevice via %s -> %d, device %p", proxy ? "SL proxy" : "native", r,
            r == VK_SUCCESS ? (void*)*out : nullptr);
    if (r == VK_SUCCESS && proxy) {
        sl::OnDeviceCreated(pd, *out);
        overlay::OnDevice(g_mainInstance, pd, *out);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL HookDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    if (FromGame(_ReturnAddress())) {
        EFG_LOG("vkDestroyDevice %p", (void*)device);
        overlay::OnDeviceDestroying(device);
        sl::OnDeviceDestroying(device);
    }
    r_DestroyDevice(device, alloc);
}

// ---- swapchain / present --------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL HookCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                   const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    const bool fromGame = FromGame(_ReturnAddress());
    const bool viaSl = fromGame && sl::DeviceActive(device);
    VkResult r;
    if (viaSl) {
        SlCallScope scope;
        r = sl::Proxies().createSwapchain(device, ci, alloc, out);
    } else {
        r = r_CreateSwapchain(device, ci, alloc, out);
    }
    EFG_LOG("vkCreateSwapchainKHR%s: %ux%u format %d colorSpace %d minImages %u usage 0x%x presentMode %s "
            "oldSwapchain %p pNext [%s] -> %d, swapchain %p",
            viaSl ? " via SL" : (fromGame ? "" : " (internal)"), ci->imageExtent.width, ci->imageExtent.height,
            ci->imageFormat, ci->imageColorSpace, ci->minImageCount, ci->imageUsage, PresentModeName(ci->presentMode),
            (void*)ci->oldSwapchain, ChainTypes(ci->pNext).c_str(), r, r == VK_SUCCESS ? (void*)*out : nullptr);
    if (viaSl && r == VK_SUCCESS) overlay::OnSwapchainCreated(*out, *ci);
    return r;
}

VKAPI_ATTR void VKAPI_CALL HookDestroySwapchain(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    if (FromGame(_ReturnAddress()) && sl::DeviceActive(device)) {
        EFG_LOG("vkDestroySwapchainKHR %p via SL", (void*)sc);
        overlay::OnSwapchainDestroyed(sc);
        SlCallScope scope;
        sl::Proxies().destroySwapchain(device, sc, alloc);
        return;
    }
    r_DestroySwapchain(device, sc, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL HookGetSwapchainImages(VkDevice device, VkSwapchainKHR sc, uint32_t* count, VkImage* images) {
    if (FromGame(_ReturnAddress()) && sl::DeviceActive(device)) {
        VkResult r;
        {
            SlCallScope scope;
            r = sl::Proxies().getSwapchainImages(device, sc, count, images);
        }
        if (r == VK_SUCCESS && images) {
            overlay::OnSwapchainImages(sc, *count, images);
            sl::OnSwapchainImages(*count, images);
            frame_trace::OnSwapchainImages(*count, images);
        }
        return r;
    }
    return r_GetSwapchainImages(device, sc, count, images);
}

VKAPI_ATTR VkResult VKAPI_CALL HookAcquireNextImage(VkDevice device, VkSwapchainKHR sc, uint64_t timeout,
                                                    VkSemaphore sem, VkFence fence, uint32_t* index) {
    VkResult r;
    if (FromGame(_ReturnAddress()) && sl::DeviceActive(device)) {
        sl::OnAcquire();
        SlCallScope scope;
        r = sl::Proxies().acquireNextImage(device, sc, timeout, sem, fence, index);
    } else {
        r = r_AcquireNextImage(device, sc, timeout, sem, fence, index);
    }
    if (r != VK_SUCCESS) EFG_LOG("vkAcquireNextImageKHR -> %d", r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    if (!FromGame(_ReturnAddress())) return r_QueuePresent(queue, info);

    unsigned long long frame = g_frame.fetch_add(1) + 1;
    if (g_presentLogBudget.fetch_sub(1) > 0) {
        EFG_LOG("vkQueuePresentKHR frame %llu: queue %p, %u swapchains, %u wait semaphores, pNext [%s]%s", frame,
                (void*)queue, info->swapchainCount, info->waitSemaphoreCount, ChainTypes(info->pNext).c_str(),
                sl::QueueActive() ? " via SL" : "");
    }
    frame_trace::OnPresent(queue, info);
    NgxOnPresent(frame);
    VkPresentInfoKHR overlayInfo;
    const VkPresentInfoKHR* presentInfo = overlay::BeforePresent(queue, info, overlayInfo);
    VkResult r;
    if (sl::QueueActive()) {
        sl::BeforePresent();
        {
            SlCallScope scope;
            r = sl::Proxies().queuePresent(queue, presentInfo);
        }
        sl::AfterPresent(frame);
    } else {
        r = r_QueuePresent(queue, presentInfo);
    }
    if (r != VK_SUCCESS) EFG_LOG("vkQueuePresentKHR frame %llu -> %d", frame, r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookDeviceWaitIdle(VkDevice device) {
    if (FromGame(_ReturnAddress()) && sl::DeviceActive(device)) {
        SlCallScope scope;
        return sl::Proxies().deviceWaitIdle(device);
    }
    return r_DeviceWaitIdle(device);
}

// ---- NV latency (logging only; the game does not enable VK_NV_low_latency2 itself) ----------

VKAPI_ATTR VkResult VKAPI_CALL HookSetLatencySleepMode(VkDevice device, VkSwapchainKHR sc,
                                                       const VkLatencySleepModeInfoNV* info) {
    EFG_LOG("game vkSetLatencySleepModeNV: lowLatency %d boost %d", info ? info->lowLatencyMode : -1,
            info ? info->lowLatencyBoost : -1);
    return g_realSetLatencySleepMode.load()(device, sc, info);
}

VKAPI_ATTR VkResult VKAPI_CALL HookLatencySleep(VkDevice device, VkSwapchainKHR sc, const VkLatencySleepInfoNV* info) {
    return g_realLatencySleep.load()(device, sc, info);
}

VKAPI_ATTR void VKAPI_CALL HookSetLatencyMarker(VkDevice device, VkSwapchainKHR sc, const VkSetLatencyMarkerInfoNV* info) {
    if (g_markerLogBudget.fetch_sub(1) > 0) {
        EFG_LOG("game vkSetLatencyMarkerNV %d presentID %llu", info->marker, (unsigned long long)info->presentID);
    }
    g_realSetLatencyMarker.load()(device, sc, info);
}

// ---- HUD-less capture (wrapped at lookup time) ----------------------------------------------

PFN_vkCreateImageView r_CreateImageView = nullptr;
PFN_vkCmdBeginRendering r_CmdBeginRendering = nullptr;
PFN_vkCmdBeginRendering r_CmdBeginRenderingKHR = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL HookCreateImageView(VkDevice device, const VkImageViewCreateInfo* ci,
                                                   const VkAllocationCallbacks* alloc, VkImageView* out) {
    VkResult r = r_CreateImageView(device, ci, alloc, out);
    if (r == VK_SUCCESS) sl::OnImageViewCreated(*out, ci->image);
    return r;
}

VKAPI_ATTR void VKAPI_CALL HookCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri) {
    sl::OnBeginRendering(cmd, ri);
    r_CmdBeginRendering(cmd, ri);
}

VKAPI_ATTR void VKAPI_CALL HookCmdBeginRenderingKHR(VkCommandBuffer cmd, const VkRenderingInfo* ri) {
    sl::OnBeginRendering(cmd, ri);
    r_CmdBeginRenderingKHR(cmd, ri);
}

// ---- lookup interception --------------------------------------------------------------------

struct ExportHook {
    const char* name;
    void* detour;
    void** real;
    void* exportAddr = nullptr;
};

ExportHook g_exportHooks[] = {
    {"vkCreateInstance", reinterpret_cast<void*>(&HookCreateInstance), reinterpret_cast<void**>(&r_CreateInstance)},
    {"vkCreateDevice", reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&r_CreateDevice)},
    {"vkDestroyDevice", reinterpret_cast<void*>(&HookDestroyDevice), reinterpret_cast<void**>(&r_DestroyDevice)},
    {"vkCreateWin32SurfaceKHR", reinterpret_cast<void*>(&HookCreateWin32Surface), reinterpret_cast<void**>(&r_CreateWin32Surface)},
    {"vkDestroySurfaceKHR", reinterpret_cast<void*>(&HookDestroySurface), reinterpret_cast<void**>(&r_DestroySurface)},
    {"vkCreateSwapchainKHR", reinterpret_cast<void*>(&HookCreateSwapchain), reinterpret_cast<void**>(&r_CreateSwapchain)},
    {"vkDestroySwapchainKHR", reinterpret_cast<void*>(&HookDestroySwapchain), reinterpret_cast<void**>(&r_DestroySwapchain)},
    {"vkGetSwapchainImagesKHR", reinterpret_cast<void*>(&HookGetSwapchainImages), reinterpret_cast<void**>(&r_GetSwapchainImages)},
    {"vkAcquireNextImageKHR", reinterpret_cast<void*>(&HookAcquireNextImage), reinterpret_cast<void**>(&r_AcquireNextImage)},
    {"vkQueuePresentKHR", reinterpret_cast<void*>(&HookQueuePresent), reinterpret_cast<void**>(&r_QueuePresent)},
    {"vkDeviceWaitIdle", reinterpret_cast<void*>(&HookDeviceWaitIdle), reinterpret_cast<void**>(&r_DeviceWaitIdle)},
};

template <class Pfn>
PFN_vkVoidFunction Wrap(PFN_vkVoidFunction real, std::atomic<Pfn>& slot, Pfn hook) {
    if (!real) return nullptr;  // extension not enabled: keep returning null
    slot.store(reinterpret_cast<Pfn>(real));
    return reinterpret_cast<PFN_vkVoidFunction>(hook);
}

std::mutex g_lookupMutex;
std::unordered_set<std::string> g_lookedUp;
constexpr size_t kMaxLoggedLookups = 300;

void LogLookup(const char* api, const void* handle, const char* name, PFN_vkVoidFunction real) {
    std::lock_guard lock(g_lookupMutex);
    if (g_lookedUp.size() >= kMaxLoggedLookups) return;
    if (!g_lookedUp.insert(std::string(api) + name).second) return;
    EFG_LOG("lookup %s(%p, %s) -> %p", api, handle, name, (void*)real);
}

PFN_vkVoidFunction InterceptByName(const char* name, PFN_vkVoidFunction real) {
    if (!real) return nullptr;
    for (const auto& h : g_exportHooks) {
        if (h.exportAddr && strcmp(name, h.name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(h.exportAddr);
    }
    if (strcmp(name, "vkSetLatencySleepModeNV") == 0) return Wrap(real, g_realSetLatencySleepMode, &HookSetLatencySleepMode);
    if (strcmp(name, "vkLatencySleepNV") == 0) return Wrap(real, g_realLatencySleep, &HookLatencySleep);
    if (strcmp(name, "vkSetLatencyMarkerNV") == 0) return Wrap(real, g_realSetLatencyMarker, &HookSetLatencyMarker);
    real = frame_trace::Intercept(name, real);  // no-op unless [Debug] FrameTrace=1
    if (strcmp(name, "vkCreateImageView") == 0) {
        r_CreateImageView = reinterpret_cast<PFN_vkCreateImageView>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&HookCreateImageView);
    }
    if (strcmp(name, "vkCmdBeginRendering") == 0) {
        r_CmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&HookCmdBeginRendering);
    }
    if (strcmp(name, "vkCmdBeginRenderingKHR") == 0) {
        r_CmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRendering>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&HookCmdBeginRenderingKHR);
    }
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkVoidFunction real = g_origGDPA(device, name);
    if (!name || !FromGame(_ReturnAddress())) return real;  // SL and DLSS-G get the raw functions
    LogLookup("GDPA", device, name, real);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return real ? reinterpret_cast<PFN_vkVoidFunction>(&HookGetDeviceProcAddr) : nullptr;
    }
    return InterceptByName(name, real);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetInstanceProcAddr(VkInstance instance, const char* name) {
    PFN_vkVoidFunction real = g_origGIPA(instance, name);
    if (!name || !FromGame(_ReturnAddress())) return real;
    LogLookup("GIPA", instance, name, real);
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&HookGetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return real ? reinterpret_cast<PFN_vkVoidFunction>(&HookGetDeviceProcAddr) : nullptr;
    }
    return InterceptByName(name, real);
}

}  // namespace

PFN_RealVoid RealDeviceProc(void* device, const char* name) {
    return g_origGDPA ? reinterpret_cast<PFN_RealVoid>(g_origGDPA(static_cast<VkDevice>(device), name)) : nullptr;
}

PFN_RealVoid RealInstanceProc(void* instance, const char* name) {
    return g_origGIPA ? reinterpret_cast<PFN_RealVoid>(g_origGIPA(static_cast<VkInstance>(instance), name)) : nullptr;
}

void InstallVulkanHooks(HMODULE vulkan1) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(vulkan1, path, MAX_PATH);
    EFG_LOG("vulkan-1.dll loaded: %ls", path);

    // Create every export hook (filling its trampoline) before enabling any of them, so a detour
    // can never run with an empty "real" pointer.
    for (auto& h : g_exportHooks) {
        void* target = reinterpret_cast<void*>(GetProcAddress(vulkan1, h.name));
        MH_STATUS st = target ? MH_CreateHook(target, h.detour, h.real) : MH_ERROR_FUNCTION_NOT_FOUND;
        EFG_LOG("hook vulkan-1!%s @ %p: %s", h.name, target, MH_StatusToString(st));
        if (st == MH_OK) h.exportAddr = target;
    }
    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    EFG_LOG("vulkan-1 export hooks enabled: %s", MH_StatusToString(en));

    // Lookup hooks last, so they never hand out an export that is not hooked yet.
    InstallHook(reinterpret_cast<void*>(GetProcAddress(vulkan1, "vkGetInstanceProcAddr")), &HookGetInstanceProcAddr,
                &g_origGIPA, "vulkan-1!vkGetInstanceProcAddr");
    InstallHook(reinterpret_cast<void*>(GetProcAddress(vulkan1, "vkGetDeviceProcAddr")), &HookGetDeviceProcAddr,
                &g_origGDPA, "vulkan-1!vkGetDeviceProcAddr");
}

}  // namespace efg
