param(
    [string]$JuceVersion = "9.0.0",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoRoot = Split-Path $PSScriptRoot -Parent
$PluginRoot = Join-Path $RepoRoot "src\plugin"
$BuildDir = Join-Path $PluginRoot "build"
$CacheRoot = Join-Path $RepoRoot ".portable-cache"
$JuceZip = Join-Path $CacheRoot "JUCE-$JuceVersion.zip"
$JuceExtractRoot = Join-Path $CacheRoot "JUCE-$JuceVersion"
$JuceSource = Join-Path $JuceExtractRoot "JUCE-$JuceVersion"
$JuceUrl = "https://github.com/juce-framework/JUCE/archive/refs/tags/$JuceVersion.zip"

$PluginRootPrefix = [System.IO.Path]::GetFullPath($PluginRoot).TrimEnd('\') + '\'
$ResolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not $ResolvedBuildDir.StartsWith($PluginRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe build directory: $ResolvedBuildDir"
}

function Assert-Exists([string]$Path, [string]$Message) {
    if (-not (Test-Path $Path)) {
        throw "$Message`nMissing: $Path"
    }
}

function Enter-VisualStudioShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $DevShell = $null
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

    if (Test-Path -LiteralPath $VsWhere -PathType Leaf) {
        $InstallPath = & $VsWhere `
            -latest `
            -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath

        if ($InstallPath) {
            $DevShell = Join-Path $InstallPath "Common7\Tools\Launch-VsDevShell.ps1"
        }
    }

    if (-not $DevShell -or -not (Test-Path -LiteralPath $DevShell -PathType Leaf)) {
        throw "Visual Studio with Desktop development with C++ was not found."
    }

    Write-Host "Loading the Visual Studio C++ build environment..."
    & $DevShell -Arch amd64 -HostArch amd64
}

function Download-FileWithRetry(
    [string]$Uri,
    [string]$Destination,
    [int]$Attempts = 4
) {
    $parent = Split-Path $Destination -Parent
    New-Item -ItemType Directory -Path $parent -Force | Out-Null

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Write-Host "Downloading JUCE $JuceVersion (attempt $attempt/$Attempts)..."
            Remove-Item "$Destination.partial" -Force -ErrorAction SilentlyContinue

            Invoke-WebRequest `
                -Uri $Uri `
                -OutFile "$Destination.partial" `
                -UseBasicParsing

            Move-Item "$Destination.partial" $Destination -Force
            return
        }
        catch {
            Remove-Item "$Destination.partial" -Force -ErrorAction SilentlyContinue

            if ($attempt -eq $Attempts) {
                throw
            }

            $delay = 2 * $attempt
            Write-Host "JUCE download failed. Retrying in $delay seconds..." -ForegroundColor Yellow
            Start-Sleep -Seconds $delay
        }
    }
}

Write-Host "Preparing pinned JUCE $JuceVersion..."
Enter-VisualStudioShell

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw "CMake was not found. Install the C++ CMake tools from Visual Studio Installer."
}

New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null

if (-not (Test-Path $JuceZip -PathType Leaf)) {
    Download-FileWithRetry $JuceUrl $JuceZip
}

if (-not (Test-Path (Join-Path $JuceSource "CMakeLists.txt") -PathType Leaf)) {
    Write-Host "Extracting JUCE..."
    Remove-Item $JuceExtractRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $JuceExtractRoot -Force | Out-Null

    try {
        Expand-Archive -Path $JuceZip -DestinationPath $JuceExtractRoot -Force
    }
    catch {
        # A partially/corruptly downloaded archive should not poison later runs.
        Remove-Item $JuceZip -Force -ErrorAction SilentlyContinue
        Remove-Item $JuceExtractRoot -Recurse -Force -ErrorAction SilentlyContinue
        throw "JUCE archive extraction failed. The cached ZIP was removed; rerun the build to redownload it.`n$($_.Exception.Message)"
    }
}

Assert-Exists (Join-Path $JuceSource "CMakeLists.txt") "JUCE source extraction failed."

Write-Host ""
Write-Host "Configuring StemLab VST3 + Standalone..."
Write-Host "Source: $PluginRoot"
Write-Host "Build:  $BuildDir"
Write-Host "JUCE:   $JuceSource"
Write-Host ""

if ($Clean -and (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    Write-Host "Removing the previous C++ build..."
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

cmake `
    -S $PluginRoot `
    -B $BuildDir `
    -A x64 `
    "-DSTEMLAB_JUCE_SOURCE_DIR=$JuceSource"

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Building Release..."
cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Running C++ grid tests..."
ctest --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Build complete."
Write-Host "Standalone:"
Write-Host "  $(Join-Path $BuildDir 'StemLabPlugin_artefacts\Release\Standalone\StemLab.exe')"
Write-Host "VST3:"
Write-Host "  $(Join-Path $BuildDir 'StemLabPlugin_artefacts\Release\VST3\StemLab.vst3')"
