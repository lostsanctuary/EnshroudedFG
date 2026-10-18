# Third-party notices

EnshroudedFG's own code is MIT licensed (see [LICENSE](LICENSE)). It builds on the components below,
which keep their own licences.

## In this repository

| Component | Where | Licence |
|---|---|---|
| [NVIDIA Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) 2.14.16 headers | `external/streamline/include` | MIT, see `external/streamline/license.txt` |
| [MinHook](https://github.com/TsudaKageyu/minhook) | `external/minhook` (submodule) | BSD 2-Clause, see `external/minhook/LICENSE.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.4 | `external/imgui` (submodule) | MIT, see `external/imgui/LICENSE.txt` |

## Downloaded by `tools/fetch_deps.ps1` (not in this repository)

| Component | Where | Licence |
|---|---|---|
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) NGX headers, commit `9981c63` | `external/ngx/include` | NVIDIA DLSS SDK licence (`LICENSE.txt` in that repository) |
| Streamline 2.14.16 production DLLs: `sl.interposer`, `sl.common`, `sl.dlss_g`, `sl.reflex`, `sl.pcl`, `nvngx_dlssg`, `NvLowLatencyVk` | `external/streamline/bin/x64` | Streamline: MIT. DLSS Frame Generation and Reflex runtimes: NVIDIA licences shipped alongside (`nvngx_dlss.license.txt`, `reflex.license.txt`) |

The release zips include the NVIDIA runtime DLLs unmodified and NVIDIA-signed, together with their
licence files in `EnshroudedFG\streamline\licenses\`.

## Trademarks

NVIDIA, GeForce RTX, DLSS and Reflex are trademarks of NVIDIA Corporation. Enshrouded is a trademark
of Keen Games GmbH. This project is not affiliated with or endorsed by NVIDIA or Keen Games.
