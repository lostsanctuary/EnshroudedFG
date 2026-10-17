#pragma once

namespace efg {

constexpr int kMultiplierAuto = 0;

// Settings from <game>\EnshroudedFG.ini. Missing keys are written with their defaults on start-up, so
// the ini always documents every option.
struct Config {
    // [FrameGen] Multiplier: 1 = off, 2..6 = DLSS multi frame generation 2x..6x,
    // kMultiplierAuto ("auto") = pick 2x..max at run time so the output just fills the display.
    int multiplier = 2;
    // [FrameGen] AutoKeep: share of rendered fps Auto expects to keep per extra generated frame
    // (0.85..1.0). Starts at 0.95; Auto refines it from its own steps and saves it here.
    float autoKeep = 0.95f;
    // [FrameGen] OutputCap: limit on displayed fps while frame generation runs (Reflex frame limiter;
    // rendered fps becomes cap / multiplier). 0 = off, -1 = auto (3% below the monitor refresh rate).
    int outputCap = 0;
    // [Display] PresentMode: 0 = as the game requests, otherwise a VkPresentModeKHR + 1
    // (1 = immediate, 2 = mailbox, 3 = fifo / vsync). Applied when the game (re)creates its swapchain.
    int presentModeOverride = 0;
    // [Overlay] ToggleKey: virtual-key code that opens/closes the menu (default: Insert).
    int overlayKey = 0x2D;
    // [Overlay] Readout: small always-on corner readout (multiplier, rendered and output fps).
    bool readout = false;
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

// True when frame generation is wanted at all (a fixed 2x..6x or Auto).
inline bool FrameGenEnabled(const Config& c) { return c.multiplier != 1; }

// Runtime changes from the overlay; each is written to the ini immediately.
void SetMultiplier(int multiplier);  // 1..6 or kMultiplierAuto
void SetUseGameCamera(bool use);
void SetOutputCap(int cap);  // 0 = off, -1 = auto, otherwise fps
void SetAutoKeep(float keep);
void SetReadout(bool on);  // saved only when the rounded value changes

}  // namespace efg
