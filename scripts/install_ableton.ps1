param(
    [string]$UserLibrary,
    [switch]$ElevatedChild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$VstSource = Join-Path $RepoRoot "plugin\build\StemLabPlugin_artefacts\Release\VST3\StemLab.vst3"
$RemoteSource = Join-Path $RepoRoot "integrations\ableton\StemLabRemote"
$VstDestination = Join-Path $env:CommonProgramFiles "VST3\StemLab.vst3"

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

function Test-Administrator {
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
    return $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Path -LiteralPath $VstSource -PathType Container)) {
    throw "Build StemLab first. Missing: $VstSource"
}
if (-not (Test-Path -LiteralPath (Join-Path $RemoteSource "__init__.py") -PathType Leaf)) {
    throw "StemLabRemote source is missing: $RemoteSource"
}
if (Get-Process -Name "Ableton*" -ErrorAction SilentlyContinue) {
    throw "Save your work and fully quit Ableton Live before installing StemLab."
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
$RemoteDestination = Join-Path $RemoteRoot "StemLabRemote"
$ResolvedRemoteRoot = [System.IO.Path]::GetFullPath($RemoteRoot).TrimEnd('\') + '\'
$ResolvedRemoteDestination = [System.IO.Path]::GetFullPath($RemoteDestination)

if (-not $ResolvedRemoteDestination.StartsWith($ResolvedRemoteRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe Remote Script destination: $ResolvedRemoteDestination"
}

Write-Host "Installing StemLab.vst3..."
New-Item -ItemType Directory -Path (Split-Path $VstDestination -Parent) -Force | Out-Null
Remove-Item -LiteralPath $VstDestination -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $VstSource -Destination $VstDestination -Recurse -Force

Write-Host "Installing StemLabRemote..."
New-Item -ItemType Directory -Path $RemoteRoot -Force | Out-Null
Remove-Item -LiteralPath $RemoteDestination -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $RemoteSource -Destination $RemoteDestination -Recurse -Force
Remove-Item -LiteralPath (Join-Path $RemoteDestination "__pycache__") -Recurse -Force -ErrorAction SilentlyContinue

$VstModule = Join-Path $VstDestination "Contents\x86_64-win\StemLab.vst3"
if (-not (Test-Path -LiteralPath $VstModule -PathType Leaf)) {
    throw "VST3 install verification failed: $VstModule"
}

Write-Host ""
Write-Host "Ableton integration installed." -ForegroundColor Green
Write-Host "Restart Ableton, then select StemLabRemote as a Control Surface with no input or output."
