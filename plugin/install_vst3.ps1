$ErrorActionPreference = "Stop"

$source = Join-Path $PSScriptRoot "build\StemLabPlugin_artefacts\Release\VST3\StemLab.vst3"
$dest = "C:\Program Files\Common Files\VST3\StemLab.vst3"

if (-not (Test-Path $source)) {
    throw "Build StemLab first. Could not find: $source"
}

Write-Host "Stopping any standalone StemLab process..."
Stop-Process -Name StemLab -Force -ErrorAction SilentlyContinue

if (Test-Path $dest) {
    Write-Host "Removing previously installed StemLab.vst3..."
    Remove-Item $dest -Recurse -Force
}

Write-Host "Installing fresh StemLab.vst3..."
Copy-Item $source $dest -Recurse -Force

$module = Join-Path $dest "Contents\x86_64-win\StemLab.vst3"

if (-not (Test-Path $module)) {
    throw "VST3 install verification failed. Missing module: $module"
}

Write-Host ""
Write-Host "Installed StemLab VST3 0.9.9:"
Write-Host "  $dest"
Write-Host ""
Write-Host "Verified module:"
Write-Host "  $module"
Write-Host ""
Write-Host "Restart Ableton and rescan VST3 plug-ins."
