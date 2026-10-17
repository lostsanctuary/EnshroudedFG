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

### Phase 6: Image quality
- **Find where the HUD starts:** take a RenderDoc capture to locate the render pass where the HUD begins, after tonemapping and post-processing.
- **HUD-less color:** hook `vkCmdBeginRendering` / `vkCmdBeginRenderPass2` for that pass and copy the target into a HUD-less image. Tag it as `kBufferTypeHUDLessColor`, together with a UI buffer, and set `enableUserInterfaceRecomposition`. This removes HUD and compass smearing.
- **Pause FG when there's no 3D frame:** set all tags to null and FG off on frames with no DLSS evaluate (menus, loading, map screen).
- **Window changes:** set `eOff` before resize, minimize or fullscreen toggles, and on alt-tab.

### Phase 7: Hardening and packaging
- **VSync:** Vulkan FG doesn't support vsync. Force `MAILBOX`/`IMMEDIATE` when FG is on, warn about in-game vsync, and recommend G-Sync plus a frame cap through the overlay.
- **Overlays:** test with RTSS, OBS and the Steam overlay.
- **Game updates:** survive them through export hooks and signature scans, not hardcoded addresses.
- **Packaging:** `install.ps1` copies the files into the game folder and backs up anything it replaces. `uninstall.ps1` restores it.

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
