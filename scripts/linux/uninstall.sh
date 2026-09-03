#!/usr/bin/env bash
#
# Remove StemLab from this machine.
#
#   ./uninstall.sh                 all of it: app, Engine, VST3, settings,
#                                  model weights, caches
#   ./uninstall.sh --keep-models   ... but leave the downloaded weights, for
#                                  a reinstall that should not re-download
#   ./uninstall.sh --everything    ... and your captures, recordings and
#                                  separated stems as well
#
#   ./uninstall.sh --dry-run       print what would go; remove nothing
#   ./uninstall.sh --yes           do not ask
#
# The one thing the default never touches is audio: everything under
# <music>/StemLab is yours, and a DAW project may reference it by path.
# Removing StemLab must not silently break a session that opens next week.
# --everything is the only mode that takes it, and it says so first.
#
# THE LOAD-BEARING DETAIL. On Linux the app's data directory and its default
# install directory are the same folder: the bundle unpacks into
# ~/.local/share/StemLab, and older versions wrote Captures/ and Recordings/
# inside it. So the app is removed entry by entry rather than with one rm -rf,
# and anything in that folder which is not the app's is kept and named.
# Getting this wrong deletes recordings during a routine uninstall.
#
# THE OTHER ONE. Two of the caches are not ours alone. ~/.cache/huggingface
# and ~/.cache/torch/hub/checkpoints are where every torch application on the
# machine keeps its downloads, so only StemLab's own entries inside them are
# named and removed. An uninstaller that took the whole folder would delete
# somebody else's models, which is a worse outcome than leaving a few
# megabytes behind.

set -euo pipefail
shopt -s nullglob

KEEP_MODELS=0
SCOPE_EVERYTHING=0
DRY_RUN=0
ASSUME_YES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --keep-models) KEEP_MODELS=1;      shift ;;
        --everything)  SCOPE_EVERYTHING=1; shift ;;
        --dry-run)    DRY_RUN=1;          shift ;;
        --yes|-y)     ASSUME_YES=1;       shift ;;
        -h|--help)
            sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

die() { echo "$*" >&2; exit 1; }

# XDG with the same fallbacks the app uses. A relative XDG_* is invalid per
# the spec and is ignored rather than resolved against the working directory,
# which is where an rm would then go looking.
xdg() {
    local value="${!1:-}"
    [[ "$value" == /* ]] || value="$HOME/$2"
    printf '%s' "$value"
}

DATA_HOME="$(xdg XDG_DATA_HOME .local/share)"
CONFIG_HOME="$(xdg XDG_CONFIG_HOME .config)"
CACHE_HOME="$(xdg XDG_CACHE_HOME .cache)"

# Where the app writes captures, recordings and finished stems. XDG_MUSIC_DIR
# lives in user-dirs.dirs rather than the environment, and can be localised
# ("Musik"), so it is read the same way the app reads it.
music_dir() {
    local configured=""

    if [[ -f "$CONFIG_HOME/user-dirs.dirs" ]]; then
        configured="$(sed -n 's/^[[:space:]]*XDG_MUSIC_DIR=//p' "$CONFIG_HOME/user-dirs.dirs" \
            | tail -1 | tr -d '"' | sed "s|^\$HOME|$HOME|")"
    fi

    [[ -n "$configured" && "$configured" == /* ]] || configured="$HOME/Music"

    printf '%s' "$configured"
}

MEDIA_HOME="$(music_dir)/StemLab"

# What the bundle lays down. install_backend.sh on its own creates only
# Engine/, so a source install is this list minus the app.
BUNDLE_ENTRIES=(
    Engine
    StemLab
    StemLab.vst3
    icons
    install.sh
    uninstall.sh
    update.sh
    README.txt
    .stemlab-version
)

# What the running app writes back into that same folder, which the bundle
# never contained and an entry-by-entry removal would otherwise mistake for
# the user's. models/ is src/stemlab/paths.py's recursive_models_dir; Ableton/
# is StemLabPaths' remoteStatusDirectory.
RUNTIME_ENTRIES=(Ableton)
MODEL_ENTRIES=(models)

APP_ENTRIES=("${BUNDLE_ENTRIES[@]}" "${RUNTIME_ENTRIES[@]}")

[[ $KEEP_MODELS -eq 1 ]] || APP_ENTRIES+=("${MODEL_ENTRIES[@]}")

# ------------------------------------------------------- finding the install

# One location, so there is nothing to look up. STEMLAB_INSTALL_DIR is still
# honoured for an install built somewhere else with --dest.
installed_dir=""
if [[ -n "${STEMLAB_INSTALL_DIR:-}" ]]; then
    installed_dir="$STEMLAB_INSTALL_DIR"
fi

[[ -n "$installed_dir" ]] || installed_dir="$DATA_HOME/StemLab"

# Only a folder that is recognisably ours is touched. A source install has the
# Engine and its marker; a bundle also has the VST3. Anything else - a home
# directory, a path someone pointed STEMLAB_INSTALL_DIR at by mistake - is
# left alone and named rather than deleted.
looks_like_stemlab() {
    local dir="$1"

    [[ -n "$dir" && "$dir" == /* && -d "$dir" ]] || return 1
    [[ -f "$dir/Engine/.stemlab-engine" || -d "$dir/StemLab.vst3" ]] || return 1

    return 0
}

# --------------------------------------------------------- what will be gone

# Parallel arrays rather than an associative one: bash 3 still ships on enough
# machines to be worth not depending on, and the order here is the order the
# summary prints in.
targets=()
labels=()

consider() {
    local path="$1" label="$2"

    [[ -n "$path" && "$path" == /* ]] || return 0
    [[ -e "$path" ]] || return 0

    targets+=("$path")
    labels+=("$label")
}

install_refused=""
kept_in_install=()

if looks_like_stemlab "$installed_dir"; then
    if [[ $SCOPE_EVERYTHING -eq 1 && $KEEP_MODELS -eq 0 ]]; then
        consider "$installed_dir" "the app, its Engine, and everything beside them"
    else
        for entry in "${APP_ENTRIES[@]}"; do
            consider "$installed_dir/$entry" "part of the app"
        done
    fi
elif [[ -d "$installed_dir" ]]; then
    install_refused="$installed_dir"
fi

consider "$HOME/.vst3/StemLab.vst3" "the VST3 plug-in"

# What install.sh put outside the install folder so that a launcher could find
# the app: the menu entry, and the icon it names, filed into the icon theme at
# each size. Named one by one rather than by their directories - hicolor holds
# every application's icons, and only the files called stemlab are ours.
consider "$DATA_HOME/applications/stemlab.desktop" "the applications-menu entry"

for icon_size in 16 32 48 64 128 256; do
    consider "$DATA_HOME/icons/hicolor/${icon_size}x${icon_size}/apps/stemlab.png" \
        "the launcher icon"
done

consider "$DATA_HOME/icons/hicolor/scalable/apps/stemlab.svg" "the launcher icon"

consider "$CONFIG_HOME/StemLab" "settings: the torch-compile preference"

# Everything of ours under the cache directory, in one target: the analysis
# database, the compiled kernels and their warm-up markers, the jobs folder
# older installs kept here, and setup/ - where the setup script stages a
# download of several gigabytes and leaves it on purpose when a run fails, so
# that re-running resumes. All of it derived or installer debris; none of it
# audio, which lives under the music folder.
consider "$CACHE_HOME/StemLab" "analysis cache, compiled kernels, installer staging"

# Only when it has been pointed somewhere else, since the line above covers
# the default.
if [[ -n "${STEMLAB_ANALYSIS_HOME:-}" ]]; then
    consider "$STEMLAB_ANALYSIS_HOME" "analysis, MIDI staging, compiled kernels"
fi

# Nothing writes ~/.stemlab any more - the analysis cache and the model
# weights moved to directories that say what they are and that a disk-cleaning
# tool can reason about. Anyone who ran an older StemLab still has the old one,
# holding gigabytes nothing will ever read again.
consider "$HOME/.stemlab" "the directory older versions kept both in"

# JUCE resolves its temp directory through TMPDIR before falling back to /tmp,
# so this has to as well or it misses the folder on any machine that sets one.
temp_root="${TMPDIR:-}"
[[ "$temp_root" == /* ]] || temp_root="/tmp"

consider "$temp_root/StemLab" "temporary files"

if [[ $KEEP_MODELS -eq 0 ]]; then
    consider "$CACHE_HOME/bs-roformer-infer" "BS-RoFormer weights"

    # Named entries inside shared caches, never the caches themselves. See the
    # note at the top: other torch applications keep their downloads here.
    hf_cache="${HF_HUB_CACHE:-}"

    if [[ "$hf_cache" != /* ]]; then
        hf_home="${HF_HOME:-}"
        [[ "$hf_home" == /* ]] || hf_home="$CACHE_HOME/huggingface"
        hf_cache="$hf_home/hub"
    fi

    consider "$hf_cache/models--adefossez--HTDemucs-6s" "Demucs weights (HuggingFace copy)"

    torch_home="${TORCH_HOME:-}"
    [[ "$torch_home" == /* ]] || torch_home="$CACHE_HOME/torch"

    consider "$torch_home/hub/checkpoints/5c90dfd2-34c22ccb.th" "Demucs weights (torch hub copy)"
fi

if [[ $SCOPE_EVERYTHING -eq 1 ]]; then
    consider "$MEDIA_HOME" "your captures, recordings and separated stems"
fi

# Whatever else lives in the install directory is the user's - Captures and
# Recordings from an install predating the move to the music folder - but the
# rule is "not the app's, and not already going" rather than a second list.
#
# The model weights are the app's whether or not this run takes them, so they
# are excluded here even under --keep-models: listing them as "not the app's"
# would say the opposite of what is true.
#
# Computed after every consider above rather than beside the first one,
# because those weights live inside the install directory, and without this
# ordering one run printed them as removed and as kept.
if ! [[ $SCOPE_EVERYTHING -eq 1 && $KEEP_MODELS -eq 0 ]] \
    && looks_like_stemlab "$installed_dir"; then
    for path in "$installed_dir"/* "$installed_dir"/.[!.]*; do
        name="${path##*/}"
        mine=0

        for entry in "${APP_ENTRIES[@]}" "${MODEL_ENTRIES[@]}"; do
            [[ "$name" == "$entry" ]] && mine=1 && break
        done

        for target in ${targets[@]+"${targets[@]}"}; do
            [[ "$path" == "$target" ]] && mine=1 && break
        done

        [[ $mine -eq 1 ]] || kept_in_install+=("$path")
    done
fi

if [[ ${#targets[@]} -eq 0 ]]; then
    echo "Nothing to remove - StemLab is not installed for this user."

    if [[ -n "$install_refused" ]]; then
        echo
        echo "$install_refused exists but does not look like a StemLab install,"
        echo "so it was left alone. Remove it by hand if it is one."
    fi

    exit 0
fi

echo "This will remove:"
echo

for index in "${!targets[@]}"; do
    size="$(du -sh "${targets[$index]}" 2>/dev/null | cut -f1 || true)"
    printf '  %-6s %s\n' "${size:--}" "${targets[$index]}"
    printf '         %s\n' "${labels[$index]}"
done

if [[ ${#kept_in_install[@]} -gt 0 ]]; then
    echo
    echo "Kept in $installed_dir (not the app's):"

    for path in "${kept_in_install[@]}"; do
        size="$(du -sh "$path" 2>/dev/null | cut -f1 || true)"
        printf '  %-6s %s\n' "${size:--}" "$path"
    done
fi

echo

if [[ $KEEP_MODELS -eq 1 ]]; then
    echo "Model weights are kept, so a reinstall will not download them again."
fi

if [[ $SCOPE_EVERYTHING -eq 0 ]]; then
    echo "Your captures, recordings and separated stems in"
    echo "$MEDIA_HOME are kept - a DAW project may still point at them."
else
    echo "This includes audio you made. There is no undo."
fi

if [[ -n "$install_refused" ]]; then
    echo
    echo "Left alone: $install_refused does not look like a StemLab install."
fi

if [[ $DRY_RUN -eq 1 ]]; then
    echo
    echo "Dry run - nothing was removed."
    exit 0
fi

if [[ $ASSUME_YES -eq 0 ]]; then
    echo
    read -r -p "Remove these? [y/N] " reply

    case "$reply" in
        y|Y|yes|YES) ;;
        *) echo "Nothing was removed."; exit 0 ;;
    esac
fi

# ------------------------------------------------------------------ removal

# The install location can be a system directory - the setup script supports
# installing there with sudo - so removing it may need the same. Everything
# else is per-user by construction and never does.
remove() {
    local path="$1"

    if [[ -w "$(dirname "$path")" ]]; then
        rm -rf "$path"
        return
    fi

    command -v sudo >/dev/null 2>&1 \
        || die "Removing $path needs root, and sudo is not installed."

    echo "Removing $path needs root (sudo may ask for your password)..."
    sudo rm -rf "$path"
}

for path in "${targets[@]}"; do
    remove "$path"
done

# Tidy up only what is provably empty: rmdir refusing a non-empty directory is
# exactly the check wanted, and ~/.vst3 and the install folder both belong to
# more than StemLab.
rmdir "$HOME/.vst3" 2>/dev/null || true
rmdir "$installed_dir" 2>/dev/null || true

# A desktop that caches its menu keeps showing an entry whose file is gone
# until it is told otherwise. Best effort, as in install.sh.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DATA_HOME/applications" >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -qtf "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true
fi

echo
echo "StemLab is removed."

if [[ ${#kept_in_install[@]} -gt 0 ]]; then
    echo "Your files are still in $installed_dir."
fi

if [[ $KEEP_MODELS -eq 1 ]]; then
    echo "Model weights are still on disk, as asked."
fi

echo "Rescan plug-ins in your DAW so it forgets the VST3."
