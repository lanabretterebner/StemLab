param(
    [Parameter(Mandatory = $false)]
    [string]$UserLibrary
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

function Invoke-Installer([string]$Path) {
    try {
        if ([string]::IsNullOrWhiteSpace($UserLibrary)) {
            & $Path
        }
        else {
            & $Path -UserLibrary $UserLibrary
        }

        exit $LASTEXITCODE
    }
    catch {
        Write-Host ""
        Write-Host "StemLab Ableton setup could not continue:" -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
        Write-Host ""
        [void](Read-Host "Press Enter to close this window")
        exit 1
    }
}

# Running from the generated Windows release.
$LocalPortableInstaller = Join-Path $Root "install_portable_windows.ps1"
$LocalVst = Join-Path $Root "StemLab.vst3"
$LocalEngine = Join-Path $Root "Engine\python.exe"
$LocalRemote = Join-Path $Root "StemLabRemote\__init__.py"

if ((Test-Path $LocalVst) -and
    (Test-Path $LocalEngine) -and
    (Test-Path $LocalRemote)) {
    Invoke-Installer $LocalPortableInstaller
}

# Running from the GitHub/source repository after a portable build.
$BuiltRelease = Join-Path $Root "dist\StemLab-0.9.9-Windows"
$BuiltInstaller = Join-Path $BuiltRelease "install_portable_windows.ps1"

if (Test-Path $BuiltInstaller) {
    Write-Host "Using the built portable release:"
    Write-Host "  $BuiltRelease"
    Write-Host ""
    Invoke-Installer $BuiltInstaller
}

Write-Host ""
Write-Host "No built portable Windows release was found." -ForegroundColor Yellow
Write-Host ""
Write-Host "From the repository root, run:"
Write-Host "  .\build_portable_windows.ps1" -ForegroundColor Cyan
Write-Host ""
exit 2
