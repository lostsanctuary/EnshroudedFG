# Copies the built proxy into the Enshrouded folder. Run uninstall.ps1 to undo.
param(
    [string]$GameDir = "F:\SteamLibrary\steamapps\common\Enshrouded",
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root "build\$Config\winmm.dll"
if (-not (Test-Path $dll)) { throw "Build output not found: $dll" }
if (-not (Test-Path (Join-Path $GameDir "enshrouded.exe"))) { throw "enshrouded.exe not found in $GameDir" }
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
