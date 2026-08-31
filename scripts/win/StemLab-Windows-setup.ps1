# One-step setup for the StemLab Windows installer. Ships on the releases
# page; it is the only file you need to download. Right-click it and choose
# "Run with PowerShell", or from a terminal:
#
#   powershell -ExecutionPolicy Bypass -File StemLab-Windows-setup.ps1 cuda
#
# Given a flavor (cpu, cuda, xpu) it downloads that installer and its .bin
# slices, if any, from the release it shipped with, checks them against the
# release's SHA256SUMS, runs the installer, and deletes the downloads once
# the install finishes. With no flavor it asks, or picks up installer files
# already sitting next to it.
#
# NOTHING IS WRITTEN NEXT TO THIS SCRIPT. An installer and its slices run to
# gigabytes, so everything downloaded is staged in
#
#   %LOCALAPPDATA%\StemLab\Setup        ($env:STEMLAB_SETUP_STAGE overrides)
#
# and that whole directory goes when the install succeeds. Not %TEMP%: Storage
# Sense deletes it on a schedule, and this is the folder a half-finished
# multi-gigabyte download has to survive in. A run that fails keeps the staging
# directory on purpose - that is what makes running this again resume rather
# than start over.
#
# StemLab installs for your account only and asks for no administrator
# rights, so neither does this.
#
# Kept compatible with Windows PowerShell 5.1 - that is what "Run with
# PowerShell" launches.

param([string]$What = "")

$ErrorActionPreference = "Stop"
# Windows PowerShell renders the byte-progress bar so eagerly that it
# dominates the download time on a multi-GB file.
$ProgressPreference = "SilentlyContinue"

# Read from, never written to: this is where a hand-downloaded installer sits,
# and where this script deletes itself from at the end.
$SourceDir = $PSScriptRoot

# Filled in when the release is built. In a source checkout they stay as
# placeholders and only the already-downloaded-files path works.
$ReleaseUrl = "@RELEASE_URL@"
$Version = "@VERSION@"

$Flavors = @("cuda", "xpu", "cpu")

function Fail([string]$Message) {
    Write-Host $Message -ForegroundColor Red
    exit 1
}

function Test-RemoteAvailable {
    return -not ($ReleaseUrl.StartsWith("@") -or $Version.StartsWith("@"))
}

# The same volume as %LOCALAPPDATA%\StemLab, which is where the installer puts
# everything, so nothing here has to cross a disk.
$Stage = $env:STEMLAB_SETUP_STAGE
if (-not $Stage) { $Stage = Join-Path $env:LOCALAPPDATA "StemLab\Setup" }
if (-not [System.IO.Path]::IsPathRooted($Stage)) {
    Fail "STEMLAB_SETUP_STAGE must be a full path (got: $Stage)"
}

try {
    New-Item -ItemType Directory -Path $Stage -Force | Out-Null
}
catch {
    Fail "Cannot create the staging folder $Stage`nSet STEMLAB_SETUP_STAGE to somewhere writable with room for the installer."
}

# A .download is another run still fetching, or the remains of one that was
# killed. Verifying a half-written installer fails its checksum for no visible
# reason, so refuse to run over either.
$Partial = @(Get-ChildItem -LiteralPath $Stage -Filter "*.download" -File -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Name })
if ($Partial.Count -gt 0) {
    Fail @"
An unfinished download is in $Stage`:
  $($Partial -join ', ')
Another run may still be fetching it - wait for that to finish. If a run was
killed, delete those files and run this again.
"@
}

function Find-Piece([string]$Name) {
    # This run's staging folder first, then beside the script, which is where
    # a hand-downloaded installer lands. Returns $null when neither has it.
    $Staged = Join-Path $Stage $Name
    if (Test-Path -LiteralPath $Staged -PathType Leaf) { return $Staged }

    $Beside = Join-Path $SourceDir $Name
    if (Test-Path -LiteralPath $Beside -PathType Leaf) { return $Beside }

    return $null
}

function Get-RemoteFile([string]$Name, [switch]$Optional) {
    # Downloads $ReleaseUrl/$Name into the staging folder, leaving nothing
    # behind on failure. Returns $false for a 404 when the file is optional.
    $Final = Join-Path $Stage $Name
    $Temp = "$Final.download"
    try {
        Invoke-WebRequest -Uri "$ReleaseUrl/$Name" -OutFile $Temp -UseBasicParsing
    }
    catch {
        Remove-Item -LiteralPath $Temp -Force -ErrorAction SilentlyContinue
        $Status = 0
        try { $Status = [int]$_.Exception.Response.StatusCode } catch { }
        if ($Optional -and $Status -eq 404) { return $false }
        Fail "Could not download $Name from $ReleaseUrl`n$($_.Exception.Message)"
    }
    Move-Item -LiteralPath $Temp -Destination $Final -Force
    Write-Host "  downloaded $Name"
    return $true
}

# ------------------------------------------------------------ pick a setup

# Names, not paths: the same installer can be half-staged and half beside the
# script, and Find-Piece decides which copy of a given file is used.
$Candidates = @()
foreach ($Directory in @($Stage, $SourceDir)) {
    foreach ($File in @(Get-ChildItem -LiteralPath $Directory -Filter "StemLab-Setup-*.exe" -File -ErrorAction SilentlyContinue)) {
        if ($Candidates -notcontains $File.Name) { $Candidates += $File.Name }
    }
}

$Download = $false
if ($What -and $Flavors -contains $What) {
    if (-not (Test-RemoteAvailable)) {
        Fail "This copy of the script is not tied to a release, so it cannot download. Pass a filename instead."
    }
    $Setup = "StemLab-Setup-$Version-$What.exe"
    # Anything already here for this installer is used as-is; only a complete
    # absence triggers the download.
    if (-not (Find-Piece $Setup)) { $Download = $true }
}
elseif ($What) {
    $Setup = Split-Path $What -Leaf
    if (-not (Find-Piece $Setup)) { Fail "$Setup is not here." }
}
elseif ($Candidates.Count -eq 1) {
    $Setup = $Candidates[0]
}
elseif ($Candidates.Count -gt 1) {
    Write-Host "More than one installer is here - say which one:"
    foreach ($Candidate in $Candidates) {
        Write-Host "  powershell -ExecutionPolicy Bypass -File $($MyInvocation.MyCommand.Name) $Candidate"
    }
    exit 2
}
elseif (Test-RemoteAvailable) {
    Write-Host "Which StemLab build fits your hardware?"
    Write-Host "  cuda - NVIDIA"
    Write-Host "  xpu  - Intel Arc / Xe"
    Write-Host "  cpu  - no GPU offload (smallest; AMD cards take this on Windows)"
    $Choice = Read-Host "Flavor"
    if ($Flavors -notcontains $Choice) { Fail "Unknown flavor: $Choice" }
    $Setup = "StemLab-Setup-$Version-$Choice.exe"
    $Download = $true
}
else {
    Fail "No StemLab-Setup-*.exe found next to this script. Download the installer into this folder first."
}

# ---------------------------------------------------------------- download

if ($Download) {
    Write-Host "Downloading $Setup from"
    Write-Host "  $ReleaseUrl"
    Write-Host "into $Stage"
    $null = Get-RemoteFile $Setup
    # An installer over the single-file limit ships its payload in numbered
    # .bin slices; the first miss is the end of them. A transfer cut short
    # mid-sequence is caught below, by the checksums or by the installer's
    # own slice checks.
    $Base = [System.IO.Path]::GetFileNameWithoutExtension($Setup)
    $Slice = 1
    while (Get-RemoteFile "$Base-$Slice.bin" -Optional) {
        $Slice += 1
    }
}

$SetupBase = [System.IO.Path]::GetFileNameWithoutExtension($Setup)

$PieceNames = @($Setup)
foreach ($Directory in @($Stage, $SourceDir)) {
    foreach ($File in @(Get-ChildItem -LiteralPath $Directory -Filter "$SetupBase-*.bin" -File -ErrorAction SilentlyContinue)) {
        if ($PieceNames -notcontains $File.Name) { $PieceNames += $File.Name }
    }
}

# ------------------------------------------------------------------ verify

# The release publishes one SHA256SUMS over every asset. Fetch it when this
# script can, use a local copy when one is already here.
$SumsDownloaded = $false
$SumsPath = Find-Piece "SHA256SUMS"
if (-not $SumsPath -and (Test-RemoteAvailable)) {
    $SumsDownloaded = Get-RemoteFile "SHA256SUMS" -Optional
    if ($SumsDownloaded) { $SumsPath = Join-Path $Stage "SHA256SUMS" }
}

if ($SumsPath) {
    $Sums = @{ }
    foreach ($Line in Get-Content -LiteralPath $SumsPath) {
        if ($Line -match '^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$') {
            $Sums[$Matches[2]] = $Matches[1].ToLowerInvariant()
        }
    }
    foreach ($Name in $PieceNames) {
        if (-not $Sums.ContainsKey($Name)) {
            Write-Warning "SHA256SUMS does not list $Name, so it is not being verified."
            continue
        }
        Write-Host "Verifying $Name..."
        $Actual = (Get-FileHash -LiteralPath (Find-Piece $Name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($Actual -ne $Sums[$Name]) {
            Fail "Checksum mismatch on $Name - the download is damaged. Delete it, re-download, and run this again."
        }
    }
}
else {
    Write-Warning "No SHA256SUMS here, so the installer is not being verified. Its own slice checks still apply."
}

# --------------------------------------------------------------- install

# The installer needs its .bin slices beside it, so it is run from wherever
# the set actually is rather than copied anywhere.
$SetupPath = Find-Piece $Setup

Write-Host "Starting the installer. StemLab installs for your account only,"
Write-Host "so Windows should not ask for an administrator password."
if ($env:STEMLAB_SETUP_INSTALLER) {
    # Test hook: lets an end-to-end test observe the invocation and choose
    # the exit code without a real Windows installer.
    & $env:STEMLAB_SETUP_INSTALLER $SetupPath
    $ExitCode = $LASTEXITCODE
}
else {
    $Process = Start-Process -FilePath $SetupPath -Wait -PassThru
    $ExitCode = $Process.ExitCode
}

if ($ExitCode -ne 0) {
    Fail "The installer did not finish (exit code $ExitCode). The downloaded files were kept in $Stage so you can try again."
}

# ----------------------------------------------------------------- tidy up

# Everything the setup needed is spent now: the staging folder holds whatever
# this run downloaded, and an installer somebody put beside this script by hand
# is removed too - a one-download install that leaves gigabytes behind is not
# one. The source-checkout copy of this script (unbaked placeholders) is the
# one copy that is not a download, so it stays.
Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue

foreach ($Name in $PieceNames) {
    Remove-Item -LiteralPath (Join-Path $SourceDir $Name) -Force -ErrorAction SilentlyContinue
}
if ($SumsDownloaded -or (Test-Path -LiteralPath (Join-Path $SourceDir "SHA256SUMS"))) {
    Remove-Item -LiteralPath (Join-Path $SourceDir "SHA256SUMS") -Force -ErrorAction SilentlyContinue
}

if (Test-RemoteAvailable) {
    Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "StemLab is installed. Removed $Setup and its downloads."
