#pragma once

#include "sl/sl_bridge.h"

namespace efg::frame_trace {

// Opt-in diagnostic ([Debug] FrameTrace=1 in EnshroudedFG.ini): records one frame's command stream
// (render passes and their attachments, barriers, copies, draw/dispatch counts, DLSS-SR evaluate,
// submits and present) to EnshroudedFG\frame_trace.txt. Used to find where the HUD is drawn.
// Costs nothing when disabled: no Vulkan functions are wrapped.

bool Enabled();

// Wraps traced device functions at vkGetDeviceProcAddr time (only when enabled).
PFN_vkVoidFunction Intercept(const char* name, PFN_vkVoidFunction real);

void Arm();  // trace the next full frame (hotkey F11, or automatically once in the world)
void OnSwapchainImages(uint32_t count, const VkImage* images);
void OnSrEvaluate(VkCommandBuffer cmd, VkImage color, VkImage output, VkImage depth, VkImage mvec);
void OnPresent(VkQueue queue, const VkPresentInfoKHR* info);  // frame boundary

}  // namespace efg::frame_trace
