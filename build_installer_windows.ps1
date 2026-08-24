param(
    [switch]$SkipPortableBuild,
    [switch]$UseDiskSpanning,
    [string]$FfmpegPath,
    [string]$ReleaseDir,
    [string]$InnoSetupPath
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$Version = "0.9.9"
$Dist = Join-Path $Root "dist"
$BuildLog = Join-Path $Root "StemLab-installer-build.log"

try {
    Start-Transcript -Path $BuildLog -Force | Out-Null
}
catch {
    # Logging should never block the build.
}

if ([string]::IsNullOrWhiteSpace($ReleaseDir)) {
    $ReleaseDir = Join-Path $Dist "StemLab-$Version-Windows"
}

if (-not $SkipPortableBuild) {
    Write-Host "Building the portable StemLab payload first..." -ForegroundColor Cyan

    # Use named-parameter hashtable splatting. Array splatting passes values
    # positionally to PowerShell scripts, which previously bound "-NoZip" to
    # PythonVersion and produced an invalid python.org download URL.
    $portableParams = @{
        NoZip = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegPath)) {
        $portableParams.FfmpegPath = $FfmpegPath
    }

    & (Join-Path $Root "build_portable_windows.ps1") @portableParams
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$required = @(
    (Join-Path $ReleaseDir "StemLab.exe"),
    (Join-Path $ReleaseDir "StemLab.vst3"),
    (Join-Path $ReleaseDir "Engine\python.exe"),
    (Join-Path $ReleaseDir "Engine\ffmpeg.exe"),
    (Join-Path $ReleaseDir "FFMPEG_BUILD_INFO.txt"),
    (Join-Path $ReleaseDir "StemLabRemote\__init__.py")
)

foreach ($path in $required) {
    if (-not (Test-Path $path)) {
        throw "Installer payload is incomplete. Missing: $path"
    }
}

if ([string]::IsNullOrWhiteSpace($InnoSetupPath)) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        $InnoSetupPath = $command.Source
    }
}

if ([string]::IsNullOrWhiteSpace($InnoSetupPath)) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 7\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 7\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 7\ISCC.exe")
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    $InnoSetupPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if ([string]::IsNullOrWhiteSpace($InnoSetupPath) -or -not (Test-Path $InnoSetupPath)) {
    throw @"
Inno Setup was not found.

Install Inno Setup 6.7+ and rerun this script.
With winget:
  winget install JRSoftware.InnoSetup
"@
}

New-Item -ItemType Directory -Path $Dist -Force | Out-Null

$payloadBytes = (Get-ChildItem $ReleaseDir -Recurse -File | Measure-Object Length -Sum).Sum
$payloadGiB = $payloadBytes / 1GB
$spanning = [bool]$UseDiskSpanning

Write-Host ""
Write-Host ("Installer payload before compression: {0:N2} GB" -f $payloadGiB)
if ($spanning) {
    Write-Host "Disk spanning was explicitly enabled. Setup data will be split into .bin slices." -ForegroundColor Yellow
    Write-Host "Keep the generated EXE and BIN files together when distributing." -ForegroundColor Yellow
}
else {
    Write-Host "Targeting one self-contained setup EXE (no .bin file)." -ForegroundColor Green
    Write-Host "The final EXE size will be checked against GitHub's 2 GiB release-asset limit." -ForegroundColor DarkGray
}

$iss = Join-Path $Root "installer\StemLab.iss"
$assetDir = Join-Path $Root "assets"

# Remove stale installer artifacts from a previous spanning/non-spanning build.
# This prevents an old .bin file from being mistaken as part of a new single-EXE release.
Get-ChildItem $Dist -Filter "StemLab-Setup-$Version*" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force

$arguments = @(
    "/DSourceDir=$ReleaseDir",
    "/DOutputDir=$Dist",
    "/DAssetDir=$assetDir",
    "/DAppVersion=$Version"
)

if ($spanning) {
    $arguments += "/DUseDiskSpanning=1"
}

$arguments += $iss

Write-Host ""
Write-Host "Compiling polished StemLab installer..." -ForegroundColor Cyan
Write-Host "Build log: $BuildLog" -ForegroundColor DarkGray
& $InnoSetupPath @arguments
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Inno Setup failed with exit code $LASTEXITCODE." -ForegroundColor Red
    Write-Host "The full build log is here:" -ForegroundColor Yellow
    Write-Host "  $BuildLog" -ForegroundColor Yellow
    try { Stop-Transcript | Out-Null } catch {}
    exit $LASTEXITCODE
}

$setupExe = Join-Path $Dist "StemLab-Setup-$Version.exe"
if (-not (Test-Path $setupExe)) {
    throw "Inno Setup finished, but the expected installer was not found: $setupExe"
}

Write-Host ""
Write-Host "Installer build complete." -ForegroundColor Green
Write-Host "  $setupExe"

if ($spanning) {
    Get-ChildItem $Dist -Filter "StemLab-Setup-$Version*.bin" |
        ForEach-Object { Write-Host "  $($_.FullName)" }
}
else {
    $setupBytes = (Get-Item $setupExe).Length
    $setupGiB = $setupBytes / 1GB
    $githubAssetLimit = 2GB

    Write-Host ("Single-file installer size: {0:N2} GiB" -f $setupGiB)
    if ($setupBytes -le $githubAssetLimit) {
        Write-Host "Ready for GitHub Releases as one downloadable EXE." -ForegroundColor Green
    }
    else {
        Write-Host "WARNING: The EXE is larger than GitHub's 2 GiB per-asset limit." -ForegroundColor Yellow
        Write-Host "Rebuild with -UseDiskSpanning if you need GitHub-hosted release assets." -ForegroundColor Yellow
    }
}

try { Stop-Transcript | Out-Null } catch {}
