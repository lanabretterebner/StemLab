$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$Python = Join-Path $Root ".substem-venv\Scripts\python.exe"

if (-not (Test-Path $Python)) {
    throw @"
StemLab's recursive test environment was not found:
  $Python

Create it first with:
  py -3.11 -m venv .substem-venv
  .\.substem-venv\Scripts\Activate.ps1
  python -m pip install "audio-separator[gpu]==0.44.5"
"@
}

Write-Host "Registering the current StemLab source in .substem-venv..."
& $Python -m pip install -e $Root --no-deps
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Checking Recursive Stem Splitting runtime..."
& $Python -c "import torch, audio_separator, stemlab; from stemlab import recursive; print('StemLab', stemlab.__version__); print('Torch', torch.__version__); print('CUDA', torch.cuda.is_available()); print('Recursive Stem Splitting OK')"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$RecursiveExe = Join-Path $Root ".substem-venv\Scripts\stemlab-recursive-job.exe"
if (-not (Test-Path $RecursiveExe)) {
    throw "stemlab-recursive-job.exe was not created: $RecursiveExe"
}

Write-Host ""
Write-Host "Recursive development runtime ready."
Write-Host "  $RecursiveExe"
