#pragma once

namespace efg::camera {

// The game's main camera, read from enshrouded.exe memory (layout from static reverse engineering;
// see PLAN.md Phase 6). Positions are 32.32 fixed point in the game, converted to metres here.
struct CameraSample {
    bool valid = false;
    double position[3] = {};          // metres, world space
    float rotation[4] = {0, 0, 0, 1}; // quaternion x, y, z, w (left-handed, +X right, +Y up, +Z forward)
    bool hasProjection = false;       // fovY / near / far below are the game's values
    float fovY = 0, nearPlane = 0, farPlane = 0;
};

// Finds the camera locations by byte signature (once). Safe to call repeatedly.
bool Init();

// Reads the current camera; `valid` is false outside the world or if any read/sanity check fails.
CameraSample Read();

}  // namespace efg::camera
