#!/usr/bin/env bash
#
# Update an installed StemLab to the latest release.
#
#   ./update.sh                 update if there is a newer release
#   ./update.sh --check         say what is installed and what is out; change nothing
#   ./update.sh --force         reinstall even when the versions match
#   ./update.sh --flavor cuda   change GPU flavor (default: keep the current one)
#
# What it does is fetch that release's own setup script and run it. That
# script already knows how to download a bundle whole or in parts, verify it
# against its .sha256, replace the install and register the VST3 - and it is
# the copy that shipped with the release being installed, so this never has to
# track how a future bundle is laid out.
#
# Nothing you own is touched. Model weights live under ~/.cache, settings under
# ~/.config, and your audio in <your music folder>/StemLab - none of them inside
# the install - so replacing the install leaves all three where they are.
#
# STEMLAB_LATEST_TAG short-circuits the release lookup. It exists so the tests
# can drive this without a network, and is documented here rather than hidden
# because a support answer sometimes needs to pin a version by hand.

set -euo pipefail

CHECK_ONLY=0
FORCE=0
WANT_FLAVOR=""

REPO="${STEMLAB_REPO:-lanabretterebner/StemLab}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)  CHECK_ONLY=1; shift ;;
        --force)  FORCE=1;      shift ;;
        --flavor)
            case "${2:-}" in
                cpu|cuda|rocm|xpu) WANT_FLAVOR="$2" ;;
                *) echo "Unknown flavor: ${2:-} (use cpu|cuda|rocm|xpu)" >&2; exit 2 ;;
            esac
            shift 2 ;;
        -h|--help)
            sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

die() { echo "$*" >&2; exit 1; }

xdg() {
    local value="${!1:-}"
    [[ "$value" == /* ]] || value="$HOME/$2"
    printf '%s' "$value"
}

DATA_HOME="$(xdg XDG_DATA_HOME .local/share)"
CONFIG_HOME="$(xdg XDG_CONFIG_HOME .config)"

# ---------------------------------------------------------- a source checkout

# Updating a checkout is git's job, not this script's: rebuilding pulls a
# relocatable CPython and a gigabyte of torch, which is not something to start
# because someone typed "update". Saying so beats doing it or doing nothing.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -f "$HERE/../../pyproject.toml" && -d "$HERE/../../.git" ]]; then
    cat <<EOF
This is the copy in the source tree, so there is no bundle for it to update.
To move a checkout forward:

  git -C "$(cd "$HERE/../.." && pwd)" pull
  ./scripts/linux/build.sh          # rebuild, then run the bundle's install.sh

An installed bundle carries its own copy of this script, next to the app.
Run that one to update the install.
EOF
    exit 0
fi

# ------------------------------------------------------- finding the install

installed_dir=""

# One place, so there is nothing to look up. STEMLAB_INSTALL_DIR is still
# honoured for an install built somewhere else with --dest.
if [[ -n "${STEMLAB_INSTALL_DIR:-}" ]]; then
    installed_dir="$STEMLAB_INSTALL_DIR"
fi

[[ -n "$installed_dir" ]] || installed_dir="$DATA_HOME/StemLab"

[[ -d "$installed_dir/Engine" ]] || die "No StemLab install found at $installed_dir.
Install it first with the setup script from the releases page."

# The version marker is written by build.sh. Bundles from before it existed
# only say so in the first line of README.txt, so that is read as a fallback -
# an update is exactly the moment someone is running an older bundle.
installed_version=""

if [[ -f "$installed_dir/.stemlab-version" ]]; then
    installed_version="$(head -1 "$installed_dir/.stemlab-version" | tr -d '[:space:]')"
elif [[ -f "$installed_dir/README.txt" ]]; then
    installed_version="$(sed -n '1s/^StemLab \([0-9][^ ]*\).*/\1/p' "$installed_dir/README.txt")"
fi

# The flavor is kept unless asked otherwise: silently turning a cuda install
# into a cpu one on update would look like the app got slow for no reason.
installed_flavor=""

if [[ -f "$installed_dir/Engine/.stemlab-torch-flavor" ]]; then
    installed_flavor="$(head -1 "$installed_dir/Engine/.stemlab-torch-flavor" | tr -d '[:space:]')"
fi

flavor="${WANT_FLAVOR:-$installed_flavor}"

# ---------------------------------------------------------- the latest release

latest_tag() {
    if [[ -n "${STEMLAB_LATEST_TAG:-}" ]]; then
        printf '%s' "$STEMLAB_LATEST_TAG"
        return 0
    fi

    # The /releases/latest page redirects to the tag, so the final URL is the
    # answer. Cheaper than the API, and it needs neither a token nor jq.
    local url
    url="$(curl -sIL -o /dev/null -w '%{url_effective}' \
        "https://github.com/$REPO/releases/latest" 2>/dev/null)" || return 1

    [[ "$url" == */releases/tag/* ]] || return 1

    printf '%s' "${url##*/}"
}

tag="$(latest_tag)" || die "Could not reach github.com to ask for the latest release.
Check the connection and run this again."

[[ -n "$tag" ]] || die "github.com did not name a latest release for $REPO."

latest_version="${tag#v}"

echo "Installed: ${installed_version:-unknown} (${installed_flavor:-unknown flavor}) in $installed_dir"
echo "Latest:    $latest_version"

# sort -V rather than a string compare, so 0.1.10 is newer than 0.1.9 and a
# downgrade is recognised instead of silently reinstalling.
newer_available=0

if [[ -z "$installed_version" ]]; then
    # An install that will not say what it is gets updated: the alternative is
    # refusing to help the one case most likely to be out of date.
    newer_available=1
elif [[ "$installed_version" != "$latest_version" ]]; then
    oldest="$(printf '%s\n%s\n' "$installed_version" "$latest_version" | sort -V | head -1)"

    [[ "$oldest" == "$installed_version" ]] && newer_available=1
fi

if [[ $newer_available -eq 0 && $FORCE -eq 0 ]]; then
    if [[ "$installed_version" == "$latest_version" ]]; then
        echo
        echo "Already up to date."
    else
        echo
        echo "The installed version is newer than the latest release."
        echo "Use --force to install $latest_version over it anyway."
    fi

    exit 0
fi

if [[ $CHECK_ONLY -eq 1 ]]; then
    echo
    echo "An update is available. Run this without --check to install it."
    exit 0
fi

[[ -n "$flavor" ]] || die "This install does not record its GPU flavor, so the
update cannot pick one for you. Re-run with --flavor cpu|cuda|rocm|xpu."

# ------------------------------------------------------------------- updating

command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 \
    || die "Neither curl nor wget is installed."

# Only the setup script itself lands here - a few kilobytes. The bundle it
# then downloads is staged under the cache by that script, not beside it, so
# this directory stays small no matter how large the release is.
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

setup_url="https://github.com/$REPO/releases/download/$tag/StemLab-Linux-setup.sh"

echo
echo "Fetching the $tag setup script..."

if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$setup_url" -o "$workdir/StemLab-Linux-setup.sh" \
        || die "Could not download $setup_url"
else
    wget -q -O "$workdir/StemLab-Linux-setup.sh" "$setup_url" \
        || die "Could not download $setup_url"
fi

# A release whose setup script is missing would otherwise be discovered as a
# confusing bash syntax error partway through an HTML error page.
head -1 "$workdir/StemLab-Linux-setup.sh" | grep -q '^#!' \
    || die "$setup_url did not return a script - is $tag a StemLab release?"

echo "Installing StemLab $latest_version ($flavor). This downloads the bundle."
echo

# Exported rather than passed: the setup script reads the install location the
# same way this does, and an update must land where the install already is.
#
# That script replaces $installed_dir wholesale - including this file, which is
# running from inside it. That is safe rather than lucky: bash holds the script
# open while it runs it, and unlinking a file on Linux leaves the open
# descriptor reading the same inode, so the rest of this script executes from
# the copy that was deleted. Nothing below may assume the file is still there.
STEMLAB_INSTALL_DIR="$installed_dir" bash "$workdir/StemLab-Linux-setup.sh" "$flavor"

echo
echo "Updated to $latest_version. Rescan plug-ins in your DAW."
