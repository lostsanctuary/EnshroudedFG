# Security policy

## Reporting a vulnerability

Please **don't open a public issue** for security problems. Report them privately through GitHub instead:

1. Go to the repository's **Security** tab and choose **Report a vulnerability**, or open [this link](https://github.com/lostsanctuary/EnshroudedFG/security/advisories/new) directly.
2. Describe the problem, the affected version and how to reproduce it. A proof of concept helps, if you have one.

Only the maintainer can see the report. You'll get a reply within a week. Once a fix is released, the advisory is published with credit to you, unless you'd rather stay anonymous.

## Supported versions

Only the latest release gets security fixes. Please check that the problem still happens there before reporting it.

## What counts

EnshroudedFG is a DLL that the game loads, so security issues include:

- **Untrusted code loading:** ways to make the mod load or run code it shouldn't, for example an unsigned or swapped Streamline plugin getting past the signature check, or DLLs loaded from unexpected paths.
- **Memory safety:** crashes or memory corruption in the mod that can be triggered by data from outside the game process, such as a crafted `EnshroudedFG.ini`.
- **Scripts:** problems in `tools/*.ps1`. That includes `fetch_deps.ps1` downloading or extracting files unsafely, and the install scripts writing outside the game folder.
- **Release packages:** a release zip that contains something it shouldn't.

These are out of scope:

- **Upstream bugs:** problems in Enshrouded itself, the NVIDIA driver, DLSS or Streamline. Report those to Keen Games or NVIDIA.
- **Normal bugs:** crashes, visual artifacts or performance problems that aren't security issues. Use the [bug report form](https://github.com/lostsanctuary/EnshroudedFG/issues/new/choose) for those.
- **Cheating and anti-cheat:** questions about using the mod in multiplayer.
