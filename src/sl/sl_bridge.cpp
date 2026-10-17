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

#include "camera/camera_provider.h"
#include "core/config.h"
#include "core/log.h"
#include "core/module_ranges.h"
#include "debug/frame_trace.h"
#include "overlay/overlay.h"
#include "hooks/hooks.h"
#include "render/copy_pass.h"
#include "render/copy_rgba8_spv.h"
#include "render/mv_convert_spv.h"

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
render::CopyPass g_mvCopy;       // R16G16_SNORM motion vectors -> RG16F
render::CopyPass g_hudlessCopy;  // swapchain image before the HUD pass -> RGBA8 HUD-less colour
bool g_copyPassesTried = false;
PFN_vkCmdPipelineBarrier p_CmdPipelineBarrier = nullptr;

// Game-facing swapchain images and the game's views of them (to recognise the HUD pass).
std::mutex g_swapchainMutex;
std::unordered_map<VkImage, uint32_t> g_swapchainImages;
std::unordered_map<VkImageView, VkImage> g_swapchainViews;
bool g_hudlessTagged = false;  // this frame's HUD-less colour has been captured and tagged

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
uint32_t g_frameLimitUs = 0;     // Reflex frame limit currently applied (0 = none)
uint32_t g_refreshHz = 0;        // refresh rate of the monitor showing the game
double g_outputCapFps = 0.0;     // effective output cap while frame generation runs
LARGE_INTEGER g_lastPresentQpc{};
SrInputs g_lastSr{};
constexpr int kStreakToEnable = 3;  // world frames required before turning FG on (avoids menu flicker)

// Auto multiplier: a hand-rolled dynamic MFG (Streamline's Dynamic MFG is D3D12-only).
uint32_t g_autoMultiplier = 2;     // multiplier Auto currently runs at (kept across menus/loading)
double g_autoKeep = 0;             // share of rendered fps kept per extra generated frame (0 = from ini)
double g_autoLastEval = 0;         // time of the last evaluation in s (0 = FG not running under Auto)
uint32_t g_autoWindowFrames = 0;   // frames presented since then (rendered fps = frames / elapsed)
uint32_t g_autoFromMultiplier = 0; // multiplier before the last change, to measure its cost (0 = none)
double g_autoFromFps = 0;          // rendered fps before the last change
uint32_t g_autoCeiling = 0;        // after a step down, the multipliers above stay blocked ...
double g_autoCeilingUntil = 0;     // ... until this time
constexpr double kAutoIntervalS = 2.0;
constexpr double kAutoHoldS = 10.0;

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

// ---- game camera (Phase 6) -----------------------------------------------------------------

struct CameraState {
    bool valid = false;
    double position[3] = {};
    sl::float3 right, up, fwd;
    sl::float4x4 viewToClip;
};
CameraState g_prevCamera;
double g_cameraOrigin[3] = {};  // keeps SL's float camera position small
bool g_cameraOriginSet = false;
int g_cameraLogBudget = 3;
// Per-summary-interval camera statistics (logged with the DLSS-G state).
unsigned g_camGameFrames = 0, g_camApproxFrames = 0, g_camResets = 0;
double g_camMaxStep = 0;
sl::float3 g_camLastFwd;
double g_camLastPos[3] = {};

sl::float3 Rotate(const float q[4], float vx, float vy, float vz) {
    // v' = v + 2w(q x v) + 2 q x (q x v)
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float tx = 2.0f * (qy * vz - qz * vy), ty = 2.0f * (qz * vx - qx * vz), tz = 2.0f * (qx * vy - qy * vx);
    return sl::float3(vx + qw * tx + (qy * tz - qz * ty), vy + qw * ty + (qz * tx - qx * tz),
                      vz + qw * tz + (qx * ty - qy * tx));
}

sl::float4x4 Projection(float fovY, float aspect, float n, float f, bool reverseZ) {
    const float ys = 1.0f / std::tan(fovY * 0.5f);
    const float xs = ys / aspect;
    sl::float4x4 p;
    p[0] = sl::float4(xs, 0, 0, 0);
    p[1] = sl::float4(0, ys, 0, 0);
    if (reverseZ) {
        p[2] = sl::float4(0, 0, -n / (f - n), 1);
        p[3] = sl::float4(0, 0, n * f / (f - n), 0);
    } else {
        p[2] = sl::float4(0, 0, f / (f - n), 1);
        p[3] = sl::float4(0, 0, -n * f / (f - n), 0);
    }
    return p;
}

// Camera-to-world with the translation given relative to the current camera (precision).
sl::float4x4 CameraToCentredWorld(const sl::float3& r, const sl::float3& u, const sl::float3& f, float tx, float ty,
                                  float tz) {
    sl::float4x4 m;
    m[0] = sl::float4(r.x, r.y, r.z, 0);
    m[1] = sl::float4(u.x, u.y, u.z, 0);
    m[2] = sl::float4(f.x, f.y, f.z, 0);
    m[3] = sl::float4(tx, ty, tz, 1);
    return m;
}

// Fills the camera part of the constants from the game's camera. Returns false (caller falls back to
// the approximation) when the camera cannot be read.
bool FillGameCamera(sl::Constants& c, const SrInputs& in, bool& forceReset) {
    const camera::CameraSample cam = camera::Read();
    if (!cam.valid) {
        g_prevCamera.valid = false;
        return false;
    }
    const Config& cfg = LoadConfig();
    const float aspect = in.outputH ? float(in.outputW) / float(in.outputH) : 16.0f / 9.0f;
    const float fovY = cam.hasProjection ? cam.fovY : cfg.fovYDegrees * 3.14159265f / 180.0f;
    const float n = cam.hasProjection ? cam.nearPlane : cfg.nearPlane;
    const float f = cam.hasProjection ? cam.farPlane : cfg.farPlane;

    CameraState cur;
    cur.valid = true;
    for (int i = 0; i < 3; ++i) cur.position[i] = cam.position[i];
    cur.right = Rotate(cam.rotation, 1, 0, 0);
    cur.up = Rotate(cam.rotation, 0, 1, 0);
    cur.fwd = Rotate(cam.rotation, 0, 0, 1);
    cur.viewToClip = Projection(fovY, aspect, n, f, in.depthInverted);

    if (!g_cameraOriginSet) {
        for (int i = 0; i < 3; ++i) g_cameraOrigin[i] = cam.position[i];
        g_cameraOriginSet = true;
    }

    c.cameraViewToClip = cur.viewToClip;
    sl::matrixFullInvert(c.clipToCameraView, cur.viewToClip);
    c.clipToLensClip = Identity();
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(float(cam.position[0] - g_cameraOrigin[0]), float(cam.position[1] - g_cameraOrigin[1]),
                             float(cam.position[2] - g_cameraOrigin[2]));
    c.cameraRight = cur.right;
    c.cameraUp = cur.up;
    c.cameraFwd = cur.fwd;
    c.cameraNear = n;
    c.cameraFar = f;
    c.cameraFOV = fovY;
    c.cameraAspectRatio = aspect;

    // clipToPrevClip = clip -> view -> (current camera -> previous camera) -> previous clip.
    double dist = 0;
    if (g_prevCamera.valid) {
        const float dx = float(g_prevCamera.position[0] - cur.position[0]);
        const float dy = float(g_prevCamera.position[1] - cur.position[1]);
        const float dz = float(g_prevCamera.position[2] - cur.position[2]);
        dist = std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
        sl::float4x4 camToWorld = CameraToCentredWorld(cur.right, cur.up, cur.fwd, 0, 0, 0);
        sl::float4x4 prevCamToWorld = CameraToCentredWorld(g_prevCamera.right, g_prevCamera.up, g_prevCamera.fwd, dx, dy, dz);
        sl::float4x4 worldToPrevCam, camToPrevCam, clipToPrevCam;
        sl::matrixOrthoNormalInvert(worldToPrevCam, prevCamToWorld);
        sl::matrixMul(camToPrevCam, camToWorld, worldToPrevCam);
        sl::matrixMul(clipToPrevCam, c.clipToCameraView, camToPrevCam);
        sl::matrixMul(c.clipToPrevClip, clipToPrevCam, g_prevCamera.viewToClip);
        sl::matrixFullInvert(c.prevClipToClip, c.clipToPrevClip);
    } else {
        c.clipToPrevClip = Identity();
        c.prevClipToClip = Identity();
        forceReset = true;
    }
    if (dist > 25.0) forceReset = true;  // teleport / respawn / cut
    if (forceReset) ++g_camResets;
    if (dist > g_camMaxStep) g_camMaxStep = dist;
    g_camLastFwd = cur.fwd;
    for (int i = 0; i < 3; ++i) g_camLastPos[i] = cam.position[i];

    if (g_cameraLogBudget > 0) {
        --g_cameraLogBudget;
        EFG_LOG("game camera: pos (%.2f, %.2f, %.2f) m fwd (%.3f, %.3f, %.3f) fovY %.1f deg near %.3f far %.0f%s, moved %.3f m",
                cam.position[0], cam.position[1], cam.position[2], cur.fwd.x, cur.fwd.y, cur.fwd.z,
                fovY * 180.0f / 3.14159265f, n, f, cam.hasProjection ? "" : " (projection from ini)", dist);
    }
    g_prevCamera = cur;
    return true;
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

void EnsureCopyPasses() {
    if (g_copyPassesTried) return;
    g_copyPassesTried = true;
    VkDevice device = g_device.load();
    p_CmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(RealDeviceProc(device, "vkCmdPipelineBarrier"));
    if (!g_mvCopy.Init("mvec", g_instance, g_physicalDevice, device, VK_FORMAT_R16G16_SFLOAT, kMvConvertSpv,
                       sizeof(kMvConvertSpv)))
        EFG_LOG("motion-vector copy pass unavailable - tagging the game's SNORM motion vectors directly");
    if (!g_hudlessCopy.Init("hudless", g_instance, g_physicalDevice, device, VK_FORMAT_R8G8B8A8_UNORM, kCopyRgba8Spv,
                            sizeof(kCopyRgba8Spv)))
        EFG_LOG("HUD-less copy pass unavailable - frame generation will run without HUD-less colour");
}

void TagNull() {
    if (!p_slSetTagForFrame || !g_frame) return;
    sl::ResourceTag tags[] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
    };
    SlCallScope scope;
    p_slSetTagForFrame(*g_frame, sl::ViewportHandle(0), tags, 3, nullptr);
}

uint32_t QueryRefreshRate() {
    HWND hwnd = overlay::Window();
    HMONITOR mon = MonitorFromWindow(hwnd ? hwnd : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!GetMonitorInfoW(mon, &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
}

// Output cap through the Reflex frame limiter. While DLSS-G runs, the driver applies frameLimitUs to
// presented frames (measured in-game: a 272 fps limit gave ~272 fps output and 272/N rendered fps), so
// the limit is simply the output interval. Off by default: it lowers rendered fps at high multipliers.
void ApplyFrameLimit() {
    if (!p_slReflexSetOptions) return;
    const Config& cfg = LoadConfig();
    double cap = 0;
    if (g_fgOn && cfg.outputCap != 0) {
        if (cfg.outputCap > 0) cap = cfg.outputCap;
        else if (g_refreshHz) cap = g_refreshHz * 0.97;
    }
    g_outputCapFps = cap;
    uint32_t limitUs = cap > 0 ? static_cast<uint32_t>(1e6 / cap) : 0;
    if (limitUs == g_frameLimitUs) return;
    sl::ReflexOptions ro{};
    ro.mode = sl::ReflexMode::eLowLatency;
    ro.frameLimitUs = limitUs;
    sl::Result r;
    {
        SlCallScope scope;
        r = p_slReflexSetOptions(ro);
    }
    EFG_LOG("output cap: %s -> Reflex frame limit %u us (%.0f fps, refresh %u Hz, %ux) -> %s", cap > 0 ? "on" : "off",
            limitUs, limitUs ? 1e6 / limitUs : 0.0, g_refreshHz, g_fgFrames + 1, ResultName(r));
    if (r == sl::Result::eOk) g_frameLimitUs = limitUs;
}

uint32_t MaxMultiplier() { return g_framesToGenerateMax ? g_framesToGenerateMax + 1 : 2; }

// The multiplier to run: the configured one, or Auto's current choice.
uint32_t WantedMultiplier() {
    const int m = LoadConfig().multiplier;
    return m == kMultiplierAuto ? g_autoMultiplier : static_cast<uint32_t>(m);
}

// Auto: every 2 s while frame generation runs, pick the multiplier N in [2, max] whose output
// (rendered fps averaged over those 2 s, so a brief dip cannot trigger a step, x N) just fills the
// display (target = refresh x 0.97). The output at another multiplier M is predicted as
// rendered x M x keep^(M-N), where keep is the share of rendered fps that survives one more generated
// frame ([FrameGen] AutoKeep: 0.95 at first, refined from Auto's own steps and saved back). Down when
// the output is more than 5% above the target; up only while the higher multiplier is predicted to
// stay below it. After a step down, the multipliers above stay blocked for 10 s so it cannot flip-flop.
void UpdateAutoMultiplier(double now) {
    if (LoadConfig().multiplier != kMultiplierAuto || !g_fgOn || !g_refreshHz || g_baseFps <= 1) {
        g_autoLastEval = 0;
        g_autoFromMultiplier = 0;
        return;
    }
    const uint32_t maxM = MaxMultiplier();
    g_autoMultiplier = std::clamp<uint32_t>(g_autoMultiplier, 2, maxM);
    const uint32_t n = g_fgFrames + 1;  // as applied to DLSS-G
    const bool first = g_autoLastEval == 0;
    if (first || n != g_autoMultiplier) {
        // Just started (or a change is still being applied): measure a full interval first.
        if (first) EFG_LOG("auto: frame generation running at %ux, evaluating every %.0f s", n, kAutoIntervalS);
        g_autoLastEval = now;
        g_autoWindowFrames = 0;
        return;
    }
    ++g_autoWindowFrames;
    if (now - g_autoLastEval < kAutoIntervalS) return;
    const double r = g_autoWindowFrames / (now - g_autoLastEval);
    if (g_autoKeep <= 0) g_autoKeep = LoadConfig().autoKeep;
    g_autoLastEval = now;
    g_autoWindowFrames = 0;
    const double target = g_refreshHz * 0.97;
    if (g_autoFromMultiplier && g_autoFromMultiplier != n && g_autoFromFps > 1) {
        const double observed = std::pow(r / g_autoFromFps, 1.0 / (double(n) - double(g_autoFromMultiplier)));
        g_autoKeep = std::clamp(0.5 * g_autoKeep + 0.5 * observed, 0.85, 1.0);
        SetAutoKeep(static_cast<float>(g_autoKeep));
    }
    g_autoFromMultiplier = 0;

    auto predicted = [&](uint32_t m) { return r * m * std::pow(g_autoKeep, double(m) - double(n)); };
    uint32_t next = n;
    const char* why = "stays";
    if (r * n > target * 1.05 && n > 2) {
        next = 2;
        for (uint32_t m = n - 1; m > 2; --m) {
            if (predicted(m) <= target) {
                next = m;
                break;
            }
        }
        g_autoCeiling = n - 1;
        g_autoCeilingUntil = now + kAutoHoldS;
        why = "output too high";
    } else {
        const uint32_t ceiling = now < g_autoCeilingUntil ? std::min(g_autoCeiling, maxM) : maxM;
        while (next < ceiling && predicted(next + 1) < target) ++next;
        if (next != n) why = "room for more";
    }
    if (next != n || LoadConfig().verboseLog) {
        EFG_LOG("auto: rendered %.1f fps x%u = %.0f fps output, target %.0f (%u Hz), keep %.2f/step -> %ux (%s)", r,
                n, r * n, target, g_refreshHz, g_autoKeep, next, why);
    }
    if (next != n) {
        g_autoFromMultiplier = n;
        g_autoFromFps = r;
        g_autoMultiplier = next;
    }
}

// Applies DLSS-G on/off and the wanted multiplier; only calls into SL when either changes, so the
// overlay (and Auto) can switch 2x..6x live.
void SetFrameGeneration(bool on) {
    if (!p_slDLSSGSetOptions) return;
    uint32_t wanted = WantedMultiplier() - 1;
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
    camera::Init();  // signature scan once, at load time
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
    g_device.store(VK_NULL_HANDLE);
    {
        // Shut SL down first: it may still hold this frame's tags, which point at our copy images.
        SlCallScope scope;
        EFG_LOG("slShutdown -> %s", ResultName(p_slShutdown()));
    }
    g_initialized.store(false);
    g_mvCopy.Shutdown();
    g_hudlessCopy.Shutdown();
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
    EnsureCopyPasses();
    ImageRef mvecImage = in.mvec;
    VkImageLayout mvecLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (in.mvec.format == VK_FORMAT_R16G16_SNORM && g_mvCopy.Ready()) {
        ImageRef converted = g_mvCopy.Copy(in.cmd, in.mvec.view, in.mvec.width, in.mvec.height,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    bool forceReset = false;
    const bool gameCamera = LoadConfig().useGameCamera && FillGameCamera(c, in, forceReset);
    if (!gameCamera) FillApproximateCamera(c, in);
    ++(gameCamera ? g_camGameFrames : g_camApproxFrames);
    c.jitterOffset = sl::float2(in.jitterX, in.jitterY);
    // NGX MV scale converts the stored vectors to pixels; SL wants them normalized to [-1,1].
    c.mvecScale = sl::float2(in.mvScaleX / float(in.renderW), in.mvScaleY / float(in.renderH));
    c.depthInverted = B(in.depthInverted);
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = B(in.reset || forceReset);
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
    const bool wantFg = FrameGenEnabled(LoadConfig()) && g_frameHasSr && streak >= kStreakToEnable &&
                        !overlay::Visible();
    if (!g_frameHasSr) TagNull();
    SetFrameGeneration(wantFg);
    ApplyFrameLimit();

    Marker(sl::PCLMarker::eRenderSubmitEnd);
    Marker(sl::PCLMarker::ePresentStart);
}

void AfterPresent(unsigned long long presentCount) {
    Marker(sl::PCLMarker::ePresentEnd);
    g_srStreak = g_frameHasSr ? g_srStreak + 1 : 0;
    g_frameHasSr = false;
    g_hudlessTagged = false;
    if (g_srStreak == 900) frame_trace::Arm();  // one automatic trace ~a few seconds into the world

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_lastPresentQpc.QuadPart) {
        double dt = double(now.QuadPart - g_lastPresentQpc.QuadPart) / double(freq.QuadPart);
        if (dt > 0) g_baseFps = g_baseFps > 0 ? g_baseFps * 0.95 + (1.0 / dt) * 0.05 : 1.0 / dt;
    }
    g_lastPresentQpc = now;
    if (presentCount == 2 || presentCount % 600 == 0) g_refreshHz = QueryRefreshRate();
    UpdateAutoMultiplier(double(now.QuadPart) / double(freq.QuadPart));
    static LARGE_INTEGER lastCapLog{};
    if (g_frameLimitUs && LoadConfig().verboseLog &&
        double(now.QuadPart - lastCapLog.QuadPart) / double(freq.QuadPart) > 2.0) {
        lastCapLog = now;
        EFG_LOG("output cap check: rendered %.1f fps at %ux -> ~%.0f fps output (cap %.0f)", g_baseFps, g_fgFrames + 1,
                g_baseFps * (g_fgFrames + 1), g_outputCapFps);
    }
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
        if (g_camGameFrames || g_camApproxFrames) {
            EFG_LOG("camera: %u frames game, %u approx, %u forced resets, max step %.2f m, last pos (%.1f, %.1f, %.1f) "
                    "fwd (%.3f, %.3f, %.3f)",
                    g_camGameFrames, g_camApproxFrames, g_camResets, g_camMaxStep, g_camLastPos[0], g_camLastPos[1],
                    g_camLastPos[2], g_camLastFwd.x, g_camLastFwd.y, g_camLastFwd.z);
            g_camGameFrames = g_camApproxFrames = g_camResets = 0;
            g_camMaxStep = 0;
        }
        if (presentCount == 2 || presentCount % 6000 == 0) LogReflexState("periodic");
    }
    BeginFrame();  // next frame's simulation starts now (Reflex sleep happens here)
}

void OnSwapchainImages(uint32_t count, const VkImage* images) {
    if (!images) return;
    std::lock_guard lock(g_swapchainMutex);
    g_swapchainImages.clear();
    g_swapchainViews.clear();
    for (uint32_t i = 0; i < count; ++i) g_swapchainImages[images[i]] = i;
}

void OnImageViewCreated(VkImageView view, VkImage image) {
    std::lock_guard lock(g_swapchainMutex);
    if (g_swapchainImages.count(image)) g_swapchainViews[view] = image;
}

// The game composites its HUD in the first render pass that loads the swapchain image after DLSS-SR
// (see PLAN.md, Phase 6 frame trace). Right before that pass, copy the image as HUD-less colour.
void OnBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri) {
    if (!g_frameHasSr || g_hudlessTagged || !ri || ri->colorAttachmentCount < 1 || !ri->pColorAttachments) return;
    if (!FrameGenEnabled(LoadConfig()) || !g_frame || !p_slSetTagForFrame) return;
    const VkRenderingAttachmentInfo& a = ri->pColorAttachments[0];
    if (a.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || !a.imageView) return;
    VkImage image;
    {
        std::lock_guard lock(g_swapchainMutex);
        auto it = g_swapchainViews.find(a.imageView);
        if (it == g_swapchainViews.end()) return;
        image = it->second;
    }
    g_hudlessTagged = true;  // one attempt per frame
    if (!g_hudlessCopy.Ready() || !p_CmdPipelineBarrier) return;
    const uint32_t w = ri->renderArea.extent.width, h = ri->renderArea.extent.height;

    // The image is in the pass's attachment layout here; make it readable for the copy and back.
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = a.imageLayout;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    ImageRef hudless = g_hudlessCopy.Copy(cmd, a.imageView, w, h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = a.imageLayout;
    p_CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    if (!hudless.image) return;

    sl::Resource res = MakeResource(hudless, VK_IMAGE_LAYOUT_GENERAL);
    res.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sl::Extent extent{0, 0, w, h};
    sl::ResourceTag tag(&res, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent);
    sl::Result r;
    {
        SlCallScope scope;
        r = p_slSetTagForFrame(*g_frame, sl::ViewportHandle(0), &tag, 1, reinterpret_cast<sl::CommandBuffer*>(cmd));
    }
    static int logBudget = 2;
    if (logBudget > 0 || r != sl::Result::eOk) {
        if (logBudget > 0) --logBudget;
        EFG_LOG("frame %u: HUD-less colour captured from swapchain image (%ux%u, layout %d) -> %s", (uint32_t)*g_frame,
                w, h, a.imageLayout, ResultName(r));
    }
}

FgStatus GetFgStatus() {
    FgStatus s;
    s.slActive = g_initialized.load() && g_device.load() != VK_NULL_HANDLE;
    s.fgOn = g_fgOn;
    s.worldFrames = g_srStreak >= kStreakToEnable;
    s.maxMultiplier = g_framesToGenerateMax ? g_framesToGenerateMax + 1 : 0;
    s.multiplier = FrameGenEnabled(LoadConfig()) ? std::min(WantedMultiplier(), MaxMultiplier()) : 1;
    s.autoMultiplier = std::min(g_autoMultiplier, MaxMultiplier());
    s.framesPresented = g_framesPresented;
    s.status = g_dlssgStatus;
    s.baseFps = g_baseFps;
    s.refreshHz = g_refreshHz;
    s.outputCapFps = g_outputCapFps;
    return s;
}

}  // namespace efg::sl_bridge
