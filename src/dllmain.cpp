#include <windows.h>

#include <string>

#include "core/log.h"
#include "core/module_ranges.h"
#include "hooks/hooks.h"
#include "version.h"

namespace efg {
bool LoadRealWinmm();
size_t RealWinmmExportCount();
}  // namespace efg

namespace {

bool IsEnshroudedHost() {
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"enshrouded.exe") == 0;
}

void LogStartup(bool realWinmmLoaded) {
    EFG_LOG("EnshroudedFG %s loaded (proxy winmm.dll)", EFG_VERSION_STRING);
    EFG_LOG("system winmm.dll %s, %zu exports forwarded", realWinmmLoaded ? "loaded" : "FAILED TO LOAD",
            efg::RealWinmmExportCount());
    EFG_LOG("game dir: %ls", efg::GameDir().c_str());

    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    EFG_LOG("enshrouded.exe base %p, image size 0x%lx, timestamp 0x%08lx", exe, nt->OptionalHeader.SizeOfImage,
            nt->FileHeader.TimeDateStamp);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Forwarding must work before anything else runs, so load the real DLL right away.
        bool realWinmmLoaded = efg::LoadRealWinmm();
        if (IsEnshroudedHost()) {
            efg::LogInit();
            LogStartup(realWinmmLoaded);
            efg::RegisterInternalModule(instance, L"EnshroudedFG (winmm.dll proxy)");
            // Installed synchronously so the game can't load vulkan-1 / _nvngx before we watch for them.
            efg::InstallLoaderHooks();
        }
    }
    return TRUE;
}
