// Streamline integration (manual hooking). SL's own vkCreateInstance / vkCreateDevice proxies add
// the extensions, features and queues DLSS-G and Reflex need, so we route the game's real
// instance and device through them and skip slSetVulkanInfo. Phase 3: DLSS-G stays off.

#include "sl/sl_bridge.h"

#include <windows.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <atomic>
#include <string>

#include "core/log.h"
#include "core/module_ranges.h"

namespace efg::sl_bridge {
namespace {

HMODULE g_interposer = nullptr;
std::atomic<bool> g_initialized{false};
std::atomic<VkDevice> g_device{VK_NULL_HANDLE};
VkInstance g_instance = VK_NULL_HANDLE;
std::wstring g_pluginDir;

// Core API from sl.interposer.dll
PFun_slInit* p_slInit = nullptr;
PFun_slShutdown* p_slShutdown = nullptr;
PFun_slIsFeatureSupported* p_slIsFeatureSupported = nullptr;
PFun_slGetFeatureRequirements* p_slGetFeatureRequirements = nullptr;
PFun_slGetFeatureFunction* p_slGetFeatureFunction = nullptr;
PFun_slGetNewFrameToken* p_slGetNewFrameToken = nullptr;
PFN_vkGetInstanceProcAddr p_slGIPA = nullptr;
PFN_vkGetDeviceProcAddr p_slGDPA = nullptr;

// Feature API (slGetFeatureFunction)
PFun_slDLSSGSetOptions* p_slDLSSGSetOptions = nullptr;
PFun_slDLSSGGetState* p_slDLSSGGetState = nullptr;
PFun_slReflexSetOptions* p_slReflexSetOptions = nullptr;
PFun_slReflexSleep* p_slReflexSleep = nullptr;
PFun_slReflexGetState* p_slReflexGetState = nullptr;
PFun_slPCLSetMarker* p_slPCLSetMarker = nullptr;

DeviceProxies g_proxies{};

// Current frame (single render thread in this game).
sl::FrameToken* g_frame = nullptr;
bool g_simEnded = false;

const char* ResultName(sl::Result r) {
    switch (r) {
        case sl::Result::eOk: return "eOk";
        case sl::Result::eErrorIO: return "eErrorIO";
        case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
        case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated";
        case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
        case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
        case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins";
        case sl::Result::eErrorVulkanAPI: return "eErrorVulkanAPI";
        case sl::Result::eErrorDXGIAPI: return "eErrorDXGIAPI";
        case sl::Result::eErrorD3DAPI: return "eErrorD3DAPI";
        case sl::Result::eErrorNRDAPI: return "eErrorNRDAPI";
        case sl::Result::eErrorNVAPI: return "eErrorNVAPI";
        case sl::Result::eErrorReflexAPI: return "eErrorReflexAPI";
        case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed";
        case sl::Result::eErrorJSONParsing: return "eErrorJSONParsing";
        case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy";
        case sl::Result::eErrorMissingResourceState: return "eErrorMissingResourceState";
        case sl::Result::eErrorInvalidIntegration: return "eErrorInvalidIntegration";
        case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
        case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
        case sl::Result::eErrorComputeFailed: return "eErrorComputeFailed";
        case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled";
        case sl::Result::eErrorExceptionHandler: return "eErrorExceptionHandler";
        case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
        case sl::Result::eErrorMissingConstants: return "eErrorMissingConstants";
        case sl::Result::eErrorDuplicatedConstants: return "eErrorDuplicatedConstants";
        case sl::Result::eErrorMissingOrInvalidAPI: return "eErrorMissingOrInvalidAPI";
        case sl::Result::eErrorCommonConstantsMissing: return "eErrorCommonConstantsMissing";
        case sl::Result::eErrorUnsupportedInterface: return "eErrorUnsupportedInterface";
        case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
        case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
        case sl::Result::eErrorFeatureMissingHooks: return "eErrorFeatureMissingHooks";
        case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
        case sl::Result::eErrorFeatureWrongPriority: return "eErrorFeatureWrongPriority";
        case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
        case sl::Result::eErrorFeatureManagerInvalidState: return "eErrorFeatureManagerInvalidState";
        case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
        case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
        default: return "other";
    }
}

void SlLogCallback(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo) return;  // full detail goes to sl.log
    std::string m(msg ? msg : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    EFG_LOG("[SL %s] %s", type == sl::LogType::eError ? "error" : "warn", m.c_str());
}

template <class T>
bool GetCore(T*& out, const char* name) {
    out = reinterpret_cast<T*>(GetProcAddress(g_interposer, name));
    if (!out) EFG_LOG("sl.interposer is missing export %s", name);
    return out != nullptr;
}

template <class T>
void GetFeatureFn(sl::Feature feature, T*& out, const char* name) {
    void* fn = nullptr;
    sl::Result r = p_slGetFeatureFunction(feature, name, fn);
    out = reinterpret_cast<T*>(fn);
    EFG_LOG("slGetFeatureFunction(%s) -> %s", name, ResultName(r));
}

void LogRequirements(sl::Feature feature, const char* label) {
    sl::FeatureRequirements req{};
    sl::Result r = p_slGetFeatureRequirements(feature, req);
    if (r != sl::Result::eOk) {
        EFG_LOG("requirements %s: %s", label, ResultName(r));
        return;
    }
    EFG_LOG("requirements %s: flags 0x%x (vulkan %d vsyncOff %d HWS %d), queues compute %u graphics %u opticalFlow %u, "
            "driver %u.%u.%u required %u.%u.%u",
            label, (unsigned)req.flags, !!((unsigned)req.flags & (unsigned)sl::FeatureRequirementFlags::eVulkanSupported),
            !!((unsigned)req.flags & (unsigned)sl::FeatureRequirementFlags::eVSyncOffRequired),
            !!((unsigned)req.flags & (unsigned)sl::FeatureRequirementFlags::eHardwareSchedulingRequired),
            req.vkNumComputeQueuesRequired, req.vkNumGraphicsQueuesRequired, req.vkNumOpticalFlowQueuesRequired,
            req.driverVersionDetected.major, req.driverVersionDetected.minor, req.driverVersionDetected.build,
            req.driverVersionRequired.major, req.driverVersionRequired.minor, req.driverVersionRequired.build);
    for (uint32_t i = 0; i < req.vkNumInstanceExtensions; ++i) EFG_LOG("  instance ext %s", req.vkInstanceExtensions[i]);
    for (uint32_t i = 0; i < req.vkNumDeviceExtensions; ++i) EFG_LOG("  device ext %s", req.vkDeviceExtensions[i]);
    for (uint32_t i = 0; i < req.vkNumFeatures12; ++i) EFG_LOG("  feature12 %s", req.vkFeatures12[i]);
    for (uint32_t i = 0; i < req.vkNumFeatures13; ++i) EFG_LOG("  feature13 %s", req.vkFeatures13[i]);
}

void LogDlssgState(const char* when) {
    if (!p_slDLSSGGetState) return;
    sl::DLSSGState st{};
    sl::DLSSGOptions opts{};
    sl::Result r;
    {
        SlCallScope scope;
        r = p_slDLSSGGetState(sl::ViewportHandle(0), st, &opts);
    }
    EFG_LOG("DLSS-G state (%s): %s, status 0x%x, numFramesToGenerateMax %u, framesPresented %u, minWidthOrHeight %u, "
            "vsyncSupport %d, dynamicMFG %d, VRAM %.1f MB",
            when, ResultName(r), (unsigned)st.status, st.numFramesToGenerateMax, st.numFramesActuallyPresented,
            st.minWidthOrHeight, (int)st.bIsVsyncSupportAvailable, (int)st.bIsDynamicMFGSupported,
            st.estimatedVRAMUsageInBytes / (1024.0 * 1024.0));
}

void LogReflexState(const char* when) {
    if (!p_slReflexGetState) return;
    sl::ReflexState st{};
    sl::Result r = p_slReflexGetState(st);
    EFG_LOG("Reflex state (%s): %s, lowLatencyAvailable %d, latencyReportAvailable %d", when, ResultName(r),
            st.lowLatencyAvailable, st.latencyReportAvailable);
}

void BeginFrame() {
    if (!p_slGetNewFrameToken) return;
    SlCallScope scope;
    sl::FrameToken* token = nullptr;
    if (p_slGetNewFrameToken(token, nullptr) != sl::Result::eOk || !token) return;
    g_frame = token;
    g_simEnded = false;
    if (p_slReflexSleep) p_slReflexSleep(*g_frame);
    if (p_slPCLSetMarker) p_slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g_frame);
}

void Marker(sl::PCLMarker m) {
    if (p_slPCLSetMarker && g_frame) {
        SlCallScope scope;
        p_slPCLSetMarker(m, *g_frame);
    }
}

// Keeps a hard link to the game's own nvngx_dlss.dll next to the SL plugins (same volume), refreshed
// whenever the game's copy changes (game updates replace the file).
void EnsureGameDlssInPluginDir() {
    std::wstring src = GameDir() + L"nvngx_dlss.dll";
    std::wstring dst = g_pluginDir + L"\\nvngx_dlss.dll";
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &a)) {
        EFG_LOG("game nvngx_dlss.dll not found");
        return;
    }
    if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &b) && a.nFileSizeLow == b.nFileSizeLow &&
        a.nFileSizeHigh == b.nFileSizeHigh && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0) {
        return;  // up to date
    }
    DeleteFileW(dst.c_str());
    if (CreateHardLinkW(dst.c_str(), src.c_str(), nullptr)) {
        EFG_LOG("linked game nvngx_dlss.dll into %ls", g_pluginDir.c_str());
    } else if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        EFG_LOG("copied game nvngx_dlss.dll into %ls (hard link failed, error %lu)", g_pluginDir.c_str(), GetLastError());
    } else {
        EFG_LOG("could not place nvngx_dlss.dll in %ls, error %lu", g_pluginDir.c_str(), GetLastError());
    }
}

}  // namespace

bool Initialize() {
    if (g_initialized.load()) return true;
    g_pluginDir = ModDir() + L"streamline";
    std::wstring interposerPath = g_pluginDir + L"\\sl.interposer.dll";
    if (GetFileAttributesW(interposerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        EFG_LOG("Streamline not installed (%ls missing) - running without SL", interposerPath.c_str());
        return false;
    }
    if (!sl::security::verifyEmbeddedSignature(interposerPath.c_str())) {
        EFG_LOG("sl.interposer.dll signature check FAILED - running without SL");
        return false;
    }
    g_interposer = LoadLibraryW(interposerPath.c_str());
    if (!g_interposer) {
        EFG_LOG("LoadLibrary(sl.interposer.dll) failed, error %lu", GetLastError());
        return false;
    }
    RegisterInternalModule(g_interposer, L"sl.interposer.dll");
    bool ok = GetCore(p_slInit, "slInit") && GetCore(p_slShutdown, "slShutdown") &&
              GetCore(p_slIsFeatureSupported, "slIsFeatureSupported") &&
              GetCore(p_slGetFeatureRequirements, "slGetFeatureRequirements") &&
              GetCore(p_slGetFeatureFunction, "slGetFeatureFunction") &&
              GetCore(p_slGetNewFrameToken, "slGetNewFrameToken");
    p_slGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_interposer, "vkGetInstanceProcAddr"));
    p_slGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(g_interposer, "vkGetDeviceProcAddr"));
    if (!ok || !p_slGIPA || !p_slGDPA) return false;

    EnsureGameDlssInPluginDir();

    static const sl::Feature kFeatures[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    // SL also hands these paths to NGX as feature-DLL search paths. NGX is set up by SL before the
    // game initializes its own DLSS-SR, so the game folder (nvngx_dlss.dll) must be on the list.
    static std::wstring gameDir = [] {
        std::wstring d = GameDir();
        if (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        return d;
    }();
    static const wchar_t* pluginPaths[2];
    pluginPaths[0] = g_pluginDir.c_str();
    pluginPaths[1] = gameDir.c_str();
    static std::wstring logDir = ModDir();

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 2;
    pref.pathToLogsAndData = logDir.c_str();
    pref.logMessageCallback = &SlLogCallback;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = kFeatures;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(kFeatures));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "holistic";
    pref.projectId = "5f0c2c55-9e0b-4d3e-9a51-0e6f1d7c2b8e";
    pref.renderAPI = sl::RenderAPI::eVulkan;

    sl::Result r;
    {
        SlCallScope scope;
        r = p_slInit(pref, sl::kSDKVersion);
    }
    EFG_LOG("slInit (SDK %d.%d.%d, manual hooking, Vulkan) -> %s", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH,
            ResultName(r));
    if (r != sl::Result::eOk) return false;

    LogRequirements(sl::kFeatureDLSS_G, "DLSS-G");
    LogRequirements(sl::kFeatureReflex, "Reflex");
    g_initialized.store(true);
    return true;
}

bool Initialized() { return g_initialized.load(); }

PFN_vkCreateInstance CreateInstanceProxy() {
    return g_initialized ? reinterpret_cast<PFN_vkCreateInstance>(p_slGIPA(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
}

PFN_vkCreateDevice CreateDeviceProxy(VkInstance instance) {
    if (!g_initialized || instance != g_instance) return nullptr;
    return reinterpret_cast<PFN_vkCreateDevice>(p_slGIPA(instance, "vkCreateDevice"));
}

PFN_vkVoidFunction InstanceProxy(VkInstance instance, const char* name) {
    if (!g_initialized || instance != g_instance) return nullptr;
    return p_slGIPA(instance, name);
}

void OnInstanceCreated(VkInstance instance) {
    g_instance = instance;
    EFG_LOG("SL instance %p", (void*)instance);
}

void OnDeviceCreated(VkPhysicalDevice physicalDevice, VkDevice device) {
    if (!g_initialized) return;
    SlCallScope scope;
    auto get = [&](const char* n) { return p_slGDPA(device, n); };
    g_proxies.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(get("vkCreateSwapchainKHR"));
    g_proxies.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(get("vkDestroySwapchainKHR"));
    g_proxies.getSwapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(get("vkGetSwapchainImagesKHR"));
    g_proxies.acquireNextImage = reinterpret_cast<PFN_vkAcquireNextImageKHR>(get("vkAcquireNextImageKHR"));
    g_proxies.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(get("vkQueuePresentKHR"));
    g_proxies.deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get("vkDeviceWaitIdle"));
    EFG_LOG("SL device proxies: createSwapchain %p destroySwapchain %p getImages %p acquire %p present %p waitIdle %p",
            (void*)g_proxies.createSwapchain, (void*)g_proxies.destroySwapchain, (void*)g_proxies.getSwapchainImages,
            (void*)g_proxies.acquireNextImage, (void*)g_proxies.queuePresent, (void*)g_proxies.deviceWaitIdle);
    if (!g_proxies.createSwapchain || !g_proxies.destroySwapchain || !g_proxies.getSwapchainImages ||
        !g_proxies.acquireNextImage || !g_proxies.queuePresent || !g_proxies.deviceWaitIdle) {
        EFG_LOG("missing SL device proxies - SL routing disabled");
        return;
    }

    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physicalDevice;
    EFG_LOG("slIsFeatureSupported DLSS-G -> %s, Reflex -> %s, PCL -> %s",
            ResultName(p_slIsFeatureSupported(sl::kFeatureDLSS_G, adapter)),
            ResultName(p_slIsFeatureSupported(sl::kFeatureReflex, adapter)),
            ResultName(p_slIsFeatureSupported(sl::kFeaturePCL, adapter)));

    GetFeatureFn(sl::kFeatureDLSS_G, p_slDLSSGSetOptions, "slDLSSGSetOptions");
    GetFeatureFn(sl::kFeatureDLSS_G, p_slDLSSGGetState, "slDLSSGGetState");
    GetFeatureFn(sl::kFeatureReflex, p_slReflexSetOptions, "slReflexSetOptions");
    GetFeatureFn(sl::kFeatureReflex, p_slReflexSleep, "slReflexSleep");
    GetFeatureFn(sl::kFeatureReflex, p_slReflexGetState, "slReflexGetState");
    GetFeatureFn(sl::kFeaturePCL, p_slPCLSetMarker, "slPCLSetMarker");

    if (p_slReflexSetOptions) {
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        EFG_LOG("slReflexSetOptions(eLowLatency) -> %s", ResultName(p_slReflexSetOptions(ro)));
    }
    if (p_slDLSSGSetOptions) {
        sl::DLSSGOptions o{};
        o.mode = sl::DLSSGMode::eOff;
        EFG_LOG("slDLSSGSetOptions(eOff) -> %s", ResultName(p_slDLSSGSetOptions(sl::ViewportHandle(0), o)));
    }
    g_device.store(device);
}

void OnDeviceDestroying(VkDevice device) {
    if (!g_initialized || device != g_device.load()) return;
    g_device.store(VK_NULL_HANDLE);
    SlCallScope scope;
    EFG_LOG("slShutdown -> %s", ResultName(p_slShutdown()));
    g_initialized.store(false);
}

bool DeviceActive(VkDevice device) { return device != VK_NULL_HANDLE && device == g_device.load(); }
bool QueueActive() { return g_device.load() != VK_NULL_HANDLE; }

const DeviceProxies& Proxies() { return g_proxies; }

void OnAcquire() {
    if (!g_frame) BeginFrame();
    if (g_frame && !g_simEnded) {
        Marker(sl::PCLMarker::eSimulationEnd);
        Marker(sl::PCLMarker::eRenderSubmitStart);
        g_simEnded = true;
    }
}

void BeforePresent() {
    if (!g_frame) BeginFrame();
    if (!g_simEnded) OnAcquire();
    Marker(sl::PCLMarker::eRenderSubmitEnd);
    Marker(sl::PCLMarker::ePresentStart);
}

void AfterPresent(unsigned long long presentCount) {
    Marker(sl::PCLMarker::ePresentEnd);
    if (presentCount == 2 || presentCount % 600 == 0) {
        LogDlssgState(presentCount == 2 ? "first frames" : "periodic");
        if (presentCount == 2 || presentCount % 6000 == 0) LogReflexState("periodic");
    }
    BeginFrame();  // next frame's simulation starts now (Reflex sleep happens here)
}

}  // namespace efg::sl_bridge
