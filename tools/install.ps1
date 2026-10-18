# Copies the built proxy into the Enshrouded folder. Run uninstall.ps1 to undo.
param(
    [string]$GameDir = "",  # default: found through Steam
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_game.ps1")
$GameDir = Resolve-GameDir $GameDir
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root "build\$Config\winmm.dll"
if (-not (Test-Path $dll)) { throw "Build output not found: $dll" }
if (Get-Process enshrouded -ErrorAction SilentlyContinue) { throw "Close Enshrouded before installing." }

$target = Join-Path $GameDir "winmm.dll"
if (Test-Path $target) {
    $info = (Get-Item $target).VersionInfo
    if ($info.CompanyName -match "Microsoft") {
        Move-Item $target "$target.efg-backup" -Force
        Write-Host "Backed up existing winmm.dll to winmm.dll.efg-backup"
    }
}
New-Item -ItemType Directory -Force (Join-Path $GameDir "EnshroudedFG") | Out-Null
Copy-Item $dll $target -Force
Write-Host "Installed $dll -> $target"

# Streamline production DLLs (DLSS-G, Reflex, PCL) go in EnshroudedFG\streamline
$slSrc = Join-Path $root "external\streamline\bin\x64"
$slDst = Join-Path $GameDir "EnshroudedFG\streamline"
if (Test-Path (Join-Path $slSrc "sl.interposer.dll")) {
    New-Item -ItemType Directory -Force $slDst | Out-Null
    Copy-Item (Join-Path $slSrc "*") $slDst -Force
    Write-Host "Installed Streamline DLLs -> $slDst"
} else {
    Write-Warning "Streamline DLLs not found in $slSrc - the mod will run without frame generation"
}
