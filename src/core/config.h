#pragma once

namespace efg {

// Settings from <game>\EnshroudedFG.ini (created with defaults on first run).
struct Config {
    // 1 = frame generation off, 2..6 = DLSS multi frame generation multiplier.
    int multiplier = 2;
    // Virtual-key code that opens/closes the overlay (default: Insert).
    int overlayKey = 0x2D;
    // Use the game's camera (position/rotation/FOV read from memory); false = approximate camera.
    bool useGameCamera = true;
    // Vertical field of view used for the approximate camera projection (degrees).
    float fovYDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 10000.0f;
};

const Config& LoadConfig();

// Changes the multiplier at runtime (clamped to 1..6) and writes it to the ini.
void SetMultiplier(int multiplier);

// Switches between the game's camera and the approximation at runtime and writes it to the ini.
void SetUseGameCamera(bool use);

}  // namespace efg
