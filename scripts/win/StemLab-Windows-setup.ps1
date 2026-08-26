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
# Kept compatible with Windows PowerShell 5.1 - that is what "Run with
# PowerShell" launches.

param([string]$What = "")

$ErrorActionPreference = "Stop"
# Windows PowerShell renders the byte-progress bar so eagerly that it
# dominates the download time on a multi-GB file.
$ProgressPreference = "SilentlyContinue"

Set-Location -LiteralPath $PSScriptRoot

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

function Get-RemoteFile([string]$Name, [switch]$Optional) {
    # Downloads $ReleaseUrl/$Name into the current folder, leaving nothing
    # behind on failure. Returns $false for a 404 when the file is optional.
    $Temp = "$Name.download"
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
    Move-Item -LiteralPath $Temp -Destination $Name -Force
    Write-Host "  downloaded $Name"
    return $true
}

# ------------------------------------------------------------ pick a setup

$Candidates = @(Get-ChildItem -Path "StemLab-Setup-*.exe" -File -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Name })

$Download = $false
if ($What -and $Flavors -contains $What) {
    if (-not (Test-RemoteAvailable)) {
        Fail "This copy of the script is not tied to a release, so it cannot download. Pass a filename instead."
    }
    $Setup = "StemLab-Setup-$Version-$What.exe"
    # Anything already here for this installer is used as-is; only a complete
    # absence triggers the download.
    if (-not (Test-Path -LiteralPath $Setup)) { $Download = $true }
}
elseif ($What) {
    $Setup = $What
    if (-not (Test-Path -LiteralPath $Setup)) { Fail "$Setup is not here." }
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
$Pieces = @($Setup) + @(Get-ChildItem -Path "$SetupBase-*.bin" -File -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Name })

# ------------------------------------------------------------------ verify

# The release publishes one SHA256SUMS over every asset. Fetch it when this
# script can, use a local copy when one is already here.
$SumsDownloaded = $false
if (-not (Test-Path -LiteralPath "SHA256SUMS") -and (Test-RemoteAvailable)) {
    $SumsDownloaded = Get-RemoteFile "SHA256SUMS" -Optional
}

if (Test-Path -LiteralPath "SHA256SUMS") {
    $Sums = @{ }
    foreach ($Line in Get-Content -LiteralPath "SHA256SUMS") {
        if ($Line -match '^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$') {
            $Sums[$Matches[2]] = $Matches[1].ToLowerInvariant()
        }
    }
    foreach ($Piece in $Pieces) {
        if (-not $Sums.ContainsKey($Piece)) {
            Write-Warning "SHA256SUMS does not list $Piece, so it is not being verified."
            continue
        }
        Write-Host "Verifying $Piece..."
        $Actual = (Get-FileHash -LiteralPath $Piece -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($Actual -ne $Sums[$Piece]) {
            Fail "Checksum mismatch on $Piece - the download is damaged. Delete it, re-download, and run this again."
        }
    }
}
else {
    Write-Warning "No SHA256SUMS here, so the installer is not being verified. Its own slice checks still apply."
}

# --------------------------------------------------------------- install

Write-Host "Starting the installer (expect the usual administrator prompt)..."
if ($env:STEMLAB_SETUP_INSTALLER) {
    # Test hook: lets an end-to-end test observe the invocation and choose
    # the exit code without a real Windows installer.
    & $env:STEMLAB_SETUP_INSTALLER (Join-Path (Get-Location) $Setup)
    $ExitCode = $LASTEXITCODE
}
else {
    $Process = Start-Process -FilePath (Join-Path (Get-Location) $Setup) -Wait -PassThru
    $ExitCode = $Process.ExitCode
}

if ($ExitCode -ne 0) {
    Fail "The installer did not finish (exit code $ExitCode). The downloaded files were kept so you can try again."
}

# ----------------------------------------------------------------- tidy up

foreach ($Piece in $Pieces) {
    Remove-Item -LiteralPath $Piece -Force
}
if ($SumsDownloaded) {
    Remove-Item -LiteralPath "SHA256SUMS" -Force
}

Write-Host ""
Write-Host "StemLab is installed. Removed $Setup and its downloads."
