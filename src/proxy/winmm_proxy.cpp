// winmm.dll proxy: loads the real System32 winmm.dll and fills the jump table used by the
// generated MASM stubs (generated/winmm_stubs.asm), so every export forwards unchanged.

#include <windows.h>

#include <cstdint>

#include "generated/winmm_exports.inc"

constexpr size_t kNamedCount = ARRAYSIZE(kWinmmExportNames);

extern "C" {
uintptr_t g_winmm_table[kNamedCount];
uintptr_t g_winmm_ord_table[ARRAYSIZE(kWinmmUnnamedOrdinals)];
}

namespace {

HMODULE g_realWinmm = nullptr;

// Target for any export the system DLL does not provide on this Windows build.
extern "C" uintptr_t MissingWinmmExport() { return 0; }

}  // namespace

namespace efg {

bool LoadRealWinmm() {
    wchar_t path[MAX_PATH]{};
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len + 11 >= MAX_PATH) return false;
    wcscat_s(path, L"\\winmm.dll");
    g_realWinmm = LoadLibraryW(path);

    for (size_t i = 0; i < kNamedCount; ++i) {
        FARPROC p = g_realWinmm ? GetProcAddress(g_realWinmm, kWinmmExportNames[i]) : nullptr;
        g_winmm_table[i] = p ? reinterpret_cast<uintptr_t>(p)
                             : reinterpret_cast<uintptr_t>(&MissingWinmmExport);
    }
    for (unsigned i = 0; i < kWinmmUnnamedCount; ++i) {
        FARPROC p = g_realWinmm ? GetProcAddress(g_realWinmm, MAKEINTRESOURCEA(kWinmmUnnamedOrdinals[i]))
                                : nullptr;
        g_winmm_ord_table[i] = p ? reinterpret_cast<uintptr_t>(p)
                                 : reinterpret_cast<uintptr_t>(&MissingWinmmExport);
    }
    return g_realWinmm != nullptr;
}

size_t RealWinmmExportCount() { return kNamedCount + kWinmmUnnamedCount; }

}  // namespace efg
