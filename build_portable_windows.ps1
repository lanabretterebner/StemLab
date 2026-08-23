param(
    [string]$PythonVersion = "3.11.9",
    [string]$TorchVersion = "2.12.1+cu126",
    [string]$TorchIndexUrl = "https://download.pytorch.org/whl/cu126",
    [string]$FfmpegPath,
    [switch]$SkipCppBuild,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root = $PSScriptRoot
$PluginRoot = Join-Path $Root "plugin"
$BuildRoot = Join-Path $Root "dist"
$ReleaseName = "StemLab-0.9.9-Windows"
$Release = Join-Path $BuildRoot $ReleaseName
$Engine = Join-Path $Release "Engine"
$Cache = Join-Path $Root ".portable-cache"

# ZIP downloads can mark every PowerShell script as coming from the Internet.
# Once this top-level builder is running, unblock the repository scripts so
# nested build/install scripts do not trigger a second security prompt.
Get-ChildItem -Path $Root -Recurse -File -Filter "*.ps1" -ErrorAction SilentlyContinue |
    Unblock-File -ErrorAction SilentlyContinue

function Assert-Exists([string]$Path, [string]$Message) {
    if (-not (Test-Path $Path)) {
        throw "$Message`nMissing: $Path"
    }
}

Write-Host "==============================================="
Write-Host " StemLab 0.9.9 portable Windows release build"
Write-Host "==============================================="
Write-Host ""

if (-not $SkipCppBuild) {
    Write-Host "Building JUCE Standalone + VST3..."
    & (Join-Path $PluginRoot "build_windows.ps1")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$Standalone = Join-Path $PluginRoot "build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
$Vst3 = Join-Path $PluginRoot "build\StemLabPlugin_artefacts\Release\VST3\StemLab.vst3"

Assert-Exists $Standalone "Standalone build not found."
Assert-Exists $Vst3 "VST3 build not found."

Write-Host "Creating clean release tree..."
Remove-Item $Release -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Release -Force | Out-Null
New-Item -ItemType Directory -Path $Engine -Force | Out-Null
New-Item -ItemType Directory -Path $Cache -Force | Out-Null

Copy-Item $Standalone (Join-Path $Release "StemLab.exe") -Force
Copy-Item $Vst3 (Join-Path $Release "StemLab.vst3") -Recurse -Force
Copy-Item (Join-Path $Root "ableton_remote\StemLabRemote") (Join-Path $Release "StemLabRemote") -Recurse -Force
Copy-Item (Join-Path $Root "README.md") $Release -Force
Copy-Item (Join-Path $Root "LICENSE") $Release -Force
Copy-Item (Join-Path $Root "THIRD_PARTY.md") $Release -Force
Copy-Item (Join-Path $Root "PORTABLE_INSTALL.txt") $Release -Force
Copy-Item (Join-Path $Root "START_HERE.txt") $Release -Force
Copy-Item (Join-Path $Root "ABLETON_QUICKSTART.md") $Release -Force
Copy-Item (Join-Path $Root "install_portable_windows.ps1") $Release -Force
Copy-Item (Join-Path $Root "install_ableton_integration.ps1") $Release -Force
Copy-Item (Join-Path $Root "uninstall_portable_windows.ps1") $Release -Force

# ------------------------------------------------------------------
# Embedded Python
# ------------------------------------------------------------------
$PyTag = ($PythonVersion -split '\.')[0..1] -join ''
$PythonZip = Join-Path $Cache "python-$PythonVersion-embed-amd64.zip"
$PythonUrl = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"

if (-not (Test-Path $PythonZip)) {
    Write-Host "Downloading embedded Python $PythonVersion..."
    Invoke-WebRequest -Uri $PythonUrl -OutFile $PythonZip
}

Write-Host "Extracting embedded Python..."
Expand-Archive -Path $PythonZip -DestinationPath $Engine -Force

$Pth = Join-Path $Engine "python$PyTag._pth"
Assert-Exists $Pth "Embedded Python ._pth file was not found."

$PthText = Get-Content $Pth -Raw
if ($PthText -notmatch '(?m)^Lib\\site-packages$') {
    $PthText = $PthText.TrimEnd() + "`r`nLib\site-packages`r`n"
}
$PthText = $PthText -replace '(?m)^#import site$', 'import site'
Set-Content -Path $Pth -Value $PthText -Encoding ASCII

$Python = Join-Path $Engine "python.exe"
Assert-Exists $Python "Embedded python.exe was not found."

# ------------------------------------------------------------------
# Bootstrap pip only while assembling the runtime.
# ------------------------------------------------------------------
$GetPip = Join-Path $Cache "get-pip.py"
if (-not (Test-Path $GetPip)) {
    Write-Host "Downloading pip bootstrap..."
    Invoke-WebRequest -Uri "https://bootstrap.pypa.io/get-pip.py" -OutFile $GetPip
}

Write-Host "Bootstrapping release-runtime pip..."
& $Python $GetPip --no-warn-script-location
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installing tested CUDA PyTorch runtime..."
& $Python -m pip install --no-cache-dir --index-url $TorchIndexUrl "torch==$TorchVersion"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installing StemLab runtime dependencies..."
& $Python -m pip install --no-cache-dir `
    "numpy>=1.26" `
    "scipy>=1.12" `
    "soundfile>=0.12" `
    "pyyaml>=6.0" `
    "tqdm>=4.66" `
    "bs-roformer-infer==0.1.5" `
    "demucs==4.1.0"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Copy only StemLab's runtime Python package. No source tests, dev GUI, build
# tree, or editable-install metadata are needed in the release engine.
$SitePackages = Join-Path $Engine "Lib\site-packages"
$StemLabPackage = Join-Path $SitePackages "stemlab"
Remove-Item $StemLabPackage -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $Root "stemlab") $StemLabPackage -Recurse -Force

# ------------------------------------------------------------------
# FFmpeg is required for compressed input and Demucs Windows decoding.
# Copy the exact known-working executable from the build machine.
# ------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
    $FfmpegCommand = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($null -eq $FfmpegCommand) {
        throw @"
FFmpeg was not found on PATH.

Install a redistributable FFmpeg build or pass its full path explicitly:
  .\build_portable_windows.ps1 -FfmpegPath "C:\path\to\ffmpeg.exe"
"@
    }
    $FfmpegPath = $FfmpegCommand.Source
}

$FfmpegPath = [System.IO.Path]::GetFullPath($FfmpegPath)
Assert-Exists $FfmpegPath "FFmpeg executable was not found."

Write-Host "Bundling FFmpeg into the release payload only..."
Write-Host "  $FfmpegPath" -ForegroundColor DarkGray
$BundledFfmpeg = Join-Path $Engine "ffmpeg.exe"
Copy-Item $FfmpegPath $BundledFfmpeg -Force

# Record the exact binary/build configuration being redistributed so release
# licensing can be checked without committing the 100+ MB binary to git.
$FfmpegBuildInfo = Join-Path $Release "FFMPEG_BUILD_INFO.txt"
& $BundledFfmpeg -version 2>&1 | Out-File -FilePath $FfmpegBuildInfo -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    throw "The bundled FFmpeg executable could not be started."
}

# ------------------------------------------------------------------
# Runtime cleanup. pip is a build-time tool and is deliberately not shipped.
# Do not remove *.dist-info: StemLab's relocatable BS-RoFormer launcher uses
# console entry-point metadata from bs-roformer-infer.
# ------------------------------------------------------------------
Write-Host "Removing build-only Python files..."
Get-ChildItem $Engine -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

Get-ChildItem $SitePackages -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(pip|setuptools|wheel)(-|$)' } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

$Scripts = Join-Path $Engine "Scripts"
if (Test-Path $Scripts) {
    Get-ChildItem $Scripts -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(pip|wheel)' } |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

# Keep Python's LICENSE.txt from the embeddable distribution in Engine.

# ------------------------------------------------------------------
# Smoke tests
# ------------------------------------------------------------------
Write-Host "Running portable runtime smoke tests..."
& $Python -c "import torch, numpy, scipy, soundfile, demucs, bs_roformer, stemlab; print('Torch', torch.__version__); print('CUDA build', torch.version.cuda); print('CUDA available', torch.cuda.is_available()); print('GPU', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'NONE'); print('StemLab', stemlab.__version__)"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $Python -m stemlab.plugin_job --help | Out-Null
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $Python -m stemlab.bs_roformer_cli --help | Out-Null
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Bytes = (Get-ChildItem $Release -Recurse -File | Measure-Object Length -Sum).Sum
$GiB = $Bytes / 1GB
$MiB = $Bytes / 1MB

Write-Host ""
Write-Host ("Portable release size: {0:N2} GB ({1:N0} MB)" -f $GiB, $MiB)
Write-Host "Release folder:"
Write-Host "  $Release"

if (-not $NoZip) {
    $Zip = Join-Path $BuildRoot "StemLab-Windows.zip"
    Remove-Item $Zip -Force -ErrorAction SilentlyContinue

    $SevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($null -ne $SevenZip) {
        Write-Host "Compressing with 7-Zip..."
        Push-Location $BuildRoot
        & $SevenZip.Source a -tzip -mx=9 $Zip $ReleaseName | Out-Null
        $Code = $LASTEXITCODE
        Pop-Location
        if ($Code -ne 0) { exit $Code }
    }
    else {
        Write-Host "7-Zip not found; using Compress-Archive..."
        Compress-Archive -Path $Release -DestinationPath $Zip -CompressionLevel Optimal
    }

    $ZipBytes = (Get-Item $Zip).Length
    Write-Host ("ZIP size: {0:N2} GB ({1:N0} MB)" -f ($ZipBytes / 1GB), ($ZipBytes / 1MB))
    Write-Host "ZIP:"
    Write-Host "  $Zip"
}

Write-Host ""
Write-Host "Portable build complete."
Write-Host "Test it on a machine/shell where StemLab is NOT pip-installed."
