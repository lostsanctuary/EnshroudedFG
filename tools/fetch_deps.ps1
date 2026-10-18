# Downloads the NVIDIA files that are not stored in this repository:
#   external\ngx\include\        NGX SDK headers from github.com/NVIDIA/DLSS (pinned commit)
#   external\streamline\bin\x64\ Streamline SDK production DLLs from github.com/NVIDIA-RTX/Streamline
# Both are covered by NVIDIA's licences and may not be redistributed in source form, so each
# developer fetches them. Run once after cloning (and after `git submodule update --init`).
param(
    [switch]$Force,          # re-download even if the files are already there
    [switch]$SkipStreamline  # headers only (the build needs no Streamline DLLs)
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # Invoke-WebRequest is very slow with the progress bar
$root = Split-Path -Parent $PSScriptRoot

# ---- NGX headers ---------------------------------------------------------------------------------
$ngxCommit = "9981c6350dc58c0ce21f024fd65e31e851c31689"  # "DLSS 310.9.16 SDK"
$ngxFiles = @(
    "nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_defs_dlssd.h", "nvsdk_ngx_defs_dlssg.h",
    "nvsdk_ngx_defs_vk.h", "nvsdk_ngx_helpers.h", "nvsdk_ngx_helpers_cuda.h", "nvsdk_ngx_helpers_d3d.h",
    "nvsdk_ngx_helpers_dlssd.h", "nvsdk_ngx_helpers_dlssd_cuda.h", "nvsdk_ngx_helpers_dlssd_d3d.h",
    "nvsdk_ngx_helpers_dlssd_vk.h", "nvsdk_ngx_helpers_dlssg.h", "nvsdk_ngx_helpers_dlssg_d3d.h",
    "nvsdk_ngx_helpers_dlssg_vk.h", "nvsdk_ngx_helpers_vk.h", "nvsdk_ngx_loader.h", "nvsdk_ngx_params.h",
    "nvsdk_ngx_params_dlssd.h", "nvsdk_ngx_params_dlssg.h", "nvsdk_ngx_standalone_common.h",
    "nvsdk_ngx_standalone_cuda.h", "nvsdk_ngx_vk.h")
$ngxDir = Join-Path $root "external\ngx\include"
if ($Force -or -not (Test-Path (Join-Path $ngxDir "nvsdk_ngx_vk.h"))) {
    New-Item -ItemType Directory -Force $ngxDir | Out-Null
    foreach ($f in $ngxFiles) {
        Invoke-WebRequest -UseBasicParsing -OutFile (Join-Path $ngxDir $f) `
            "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/include/$f"
    }
    Write-Host "NGX headers ($($ngxCommit.Substring(0, 7))) -> $ngxDir"
} else {
    Write-Host "NGX headers already present in $ngxDir"
}

# ---- Streamline production DLLs -----------------------------------------------------------------
if ($SkipStreamline) { return }
$slVersion = "2.14.16"
$slDir = Join-Path $root "external\streamline\bin\x64"
$slFiles = @("sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll",
             "nvngx_dlssg.dll", "NvLowLatencyVk.dll", "nvngx_dlss.license.txt", "reflex.license.txt")
$missing = $slFiles | Where-Object { -not (Test-Path (Join-Path $slDir $_)) }
if (-not $Force -and -not $missing) {
    Write-Host "Streamline $slVersion DLLs already present in $slDir"
    return
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) "efg-streamline-$slVersion.zip"
if (-not (Test-Path $tmp)) {
    Write-Host "Downloading Streamline SDK $slVersion (~265 MB)..."
    Invoke-WebRequest -UseBasicParsing -OutFile "$tmp.part" `
        "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$slVersion/streamline-sdk-v$slVersion.zip"
    Move-Item "$tmp.part" $tmp -Force
}

# Only the production files from bin/x64 (not bin/x64/development, which draws a watermark).
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force $slDir | Out-Null
$zip = [IO.Compression.ZipFile]::OpenRead($tmp)
try {
    foreach ($f in $slFiles) {
        $entry = $zip.Entries | Where-Object { $_.FullName -match "(^|/)bin/x64/$([regex]::Escape($f))$" } |
                 Select-Object -First 1
        if (-not $entry) { throw "$f not found in bin/x64 of the Streamline SDK zip" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $slDir $f), $true)
    }
} finally {
    $zip.Dispose()
}

# sl.interposer only loads NVIDIA-signed plugins, so check the signatures here.
Get-ChildItem $slDir -Filter *.dll | ForEach-Object {
    $sig = Get-AuthenticodeSignature $_.FullName
    if ($sig.Status -ne "Valid") { throw "$($_.Name): signature $($sig.Status)" }
}
Remove-Item $tmp -Force
Write-Host "Streamline $slVersion production DLLs -> $slDir"
