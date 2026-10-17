EnshroudedFG - DLSS Multi Frame Generation (2x-6x) for Enshrouded
=====================================================================

Adds NVIDIA DLSS Frame Generation / Multi Frame Generation to Enshrouded (Vulkan), using the
game's own DLSS inputs, its HUD-less image and its real camera. Switch Off / 2x / 3x / 4x / 5x / 6x
in game with the Insert key.

Requirements
------------
- NVIDIA GeForce RTX 40 series (up to 2x) or RTX 50 series (up to 6x)
- NVIDIA driver 595.97 or newer, Windows 10 20H1+ with "Hardware-accelerated GPU scheduling" ON
- In Enshrouded: Anti-aliasing / upscaling set to DLSS (any quality, or DLAA). Frame generation
  needs the game's DLSS inputs, so it does nothing with FSR/XeSS/TAA.
- Remove other injectors first (ReShade Vulkan layer, OptiScaler, other frame-gen mods), and do not
  use the "--keep-vulkan-layers" launch option.

Install
-------
1. Close the game.
2. Copy the CONTENTS of this folder into the game folder, e.g.
     F:\SteamLibrary\steamapps\common\Enshrouded\
   so that winmm.dll sits next to enshrouded.exe and the EnshroudedFG\ folder is created there.
3. Start the game through Steam, load into the world and press Insert.

Uninstall
---------
Delete winmm.dll and the EnshroudedFG folder (and EnshroudedFG.ini) from the game folder.
Verify the game files in Steam if you want to be sure everything is back to stock.

In-game menu (Insert)
---------------------
- Frame generation: Off / 2x / 3x / 4x / 5x / 6x. Multipliers your GPU cannot do are greyed out.
- Output cap (default Off): Auto keeps the displayed frame rate 3% below your monitor's refresh rate
  using the Reflex frame limiter. Rendering then runs at cap / multiplier, so at high multipliers it
  feels laggier. Usually better: choose the multiplier whose output just reaches your refresh rate
  (the menu shows a tip), e.g. 2x when you render ~140 fps on a 280 Hz display.
- Use game camera data: on by default; off uses an approximate camera (for comparison only).
Frame generation pauses while the menu is open. All choices are saved to EnshroudedFG.ini.

EnshroudedFG.ini (next to enshrouded.exe)
------------------------------------------
[FrameGen] Multiplier   1 (off) .. 6
[FrameGen] OutputCap    off | auto | <fps>
[Display]  PresentMode  game | immediate | mailbox | fifo   (applies after restarting the game)
[Overlay]  ToggleKey    virtual-key code, default 0x2D (Insert)
[Camera]   Source       game | approx
[Debug]    VerboseLog / FrameTrace   diagnostics only (FrameTrace: F11 writes EnshroudedFG\frame_trace.txt)

Notes
-----
- Frame generation is automatically off in menus and loading screens and resumes in the world.
- Higher multipliers cost GPU time: rendered fps drops (on an RTX 5090 at 1080p DLSS Quality roughly
  147 -> 119 fps at 2x -> 77 fps at 6x). Pick the multiplier whose output matches your display.
- Logs: EnshroudedFG\log.txt and EnshroudedFG\sl.log. Include both when reporting a problem.
- The camera data is read from game memory via byte signatures. After a game update the mod falls
  back to an approximate camera automatically if the signatures no longer match (see log.txt).
- Play solo first; use in multiplayer at your own risk.

Third-party components
----------------------
- NVIDIA Streamline SDK 2.14.16 production binaries (sl.*.dll, nvngx_dlssg.dll, NvLowLatencyVk.dll)
  - see EnshroudedFG\streamline\licenses\
- MinHook (BSD 2-Clause), Dear ImGui (MIT) - see EnshroudedFG\streamline\licenses\THIRD_PARTY.txt
