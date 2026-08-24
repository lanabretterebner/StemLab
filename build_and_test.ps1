$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

Set-Location $Root
Write-Host "Preparing recursive/adaptive development runtime..."
& .\setup_recursive_dev.ps1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Python = Join-Path $Root ".substem-venv\Scripts\python.exe"
if (-not (Test-Path $Python)) {
    $Python = Join-Path $Root ".venv\Scripts\python.exe"
}
if (-not (Test-Path $Python)) {
    throw "Could not find StemLab development Python."
}

Write-Host ""
Write-Host "Running adaptive tree smoke tests..."
& $Python -m pytest tests\test_adaptive_tree.py -q
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Building JUCE plugin..."
Set-Location (Join-Path $Root "plugin")
& .\build_windows.ps1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Adaptive Tree test/build completed."
Write-Host "Open plugin\build in Visual Studio for hands-on C++/UI work."
