param(
    [string]$UserLibrary,
    [switch]$ElevatedChild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$PortableVst = Join-Path $RepoRoot "FI-STEM.vst3"
$PortableRemote = Join-Path $RepoRoot "FIStemRemote"
$DevelopmentVst = Join-Path $RepoRoot "plugin\build\FIStemPlugin_artefacts\Release\VST3\FI-STEM.vst3"
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
    Join-Path $RepoRoot "integrations\ableton\FIStemRemote"
}
$VstDestination = Join-Path $env:CommonProgramFiles "VST3\FI-STEM.vst3"
$LegacyVstDestination = Join-Path $env:CommonProgramFiles "VST3\StemLab.vst3"
$VstModule = Join-Path $VstDestination "Contents\x86_64-win\FI-STEM.vst3"

function Normalize-UserLibrary([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }

    $FullPath = [System.IO.Path]::GetFullPath($Path.Trim().Trim('"'))
    $Leaf = Split-Path $FullPath -Leaf

    if ($Leaf -ieq "FIStemRemote") {
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

function Test-Administrator {
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
    return $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $VstSource -and -not (Test-Path -LiteralPath $VstModule -PathType Leaf)) {
    throw "FI-STEM.vst3 was not found in the portable/source payload or the system VST3 directory."
}
if (-not (Test-Path -LiteralPath (Join-Path $RemoteSource "__init__.py") -PathType Leaf)) {
    throw "FIStemRemote source is missing: $RemoteSource"
}
if (Get-Process -Name "Ableton*" -ErrorAction SilentlyContinue) {
    throw "Save your work and fully quit Ableton Live before installing FI-STEM."
}

$ResolvedUserLibrary = Resolve-UserLibrary $UserLibrary

if (-not (Test-Administrator)) {
    $Arguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", ('"' + $PSCommandPath + '"'),
        "-ElevatedChild",
        "-UserLibrary", ('"' + $ResolvedUserLibrary + '"')
    )
    $Process = Start-Process powershell.exe -Verb RunAs -ArgumentList $Arguments -Wait -PassThru
    exit $Process.ExitCode
}

$RemoteRoot = Join-Path $ResolvedUserLibrary "Remote Scripts"
$RemoteDestination = Join-Path $RemoteRoot "FIStemRemote"
$LegacyRemoteDestination = Join-Path $RemoteRoot "StemLabRemote"
$ResolvedRemoteRoot = [System.IO.Path]::GetFullPath($RemoteRoot).TrimEnd('\') + '\'
$ResolvedRemoteDestination = [System.IO.Path]::GetFullPath($RemoteDestination)

if (-not $ResolvedRemoteDestination.StartsWith($ResolvedRemoteRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe Remote Script destination: $ResolvedRemoteDestination"
}

if (Test-Path -LiteralPath $LegacyVstDestination) {
    Write-Host "Removing legacy VST3..."
    Remove-Item -LiteralPath $LegacyVstDestination -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path -LiteralPath $LegacyRemoteDestination) {
    Write-Host "Removing legacy Ableton Remote Script..."
    Remove-Item -LiteralPath $LegacyRemoteDestination -Recurse -Force -ErrorAction SilentlyContinue
}

if ($VstSource) {
    Write-Host "Installing FI-STEM.vst3..."
    New-Item -ItemType Directory -Path (Split-Path $VstDestination -Parent) -Force | Out-Null
    Remove-Item -LiteralPath $VstDestination -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $VstSource -Destination $VstDestination -Recurse -Force
}
else {
    Write-Host "FI-STEM.vst3 is already installed in the system VST3 directory."
}

Write-Host "Installing FIStemRemote..."
New-Item -ItemType Directory -Path $RemoteRoot -Force | Out-Null
Remove-Item -LiteralPath $RemoteDestination -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $RemoteSource -Destination $RemoteDestination -Recurse -Force
Remove-Item -LiteralPath (Join-Path $RemoteDestination "__pycache__") -Recurse -Force -ErrorAction SilentlyContinue

# Record the exact engine command for the separately installed VST3. Portable
# builds point at Engine\python.exe; source-development installs point at the
# venv worker. This avoids embedding a developer-specific absolute checkout path
# into the C++ binary.
$PortableEnginePython = Join-Path $RepoRoot "Engine\python.exe"
$DevelopmentWorker = Join-Path $RepoRoot ".venv\Scripts\stemlab-plugin-job.exe"
$EnginePointer = $null
if (Test-Path -LiteralPath $PortableEnginePython -PathType Leaf) {
    $EnginePointer = $PortableEnginePython
}
elseif (Test-Path -LiteralPath $DevelopmentWorker -PathType Leaf) {
    $EnginePointer = $DevelopmentWorker
}

if ($EnginePointer) {
    $FiStemData = Join-Path $env:LOCALAPPDATA "FI-STEM"
    New-Item -ItemType Directory -Path $FiStemData -Force | Out-Null
    Set-Content `
        -LiteralPath (Join-Path $FiStemData "portable_engine_path.txt") `
        -Encoding ASCII `
        -Value ([System.IO.Path]::GetFullPath($EnginePointer))
}

if (-not (Test-Path -LiteralPath $VstModule -PathType Leaf)) {
    throw "VST3 install verification failed: $VstModule"
}

Write-Host ""
Write-Host "Ableton integration installed." -ForegroundColor Green
Write-Host "Restart Ableton, then select FIStemRemote as a Control Surface with no input or output."
