#pragma once

#include <windows.h>

namespace efg {

void InstallLoaderHooks();
void InstallVulkanHooks(HMODULE vulkan1);
void InstallNgxHooks(HMODULE nvngx);

// Called by the Vulkan present hook once per presented frame.
void NgxOnPresent(unsigned long long frame);

}  // namespace efg
