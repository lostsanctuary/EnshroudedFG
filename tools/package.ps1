# Builds a release zip: dist\EnshroudedFG-<version>.zip, laid out like the game folder.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$versionLine = Select-String -Path (Join-Path $root "src\version.h") -Pattern 'EFG_VERSION_STRING "([^"]+)"'
if (-not $versionLine) { throw "Version not found in src\version.h" }
$version = $versionLine.Matches[0].Groups[1].Value

$dll = Join-Path $root "build\$Config\winmm.dll"
$slBin = Join-Path $root "external\streamline\bin\x64"
foreach ($p in @($dll, (Join-Path $slBin "sl.interposer.dll"))) {
    if (-not (Test-Path $p)) { throw "Missing $p - build first and fetch the Streamline SDK binaries" }
}

$stage = Join-Path $root "dist\EnshroudedFG-$version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$slDst = Join-Path $stage "EnshroudedFG\streamline"
$licDst = Join-Path $slDst "licenses"
New-Item -ItemType Directory -Force $licDst | Out-Null

Copy-Item $dll $stage
Copy-Item (Join-Path $root "tools\README.txt") $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $licDst "EnshroudedFG-LICENSE.txt")
foreach ($f in "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll",
               "nvngx_dlssg.dll", "NvLowLatencyVk.dll") {
    Copy-Item (Join-Path $slBin $f) $slDst
}
Copy-Item (Join-Path $root "external\streamline\license.txt") (Join-Path $licDst "Streamline-license.txt")
foreach ($f in "nvngx_dlss.license.txt", "reflex.license.txt") {
    if (Test-Path (Join-Path $slBin $f)) { Copy-Item (Join-Path $slBin $f) $licDst }
}
$third = @("MinHook - https://github.com/TsudaKageyu/minhook", "")
$third += Get-Content (Join-Path $root "external\minhook\LICENSE.txt")
$third += @("", "", "Dear ImGui - https://github.com/ocornut/imgui", "")
$third += Get-Content (Join-Path $root "external\imgui\LICENSE.txt")
Set-Content -Path (Join-Path $licDst "THIRD_PARTY.txt") -Value $third -Encoding UTF8

# Every Streamline/NVIDIA binary must keep its NVIDIA signature.
Get-ChildItem $slDst -Filter *.dll | ForEach-Object {
    $sig = Get-AuthenticodeSignature $_.FullName
    if ($sig.Status -ne "Valid") { throw "$($_.Name): signature $($sig.Status)" }
}

$zip = Join-Path $root "dist\EnshroudedFG-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Compress-Archive in Windows PowerShell 5.1 stores paths with backslashes, which the zip format
# doesn't allow and some unzip tools mishandle. Write every entry with forward slashes instead.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $_.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}
Write-Host "Packaged $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($stage.Length + 1) }
