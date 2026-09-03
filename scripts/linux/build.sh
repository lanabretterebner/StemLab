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
#     install.sh         VST3 install + Engine into ~/.local/share/StemLab
#     README.txt
#   dist/StemLab-<version>-Linux.tar.gz
#
# Usage:
#   ./scripts/linux/build.sh                     # auto GPU flavor
#   ./scripts/linux/build.sh --torch-flavor cpu  # cpu|cuda|rocm|xpu|auto
#   ./scripts/linux/build.sh --juce-source DIR   # reuse a JUCE checkout
#   ./scripts/linux/build.sh --no-tarball        # skip the .tar.gz
#   ./scripts/linux/build.sh --skip-plugin-build # reuse already-built artefacts

set -euo pipefail

TORCH_FLAVOR="auto"
JUCE_SOURCE=""
MAKE_TARBALL=1
SKIP_PLUGIN_BUILD=0

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

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
        --skip-plugin-build) SKIP_PLUGIN_BUILD=1; shift ;;
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

ARTEFACTS="$REPO_ROOT/src/plugin/build/StemLabPlugin_artefacts/Release"

# --skip-plugin-build trusts whatever is already in $ARTEFACTS (the release
# workflow builds the flavor-independent binaries once and extracts them
# there); the checks below still refuse an empty or partial directory.
if [[ $SKIP_PLUGIN_BUILD -eq 1 ]]; then
    echo "Skipping the plugin build; using artefacts in $ARTEFACTS"
else
    BUILD_ARGS=()
    [[ -n "$JUCE_SOURCE" ]] && BUILD_ARGS+=(--juce-source "$JUCE_SOURCE")

    "$REPO_ROOT/scripts/linux/build_plugin.sh" ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}
fi

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

# The application icon, at the sizes an icon theme is indexed by. A Linux
# executable has nowhere to carry one the way a .exe or an .app bundle does,
# so it ships beside the app and install.sh files it into the icon theme.
cp -r "$REPO_ROOT/src/plugin/Resources/icons" "$DIST_DIR/icons"

# ---------------------------------------------------------------- 3. engine

FLAVOR_ARGS=()
[[ "$TORCH_FLAVOR" == "auto" ]] || FLAVOR_ARGS+=("--$TORCH_FLAVOR")

# --build-only: assembling a distributable must not touch the build
# machine's own install.
"$REPO_ROOT/scripts/linux/install_backend.sh" \
    ${FLAVOR_ARGS[@]+"${FLAVOR_ARGS[@]}"} \
    --dest "$DIST_DIR/Engine" \
    --build-only

RESOLVED_FLAVOR="$(cat "$DIST_DIR/Engine/.stemlab-torch-flavor" 2>/dev/null || echo unknown)"

# ------------------------------------------------------------- 4. installer

cat > "$DIST_DIR/install.sh" <<'INSTALLER'
#!/usr/bin/env bash
# Install the StemLab VST3 and its Engine for the current user.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DATA_HOME="${XDG_DATA_HOME:-}"
[[ "$DATA_HOME" == /* ]] || DATA_HOME="$HOME/.local/share"

# Never delete a working install unless its replacement is actually here.
[[ -d "$HERE/StemLab.vst3" && -d "$HERE/Engine" ]] || {
    echo "This folder is incomplete (StemLab.vst3 or Engine missing)." >&2
    echo "Nothing was changed." >&2
    exit 1
}

mkdir -p "$HOME/.vst3"
rm -rf "$HOME/.vst3/StemLab.vst3"
cp -r "$HERE/StemLab.vst3" "$HOME/.vst3/StemLab.vst3"

# The Engine moves to the one location StemLab reads. It used to be left in
# this folder and pointed at by a file in ~/.config, which meant the install
# depended on the download folder never being moved or emptied - and that
# whichever of several extracted copies wrote the pointer last won. There is
# no pointer now: the app looks here and only here.
#
# Moved rather than copied. It is gigabytes, and a bundle that has been
# installed has no further use for its own copy.
#
# STEMLAB_INSTALL_DIR is read here for the same reason the setup script reads
# it: that script moves the extracted folder to whatever the variable names
# and then runs this one from inside it. With the location hardcoded, $HERE
# and $INSTALL_DIR disagree on every custom install - so this script moves the
# Engine out of the install the user asked for and over the top of the default
# one, deleting whatever was there. Under the sudo path it does not even get
# that far: the move runs as the invoking user and fails outright.
INSTALL_DIR="${STEMLAB_INSTALL_DIR:-$DATA_HOME/StemLab}"

# Two ways this script is reached, and they need different things.
#
# The setup script moves the extracted folder to $INSTALL_DIR and runs this
# from inside it, so everything is already exactly where it belongs and the
# only work left is the VST3. Moving anything here would mean moving it onto
# itself - which, with the rm -rf that a real move needs, deletes the Engine.
#
# Someone who downloaded and extracted the tarball by hand runs this from
# wherever they put it, and then it has to install.
if [[ "$HERE" != "$INSTALL_DIR" ]]; then
    mkdir -p "$INSTALL_DIR"

    rm -rf "$INSTALL_DIR/Engine"
    mv "$HERE/Engine" "$INSTALL_DIR/Engine"

    # The app and the two scripts that manage it, so the extracted folder is
    # not the only copy of the uninstaller. uninstall.sh already expects to
    # find them here.
    for item in StemLab icons uninstall.sh update.sh README.txt .stemlab-version; do
        [[ -e "$HERE/$item" ]] && cp -r "$HERE/$item" "$INSTALL_DIR/$item"
    done
fi

chmod +x "$INSTALL_DIR/StemLab" "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/update.sh" 2>/dev/null || true

# What puts StemLab in the applications menu with its own icon.
#
# The window publishes _NET_WM_ICON itself, which is enough for the icon a
# taskbar draws for a window that is already open. A desktop entry is a
# different thing: it is the only way a launcher knows the app exists at all,
# and StartupWMClass is what ties the running window back to that entry, so a
# dock shows one StemLab rather than a pinned launcher beside an unknown
# window. JUCE sets WM_CLASS from the application name, which is "StemLab".
#
# Per-user, under $DATA_HOME, because that is where the rest of this install
# goes; nothing here needs root.
for size in 16 32 48 64 128 256; do
    icon="$INSTALL_DIR/icons/stemlab-$size.png"
    [[ -f "$icon" ]] || continue

    mkdir -p "$DATA_HOME/icons/hicolor/${size}x${size}/apps"
    cp "$icon" "$DATA_HOME/icons/hicolor/${size}x${size}/apps/stemlab.png"
done

if [[ -f "$INSTALL_DIR/icons/stemlab.svg" ]]; then
    mkdir -p "$DATA_HOME/icons/hicolor/scalable/apps"
    cp "$INSTALL_DIR/icons/stemlab.svg" \
        "$DATA_HOME/icons/hicolor/scalable/apps/stemlab.svg"
fi

mkdir -p "$DATA_HOME/applications"

# Exec is quoted because STEMLAB_INSTALL_DIR can point anywhere, including a
# path with a space in it, and the desktop entry spec parses Exec as a
# command line rather than as a single path.
cat > "$DATA_HOME/applications/stemlab.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Version=1.0
Name=StemLab
GenericName=Stem Separation
Comment=Capture, separate, and export stems to Ableton Live
Exec="$INSTALL_DIR/StemLab"
Icon=stemlab
Terminal=false
Categories=AudioVideo;Audio;
Keywords=stems;separation;acapella;instrumental;karaoke;
StartupNotify=true
StartupWMClass=StemLab
DESKTOP

# Best effort, both of them: a desktop that keeps these caches wants to be
# told, and one that does not have the commands reads the files directly.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DATA_HOME/applications" >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -qtf "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true
fi

cat <<EOF
StemLab installed.

  VST3:       $HOME/.vst3/StemLab.vst3
  Standalone: $INSTALL_DIR/StemLab
  Engine:     $INSTALL_DIR/Engine
  Menu entry: $DATA_HOME/applications/stemlab.desktop
  Update:     $INSTALL_DIR/update.sh
  Uninstall:  $INSTALL_DIR/uninstall.sh

Rescan plug-ins in your DAW (REAPER: Options > Preferences > Plug-ins > VST >
Re-scan). If you extracted this by hand somewhere else, that folder can be
deleted now - everything in it has been installed to $INSTALL_DIR.
EOF
INSTALLER
chmod +x "$DIST_DIR/install.sh"

# The version, for anything that needs to know what this bundle is without
# parsing prose. update.sh reads it to decide whether a release is newer.
printf '%s\n' "$VERSION" > "$DIST_DIR/.stemlab-version"

# Removing and updating ship with the app: a user who wants either should not
# have to find the repository to get it.
cp "$REPO_ROOT/scripts/linux/uninstall.sh" "$DIST_DIR/uninstall.sh"
cp "$REPO_ROOT/scripts/linux/update.sh" "$DIST_DIR/update.sh"
chmod +x "$DIST_DIR/uninstall.sh" "$DIST_DIR/update.sh"

cat > "$DIST_DIR/README.txt" <<EOF
StemLab $VERSION for Linux (self-contained, torch flavor: $RESOLVED_FLAVOR)

GPU support baked into this bundle: $RESOLVED_FLAVOR
  cuda = NVIDIA, rocm = AMD (RDNA2+), xpu = Intel Arc/Xe, cpu = none.
If it does not match your GPU, separation still works on CPU - rebuild or
re-run scripts/linux/install_backend.sh with the right flavor for full speed.

1. Run ./install.sh once. It moves the app and its Engine into
   ~/.local/share/StemLab and installs the VST3 to ~/.vst3.
2. Rescan plug-ins in your DAW, or launch ~/.local/share/StemLab/StemLab for
   the standalone app.

Everything StemLab needs to run is in this folder - no Python, venv, or model
runtime to install. Once install.sh has run, this folder has been emptied into
~/.local/share/StemLab and can be deleted; update.sh and uninstall.sh are
installed alongside the app.

Model weights are not included. Each downloads the first time you use the
model that needs it, is checked against a recorded digest before it is
trusted, and is named in the status area while it downloads. The first
separation is therefore slower than the ones after it.

ffmpeg from your distribution is used for MP3/OGG/AIFF input:
  sudo apt install ffmpeg    (or your distribution's equivalent)
EOF

# There is no verification step of our own here any more. install_backend.sh
# ends by importing stemlab, stemlab.recursive, torch, torchaudio and soxr in
# this very Engine and comparing the torch and torchaudio versions, and
# nothing between that call and this line touches the Engine - so a check here
# could only re-prove a subset of what has already been proved, an interpreter
# startup later.

# --------------------------------------------------------------- 5. tarball

DIST_NAME="StemLab-$VERSION-Linux-$RESOLVED_FLAVOR"
FINAL_DIR="$REPO_ROOT/dist/$DIST_NAME"
rm -rf "$FINAL_DIR"
mv "$DIST_DIR" "$FINAL_DIR"
DIST_DIR="$FINAL_DIR"

if [[ $MAKE_TARBALL -eq 1 ]]; then
    echo "Creating $DIST_NAME.tar.gz (this is large)..."
    # A GPU bundle runs to 15GB and gzip is single-threaded; pigz compresses
    # on every core and emits the same .tar.gz format, so the consumers
    # (setup script, sha256, split) cannot tell the difference.
    tar -C "$REPO_ROOT/dist" -I "$(command -v pigz || echo gzip)" \
        -cf "$REPO_ROOT/dist/$DIST_NAME.tar.gz" "$DIST_NAME"
fi

echo
echo "Bundle complete:"
echo "  $DIST_DIR"
[[ $MAKE_TARBALL -eq 1 ]] && echo "  $DIST_DIR.tar.gz"
du -sh "$DIST_DIR" | awk '{print "  size: " $1}'
