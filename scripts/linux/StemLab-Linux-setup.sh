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
# Engine into ~/.local/share/StemLab (override with STEMLAB_INSTALL_DIR),
# registers the VST3 for the current user, and removes every downloaded file
# - the archive, its parts, the checksum, and this script itself.

set -euo pipefail
shopt -s nullglob

cd "$(dirname "${BASH_SOURCE[0]}")"

die() { echo "$*" >&2; exit 1; }

# Filled in when the release is built. In a source checkout they stay as
# placeholders and only the already-downloaded-files path works.
RELEASE_URL="@RELEASE_URL@"
VERSION="@VERSION@"

remote_available() { [[ "$RELEASE_URL" != @* && "$VERSION" != @* ]]; }

RETRY_DELAY="${STEMLAB_SETUP_RETRY_DELAY:-5}"

remote_exists() {
    # remote_exists <filename>: 0 = there, 44 = definitively not there
    # (HTTP 404), 1 = cannot tell (network trouble). Probing is how this
    # script discovers whether a bundle shipped whole or split and where
    # the parts end, so an expected miss must be quiet - a curl error
    # splashed for a routine probe reads like the download failed.
    local code
    if command -v curl >/dev/null 2>&1; then
        code="$(curl -sIL -o /dev/null -w '%{http_code}' "$RELEASE_URL/$1" 2>/dev/null)" \
            || return 1
        case "$code" in
            200) return 0 ;;
            404) return 44 ;;
            *)   return 1 ;;
        esac
    elif command -v wget >/dev/null 2>&1; then
        # wget cannot cleanly separate a 404 from other server errors; a
        # spider miss is treated as "not there" and the checksum still
        # guards the result.
        if wget -q --spider "$RELEASE_URL/$1" 2>/dev/null; then
            return 0
        fi
        return 44
    else
        die "Neither curl nor wget is installed - download the bundle files by hand and re-run this."
    fi
}

fetch() {
    # fetch <filename>: download $RELEASE_URL/<filename> into the current
    # folder, leaving nothing behind on failure. Only called for files
    # remote_exists confirmed, so any failure here is a real one.
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

# A .download file is another invocation mid-transfer - or the debris of one
# that died. Joining whatever parts have landed so far while a sibling run is
# still fetching produces an archive that fails its checksum for no visible
# reason, so refuse to run over either.
stray=(*.download)
if [[ ${#stray[@]} -gt 0 ]]; then
    die "Found ${stray[*]} here.
Another run of this script may still be downloading in this folder - wait
for it to finish. If a run was interrupted, delete the .download files and
run this again."
fi

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

downloaded_here=0
if [[ $download -eq 1 ]]; then
    echo "Downloading $bundle from"
    echo "  $RELEASE_URL"

    # The checksum must exist for this to be a real flavor of this release -
    # but assets of a release published moments ago can 404 for a minute
    # while they propagate, so a miss earns a few retries before it means
    # "no such bundle".
    tries=0
    until remote_exists "$bundle.sha256"; do
        status=$?
        [[ $status -eq 44 ]] || die "Cannot reach $RELEASE_URL right now - check the connection and run this again."
        tries=$((tries + 1))
        [[ $tries -lt 4 ]] || die "This release has no $bundle.sha256 - does it carry a '$1' bundle?
(A release published moments ago can also take a minute to become downloadable.)"
        echo "$bundle.sha256 is not there (yet) - retrying in ${RETRY_DELAY}s..."
        sleep "$RETRY_DELAY"
    done
    fetch "$bundle.sha256" || die "Downloading $bundle.sha256 failed - run this again to retry."

    remote_exists "$bundle" && whole=0 || whole=$?
    if [[ $whole -eq 0 ]]; then
        echo "Fetching $bundle..."
        fetch "$bundle" || die "Downloading $bundle failed - run this again to retry."
    elif [[ $whole -eq 44 ]]; then
        echo "$bundle shipped split into parts; fetching them."
        # The parts are numbered from 00, and a 404 one past the end is how
        # the sequence ends. Anything short of a definite 404 is network
        # trouble and must NOT end the sequence: silently stopping early
        # would join a truncated archive that fails its checksum for no
        # visible reason.
        i=0
        while :; do
            name="$(printf '%s.part%02d' "$bundle" "$i")"
            remote_exists "$name" && st=0 || st=$?
            if [[ $st -eq 44 ]]; then
                break
            elif [[ $st -ne 0 ]]; then
                die "Lost the connection while checking for $name - run this again to resume."
            fi
            echo "Fetching $name..."
            fetch "$name" || die "Downloading $name failed - run this again to resume."
            i=$((i + 1))
        done
        [[ $i -gt 0 ]] || die "Neither $bundle nor $bundle.part00 exists at $RELEASE_URL."
        echo "All $i parts fetched."
    else
        die "Cannot reach $RELEASE_URL right now - check the connection and run this again."
    fi
    downloaded_here=1
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
        if [[ $downloaded_here -eq 1 ]]; then
            # These files came from this very run and are proven bad as a
            # set. Keeping them would make the next run reuse them as-is
            # and fail the same way forever, so clear them for a fresh
            # fetch instead.
            rm -f "$bundle" "$bundle.sha256" ${parts[@]+"${parts[@]}"}
            die "Checksum mismatch - the downloaded files were damaged, so they were removed.
Run this again to fetch them fresh."
        fi
        die "Checksum mismatch - a part or the archive is damaged.
Delete $bundle.sha256 and every $bundle.part* here, download them again, then re-run this."
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

# Default to the XDG data directory rather than /opt. StemLab is a per-user
# audio plugin: its VST3 goes to ~/.vst3 and its engine pointer to
# ~/.config, so a system-wide payload was the odd one out and bought only a
# sudo prompt. This also converges with install_backend.sh, which already
# builds its Engine at $XDG_DATA_HOME/StemLab/Engine - the same path this
# now unpacks to, instead of a second one beside it.
#
# Relative XDG values are invalid per the spec and are ignored by the
# plugin, so ignore them the same way or the install lands where nothing
# looks for it.
DATA_HOME="${XDG_DATA_HOME:-}"
[[ "$DATA_HOME" == /* ]] || DATA_HOME="$HOME/.local/share"

INSTALL_DIR="${STEMLAB_INSTALL_DIR:-$DATA_HOME/StemLab}"
parent_dir="$(dirname "$INSTALL_DIR")"

# The tarball holds exactly one folder, named like itself.
folder="${bundle%.tar.gz}"

echo "Extracting $bundle (this is large)..."
tar -xzf "$bundle"

[[ -f "$folder/install.sh" ]] || die "The archive did not contain $folder/install.sh - it is not a StemLab Linux bundle."

# The default location needs no privileges at all. This stays for anyone
# who points STEMLAB_INSTALL_DIR at a system directory: only the file moves
# go through sudo, and install.sh runs as the invoking user afterwards,
# because everything it does is per-user - the VST3 copy, the engine
# pointer - and must not be owned by root.
priv=()
if [[ ! -d "$parent_dir" || ! -w "$parent_dir" || ( -e "$INSTALL_DIR" && ! -w "$INSTALL_DIR" ) ]]; then
    command -v sudo >/dev/null 2>&1 || die "Installing to $INSTALL_DIR needs root.
Re-run as root, or unset STEMLAB_INSTALL_DIR to install under
$DATA_HOME/StemLab, which needs no privileges."
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
