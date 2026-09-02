param(
    [string]$UserLibrary
)

$ErrorActionPreference = "Stop"

# This script runs from two layouts: scripts\win\ in a source checkout and
# scripts\ in the portable payload (whose flat layout predates the split and
# is what installed copies already have). The root is whichever holds the
# payload or the repository.
$RepoRoot = Split-Path $PSScriptRoot -Parent
if ((Split-Path $PSScriptRoot -Leaf) -ieq "win") {
    $RepoRoot = Split-Path $RepoRoot -Parent
}
$PortableVst = Join-Path $RepoRoot "StemLab.vst3"
$PortableRemote = Join-Path $RepoRoot "StemLabRemote"
$DevelopmentVst = Join-Path $RepoRoot "src\plugin\build\StemLabPlugin_artefacts\Release\VST3\StemLab.vst3"
$VstSource = if (Test-Path -LiteralPath $PortableVst -PathType Container) {
    $PortableVst
} elseif (Test-Path -LiteralPath $DevelopmentVst -PathType Container) {
    $DevelopmentVst
} else {
    $null
}
$RemoteSource = if (Test-Path -LiteralPath (Join-Path $PortableRemote "__init__.py") -PathType Leaf) {
    $PortableRemote
} else {
    Join-Path $RepoRoot "src\integrations\ableton\StemLabRemote"
}
# The per-user VST3 folder Steinberg's plug-in-locations page lists first,
# which is where the installer now puts the bundle. Writing here needs no
# elevation, and neither does anything else this script does.
$Vst3Root = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3"
$VstDestination = Join-Path $Vst3Root "StemLab.vst3"
# Where 0.1.x put it: machine-wide, under Program Files, and removable only
# with administrator rights. Hosts scan both folders, so one left behind means
# the DAW lists StemLab twice.
$LegacyVstDestination = Join-Path $env:CommonProgramFiles "VST3\StemLab.vst3"
$VstModule = Join-Path $VstDestination "Contents\x86_64-win\StemLab.vst3"

function Normalize-UserLibrary([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }

    $FullPath = [System.IO.Path]::GetFullPath($Path.Trim().Trim('"'))
    $Leaf = Split-Path $FullPath -Leaf

    if ($Leaf -ieq "StemLabRemote") {
        $FullPath = Split-Path (Split-Path $FullPath -Parent) -Parent
    }
    elseif ($Leaf -ieq "Remote Scripts") {
        $FullPath = Split-Path $FullPath -Parent
    }
    elseif ($Leaf -ieq "Ableton" -and (Test-Path (Join-Path $FullPath "User Library"))) {
        $FullPath = Join-Path $FullPath "User Library"
    }

    return [System.IO.Path]::GetFullPath($FullPath)
}

function Resolve-UserLibrary([string]$ExplicitPath) {
    $Normalized = Normalize-UserLibrary $ExplicitPath
    if ($Normalized) {
        if (-not (Test-Path -LiteralPath $Normalized -PathType Container)) {
            throw "Ableton User Library does not exist: $Normalized"
        }
        return $Normalized
    }

    $DocumentFolders = @(
        [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments),
        (Join-Path $env:USERPROFILE "Documents"),
        $(if ($env:OneDrive) { Join-Path $env:OneDrive "Documents" }),
        $(if ($env:OneDriveConsumer) { Join-Path $env:OneDriveConsumer "Documents" }),
        $(if ($env:OneDriveCommercial) { Join-Path $env:OneDriveCommercial "Documents" })
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    foreach ($Documents in $DocumentFolders) {
        $Candidate = Join-Path $Documents "Ableton\User Library"
        if (Test-Path -LiteralPath $Candidate -PathType Container) {
            return [System.IO.Path]::GetFullPath($Candidate)
        }
    }

    try {
        Add-Type -AssemblyName System.Windows.Forms
        $Dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $Dialog.Description = "Select your Ableton User Library folder"
        $Dialog.ShowNewFolderButton = $false
        if ($Dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return Normalize-UserLibrary $Dialog.SelectedPath
        }
    }
    catch {
        # The actionable error below also works in shells without WinForms.
    }

    throw "Ableton User Library was not found. In Ableton, right-click User Library, choose Show in Explorer, then pass that folder with -UserLibrary."
}

if (-not $VstSource -and -not (Test-Path -LiteralPath $VstModule -PathType Leaf)) {
    throw "StemLab.vst3 was not found in the payload, in a source build, or in $Vst3Root."
}
if (-not (Test-Path -LiteralPath (Join-Path $RemoteSource "__init__.py") -PathType Leaf)) {
    throw "StemLabRemote source is missing: $RemoteSource"
}
if (Get-Process -Name "Ableton*" -ErrorAction SilentlyContinue) {
    throw "Save your work and fully quit Ableton Live before installing StemLab."
}

$ResolvedUserLibrary = Resolve-UserLibrary $UserLibrary

# Only this one removal needs administrator rights, so only it is elevated.
#
# The rest of this script must NOT run elevated, which is why the whole thing
# no longer relaunches itself: under over-the-shoulder elevation the elevated
# process belongs to the administrator, and $env:LOCALAPPDATA and the Ableton
# User Library would then be their profile rather than the one at the keyboard.
if (Test-Path -LiteralPath $LegacyVstDestination) {
    Write-Host "Removing the old machine-wide StemLab.vst3 (needs administrator rights)..."

    $Quoted = $LegacyVstDestination.Replace("'", "''")
    $Removal = "`$ErrorActionPreference='Stop'; Remove-Item -LiteralPath '$Quoted' -Recurse -Force"

    # Declining the UAC prompt throws rather than returning a code, and with
    # $ErrorActionPreference = Stop that would end the script on a raw .NET
    # message instead of the one below.
    $ExitCode = 1
    try {
        $Process = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-Command", $Removal
        )
        $ExitCode = $Process.ExitCode
    }
    catch {
        $ExitCode = 1
    }

    if ($ExitCode -ne 0 -or (Test-Path -LiteralPath $LegacyVstDestination)) {
        throw @"
The old machine-wide plug-in is still installed:
  $LegacyVstDestination
Delete that folder as an administrator and run this again. Leaving it there
makes your DAW list StemLab twice, and the old copy cannot find its Engine.
"@
    }
}

$RemoteRoot = Join-Path $ResolvedUserLibrary "Remote Scripts"
$RemoteDestination = Join-Path $RemoteRoot "StemLabRemote"
$ResolvedRemoteRoot = [System.IO.Path]::GetFullPath($RemoteRoot).TrimEnd('\') + '\'
$ResolvedRemoteDestination = [System.IO.Path]::GetFullPath($RemoteDestination)

if (-not $ResolvedRemoteDestination.StartsWith($ResolvedRemoteRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe Remote Script destination: $ResolvedRemoteDestination"
}

if ($VstSource) {
    Write-Host "Installing StemLab.vst3..."
    New-Item -ItemType Directory -Path (Split-Path $VstDestination -Parent) -Force | Out-Null
    Remove-Item -LiteralPath $VstDestination -Recurse -Force -ErrorAction SilentlyContinue

    # The removal above is SilentlyContinue, so it has to be checked rather
    # than assumed: a bundle another host still has loaded, or an Explorer
    # window sitting inside it, survives it. Copy-Item into a directory that
    # still exists puts the new bundle INSIDE the old one, as
    # StemLab.vst3\StemLab.vst3\Contents\..., and the verification at the end
    # of this script then finds the OLD module in its usual place and reports
    # success - so the user is told the install worked and goes on running
    # the plug-in they already had.
    if (Test-Path -LiteralPath $VstDestination) {
        throw @"
Could not replace the installed plug-in:
  $VstDestination
Something still has it open - a DAW with StemLab loaded, or a window open
inside the bundle. Close it and run this again.
"@
    }

    Copy-Item -LiteralPath $VstSource -Destination $VstDestination -Recurse -Force
}
else {
    Write-Host "StemLab.vst3 is already installed in $Vst3Root."
}

Write-Host "Installing StemLabRemote..."
New-Item -ItemType Directory -Path $RemoteRoot -Force | Out-Null
Remove-Item -LiteralPath $RemoteDestination -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $RemoteSource -Destination $RemoteDestination -Recurse -Force
Remove-Item -LiteralPath (Join-Path $RemoteDestination "__pycache__") -Recurse -Force -ErrorAction SilentlyContinue

# Nothing is written to say where the Engine is. The plug-in resolves exactly
# %LOCALAPPDATA%\StemLab\Engine\python.exe, which is where the installer puts
# it, and does not look anywhere else - so an install has nothing to record and
# nothing to get wrong. A source checkout has no Engine there: set
# STEMLAB_ENGINE to the interpreter you want before starting Ableton.
$InstalledEngine = Join-Path $env:LOCALAPPDATA "StemLab\Engine\python.exe"

if (-not (Test-Path -LiteralPath $InstalledEngine -PathType Leaf)) {
    Write-Host ""
    Write-Host "No Engine at $InstalledEngine." -ForegroundColor Yellow
    Write-Host "Separation will not run until StemLab is installed, or until you set"
    Write-Host "STEMLAB_ENGINE to an interpreter that has stemlab installed."
}

if (-not (Test-Path -LiteralPath $VstModule -PathType Leaf)) {
    throw "VST3 install verification failed: $VstModule"
}

Write-Host ""
Write-Host "Ableton integration installed." -ForegroundColor Green
Write-Host "Restart Ableton, then select StemLabRemote as a Control Surface with no input or output."
