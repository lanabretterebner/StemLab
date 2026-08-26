#!/usr/bin/env bash
#
# One-step setup for the StemLab Linux bundle. Ships on the releases page;
# it is the only file you need to download. A downloaded file has no execute
# bit, so run it through bash:
#
#   bash StemLab-Linux-setup.sh xpu      # fetch + install the xpu bundle
#   bash StemLab-Linux-setup.sh          # set up a bundle already downloaded
#                                        # into this folder
#   bash StemLab-Linux-setup.sh StemLab-0.1.0-Linux-xpu.tar.gz   # this one
#
# Given a flavor (cpu, cuda, rocm, xpu) it downloads that bundle's pieces
# from the release it shipped with. Either way it then joins split .partNN
# files, verifies the archive against its .sha256, installs the app and the
# Engine into /opt/StemLab (override with STEMLAB_INSTALL_DIR), registers
# the VST3 for the current user, and removes every downloaded file - the
# archive, its parts, the checksum, and this script itself.

set -euo pipefail
shopt -s nullglob

cd "$(dirname "${BASH_SOURCE[0]}")"

die() { echo "$*" >&2; exit 1; }

# Filled in when the release is built. In a source checkout they stay as
# placeholders and only the already-downloaded-files path works.
RELEASE_URL="@RELEASE_URL@"
VERSION="@VERSION@"

remote_available() { [[ "$RELEASE_URL" != @* && "$VERSION" != @* ]]; }

fetch() {
    # fetch <filename>: download $RELEASE_URL/<filename> into the current
    # folder, leaving nothing behind on failure.
    local tmp="$1.download"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --progress-bar -o "$tmp" "$RELEASE_URL/$1" \
            || { rm -f "$tmp"; return 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$tmp" "$RELEASE_URL/$1" \
            || { rm -f "$tmp"; return 1; }
    else
        die "Neither curl nor wget is installed - download the bundle files by hand and re-run this."
    fi
    mv "$tmp" "$1"
}

# ------------------------------------------------------------ pick a bundle

[[ $# -le 1 ]] || die "Usage: bash ${BASH_SOURCE[0]##*/} [cpu|cuda|rocm|xpu | StemLab-...-Linux-<flavor>.tar.gz]"

candidates=()
for f in StemLab-*-Linux-*.tar.gz; do
    candidates+=("$f")
done
for f in StemLab-*-Linux-*.tar.gz.part00; do
    stem="${f%.part00}"
    [[ -f "$stem" ]] || candidates+=("$stem")
done

download=0
if [[ $# -eq 1 ]]; then
    case "$1" in
        cpu|cuda|rocm|xpu)
            remote_available || die "This copy of the script is not tied to a release, so it cannot download. Pass a filename instead."
            bundle="StemLab-$VERSION-Linux-$1.tar.gz"
            # Anything already here for this bundle is used as-is; only a
            # complete absence triggers the download.
            existing=("$bundle".part[0-9][0-9])
            if [[ ! -f "$bundle" && ${#existing[@]} -eq 0 ]]; then
                download=1
            fi
            ;;
        *)
            # A part name is accepted and means its bundle.
            bundle="${1%.part[0-9][0-9]}"
            ;;
    esac
elif [[ ${#candidates[@]} -eq 1 ]]; then
    bundle="${candidates[0]}"
elif [[ ${#candidates[@]} -gt 1 ]]; then
    {
        echo "More than one bundle is here - say which one:"
        for candidate in "${candidates[@]}"; do
            echo "  bash ${BASH_SOURCE[0]##*/} $candidate"
        done
    } >&2
    exit 2
elif remote_available; then
    die "Nothing is downloaded here yet. Pick the flavor for your hardware and run:
  bash ${BASH_SOURCE[0]##*/} cuda    # NVIDIA
  bash ${BASH_SOURCE[0]##*/} rocm    # AMD (RDNA2+)
  bash ${BASH_SOURCE[0]##*/} xpu     # Intel Arc / Xe
  bash ${BASH_SOURCE[0]##*/} cpu     # no GPU offload (smallest)
It will download everything it needs."
else
    die "No StemLab-*-Linux-*.tar.gz (or its .part00) found next to this script.
Download the bundle into this folder first."
fi

# ---------------------------------------------------------------- download

if [[ $download -eq 1 ]]; then
    echo "Downloading $bundle from"
    echo "  $RELEASE_URL"
    fetch "$bundle.sha256" \
        || die "Could not fetch $bundle.sha256 - does this release have a '${1}' bundle?"
    if ! fetch "$bundle"; then
        # No whole archive up there means it shipped split; the parts are
        # numbered from 00 and the first miss is the end of them. A transfer
        # cut short mid-sequence is caught by the checksum below.
        i=0
        while fetch "$(printf '%s.part%02d' "$bundle" "$i")"; do
            i=$((i + 1))
        done
        [[ $i -gt 0 ]] || die "Neither $bundle nor $bundle.part00 exists at $RELEASE_URL."
    fi
fi

# -------------------------------------------------------- join split parts

parts=("$bundle".part[0-9][0-9])
joined_here=0

if [[ ! -f "$bundle" ]]; then
    [[ ${#parts[@]} -gt 0 ]] || die "Neither $bundle nor its .partNN files are here."
    # cat would silently paper over a gap in the numbering and hand tar a
    # broken archive, so the sequence is checked first.
    for i in "${!parts[@]}"; do
        expected="$bundle.part$(printf '%02d' "$i")"
        [[ -f "$expected" ]] || die "Missing $expected - download every part into this folder first."
    done
    echo "Joining ${#parts[@]} parts into $bundle..."
    cat "${parts[@]}" > "$bundle"
    joined_here=1
fi

# ------------------------------------------------------------------ verify

if [[ -f "$bundle.sha256" ]]; then
    echo "Verifying $bundle..."
    if ! sha256sum -c "$bundle.sha256"; then
        # A bad join is this script's own product; a bad download is not.
        [[ $joined_here -eq 1 ]] && rm -f "$bundle"
        die "Checksum mismatch - a part or the archive is damaged. Re-download and run this again."
    fi
    # The parts are proven redundant now; dropping them before extraction
    # roughly halves the peak disk this needs.
    if [[ ${#parts[@]} -gt 0 ]]; then
        rm -f "${parts[@]}"
        parts=()
    fi
else
    echo "NOTE: no $bundle.sha256 here, so the archive is not being verified." >&2
fi

# ----------------------------------------------------- extract and install

INSTALL_DIR="${STEMLAB_INSTALL_DIR:-/opt/StemLab}"
parent_dir="$(dirname "$INSTALL_DIR")"

# The tarball holds exactly one folder, named like itself.
folder="${bundle%.tar.gz}"

echo "Extracting $bundle (this is large)..."
tar -xzf "$bundle"

[[ -f "$folder/install.sh" ]] || die "The archive did not contain $folder/install.sh - it is not a StemLab Linux bundle."

# Placing the payload under /opt usually needs root, but everything
# per-user - the VST3 copy, the engine pointer - must NOT run as root, so
# only the file moves go through sudo and install.sh runs as the invoking
# user afterwards.
priv=()
if [[ ! -d "$parent_dir" || ! -w "$parent_dir" || ( -e "$INSTALL_DIR" && ! -w "$INSTALL_DIR" ) ]]; then
    command -v sudo >/dev/null 2>&1 || die "Installing to $INSTALL_DIR needs root.
Re-run as root, or point STEMLAB_INSTALL_DIR at a folder you can write."
    echo "Placing StemLab in $INSTALL_DIR (sudo may ask for your password)..."
    priv=(sudo)
fi

if [[ -e "$INSTALL_DIR" ]]; then
    # Only reached with the verified replacement already extracted.
    echo "Replacing the previous install at $INSTALL_DIR..."
    "${priv[@]}" rm -rf "$INSTALL_DIR"
fi
"${priv[@]}" mkdir -p "$parent_dir"
"${priv[@]}" mv "$folder" "$INSTALL_DIR"

echo "Installing..."
bash "$INSTALL_DIR/install.sh"

# ----------------------------------------------------------------- tidy up

# Everything the setup needed is spent now: the archive, its parts, the
# checksum, the reassembly note - and this script, which shipped with the
# release it just installed. A clean folder is the point of a one-download
# install. The source-checkout copy (unbaked placeholders) is the one copy
# that is not a download, so it stays.
rm -f "$bundle" "$bundle.sha256" "$bundle.README.txt"
if [[ ${#parts[@]} -gt 0 ]]; then
    rm -f "${parts[@]}"
fi
if remote_available; then
    rm -f "${BASH_SOURCE[0]##*/}"
fi

echo
echo "StemLab is installed in $INSTALL_DIR."
echo "  Standalone app: $INSTALL_DIR/StemLab"
echo "  VST3:           already registered for your user - rescan in your DAW"
echo "Removed the downloaded files, this script included."
