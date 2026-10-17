#pragma once

#include <MinHook.h>

#include "core/log.h"

namespace efg {

bool HooksInit();

// Creates and enables an inline hook. `original` receives the trampoline to the original code.
template <class Fn>
bool InstallHook(void* target, Fn detour, Fn* original, const char* name) {
    if (!target) {
        EFG_LOG("hook %s: target not found", name);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original));
    if (st == MH_OK) st = MH_EnableHook(target);
    EFG_LOG("hook %s @ %p: %s", name, target, MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace efg
