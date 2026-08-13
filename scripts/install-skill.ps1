# One-click install of the qt-commander-ui skill to the user-level skills
# directory (~/.claude/skills).  The skill is picked up by every Claude
# Code session of this user -- the same scope as the qt-commander MCP
# server (configured at user level in ~/.claude.json).  Run once after
# cloning/distributing this repo:
#
#   powershell -ExecutionPolicy Bypass -File scripts\install-skill.ps1
#
# Optional: -Link creates a directory junction to the repo instead of a
# copy, so edits in the repo are picked up without re-running the script
# (the junction breaks if the repo is moved/deleted).
param(
    [switch]$Link
)

$ErrorActionPreference = "Stop"

$repoSkill = Join-Path $PSScriptRoot "..\.claude\skills\qt-commander-ui"
$repoSkill = (Resolve-Path $repoSkill).Path
$userSkills = Join-Path $HOME ".claude\skills"
$target = Join-Path $userSkills "qt-commander-ui"

if (-not (Test-Path $repoSkill)) {
    Write-Error "Source skill not found: $repoSkill"
    exit 1
}

# Create the user skills dir if missing.
if (-not (Test-Path $userSkills)) {
    New-Item -ItemType Directory -Force $userSkills | Out-Null
}

# Remove any previous install (copy or stale junction).
if (Test-Path $target) {
    Remove-Item -Recurse -Force $target
}

if ($Link) {
    New-Item -ItemType Junction -Path $target -Target $repoSkill | Out-Null
    Write-Host "Linked  $target  ->  $repoSkill"
} else {
    Copy-Item -Recurse $repoSkill $target
    Write-Host "Copied  $repoSkill  ->  $target"
}

Write-Host "qt-commander-ui skill installed. It is now available to all"
Write-Host "Claude Code sessions of this user (same scope as the MCP server)."
