# Plan: Inject DLSS Multi Frame Generation (Off / 2x–6x) into Enshrouded

## Context

Enshrouded 1.0 (Vulkan only, Keen's Holistic engine) has DLSS SR/DLAA and Reflex but no frame generation. ComputerBase's 1.0 test (16 Oct 2026) confirms MFG is not supported. The user has an RTX 5090 and wants real NVIDIA DLSS-G with a selectable multiplier from 2x to 6x, switched from an in-game ImGui overlay.

What I found on this machine:
- **How the game calls DLSS:** `enshrouded.exe` statically links the NGX SDK and calls `NVSDK_NGX_VULKAN_*` directly. It never references Streamline, and the crash-dump module list shows no `sl.*` DLL loaded. That NGX call is our input tap: depth, motion vectors, jitter, MV scale, reset and render size.
- **Leftover Streamline files:** the install folder has Streamline DLLs that report version **2.13** (dated 27 Aug 2026), plus `nvngx_dlssg.dll` 310.8. All are NVIDIA-signed but the game doesn't use them. NVIDIA never published a 2.13 release (2.12 is followed by 2.14.x), so these are almost certainly from a mod.
- **Old ReShade addon:** the crash dump also shows `renodx-dlss5.addon64` loaded. It isn't in the folder any more, and the user agreed to disable ReShade.
- **Queues:** family 0 is graphics/compute/present ×16, family 2 is compute ×8. The device exposes `VK_NV_low_latency2`, `VK_NV_optical_flow` and `present_id/wait`.
- **Present mode:** FIFO when vsync is on.
- **Build tools:** no Visual Studio, CMake, Ninja or Vulkan SDK installed. The project folder `F:\Enshrouded FrameGen` is empty.

**An existing mod already claims this.** "Enshrouded DLSS Frame Generation" by Biozilla (Nexus mod 141, v1.1.x, Oct 2026) claims real DLSS-G 2x–6x on Vulkan using the game's DLSS inputs. It's closed source, and one RTX 5090 user reports 4x–6x feeling laggy. Phase 0 checks it first, both as a baseline and so we know whether a custom build is worth the effort.

## Approach

Build a **proxy `winmm.dll`** in C++ that loads from the game folder. The exe imports WINMM, and the OptiScaler wiki confirms winmm proxying works for Enshrouded. The proxy forwards the real winmm exports and does four jobs:

1. **Vulkan:** hooks `vulkan-1.dll` with MinHook (`vkCreateInstance`, `vkCreateDevice`, `vkGet*ProcAddr`) and integrates Streamline 2.14.16 in **manual-hooking** mode. The swapchain and present calls are routed through SL's proxies.
2. **NGX:** hooks `NVSDK_NGX_VULKAN_CreateFeature/EvaluateFeature` in `_nvngx.dll` to capture the DLSS SR inputs for each frame.
3. **Reflex:** translates the game's own `VK_NV_low_latency2` calls into `slReflex*` and `slPCLSetMarker` calls. DLSS-G refuses to run with a non-SL Reflex.
4. **Overlay:** draws an ImGui overlay into a UI buffer tagged for DLSS-G, so the menu isn't warped by generated frames.

**Why a proxy DLL and not a Vulkan layer:** SL's proxies call back into `vulkan-1.dll` exports. Inside a layer those calls re-enter the layer chain, and a proxy avoids that.

### Repository layout (`F:\Enshrouded FrameGen`)
```
CMakeLists.txt
external/   streamline-2.14.16 (headers + production signed DLLs), minhook, imgui, ngx headers (NVIDIA DLSS SDK), vulkan headers
src/proxy/      winmm exports forwarding (.def + loader for System32\winmm.dll)
src/hooks/      vulkan_hooks.cpp, ngx_hooks.cpp, latency_hooks.cpp, wndproc_hooks.cpp
src/sl/         sl_bridge.cpp (slInit, VulkanInfo, tags, constants, DLSSG options/state)
src/camera/     camera_provider.cpp (approximate → reverse-engineered matrices)
src/overlay/    overlay.cpp (ImGui Vulkan+Win32, UI buffer), config.cpp (EnshroudedFG.ini)
tools/install.ps1, uninstall.ps1
```
Runtime files: `winmm.dll` and `EnshroudedFG.ini` go next to the exe. The SL DLLs and a log go in `EnshroudedFG\`, which gets loaded with a full absolute path because SL checks signatures. The game's own `nvngx_dlssg.dll` is used, or a newer one.

## Phases

### Phase 0: Baseline and cleanup
- **Clean install:** in Steam, run "Verify integrity of game files", then back up and remove the stray `sl.*`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll` and license files if Steam doesn't own them.
- **ReShade:** disable the ReShade Vulkan layer and RenoDX, and drop `--keep-vulkan-layers` from the launch options.
- **System:** confirm driver ≥ 595.97, hardware-accelerated GPU scheduling on, and Windows ≥ 19041.
- **Optional test of Nexus 141:** record base and output FPS at 2x/4x/6x, latency, and UI artifacts. That gives a quality bar to beat, or a reason to stop here.

**Phase 0 status (2026-10-17):**
- ✅ Driver 616.92, which is at least 595.97.
- ✅ Hardware-accelerated GPU scheduling is on (HwSchMode=2).
- ✅ Windows build 26300.
- ✅ The ReShade Vulkan layer is no longer registered. The implicit layers still registered are Steam, Fossilize, RTSS, OBS, EOS and GOG.
- ✅ Moved the stray files into `backup/game-files-2026-10-17/`, with SHA-256 hashes in `manifest.csv`: 8× `sl.*` (2.13), `nvngx_dlssg`/`nvngx_dlssnr` 310.8, 3 license files and `ReShadePreset.ini`. Kept `nvngx_dlss.dll` 310.9.1, which came with the game's own 15 Oct update.
- ✅ Steam verification finished and restored **none** of the moved files, so they were all from a mod. The clean install has only `nvngx_dlss.dll`, `steam_api64.dll` and the Aftermath DLL. Our mod will supply its own Streamline 2.14.16 and `nvngx_dlssg`.
- ⏳ The user still needs to remove `--keep-vulkan-layers` in Steam → Enshrouded → Properties → Launch Options.
- ⏳ Optional: a baseline test of Nexus 141, which the user downloads themselves.

### Phase 1: Toolchain and skeleton
- **Install with winget:** VS 2022 Build Tools (Desktop C++ workload, Windows SDK), CMake, Ninja, Vulkan SDK.
- **Fetch dependencies:** Streamline SDK 2.14.16 release (use the **production** DLLs, not the development watermarked ones), MinHook, Dear ImGui, and NGX headers from github.com/NVIDIA/DLSS.
- **Proxy DLL:** a `winmm.dll` that forwards all exports and writes a log line from a deferred init thread. Nothing hooks yet.
- **Done when:** the game runs exactly as before and the log shows the proxy loaded.

**Phase 1 status (2026-10-17): ✅ done.**
- **Toolchain:** VS 2022 Build Tools 17.14, CMake 4.4.4, Ninja 1.13.2, Vulkan SDK 1.4.363.0.
- **Dependencies:** MinHook (8af6b4a) and ImGui v1.92.4 added as git submodules. The Streamline and NGX SDKs are deferred to Phases 2–3, where they're first needed.
- **Proxy:** `tools/gen_winmm_proxy.py` generates MASM jump stubs for all 180 named exports plus 1 ordinal-only export, at their original ordinals. Verified with ctypes.
- **In-game test:** loaded, logged to `EnshroudedFG\log.txt`, no crash. At proxy init, `vulkan-1.dll` and `_nvngx.dll` are not loaded yet, so hooks can be installed before the game uses them.
- **Scripts:** `tools/install.ps1` and `uninstall.ps1`.

### Phase 2: Input capture (no frame gen yet)
- **NGX hook:** hook `LoadLibraryExW` (and the A variant) to catch `_nvngx.dll`, then MinHook `NVSDK_NGX_VULKAN_CreateFeature/EvaluateFeature`.
- **From `CreateFeature`, read:** the DLSS feature flags (`DepthInverted`, `MVJittered`, `MVLowRes`, `IsHDR`) and the render and output sizes.
- **From `EvaluateFeature`, read:** the `NVSDK_NGX_Resource_VK` entries for Depth, MotionVectors and Output, plus `Jitter_Offset_X/Y`, `MV_Scale_X/Y`, `Reset` and the render subrect. Each resource entry gives the VkImage, view, format, size and subresource range.
- **Vulkan hooks:** hook `vkCreateInstance` and `vkCreateDevice` in vulkan-1, and wrap `vkGetDeviceProcAddr` so we can intercept device functions. Log the swapchain format and size.
- **Done when:** the log shows sensible per-frame inputs, and an "Off" frame has no evaluate call (menus and loading screens), which tells us when to pause FG.

**Phase 2 status (2026-10-17): ✅ done.** Findings from the logs (v0.2.2):

**How the game reaches Vulkan**
- Startup: the game loads `vulkan-1`, makes a throwaway 0-extension instance to probe devices, then **frees and reloads `vulkan-1`**. We pin `vulkan-1` and `_nvngx` so the hooks survive the reload.
- Core functions (`vkCreateInstance`, `vkCreateDevice` and the swapchain/present functions) are resolved with `GetProcAddress` on the `vulkan-1` exports. Everything else goes through `vkGet{Instance,Device}ProcAddr`, which we hook too. Note that `kernel32!GetProcAddress` goes through `KernelBase!GetProcAddressForCaller`.
- Instance: API 1.2, 6 extensions (surface, win32_surface, swapchain_colorspace, get_surface_capabilities2, surface_maintenance1, gpdp2).

**Device**
- 1 queue each in families 0 (graphics), 1 (transfer) and 2 (compute). The families support 16/2/8 queues, so there is plenty of room for SL's queues.
- 20 extensions. **`VK_NV_low_latency2` and `present_id` are not among them.** NvAPI `QueryInterface` is only called by NGX and driver modules, never by the exe.
- **So the game's Reflex is not a Vulkan Reflex integration we can translate.** Phase 3 must enable `VK_NV_low_latency2` itself and generate PCL markers around acquire, submit and present.

**Swapchain and present**
- Swapchain: 1920×1080, `R8G8B8A8_UNORM` (37), sRGB colour space, 3 images, usage SAMPLED|STORAGE|COLOR_ATTACHMENT.
- The game requests **FIFO** even with in-game vsync off (the menu ran at 700 fps, so a driver override is probably in place). Phase 3 must switch the present mode to IMMEDIATE/MAILBOX while FG is on.
- One `vkQueuePresentKHR` per frame, on one queue, with 1 wait semaphore and no pNext.

**DLSS SR**
- SR is created once on entering the world: render 1280×720 → 1920×1080, flags HDR | MVLowRes | DepthInverted, so the MVs are not jittered and depth is reverse-Z.
- Inputs:

  | Input | Format | Notes |
  |---|---|---|
  | Color | RGBA16F (97) | HDR, before tonemapping |
  | Depth | R32F (100) | the very first frame used D32 (126) |
  | Motion vectors | RG16 SNORM (78) | MV scale (1280, 720), so they're in UV space and **SL `mvecScale` = (1,1)** |

- Jitter is in pixels, roughly ±0.5 in a repeating sequence. Reset is never set.
- **The same depth and MV images are used every frame.** There is no ping-pong, so they must be copied, or tagged `eOnlyValidNow`.
- **There is one SR evaluate per world frame.** The main menu has **no evaluate** at ~600–700 fps, so a frame without an evaluate is the "pause FG" signal. The map screen still evaluates. The world runs at ~155 fps on the RTX 5090 at DLSS Quality 1080p.
- Everything runs on one thread, including render submission.

### Phase 3: Streamline bring-up (FG still off)
- **`slInit`:**
  - `renderAPI = eVulkan`
  - flags `eUseManualHooking | eUseFrameBasedResourceTagging`
  - `featuresToLoad = {kFeatureDLSS_G, kFeatureReflex, kFeaturePCL}`
  - `engine = eCustom`, plus a fixed `projectId` GUID
  - `pathsToPlugins` = `EnshroudedFG\`
  - The call goes in the `vkCreateInstance` hook, before calling down.
- **Requirements:** call `slGetFeatureRequirements(kFeatureDLSS_G)`, then add its instance and device extensions and its 1.2/1.3 features (using the `sl_helpers_vk.h` helpers). Also add the extra compute, graphics and optical-flow queues it asks for. That means raising `queueCount` in the game's `VkDeviceQueueCreateInfo` and recording the start indices after the game's own queues.
- **`slSetVulkanInfo`:** call it immediately after the real `vkCreateDevice`.
- **Swapchain functions:** route `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR`, `vkDeviceWaitIdle`, `vkCreateWin32SurfaceKHR` and `vkDestroySurfaceKHR` through `vkGetDeviceProcAddrProxy`.
- **Reflex translation:** intercept the game's `VK_NV_low_latency2` calls and map them as follows. Frame tokens come from `slGetNewFrameToken` at simulation start, matched to the game's `presentID`.

  | Game call | Becomes |
  |---|---|
  | `vkSetLatencySleepModeNV` | `slReflexSetOptions` |
  | `vkLatencySleepNV` | `slReflexSleep`, then we signal the game's semaphore |
  | `vkSetLatencyMarkerNV` | `slPCLSetMarker` (the marker enum values line up 1:1) |

- **Done when:** the game runs normally through SL and `slDLSSGGetState` reports `eOk` with `numFramesToGenerateMax == 5`.

**Phase 3 status (2026-10-17): ✅ done (v0.3.3).**

**Result**
- The real instance, device, surface and swapchain/present go through SL's proxies. Manual-hooking mode is used, with SL's own `vkCreateInstance`/`vkCreateDevice` proxies.
- `slInit`, all three `slIsFeatureSupported` calls and Reflex options return `eOk`.
- DLSS-G status is `eOk` with **`numFramesToGenerateMax 5`**, so 6x is available. `bIsVsyncSupportAvailable` is 1 and Dynamic MFG is 0, as expected on Vulkan.
- DLSS-SR still works and its inputs are captured every world frame.
- Overhead is about 5% (147 vs 155 fps in the world), with clean shutdown via `slShutdown`.

**Pitfalls fixed along the way**
- **Device creation crash:** SL's `vkCreateDevice` proxy looks the instance up in a physical-device map that only SL's `vkEnumeratePhysicalDevices` proxy fills. We now call that proxy just before creating the device.
- **Driver plugins:** the driver/NVIDIA App ships **OTA SL plugins** (`ProgramData\NVIDIA\NGX\models\sl_*_override_0\…\*.dll`, same 2.14.16 build) and SL prefers them. They are now registered as internal modules for the re-entry checks.
- **DLSS reported unavailable:** SL initializes NGX first, using its plugin paths as the NGX feature search paths. The game's DLSS-SR then reported "not available", **and the game silently switched the saved setting to FSR3 with automatic render scale**; I restored the user's settings. Fix: the game folder is added to `pathsToPlugins`, and the game's `nvngx_dlss.dll` is hard-linked into `EnshroudedFG\streamline`, refreshed whenever it changes.
- **Init detour crash:** a pass-through detour must declare *at least* as many arguments as the hooked function. `NVSDK_NGX_VULKAN_Init_ProjectID_Ext` takes 11.

**DLSS-G requirements we must honour in Phase 4**
- VSync off (`eVSyncOffRequired`) and HWS on.
- Queues: 2 compute, 1 graphics, 1 optical-flow. SL adds them itself.
- SL's swapchain proxy raised `minImages` from 3 to 6 and chains `VkSwapchainLatencyCreateInfoNV`.

### Phase 4: First generated frames (2x)
- **Each frame:**
  - Tag depth and motion vectors with `slSetTagForFrame` (viewport 0).
  - Start with `eOnlyValidNow`, so SL copies them and we don't depend on the game's image lifetimes. Switch to `eValidUntilPresent` with our own copies only if profiling calls for it.
  - Set `slSetConstants` once per frame: jitter, `mvecScale = MV_Scale / renderSize`, `depthInverted`, `motionVectorsJittered`, `reset`, and `renderingGameFrames`.
- **Camera matrices, step A (approximation):**
  - Build a projection from the FOV setting, the aspect ratio and a reverse-Z near/far.
  - Use an identity view and leave `clipToPrevClip` as identity.
  - Set `cameraMotionIncluded = true`, and give orthonormal camera vectors so scene-change detection stays on.
  - Test whether DLSS-G is happy with this.
- **Camera matrices, step B (only if A causes artifacts or `eFailCommonConstantsInvalid`):**
  - Reverse-engineer the camera object in `enshrouded.exe` with REA/Ghidra. The engine already serializes `cameraTransform` (position + quaternion) into crash `.info` files, which is a strong lead.
  - Read view and projection each frame, using signature scans rather than fixed offsets.
- **Turn it on:** `slDLSSGSetOptions(mode = eOn, numFramesToGenerate = 1)`.
- **Done when:** output FPS is about 2× base, nothing is pink, and status is `eOk`.

**Phase 4 status (2026-10-17): ✅ done (v0.4.2), 2x frame generation working in-game.**

**User impression:** clearly smoother, no visible artifacts, latency slightly floaty.

**How it works**
- **Tags and constants:** at the game's DLSS-SR evaluate, depth and motion vectors are tagged `eValidUntilPresent` with extent 1280×720, and `slSetConstants` is called once per frame. The camera is approximated (60° vertical FOV, reverse-Z, identity clip-to-prev-clip, camera motion included in the MVs); `mvecScale` is the NGX MV scale divided by render size, which comes out as (1,1).
- **When FG runs:** it switches on after 3 consecutive frames with SR inputs, and off with null tags on frames without them (menus, loading). `eRetainResourcesWhenOff` is set.
- **Motion vector conversion:** SL's Vulkan format table has no `R16G16_SNORM`, which caused 8 "undefined format" errors per frame. A compute pass (`src/render/mv_convert.*` plus the embedded SPIR-V shader `mv_convert.comp`) now copies the MVs into one of two ping-pong RG16F images right before the DLSS-SR evaluate, and that copy is what gets tagged.
- **Log flood guard:** repeated SL warnings and errors are logged 3 times, then only counted.
- **Settings:** `EnshroudedFG.ini` holds `[FrameGen] Multiplier` (1–6) and `[Camera]` FOV/near/far.

**Bugs fixed**
- NGX handle ids are only unique per NGX context. SL's DLSS-G feature also got id 1 and replaced the game's SR entry, so FG switched itself off. Features are now keyed by handle pointer, and NGX calls from SL modules are ignored.
- The game switched its DLSS setting to FSR3 once, after the broken v0.3.1 run (see Phase 3).

**Measurements and open points**
- Base fps went from ~147 to ~119 with FG on, so output is ~238 fps at 2x. The cost is ~1.6 ms per real frame.
- SL warns "VSync interval 1 not supported with FG, clamping to 1", because the game creates a FIFO swapchain. Revisit in Phase 7 (option to force IMMEDIATE/MAILBOX while FG is on).
- Quick 3x–6x tests are already possible by setting `Multiplier` in the ini.

### Phase 5: MFG selector and ImGui overlay
- **Overlay:**
  - Toggled with the Insert key (configurable). ImGui uses the Win32 and Vulkan backends on SL's swapchain images.
  - It's drawn into an RGBA8 UI image tagged `kBufferTypeUIColorAndAlpha` and composited on real frames, so it isn't interpolated.
  - A WndProc hook blocks game input while the menu is open.
- **Controls:**
  - Frame Generation: Off / 2x / 3x / 4x / 5x / 6x. This sets `numFramesToGenerate` from 1 to 5, capped at `numFramesToGenerateMax`.
  - Reflex mode, HUD-less toggle, and an overlay hotkey setting.
  - Stats: base FPS, output FPS, `numFramesActuallyPresented`, DLSS-G status and VRAM estimate.
- **Persistence:** settings are saved to `EnshroudedFG.ini`.
- **Switching modes:**
  - Off↔On: set `eOff` and force a swapchain recreate, as the docs require.
  - Changing N while on: apply directly, and verify that no recreate is needed.
- **Dynamic MFG:** not offered, because SL marks it D3D12-only.

**Phase 5 status (2026-10-17): ✅ done (v0.5.0).**

**What shipped**
- **Menu:** the Insert key (`[Overlay] ToggleKey` in the ini) opens an ImGui menu (`src/overlay/overlay.*`) with Off/2x–6x. Options above the GPU maximum are disabled. Changes apply live and are saved to the ini.
- **Rendering:** ImGui draws into the presented image on our own command buffer, which waits on the game's present semaphores; present then waits on ours. It uses dynamic rendering and push-free descriptors, via ImGui's internal pool.
- **Input:** the window is subclassed and input is swallowed while the menu is open. `SetCursorPos`/`ClipCursor` are suspended and ImGui draws a software cursor.
- **FG while open:** v0.5 paused frame generation while the menu was open; since v0.8.2 it keeps running, because the menu is drawn after the HUD-less capture and DLSS-G treats it as UI.

**Verified in-game**
- DLSS-G reported `numFramesActuallyPresented` of **2, 3, 4, 5 and 6** for 2x–6x.
- Menu input works and the game ignores input while the menu is open.

**User feedback**
- 4x–6x show visible artifacts and feel too laggy.
- The display is above 240 Hz, and base fps is ~115, so 6x asks for about 690 fps.

**Follow-ups this feeds into**
- Phase 6: HUD-less and UI buffers, plus the real camera matrices, to reduce artifacts.
- Phase 7: an output cap, so base fps × multiplier stays at or below the refresh rate (Reflex `frameLimitUs`), plus a VSync/present-mode option. Also clean up the stray `; Multiplier=` line that v0.4 wrote into the ini.

### Phase 6: Image quality
- **Find where the HUD starts:** take a RenderDoc capture to locate the render pass where the HUD begins, after tonemapping and post-processing.
- **HUD-less color:** hook `vkCmdBeginRendering` / `vkCmdBeginRenderPass2` for that pass and copy the target into a HUD-less image. Tag it as `kBufferTypeHUDLessColor`, together with a UI buffer, and set `enableUserInterfaceRecomposition`. This removes HUD and compass smearing.
- **Pause FG when there's no 3D frame:** set all tags to null and FG off on frames with no DLSS evaluate (menus, loading, map screen).
- **Window changes:** set `eOff` before resize, minimize or fullscreen toggles, and on alt-tab.

**Phase 6 status (2026-10-17): HUD-less ✅ (v0.6.1), camera matrices in progress.**

**Frame tracer.** `[Debug] FrameTrace=1` enables it; F11 traces one frame, plus one automatic trace after 900 world frames. Output goes to `EnshroudedFG\frame_trace.txt` (`src/debug/frame_trace.*`). A traced world frame looks like this:

1. DLSS-SR: RGBA16F 1280×720 → 1920×1080.
2. One compute dispatch tonemaps straight into the **swapchain image**, as GENERAL.
3. A compute downsample chain to 640/320/160 RGBA8 (UI blur backgrounds).
4. Two off-screen RGBA8 2048×1208 UI passes, with 35 and 106 draws.
5. **A final `vkCmdBeginRendering` on the swapchain image (loadOp LOAD, 91 draws) composites the HUD.**
6. Present.

**HUD-less colour**
- Capture point: in the `vkCmdBeginRendering` hook, on the first pass after DLSS-SR whose colour attachment is the swapchain image with LOAD.
- At that point we insert barrier → compute copy (`copy_rgba8.comp`, sharing the generic `render::CopyPass` with the MV conversion) → barrier, writing into ping-pong RGBA8 images.
- The copy is tagged `kBufferTypeHUDLessColor` (eValidUntilPresent, 1920×1080).
- Swapchain images and the game's views of them are tracked through the `vkGetSwapchainImagesKHR` and `vkCreateImageView` hooks.

**User test at 4x–6x:** "HUD clean now", and the world "looks good".

**Game camera** (`src/camera/camera_provider.*`), from static reverse engineering of build 25988036. All five byte signatures resolved uniquely in-game.

| What | Where |
|---|---|
| Static camera transform | exe+0x1FC4F68, found via the camera-copy setter signature |
| Camera inside GameClient | `module(exe+0x1FE2CB0) → +0x10 → +0x38 → [3] (Play, id check) → +0x1B8 = GameClient`, camera at +0x4672A8 |

- **Layout:** 3×int64 position in 32.32 fixed point, then a quaternion (x, y, z, w), then scale. Near/far are at +0x38/+0x3C and the vertical FOV in radians at +0x40.
- **Conventions:** left-handed, +Y up, +Z forward, row vectors.
- **What SL receives:** our reverse-Z projection built from the game's FOV/near/far, and camera vectors from the quaternion. `clipToPrevClip` comes from the previous frame's camera, with positions centred on the current camera for precision. Reset is forced if the camera jumps more than 25 m.
- **Safety:** reads go through SEH-guarded copies, and any failed read or sanity check falls back to the approximation.
- **Toggle:** `[Camera] Source=game|approx`, or the "Use game camera data" menu checkbox.
- **User A/B at 4x–6x:** the game camera looked better.
- **v0.6.3 verification (2 min at 6x):**
  - Every frame used the game camera, with no forced resets and a per-frame step of 0.2–1.6 m.
  - `framesPresented 6` the whole time; rendered fps ~75–80, so ~450–480 output.
  - Clean `slShutdown`.
  - Right after entering the world, DLSS-G presents 1 frame/frame for a few seconds while the game still holds its static spawn camera (identity rotation). It recovers as soon as the camera moves. This is benign and left as is.
- **Cost of higher multipliers (input to Phase 7):** rendered fps drops from ~147 (no FG) to ~119 (2x) to ~77 (6x).
- **Fix:** SL shutdown now runs before our copy images are freed. v0.6.2 crashed inside SL's exception handler at exit.

### Phase 7: Hardening and packaging
- **VSync:** Vulkan FG doesn't support vsync. Force `MAILBOX`/`IMMEDIATE` when FG is on, warn about in-game vsync, and recommend G-Sync plus a frame cap through the overlay.
- **Overlays:** test with RTSS, OBS and the Steam overlay.
- **Game updates:** survive them through export hooks and signature scans, not hardcoded addresses.
- **Packaging:** `install.ps1` copies the files into the game folder and backs up anything it replaces. `uninstall.ps1` restores it.

**Phase 7 status (2026-10-17): ✅ done (v0.7.2), packaged as `dist/EnshroudedFG-0.7.2.zip` via `tools/package.ps1`.**

**Output cap (`[FrameGen] OutputCap` = off | auto | fps; menu radio)**
- Uses the Reflex frame limiter. **While DLSS-G runs, the driver applies `frameLimitUs` to presented frames.**
  - Measured in v0.7.1 on a 280 Hz display with a 272 fps limit: output held at 271–278 fps and rendered fps settled at 272/N (45 at 6x, 55 at 5x, 68 at 4x, 90 at 3x).
  - v0.7.0 had wrongly assumed the limit applies to rendered frames, which pushed rendered fps down to 18–28.
- **User verdict:** the cap feels worse at 6x, because rendering drops to 45 fps. 2x without a cap feels best, since ~110–140 rendered fps already fills 280 Hz.
- So the cap **defaults to off**. The menu shows a tip with the multiplier that fits the display.

**Present mode (`[Display] PresentMode` = game | immediate | mailbox | fifo)**
- Overrides the game's swapchain present mode if the surface supports the requested one. It applies when the game recreates its swapchain.

**Ini, logging and menu fixes**
- Every option is written with its default.
- The stray `; Multiplier` lines are removed by filtering the file text, because the profile API can't delete `;` lines.
- `[Debug] VerboseLog=0` keeps lookup and `GetProcAddress` diagnostics off by default.
- The menu's duplicate "Off" ImGui IDs made the cap's "Off" button switch frame generation off. Fixed with `##outputcap` and `PushID` around the multiplier buttons.

**Package**
- Contents: `winmm.dll`, `README.txt` (requirements, install/uninstall, menu, ini, notes) and `EnshroudedFG\streamline\` with the NVIDIA-signed SL 2.14.16 DLLs (signatures checked during packaging).
- Licences included: Streamline, DLSS, Reflex, MinHook and ImGui.
- Review NVIDIA's licence terms before redistributing the package publicly.

### Phase 8: Auto multiplier (hand-rolled dynamic MFG)
Streamline's Dynamic MFG is D3D12-only (`bIsDynamicMFGSupported` reads 0 here), so Auto is our own controller: `UpdateAutoMultiplier` in `src/sl/sl_bridge.cpp`, run from `AfterPresent`. It changes N through the existing live `slDLSSGSetOptions` path.

**Phase 8 status (2026-10-17): ✅ done (v0.8.0). Commits f440212, e50877d and 0674350, then the version bump.**

**Auto (`[FrameGen] Multiplier=auto`, menu radio "Auto (Nx)")**
- **Goal:** pick N in [2, max] so that rendered fps × N just fills the display (target = refresh × 0.97).
- **Timing:** evaluated every 2 s while FG runs. It waits a full interval after FG turns on (menus, loading) and after each change.
- **Prediction:** output at another multiplier M is predicted as rendered × M × keep^(M−N), where keep is the share of rendered fps that survives one more generated frame.
- **Step down:** when output is more than 5% above target, go to the largest M predicted to stay at or below target (minimum 2). Multipliers above stay blocked for 10 s.
- **Step up:** to the largest M still predicted below target.
- **Logging:** each change is logged as `auto: rendered X fps xN = Y fps output, target T (Hz), cap C, keep K/step -> Mx (reason)`. `VerboseLog=1` also logs every evaluation.

**Finding 1, fast EMA (first DLAA test, 155 evaluations, every decision matched the rules)**
- Decisions first used the fast `g_baseFps` EMA (~0.2 s).
- Two 2x→3x steps were triggered by momentary dips (95 and 91.5 fps). 3x then rendered 100–106 fps (298–317 output), so Auto went back to 2x 2 s later.
- **Fix:** decide on the average rendered fps over the whole 2 s window (frames ÷ elapsed).

**Finding 2, starting cost guess (second DLAA test)**
- The dip flips were gone, and a later step to 3x held for 110 s.
- One 2x→3x→2x flip remained at session start. Under DLAA, 3x cost about 0% rendered fps (99.2 → 99.3), so the 0.9 prior predicted 268 for 3x when it was really 298.
- **Fix:** `[FrameGen] AutoKeep`, default 0.95, clamped 0.85–1.0. It is refined from Auto's own uncapped steps (50/50 blend) and saved back to the ini when the 3-decimal value changes. With 0.95, the 99.2 fps case predicts 283 and stays at 2x.

**Finding 3, output cap (cap-mode probing)**
- **Problem:** with `OutputCap` on, output can never exceed the cap (≈ the target), so "more than 5% above" never fires. Auto stayed at whatever N it had reached, which was 3x in-game.
- **Why rendered fps can't decide it:** capped rendered fps is just cap/N and shows no headroom.
- **Cap-mode rule:** aim for the fewest generated frames that still reach the cap.
  - Output ≥ 97% of the cap → probe N−1.
  - Output < 93% → step up to the smallest M predicted to reach the cap.
  - A probed-down N that falls below 93% before ever reaching the cap is a failed probe. The wait before the next probe doubles (20 → 40 → 80 → 160 s) and resets to 20 s after a probe that reaches the cap.
  - Keep learning is skipped while capped.
- **In-game, first cap-mode build (12 changes, all per rules):** Auto stepped down from 5x for the first time. It settled at 4x–5x (54–68 rendered fps) where uncapped Auto chose 2x–3x (~90–100), as the cap's semantics imply.
  - Twice a probe sat in the 93–97% band and then drifted lower without counting as failed, so the wait didn't grow.
  - **Fix:** a probe stays pending until it confirms the cap.
- **In-game, final build (18 changes, all per rules):**
  - Failed probes waited 20.1 s, then 40 s, then 80 s.
  - A 5x→4x→3x→2x cascade reset the wait to 20 s (4x and 3x reached the cap), and only 2x failed.
  - A probe that held the band (254) and then dropped (248) now counted as failed, and the wait doubled to 160 s.
  - The 160 s clamp itself was not exercised before the session ended.

**Defaults unchanged:** `Multiplier=2` and `OutputCap=off`. Auto is opt-in, from the menu or the ini.

**Corner readout (v0.8.1; commits 9d42b2f and 3f0b31b)**
- **What it is:** `[Overlay] Readout=1`, or the "Corner readout" menu checkbox; off by default. A small ImGui line shows the mode (`FG Auto 3x`, `FG 2x`, `paused`, `FG off`), rendered fps and output fps (rendered × N while FG runs).
- **How it's drawn:** `overlay::BeforePresent` now also draws when only the readout is on. It's drawn onto the swapchain image after the HUD pass, so it isn't in the HUD-less copy and DLSS-G treats it like the game's HUD. User test: clean at 3x and 4x, no smearing.
- **Dragging:** while the menu is open, the readout accepts mouse input and can be dragged.
  - The spot is saved on release as `[Overlay] ReadoutX` / `ReadoutY` (pixels; -1 means the top-right corner).
  - Right-click resets it to the corner.
  - A saved spot is clamped back on screen if the resolution shrinks.
  - With the menu closed it is `NoInputs`.
  - User test: dragging works in-game.

### v0.8.2: themed overlay, Reflex control, counter items (2026-10-18)

**Enshrouded-style theme** (`src/overlay/theme.*`), modelled on the game's settings and character screens:
- **Surfaces:** indigo gradient panels with a gold hairline frame and diamond corner ornaments, plus a soft drop shadow.
- **Type:** Palatino Linotype (Georgia/Constantia fallback, loaded at runtime from `C:\Windows\Fonts`, nothing bundled) for body text, spaced capitals for titles.
- **Controls:** `‹ value ›` stepper rows like the game's settings, big serif stat numbers, white keycap hints.
- **Scale:** everything scales with output height / 1080.

**Menu (Insert)**
- Rows: Frame Generation, Output Cap, NVIDIA Reflex, Camera Data, Frame Counter, Counter Shows.
- Status block: rendered, output, multiplier and latency stats, plus colored status lines.
- Pitfalls fixed:
  - `PushTextWrapPos` takes a window-local x.
  - An item covering a row steals hover and clicks from items placed on top of it, so row hover is a plain rect test.

**Counter:** a miniature of the menu panel: parchment captions, bold serif values, gold diamond separators, and "paused" in muted italics.
- `[Overlay] ReadoutItems` (fg, output, fps, latency, frametime, reflex) picks the segments.
- The menu edits it through a themed "Counter Shows" dropdown with diamond check items; at least one item always stays selected.

**FG while the menu is open:** frame generation keeps running. The menu is drawn after the HUD-less capture, so DLSS-G treats it as UI. User test: the menu stays crisp, and changes apply live.

**Reflex:** `[Reflex] Mode` on | boost | off (default on), plus a menu row.
- DLSS-G needs Reflex, so "off" only applies while FG is off.
- Mode and the output-cap frame limit go through one `slReflexSetOptions` path.
- Latency is the average of `gpuRenderEndTime - simStartTime` over Reflex's 64 frame reports, polled every 30 frames while the menu or the counter is visible.
- User test: 20–40 ms at 6x. Boost changed nothing, as expected when GPU-bound.

## Key risks
- **Manual hooking:** SL Vulkan manual hooking is documented as "advanced integration only". Running an injected `sl.interposer` next to the game's own NGX instance is untested, so Phase 3 is the go/no-go gate.
- **Camera matrices:** the approximation may not be good enough, and step B is a reverse-engineering effort of unknown size.
- **Latency at high multipliers:** the community already reports this for 4x–6x. Reflex has to be correctly wired through SL.
- **Game patches:** they can move the pass where the HUD starts, which the Phase 6 HUD-less capture depends on.
- **Online play:** no anti-cheat was found for Enshrouded, but still use the mod in solo play first.

## Verification
- **Each phase:** has its own "Done when" checkpoint and is checked against the log (`EnshroudedFG\log.txt`).
- **NVIDIA indicator:** turn on the NGX indicator by setting `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator = 0x400`. It confirms DLSS-G is active and shows the multiplier.
- **Frame rates:** PresentMon or RTSS shows output ≈ N × base FPS at each of 2x–6x, and the overlay reports `numFramesActuallyPresented = N`.
- **Visual checks:** no pink frames, no warped overlay or HUD after Phase 6, and stable alt-tab, resize and menu transitions.
- **Comparison:** latency (Reflex/PCL stats in the overlay) and image quality against the Nexus 141 baseline from Phase 0.
