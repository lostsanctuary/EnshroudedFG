// Watches DLL loads so the Vulkan and NGX hooks go in as soon as their modules appear.
// Every LoadLibrary* variant ends up in KernelBase!LoadLibraryExW.

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "core/config.h"
#include "core/module_ranges.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "overlay/overlay.h"

namespace efg {
namespace {

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExW_t g_origLoadLibraryExW = nullptr;

// kernel32!GetProcAddress calls KernelBase!GetProcAddressForCaller, so that is the one to watch.
using GetProcAddressForCaller_t = FARPROC(WINAPI*)(HMODULE, LPCSTR, void*);
GetProcAddressForCaller_t g_origGetProcAddressForCaller = nullptr;

std::atomic<bool> g_vulkanHooked{false};
std::atomic<bool> g_ngxHooked{false};

// Diagnostics: which entry points the game resolves by name from the modules we care about.
std::atomic<HMODULE> g_watchVulkan{nullptr}, g_watchNgx{nullptr}, g_watchNvapi{nullptr}, g_watchNvll{nullptr};
std::atomic<int> g_procLogBudget{400};

// The game probes Vulkan with a throwaway instance and then frees vulkan-1.dll; the next load maps a
// fresh, unpatched image. Pinning keeps the hooked image (and its hooks) alive for the process lifetime.
bool Pin(const wchar_t* name) {
    HMODULE pinned = nullptr;
    BOOL ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &pinned);
    EFG_LOG("pin %ls: %s", name, ok ? "ok" : "FAILED");
    return ok != FALSE;
}

void CheckModules() {
    if (!g_watchNvapi.load()) g_watchNvapi.store(GetModuleHandleW(L"nvapi64.dll"));
    if (!g_watchNvll.load()) g_watchNvll.store(GetModuleHandleW(L"NvLowLatencyVk.dll"));
    if (!g_vulkanHooked.load()) {
        if (HMODULE m = GetModuleHandleW(L"vulkan-1.dll"); m && !g_vulkanHooked.exchange(true)) {
            Pin(L"vulkan-1.dll");
            g_watchVulkan.store(m);
            InstallVulkanHooks(m);
        }
    }
    if (!g_ngxHooked.load()) {
        if (HMODULE m = GetModuleHandleW(L"_nvngx.dll"); m && !g_ngxHooked.exchange(true)) {
            Pin(L"_nvngx.dll");
            g_watchNgx.store(m);
            InstallNgxHooks(m);
        }
    }
}

HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE result = g_origLoadLibraryExW(name, file, flags);
    if (result && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                             LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(result, path, MAX_PATH) && IsStreamlineModuleName(path)) {
            RegisterInternalModule(result, path);
        }
        CheckModules();
    }
    return result;
}

FARPROC WINAPI DetourGetProcAddressForCaller(HMODULE module, LPCSTR name, void* callerAddr) {
    FARPROC result = g_origGetProcAddressForCaller(module, name, callerAddr);
    if (module && (module == g_watchVulkan.load() || module == g_watchNgx.load() || module == g_watchNvapi.load() ||
                   module == g_watchNvll.load())) {
        if (g_procLogBudget.fetch_sub(1) > 0) {
            const char* which = module == g_watchVulkan.load() ? "vulkan-1"
                                : module == g_watchNgx.load()  ? "_nvngx"
                                : module == g_watchNvapi.load() ? "nvapi64"
                                                                : "NvLowLatencyVk";
            void* caller = callerAddr;
            if (IS_INTRESOURCE(name)) {
                EFG_LOG("GetProcAddress(%s, #%u) -> %p (caller %p)", which, (unsigned)(uintptr_t)name, (void*)result, caller);
            } else {
                EFG_LOG("GetProcAddress(%s, %s) -> %p (caller %p)", which, name, (void*)result, caller);
            }
        }
    }
    return result;
}

}  // namespace

bool HooksInit() {
    static bool ok = MH_Initialize() == MH_OK;
    return ok;
}

void InstallLoaderHooks() {
    if (!HooksInit()) {
        EFG_LOG("MH_Initialize failed - no hooks installed");
        return;
    }
    void* target = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "LoadLibraryExW");
    InstallHook(target, &DetourLoadLibraryExW, &g_origLoadLibraryExW, "KernelBase!LoadLibraryExW");
    if (LoadConfig().verboseLog) {  // diagnostics only: which entry points get resolved by name
        void* gpa = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetProcAddressForCaller");
        InstallHook(gpa, &DetourGetProcAddressForCaller, &g_origGetProcAddressForCaller,
                    "KernelBase!GetProcAddressForCaller");
    }
    overlay::InstallInputHooks();
    CheckModules();  // in case either module was loaded before us
}

}  // namespace efg
