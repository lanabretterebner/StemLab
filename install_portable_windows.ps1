param(
    [Parameter(Mandatory = $false)]
    [string]$UserLibrary,

    # Internal flag used only by the UAC-elevated child process.
    [Parameter(Mandatory = $false)]
    [switch]$ElevatedChild
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

function Normalize-UserLibraryPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    $full = [System.IO.Path]::GetFullPath($Path.Trim('"').Trim())
    $leaf = Split-Path $full -Leaf

    if ($leaf -ieq "StemLabRemote") {
        $remoteScripts = Split-Path $full -Parent
        if ((Split-Path $remoteScripts -Leaf) -ieq "Remote Scripts") {
            return (Split-Path $remoteScripts -Parent)
        }
    }

    if ($leaf -ieq "Remote Scripts") {
        return (Split-Path $full -Parent)
    }

    if ($leaf -ieq "Ableton") {
        $child = Join-Path $full "User Library"
        if (Test-Path $child) {
            return $child
        }
    }

    return $full
}

function Select-AbletonUserLibrary {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = "Select your Ableton User Library folder"
        $dialog.ShowNewFolderButton = $false

        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return (Normalize-UserLibraryPath $dialog.SelectedPath)
        }
    }
    catch {
        # If the graphical picker is unavailable, the caller will show the
        # normal actionable error instead.
    }

    return $null
}

function Resolve-AbletonUserLibrary([string]$ExplicitPath) {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $resolved = Normalize-UserLibraryPath $ExplicitPath

        if (-not (Test-Path $resolved -PathType Container)) {
            throw "Ableton User Library does not exist: $resolved"
        }

        return $resolved
    }

    $candidates = New-Object System.Collections.Generic.List[string]

    function Add-Candidate([string]$DocumentsPath) {
        if ([string]::IsNullOrWhiteSpace($DocumentsPath)) {
            return
        }

        $candidate = Join-Path $DocumentsPath "Ableton\User Library"

        if ((Test-Path $candidate -PathType Container) -and
            (-not $candidates.Contains($candidate))) {
            $candidates.Add($candidate)
        }
    }

    Add-Candidate ([Environment]::GetFolderPath(
        [Environment+SpecialFolder]::MyDocuments
    ))
    Add-Candidate (Join-Path $env:USERPROFILE "Documents")

    if (-not [string]::IsNullOrWhiteSpace($env:OneDrive)) {
        Add-Candidate (Join-Path $env:OneDrive "Documents")
    }
    if (-not [string]::IsNullOrWhiteSpace($env:OneDriveConsumer)) {
        Add-Candidate (Join-Path $env:OneDriveConsumer "Documents")
    }
    if (-not [string]::IsNullOrWhiteSpace($env:OneDriveCommercial)) {
        Add-Candidate (Join-Path $env:OneDriveCommercial "Documents")
    }

    if ($candidates.Count -gt 0) {
        return $candidates[0]
    }

    $picked = Select-AbletonUserLibrary
    if (-not [string]::IsNullOrWhiteSpace($picked) -and
        (Test-Path $picked -PathType Container)) {
        return $picked
    }

    throw "Ableton User Library was not found. In Ableton, right-click User Library in the Browser, choose Show in Explorer, then run setup again and select that folder."
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )
}

function Relaunch-AsAdministrator {
    Write-Host ""
    Write-Host "StemLab needs administrator permission only to copy the VST3 into Program Files."
    Write-Host "Opening the Windows UAC prompt..." -ForegroundColor Cyan
    Write-Host ""

    $args = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -ElevatedChild'

    if (-not [string]::IsNullOrWhiteSpace($UserLibrary)) {
        $escaped = $UserLibrary.Replace('"', '\"')
        $args += ' -UserLibrary "' + $escaped + '"'
    }

    $process = Start-Process `
        -FilePath "powershell.exe" `
        -Verb RunAs `
        -ArgumentList $args `
        -Wait `
        -PassThru

    exit $process.ExitCode
}

$VstSource = Join-Path $Root "StemLab.vst3"
$EnginePython = Join-Path $Root "Engine\python.exe"
$RemoteSource = Join-Path $Root "StemLabRemote"
$VstDest = "C:\Program Files\Common Files\VST3\StemLab.vst3"
$LogPath = Join-Path $env:TEMP "StemLab-Ableton-install.log"
$EnginePointerDir = Join-Path $env:LOCALAPPDATA "StemLab"
$EnginePointer = Join-Path $EnginePointerDir "portable_engine_path.txt"

function Test-PortablePackage {
    return (
        (Test-Path $VstSource -PathType Container) -and
        (Test-Path $EnginePython -PathType Leaf) -and
        (Test-Path (Join-Path $RemoteSource "__init__.py") -PathType Leaf)
    )
}

if (-not (Test-PortablePackage)) {
    throw "This setup helper must be run from a built StemLab portable release containing StemLab.vst3, Engine, and StemLabRemote."
}

# Resolve these while still running as the normal user. The portable engine
# remains in the extracted StemLab folder; Ableton's VST3 reads this pointer.
$ResolvedUserLibrary = Resolve-AbletonUserLibrary $UserLibrary
New-Item -ItemType Directory -Path $EnginePointerDir -Force | Out-Null
Set-Content -Path $EnginePointer -Value ([System.IO.Path]::GetFullPath($EnginePython)) -Encoding UTF8

# Never kill Ableton automatically: an open Live set may contain unsaved work.
if (Get-Process -Name Ableton* -ErrorAction SilentlyContinue) {
    throw "Ableton Live is currently open. Save your work, fully quit Ableton Live, then run StemLab's Ableton setup again."
}

if (-not (Test-IsAdministrator)) {
    $UserLibrary = $ResolvedUserLibrary
    Relaunch-AsAdministrator
}

$RemoteRoot = Join-Path $ResolvedUserLibrary "Remote Scripts"
$RemoteDest = Join-Path $RemoteRoot "StemLabRemote"
$TranscriptStarted = $false

try {
    try {
        Start-Transcript -Path $LogPath -Append -Force | Out-Null
        $TranscriptStarted = $true
    }
    catch {}

    Write-Host ""
    Write-Host "======================================="
    Write-Host " StemLab Ableton Live setup"
    Write-Host "======================================="
    Write-Host ""
    Write-Host "Portable ML engine:"
    Write-Host "  $EnginePython"
    Write-Host ""
    Write-Host "Ableton User Library:"
    Write-Host "  $ResolvedUserLibrary"
    Write-Host ""

    Write-Host "Installing StemLab VST3..."
    New-Item -ItemType Directory -Path (Split-Path $VstDest -Parent) -Force | Out-Null
    Remove-Item $VstDest -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item $VstSource $VstDest -Recurse -Force

    $VstModule = Join-Path $VstDest "Contents\x86_64-win\StemLab.vst3"
    if (-not (Test-Path $VstModule -PathType Leaf)) {
        throw "VST3 install verification failed. Missing: $VstModule"
    }

    Write-Host "Installing StemLabRemote..."
    New-Item -ItemType Directory -Path $RemoteRoot -Force | Out-Null
    Remove-Item $RemoteDest -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item $RemoteSource $RemoteDest -Recurse -Force

    $RemoteInit = Join-Path $RemoteDest "__init__.py"
    if (-not (Test-Path $RemoteInit -PathType Leaf)) {
        throw "Remote Script install verification failed. Missing: $RemoteInit"
    }

    Remove-Item (Join-Path $RemoteDest "__pycache__") -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem $RemoteDest -Recurse -File -ErrorAction SilentlyContinue |
        Unblock-File -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "StemLab Ableton integration is ready." -ForegroundColor Green
    Write-Host ""
    Write-Host "Restart Ableton Live, then set:"
    Write-Host "  Settings > Link, Tempo & MIDI"
    Write-Host "  Control Surface = StemLabRemote"
    Write-Host "  Input = None"
    Write-Host "  Output = None"
    Write-Host ""
    Write-Host "The large ML engine remains in your extracted StemLab folder."
    Write-Host "Keep that folder in place while using the Ableton plug-in."
    Write-Host ""
    Write-Host "Installer log: $LogPath" -ForegroundColor DarkGray
}
catch {
    Write-Host ""
    Write-Host "STEMLAB ABLETON SETUP FAILED" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host ""
    Write-Host "Installer log: $LogPath" -ForegroundColor Yellow

    if ($TranscriptStarted) {
        try { Stop-Transcript | Out-Null } catch {}
        $TranscriptStarted = $false
    }

    if ($ElevatedChild) {
        [void](Read-Host "Press Enter to close this window")
    }

    exit 1
}
finally {
    if ($TranscriptStarted) {
        try { Stop-Transcript | Out-Null } catch {}
    }
}
