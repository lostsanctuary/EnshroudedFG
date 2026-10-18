# Changelog

User-facing changes in each release. Downloads are on the [Releases](https://github.com/lostsanctuary/EnshroudedFG/releases) page, and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) has the full development log.

Settings are kept across versions. Unless noted, the defaults stay `Multiplier=2`, `OutputCap=off` and `Readout=0`.

## [Unreleased]

<!-- Add changes here as they land. When releasing, rename this section to "## [x.y.z] - YYYY-MM-DD"
     and start a new empty [Unreleased] above it: the Release workflow refuses a tag whose version
     has no "## [x.y.z]" section here. -->

## [0.8.6] - 2026-10-18

### Added
- **Copy-pass GPU timing:** with `[Debug] VerboseLog=1`, the log shows every 600 frames how much GPU time the mod's own copies take (depth, motion vectors, HUD-less). With logging off it costs nothing. They measure about 44 µs per frame, around 0.5% of a 6x frame on an RTX 5090.

### Changed
- **Barrier after each copy:** the game's next rendering work no longer waits for the mod's copies to finish, only DLSS-G does. The gain is a few microseconds per frame.

## [0.8.5] - 2026-10-18

### Changed
- **Less frame generation overhead:** the game now starts rendering its next frame while DLSS-G is still generating frames, instead of waiting for it. On an RTX 5090, rendered fps rose from 130 to 135 at 2x and from 93 to 115 at 6x, with no change in image quality or latency.
- **Fallback setting:** `[FrameGen] QueueParallelism = block` restores the previous behaviour if you see artifacts or instability. It applies after restarting the game.

### Added

These are for contributors and maintainers.

- **Changelog check:** pull requests that change `src/` must update the `[Unreleased]` section of this file or add a new release section. The `skip-changelog` label skips the check for changes that aren't user-facing. Tagged releases also fail unless `CHANGELOG.md` has an entry for their version.
- **Branch protection:** `main` only accepts changes through pull requests that pass the `build` and `changelog` checks, for maintainers too. Force-pushes and deleting `main` are blocked.
- **CODEOWNERS:** @lostsanctuary is the code owner of the whole repository and is asked to review pull requests from other contributors.
- **Dependabot auto-merge:** Dependabot pull requests that only bump GitHub Actions merge by themselves once `build` and `changelog` pass. Submodule bumps (minhook, imgui) still need a person, because they change the mod and need testing in game.
- **Branch cleanup script:** `tools/prune_branches.ps1` deletes local branches whose GitHub branch was deleted after the pull request merged.

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

[Unreleased]: https://github.com/lostsanctuary/EnshroudedFG/compare/v0.8.6...HEAD
[0.8.6]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.6
[0.8.5]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.5
[0.8.4]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.4
[0.8.3]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.3
[0.8.1]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.1
[0.8.0]: https://github.com/lostsanctuary/EnshroudedFG/releases/tag/v0.8.0
