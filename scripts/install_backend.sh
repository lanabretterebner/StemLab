#!/usr/bin/env bash
#
# Install the StemLab separation backend on Linux - no venv, no system Python.
#
# Downloads a relocatable CPython (python-build-standalone), installs StemLab
# and its ML dependencies into it, and records the result where the plugin
# and Standalone app auto-discover it. After this script finishes, StemLab
# works with zero further configuration.
#
#   Engine   ~/.local/share/StemLab/Engine        ($XDG_DATA_HOME override)
#   Pointer  ~/.config/StemLab/portable_engine_path.txt
#
# Usage:
#   ./scripts/install_backend.sh              # auto-detect GPU -> cuda/rocm/cpu
#   ./scripts/install_backend.sh --cpu        # force CPU-only torch (smallest)
#   ./scripts/install_backend.sh --cuda       # force NVIDIA CUDA torch
#   ./scripts/install_backend.sh --rocm       # force AMD ROCm torch
#   ./scripts/install_backend.sh --xpu        # force Intel GPU (XPU) torch
#   ./scripts/install_backend.sh --dest DIR   # custom Engine location
#   ./scripts/install_backend.sh --reinstall  # rebuild the Engine from scratch
#   ./scripts/install_backend.sh --no-pointer # build-only: skip the discovery
#                                             pointer (used by the portable
#                                             bundle builder)

set -euo pipefail

# Pinned relocatable CPython. 3.11 matches the interpreter the Windows
# portable release ships.
PBS_RELEASE="20250818"
PBS_PYTHON="3.11.13"

TORCH_FLAVOR="auto"
REINSTALL=0
WRITE_POINTER=1
DEST=""

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpu)       TORCH_FLAVOR="cpu";  shift ;;
        --cuda)      TORCH_FLAVOR="cuda"; shift ;;
        --rocm)      TORCH_FLAVOR="rocm"; shift ;;
        --xpu)       TORCH_FLAVOR="xpu";  shift ;;
        --dest)      DEST="$2";           shift 2 ;;
        --reinstall) REINSTALL=1;         shift ;;
        --no-pointer) WRITE_POINTER=0;    shift ;;
        -h|--help)
            sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

# Relative XDG values are invalid per the spec and are ignored by the plugin,
# so the installer must ignore them the same way or the pointer file lands
# where the plugin never looks.
DATA_HOME="${XDG_DATA_HOME:-}"
[[ "$DATA_HOME" == /* ]] || DATA_HOME="$HOME/.local/share"

CONFIG_HOME="${XDG_CONFIG_HOME:-}"
[[ "$CONFIG_HOME" == /* ]] || CONFIG_HOME="$HOME/.config"

[[ -n "$DEST" ]] || DEST="$DATA_HOME/StemLab/Engine"

# The pointer file must hold an absolute path - the plugin resolves it with
# no working directory of its own.
[[ "$DEST" == /* ]] || DEST="$PWD/$DEST"

PYTHON="$DEST/bin/python3"

# Written the moment this script creates the directory, before anything else
# lands in it. rm -rf below only ever runs on a directory carrying it, so
# "--reinstall --dest /some/precious/dir" can never delete data this script
# does not own.
OWNER_MARKER="$DEST/.stemlab-engine"

# Written only after the interpreter extracted and validated, so a run that
# died mid-extraction is detected and redone instead of trusted.
READY_MARKER="$DEST/.stemlab-engine-ready"

FLAVOR_FILE="$DEST/.stemlab-torch-flavor"

# ------------------------------------------------------------------ preflight

for tool in curl tar sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing tool: $tool. Install it with your package manager." >&2
        exit 1
    }
done

case "$(uname -m)" in
    x86_64)  PBS_ARCH="x86_64-unknown-linux-gnu" ;;
    aarch64) PBS_ARCH="aarch64-unknown-linux-gnu" ;;
    *)
        echo "Unsupported architecture: $(uname -m)" >&2
        exit 1 ;;
esac

if [[ "$TORCH_FLAVOR" == "auto" ]]; then
    if command -v nvidia-smi >/dev/null 2>&1 \
       || [[ -e /proc/driver/nvidia/version ]]; then
        TORCH_FLAVOR="cuda"
    else
        # Never auto-pick ROCm: /sys/module/amdgpu also exists for APUs,
        # iGPUs and RDNA1 cards that the rocm wheels have no kernels for,
        # and HIP still reports those as "available" - the failure would
        # surface as a crash mid-separation, not as a clean CPU fallback.
        TORCH_FLAVOR="cpu"

        if [[ -d /sys/module/amdgpu ]]; then
            echo "An AMD GPU was detected. For a discrete RDNA2-or-newer card,"
            echo "re-run with --rocm to enable GPU separation."
        elif command -v lspci >/dev/null 2>&1 \
             && lspci 2>/dev/null | grep -qiE "VGA.*Intel|Display.*Intel"; then
            echo "An Intel GPU was detected. torch's XPU backend may work for it:"
            echo "re-run with --xpu to try (needs the Intel compute runtime)."
        fi
    fi
    echo "Detected torch flavor: $TORCH_FLAVOR (override with --cpu / --cuda / --rocm / --xpu)"
fi

# A directory this script did not create is never touched, reused, or
# deleted - whatever its contents look like.
if [[ -d "$DEST" && ! -f "$OWNER_MARKER" ]] \
   && [[ -n "$(ls -A "$DEST" 2>/dev/null)" ]]; then
    echo "Refusing to use $DEST: it already exists and was not created by" >&2
    echo "this installer. Choose another --dest or remove it yourself." >&2
    exit 1
fi

# ------------------------------------------------------------ engine decision

engine_ready=0

if [[ $REINSTALL -eq 0 && -x "$PYTHON" && -f "$READY_MARKER" ]]; then
    engine_ready=1
    echo "Reusing existing Engine interpreter: $PYTHON"
elif [[ $REINSTALL -eq 0 && -d "$DEST" && -f "$OWNER_MARKER" \
        && ! -f "$READY_MARKER" ]]; then
    echo "A previous install of $DEST did not finish - rebuilding it."
fi

if [[ $engine_ready -eq 0 ]]; then
    PBS_FILE="cpython-${PBS_PYTHON}+${PBS_RELEASE}-${PBS_ARCH}-install_only.tar.gz"
    PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_RELEASE}/${PBS_FILE}"

    TMP_TAR="$(mktemp --suffix=.tar.gz)"
    trap 'rm -f "$TMP_TAR"' EXIT

    # Download BEFORE removing anything, so a failed download leaves an
    # existing working Engine untouched.
    echo "Downloading relocatable CPython ${PBS_PYTHON} (${PBS_ARCH})..."

    for attempt in 1 2 3 4; do
        if curl -fSL --retry 0 -o "$TMP_TAR" "$PBS_URL"; then
            break
        fi

        [[ $attempt -eq 4 ]] && {
            echo "Could not download $PBS_URL" >&2
            echo "The existing Engine (if any) was left untouched." >&2
            exit 1
        }

        delay=$((2 * attempt))
        echo "Download failed. Retrying in ${delay}s..." >&2
        sleep "$delay"
    done

    # Verify against the release's published checksums before anything is
    # extracted - this archive becomes the interpreter that runs every job.
    # A truncated or tampered download is caught here rather than as a
    # baffling failure later. If the checksum list itself cannot be fetched
    # (an offline mirror, a proxy that blocks it) the install continues with
    # a warning rather than becoming unusable.
    PBS_SUMS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_RELEASE}/SHA256SUMS"
    TMP_SUMS="$(mktemp)"
    trap 'rm -f "$TMP_TAR" "$TMP_SUMS"' EXIT

    if curl -fsSL --retry 2 -o "$TMP_SUMS" "$PBS_SUMS_URL"; then
        EXPECTED_SHA="$(awk -v want="$PBS_FILE" '$2 == want { print $1 }' "$TMP_SUMS" | head -1)"

        if [[ -z "$EXPECTED_SHA" ]]; then
            echo "WARNING: $PBS_FILE is not listed in SHA256SUMS; skipping verification." >&2
        else
            ACTUAL_SHA="$(sha256sum "$TMP_TAR" | awk '{ print $1 }')"

            if [[ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]]; then
                echo "Checksum mismatch for $PBS_FILE." >&2
                echo "  expected: $EXPECTED_SHA" >&2
                echo "  actual:   $ACTUAL_SHA" >&2
                echo "Refusing to install. The existing Engine (if any) was left untouched." >&2
                exit 1
            fi

            echo "Checksum verified (sha256)."
        fi
    else
        echo "WARNING: could not fetch SHA256SUMS; installing without verification." >&2
    fi

    if [[ -d "$DEST" ]]; then
        if [[ -f "$OWNER_MARKER" ]]; then
            echo "Removing previous Engine at $DEST..."
            rm -rf "$DEST"
        elif [[ -n "$(ls -A "$DEST" 2>/dev/null)" ]]; then
            # Unreachable after the preflight check, but never worth risking.
            echo "Refusing to remove $DEST: not created by this installer." >&2
            exit 1
        fi
    fi

    echo "Extracting to $DEST..."
    mkdir -p "$DEST"
    touch "$OWNER_MARKER"

    # The archive contains a single python/ directory; strip it so the
    # layout is Engine/bin/python3, which the plugin recognises as a
    # portable runtime.
    if ! tar -xzf "$TMP_TAR" -C "$DEST" --strip-components=1; then
        echo "Extraction failed - removing the partial Engine." >&2
        rm -rf "$DEST"
        exit 1
    fi

    [[ -x "$PYTHON" ]] || {
        echo "Extraction did not produce $PYTHON" >&2
        rm -rf "$DEST"
        exit 1
    }

    touch "$READY_MARKER"
fi

# ------------------------------------------------------------------- backend

echo "Installing the StemLab backend (torch: $TORCH_FLAVOR)..."
echo "The ML dependencies are large; the first install takes a while."

# Switching torch flavor on an existing Engine needs a forced reinstall -
# "torch>=2.4" is already satisfied, so a plain install would silently keep
# the old build.
TORCH_ARGS=()
previous_flavor=""
[[ -f "$FLAVOR_FILE" ]] && previous_flavor="$(cat "$FLAVOR_FILE")"

if [[ -n "$previous_flavor" && "$previous_flavor" != "$TORCH_FLAVOR" ]]; then
    echo "Switching torch flavor: $previous_flavor -> $TORCH_FLAVOR"
    TORCH_ARGS+=(--force-reinstall)
fi

# --index-url (not --extra-index-url): torch must come from the matching
# variant index, never from a newer CUDA release that happens to be on PyPI.
# CUDA is PyPI's default build, so it needs no index override.
case "$TORCH_FLAVOR" in
    cpu)  TORCH_ARGS+=(--index-url "https://download.pytorch.org/whl/cpu") ;;
    rocm) TORCH_ARGS+=(--index-url "https://download.pytorch.org/whl/rocm6.4") ;;
    xpu)  TORCH_ARGS+=(--index-url "https://download.pytorch.org/whl/xpu") ;;
esac

# PYTHONNOUSERSITE keeps the user's ~/.local packages out of dependency
# resolution - the Engine must be self-contained.
PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install --upgrade pip --quiet

# Install torch AND torchaudio pinned to the same flavor AND the same
# version. Demucs depends on torchaudio, whose metadata declares no
# dependency on torch at all - not even a lower bound - so pip is free to
# pair any torch with any torchaudio, and left alone it does: the cpu and
# xpu indexes carry torch 2.13 while torchaudio stops at 2.11. torchaudio
# ships a compiled extension linked against one torch, so that pairing loads
# a library against the wrong runtime. On Linux it happened to import
# anyway; on Windows the loader refused it outright.
#
# torchaudio is the one that lags, so it picks the version and torch follows.
# Asking for a version rather than hardcoding one keeps this correct as both
# move.
PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install \
    ${TORCH_ARGS[@]+"${TORCH_ARGS[@]}"} "torchaudio"

TORCH_VERSION="$(PYTHONNOUSERSITE=1 "$PYTHON" -s -c \
    'from importlib.metadata import version; print(version("torchaudio").split("+")[0])')"

[[ -n "$TORCH_VERSION" ]] || {
    echo "Could not determine which torchaudio version was installed." >&2
    exit 1
}

echo "Pinning torch to torchaudio $TORCH_VERSION..."
PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install \
    ${TORCH_ARGS[@]+"${TORCH_ARGS[@]}"} \
    "torch==$TORCH_VERSION" "torchaudio==$TORCH_VERSION"

# Recursive/adaptive stem splitting needs audio-separator. The project's own
# "recursive" extra pins the CUDA onnxruntime unconditionally, so install the
# flavor-matched build here instead: CUDA gets the GPU runtime; every other
# flavor gets the CPU runtime (onnxruntime has no PyPI build for ROCm/XPU, so
# the recursive stage runs on CPU there - the main separation still offloads).
if [[ "$TORCH_FLAVOR" == "cuda" ]]; then
    PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install "audio-separator[gpu]==0.44.5"
else
    PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install "audio-separator[cpu]==0.44.5"
fi

PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install "$REPO_ROOT"

printf '%s\n' "$TORCH_FLAVOR" > "$FLAVOR_FILE"

# ---------------------------------------------------------------- validation

echo "Verifying the Engine..."

PYTHONNOUSERSITE=1 "$PYTHON" -s - <<'PYCHECK'
import sys

import stemlab
import stemlab.recursive
import torch

# Importing torchaudio is the check, not a formality: its compiled extension
# is linked against one torch, and loading it against another is how a
# mismatched Engine fails - at the user's first separation rather than here.
import torchaudio

print(f"  stemlab import: ok")
print(f"  recursive splitting available: {stemlab.recursive.Separator is not None}")
print(f"  torch {torch.__version__}, CUDA available: {torch.cuda.is_available()}")
print(f"  torchaudio {torchaudio.__version__}")

if torch.__version__.split("+")[0] != torchaudio.__version__.split("+")[0]:
    print(
        f"  torch {torch.__version__} and torchaudio {torchaudio.__version__} "
        "are different versions; demucs would fail on this Engine.",
        file=sys.stderr,
    )
    raise SystemExit(1)
PYCHECK

# ------------------------------------------------------------------- pointer

if [[ $WRITE_POINTER -eq 1 ]]; then
    mkdir -p "$CONFIG_HOME/StemLab"
    printf '%s\n' "$PYTHON" > "$CONFIG_HOME/StemLab/portable_engine_path.txt"

    cat <<EOF

StemLab backend installed.

  Engine:  $DEST
  Pointer: $CONFIG_HOME/StemLab/portable_engine_path.txt

The plugin and Standalone app will discover it automatically - no settings
needed. Re-run this script any time to update.
EOF
else
    cat <<EOF

StemLab backend assembled (no discovery pointer written).

  Engine:  $DEST
EOF
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    cat <<'EOF'
NOTE: ffmpeg was not found on PATH. It is needed for MP3/OGG/AIFF input
(WAV and FLAC work without it). Install it with your package manager, e.g.:
  sudo apt install ffmpeg
EOF
fi
