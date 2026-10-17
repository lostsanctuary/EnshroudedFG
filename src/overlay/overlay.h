#pragma once

#include "sl/sl_bridge.h"

#include <windows.h>

namespace efg::overlay {

// In-game ImGui menu (toggle key from the ini, default Insert). It is drawn into the swapchain image
// right before present, on our own command buffer that waits on the game's present semaphores.
// Frame generation is paused while the menu is open, so the menu is never interpolated.

void OnWindow(HWND hwnd);  // from vkCreateWin32SurfaceKHR: subclass the window for input
void OnDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
void OnSwapchainCreated(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info);
void OnSwapchainImages(VkSwapchainKHR swapchain, uint32_t count, const VkImage* images);
void OnSwapchainDestroyed(VkSwapchainKHR swapchain);
void OnDeviceDestroying(VkDevice device);

bool Visible();
HWND Window();  // the game window (null until its surface is created)

// Draws the menu (when visible) into the image being presented and returns the present info to use
// (its wait semaphores replaced by ours). `storage` holds the modified struct.
const VkPresentInfoKHR* BeforePresent(VkQueue queue, const VkPresentInfoKHR* info, VkPresentInfoKHR& storage);

void InstallInputHooks();

}  // namespace efg::overlay
