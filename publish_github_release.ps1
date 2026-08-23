param(
    [string]$Tag = "v0.9.9",
    [string]$Title = "StemLab 0.9.9",
    [string]$FfmpegPath,
    [switch]$SkipBuild,
    [switch]$IncludePortableZip
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$Version = "0.9.9"
$Dist = Join-Path $Root "dist"
$Notes = Join-Path $Root "RELEASE_NOTES.md"

$gh = Get-Command gh.exe -ErrorAction SilentlyContinue
if ($null -eq $gh) {
    $gh = Get-Command gh -ErrorAction SilentlyContinue
}

if ($null -eq $gh) {
    throw @"
GitHub CLI (gh) was not found.
Install it, authenticate with `gh auth login`, then rerun this script.
"@
}

if (-not $SkipBuild) {
    # build_installer_windows.ps1 builds the portable payload first. That payload
    # contains Engine\ffmpeg.exe, but dist/ is gitignored so the binary never
    # enters repository history.
    $installerParams = @{}
    if (-not [string]::IsNullOrWhiteSpace($FfmpegPath)) {
        $installerParams.FfmpegPath = $FfmpegPath
    }
    & (Join-Path $Root "build_installer_windows.ps1") @installerParams
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($IncludePortableZip) {
        $portableParams = @{}
        if (-not [string]::IsNullOrWhiteSpace($FfmpegPath)) {
            $portableParams.FfmpegPath = $FfmpegPath
        }
        & (Join-Path $Root "build_portable_windows.ps1") @portableParams
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

$assets = @()
$setupExe = Join-Path $Dist "StemLab-Setup-$Version.exe"
if (Test-Path $setupExe -PathType Leaf) {
    $assets += (Get-Item $setupExe)
}

# Large Inno Setup builds may create data slices beside the setup EXE.
$assets += @(Get-ChildItem $Dist -File -Filter "StemLab-Setup-$Version*.bin" -ErrorAction SilentlyContinue | Sort-Object Name)

if ($IncludePortableZip) {
    $portableZip = Join-Path $Dist "StemLab-Windows.zip"
    if (Test-Path $portableZip -PathType Leaf) {
        $assets += (Get-Item $portableZip)
    }
}

if ($assets.Count -eq 0) {
    throw "No release assets were found in $Dist. Build the installer first."
}

Write-Host "Release assets:" -ForegroundColor Cyan
$assets | ForEach-Object {
    Write-Host ("  {0}  ({1:N1} MB)" -f $_.Name, ($_.Length / 1MB))
}

& $gh.Source release view $Tag *> $null
$releaseExists = ($LASTEXITCODE -eq 0)
$assetPaths = @($assets | ForEach-Object { $_.FullName })

if ($releaseExists) {
    Write-Host "Updating existing GitHub Release $Tag..." -ForegroundColor Cyan
    & $gh.Source release upload $Tag @assetPaths --clobber
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $gh.Source release edit $Tag --title $Title --notes-file $Notes --latest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
else {
    Write-Host "Creating GitHub Release $Tag..." -ForegroundColor Cyan
    & $gh.Source release create $Tag @assetPaths --title $Title --notes-file $Notes
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host ""
Write-Host "GitHub release is ready." -ForegroundColor Green
Write-Host "The source repository stays small; FFmpeg and the ML runtime are shipped only as Release assets."
