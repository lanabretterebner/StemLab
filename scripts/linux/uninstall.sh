#!/usr/bin/env bash
#
# Remove StemLab from this machine, in three widening steps.
#
#   ./uninstall.sh                 the app: its files, the VST3, the settings
#   ./uninstall.sh --models        ... and the model weights and analysis cache
#   ./uninstall.sh --everything    ... and your captures, recordings and jobs
#
#   ./uninstall.sh --dry-run       print what would go; remove nothing
#   ./uninstall.sh --yes           do not ask
#
# The default keeps the model weights on purpose: they are gigabytes over a
# slow download, shared with nothing else, so an uninstall that took them
# costs an hour to undo. --everything is the only mode that touches audio you
# made, and it says so before it does.
#
# THE LOAD-BEARING DETAIL. On Linux the app's data directory and its default
# install directory are the same folder: the bundle unpacks into
# ~/.local/share/StemLab, and Captures/ and Recordings/ are written inside it.
# So the app is removed entry by entry rather than with one rm -rf, and
# anything in that folder which is not the app's is kept and named. Getting
# this wrong deletes recordings during a routine uninstall.

set -euo pipefail
shopt -s nullglob

SCOPE_MODELS=0
SCOPE_EVERYTHING=0
DRY_RUN=0
ASSUME_YES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --models)     SCOPE_MODELS=1;     shift ;;
        --everything) SCOPE_MODELS=1; SCOPE_EVERYTHING=1; shift ;;
        --dry-run)    DRY_RUN=1;          shift ;;
        --yes|-y)     ASSUME_YES=1;       shift ;;
        -h|--help)
            sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

# Everything the bundle lays down, and nothing else. install_backend.sh on its
# own creates only Engine/, so a source install is the same list minus the app.
BUNDLE_ENTRIES=(
    Engine
    StemLab
    StemLab.vst3
    install.sh
    uninstall.sh
    update.sh
    README.txt
    .stemlab-version
)

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
    if [[ $SCOPE_EVERYTHING -eq 1 ]]; then
        consider "$installed_dir" "the app, its Engine, and everything beside them"
    else
        for entry in "${BUNDLE_ENTRIES[@]}"; do
            consider "$installed_dir/$entry" "part of the app"
        done
    fi
elif [[ -d "$installed_dir" ]]; then
    install_refused="$installed_dir"
fi

consider "$HOME/.vst3/StemLab.vst3" "the VST3 plug-in"
consider "$CONFIG_HOME/StemLab" "settings: the torch-compile preference"

# The setup script stages a download of several gigabytes here and removes it
# when the install succeeds. A run that failed leaves it on purpose, so that
# re-running resumes - which means an uninstall is the last chance anyone has
# to notice it. It is installer debris, never anything the user made, so it
# goes in the default scope.
consider "$CACHE_HOME/StemLab/setup" "an unfinished download the setup script left"

if [[ $SCOPE_MODELS -eq 1 ]]; then
    consider "$CACHE_HOME/bs-roformer-infer" "BS-RoFormer weights"
    consider "$CACHE_HOME/torch/hub/checkpoints" "Demucs weights (torch hub)"
    consider "$CACHE_HOME/huggingface" "the HuggingFace cache"
    consider "${STEMLAB_ANALYSIS_HOME:-$CACHE_HOME/StemLab/analysis}" \
        "analysis, MIDI staging, compiled kernels"
    consider "$DATA_HOME/StemLab/models" "weights for the adaptive splits"

    # Nothing writes ~/.stemlab any more - the analysis cache and the model
    # weights moved to the two directories above, which say what they are and
    # which a disk-cleaning tool can reason about. Anyone who ran an older
    # StemLab still has the old one, holding gigabytes that nothing will ever
    # read again, so an uninstall is the one moment worth naming it.
    consider "$HOME/.stemlab" "the directory older versions kept both in"
fi

if [[ $SCOPE_EVERYTHING -eq 1 ]]; then
    consider "$MEDIA_HOME" "your captures, recordings and separated stems"
    consider "$CACHE_HOME/StemLab" "separation jobs (older installs kept them here)"
fi

# Whatever else lives in the install directory is the user's - Captures and
# Recordings by default, but the rule is "not the app's, and not already
# going" rather than a second list.
#
# Computed after every consider above rather than beside the first one,
# because the model weights live at $DATA_HOME/StemLab/models, which is inside
# the install directory. --models takes them; without this ordering they would
# be printed as removed and as kept in the same run.
if [[ $SCOPE_EVERYTHING -eq 0 ]] && looks_like_stemlab "$installed_dir"; then
    for path in "$installed_dir"/* "$installed_dir"/.[!.]*; do
        name="${path##*/}"
        mine=0

        for entry in "${BUNDLE_ENTRIES[@]}"; do
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

if [[ $SCOPE_MODELS -eq 0 ]]; then
    echo "Model weights and caches are kept. Add --models to remove those too."
fi

if [[ $SCOPE_EVERYTHING -eq 0 ]]; then
    echo "Your captures, recordings and separated stems in $MEDIA_HOME are kept."
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

echo
echo "StemLab is removed."

if [[ ${#kept_in_install[@]} -gt 0 ]]; then
    echo "Your files are still in $installed_dir."
fi

if [[ $SCOPE_MODELS -eq 0 ]]; then
    echo "Model weights are still on disk; ./uninstall.sh --models takes those."
fi

echo "Rescan plug-ins in your DAW so it forgets the VST3."
