param(
    [string]$EnvironmentPath = ".venv",
    [string]$FfmpegPath = "",
    [switch]$SkipPortableBuild,
    [switch]$SkipPluginBuild,
    [switch]$SkipTests,
    [switch]$CleanPlugin
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$DistRoot = Join-Path $RepoRoot "dist"

$VersionMatch = Select-String -LiteralPath (Join-Path $RepoRoot "pyproject.toml") -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1
if (-not $VersionMatch) { throw "Could not read StemLab version from pyproject.toml." }
$Version = $VersionMatch.Matches[0].Groups[1].Value
$PortableRoot = Join-Path $DistRoot "StemLab-Portable-$Version"

if (-not $SkipPortableBuild) {
    $PortableArgs = @{
        EnvironmentPath = $EnvironmentPath
        OutputDirectory = $PortableRoot
    }
    if ($FfmpegPath) { $PortableArgs.FfmpegPath = $FfmpegPath }
    if ($SkipPluginBuild) { $PortableArgs.SkipPluginBuild = $true }
    if ($SkipTests) { $PortableArgs.SkipTests = $true }
    if ($CleanPlugin) { $PortableArgs.CleanPlugin = $true }

    & (Join-Path $PSScriptRoot "build_portable_windows.ps1") @PortableArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path -LiteralPath (Join-Path $PortableRoot "StemLab.exe") -PathType Leaf)) {
    throw "Portable payload is missing: $PortableRoot"
}

# The payload names itself. build_portable_windows.ps1 writes this from the
# torch it actually installed, so the installer filename describes what is
# inside it rather than what a build was asked for.
$FlavorFile = Join-Path $PortableRoot "Engine\.stemlab-torch-flavor"
if (-not (Test-Path -LiteralPath $FlavorFile -PathType Leaf)) {
    throw @"
The portable payload does not record which torch build it carries.
Missing: $FlavorFile
Rebuild the payload with build_portable_windows.ps1.
"@
}
$Flavor = (Get-Content -LiteralPath $FlavorFile -Raw).Trim()
if (-not $Flavor) { throw "The recorded torch flavor is empty: $FlavorFile" }
Write-Host "Payload torch flavor: $Flavor" -ForegroundColor Cyan

# The installer copies the VST3 bundle to the system VST3 directory, so a
# payload whose bundle is hollow must fail here, not on the user's machine.
$PortableVst3Module = Join-Path $PortableRoot "StemLab.vst3\Contents\x86_64-win\StemLab.vst3"
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

$Iss = Join-Path $PSScriptRoot "StemLab.iss"
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
Write-Host "Building StemLab $Version installer..." -ForegroundColor Cyan
Write-Host "Using temporary $InnoSourceDir mapping to avoid Windows/Inno long-path failures."

& subst.exe "$InnoDrive`:" $PortableRoot
if ($LASTEXITCODE -ne 0) {
    throw "Could not create temporary $InnoSourceDir mapping for: $PortableRoot"
}

$IsccExitCode = 1
try {
    & $Iscc "/DSourceDir=$InnoSourceDir" "/DAppVersion=$Version" "/DFlavor=$Flavor" "/DOutputDir=$DistRoot" $Iss
    $IsccExitCode = $LASTEXITCODE
}
finally {
    & subst.exe "$InnoDrive`:" /D | Out-Null
}
if ($IsccExitCode -ne 0) { exit $IsccExitCode }

$Setup = Join-Path $DistRoot "StemLab-Setup-$Version-$Flavor.exe"
if (-not (Test-Path -LiteralPath $Setup -PathType Leaf)) {
    throw "Inno Setup completed without producing: $Setup"
}

Write-Host ""
Write-Host "Installer build complete." -ForegroundColor Green
Write-Host "  $Setup"
Get-ChildItem -LiteralPath $DistRoot -Filter "StemLab-Setup-$Version-$Flavor-*.bin" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  $($_.FullName)"
}
