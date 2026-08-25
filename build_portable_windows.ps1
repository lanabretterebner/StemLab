param(
    [string]$EnvironmentPath = ".venv",
    [string]$OutputDirectory = "",
    [string]$FfmpegPath = "",
    [bool]$DownloadModels = $true,
    [switch]$SkipPluginBuild,
    [switch]$SkipTests,
    [switch]$CleanPlugin
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$DistRoot = Join-Path $RepoRoot "dist"
$CacheRoot = Join-Path $RepoRoot ".portable-cache"
$Manifest = Join-Path $RepoRoot "packaging\models.json"
$StageScript = Join-Path $RepoRoot "scripts\stage_models.py"

$VersionMatch = Select-String -LiteralPath (Join-Path $RepoRoot "pyproject.toml") -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1
if (-not $VersionMatch) { throw "Could not read FI-STEM version from pyproject.toml." }
$Version = $VersionMatch.Matches[0].Groups[1].Value

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $DistRoot "FI-STEM-Portable-$Version"
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

Assert-File $DevPython "FI-STEM's Python environment is missing. Run .\scripts\setup_dev.ps1 first."
Assert-Directory $VenvSitePackages "FI-STEM's Python site-packages directory is missing."
Assert-File $Manifest "The release model manifest is missing."
Assert-File $StageScript "The model staging helper is missing."

if (-not $SkipTests) {
    Write-Host "Running the Python regression suite..." -ForegroundColor Cyan
    & $DevPython -m pytest -q $RepoRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $SkipPluginBuild) {
    Write-Host "Building the Standalone and VST3 targets..." -ForegroundColor Cyan
    $BuildArgs = @{}
    if ($CleanPlugin) { $BuildArgs.Clean = $true }
    & (Join-Path $RepoRoot "scripts\build_plugin.ps1") @BuildArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$Standalone = Join-Path $RepoRoot "plugin\build\FIStemPlugin_artefacts\Release\Standalone\FI-STEM.exe"
$Vst3 = Join-Path $RepoRoot "plugin\build\FIStemPlugin_artefacts\Release\VST3\FI-STEM.vst3"
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

# Development installs are editable and point back into the source checkout.
# Remove only the legacy editable bootstrap, then copy the real package tree.
Get-ChildItem -LiteralPath $EngineSitePackages -Force -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -eq "stemlab" -or
    $_.Name -like "__editable__*stemlab*" -or
    $_.Name -like "stemlab_open-*.dist-info" -or
    $_.Name -like "fi_stem_open-*.dist-info"
} | Remove-Item -Recurse -Force
Invoke-Robocopy (Join-Path $RepoRoot "stemlab") (Join-Path $EngineSitePackages "stemlab") @("/XD", "__pycache__")

Copy-Item -LiteralPath $FfmpegPath -Destination (Join-Path $Engine "ffmpeg.exe") -Force
$FfmpegInfo = & $FfmpegPath -version 2>&1 | Select-Object -First 8
Set-Content -LiteralPath (Join-Path $OutputDirectory "FFMPEG_BUILD_INFO.txt") -Encoding UTF8 -Value @(
    "This is the ffmpeg executable copied into this FI-STEM build.",
    "Verify the redistribution terms for this exact build before publishing it.",
    "",
    $FfmpegInfo
)

$PersistentModelEngine = Join-Path $CacheRoot "release-model-engine"
$SourceRoots = @(
    $PersistentModelEngine,
    (Join-Path $env:LOCALAPPDATA "FI-STEM\Models"),
    (Join-Path $env:LOCALAPPDATA "StemLab\Models"), # legacy cache fallback
    (Join-Path $env:USERPROFILE ".cache\bs-roformer-infer"),
    (Join-Path $env:USERPROFILE ".cache\torch\hub\checkpoints"),
    (Join-Path $env:USERPROFILE ".cache\audio-separator")
)
if ($env:BS_ROFORMER_MODELS_PATH) { $SourceRoots += $env:BS_ROFORMER_MODELS_PATH }
if ($env:STEMLAB_RECURSIVE_MODEL_DIR) { $SourceRoots += $env:STEMLAB_RECURSIVE_MODEL_DIR }
$SourceRoots = $SourceRoots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique

if ($DownloadModels) {
    # Download/copy once into a persistent checksum-verified cache. dist/ is
    # intentionally disposable, so downloading directly into it would force
    # multi-gigabyte model downloads on every rebuild.
    $CacheStageArgs = @("--manifest", $Manifest, "--engine", $PersistentModelEngine)
    foreach ($Root in $SourceRoots) {
        if ([System.IO.Path]::GetFullPath($Root) -ne [System.IO.Path]::GetFullPath($PersistentModelEngine)) {
            $CacheStageArgs += @("--source-root", $Root)
        }
    }
    $CacheStageArgs += "--download-missing"

    Write-Host "Updating the persistent verified model cache..." -ForegroundColor Cyan
    & $DevPython $StageScript @CacheStageArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if (-not ($SourceRoots -contains $PersistentModelEngine)) {
        $SourceRoots = @($PersistentModelEngine) + $SourceRoots
    }
}

$StageArgs = @("--manifest", $Manifest, "--engine", $Engine)
foreach ($Root in $SourceRoots) { $StageArgs += @("--source-root", $Root) }

Write-Host "Staging and verifying release model assets..." -ForegroundColor Cyan
& $DevPython $StageScript @StageArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Copying application and Ableton integration..." -ForegroundColor Cyan
Copy-Item -LiteralPath $Standalone -Destination (Join-Path $OutputDirectory "FI-STEM.exe") -Force
Invoke-Robocopy $Vst3 (Join-Path $OutputDirectory "FI-STEM.vst3")
Invoke-Robocopy (Join-Path $RepoRoot "integrations\ableton\FIStemRemote") (Join-Path $OutputDirectory "FIStemRemote") @("/XD", "__pycache__")
New-Item -ItemType Directory -Path (Join-Path $OutputDirectory "scripts") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot "scripts\install_ableton.ps1") -Destination (Join-Path $OutputDirectory "scripts\install_ableton.ps1") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "LICENSE") -Destination $OutputDirectory -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "plugin\Resources\FIStemIcon.ico") -Destination (Join-Path $OutputDirectory "FIStemIcon.ico") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "docs\third-party.md") -Destination (Join-Path $OutputDirectory "THIRD_PARTY.md") -Force
Copy-Item -LiteralPath $Manifest -Destination (Join-Path $OutputDirectory "models.json") -Force

$EnginePython = Join-Path $Engine "python.exe"
Assert-File $EnginePython "The embedded Python runtime was not assembled correctly."

Write-Host "Verifying isolated portable imports..." -ForegroundColor Cyan
Push-Location $OutputDirectory
try {
    & $EnginePython -c "import stemlab, torch, beat_this, demucs, audio_separator, mido; print('Portable imports OK; CUDA:', torch.cuda.is_available())"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $env:STEMLAB_ENGINE_DIR = $Engine
    try {
        & $EnginePython (Join-Path $RepoRoot "scripts\smoke_portable.py")
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
Write-Host "FI-STEM portable build complete." -ForegroundColor Green
Write-Host "  $OutputDirectory"
Write-Host "Standalone: $(Join-Path $OutputDirectory 'FI-STEM.exe')"
Write-Host "Ableton installer: $(Join-Path $OutputDirectory 'scripts\install_ableton.ps1')"
