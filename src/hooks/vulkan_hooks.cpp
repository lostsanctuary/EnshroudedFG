// Vulkan interception. Enshrouded loads vulkan-1.dll dynamically and resolves every entry point
// through vkGetInstanceProcAddr / vkGetDeviceProcAddr, so hooking those two exports lets us hand
// out our own wrappers for the functions we care about. Phase 2: observe and log only.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"

namespace efg {
namespace {

PFN_vkGetInstanceProcAddr g_origGIPA = nullptr;  // MinHook trampolines
PFN_vkGetDeviceProcAddr g_origGDPA = nullptr;

// "Next" pointers for the wrappers we hand out. Filled from whichever lookup the game used.
std::atomic<PFN_vkCreateInstance> g_realCreateInstance{nullptr};
std::atomic<PFN_vkCreateDevice> g_realCreateDevice{nullptr};
std::atomic<PFN_vkCreateSwapchainKHR> g_realCreateSwapchain{nullptr};
std::atomic<PFN_vkDestroySwapchainKHR> g_realDestroySwapchain{nullptr};
std::atomic<PFN_vkAcquireNextImageKHR> g_realAcquireNextImage{nullptr};
std::atomic<PFN_vkQueuePresentKHR> g_realQueuePresent{nullptr};
std::atomic<PFN_vkSetLatencySleepModeNV> g_realSetLatencySleepMode{nullptr};
std::atomic<PFN_vkLatencySleepNV> g_realLatencySleep{nullptr};
std::atomic<PFN_vkSetLatencyMarkerNV> g_realSetLatencyMarker{nullptr};

std::atomic<unsigned long long> g_frame{0};
std::atomic<int> g_markerLogBudget{48};
std::atomic<int> g_sleepLogBudget{4};
std::atomic<int> g_presentLogBudget{3};

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

const char* MarkerName(VkLatencyMarkerNV m) {
    switch (m) {
        case VK_LATENCY_MARKER_SIMULATION_START_NV: return "SIM_START";
        case VK_LATENCY_MARKER_SIMULATION_END_NV: return "SIM_END";
        case VK_LATENCY_MARKER_RENDERSUBMIT_START_NV: return "RENDER_START";
        case VK_LATENCY_MARKER_RENDERSUBMIT_END_NV: return "RENDER_END";
        case VK_LATENCY_MARKER_PRESENT_START_NV: return "PRESENT_START";
        case VK_LATENCY_MARKER_PRESENT_END_NV: return "PRESENT_END";
        case VK_LATENCY_MARKER_INPUT_SAMPLE_NV: return "INPUT_SAMPLE";
        case VK_LATENCY_MARKER_TRIGGER_FLASH_NV: return "TRIGGER_FLASH";
        default: return "other";
    }
}

// ---- wrappers -------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL HookCreateInstance(const VkInstanceCreateInfo* ci,
                                                  const VkAllocationCallbacks* alloc,
                                                  VkInstance* out) {
    const VkApplicationInfo* app = ci->pApplicationInfo;
    EFG_LOG("vkCreateInstance: app '%s' engine '%s' apiVersion %u.%u.%u, %u layers, %u extensions, pNext [%s]",
            app && app->pApplicationName ? app->pApplicationName : "", app && app->pEngineName ? app->pEngineName : "",
            app ? VK_API_VERSION_MAJOR(app->apiVersion) : 0, app ? VK_API_VERSION_MINOR(app->apiVersion) : 0,
            app ? VK_API_VERSION_PATCH(app->apiVersion) : 0, ci->enabledLayerCount, ci->enabledExtensionCount,
            ChainTypes(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->enabledLayerCount; ++i) EFG_LOG("  layer %s", ci->ppEnabledLayerNames[i]);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) EFG_LOG("  ext %s", ci->ppEnabledExtensionNames[i]);
    VkResult r = g_realCreateInstance.load()(ci, alloc, out);
    EFG_LOG("vkCreateInstance -> %d, instance %p", r, r == VK_SUCCESS ? (void*)*out : nullptr);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                                const VkAllocationCallbacks* alloc, VkDevice* out) {
    EFG_LOG("vkCreateDevice: physicalDevice %p, %u queue infos, %u extensions, pNext [%s]", pd,
            ci->queueCreateInfoCount, ci->enabledExtensionCount, ChainTypes(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
        const auto& q = ci->pQueueCreateInfos[i];
        EFG_LOG("  queue family %u count %u flags 0x%x pNext [%s]", q.queueFamilyIndex, q.queueCount, q.flags,
                ChainTypes(q.pNext).c_str());
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) EFG_LOG("  ext %s", ci->ppEnabledExtensionNames[i]);
    VkResult r = g_realCreateDevice.load()(pd, ci, alloc, out);
    EFG_LOG("vkCreateDevice -> %d, device %p", r, r == VK_SUCCESS ? (void*)*out : nullptr);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                   const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    VkResult r = g_realCreateSwapchain.load()(device, ci, alloc, out);
    EFG_LOG("vkCreateSwapchainKHR: %ux%u format %d colorSpace %d minImages %u usage 0x%x presentMode %s "
            "oldSwapchain %p pNext [%s] -> %d, swapchain %p",
            ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, ci->imageColorSpace, ci->minImageCount,
            ci->imageUsage, PresentModeName(ci->presentMode), (void*)ci->oldSwapchain, ChainTypes(ci->pNext).c_str(),
            r, r == VK_SUCCESS ? (void*)*out : nullptr);
    return r;
}

VKAPI_ATTR void VKAPI_CALL HookDestroySwapchain(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    EFG_LOG("vkDestroySwapchainKHR %p", (void*)sc);
    g_realDestroySwapchain.load()(device, sc, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL HookAcquireNextImage(VkDevice device, VkSwapchainKHR sc, uint64_t timeout,
                                                    VkSemaphore sem, VkFence fence, uint32_t* index) {
    VkResult r = g_realAcquireNextImage.load()(device, sc, timeout, sem, fence, index);
    if (r != VK_SUCCESS) EFG_LOG("vkAcquireNextImageKHR -> %d", r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    unsigned long long frame = g_frame.fetch_add(1) + 1;
    if (g_presentLogBudget.fetch_sub(1) > 0) {
        uint64_t presentId = 0;
        for (auto* p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
                auto* pid = reinterpret_cast<const VkPresentIdKHR*>(p);
                if (pid->pPresentIds) presentId = pid->pPresentIds[0];
            }
        }
        EFG_LOG("vkQueuePresentKHR frame %llu: queue %p, %u swapchains, %u wait semaphores, presentId %llu, pNext [%s]",
                frame, (void*)queue, info->swapchainCount, info->waitSemaphoreCount, presentId,
                ChainTypes(info->pNext).c_str());
    }
    NgxOnPresent(frame);
    VkResult r = g_realQueuePresent.load()(queue, info);
    if (r != VK_SUCCESS) EFG_LOG("vkQueuePresentKHR frame %llu -> %d", frame, r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL HookSetLatencySleepMode(VkDevice device, VkSwapchainKHR sc,
                                                       const VkLatencySleepModeInfoNV* info) {
    EFG_LOG("vkSetLatencySleepModeNV swapchain %p: lowLatency %d boost %d minIntervalUs %u", (void*)sc,
            info ? info->lowLatencyMode : -1, info ? info->lowLatencyBoost : -1, info ? info->minimumIntervalUs : 0);
    return g_realSetLatencySleepMode.load()(device, sc, info);
}

VKAPI_ATTR VkResult VKAPI_CALL HookLatencySleep(VkDevice device, VkSwapchainKHR sc, const VkLatencySleepInfoNV* info) {
    if (g_sleepLogBudget.fetch_sub(1) > 0) {
        EFG_LOG("vkLatencySleepNV swapchain %p: semaphore %p value %llu", (void*)sc, (void*)info->signalSemaphore,
                (unsigned long long)info->value);
    }
    return g_realLatencySleep.load()(device, sc, info);
}

VKAPI_ATTR void VKAPI_CALL HookSetLatencyMarker(VkDevice device, VkSwapchainKHR sc, const VkSetLatencyMarkerInfoNV* info) {
    if (g_markerLogBudget.fetch_sub(1) > 0) {
        EFG_LOG("vkSetLatencyMarkerNV %-13s presentID %llu (presents so far %llu)", MarkerName(info->marker),
                (unsigned long long)info->presentID, g_frame.load());
    }
    g_realSetLatencyMarker.load()(device, sc, info);
}

// ---- lookup interception --------------------------------------------------------------------
//
// Core functions are hooked once, at their vulkan-1.dll export (MinHook). Lookups for those names
// return the patched export, so every call path - direct GetProcAddress on vulkan-1, the loader
// trampolines from vkGetInstanceProcAddr, or raw driver pointers from vkGetDeviceProcAddr - goes
// through the same detour exactly once. Extension functions vulkan-1 does not export (the NV
// latency ones) are wrapped at lookup time instead.

struct ExportHook {
    const char* name;
    void* detour;
    void* trampoline;  // stored into the matching g_real* slot after install
    void* exportAddr = nullptr;
};

ExportHook g_exportHooks[] = {
    {"vkCreateInstance", reinterpret_cast<void*>(&HookCreateInstance), nullptr},
    {"vkCreateDevice", reinterpret_cast<void*>(&HookCreateDevice), nullptr},
    {"vkCreateSwapchainKHR", reinterpret_cast<void*>(&HookCreateSwapchain), nullptr},
    {"vkDestroySwapchainKHR", reinterpret_cast<void*>(&HookDestroySwapchain), nullptr},
    {"vkAcquireNextImageKHR", reinterpret_cast<void*>(&HookAcquireNextImage), nullptr},
    {"vkQueuePresentKHR", reinterpret_cast<void*>(&HookQueuePresent), nullptr},
};

template <class Pfn>
PFN_vkVoidFunction Wrap(PFN_vkVoidFunction real, std::atomic<Pfn>& slot, Pfn hook) {
    if (!real) return nullptr;  // extension not enabled: keep returning null
    slot.store(reinterpret_cast<Pfn>(real));
    return reinterpret_cast<PFN_vkVoidFunction>(hook);
}

std::mutex g_lookupMutex;
std::unordered_set<std::string> g_lookedUp;
constexpr size_t kMaxLoggedLookups = 600;

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
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkVoidFunction real = g_origGDPA(device, name);
    if (!name) return real;
    LogLookup("GDPA", device, name, real);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return real ? reinterpret_cast<PFN_vkVoidFunction>(&HookGetDeviceProcAddr) : nullptr;
    }
    return InterceptByName(name, real);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetInstanceProcAddr(VkInstance instance, const char* name) {
    PFN_vkVoidFunction real = g_origGIPA(instance, name);
    if (!name) return real;
    LogLookup("GIPA", instance, name, real);
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&HookGetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return real ? reinterpret_cast<PFN_vkVoidFunction>(&HookGetDeviceProcAddr) : nullptr;
    }
    return InterceptByName(name, real);
}

}  // namespace

void InstallVulkanHooks(HMODULE vulkan1) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(vulkan1, path, MAX_PATH);
    EFG_LOG("vulkan-1.dll loaded: %ls", path);

    // Create every export hook and publish its trampoline before enabling any of them, so a detour
    // can never run with an empty "real" pointer.
    for (auto& h : g_exportHooks) {
        void* target = reinterpret_cast<void*>(GetProcAddress(vulkan1, h.name));
        MH_STATUS st = target ? MH_CreateHook(target, h.detour, &h.trampoline) : MH_ERROR_FUNCTION_NOT_FOUND;
        EFG_LOG("hook vulkan-1!%s @ %p: %s", h.name, target, MH_StatusToString(st));
        if (st == MH_OK) h.exportAddr = target;
    }
    g_realCreateInstance.store(reinterpret_cast<PFN_vkCreateInstance>(g_exportHooks[0].trampoline));
    g_realCreateDevice.store(reinterpret_cast<PFN_vkCreateDevice>(g_exportHooks[1].trampoline));
    g_realCreateSwapchain.store(reinterpret_cast<PFN_vkCreateSwapchainKHR>(g_exportHooks[2].trampoline));
    g_realDestroySwapchain.store(reinterpret_cast<PFN_vkDestroySwapchainKHR>(g_exportHooks[3].trampoline));
    g_realAcquireNextImage.store(reinterpret_cast<PFN_vkAcquireNextImageKHR>(g_exportHooks[4].trampoline));
    g_realQueuePresent.store(reinterpret_cast<PFN_vkQueuePresentKHR>(g_exportHooks[5].trampoline));
    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    EFG_LOG("vulkan-1 export hooks enabled: %s", MH_StatusToString(en));

    // Lookup hooks last, so they never hand out an export that is not hooked yet.
    InstallHook(reinterpret_cast<void*>(GetProcAddress(vulkan1, "vkGetInstanceProcAddr")), &HookGetInstanceProcAddr,
                &g_origGIPA, "vulkan-1!vkGetInstanceProcAddr");
    InstallHook(reinterpret_cast<void*>(GetProcAddress(vulkan1, "vkGetDeviceProcAddr")), &HookGetDeviceProcAddr,
                &g_origGDPA, "vulkan-1!vkGetDeviceProcAddr");
}

}  // namespace efg
