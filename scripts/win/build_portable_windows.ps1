param(
    [string]$EnvironmentPath = ".venv",
    [string]$OutputDirectory = "",
    [string]$FfmpegPath = "",
    [switch]$SkipPluginBuild,
    [switch]$SkipTests,
    [switch]$CleanPlugin
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$DistRoot = Join-Path $RepoRoot "dist"
$CacheRoot = Join-Path $RepoRoot ".portable-cache"

$VersionMatch = Select-String -LiteralPath (Join-Path $RepoRoot "pyproject.toml") -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1
if (-not $VersionMatch) { throw "Could not read StemLab version from pyproject.toml." }
$Version = $VersionMatch.Matches[0].Groups[1].Value

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $DistRoot "StemLab-Portable-$Version"
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepoRoot $OutputDirectory
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

$DistPrefix = [System.IO.Path]::GetFullPath($DistRoot).TrimEnd('\') + '\'
if (-not (($OutputDirectory.TrimEnd('\') + '\').StartsWith($DistPrefix, [StringComparison]::OrdinalIgnoreCase))) {
    throw "Portable output must be under the repository dist directory: $DistRoot"
}

if ([System.IO.Path]::IsPathRooted($EnvironmentPath)) {
    $Environment = [System.IO.Path]::GetFullPath($EnvironmentPath)
}
else {
    $Environment = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $EnvironmentPath))
}
$DevPython = Join-Path $Environment "Scripts\python.exe"
$VenvSitePackages = Join-Path $Environment "Lib\site-packages"

function Assert-File([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Message`nMissing: $Path"
    }
}

function Assert-Directory([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Message`nMissing: $Path"
    }
}

function Invoke-Robocopy([string]$Source, [string]$Destination, [string[]]$ExtraArgs = @()) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $Arguments = @($Source, $Destination, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP") + $ExtraArgs
    & robocopy.exe @Arguments | Out-Host
    $Code = $LASTEXITCODE
    if ($Code -ge 8) {
        throw "Robocopy failed with exit code $Code while copying $Source"
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Download-VerifiedFile(
    [string]$Uri,
    [string]$Destination,
    [string]$Sha256
) {
    New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force | Out-Null
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        if ((Get-Sha256 $Destination) -eq $Sha256.ToLowerInvariant()) { return }
        Remove-Item -LiteralPath $Destination -Force
    }

    $Partial = "$Destination.partial"
    Remove-Item -LiteralPath $Partial -Force -ErrorAction SilentlyContinue
    try {
        Write-Host "Downloading pinned CPython runtime..."
        Invoke-WebRequest -Uri $Uri -OutFile $Partial -UseBasicParsing
        $Actual = Get-Sha256 $Partial
        if ($Actual -ne $Sha256.ToLowerInvariant()) {
            throw "SHA-256 mismatch for $Uri`nExpected: $Sha256`nActual:   $Actual"
        }
        Move-Item -LiteralPath $Partial -Destination $Destination -Force
    }
    catch {
        Remove-Item -LiteralPath $Partial -Force -ErrorAction SilentlyContinue
        throw
    }
}

Assert-File $DevPython "StemLab's Python environment is missing. Run .\scripts\win\setup_dev.ps1 first."
Assert-Directory $VenvSitePackages "StemLab's Python site-packages directory is missing."

if (-not $SkipTests) {
    Write-Host "Running the Python regression suite..." -ForegroundColor Cyan
    & $DevPython -m pytest -q $RepoRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $SkipPluginBuild) {
    Write-Host "Building the Standalone and VST3 targets..." -ForegroundColor Cyan
    $BuildArgs = @{}
    if ($CleanPlugin) { $BuildArgs.Clean = $true }
    & (Join-Path $PSScriptRoot "build_plugin.ps1") @BuildArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$Standalone = Join-Path $RepoRoot "src\plugin\build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
$Vst3 = Join-Path $RepoRoot "src\plugin\build\StemLabPlugin_artefacts\Release\VST3\StemLab.vst3"
Assert-File $Standalone "The Standalone build is missing."
Assert-Directory $Vst3 "The VST3 build is missing."

if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
    $FfmpegCommand = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if (-not $FfmpegCommand) { $FfmpegCommand = Get-Command ffmpeg -ErrorAction SilentlyContinue }
    if ($FfmpegCommand) { $FfmpegPath = $FfmpegCommand.Source }
}
Assert-File $FfmpegPath "FFmpeg was not found. Put ffmpeg.exe on PATH or pass -FfmpegPath C:\path\to\ffmpeg.exe."
$FfmpegPath = [System.IO.Path]::GetFullPath($FfmpegPath)

New-Item -ItemType Directory -Path $DistRoot -Force | Out-Null
if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$Engine = Join-Path $OutputDirectory "Engine"
New-Item -ItemType Directory -Path $Engine -Force | Out-Null

$PythonVersion = "3.11.9"
$PythonZipName = "python-$PythonVersion-embed-amd64.zip"
$PythonZip = Join-Path $CacheRoot "python\$PythonZipName"
$PythonUrl = "https://www.python.org/ftp/python/$PythonVersion/$PythonZipName"
$PythonSha256 = "009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b"
Download-VerifiedFile $PythonUrl $PythonZip $PythonSha256

Write-Host "Assembling the portable Python engine..." -ForegroundColor Cyan
Expand-Archive -LiteralPath $PythonZip -DestinationPath $Engine -Force
Set-Content -LiteralPath (Join-Path $Engine "python311._pth") -Encoding ASCII -Value @(
    "python311.zip",
    ".",
    "Lib\site-packages",
    "import site"
)

$EngineSitePackages = Join-Path $Engine "Lib\site-packages"
Invoke-Robocopy $VenvSitePackages $EngineSitePackages @("/XD", "__pycache__")

# Some wheels install runtime DLLs beside site-packages rather than inside
# it: Intel's sycl/opencl runtimes (dependencies of xpu torch) land in the
# venv's Library\bin via the wheel data scheme, and torch's own DLL loader
# searches sys.exec_prefix\Library\bin for them. Copying site-packages
# alone ships c10_xpu.dll without the DLLs it links against, and the Engine
# dies on import with WinError 126. Absent for flavors that install no such
# wheels, hence the guard.
$VenvLibrary = Join-Path $Environment "Library"
if (Test-Path -LiteralPath $VenvLibrary -PathType Container) {
    Write-Host "Copying runtime DLLs from the environment's Library directory..."
    Invoke-Robocopy $VenvLibrary (Join-Path $Engine "Library")
}

# Development installs are editable and point back into the source checkout.
# Remove only the legacy editable bootstrap, then copy the real package tree.
Get-ChildItem -LiteralPath $EngineSitePackages -Force -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -eq "stemlab" -or
    $_.Name -like "__editable__*stemlab*" -or
    $_.Name -like "stemlab_open-*.dist-info" -or
    $_.Name -like "fi_stem_open-*.dist-info"
} | Remove-Item -Recurse -Force
Invoke-Robocopy (Join-Path $RepoRoot "src\stemlab") (Join-Path $EngineSitePackages "stemlab") @("/XD", "__pycache__")

Copy-Item -LiteralPath $FfmpegPath -Destination (Join-Path $Engine "ffmpeg.exe") -Force
$FfmpegInfo = & $FfmpegPath -version 2>&1 | Select-Object -First 8
Set-Content -LiteralPath (Join-Path $OutputDirectory "FFMPEG_BUILD_INFO.txt") -Encoding UTF8 -Value @(
    "This is the ffmpeg executable copied into this StemLab build.",
    "Verify the redistribution terms for this exact build before publishing it.",
    "",
    $FfmpegInfo
)

# Model weights are not staged into the bundle. Each downloads the first
# time its model is used, is verified against the digest recorded beside
# it, and the plugin names the download in its status area while it runs
# - the same as the Linux bundle. Staging made a release depend on
# whatever the build machine happened to have cached, which is what hid
# two dead download URLs for as long as that cache stayed warm.

Write-Host "Copying application and Ableton integration..." -ForegroundColor Cyan
Copy-Item -LiteralPath $Standalone -Destination (Join-Path $OutputDirectory "StemLab.exe") -Force
Invoke-Robocopy $Vst3 (Join-Path $OutputDirectory "StemLab.vst3")
Invoke-Robocopy (Join-Path $RepoRoot "src\integrations\ableton\StemLabRemote") (Join-Path $OutputDirectory "StemLabRemote") @("/XD", "__pycache__")
New-Item -ItemType Directory -Path (Join-Path $OutputDirectory "scripts") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "install_ableton.ps1") -Destination (Join-Path $OutputDirectory "scripts\install_ableton.ps1") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "LICENSE") -Destination $OutputDirectory -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "docs\third-party.md") -Destination (Join-Path $OutputDirectory "THIRD_PARTY.md") -Force
# The installer definition's SetupIconFile reads this out of the payload.
Copy-Item -LiteralPath (Join-Path $RepoRoot "src\plugin\Resources\StemLabIcon.ico") -Destination (Join-Path $OutputDirectory "StemLabIcon.ico") -Force

$EnginePython = Join-Path $Engine "python.exe"
Assert-File $EnginePython "The embedded Python runtime was not assembled correctly."

Write-Host "Verifying isolated portable imports..." -ForegroundColor Cyan
Push-Location $OutputDirectory
try {
    & $EnginePython -c "import stemlab, torch, beat_this, demucs, audio_separator, mido; print('Portable imports OK; CUDA:', torch.cuda.is_available())"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Ask torch what it is rather than being told. A wheel tags itself -
    # 2.9.1+cu128, 2.9.1+cpu, 2.9.1+xpu - so this cannot drift from the
    # payload the way a build label passed down the chain can. The Linux
    # bundle records the same marker from install_backend.sh.
    $TorchBuild = & $EnginePython -c "import torch; print(torch.__version__)"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Deliberately unanchored: a local version can carry a suffix
    # (2.9.1+cu128.post1), and anchoring would drop such a build through to
    # the default and label a CUDA installer "cpu" - the exact mislabel this
    # is here to prevent.
    $Flavor = switch -Regex ($TorchBuild) {
        '\+cu\d+'   { "cuda"; break }
        '\+xpu'     { "xpu";  break }
        '\+rocm'    { "rocm"; break }
        default     { "cpu" }
    }

    Set-Content -LiteralPath (Join-Path $Engine ".stemlab-torch-flavor") `
        -Encoding ASCII -Value $Flavor
    Write-Host "Engine torch build: $TorchBuild (flavor: $Flavor)" -ForegroundColor Cyan

    $env:STEMLAB_ENGINE_DIR = $Engine
    try {
        & $EnginePython (Join-Path $PSScriptRoot "smoke_portable.py")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    finally {
        Remove-Item Env:STEMLAB_ENGINE_DIR -ErrorAction SilentlyContinue
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "StemLab portable build complete." -ForegroundColor Green
Write-Host "  $OutputDirectory"
Write-Host "Standalone: $(Join-Path $OutputDirectory 'StemLab.exe')"
Write-Host "Ableton installer: $(Join-Path $OutputDirectory 'scripts\install_ableton.ps1')"
