EnshroudedFG - DLSS Multi Frame Generation (2x-6x) for Enshrouded
=====================================================================

Adds NVIDIA DLSS Frame Generation / Multi Frame Generation to Enshrouded (Vulkan), using the
game's own DLSS inputs, its HUD-less image and its real camera. Switch Off / 2x / 3x / 4x / 5x / 6x
/ Auto in game with the Insert key.

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
- Frame generation: Off / 2x / 3x / 4x / 5x / 6x / Auto. Multipliers your GPU cannot do are greyed out.
- Auto picks the multiplier for you (2x up to your GPU's maximum) so that rendered fps x multiplier
  just fills your display (refresh rate - 3%). Every 2 seconds while you play it looks at the average
  rendered fps over those 2 seconds (so a brief dip does not switch the multiplier): it steps
  down when the output is more than 5% above that, and up only when the next multiplier is expected
  to stay below it (it learns how much each extra generated frame costs in rendered fps). The button
  shows the current choice, e.g. "Auto (3x)"; changes are written to EnshroudedFG\log.txt ("auto: ...").
  With the output cap on, Auto instead looks for the fewest generated frames that still reach the
  cap (the cap stops any overshoot): while the output sits at the cap it tries one generated frame
  fewer, and if the output drops clearly below the cap before that lower multiplier has reached it,
  it goes back up and waits before trying again (20 s, doubling up to 160 s after repeated misses).
- Output cap (default Off): Auto keeps the displayed frame rate 3% below your monitor's refresh rate
  using the Reflex frame limiter. Rendering then runs at cap / multiplier, so at high multipliers it
  feels laggier. Usually better: choose the multiplier whose output just reaches your refresh rate
  (the menu shows a tip), e.g. 2x when you render ~140 fps on a 280 Hz display.
- NVIDIA Reflex: Off / On / On + Boost (default On). Frame generation requires Reflex, so Off only
  applies while frame generation is off. The menu and the counter show the measured latency
  (simulation start to GPU render end, from Reflex's own reports).
- Use game camera data: on by default; off uses an approximate camera (for comparison only).
- Corner readout (default off): a small always-on line showing the multiplier (e.g. "FG Auto 3x"),
  rendered fps and output fps. It starts in the top-right corner; while the menu is open you can
  drag it anywhere (the spot is saved), and right-clicking it puts it back in the corner.
Frame generation keeps running while the menu is open, so changes take effect live. All choices are
saved to EnshroudedFG.ini.

EnshroudedFG.ini (next to enshrouded.exe)
------------------------------------------
[FrameGen] Multiplier   1 (off) .. 6 | auto
[FrameGen] AutoKeep     0.85 .. 1.0, default 0.95: share of rendered fps Auto expects to keep per extra
                        generated frame. Auto updates it as it learns; delete the line to reset it.
[FrameGen] OutputCap    off | auto | <fps>
[Reflex]   Mode         on | boost | off
[Overlay]  ReadoutItems fg, output, fps, latency, frametime, reflex (what the counter shows)
[Overlay]  ReadoutSize  small | medium | large
[Overlay]  ReadoutOpacity  20 .. 100 (percent)
[Display]  PresentMode  game | immediate | mailbox | fifo   (applies after restarting the game)
[Overlay]  ToggleKey    virtual-key code, default 0x2D (Insert)
[Overlay]  Readout      0 | 1   corner readout (multiplier, rendered fps, output fps)
[Overlay]  ReadoutX/Y   readout position in pixels, set by dragging; -1 = top-right corner
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
