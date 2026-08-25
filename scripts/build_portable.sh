#!/usr/bin/env bash
#
# Build the complete self-contained Linux package: Standalone app, VST3, and
# the full Python Engine in one directory. Nothing else to install - extract,
# run ./install.sh once (or just launch ./StemLab), and everything works.
#
#   dist/StemLab-<version>-Linux/
#     StemLab            Standalone application
#     StemLab.vst3/      VST3 bundle (install.sh copies it to ~/.vst3)
#     Engine/            relocatable Python runtime + ML dependencies
#     install.sh         VST3 install + engine discovery pointer
#     README.txt
#   dist/StemLab-<version>-Linux.tar.gz
#
# Usage:
#   ./scripts/build_portable.sh                     # auto GPU flavor
#   ./scripts/build_portable.sh --torch-flavor cpu  # cpu|cuda|rocm|xpu|auto
#   ./scripts/build_portable.sh --juce-source DIR   # reuse a JUCE checkout
#   ./scripts/build_portable.sh --no-tarball        # skip the .tar.gz

set -euo pipefail

TORCH_FLAVOR="auto"
JUCE_SOURCE=""
MAKE_TARBALL=1

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --torch-flavor)
            case "$2" in
                auto|cpu|cuda|rocm|xpu) TORCH_FLAVOR="$2" ;;
                *)
                    echo "Unknown torch flavor: $2 (use cpu|cuda|rocm|xpu|auto)" >&2
                    exit 2 ;;
            esac
            shift 2 ;;
        --juce-source)  JUCE_SOURCE="$2";  shift 2 ;;
        --no-tarball)   MAKE_TARBALL=0;    shift ;;
        -h|--help)
            sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

VERSION="$(sed -n 's/^version = "\(.*\)"/\1/p' "$REPO_ROOT/pyproject.toml" | head -1)"
[[ -n "$VERSION" ]] || { echo "Could not read version from pyproject.toml" >&2; exit 1; }

if [[ "$TORCH_FLAVOR" == "auto" ]]; then
    echo "NOTE: --torch-flavor auto resolves against THIS machine's GPU."
    echo "For a bundle you will hand to someone else, pass the flavor"
    echo "explicitly (--torch-flavor cpu|cuda|rocm|xpu)."
fi

# Assembled under a temporary name; renamed once the engine's recorded
# flavor is known, so the artifact name always states what it runs on.
DIST_DIR="$REPO_ROOT/dist/StemLab-$VERSION-Linux.building"

echo "Building StemLab $VERSION for Linux (torch flavor: $TORCH_FLAVOR)..."

# ---------------------------------------------------------------- 1. plugin

BUILD_ARGS=()
[[ -n "$JUCE_SOURCE" ]] && BUILD_ARGS+=(--juce-source "$JUCE_SOURCE")

"$REPO_ROOT/scripts/build_plugin.sh" ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}

ARTEFACTS="$REPO_ROOT/plugin/build/StemLabPlugin_artefacts/Release"

[[ -f "$ARTEFACTS/Standalone/StemLab" ]] || {
    echo "Standalone build missing at $ARTEFACTS/Standalone/StemLab" >&2
    exit 1
}

[[ -d "$ARTEFACTS/VST3/StemLab.vst3" ]] || {
    echo "VST3 build missing at $ARTEFACTS/VST3/StemLab.vst3" >&2
    exit 1
}

# -------------------------------------------------------------- 2. assemble

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cp "$ARTEFACTS/Standalone/StemLab" "$DIST_DIR/StemLab"
cp -r "$ARTEFACTS/VST3/StemLab.vst3" "$DIST_DIR/StemLab.vst3"

# ---------------------------------------------------------------- 3. engine

FLAVOR_ARGS=()
[[ "$TORCH_FLAVOR" == "auto" ]] || FLAVOR_ARGS+=("--$TORCH_FLAVOR")

# --no-pointer: assembling a distributable must not rewire the build
# machine's own engine discovery.
"$REPO_ROOT/scripts/install_backend.sh" \
    ${FLAVOR_ARGS[@]+"${FLAVOR_ARGS[@]}"} \
    --dest "$DIST_DIR/Engine" \
    --no-pointer

RESOLVED_FLAVOR="$(cat "$DIST_DIR/Engine/.stemlab-torch-flavor" 2>/dev/null || echo unknown)"

# ------------------------------------------------------------- 4. installer

cat > "$DIST_DIR/install.sh" <<'INSTALLER'
#!/usr/bin/env bash
# Install the StemLab VST3 for the current user and point plugin discovery at
# the bundled Engine. The Standalone app runs from this directory as-is.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CONFIG_HOME="${XDG_CONFIG_HOME:-}"
[[ "$CONFIG_HOME" == /* ]] || CONFIG_HOME="$HOME/.config"

# Never delete a working install unless its replacement is actually here.
[[ -d "$HERE/StemLab.vst3" && -d "$HERE/Engine" ]] || {
    echo "This folder is incomplete (StemLab.vst3 or Engine missing)." >&2
    echo "Nothing was changed." >&2
    exit 1
}

mkdir -p "$HOME/.vst3"
rm -rf "$HOME/.vst3/StemLab.vst3"
cp -r "$HERE/StemLab.vst3" "$HOME/.vst3/StemLab.vst3"

mkdir -p "$CONFIG_HOME/StemLab"
printf '%s\n' "$HERE/Engine/bin/python3" \
    > "$CONFIG_HOME/StemLab/portable_engine_path.txt"

cat <<EOF
StemLab installed.

  VST3:       $HOME/.vst3/StemLab.vst3
  Standalone: $HERE/StemLab
  Engine:     $HERE/Engine (referenced in place - do not delete this folder)

Rescan plug-ins in your DAW (REAPER: Options > Preferences > Plug-ins > VST >
Re-scan). If you move this folder later, run ./install.sh again.
EOF
INSTALLER
chmod +x "$DIST_DIR/install.sh"

cat > "$DIST_DIR/README.txt" <<EOF
StemLab $VERSION for Linux (self-contained, torch flavor: $RESOLVED_FLAVOR)

GPU support baked into this bundle: $RESOLVED_FLAVOR
  cuda = NVIDIA, rocm = AMD (RDNA2+), xpu = Intel Arc/Xe, cpu = none.
If it does not match your GPU, separation still works on CPU - rebuild or
re-run scripts/install_backend.sh with the right flavor for full speed.

1. Run ./install.sh once. It installs the VST3 to ~/.vst3 and points plugin
   discovery at the bundled Engine.
2. Rescan plug-ins in your DAW, or launch ./StemLab for the standalone app.

Everything StemLab needs is in this folder - no Python, venv, or model
runtime to install. Keep the folder where it is (or re-run ./install.sh
after moving it).

ffmpeg from your distribution is used for MP3/OGG/AIFF input:
  sudo apt install ffmpeg    (or your distribution's equivalent)
EOF

# ---------------------------------------------------------------- 5. verify

echo "Verifying the bundled Engine..."

PYTHONNOUSERSITE=1 "$DIST_DIR/Engine/bin/python3" -s - <<'PYCHECK'
import stemlab
import stemlab.recursive
import torch

print(f"  stemlab {stemlab.__name__}: ok")
print(f"  recursive splitting available: {stemlab.recursive.Separator is not None}")
print(f"  torch {torch.__version__}")
PYCHECK

# ---------------------------------------------------------------- 6. tarball

DIST_NAME="StemLab-$VERSION-Linux-$RESOLVED_FLAVOR"
FINAL_DIR="$REPO_ROOT/dist/$DIST_NAME"
rm -rf "$FINAL_DIR"
mv "$DIST_DIR" "$FINAL_DIR"
DIST_DIR="$FINAL_DIR"

if [[ $MAKE_TARBALL -eq 1 ]]; then
    echo "Creating $DIST_NAME.tar.gz (this is large)..."
    tar -C "$REPO_ROOT/dist" -czf "$REPO_ROOT/dist/$DIST_NAME.tar.gz" "$DIST_NAME"
fi

echo
echo "Portable build complete:"
echo "  $DIST_DIR"
[[ $MAKE_TARBALL -eq 1 ]] && echo "  $DIST_DIR.tar.gz"
du -sh "$DIST_DIR" | awk '{print "  size: " $1}'
