param(
    [Parameter(Mandatory = $false)]
    [string]$UserLibrary
)

$ErrorActionPreference = "Stop"

if (Get-Process -Name Ableton* -ErrorAction SilentlyContinue) {
    throw "Fully quit Ableton Live before removing the StemLab integration."
}

$Vst = "C:\Program Files\Common Files\VST3\StemLab.vst3"
$EnginePointer = Join-Path $env:LOCALAPPDATA "StemLab\portable_engine_path.txt"

if ([string]::IsNullOrWhiteSpace($UserLibrary)) {
    $Documents = [Environment]::GetFolderPath("MyDocuments")
    $UserLibrary = Join-Path $Documents "Ableton\User Library"
}

$Remote = Join-Path $UserLibrary "Remote Scripts\StemLabRemote"

Remove-Item $Vst -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $Remote -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $EnginePointer -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:TEMP "StemLab") -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "StemLab VST3 and Remote Script removed."
Write-Host "The portable StemLab folder, jobs, recordings, and model caches were left alone."
