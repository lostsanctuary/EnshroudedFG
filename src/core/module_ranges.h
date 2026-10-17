#pragma once

#include <windows.h>

namespace efg {

// Address ranges of modules whose Vulkan calls must bypass our routing: our own DLL and the
// Streamline / DLSS-G modules. A detour that sees one of these as its caller calls the real
// function directly, which keeps SL's internal calls from looping back into SL.
void RegisterInternalModule(HMODULE module, const wchar_t* label);
bool IsInternalAddress(const void* address);

// True for module file names that belong to Streamline / DLSS-G.
bool IsStreamlineModuleName(const wchar_t* path);

// Per-thread nesting depth of our own calls into Streamline.
struct SlCallScope {
    SlCallScope();
    ~SlCallScope();
    SlCallScope(const SlCallScope&) = delete;
    SlCallScope& operator=(const SlCallScope&) = delete;
};
bool InSlCall();

}  // namespace efg
