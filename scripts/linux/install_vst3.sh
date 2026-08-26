#!/usr/bin/env bash
#
# Install the freshly built StemLab VST3 bundle for the current user.
#
# Usage:
#   ./scripts/install_vst3.sh
#   ./scripts/install_vst3.sh --build-type Debug
#   ./scripts/install_vst3.sh --prefix /usr/local/lib/vst3   # system-wide

set -euo pipefail

BUILD_TYPE="Release"
DEST_DIR="${HOME}/.vst3"

PLUGIN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src/plugin" && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --prefix)     DEST_DIR="$2";   shift 2 ;;
        -h|--help)
            sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

SOURCE="$PLUGIN_ROOT/build/StemLabPlugin_artefacts/$BUILD_TYPE/VST3/StemLab.vst3"

if [[ ! -d "$SOURCE" ]]; then
    SOURCE="$PLUGIN_ROOT/build/StemLabPlugin_artefacts/VST3/StemLab.vst3"
fi

if [[ ! -d "$SOURCE" ]]; then
    echo "Build StemLab first. Could not find the VST3 bundle at:" >&2
    echo "  $PLUGIN_ROOT/build/StemLabPlugin_artefacts/$BUILD_TYPE/VST3/StemLab.vst3" >&2
    echo >&2
    echo "Run: $(dirname "$PLUGIN_ROOT")/scripts/build_plugin.sh" >&2
    exit 1
fi

DEST="$DEST_DIR/StemLab.vst3"

mkdir -p "$DEST_DIR"

if [[ -e "$DEST" ]]; then
    echo "Removing previously installed StemLab.vst3..."
    rm -rf "$DEST"
fi

echo "Installing StemLab.vst3..."
cp -r "$SOURCE" "$DEST"

MODULE="$DEST/Contents/x86_64-linux/StemLab.so"

if [[ ! -f "$MODULE" ]]; then
    # Non-x86_64 hosts land here; report whatever the bundle actually holds.
    MODULE="$(find "$DEST/Contents" -name 'StemLab.so' -print -quit 2>/dev/null || true)"
fi

if [[ -z "$MODULE" || ! -f "$MODULE" ]]; then
    echo "VST3 install verification failed: no StemLab.so inside $DEST" >&2
    exit 1
fi

cat <<EOF

Installed StemLab VST3 0.9.9:
  $DEST

Verified module:
  $MODULE

Rescan VST3 plug-ins in your host.
In REAPER: Options > Preferences > Plug-ins > VST > Re-scan.
Make sure $DEST_DIR is in the VST plug-in path list.
EOF
