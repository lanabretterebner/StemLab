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
#
# NOTHING IS WRITTEN NEXT TO THIS SCRIPT. The download, the joined archive and
# the extracted tree - a bundle runs to gigabytes, and briefly to twice that
# while the parts are being joined - are staged in
#
#   ${XDG_CACHE_HOME:-~/.cache}/StemLab/setup
#
# and that directory goes at the end. Override it with STEMLAB_SETUP_STAGE if
# your cache is on a small partition.
#
# Not $TMPDIR or /tmp: /tmp is a RAM-backed tmpfs on a good number of
# distributions, sized against memory rather than disk, and the FHS lets it be
# cleared between reboots. Either one turns a resumable multi-gigabyte
# download into one that has to start over. The cache is also on the same
# filesystem as the default install directory, so the final step is a rename
# rather than a second full-size copy.
#
# A run that fails leaves the staging directory behind on purpose: that is
# what makes "run this again" resume rather than restart. uninstall.sh removes
# it if it is ever orphaned.

set -euo pipefail
shopt -s nullglob

die() { echo "$*" >&2; exit 1; }

# Absolute, and no cd: this folder is only ever read from now. It is where a
# hand-downloaded bundle is looked for, and where this script deletes itself.
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/${BASH_SOURCE[0]##*/}"
SOURCE_DIR="$(dirname "$SELF")"

# Relative XDG values are invalid per the spec and are ignored by the plugin,
# so they are ignored here the same way or the install lands where nothing
# looks for it.
DATA_HOME="${XDG_DATA_HOME:-}"
[[ "$DATA_HOME" == /* ]] || DATA_HOME="$HOME/.local/share"

CACHE_HOME="${XDG_CACHE_HOME:-}"
[[ "$CACHE_HOME" == /* ]] || CACHE_HOME="$HOME/.cache"

STAGE="${STEMLAB_SETUP_STAGE:-$CACHE_HOME/StemLab/setup}"
[[ "$STAGE" == /* ]] || die "STEMLAB_SETUP_STAGE must be an absolute path (got: $STAGE)"

INSTALL_DIR="${STEMLAB_INSTALL_DIR:-$DATA_HOME/StemLab}"

mkdir -p "$STAGE" || die "Cannot create the staging directory $STAGE.
Set STEMLAB_SETUP_STAGE to somewhere writable with room for the bundle."

# Filled in when the release is built. In a source checkout they stay as
# placeholders and only the already-downloaded-files path works.
RELEASE_URL="@RELEASE_URL@"
VERSION="@VERSION@"

remote_available() { [[ "$RELEASE_URL" != @* && "$VERSION" != @* ]]; }

RETRY_DELAY="${STEMLAB_SETUP_RETRY_DELAY:-5}"

# A partial download is this run's own debris once it is interrupted, and
# leaving it would make the next run refuse to start (see the stray check).
clear_partials() { rm -f "$STAGE"/*.download 2>/dev/null || true; }
trap 'clear_partials; exit 130' INT
trap 'clear_partials; exit 143' TERM

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
    # fetch <filename>: download $RELEASE_URL/<filename> into the staging
    # directory, leaving nothing behind on failure. Only called for files
    # remote_exists confirmed, so any failure here is a real one.
    local tmp="$STAGE/$1.download"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --progress-bar -o "$tmp" "$RELEASE_URL/$1" \
            || { rm -f "$tmp"; return 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$tmp" "$RELEASE_URL/$1" \
            || { rm -f "$tmp"; return 1; }
    else
        die "Neither curl nor wget is installed - download the bundle files by hand and re-run this."
    fi
    mv "$tmp" "$STAGE/$1"
}

# Where a named file actually is: what this run has staged first, then beside
# the script, which is where a hand-downloaded one lands.
locate() {
    local name="$1"

    if [[ -f "$STAGE/$name" ]]; then
        printf '%s' "$STAGE/$name"
        return 0
    fi

    if [[ -f "$SOURCE_DIR/$name" ]]; then
        printf '%s' "$SOURCE_DIR/$name"
        return 0
    fi

    return 1
}

# A .download file is another invocation mid-transfer - or the debris of one
# that died in a way the trap above could not catch. Joining whatever parts
# have landed so far while a sibling run is still fetching produces an archive
# that fails its checksum for no visible reason, so refuse to run over either.
stray=("$STAGE"/*.download)
if [[ ${#stray[@]} -gt 0 ]]; then
    die "Found an unfinished download in $STAGE:
  ${stray[*]}
Another run of this script may still be fetching it - wait for that to
finish. If a run was killed, delete those files and run this again."
fi

# ------------------------------------------------------------ pick a bundle

[[ $# -le 1 ]] || die "Usage: bash ${SELF##*/} [cpu|cuda|rocm|xpu | StemLab-...-Linux-<flavor>.tar.gz]"

# Bundle names, not paths: the same bundle can be half-staged and half beside
# the script, and locate() decides which copy of a given file is used.
candidates=()

remember() {
    local name="$1" existing

    for existing in ${candidates[@]+"${candidates[@]}"}; do
        [[ "$existing" == "$name" ]] && return 0
    done

    candidates+=("$name")
}

for dir in "$STAGE" "$SOURCE_DIR"; do
    for f in "$dir"/StemLab-*-Linux-*.tar.gz; do
        remember "${f##*/}"
    done
    for f in "$dir"/StemLab-*-Linux-*.tar.gz.part00; do
        name="${f##*/}"
        remember "${name%.part00}"
    done
done

download=0
if [[ $# -eq 1 ]]; then
    case "$1" in
        cpu|cuda|rocm|xpu)
            remote_available || die "This copy of the script is not tied to a release, so it cannot download. Pass a filename instead."
            bundle="StemLab-$VERSION-Linux-$1.tar.gz"
            # Anything already here for this bundle is used as-is; only a
            # complete absence triggers the download.
            existing=("$STAGE/$bundle".part[0-9][0-9] "$SOURCE_DIR/$bundle".part[0-9][0-9])
            if ! locate "$bundle" >/dev/null && [[ ${#existing[@]} -eq 0 ]]; then
                download=1
            fi
            ;;
        *)
            # A part name is accepted and means its bundle.
            name="${1##*/}"
            bundle="${name%.part[0-9][0-9]}"
            ;;
    esac
elif [[ ${#candidates[@]} -eq 1 ]]; then
    bundle="${candidates[0]}"
elif [[ ${#candidates[@]} -gt 1 ]]; then
    {
        echo "More than one bundle is here - say which one:"
        for candidate in "${candidates[@]}"; do
            echo "  bash ${SELF##*/} $candidate"
        done
    } >&2
    exit 2
elif remote_available; then
    die "Nothing is downloaded yet. Pick the flavor for your hardware and run:
  bash ${SELF##*/} cuda    # NVIDIA
  bash ${SELF##*/} rocm    # AMD (RDNA2+)
  bash ${SELF##*/} xpu     # Intel Arc / Xe
  bash ${SELF##*/} cpu     # no GPU offload (smallest)
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
    echo "into $STAGE"

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

# Parts come from one folder: the staged ones this run fetched, or the ones
# somebody downloaded by hand. Mixing halves of two different downloads is
# exactly the state the checksum exists to catch, so it is not attempted.
parts_dir=""
for dir in "$STAGE" "$SOURCE_DIR"; do
    if [[ -f "$dir/$bundle.part00" ]]; then
        parts_dir="$dir"
        break
    fi
done

parts=()
[[ -n "$parts_dir" ]] && parts=("$parts_dir/$bundle".part[0-9][0-9])

joined_here=0

if ! bundle_path="$(locate "$bundle")"; then
    [[ ${#parts[@]} -gt 0 ]] || die "Neither $bundle nor its .partNN files are here."
    # cat would silently paper over a gap in the numbering and hand tar a
    # broken archive, so the sequence is checked first.
    for i in "${!parts[@]}"; do
        expected="$parts_dir/$bundle.part$(printf '%02d' "$i")"
        [[ -f "$expected" ]] || die "Missing ${expected##*/} in $parts_dir - download every part into one folder first."
    done
    echo "Joining ${#parts[@]} parts into $STAGE/$bundle..."
    cat "${parts[@]}" > "$STAGE/$bundle"
    bundle_path="$STAGE/$bundle"
    joined_here=1
fi

# ------------------------------------------------------------------ verify

sha_path="$(locate "$bundle.sha256" || true)"

verify_archive() {
    # 0 = verified, 1 = the archive is not what the release published,
    # 2 = there is nothing to check it against.
    local expected actual

    [[ -n "$sha_path" ]] || return 2
    command -v sha256sum >/dev/null 2>&1 || return 2

    # Only the digest is read. The name in a .sha256 line is relative to the
    # folder it was generated in, and by now the archive can be in the
    # staging directory while the checksum sits beside the script - which is
    # what made "sha256sum -c" the wrong tool here.
    expected="$(awk 'NR == 1 { print tolower($1) }' "$sha_path")"
    [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || return 2

    actual="$(sha256sum "$bundle_path" | awk '{ print tolower($1) }')"

    [[ "$expected" == "$actual" ]]
}

echo "Verifying $bundle..."
verify_archive && verdict=0 || verdict=$?

if [[ $verdict -eq 2 ]]; then
    if [[ -z "$sha_path" ]]; then
        echo "NOTE: no $bundle.sha256 here, so the archive is not being verified." >&2
    else
        echo "NOTE: $bundle.sha256 could not be read (is sha256sum installed?), so" >&2
        echo "      the archive is not being verified." >&2
    fi
elif [[ $verdict -ne 0 ]]; then
    # A bad join is this script's own product; a bad download is not.
    [[ $joined_here -eq 1 ]] && rm -f "$STAGE/$bundle"

    if [[ $downloaded_here -eq 1 ]]; then
        # These files came from this very run and are proven bad as a set.
        # Keeping them would make the next run reuse them as-is and fail the
        # same way forever, so clear them for a fresh fetch instead.
        rm -f "$STAGE/$bundle" "$STAGE/$bundle.sha256" "$STAGE/$bundle".part[0-9][0-9]
        die "Checksum mismatch - the downloaded files were damaged, so they were removed.
Run this again to fetch them fresh."
    fi

    if [[ -n "$parts_dir" ]]; then
        die "Checksum mismatch - a part or the archive is damaged.
Delete $bundle.sha256 and every $bundle.part* in $parts_dir, download them
again, then re-run this."
    fi

    die "Checksum mismatch - $bundle_path is not what the release published.
Delete it and its .sha256, download them again, then re-run this."
else
    echo "  $bundle: OK"
fi

# The parts are proven redundant now; dropping them before extraction roughly
# halves the peak disk this needs, which at these sizes is the difference
# between finishing and running out.
if [[ $verdict -eq 0 && ${#parts[@]} -gt 0 ]]; then
    rm -f "${parts[@]}"
    parts=()
fi

# ----------------------------------------------------- extract and install

parent_dir="$(dirname "$INSTALL_DIR")"

# The tarball holds exactly one folder, named like itself.
folder="${bundle%.tar.gz}"

echo "Extracting $bundle into $STAGE (this is large)..."
rm -rf "${STAGE:?}/$folder"
tar -xzf "$bundle_path" -C "$STAGE"

[[ -f "$STAGE/$folder/install.sh" ]] || die "The archive did not contain $folder/install.sh - it is not a StemLab Linux bundle."

# The default location needs no privileges at all. This stays for anyone who
# points STEMLAB_INSTALL_DIR at a system directory: only the file moves go
# through sudo, and install.sh runs as the invoking user afterwards, because
# everything it does is per-user - the VST3 copy above all - and must not end
# up owned by root.
# Which directory's permissions actually decide this: mkdir -p creates every
# missing level, so a parent that does not exist yet is not a reason to reach
# for sudo - the nearest ancestor that does exist is. Asking -w about a
# directory that is not there always answers "no", which is how a first
# install into a home that had no ~/.local/share ended up prompting for a
# password and then owned by root.
probe="$parent_dir"
while [[ ! -e "$probe" && "$probe" != "/" && "$probe" != "." ]]; do
    probe="$(dirname "$probe")"
done

priv=()
if [[ ! -w "$probe" || ( -e "$INSTALL_DIR" && ! -w "$INSTALL_DIR" ) ]]; then
    command -v sudo >/dev/null 2>&1 || die "Installing to $INSTALL_DIR needs root.
Re-run as root, or unset STEMLAB_INSTALL_DIR to install under
$DATA_HOME/StemLab, which needs no privileges."
    echo "Placing StemLab in $INSTALL_DIR (sudo may ask for your password)..."
    priv=(sudo)
fi

"${priv[@]}" mkdir -p "$parent_dir"

if [[ -e "$INSTALL_DIR" ]]; then
    # Only reached with the verified replacement already extracted.
    echo "Replacing the previous install at $INSTALL_DIR..."

    # Not an rm -rf of the whole folder. On Linux the install directory and
    # the app's data directory are the same place: the engine writes the
    # adaptive-split weights to $INSTALL_DIR/models/recursive (see
    # src/stemlab/paths.py), the plugin writes $INSTALL_DIR/Ableton, and
    # installs older than the move to the music folder still have Captures
    # and Recordings in there. Deleting the folder wholesale re-downloaded
    # half a gigabyte of weights after every update, silently.
    #
    # What the new bundle contains is what gets replaced; anything else is
    # carried across. That list is read off the extracted bundle rather than
    # written down here, so a file added to the bundle tomorrow is replaced
    # tomorrow with nothing here to keep in step.
    #
    # The old install is renamed rather than copied out of, so this costs one
    # rename however many gigabytes of weights are in it.
    old_install="$INSTALL_DIR.replaced"

    "${priv[@]}" rm -rf "$old_install"
    "${priv[@]}" mv "$INSTALL_DIR" "$old_install"
    "${priv[@]}" mv "$STAGE/$folder" "$INSTALL_DIR"

    # dotglob so .stemlab-version and anything else hidden is considered;
    # nullglob so an empty old install is not a literal "*".
    shopt -s nullglob dotglob

    for kept in "$old_install"/*; do
        name="${kept##*/}"

        # Present in the new bundle: the new copy wins, the old one goes.
        [[ -e "$INSTALL_DIR/$name" ]] && continue

        echo "  keeping $name"
        "${priv[@]}" mv "$kept" "$INSTALL_DIR/$name"
    done

    shopt -u nullglob dotglob

    "${priv[@]}" rm -rf "$old_install"
else
    "${priv[@]}" mv "$STAGE/$folder" "$INSTALL_DIR"
fi

# install.sh is now running from inside $INSTALL_DIR, which is where it is
# meant to end up, so it copies nothing and only registers the VST3.
echo "Installing..."
bash "$INSTALL_DIR/install.sh"

# ----------------------------------------------------------------- tidy up

# Everything the setup needed is spent now. The staging directory holds
# whatever this run downloaded and the remains of the extraction; a bundle
# somebody put beside this script by hand is removed too, because a
# one-download install that leaves gigabytes behind is not one. The
# source-checkout copy of this script (unbaked placeholders) is the one copy
# that is not a download, so it stays.
rm -rf "${STAGE:?}"

for leftover in "$bundle" "$bundle.sha256" "$bundle.README.txt"; do
    rm -f "$SOURCE_DIR/$leftover"
done
rm -f "$SOURCE_DIR/$bundle".part[0-9][0-9]

if remote_available; then
    rm -f "$SELF"
fi

echo
echo "StemLab is installed in $INSTALL_DIR."
echo "  Standalone app: $INSTALL_DIR/StemLab"
echo "  VST3:           already registered for your user - rescan in your DAW"
echo "  Update:         $INSTALL_DIR/update.sh"
echo "  Uninstall:      $INSTALL_DIR/uninstall.sh"
echo "Removed the downloaded files, this script included."
