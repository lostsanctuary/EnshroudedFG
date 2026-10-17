# Removes the proxy from the Enshrouded folder.
# Pass -Purge to also delete the EnshroudedFG folder (Streamline DLLs, logs, settings).
param(
    [string]$GameDir = "F:\SteamLibrary\steamapps\common\Enshrouded",
    [switch]$Purge
)
$ErrorActionPreference = "Stop"
if (Get-Process enshrouded -ErrorAction SilentlyContinue) { throw "Close Enshrouded before uninstalling." }
$target = Join-Path $GameDir "winmm.dll"
if (Test-Path $target) { Remove-Item $target -Force; Write-Host "Removed $target" }
if (Test-Path "$target.efg-backup") { Move-Item "$target.efg-backup" $target; Write-Host "Restored original winmm.dll" }
$modDir = Join-Path $GameDir "EnshroudedFG"
if ($Purge -and (Test-Path $modDir)) { Remove-Item $modDir -Recurse -Force; Write-Host "Removed $modDir" }
