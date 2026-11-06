# Refresh the staged NRO (and the config) in H:\Projects\Fable2Port\stage
# without touching the 6.6 GB Game_files copy.
$ErrorActionPreference = "Stop"
$repo = "H:\Projects\Fable2Port\fable2-nx"
$dest = "H:\Projects\Fable2Port\stage\switch\Fable II"

$nro = Join-Path $repo "port\build\fable_2.nro"
if (-not (Test-Path $nro)) { throw "missing $nro" }
Copy-Item $nro (Join-Path $dest "fable_2.nro") -Force

$nroInfo = Get-Item $nro
Write-Host ("staged: {0} ({1:N1} MB)" -f (Join-Path $dest "fable_2.nro"), ($nroInfo.Length / 1MB))
