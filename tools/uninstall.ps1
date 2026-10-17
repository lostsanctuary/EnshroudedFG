# Removes the proxy from the Enshrouded folder (keeps EnshroudedFG\ logs and settings).
param([string]$GameDir = "F:\SteamLibrary\steamapps\common\Enshrouded")
$ErrorActionPreference = "Stop"
if (Get-Process enshrouded -ErrorAction SilentlyContinue) { throw "Close Enshrouded before uninstalling." }
$target = Join-Path $GameDir "winmm.dll"
if (Test-Path $target) { Remove-Item $target -Force; Write-Host "Removed $target" }
if (Test-Path "$target.efg-backup") { Move-Item "$target.efg-backup" $target; Write-Host "Restored original winmm.dll" }
