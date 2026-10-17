#include "core/module_ranges.h"

#include <atomic>
#include <cstdint>
#include <cwchar>

#include "core/log.h"

namespace efg {
namespace {

constexpr int kMaxModules = 32;
std::atomic<uintptr_t> g_begin[kMaxModules];
std::atomic<uintptr_t> g_end[kMaxModules];
std::atomic<int> g_count{0};

thread_local int t_slDepth = 0;

}  // namespace

void RegisterInternalModule(HMODULE module, const wchar_t* label) {
    if (!module) return;
    auto base = reinterpret_cast<uintptr_t>(module);
    int n = g_count.load();
    for (int i = 0; i < n; ++i) {
        if (g_begin[i].load() == base) return;
    }
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    int slot = g_count.fetch_add(1);
    if (slot >= kMaxModules) {
        EFG_LOG("internal module table full, cannot register %ls", label);
        return;
    }
    g_end[slot].store(base + nt->OptionalHeader.SizeOfImage);
    g_begin[slot].store(base);
    EFG_LOG("internal module %ls: %p-%p", label, module, (void*)(base + nt->OptionalHeader.SizeOfImage));
}

bool IsInternalAddress(const void* address) {
    auto a = reinterpret_cast<uintptr_t>(address);
    int n = g_count.load();
    if (n > kMaxModules) n = kMaxModules;
    for (int i = 0; i < n; ++i) {
        uintptr_t b = g_begin[i].load();
        if (b && a >= b && a < g_end[i].load()) return true;
    }
    return false;
}

bool IsStreamlineModuleName(const wchar_t* path) {
    if (!path) return false;
    // Driver-delivered (OTA / NVIDIA App override) SL plugins live under ProgramData\NVIDIA\NGX\models\sl_*
    // with hashed file names, and SL prefers them over ours when versions match.
    for (const wchar_t* p = path; *p; ++p) {
        if (_wcsnicmp(p, L"\\NGX\\models\\sl_", 15) == 0) return true;
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    const wchar_t* alt = wcsrchr(path, L'/');
    if (alt > name) name = alt;
    name = name ? name + 1 : path;
    return _wcsnicmp(name, L"sl.", 3) == 0 || _wcsicmp(name, L"nvngx_dlssg.dll") == 0 ||
           _wcsicmp(name, L"NvLowLatencyVk.dll") == 0;
}

SlCallScope::SlCallScope() { ++t_slDepth; }
SlCallScope::~SlCallScope() { --t_slDepth; }
bool InSlCall() { return t_slDepth > 0; }

}  // namespace efg
