# EnshroudedFG

[![Build](https://github.com/lostsanctuary/EnshroudedFG/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/lostsanctuary/EnshroudedFG/actions/workflows/build.yml)

NVIDIA DLSS Frame Generation and Multi Frame Generation (2x–6x) for **Enshrouded** (Vulkan), switchable in game.

Enshrouded ships DLSS Super Resolution, DLAA and Reflex, but no frame generation. EnshroudedFG adds it through
NVIDIA Streamline, using the game's own DLSS inputs (depth, motion vectors, jitter), its HUD-less image and its
real camera, so generated frames keep the HUD sharp and stable.

## Features

- **Frame generation:** Off, 2x, 3x, 4x, 5x, 6x, or **Auto**, which picks the multiplier that fills your display's refresh rate.
- **In-game menu (Insert)** styled after Enshrouded's own settings screen. Changes apply live, and frame generation keeps running while the menu is open.
- **NVIDIA Reflex:** On / On + Boost, with measured latency in the menu.
- **Output cap:** an optional Reflex frame limiter just below your refresh rate.
- **Frame counter:** choose what it shows (mode, output fps, rendered fps, latency, frame time, Reflex), plus its size and opacity. Drag it anywhere.
- **Pauses itself in menus and loading screens**, and resumes in the world.
- **Settings** are saved to `EnshroudedFG.ini`.

## Requirements

- NVIDIA GeForce **RTX 40 series** (up to 2x) or **RTX 50 series** (up to 6x).
- NVIDIA driver **595.97** or newer.
- Windows 10 20H1 or newer, with *Hardware-accelerated GPU scheduling* turned on.
- In Enshrouded, set anti-aliasing / upscaling to **DLSS** (any quality, or DLAA). Frame generation needs the game's DLSS inputs, so it does nothing with FSR, XeSS or TAA.
- No other injectors: ReShade's Vulkan layer, OptiScaler and other frame generation mods must be removed, and don't use the `--keep-vulkan-layers` launch option.

## Install

1. Download `EnshroudedFG-<version>.zip` from [Releases](https://github.com/lostsanctuary/EnshroudedFG/releases). [CHANGELOG.md](CHANGELOG.md) lists what changed in each version.
2. Close the game and extract the zip **into the game folder**, so that `winmm.dll` sits next to `enshrouded.exe`. To find the folder, right-click Enshrouded in Steam and choose *Manage → Browse local files*.
3. Start the game, load into your world and press **Insert**.

To uninstall, delete `winmm.dll`, the `EnshroudedFG` folder and `EnshroudedFG.ini` from the game folder.

Logs are written to `EnshroudedFG\log.txt` and `EnshroudedFG\sl.log`. To report a problem, open a [bug report](https://github.com/lostsanctuary/EnshroudedFG/issues/new/choose) and attach both; [CONTRIBUTING.md](CONTRIBUTING.md#reporting-bugs) has the details.

## Settings (`EnshroudedFG.ini`)

| Key | Values |
|---|---|
| `[FrameGen] Multiplier` | `1` (off) … `6`, or `auto` |
| `[FrameGen] OutputCap` | `off`, `auto` (refresh − 3%) or an fps value |
| `[FrameGen] AutoKeep` | `0.85` … `1.0`. Learned by Auto; delete the line to reset it |
| `[Reflex] Mode` | `on`, `boost` or `off`. Off only applies while frame generation is off |
| `[Display] PresentMode` | `game`, `immediate`, `mailbox` or `fifo`. Applies after a restart |
| `[Overlay] ToggleKey` | Virtual-key code. Default `0x2D` (Insert) |
| `[Overlay] Readout` | `0` or `1`. Turns the frame counter off or on |
| `[Overlay] ReadoutItems` | `fg, output, fps, latency, frametime, reflex` |
| `[Overlay] ReadoutSize` | `small`, `medium` or `large` |
| `[Overlay] ReadoutOpacity` | `20` … `100` (percent) |
| `[Camera] Source` | `game`, or `approx` (for comparison only) |

[`tools/README.txt`](tools/README.txt) is the readme shipped in the zip. It explains how Auto and the output cap behave.

## Building

You need:

- Visual Studio 2022 (or the Build Tools) with the *Desktop development with C++* workload.
- CMake 3.24 or newer.
- The [Vulkan SDK](https://vulkan.lunarg.com/), for the headers and `glslc`.
- Python 3.

```powershell
git clone --recursive https://github.com/lostsanctuary/EnshroudedFG.git
cd EnshroudedFG
powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1   # NVIDIA headers + Streamline DLLs
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\install.ps1      # finds the game through Steam
```

- **`fetch_deps.ps1`** downloads the NGX headers from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) at a pinned commit, and the production Streamline 2.14.16 DLLs from the [Streamline release](https://github.com/NVIDIA-RTX/Streamline/releases). NVIDIA's licence doesn't allow redistributing those in source form, so they aren't in this repository.
- **`install.ps1`** and **`uninstall.ps1`** take `-GameDir <folder>` if your game isn't in a Steam library. `uninstall.ps1 -Purge` also removes the `EnshroudedFG` folder.
- **`package.ps1`** builds the release zip in `dist\`.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full build setup, testing in game and pull requests.

## How it works

`winmm.dll` is a proxy for the system DLL that the game loads from its own folder. It forwards every winmm export and then:

1. **Vulkan:** hooks `vulkan-1.dll` and runs instance creation, device creation and the swapchain through Streamline's Vulkan proxies (manual hooking).
2. **DLSS inputs:** hooks the game's NGX DLSS calls in `_nvngx.dll` to pick up depth, motion vectors, jitter and the motion vector scale every frame. These are tagged for DLSS-G.
3. **HUD-less image:** copies the frame just before the HUD is drawn, and reads the camera from game memory (found by byte signatures). Both go to DLSS-G.
4. **Overlay:** draws the ImGui menu and counter onto the final frame before it is presented. DLSS-G treats them as UI, like the game's HUD, so they stay crisp.

[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) is the full development log, phase by phase.

## Notes

- **Game updates:** if a game update moves the camera data, the mod falls back to an approximate camera, and the log says so.
- **Multiplayer:** play solo first; multiplayer use is at your own risk.
- **Not official:** this is an unofficial mod. It isn't affiliated with or endorsed by NVIDIA or Keen Games.

## Contributing

- **Contributing:** bug reports, ideas and pull requests are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) covers the build setup, testing and how to report bugs.
- **Code of Conduct:** everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
- **Security:** please report security problems privately, as described in [SECURITY.md](SECURITY.md), not in public issues.

## Licence

[MIT](LICENSE). Third-party components and NVIDIA runtime licences are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
