#pragma once

namespace efg {

// Settings from <game>\EnshroudedFG.ini (created with defaults on first run).
struct Config {
    // 1 = frame generation off, 2..6 = DLSS multi frame generation multiplier.
    int multiplier = 2;
    // Vertical field of view used for the approximate camera projection (degrees).
    float fovYDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 10000.0f;
};

const Config& LoadConfig();

}  // namespace efg
