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
#
# Usage:
#   ./scripts/install_backend.sh              # auto-detect GPU -> cuda/rocm/cpu
#   ./scripts/install_backend.sh --cpu        # force CPU-only torch (smallest)
#   ./scripts/install_backend.sh --cuda       # force NVIDIA CUDA torch
#   ./scripts/install_backend.sh --rocm       # force AMD ROCm torch
#   ./scripts/install_backend.sh --xpu        # force Intel GPU (XPU) torch
#   ./scripts/install_backend.sh --dest DIR   # custom Engine location
#   ./scripts/install_backend.sh --reinstall  # rebuild the Engine from scratch
#   ./scripts/install_backend.sh --build-only # assembling a bundle: skip the
#                                             "installed" report
#
# --prune-only DIR runs the final prune step against an Engine that already
# exists and then exits. It sits below the block --help prints (lines 2-19)
# because it is a test hook for tests/test_engine_prune.py, not something a
# user has any reason to run.

set -euo pipefail

# Pinned relocatable CPython. 3.11 matches the interpreter the Windows
# portable release ships.
PBS_RELEASE="20250818"
PBS_PYTHON="3.11.13"

TORCH_FLAVOR="auto"
REINSTALL=0
BUILD_ONLY=0
DEST=""
PRUNE_ONLY=""
PRUNE_ONLY_SET=0

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpu)       TORCH_FLAVOR="cpu";  shift ;;
        --cuda)      TORCH_FLAVOR="cuda"; shift ;;
        --rocm)      TORCH_FLAVOR="rocm"; shift ;;
        --xpu)       TORCH_FLAVOR="xpu";  shift ;;
        --dest)      DEST="$2";           shift 2 ;;
        --reinstall) REINSTALL=1;         shift ;;
        --build-only) BUILD_ONLY=1;       shift ;;
        --prune-only) PRUNE_ONLY="$2"; PRUNE_ONLY_SET=1; shift 2 ;;
        -h|--help)
            sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2 ;;
    esac
done

# -------------------------------------------------------------- prune helpers

# The prune runs at the very end of the build; these live up here because bash
# needs them defined before that call, and because --prune-only has to answer
# before the preflight block starts reaching for curl.

# Shrink a finished Engine. Order matters: Tcl/Tk goes first so the strip pass
# does not spend time on files that are about to be deleted anyway.
#
# Measured on a torch 2.11.0+cpu dependency tree: 877.9 MB -> 731.7 MB, of
# which 98.6 MB is the strip pass and 47.6 MB is bundled test suites.
prune_engine() {
    local root="${1:-}"

    # Same rule the --reinstall rm -rf follows: a directory this script did not
    # create is never touched. set -u does not catch a variable that is set but
    # empty, and an empty root would turn the tcl/tk globs below into
    # "rm -rf /lib/tk*".
    [[ -n "$root" && "$root" == /* && -d "$root" ]] || {
        echo "Refusing to prune '$root': not an absolute existing directory." >&2
        return 1
    }

    [[ -f "$root/.stemlab-engine" ]] || {
        echo "Refusing to prune $root: no .stemlab-engine marker, so this" >&2
        echo "installer did not create it." >&2
        return 1
    }

    echo "Pruning the Engine..."
    prune_tcltk "$root"
    prune_bundled_tests "$root"
    prune_strip_shared_objects "$root"
}

# Tcl/Tk is dead weight here: nothing under src/ or scripts/ imports tkinter,
# and neither does anything pip puts beside it - torch, torchaudio, demucs,
# bs_roformer and audio-separator were all checked inside a built Engine. It
# runs headless as a subprocess of the plugin anyway, so a library that lazily
# picks a GUI backend has no display to pick Tk for and falls back to Agg.
# Worth 9.0 MB of the pinned interpreter.
#
# Left behind deliberately: idlelib, turtle.py and turtledemo import tkinter
# and stop working after this. They are neither Tcl/Tk nor tkinter, so pruning
# them is a separate decision rather than a tidy-up to make here.
#
# Nothing fails if a path is absent - an unmatched glob must stay a no-op.
prune_tcltk() {
    local root="$1"
    local index

    # Every removal below is tolerant of failure. Under `set -e` a single
    # read-only file or a racing antivirus would otherwise abort an install
    # that has already done all of its real work, moments before it reports
    # success. A prune that half-finishes costs disk; a prune that kills the
    # install costs the install.
    rm -rf "$root"/lib/python3.*/tkinter || true
    rm -f  "$root"/lib/python3.*/lib-dynload/_tkinter*.so || true
    rm -rf "$root"/lib/tcl* "$root"/lib/tk* "$root"/lib/libtcl* "$root"/lib/libtk* || true

    # itcl and thread sit beside Tcl under version-numbered directory names -
    # itcl4.2.4 and thread2.8.9 in the pinned interpreter. Find them by the
    # index file every Tcl package directory carries rather than by numbers
    # that move with the next CPython bump. lib/python3.11 and lib/pkgconfig
    # carry no pkgIndex.tcl, so the sweep cannot reach them.
    while IFS= read -r -d '' index; do
        local dir
        dir="$(dirname "$index")"

        # An allow-list, not an observation. Today only Tcl packages carry a
        # pkgIndex.tcl at this depth, but "no interpreter directory will ever
        # ship one" is a property of the current tarball rather than a rule,
        # and the consequence of being wrong is rm -rf on part of the stdlib.
        case "$(basename "$dir")" in
            itcl*|thread*|tcl*|tk*|tdbc*|sqlite3*|tclx*|expect*|itk*) ;;
            *) continue ;;
        esac

        rm -rf "$dir" || true
    done < <(find "$root/lib" -mindepth 2 -maxdepth 2 -name pkgIndex.tcl -print0 2>/dev/null)
}

# Bundled test suites. Every token of the find below is load-bearing:
#
#   -name tests   exact, never "test*" - numpy.testing is public API that
#                 numpy.ma, scipy.special and sklearn.utils import at run time,
#                 and a "test*" glob takes it out along with the test suites.
#   -not -path    keeps the whole numpy/testing subtree, including the
#                 numpy/testing/tests directory nested inside it.
#   find . after  the filter above matches ancestors too. Rooted at an absolute
#   cd            path, an Engine installed under ~/testing/ would match on the
#                 ancestor and select nothing at all - a prune that silently
#                 does nothing is worse than no prune.
#   -type d       a package that ships a *file* called tests keeps it.
#   -mindepth 2   only test suites nested inside a package; a top-level
#                 site-packages/tests could be something importable.
#
# This leaves the wheels' RECORD hashes stale. pip does not verify them on
# uninstall or upgrade, so nothing breaks, but an integrity checker would see a
# modified install tree.
prune_bundled_tests() {
    local root="$1"
    local sp d

    for sp in "$root"/lib/python3.*/site-packages; do
        [[ -d "$sp" ]] || continue

        while IFS= read -r -d '' d; do
            # Tolerant for the same reason as prune_tcltk: it runs after
            # every expensive step, with nothing left to redo it.
            rm -rf "$sp/$d" || true
            echo "  removed $d"
        done < <(cd "$sp" && find . -mindepth 2 -type d -name tests \
                                    -not -path '*/testing/*' -prune -print0)
    done
}

# --strip-unneeded, never plain "strip": it is defined to keep everything
# needed for relocation processing, so it stays correct across binutils and
# llvm-strip versions. What plain strip happens to leave in a shared object is
# an implementation detail, and this is not a good thing to bet an Engine on.
# Worth 98.6 MB on a torch 2.11.0+cpu install - most of what the prune saves.
prune_strip_shared_objects() {
    local root="$1"
    local f
    local stripped=0

    # A missing binutils must not fail an otherwise good install - this is a
    # size optimisation, not part of the build.
    if ! command -v strip >/dev/null 2>&1; then
        echo "  strip(1) not found; leaving shared objects as they are."
        return 0
    fi

    # Per-file failure is ignored on purpose: site-packages contains .so names
    # that are not ELF at all, and objects that are already stripped.
    # -type f so a versioned .so.1.2.3 is not stripped a second time through
    # the .so symlink beside it.
    while IFS= read -r -d '' f; do
        strip --strip-unneeded "$f" 2>/dev/null && stripped=$((stripped + 1)) || true
    done < <(find "$root" -type f -name '*.so*' -print0)

    echo "  stripped $stripped shared objects."
}

# __pycache__ stays, and here is the measurement that says so, because deleting
# it is the obvious next idea and it is a lot of megabytes. A built cpu Engine
# carries 189 MB of bytecode out of 1431 MB - 13.2%.
#
# What it buys back: on numpy + scipy + sklearn + joblib and their vendored
# .libs, nine fresh processes each importing numpy, scipy.signal,
# scipy.interpolate, sklearn.decomposition and sklearn.cluster took a median
# 1362 ms with bytecode and 3049 ms without it and unable to write it back.
# +1687 ms, 2.24x, on every single run, for four packages, before torch and
# demucs are in the picture - and the plugin starts a fresh Engine process per
# job. A writable Engine pays it once and self-heals (3358 ms, then ~1400 ms),
# but the bundle can land somewhere read-only, where it never does.
# 13% of the size for double the startup is not a trade worth making.
#
# The .py sources stay either way - dropping those breaks tracebacks and every
# tool that reads source.

# Test hook. Answered here so the selection logic can be driven without a
# network, an interpreter, or a two-gigabyte torch download.
if [[ "$PRUNE_ONLY_SET" == 1 ]]; then
    # Gated on the flag having been PASSED, not on its value being non-empty.
    # "--prune-only ''" is a malformed request to prune; treating it as
    # "prune-only was not asked for" turned it into a full ~1.4 GB network
    # install that replaced the Engine. It has already happened once.
    if [[ -z "$PRUNE_ONLY" ]]; then
        echo "--prune-only needs a directory." >&2
        exit 2
    fi

    [[ "$PRUNE_ONLY" == /* ]] || PRUNE_ONLY="$PWD/$PRUNE_ONLY"
    prune_engine "$PRUNE_ONLY"
    exit 0
fi

# Relative XDG values are invalid per the spec and are ignored by the plugin,
# so the installer must ignore them the same way or the Engine lands where the
# plugin never looks. The plugin has one location for it and does not search.
DATA_HOME="${XDG_DATA_HOME:-}"
[[ "$DATA_HOME" == /* ]] || DATA_HOME="$HOME/.local/share"

[[ -n "$DEST" ]] || DEST="$DATA_HOME/StemLab/Engine"

# Absolute from here on: the paths baked into the Engine's own scripts have no
# working directory to resolve against.
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

# Everything installed from here on resolves against PyPI, and one of those
# resolutions replaces torch if nothing stops it: audio-separator depends on
# onnx2torch, which depends on torchvision, and the newest torchvision pins
# the exact newest torch - so pip upgrades torch to PyPI's build, which on
# Linux is the CUDA one, whatever flavor was just installed. A constraints
# file makes pip backtrack torchvision to one that matches the torch already
# here instead. Base versions on purpose: "torch==2.11.0" is satisfied by
# the installed 2.11.0+cpu, while "torch==2.11.0+cpu" names a wheel PyPI
# does not carry and would fail the resolve outright.
TORCH_CONSTRAINTS="$DEST/.stemlab-torch-constraints.txt"

# audio-separator -> onnx2torch-py313 -> onnx-weekly, which is a rolling
# NIGHTLY, not a release. Unpinned, every rebuild resolves to whatever was
# published that week, so two bundles built days apart carry different onnx
# and a user's install never matches CI's.
#
# The pin cannot be permanent: PyPI keeps only about the last 29 of these,
# an ~11-month window, so this exact build will eventually be deleted. The
# install therefore retries without the pin rather than failing outright,
# and says so - a reproducible build is worth having, but not at the cost of
# an installer that stops working a year from now. When that warning
# appears, refresh ONNX_WEEKLY_PIN to a version that still exists.
ONNX_WEEKLY_PIN="1.23.0.dev20260824"

printf 'torch==%s\ntorchaudio==%s\n' "$TORCH_VERSION" "$TORCH_VERSION" \
    > "$TORCH_CONSTRAINTS"
printf 'onnx-weekly==%s\n' "$ONNX_WEEKLY_PIN" >> "$TORCH_CONSTRAINTS"

# Recursive/adaptive stem splitting needs audio-separator. The project's own
# "recursive" extra pins the CUDA onnxruntime unconditionally, so install the
# flavor-matched build here instead: CUDA gets the GPU runtime; every other
# flavor gets the CPU runtime (onnxruntime has no PyPI build for ROCm/XPU, so
# the recursive stage runs on CPU there - the main separation still offloads).
if [[ "$TORCH_FLAVOR" == "cuda" ]]; then
    SEPARATOR_SPEC="audio-separator[gpu]==0.44.5"
else
    SEPARATOR_SPEC="audio-separator[cpu]==0.44.5"
fi

if ! PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install \
        -c "$TORCH_CONSTRAINTS" "$SEPARATOR_SPEC"; then
    # Almost always the pinned nightly having aged out of PyPI. Drop just
    # that pin - never the torch ones, which exist to stop the flavor being
    # replaced - and carry on unreproducibly rather than not at all.
    echo
    echo "WARNING: installing $SEPARATOR_SPEC failed with onnx-weekly pinned to"
    echo "         $ONNX_WEEKLY_PIN. That nightly has most likely been removed"
    echo "         from PyPI. Retrying unpinned: the install will succeed but"
    echo "         this build is no longer reproducible. Refresh"
    echo "         ONNX_WEEKLY_PIN in scripts/linux/install_backend.sh to a"
    echo "         version that still exists." >&2
    grep -v '^onnx-weekly==' "$TORCH_CONSTRAINTS" > "$TORCH_CONSTRAINTS.notonnx"
    mv "$TORCH_CONSTRAINTS.notonnx" "$TORCH_CONSTRAINTS"
    PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install \
        -c "$TORCH_CONSTRAINTS" "$SEPARATOR_SPEC"
fi

PYTHONNOUSERSITE=1 "$PYTHON" -s -m pip install -c "$TORCH_CONSTRAINTS" "$REPO_ROOT"

printf '%s\n' "$TORCH_FLAVOR" > "$FLAVOR_FILE"

# ---------------------------------------------------------------------- prune

# Before the validation block, not after it, so the import check below runs on
# a pruned Engine rather than an unpruned one. Also after every pip install, so
# nothing puts back what was just removed.
#
# What that check actually proves is narrower than it looks, and the difference
# matters on a GPU install. It imports stemlab, torch, torchaudio and soxr, so
# it does cover the CPU shared objects the strip pass rewrites - torchaudio's
# extension in particular is linked against libtorch and will not load if
# stripping damaged it. It does NOT dlopen the CUDA/ROCm/XPU kernel libraries:
# torch.cuda.is_available() probes the driver without loading them. So on
# cuda, rocm and xpu the strip pass over those trees is unverified by this
# install, and was never exercised in development either - only the cpu flavor
# was ever built and pruned.
prune_engine "$DEST"

# ---------------------------------------------------------------- validation

echo "Verifying the Engine..."

PYTHONNOUSERSITE=1 "$PYTHON" -s - <<'PYCHECK'
import sys

import stemlab
import stemlab.recursive
import torch

# The BS-RoFormer rate alignment reaches for soxr on the first non-44.1 kHz
# separation, which is well after the model has loaded. It arrives through
# librosa and beat-this rather than being installed for its own sake, so it
# is worth failing here instead of there.
import soxr

# Importing torchaudio is the check, not a formality: its compiled extension
# is linked against one torch, and loading it against another is how a
# mismatched Engine fails - at the user's first separation rather than here.
import torchaudio

print(f"  stemlab import: ok")
print(f"  recursive splitting available: {stemlab.recursive.Separator is not None}")
print(f"  torch {torch.__version__}, CUDA available: {torch.cuda.is_available()}")
print(f"  torchaudio {torchaudio.__version__}")
print(f"  soxr {soxr.__version__}")

if torch.__version__.split("+")[0] != torchaudio.__version__.split("+")[0]:
    print(
        f"  torch {torch.__version__} and torchaudio {torchaudio.__version__} "
        "are different versions; demucs would fail on this Engine.",
        file=sys.stderr,
    )
    raise SystemExit(1)
PYCHECK

# -------------------------------------------------------------------- report

if [[ $BUILD_ONLY -eq 0 ]]; then
    cat <<EOF

StemLab backend installed.

  Engine:  $DEST
EOF

    # There is no discovery any more: the plugin looks in one place. A custom
    # --dest is still useful for building a bundle, but it is not somewhere
    # the app will go looking, so say so rather than let it look installed.
    if [[ "$DEST" != "$DATA_HOME/StemLab/Engine" ]]; then
        cat <<EOF
This is not the location StemLab reads. Point it here explicitly:

  export STEMLAB_ENGINE="$PYTHON"

EOF
    else
        cat <<EOF
The plugin and Standalone app read this location directly - no settings
needed. Re-run this script any time to update.
EOF
    fi
else
    cat <<EOF

StemLab backend assembled.

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
