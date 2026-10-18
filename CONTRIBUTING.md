# Contributing to EnshroudedFG

Bug reports, fixes and improvements are welcome. This page covers how to report a problem and how to build and test the mod yourself.

## Reporting bugs

Open an issue with the **Bug report** form: [Issues → New issue](https://github.com/lostsanctuary/EnshroudedFG/issues/new/choose). The form asks for:

- **Your setup:** GPU, NVIDIA driver version, the frame generation multiplier you were using, and the EnshroudedFG version (shown in the title bar of the Insert menu).
- **What happened:** what you did, what you expected and what you saw. Screenshots or a short clip help a lot with visual problems.
- **Both log files:** `EnshroudedFG\log.txt` and `EnshroudedFG\sl.log` from the game folder. Drag them into the form.

Copy the logs right after the problem happens, before you start the game again, because `log.txt` starts fresh on every launch. If the problem doesn't show up in the default log, set `VerboseLog=1` under `[Debug]` in `EnshroudedFG.ini`, reproduce it, and attach the longer log.

For ideas and missing options, use the **Feature request** form.

## Build setup

You need:

- **Visual Studio 2022** (or the Build Tools) with the *Desktop development with C++* workload. This includes MSVC, the Windows SDK and MASM.
- **CMake** 3.24 or newer.
- **[Vulkan SDK](https://vulkan.lunarg.com/)**, which provides the Vulkan headers and `glslc` for the compute shaders. The installer sets `VULKAN_SDK`. If CMake can't find Vulkan, open a new terminal or set that variable yourself.
- **Python 3**, which embeds the compiled shaders into C++ headers.
- **An NVIDIA RTX 40 or 50 series GPU** and Enshrouded, to test anything at runtime.

Clone with submodules (MinHook and Dear ImGui):

```powershell
git clone --recursive https://github.com/lostsanctuary/EnshroudedFG.git
cd EnshroudedFG
```

If you already cloned without `--recursive`, run `git submodule update --init`.

## Fetching the NVIDIA files: `tools/fetch_deps.ps1`

Two sets of NVIDIA files are needed but aren't in the repository, because NVIDIA's licences don't allow redistributing them in source form:

| What | Downloaded from | Put in |
|---|---|---|
| NGX SDK headers (needed to build) | [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS), pinned commit `9981c63` | `external/ngx/include/` |
| Streamline 2.14.16 production DLLs (needed to run) | [Streamline release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16) zip, about 265 MB | `external/streamline/bin/x64/` |

Run it once after cloning:

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1
```

- **Production DLLs only:** it extracts only the production DLLs from `bin/x64`, not the watermarked development builds, and checks that each DLL has a valid NVIDIA signature. Streamline refuses to load plugins that aren't signed.
- **`-SkipStreamline`:** fetches only the headers, which is enough to compile.
- **`-Force`:** downloads everything again, for example after changing the pinned versions in the script.

Both folders are in `.gitignore`. Never commit them.

## Building

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The result is `build\Release\winmm.dll`. CMake stops with a clear message if a submodule or the NGX headers are missing. The build should finish with no compiler warnings at `/W4`, so please keep it that way.

`src/proxy/generated/` holds the winmm export stubs, generated from the system `winmm.dll`. You only need to regenerate them if Windows adds or changes winmm exports:

```powershell
python -I tools\gen_winmm_proxy.py
```

## Testing in game

```powershell
powershell -ExecutionPolicy Bypass -File tools\install.ps1     # copy winmm.dll + Streamline DLLs into the game
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1   # remove them again (-Purge also deletes EnshroudedFG\)
```

- **Finding the game:** both scripts find Enshrouded through your Steam libraries. Pass `-GameDir <folder>` if the game is somewhere else.
- **Close the game first:** close Enshrouded before installing, because the DLL is locked while the game runs.
- **Logs:** `EnshroudedFG\log.txt` is the mod's own log, and `EnshroudedFG\sl.log` is Streamline's.
- **`[Debug] FrameTrace=1`:** lets you press F11 in game to write `EnshroudedFG\frame_trace.txt`, the render passes of one frame. It's useful when a game update moves the HUD pass.
- **NVIDIA's DLSS indicator:** set the registry value `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator` (DWORD) to `0x400` to confirm that DLSS-G is active and see its multiplier.

Before opening a pull request, please check at least:

- 2x and your GPU's highest multiplier in the world
- going in and out of the main menu and the map
- alt-tab
- opening the Insert menu while frame generation runs

## Pull requests

- **Scope:** keep each pull request to one change, and describe what you tested and on which GPU.
- **Style:** match the surrounding code (C++20, the existing naming and comment style). New ini keys go in `src/core/config.*` with a default, and are documented in `tools/README.txt` and the README's settings table.
- **Background:** [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) explains why things are done the way they are: Streamline manual hooking, the NGX hooks, HUD-less capture and the camera signatures. Read the relevant part before changing those areas.
- **Licence:** contributions are accepted under the project's [MIT licence](LICENSE).
