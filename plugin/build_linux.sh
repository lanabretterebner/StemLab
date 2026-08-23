#!/usr/bin/env bash
#
# Build the StemLab VST3 + Standalone targets on Linux.
#
# Mirrors build_windows.ps1: the pinned JUCE source is downloaded into
# .portable-cache/ and passed straight to CMake, so the build never depends on
# CMake's FetchContent git sub-build.
#
# Usage:
#   ./build_linux.sh
#   ./build_linux.sh --juce-source /path/to/JUCE     # use an existing checkout
#   ./build_linux.sh --build-type Debug
#   ./build_linux.sh --clean

set -euo pipefail

JUCE_VERSION="9.0.0"
BUILD_TYPE="Release"
JUCE_SOURCE=""
CLEAN=0

PLUGIN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$PLUGIN_ROOT")"
BUILD_DIR="$PLUGIN_ROOT/build"
CACHE_ROOT="$REPO_ROOT/.portable-cache"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --juce-version) JUCE_VERSION="$2"; shift 2 ;;
        --juce-source)  JUCE_SOURCE="$2";  shift 2 ;;
        --build-type)   BUILD_TYPE="$2";   shift 2 ;;
        --clean)        CLEAN=1;           shift ;;
        -h|--help)
            sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

JUCE_ZIP="$CACHE_ROOT/JUCE-$JUCE_VERSION.zip"
JUCE_EXTRACT_ROOT="$CACHE_ROOT/JUCE-$JUCE_VERSION"
JUCE_URL="https://github.com/juce-framework/JUCE/archive/refs/tags/$JUCE_VERSION.zip"

# ---------------------------------------------------------------- prerequisites

missing_tools=()
for tool in cmake pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing_tools+=("$tool")
done

if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
    missing_tools+=("g++")
fi

if [[ -z "$JUCE_SOURCE" ]] && ! command -v curl >/dev/null 2>&1 \
   && ! command -v wget >/dev/null 2>&1; then
    missing_tools+=("curl (or wget)")
fi

if [[ -z "$JUCE_SOURCE" ]] && ! command -v unzip >/dev/null 2>&1; then
    missing_tools+=("unzip")
fi

if [[ ${#missing_tools[@]} -gt 0 ]]; then
    echo "Missing build tools: ${missing_tools[*]}" >&2
    echo >&2
    echo "On Debian/Ubuntu:" >&2
    echo "  sudo apt install build-essential cmake pkg-config curl unzip" >&2
    exit 1
fi

# JUCE needs these headers. Checking now beats a 200-line CMake error later.
required_pkgs=(alsa x11 xext xi xrandr xinerama xcursor xcomposite xrender
                freetype2 fontconfig gl)
missing_pkgs=()

for pkg in "${required_pkgs[@]}"; do
    pkg-config --exists "$pkg" || missing_pkgs+=("$pkg")
done

if [[ ${#missing_pkgs[@]} -gt 0 ]]; then
    echo "Missing development packages for: ${missing_pkgs[*]}" >&2
    echo >&2
    echo "On Debian/Ubuntu, install all of them with:" >&2
    cat >&2 <<'EOF'
  sudo apt install \
    libasound2-dev libjack-jackd2-dev \
    libx11-dev libxext-dev libxi-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxcomposite-dev libxrender-dev \
    libfreetype-dev libfontconfig1-dev libgl1-mesa-dev

On Fedora:
  sudo dnf install alsa-lib-devel jack-audio-connection-kit-devel \
    libX11-devel libXext-devel libXi-devel libXrandr-devel libXinerama-devel \
    libXcursor-devel libXcomposite-devel libXrender-devel \
    freetype-devel fontconfig-devel mesa-libGL-devel

On Arch:
  sudo pacman -S --needed alsa-lib jack2 libx11 libxext libxi libxrandr \
    libxinerama libxcursor libxcomposite libxrender freetype2 fontconfig mesa
EOF
    exit 1
fi

# ------------------------------------------------------------------------ JUCE

download_juce() {
    mkdir -p "$CACHE_ROOT"

    for attempt in 1 2 3 4; do
        echo "Downloading JUCE $JUCE_VERSION (attempt $attempt/4)..."
        rm -f "$JUCE_ZIP.partial"

        if command -v curl >/dev/null 2>&1; then
            if curl -fsSL --retry 0 -o "$JUCE_ZIP.partial" "$JUCE_URL"; then
                mv "$JUCE_ZIP.partial" "$JUCE_ZIP"
                return 0
            fi
        else
            if wget -q -O "$JUCE_ZIP.partial" "$JUCE_URL"; then
                mv "$JUCE_ZIP.partial" "$JUCE_ZIP"
                return 0
            fi
        fi

        rm -f "$JUCE_ZIP.partial"
        [[ $attempt -eq 4 ]] && return 1

        delay=$((2 * attempt))
        echo "JUCE download failed. Retrying in ${delay}s..." >&2
        sleep "$delay"
    done
}

if [[ -n "$JUCE_SOURCE" ]]; then
    if [[ ! -f "$JUCE_SOURCE/CMakeLists.txt" ]]; then
        echo "--juce-source does not look like a JUCE tree: $JUCE_SOURCE" >&2
        exit 1
    fi
    JUCE_SOURCE="$(cd "$JUCE_SOURCE" && pwd)"
    echo "Using JUCE source: $JUCE_SOURCE"
else
    JUCE_SOURCE="$JUCE_EXTRACT_ROOT/JUCE-$JUCE_VERSION"

    if [[ ! -f "$JUCE_SOURCE/CMakeLists.txt" ]]; then
        [[ -f "$JUCE_ZIP" ]] || download_juce || {
            echo "Could not download JUCE $JUCE_VERSION." >&2
            echo "Pass an existing checkout with --juce-source instead." >&2
            exit 1
        }

        echo "Extracting JUCE $JUCE_VERSION..."
        rm -rf "$JUCE_EXTRACT_ROOT"
        mkdir -p "$JUCE_EXTRACT_ROOT"

        if ! unzip -q "$JUCE_ZIP" -d "$JUCE_EXTRACT_ROOT"; then
            # A truncated archive must not poison every later build.
            echo "JUCE archive is corrupt. Removing it - please rerun." >&2
            rm -f "$JUCE_ZIP"
            rm -rf "$JUCE_EXTRACT_ROOT"
            exit 1
        fi
    fi

    if [[ ! -f "$JUCE_SOURCE/CMakeLists.txt" ]]; then
        echo "JUCE extraction did not produce: $JUCE_SOURCE" >&2
        exit 1
    fi
fi

# ----------------------------------------------------------------------- build

[[ $CLEAN -eq 1 ]] && rm -rf "$BUILD_DIR"

echo "Configuring ($BUILD_TYPE)..."
cmake -S "$PLUGIN_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DSTEMLAB_JUCE_SOURCE_DIR="$JUCE_SOURCE"

echo "Building..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$(nproc)"

ARTEFACTS="$BUILD_DIR/StemLabPlugin_artefacts/$BUILD_TYPE"
[[ -d "$ARTEFACTS" ]] || ARTEFACTS="$BUILD_DIR/StemLabPlugin_artefacts"

echo
echo "Build complete."
echo
if [[ -d "$ARTEFACTS/VST3/StemLab.vst3" ]]; then
    echo "  VST3:       $ARTEFACTS/VST3/StemLab.vst3"
fi

if [[ -f "$ARTEFACTS/Standalone/StemLab" ]]; then
    echo "  Standalone: $ARTEFACTS/Standalone/StemLab"
fi
echo
echo "Install the plugin into ~/.vst3 with:"
echo "  $PLUGIN_ROOT/install_vst3.sh"
