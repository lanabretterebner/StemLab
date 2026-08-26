param(
    [ValidateSet("nvidia", "cpu", "amd")]
    [string]$Backend = "nvidia",
    [string]$EnvironmentPath = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot "windows_backend.ps1")
$BackendConfiguration = Get-StemLabBackendConfiguration $Backend

if ([string]::IsNullOrWhiteSpace($EnvironmentPath)) {
    $EnvironmentPath = $BackendConfiguration.DefaultEnvironment
}

if ([System.IO.Path]::IsPathRooted($EnvironmentPath)) {
    $Environment = [System.IO.Path]::GetFullPath($EnvironmentPath)
}
else {
    $Environment = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $EnvironmentPath))
}

$Python = Join-Path $Environment "Scripts\python.exe"

if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    Write-Host "Creating the StemLab Python $($BackendConfiguration.Python) $($BackendConfiguration.Label) environment..." -ForegroundColor Cyan
    & py "-$($BackendConfiguration.Python)" -m venv $Environment
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$ActualPython = & $Python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($ActualPython.Trim() -ne $BackendConfiguration.Python) {
    throw "$($BackendConfiguration.Label) requires Python $($BackendConfiguration.Python), but $Environment uses Python $($ActualPython.Trim()). Use a separate environment path."
}

Write-Host "Installing the pinned $($BackendConfiguration.Label) PyTorch runtime..." -ForegroundColor Cyan
Install-StemLabTorchBackend $Python $BackendConfiguration

$VerifyBackend = Join-Path $PSScriptRoot "verify_windows_backend.py"
& $Python $VerifyBackend --backend $Backend
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installing StemLab, developer tools, and recursive separation support..."
Write-Host "This is a large first-time install because it includes the audio models' runtimes." -ForegroundColor DarkGray
& $Python -m pip install -e "$RepoRoot[dev,recursive]" `
    --constraint (Join-Path $RepoRoot "requirements\windows-backend-constraints.txt")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Verify once more after resolving every transitive dependency.
& $Python $VerifyBackend --backend $Backend
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running the unit tests..."
& $Python -m pytest -q $RepoRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$MainJob = Join-Path $Environment "Scripts\stemlab-plugin-job.exe"
$RecursiveJob = Join-Path $Environment "Scripts\stemlab-recursive-job.exe"

foreach ($Job in @($MainJob, $RecursiveJob)) {
    if (-not (Test-Path -LiteralPath $Job -PathType Leaf)) {
        throw "StemLab setup finished without creating: $Job"
    }
}

Write-Host ""
Write-Host "StemLab development environment is ready." -ForegroundColor Green
Write-Host "  Python: $Python"
Write-Host "  Tests:  $RepoRoot\tests"
