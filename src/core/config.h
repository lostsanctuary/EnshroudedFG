#pragma once

namespace efg {

constexpr int kMultiplierAuto = 0;

// Settings from <game>\EnshroudedFG.ini. Missing keys are written with their defaults on start-up, so
// the ini always documents every option.
// What the frame counter shows ([Overlay] ReadoutItems).
enum ReadoutItem : unsigned {
    kReadoutFg = 1u << 0,         // frame generation mode (Auto 3x, Off, ...)
    kReadoutOutput = 1u << 1,     // displayed fps
    kReadoutFps = 1u << 2,        // rendered fps
    kReadoutLatency = 1u << 3,    // Reflex latency
    kReadoutFrameTime = 1u << 4,  // rendered frame time in ms
    kReadoutReflex = 1u << 5,     // Reflex mode
    kReadoutAll = (1u << 6) - 1,
};
constexpr unsigned kReadoutDefault = kReadoutFg | kReadoutOutput | kReadoutFps | kReadoutLatency;

// Frame counter scale per [Overlay] ReadoutSize (small, medium, large).
inline float ReadoutScale(int size) { return size <= 0 ? 0.8f : (size >= 2 ? 1.3f : 1.0f); }

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
    // [Reflex] Mode: 0 = off, 1 = on (low latency), 2 = on + boost. DLSS-G needs Reflex, so while
    // frame generation runs "off" is applied as "on".
    int reflexMode = 1;
    // [Overlay] Readout: small always-on corner readout (multiplier, rendered and output fps).
    bool readout = false;
    // [Overlay] ReadoutX / ReadoutY: top-left of the readout in pixels, dragged there in the menu;
    // -1 = default (top-right corner).
    // [Overlay] ReadoutItems: comma list of fg, output, fps, latency, frametime, reflex.
    unsigned readoutItems = kReadoutDefault;
    // [Overlay] ReadoutSize: small | medium | large (0, 1, 2) - scale of the frame counter.
    int readoutSize = 1;
    int readoutX = -1;
    int readoutY = -1;
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
void SetReflexMode(int mode);  // 0 = off, 1 = on, 2 = on + boost
void SetAutoKeep(float keep);
void SetReadout(bool on);
void SetReadoutItems(unsigned mask);
void SetReadoutSize(int size);  // 0 small, 1 medium, 2 large  // ReadoutItem bits; never empty
void SetReadoutPos(int x, int y);  // -1, -1 = back to the default corner  // saved only when the rounded value changes

}  // namespace efg
