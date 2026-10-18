# Fails unless CHANGELOG.md has a "## [<version>]" section with at least one entry.
# Used by .github/workflows/release.yml before a tagged release; run it by hand before tagging.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = "Stop"
$changelog = Join-Path (Split-Path -Parent $PSScriptRoot) "CHANGELOG.md"
if (-not (Test-Path $changelog)) { throw "CHANGELOG.md not found" }

$lines = Get-Content $changelog -Encoding UTF8
$heading = "^## \[$([regex]::Escape($Version))\]"
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match $heading) { $start = $i; break }
}
if ($start -lt 0) {
    throw "CHANGELOG.md has no '## [$Version]' section. Rename [Unreleased] to [$Version] (with today's date) before tagging v$Version."
}

# The section runs to the next "## " heading or the link references at the end.
$entries = 0
for ($i = $start + 1; $i -lt $lines.Count -and $lines[$i] -notmatch '^## '; $i++) {
    if ($lines[$i] -match '^\s*- ') { $entries++ }
}
if ($entries -eq 0) { throw "The '## [$Version]' section in CHANGELOG.md has no entries." }
Write-Host "CHANGELOG.md: [$Version] has $entries entries"
