#!/usr/bin/env bash
#
# Build the StemLab VST3 + Standalone targets on Linux.
#
# Mirrors scripts/win/build_plugin.ps1: the pinned JUCE source is downloaded
# into .portable-cache/ and passed straight to CMake, so the build never
# depends on CMake's FetchContent git sub-build.
#
# Usage:
#   ./scripts/linux/build_plugin.sh
#   ./scripts/linux/build_plugin.sh --juce-source /path/to/JUCE  # existing checkout
#   ./scripts/linux/build_plugin.sh --juce-version 9.0.0         # another JUCE tag
#   ./scripts/linux/build_plugin.sh --build-type Debug
#   ./scripts/linux/build_plugin.sh --clean
#   ./scripts/linux/build_plugin.sh --no-tests   # plugin targets only

set -euo pipefail

JUCE_VERSION="9.0.0"
BUILD_TYPE="Release"
JUCE_SOURCE=""
CLEAN=0
NO_TESTS=0

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLUGIN_ROOT="$REPO_ROOT/src/plugin"
BUILD_DIR="$PLUGIN_ROOT/build"
CACHE_ROOT="$REPO_ROOT/.portable-cache"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --juce-version) JUCE_VERSION="$2"; shift 2 ;;
        --juce-source)  JUCE_SOURCE="$2";  shift 2 ;;
        --build-type)   BUILD_TYPE="$2";   shift 2 ;;
        --clean)        CLEAN=1;           shift ;;
        --no-tests)     NO_TESTS=1;        shift ;;
        -h|--help)
            sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
                freetype2 fontconfig gl libpulse-simple)
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
    libfreetype-dev libfontconfig1-dev libgl1-mesa-dev libpulse-dev

On Fedora:
  sudo dnf install alsa-lib-devel jack-audio-connection-kit-devel \
    libX11-devel libXext-devel libXi-devel libXrandr-devel libXinerama-devel \
    libXcursor-devel libXcomposite-devel libXrender-devel \
    freetype-devel fontconfig-devel mesa-libGL-devel pulseaudio-libs-devel

On Arch:
  sudo pacman -S --needed alsa-lib jack2 libx11 libxext libxi libxrandr \
    libxinerama libxcursor libxcomposite libxrender freetype2 fontconfig mesa libpulse
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

extract_juce() {
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
}

if [[ -n "$JUCE_SOURCE" ]]; then
    if [[ ! -f "$JUCE_SOURCE/CMakeLists.txt" ]]; then
        echo "--juce-source does not look like a JUCE tree: $JUCE_SOURCE" >&2
        exit 1
    fi
    JUCE_SOURCE="$(cd "$JUCE_SOURCE" && pwd)"
    JUCE_TREE_IS_OURS=0
    echo "Using JUCE source: $JUCE_SOURCE"
else
    JUCE_TREE_IS_OURS=1
    JUCE_SOURCE="$JUCE_EXTRACT_ROOT/JUCE-$JUCE_VERSION"

    if [[ ! -f "$JUCE_SOURCE/CMakeLists.txt" ]]; then
        [[ -f "$JUCE_ZIP" ]] || download_juce || {
            echo "Could not download JUCE $JUCE_VERSION." >&2
            echo "Pass an existing checkout with --juce-source instead." >&2
            exit 1
        }

        extract_juce
    fi

    if [[ ! -f "$JUCE_SOURCE/CMakeLists.txt" ]]; then
        echo "JUCE extraction did not produce: $JUCE_SOURCE" >&2
        exit 1
    fi
fi

# JUCE's stock X11 drag source keeps `dragging` set until the target answers
# XdndFinished, so a target that never does blocks every later drag for the
# life of the process. The patch ends the gesture when the drop is sent.
DND_PATCH="$REPO_ROOT/scripts/linux/juce-linux-dnd.patch"
DND_SOURCE="$JUCE_SOURCE/modules/juce_gui_basics/native/juce_DragAndDrop_linux.cpp"

# --dry-run first, so a mismatch cannot leave a half-patched tree behind in
# the build cache, where it would poison every later build.
dnd_patch_applies() { patch -p1 -d "$JUCE_SOURCE" --dry-run <"$DND_PATCH" >/dev/null 2>&1; }
dnd_patch_in_place() { patch -p1 -R -d "$JUCE_SOURCE" --dry-run <"$DND_PATCH" >/dev/null 2>&1; }

apply_dnd_patch() {
    echo "Applying the drag-and-drop fix to JUCE..."
    if ! dnd_patch_applies || ! patch -p1 -d "$JUCE_SOURCE" <"$DND_PATCH" >/dev/null; then
        echo "Could not patch JUCE's drag-and-drop source." >&2
        exit 1
    fi
}

if [[ -f "$DND_PATCH" && -f "$DND_SOURCE" ]]; then
    if dnd_patch_in_place; then
        : # this exact patch is already in the tree
    elif grep -q "STEMLAB_DND_PATCH" "$DND_SOURCE"; then
        # An older version of our own patch. Testing only for the marker would
        # accept it as current and quietly build the superseded fix - which is
        # what a cached JUCE tree from an earlier release carries.
        if [[ $JUCE_TREE_IS_OURS -eq 1 ]]; then
            echo "Replacing a superseded drag-and-drop fix in the cached JUCE..."
            [[ -f "$JUCE_ZIP" ]] || download_juce || {
                echo "Could not download JUCE $JUCE_VERSION to re-patch it." >&2
                exit 1
            }
            extract_juce
            apply_dnd_patch
        else
            echo "$JUCE_SOURCE carries an older StemLab drag-and-drop patch." >&2
            echo "Reset that checkout (git -C \"$JUCE_SOURCE\" checkout modules) and rerun." >&2
            exit 1
        fi
    else
        apply_dnd_patch
    fi
fi

# ----------------------------------------------------------------------- build

[[ $CLEAN -eq 1 ]] && rm -rf "$BUILD_DIR"

# CMakeLists.txt guards the CTest targets with BUILD_TESTING, so switching it
# off leaves the seven test executables unbuilt - one of which links the whole
# plugin and is therefore link-time optimised like it. That is minutes of a
# release build that never runs a test.
#
# Passed on every run rather than only with --no-tests: CMake remembers it in
# the build directory's cache, so a plain run after a --no-tests one has to
# ask for the tests back explicitly or silently keep skipping them.
BUILD_TESTING="ON"
[[ $NO_TESTS -eq 1 ]] && BUILD_TESTING="OFF"

echo "Configuring ($BUILD_TYPE)..."
cmake -S "$PLUGIN_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTING="$BUILD_TESTING" \
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
echo "For an installable, self-contained build run:"
echo "  $REPO_ROOT/scripts/linux/build.sh"
