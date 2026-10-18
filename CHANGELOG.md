# Changelog

User-facing changes in each release. Downloads are on the [Releases](https://github.com/lostsanctuary/EnshroudedFG/releases) page, and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) has the full development log.

Settings are kept across versions. Unless noted, the defaults stay `Multiplier=2`, `OutputCap=off` and `Readout=0`.

## [Unreleased]

<!-- Add changes here as they land. When releasing, rename this section to "## [x.y.z] - YYYY-MM-DD"
     and start a new empty [Unreleased] above it: the Release workflow refuses a tag whose version
     has no "## [x.y.z]" section here. -->

## [0.8.4] - 2026-10-18

A maintenance release: frame generation, the menu options and your settings work exactly as in 0.8.3.

### Changed
- **ImGui 1.93:** the in-game overlay moved to Dear ImGui 1.93. The menu, its dropdown and the frame counter look the same as before.
- **Crisp edges:** the overlay's thin gold lines and row edges stay crisp. ImGui 1.93 would otherwise soften them.

### Added
- **Open source:** the source is public on GitHub under the MIT licence, with build instructions, contributing and security guides, and bug report and feature request forms.
- **Licence in the zip:** the release zip includes the mod's own licence (`EnshroudedFG\streamline\licenses\EnshroudedFG-LICENSE.txt`).
- **Automated releases:** release zips are built and packaged by GitHub Actions, which also checks that every NVIDIA DLL in them is validly signed.

### Fixed
- **Zip paths:** the release zip stores its paths with forward slashes. Earlier zips used backslashes, which some Linux and macOS unzip tools mishandle; Windows was never affected.

## [0.8.3] - 2026-10-18

Also contains the 0.8.2 changes, which had no release of their own.

### Added
- **NVIDIA Reflex option:** `[Reflex] Mode` = on | boost | off, default on. Frame generation requires Reflex, so Off only applies while frame generation is off.
- **Latency:** measured from Reflex's own frame reports and shown in the menu and, optionally, in the counter.
- **Counter Shows** (`[Overlay] ReadoutItems`): pick what the frame counter displays from a dropdown: frame-generation mode, output fps, rendered fps, latency, frame time and Reflex mode.
- **Counter Size** (`[Overlay] ReadoutSize`): small, medium or large.
- **Counter Opacity** (`[Overlay] ReadoutOpacity`): 20–100 % in 10 % steps.

### Changed
- **Menu style:** the Insert menu matches Enshrouded's own UI, with indigo panels, gold trim, a serif typeface (Palatino Linotype from your Windows fonts, or Georgia or Constantia) and `‹ value ›` rows. A status block shows rendered fps, output fps, the multiplier and latency.
- **Counter style:** the frame counter is restyled as a small version of the menu panel.
- **Menu open:** frame generation keeps running while the menu is open, so changes apply live. The menu stays crisp because DLSS-G treats it as UI, the same way it handles the game's HUD.

## [0.8.1] - 2026-10-17

### Added
- **Corner readout** (`[Overlay] Readout=1`, off by default): an always-on line showing the frame-generation mode (`FG Auto 3x`, `FG 2x`, `paused`, `FG off`), rendered fps and output fps.
- **Not interpolated:** it's drawn like the game's own HUD, so DLSS-G doesn't smear it.
- **Placement:** it starts in the top-right corner. While the menu is open you can drag it anywhere (saved as `[Overlay] ReadoutX` / `ReadoutY`), and right-clicking it puts it back.
- **Small screens:** a saved spot that would fall off-screen at a lower resolution is pulled back on. With the menu closed, the readout takes no mouse input.

## [0.8.0] - 2026-10-17

### Added
- **Auto multiplier** (`[FrameGen] Multiplier=auto`, or "Auto" in the menu): picks a multiplier from 2x up to your GPU's maximum so that rendered fps × multiplier just fills your display (refresh rate − 3%).
  - **Timing:** it re-evaluates every 2 seconds, using the average rendered fps over that window, so brief dips don't flip the multiplier.
  - **Learning:** it learns how much rendered fps each extra generated frame costs (`[FrameGen] AutoKeep`, default 0.95; delete the line to reset it).
  - **With the output cap on,** Auto looks for the fewest generated frames that still reach the cap. After a failed attempt to step down, it waits longer before trying again (20 s, doubling up to 160 s).
  - **Logging:** each change is written to `EnshroudedFG\log.txt` (`auto: ...`).
  - **Opt-in:** Auto is not the default.

[Unreleased]: https://github.com/lostsanctuary/EnshroudedFG/compare/v0.8.4...HEAD
[0.8.4]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.4
[0.8.3]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.3
[0.8.1]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.1
[0.8.0]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.0
