param(
    [Parameter(Mandatory = $false)]
    [string]$UserLibrary
)

$ErrorActionPreference = "Stop"


function Normalize-UserLibraryPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    $full = [System.IO.Path]::GetFullPath($Path.Trim('"').Trim())
    $leaf = Split-Path $full -Leaf

    # Be forgiving if the user pastes Remote Scripts or StemLabRemote itself.
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

    # Windows known-folder path follows normal Documents redirection.
    Add-Candidate ([Environment]::GetFolderPath(
        [Environment+SpecialFolder]::MyDocuments
    ))

    # Extra fallbacks for common OneDrive / non-redirected layouts.
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

    Write-Host ""
    Write-Host "Could not locate Ableton's User Library automatically." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "In Ableton Live:"
    Write-Host "  Browser > right-click User Library > Show in Explorer"
    Write-Host ""
    Write-Host "Then rerun the installer with:"
    Write-Host '  -UserLibrary "C:\full\path\to\User Library"'
    Write-Host ""

    throw "Ableton User Library was not found. No guessed/fake User Library was created."
}


$source = Join-Path $PSScriptRoot "StemLabRemote"

if (-not (Test-Path (Join-Path $source "__init__.py") -PathType Leaf)) {
    throw "Could not find StemLabRemote source folder: $source"
}

$ResolvedUserLibrary = Resolve-AbletonUserLibrary $UserLibrary
$remoteRoot = Join-Path $ResolvedUserLibrary "Remote Scripts"
$destination = Join-Path $remoteRoot "StemLabRemote"

Write-Host "Installing StemLabRemote into:"
Write-Host "  $ResolvedUserLibrary"
Write-Host ""

New-Item -ItemType Directory -Force -Path $remoteRoot | Out-Null
Remove-Item -Recurse -Force $destination -ErrorAction SilentlyContinue
Copy-Item $source $destination -Recurse -Force

Remove-Item (Join-Path $destination "__pycache__") -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem $destination -Recurse -File -ErrorAction SilentlyContinue |
    Unblock-File -ErrorAction SilentlyContinue

$initFile = Join-Path $destination "__init__.py"

if (-not (Test-Path $initFile -PathType Leaf)) {
    throw "Install failed. Missing: $initFile"
}

Write-Host ""
Write-Host "StemLabRemote installed successfully." -ForegroundColor Green
Write-Host "  $initFile"
Write-Host ""
Write-Host "Fully quit/restart Ableton Live, then set:"
Write-Host "  Control Surface = StemLabRemote"
Write-Host "  Input = None"
Write-Host "  Output = None"
