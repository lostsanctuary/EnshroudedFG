// Streamline integration (manual hooking). SL's own vkCreateInstance / vkCreateDevice proxies add
// the extensions, features and queues DLSS-G and Reflex need, so we route the game's real
// instance and device through them and skip slSetVulkanInfo. Phase 3: DLSS-G stays off.

#include "sl/sl_bridge.h"

#include <windows.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/config.h"
#include "core/log.h"
#include "core/module_ranges.h"
#include "overlay/overlay.h"
#include "render/mv_convert.h"

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
PFun_slSetTagForFrame* p_slSetTagForFrame = nullptr;
PFun_slSetConstants* p_slSetConstants = nullptr;
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
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
render::MvConverter g_mvConverter;
bool g_mvConverterTried = false;

// Current frame (single render thread in this game).
sl::FrameToken* g_frame = nullptr;
bool g_simEnded = false;

// Frame generation state.
bool g_frameHasSr = false;   // this frame had a DLSS-SR evaluate (3D world frame) and is tagged
int g_srStreak = 0;          // consecutive presented frames with SR inputs
bool g_fgOn = false;         // DLSS-G mode currently set to eOn
uint32_t g_fgFrames = 0;     // numFramesToGenerate currently applied
uint32_t g_framesToGenerateMax = 0;
uint32_t g_framesPresented = 0;  // last DLSS-G numFramesActuallyPresented
uint32_t g_dlssgStatus = 0;
double g_baseFps = 0.0;          // EMA of rendered frames per second
LARGE_INTEGER g_lastPresentQpc{};
SrInputs g_lastSr{};
constexpr int kStreakToEnable = 3;  // world frames required before turning FG on (avoids menu flicker)

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

// SL warnings/errors are mirrored into our log. Identical messages (ignoring SL's timestamp prefix)
// are logged a few times and then only counted, so a per-frame complaint cannot flood the log.
void SlLogCallback(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo) return;  // full detail goes to sl.log
    std::string m(msg ? msg : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    // "[hh-mm-ss][streamline][warn][tid:N][Ns:Nms:Nus]file.cpp:line[func] text" -> key from "file.cpp:..."
    size_t keyStart = m.find("]", m.find("us]") == std::string::npos ? 0 : m.find("us]"));
    std::string key = keyStart == std::string::npos ? m : m.substr(keyStart + 1);

    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned> counts;
    unsigned n;
    {
        std::lock_guard lock(mutex);
        n = ++counts[key];
    }
    constexpr unsigned kShown = 3;
    if (n <= kShown) {
        EFG_LOG("[SL %s] %s%s", type == sl::LogType::eError ? "error" : "warn", m.c_str(),
                n == kShown ? "  (further repeats counted)" : "");
    } else if (n % 10000 == 0) {
        EFG_LOG("[SL] message repeated %u times: %s", n, key.c_str());
    }
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
    if (r == sl::Result::eOk && st.numFramesToGenerateMax) g_framesToGenerateMax = st.numFramesToGenerateMax;
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

sl::Boolean B(bool v) { return v ? sl::Boolean::eTrue : sl::Boolean::eFalse; }

sl::float4x4 Identity() {
    sl::float4x4 m;
    m[0] = sl::float4(1, 0, 0, 0);
    m[1] = sl::float4(0, 1, 0, 0);
    m[2] = sl::float4(0, 0, 1, 0);
    m[3] = sl::float4(0, 0, 0, 1);
    return m;
}

// Approximate camera: the game's matrices are not known yet (Phase 4 step A). Row-vector,
// left-handed perspective with reverse-Z (near -> 1, far -> 0), matching the game's inverted depth.
// Motion vectors already contain camera motion, so the clip-to-previous-clip transforms are identity.
void FillApproximateCamera(sl::Constants& c, const SrInputs& in) {
    const Config& cfg = LoadConfig();
    const float aspect = in.outputH ? float(in.outputW) / float(in.outputH) : 16.0f / 9.0f;
    const float fovY = cfg.fovYDegrees * 3.14159265f / 180.0f;
    const float ys = 1.0f / std::tan(fovY * 0.5f);
    const float xs = ys / aspect;
    const float n = cfg.nearPlane, f = cfg.farPlane;
    sl::float4x4 proj;
    if (in.depthInverted) {
        proj[0] = sl::float4(xs, 0, 0, 0);
        proj[1] = sl::float4(0, ys, 0, 0);
        proj[2] = sl::float4(0, 0, -n / (f - n), 1);
        proj[3] = sl::float4(0, 0, n * f / (f - n), 0);
    } else {
        proj[0] = sl::float4(xs, 0, 0, 0);
        proj[1] = sl::float4(0, ys, 0, 0);
        proj[2] = sl::float4(0, 0, f / (f - n), 1);
        proj[3] = sl::float4(0, 0, -n * f / (f - n), 0);
    }
    c.cameraViewToClip = proj;
    sl::matrixFullInvert(c.clipToCameraView, proj);
    c.clipToLensClip = Identity();
    c.clipToPrevClip = Identity();
    c.prevClipToClip = Identity();
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0);
    c.cameraUp = sl::float3(0, 1, 0);
    c.cameraRight = sl::float3(1, 0, 0);
    c.cameraFwd = sl::float3(0, 0, 1);
    c.cameraNear = n;
    c.cameraFar = f;
    c.cameraFOV = fovY;
    c.cameraAspectRatio = aspect;
}

sl::Resource MakeResource(const ImageRef& img, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    sl::Resource r(sl::ResourceType::eTex2d, (void*)img.image, nullptr, (void*)img.view, layout);
    r.width = img.width;
    r.height = img.height;
    r.nativeFormat = img.format;
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return r;
}

void TagNull() {
    if (!p_slSetTagForFrame || !g_frame) return;
    sl::ResourceTag tags[] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
    };
    SlCallScope scope;
    p_slSetTagForFrame(*g_frame, sl::ViewportHandle(0), tags, 2, nullptr);
}

// Applies DLSS-G on/off and the configured multiplier; only calls into SL when either changes, so the
// overlay can switch 2x..6x live.
void SetFrameGeneration(bool on) {
    if (!p_slDLSSGSetOptions) return;
    const Config& cfg = LoadConfig();
    uint32_t wanted = static_cast<uint32_t>(cfg.multiplier - 1);
    uint32_t frames = std::max<uint32_t>(1, std::min(wanted, g_framesToGenerateMax ? g_framesToGenerateMax : 1));
    if (on == g_fgOn && (!on || frames == g_fgFrames)) return;
    sl::DLSSGOptions o{};
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = frames;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    o.mvecDepthWidth = g_lastSr.renderW;
    o.mvecDepthHeight = g_lastSr.renderH;
    sl::Result r;
    {
        SlCallScope scope;
        r = p_slDLSSGSetOptions(sl::ViewportHandle(0), o);
    }
    EFG_LOG("frame generation %s (%ux, numFramesToGenerate %u) -> %s", on ? "ON" : "OFF", o.numFramesToGenerate + 1,
            o.numFramesToGenerate, ResultName(r));
    if (r == sl::Result::eOk) {
        g_fgOn = on;
        g_fgFrames = frames;
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
              GetCore(p_slGetNewFrameToken, "slGetNewFrameToken") &&
              GetCore(p_slSetTagForFrame, "slSetTagForFrame") && GetCore(p_slSetConstants, "slSetConstants");
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
    g_physicalDevice = physicalDevice;
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
    SetFrameGeneration(false);
    g_mvConverter.Shutdown();
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

void OnSrEvaluate(const SrInputs& in) {
    if (!g_initialized || !g_device.load() || !p_slSetTagForFrame || !p_slSetConstants) return;
    if (!g_frame) BeginFrame();
    if (!g_frame || g_frameHasSr) return;  // one set of inputs per frame
    if (!in.depth.image || !in.mvec.image || !in.renderW || !in.renderH) return;

    // Depth and motion vectors are the game's own persistent render-resolution images; they are not
    // rewritten before this frame's present, so SL may read them in place at present time.
    // Streamline cannot describe R16G16_SNORM, so those motion vectors are first copied into RG16F.
    ImageRef mvecImage = in.mvec;
    VkImageLayout mvecLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (render::MvConverter::NeedsConversion(in.mvec.format)) {
        if (!g_mvConverterTried) {
            g_mvConverterTried = true;
            if (!g_mvConverter.Init(g_instance, g_physicalDevice, g_device.load())) {
                EFG_LOG("motion-vector converter unavailable - tagging the game's SNORM motion vectors directly");
            }
        }
        ImageRef converted = g_mvConverter.Convert(in.cmd, in.mvec);
        if (converted.image) {
            mvecImage = converted;
            mvecLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
    sl::Resource depth = MakeResource(in.depth);
    sl::Resource mvec = MakeResource(mvecImage, mvecLayout);
    mvec.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sl::Extent extent{0, 0, in.renderW, in.renderH};
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent),
    };

    sl::Constants c{};
    FillApproximateCamera(c, in);
    c.jitterOffset = sl::float2(in.jitterX, in.jitterY);
    // NGX MV scale converts the stored vectors to pixels; SL wants them normalized to [-1,1].
    c.mvecScale = sl::float2(in.mvScaleX / float(in.renderW), in.mvScaleY / float(in.renderH));
    c.depthInverted = B(in.depthInverted);
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = B(in.reset);
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = B(in.mvJittered);

    sl::Result rt, rc;
    {
        SlCallScope scope;
        rt = p_slSetTagForFrame(*g_frame, sl::ViewportHandle(0), tags, 2, reinterpret_cast<sl::CommandBuffer*>(in.cmd));
        rc = p_slSetConstants(c, *g_frame, sl::ViewportHandle(0));
    }
    static int logBudget = 3;
    if (logBudget > 0 || rt != sl::Result::eOk || rc != sl::Result::eOk) {
        if (logBudget > 0) --logBudget;
        EFG_LOG("frame %u: tagged depth %p (fmt %d) + mvec %p (fmt %d) %ux%u -> %s; constants jitter (%.3f, %.3f) "
                "mvecScale (%.3f, %.3f) depthInverted %d reset %d -> %s",
                (uint32_t)*g_frame, (void*)in.depth.image, in.depth.format, (void*)mvecImage.image, mvecImage.format,
                in.renderW, in.renderH, ResultName(rt), in.jitterX, in.jitterY, c.mvecScale.x, c.mvecScale.y,
                in.depthInverted, in.reset, ResultName(rc));
    }
    g_lastSr = in;
    g_frameHasSr = (rt == sl::Result::eOk && rc == sl::Result::eOk);
}

void BeforePresent() {
    if (!g_frame) BeginFrame();
    if (!g_simEnded) OnAcquire();

    // Frame generation only while the 3D world renders (frames with DLSS-SR inputs); menus,
    // loading screens etc. have no evaluate, so FG is switched off and the tags are cleared.
    // Also paused while our overlay menu is open, so the menu is never interpolated.
    const int streak = g_frameHasSr ? g_srStreak + 1 : 0;
    const bool wantFg = LoadConfig().multiplier >= 2 && g_frameHasSr && streak >= kStreakToEnable &&
                        !overlay::Visible();
    if (!g_frameHasSr) TagNull();
    SetFrameGeneration(wantFg);

    Marker(sl::PCLMarker::eRenderSubmitEnd);
    Marker(sl::PCLMarker::ePresentStart);
}

void AfterPresent(unsigned long long presentCount) {
    Marker(sl::PCLMarker::ePresentEnd);
    g_srStreak = g_frameHasSr ? g_srStreak + 1 : 0;
    g_frameHasSr = false;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_lastPresentQpc.QuadPart) {
        double dt = double(now.QuadPart - g_lastPresentQpc.QuadPart) / double(freq.QuadPart);
        if (dt > 0) g_baseFps = g_baseFps > 0 ? g_baseFps * 0.95 + (1.0 / dt) * 0.05 : 1.0 / dt;
    }
    g_lastPresentQpc = now;
    if (overlay::Visible() && presentCount % 30 == 0 && p_slDLSSGGetState) {
        sl::DLSSGState st{};
        SlCallScope scope;
        if (p_slDLSSGGetState(sl::ViewportHandle(0), st, nullptr) == sl::Result::eOk) {
            g_framesPresented = st.numFramesActuallyPresented;
            g_dlssgStatus = static_cast<uint32_t>(st.status);
        }
    }

    if (presentCount == 2 || presentCount % 600 == 0) {
        LogDlssgState(presentCount == 2 ? "first frames" : (g_fgOn ? "periodic, FG on" : "periodic, FG off"));
        if (presentCount == 2 || presentCount % 6000 == 0) LogReflexState("periodic");
    }
    BeginFrame();  // next frame's simulation starts now (Reflex sleep happens here)
}

FgStatus GetFgStatus() {
    FgStatus s;
    s.slActive = g_initialized.load() && g_device.load() != VK_NULL_HANDLE;
    s.fgOn = g_fgOn;
    s.worldFrames = g_srStreak >= kStreakToEnable;
    s.maxMultiplier = g_framesToGenerateMax ? g_framesToGenerateMax + 1 : 0;
    s.framesPresented = g_framesPresented;
    s.status = g_dlssgStatus;
    s.baseFps = g_baseFps;
    return s;
}

}  // namespace efg::sl_bridge
