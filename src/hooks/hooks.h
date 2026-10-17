#pragma once

#include <windows.h>

namespace efg {

void InstallLoaderHooks();
void InstallVulkanHooks(HMODULE vulkan1);
void InstallNgxHooks(HMODULE nvngx);

// Called by the Vulkan present hook once per presented frame.
void NgxOnPresent(unsigned long long frame);

// Unhooked Vulkan entry points for our own rendering work (never routed through Streamline).
using PFN_RealVoid = void (*)();
PFN_RealVoid RealDeviceProc(void* device, const char* name);
PFN_RealVoid RealInstanceProc(void* instance, const char* name);

}  // namespace efg
