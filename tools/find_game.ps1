# Dot-sourced by install.ps1 / uninstall.ps1: finds the Enshrouded folder through Steam.
function Find-EnshroudedDir {
    $appId = "1203620"
    $steam = $null
    foreach ($key in "HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam") {
        $p = Get-ItemProperty $key -ErrorAction SilentlyContinue
        if ($p.SteamPath) { $steam = $p.SteamPath; break }
        if ($p.InstallPath) { $steam = $p.InstallPath; break }
    }
    if (-not $steam) { return $null }

    # Every Steam library is listed in libraryfolders.vdf as "path"  "D:\\SteamLibrary".
    $libraries = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libraries += $m.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libraries | Select-Object -Unique) {
        $manifest = Join-Path $lib "steamapps\appmanifest_$appId.acf"
        if (-not (Test-Path $manifest)) { continue }
        $installDir = "Enshrouded"
        $m = [regex]::Match((Get-Content $manifest -Raw), '"installdir"\s+"([^"]+)"')
        if ($m.Success) { $installDir = $m.Groups[1].Value }
        $dir = Join-Path $lib "steamapps\common\$installDir"
        if (Test-Path (Join-Path $dir "enshrouded.exe")) { return $dir }
    }
    return $null
}

function Resolve-GameDir([string]$GameDir) {
    if (-not $GameDir) {
        $GameDir = Find-EnshroudedDir
        if (-not $GameDir) { throw "Enshrouded not found in your Steam libraries - pass -GameDir <folder with enshrouded.exe>" }
        Write-Host "Found Enshrouded in $GameDir"
    }
    if (-not (Test-Path (Join-Path $GameDir "enshrouded.exe"))) { throw "enshrouded.exe not found in $GameDir" }
    return $GameDir
}
