// NGX interception: the game's statically linked NGX SDK calls these _nvngx.dll exports.
// CreateFeature tells us DLSS SR's flags and sizes; EvaluateFeature gives us the per-frame
// depth / motion vectors / jitter, which are handed to Streamline for frame generation.

#include "sl/sl_bridge.h"  // Vulkan headers (VK_NO_PROTOTYPES)

#include <nvsdk_ngx_vk.h>

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "core/module_ranges.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"

namespace efg {
namespace {

using CreateFeature_t = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                      NVSDK_NGX_Handle**);
using CreateFeature1_t = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature,
                                                       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeature_t = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeature_t = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// The Init* exports differ in arity (Init_ProjectID_Ext takes 11 arguments). A pass-through detour
// that declares MORE arguments than any variant is safe on x64 - extra slots are forwarded unread -
// but declaring fewer drops real arguments, so keep a margin. Only argument 1 is interpreted, and only
// for the appId variants.
using InitAny_t = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
                                                void*, void*, void*, void*);
InitAny_t g_origInit[5] = {};

CreateFeature_t g_origCreateFeature = nullptr;
CreateFeature1_t g_origCreateFeature1 = nullptr;
EvaluateFeature_t g_origEvaluateFeature = nullptr;
ReleaseFeature_t g_origReleaseFeature = nullptr;

struct FeatureInfo {
    NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved_Unknown;
    int createFlags = 0;
    unsigned renderW = 0, renderH = 0, outW = 0, outH = 0;
    int perfQuality = -1;
};

// Snapshot of one DLSS SR evaluation.
struct SrInputs {
    VkImage color = VK_NULL_HANDLE, output = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, mvec = VK_NULL_HANDLE;
    VkFormat colorFmt = VK_FORMAT_UNDEFINED, outputFmt = VK_FORMAT_UNDEFINED, depthFmt = VK_FORMAT_UNDEFINED,
             mvecFmt = VK_FORMAT_UNDEFINED;
    VkImageView depthView = VK_NULL_HANDLE, mvecView = VK_NULL_HANDLE;
    unsigned colorW = 0, colorH = 0, outputW = 0, outputH = 0, depthW = 0, depthH = 0, mvecW = 0, mvecH = 0;
    float jitterX = 0, jitterY = 0, mvScaleX = 0, mvScaleY = 0, preExposure = 0, frameTimeMs = 0;
    int reset = 0;
    unsigned subrectW = 0, subrectH = 0;
    bool hasExposureTex = false;
};

std::mutex g_mutex;
// Keyed by handle address: handle ids are only unique per NGX context, and Streamline's DLSS-G runs in
// its own context next to the game's (both start numbering at 1).
std::unordered_map<const NVSDK_NGX_Handle*, FeatureInfo> g_features;
SrInputs g_lastInputs;

// Per-present-interval statistics.
unsigned g_evalsThisFrame = 0;
unsigned g_framesWithEval = 0, g_framesWithoutEval = 0, g_multiEvalFrames = 0, g_resets = 0;
unsigned g_depthImageChanges = 0, g_mvecImageChanges = 0;
unsigned long long g_totalEvals = 0;
int g_detailBudget = 3;
LARGE_INTEGER g_intervalStart{};
unsigned long long g_intervalStartFrame = 0;

constexpr unsigned long long kSummaryInterval = 600;

const char* FeatureName(NVSDK_NGX_Feature f) {
    switch (f) {
        case NVSDK_NGX_Feature_SuperSampling: return "DLSS-SR";
        case NVSDK_NGX_Feature_FrameGeneration: return "DLSS-G";
        case NVSDK_NGX_Feature_RayReconstruction: return "DLSS-RR";
        default: return "other";
    }
}

unsigned HandleId(const NVSDK_NGX_Handle* h) { return h ? *reinterpret_cast<const unsigned*>(h) : ~0u; }

void ReadImage(const NVSDK_NGX_Parameter* p, const char* name, VkImage& img, VkFormat& fmt, unsigned& w, unsigned& h,
               VkImageView* view = nullptr) {
    void* v = nullptr;
    if (NVSDK_NGX_FAILED(p->Get(name, &v)) || !v) return;
    auto* res = static_cast<const NVSDK_NGX_Resource_VK*>(v);
    if (res->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return;
    img = res->Resource.ImageViewInfo.Image;
    fmt = res->Resource.ImageViewInfo.Format;
    w = res->Resource.ImageViewInfo.Width;
    h = res->Resource.ImageViewInfo.Height;
    if (view) *view = res->Resource.ImageViewInfo.ImageView;
}

FeatureInfo ReadCreateParams(NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* p) {
    FeatureInfo fi;
    fi.feature = feature;
    if (!p) return fi;
    p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &fi.createFlags);
    p->Get(NVSDK_NGX_Parameter_Width, &fi.renderW);
    p->Get(NVSDK_NGX_Parameter_Height, &fi.renderH);
    p->Get(NVSDK_NGX_Parameter_OutWidth, &fi.outW);
    p->Get(NVSDK_NGX_Parameter_OutHeight, &fi.outH);
    p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &fi.perfQuality);
    return fi;
}

void RecordFeature(const char* api, NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* p,
                   NVSDK_NGX_Result r, NVSDK_NGX_Handle* h) {
    FeatureInfo fi = ReadCreateParams(feature, p);
    const int f = fi.createFlags;
    EFG_LOG("NGX %s %s (id %d) -> 0x%x handle %u: render %ux%u -> out %ux%u, perfQuality %d, flags 0x%x"
            " [HDR %d MVLowRes %d MVJittered %d DepthInverted %d AutoExposure %d]",
            api, FeatureName(feature), feature, r, HandleId(h), fi.renderW, fi.renderH, fi.outW, fi.outH,
            fi.perfQuality, f, !!(f & NVSDK_NGX_DLSS_Feature_Flags_IsHDR),
            !!(f & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes), !!(f & NVSDK_NGX_DLSS_Feature_Flags_MVJittered),
            !!(f & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted), !!(f & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
    if (NVSDK_NGX_SUCCEED(r) && h) {
        std::lock_guard lock(g_mutex);
        g_features[h] = fi;
    }
}

// ---- detours --------------------------------------------------------------------------------

template <int N>
NVSDK_NGX_Result NVSDK_CONV HookInit(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
                                     void* a9, void* a10, void* a11, void* a12, void* a13, void* a14) {
    static const char* kNames[] = {"Init", "Init_Ext", "Init_Ext2", "Init_ProjectID", "Init_ProjectID_Ext"};
    NVSDK_NGX_Result r = g_origInit[N](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    EFG_LOG("NGX VULKAN_%s(caller %p) -> 0x%x", kNames[N], _ReturnAddress(), r);
    if constexpr (N < 3) {
        // (appId, dataPath, instance, physicalDevice, device, ...)
        EFG_LOG("  appId %llu dataPath '%ls' device %p", (unsigned long long)a1, a2 ? (const wchar_t*)a2 : L"", a5);
    } else {
        // (projectId, engineType, engineVersion, dataPath, instance, physicalDevice, device, ...)
        EFG_LOG("  projectId '%s' engine %d dataPath '%ls' device %p", a1 ? (const char*)a1 : "", (int)(uintptr_t)a2,
                a4 ? (const wchar_t*)a4 : L"", a7);
    }
    return r;
}

NVSDK_NGX_Result NVSDK_CONV HookCreateFeature(VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                              NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    NVSDK_NGX_Result r = g_origCreateFeature(cmd, feature, p, out);
    if (!IsInternalAddress(_ReturnAddress())) RecordFeature("CreateFeature", feature, p, r, out ? *out : nullptr);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV HookCreateFeature1(VkDevice device, VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                               NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    NVSDK_NGX_Result r = g_origCreateFeature1(device, cmd, feature, p, out);
    if (!IsInternalAddress(_ReturnAddress())) RecordFeature("CreateFeature1", feature, p, r, out ? *out : nullptr);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV HookReleaseFeature(NVSDK_NGX_Handle* h) {
    if (!IsInternalAddress(_ReturnAddress())) {
        EFG_LOG("NGX ReleaseFeature handle %u (%p)", HandleId(h), (void*)h);
        std::lock_guard lock(g_mutex);
        g_features.erase(h);
    }
    return g_origReleaseFeature(h);
}

NVSDK_NGX_Result NVSDK_CONV HookEvaluateFeature(VkCommandBuffer cmd, const NVSDK_NGX_Handle* h,
                                                const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback cb) {
    if (IsInternalAddress(_ReturnAddress())) return g_origEvaluateFeature(cmd, h, p, cb);  // SL's DLSS-G
    bool isSr = false;
    FeatureInfo fi;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_features.find(h);
        isSr = it != g_features.end() && it->second.feature == NVSDK_NGX_Feature_SuperSampling;
        if (isSr) fi = it->second;
    }
    if (isSr && p) {
        SrInputs in;
        ReadImage(p, NVSDK_NGX_Parameter_Color, in.color, in.colorFmt, in.colorW, in.colorH);
        ReadImage(p, NVSDK_NGX_Parameter_Output, in.output, in.outputFmt, in.outputW, in.outputH);
        ReadImage(p, NVSDK_NGX_Parameter_Depth, in.depth, in.depthFmt, in.depthW, in.depthH, &in.depthView);
        ReadImage(p, NVSDK_NGX_Parameter_MotionVectors, in.mvec, in.mvecFmt, in.mvecW, in.mvecH, &in.mvecView);
        p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &in.jitterX);
        p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &in.jitterY);
        p->Get(NVSDK_NGX_Parameter_MV_Scale_X, &in.mvScaleX);
        p->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &in.mvScaleY);
        p->Get(NVSDK_NGX_Parameter_Reset, &in.reset);
        p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &in.subrectW);
        p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &in.subrectH);
        p->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &in.preExposure);
        p->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &in.frameTimeMs);
        void* exposure = nullptr;
        in.hasExposureTex = NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposure)) && exposure;

        std::lock_guard lock(g_mutex);
        ++g_evalsThisFrame;
        ++g_totalEvals;
        if (in.reset) ++g_resets;
        if (g_totalEvals > 1 && in.depth != g_lastInputs.depth) ++g_depthImageChanges;
        if (g_totalEvals > 1 && in.mvec != g_lastInputs.mvec) ++g_mvecImageChanges;
        bool sizeChanged = in.colorW != g_lastInputs.colorW || in.colorH != g_lastInputs.colorH ||
                           in.outputW != g_lastInputs.outputW || in.outputH != g_lastInputs.outputH;
        if (g_detailBudget > 0 || sizeChanged) {
            if (g_detailBudget > 0) --g_detailBudget;
            EFG_LOG("DLSS-SR eval #%llu handle %u cmd %p%s", g_totalEvals, HandleId(h), (void*)cmd,
                    sizeChanged ? " (size changed)" : "");
            EFG_LOG("  color  %p fmt %d %ux%u | output %p fmt %d %ux%u", (void*)in.color, in.colorFmt, in.colorW,
                    in.colorH, (void*)in.output, in.outputFmt, in.outputW, in.outputH);
            EFG_LOG("  depth  %p fmt %d %ux%u | mvec   %p fmt %d %ux%u", (void*)in.depth, in.depthFmt, in.depthW,
                    in.depthH, (void*)in.mvec, in.mvecFmt, in.mvecW, in.mvecH);
            EFG_LOG("  jitter (%.4f, %.4f) mvScale (%.3f, %.3f) reset %d subrect %ux%u preExposure %.3f "
                    "frameTime %.2fms exposureTex %d",
                    in.jitterX, in.jitterY, in.mvScaleX, in.mvScaleY, in.reset, in.subrectW, in.subrectH,
                    in.preExposure, in.frameTimeMs, in.hasExposureTex);
        }
        g_lastInputs = in;
    }
    if (isSr && p) {
        const SrInputs& in = g_lastInputs;  // single render thread: safe to read back outside the lock
        sl_bridge::SrInputs fg;
        fg.cmd = cmd;
        fg.depth = {in.depth, in.depthView, in.depthFmt, in.depthW, in.depthH};
        fg.mvec = {in.mvec, in.mvecView, in.mvecFmt, in.mvecW, in.mvecH};
        fg.jitterX = in.jitterX;
        fg.jitterY = in.jitterY;
        fg.mvScaleX = in.mvScaleX;
        fg.mvScaleY = in.mvScaleY;
        fg.renderW = in.subrectW ? in.subrectW : in.colorW;
        fg.renderH = in.subrectH ? in.subrectH : in.colorH;
        fg.outputW = in.outputW;
        fg.outputH = in.outputH;
        fg.reset = in.reset != 0;
        fg.depthInverted = (fi.createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
        fg.mvJittered = (fi.createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
        sl_bridge::OnSrEvaluate(fg);
    }
    return g_origEvaluateFeature(cmd, h, p, cb);
}

}  // namespace

void NgxOnPresent(unsigned long long frame) {
    std::lock_guard lock(g_mutex);
    if (g_evalsThisFrame == 0) ++g_framesWithoutEval;
    else ++g_framesWithEval;
    if (g_evalsThisFrame > 1) ++g_multiEvalFrames;
    g_evalsThisFrame = 0;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_intervalStartFrame == 0) {
        g_intervalStart = now;
        g_intervalStartFrame = frame;
        return;
    }
    if (frame - g_intervalStartFrame < kSummaryInterval) return;
    double secs = double(now.QuadPart - g_intervalStart.QuadPart) / double(freq.QuadPart);
    EFG_LOG("summary frames %llu-%llu: %.1f fps | with SR eval %u, without %u, multi-eval %u | resets %u | "
            "depth image changes %u, mvec image changes %u | last jitter (%.3f, %.3f)",
            g_intervalStartFrame, frame, double(frame - g_intervalStartFrame) / secs, g_framesWithEval,
            g_framesWithoutEval, g_multiEvalFrames, g_resets, g_depthImageChanges, g_mvecImageChanges,
            g_lastInputs.jitterX, g_lastInputs.jitterY);
    g_framesWithEval = g_framesWithoutEval = g_multiEvalFrames = g_resets = 0;
    g_depthImageChanges = g_mvecImageChanges = 0;
    g_intervalStart = now;
    g_intervalStartFrame = frame;
}

void InstallNgxHooks(HMODULE nvngx) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nvngx, path, MAX_PATH);
    EFG_LOG("_nvngx.dll loaded: %ls", path);
    auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(nvngx, n)); };
    InstallHook(get("NVSDK_NGX_VULKAN_Init"), &HookInit<0>, &g_origInit[0], "NVSDK_NGX_VULKAN_Init");
    InstallHook(get("NVSDK_NGX_VULKAN_Init_Ext"), &HookInit<1>, &g_origInit[1], "NVSDK_NGX_VULKAN_Init_Ext");
    InstallHook(get("NVSDK_NGX_VULKAN_Init_Ext2"), &HookInit<2>, &g_origInit[2], "NVSDK_NGX_VULKAN_Init_Ext2");
    InstallHook(get("NVSDK_NGX_VULKAN_Init_ProjectID"), &HookInit<3>, &g_origInit[3], "NVSDK_NGX_VULKAN_Init_ProjectID");
    InstallHook(get("NVSDK_NGX_VULKAN_Init_ProjectID_Ext"), &HookInit<4>, &g_origInit[4],
                "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
    InstallHook(get("NVSDK_NGX_VULKAN_CreateFeature"), &HookCreateFeature, &g_origCreateFeature,
                "NVSDK_NGX_VULKAN_CreateFeature");
    InstallHook(get("NVSDK_NGX_VULKAN_CreateFeature1"), &HookCreateFeature1, &g_origCreateFeature1,
                "NVSDK_NGX_VULKAN_CreateFeature1");
    InstallHook(get("NVSDK_NGX_VULKAN_EvaluateFeature"), &HookEvaluateFeature, &g_origEvaluateFeature,
                "NVSDK_NGX_VULKAN_EvaluateFeature");
    InstallHook(get("NVSDK_NGX_VULKAN_ReleaseFeature"), &HookReleaseFeature, &g_origReleaseFeature,
                "NVSDK_NGX_VULKAN_ReleaseFeature");
}

}  // namespace efg
