param(
    [string]$EnvironmentPath = ".venv"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent

if ([System.IO.Path]::IsPathRooted($EnvironmentPath)) {
    $Environment = [System.IO.Path]::GetFullPath($EnvironmentPath)
}
else {
    $Environment = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $EnvironmentPath))
}

$Python = Join-Path $Environment "Scripts\python.exe"

if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    Write-Host "Creating the FI-STEM Python 3.11 environment..." -ForegroundColor Cyan
    py -3.11 -m venv $Environment
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Installing FI-STEM, developer tools, and recursive separation support..."
Write-Host "This is a large first-time install because it includes the audio models' runtimes." -ForegroundColor DarkGray
& $Python -m pip install -e "$RepoRoot[dev,recursive]"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running the unit tests..."
& $Python -m pytest -q $RepoRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$MainJob = Join-Path $Environment "Scripts\stemlab-plugin-job.exe"
$RecursiveJob = Join-Path $Environment "Scripts\stemlab-recursive-job.exe"

foreach ($Job in @($MainJob, $RecursiveJob)) {
    if (-not (Test-Path -LiteralPath $Job -PathType Leaf)) {
        throw "FI-STEM setup finished without creating: $Job"
    }
}

Write-Host ""
Write-Host "FI-STEM development environment is ready." -ForegroundColor Green
Write-Host "  Python: $Python"
Write-Host "  Tests:  $RepoRoot\tests"
