# Stage the Fable II Switch port onto an SD card (or a local staging folder).
#
#   tools\stage_sd.ps1 -SdRoot E:\             # copy NRO + content to SD
#   tools\stage_sd.ps1 -Dest C:\stage\fable2   # staging folder instead
#
# Layout produced (see port/src/game_root.h):
#   <root>/switch/Fable II/fable_2.nro
#   <root>/switch/Fable II/fable2_config.toml (created on first launch) -- no
#   <root>/switch/Fable II/Game_files/default.xex, data/, nxeart/, $SystemUpdate/
#
# The game content comes from YOUR disc image extraction
# (fable2-nx\game_files) - it is never committed to the repo.

param(
    [string]$SdRoot = "",
    [string]$Dest = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot   # ...\fable2-nx
$nro = Join-Path $repoRoot "port\build\fable_2.nro"

if (-not (Test-Path $nro)) { Write-Error "fable_2.nro not found: $nro (build first)" }
$target = if ($SdRoot) { $SdRoot } else { $Dest }
if (-not $target) { Write-Error "pass -SdRoot <sd drive> or -Dest <folder>" }

$appDir = Join-Path $target "switch\Fable II"
New-Item -ItemType Directory $appDir -Force | Out-Null

Write-Host "Copying NRO..."
Copy-Item $nro (Join-Path $appDir "fable_2.nro") -Force

$gameFiles = Join-Path $appDir "Game_files"
if (-not (Test-Path $gameFiles)) {
    Write-Host "Copying game content (one-time ~6.5 GB, several minutes)..."
    robocopy (Join-Path $repoRoot "game_files") $gameFiles /E /MT:16 /NFL /NDL /NJH /NJS
    if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)" }
}

Write-Host ""
Write-Host "Staged:"
Write-Host "  $appDir\fable_2.nro"
Write-Host "  $appDir\Game_files\ (...)"

Write-Host ""
Write-Host "NOTE: the extracted XEX is the GOTY USA/Europe payload (hash matches"
Write-Host "catalogue entry goty-us-eu), so the integrity check passes on its own."
Write-Host "The GPU plugin is defaulted by the app; fable_2.toml (flat keys) sets it"
Write-Host "explicitly:"
Write-Host ""
Write-Host "   gpu_plugin = `"xenos`""
Write-Host ""
Write-Host "The XEX hash is logged to $appDir\logs\."
