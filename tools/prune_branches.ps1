# Deletes local branches whose GitHub branch is gone - usually because their pull request merged
# and GitHub deleted the head branch. Branches that were never pushed, or whose GitHub branch still
# exists, are left alone, and so is the branch you're on.
#   powershell -ExecutionPolicy Bypass -File tools\prune_branches.ps1 -DryRun   # list only
#   powershell -ExecutionPolicy Bypass -File tools\prune_branches.ps1           # delete
param([switch]$DryRun)
$ErrorActionPreference = "Stop"

git fetch --prune --quiet origin
if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }

$current = git branch --show-current
$gone = git for-each-ref --format="%(refname:short)|%(upstream:track)" refs/heads |
    Where-Object { $_ -match '^(.+)\|\[gone\]$' } | ForEach-Object { $Matches[1] }

if (-not $gone) { Write-Host "No local branches with a deleted GitHub branch."; return }
foreach ($branch in $gone) {
    if ($branch -eq $current) {
        Write-Warning "Skipping ${branch}: it's checked out. Switch to main first."
        continue
    }
    if ($DryRun) { Write-Host "Would delete $branch"; continue }
    # -D, not -d: squash merges leave the branch's own commits out of main's history.
    git branch -D $branch
    if ($LASTEXITCODE -ne 0) { throw "Couldn't delete $branch" }
}
