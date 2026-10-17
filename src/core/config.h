#pragma once

namespace efg {

// Settings from <game>\EnshroudedFG.ini. Missing keys are written with their defaults on start-up, so
// the ini always documents every option.
struct Config {
    // [FrameGen] Multiplier: 1 = off, 2..6 = DLSS multi frame generation 2x..6x.
    int multiplier = 2;
    // [FrameGen] OutputCap: limit on displayed fps while frame generation runs (Reflex frame limiter;
    // rendered fps becomes cap / multiplier). 0 = off, -1 = auto (3% below the monitor refresh rate).
    int outputCap = 0;
    // [Display] PresentMode: 0 = as the game requests, otherwise a VkPresentModeKHR + 1
    // (1 = immediate, 2 = mailbox, 3 = fifo / vsync). Applied when the game (re)creates its swapchain.
    int presentModeOverride = 0;
    // [Overlay] ToggleKey: virtual-key code that opens/closes the menu (default: Insert).
    int overlayKey = 0x2D;
    // [Camera] Source: game = the game's camera read from memory; approx = approximate camera.
    bool useGameCamera = true;
    // [Camera] fallback projection for the approximate camera.
    float fovYDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 10000.0f;
    // [Debug] VerboseLog: log every Vulkan/NGX entry-point lookup (diagnostics).
    bool verboseLog = false;
};

const Config& LoadConfig();

// Runtime changes from the overlay; each is written to the ini immediately.
void SetMultiplier(int multiplier);
void SetUseGameCamera(bool use);
void SetOutputCap(int cap);  // 0 = off, -1 = auto, otherwise fps

}  // namespace efg
