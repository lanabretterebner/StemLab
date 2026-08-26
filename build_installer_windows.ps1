param(
    [ValidateSet("nvidia", "cpu", "amd")]
    [string]$Backend = "nvidia",
    [string]$EnvironmentPath = "",
    [string]$FfmpegPath = "",
    [bool]$DownloadModels = $true,
    [switch]$SkipPortableBuild,
    [switch]$SkipPluginBuild,
    [switch]$SkipTests,
    [switch]$CleanPlugin
)

$ErrorActionPreference = "Stop"
$RepoRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
. (Join-Path $RepoRoot "scripts\windows_backend.ps1")
$BackendConfiguration = Get-FIStemBackendConfiguration $Backend
$DistRoot = Join-Path $RepoRoot "dist"

$VersionMatch = Select-String -LiteralPath (Join-Path $RepoRoot "pyproject.toml") -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1
if (-not $VersionMatch) { throw "Could not read FI-STEM version from pyproject.toml." }
$Version = $VersionMatch.Matches[0].Groups[1].Value
$PortableRoot = Join-Path $DistRoot "FI-STEM-Portable-$Version-$($BackendConfiguration.Suffix)"

if ($Backend -eq "amd") {
    throw "AMD ROCm installer packaging is not yet available: the verified portable build requires a reproducible Python 3.12 ROCm payload. AMD development setup is supported with .\scripts\setup_dev.ps1 -Backend amd."
}

if (-not $SkipPortableBuild) {
    $PortableArgs = @{
        Backend = $Backend
        EnvironmentPath = $EnvironmentPath
        OutputDirectory = $PortableRoot
        DownloadModels = $DownloadModels
    }
    if ($FfmpegPath) { $PortableArgs.FfmpegPath = $FfmpegPath }
    if ($SkipPluginBuild) { $PortableArgs.SkipPluginBuild = $true }
    if ($SkipTests) { $PortableArgs.SkipTests = $true }
    if ($CleanPlugin) { $PortableArgs.CleanPlugin = $true }

    & (Join-Path $RepoRoot "build_portable_windows.ps1") @PortableArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path -LiteralPath (Join-Path $PortableRoot "FI-STEM.exe") -PathType Leaf)) {
    throw "Portable payload is missing: $PortableRoot"
}

$PortableVst3Module = Join-Path $PortableRoot "FI-STEM.vst3\Contents\x86_64-win\FI-STEM.vst3"
if (-not (Test-Path -LiteralPath $PortableVst3Module -PathType Leaf)) {
    throw "Portable VST3 module is missing: $PortableVst3Module"
}

$IsccCandidates = @()
$IsccCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if ($IsccCommand) { $IsccCandidates += $IsccCommand.Source }
$IsccCandidates += @(
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
)
$Iscc = $IsccCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if (-not $Iscc) {
    throw @"
Inno Setup was not found.
Install Inno Setup 6.7+ and rerun this script.
With winget:
  winget install JRSoftware.InnoSetup
"@
}

$Iss = Join-Path $RepoRoot "packaging\FIStem.iss"
if (-not (Test-Path -LiteralPath $Iss -PathType Leaf)) { throw "Missing installer definition: $Iss" }
New-Item -ItemType Directory -Path $DistRoot -Force | Out-Null

# Inno Setup can still hit the classic Windows MAX_PATH limit while recursively
# enumerating deeply nested Python/PyTorch files.  Compile from a temporary SUBST
# drive so every source path presented to ISCC stays short without deleting any
# runtime or third-party license files from the portable payload.
$UsedDrives = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Name.ToUpperInvariant() })
$InnoDrive = @("Z", "Y", "X", "W", "V", "U", "T", "S", "R", "Q", "P") |
    Where-Object { $UsedDrives -notcontains $_ } |
    Select-Object -First 1
if (-not $InnoDrive) {
    throw "Could not find a free temporary drive letter for the Inno Setup long-path workaround."
}

$InnoSourceDir = "$InnoDrive`:"
Write-Host "Building FI-STEM $Version installer..." -ForegroundColor Cyan
Write-Host "Using temporary $InnoSourceDir mapping to avoid Windows/Inno long-path failures."

& subst.exe "$InnoDrive`:" $PortableRoot
if ($LASTEXITCODE -ne 0) {
    throw "Could not create temporary $InnoSourceDir mapping for: $PortableRoot"
}

$IsccExitCode = 1
try {
    & $Iscc "/DSourceDir=$InnoSourceDir" "/DAppVersion=$Version" `
        "/DBackendSuffix=$($BackendConfiguration.Suffix)" "/DOutputDir=$DistRoot" $Iss
    $IsccExitCode = $LASTEXITCODE
}
finally {
    & subst.exe "$InnoDrive`:" /D | Out-Null
}
if ($IsccExitCode -ne 0) { exit $IsccExitCode }

$Setup = Join-Path $DistRoot "FI-STEM-Setup-$Version-$($BackendConfiguration.Suffix).exe"
if (-not (Test-Path -LiteralPath $Setup -PathType Leaf)) {
    throw "Inno Setup completed without producing: $Setup"
}

Write-Host ""
Write-Host "Installer build complete." -ForegroundColor Green
Write-Host "  $Setup"
Get-ChildItem -LiteralPath $DistRoot -Filter "FI-STEM-Setup-$Version-$($BackendConfiguration.Suffix)-*.bin" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  $($_.FullName)"
}
