#!/usr/bin/env bash
#
# Compare reference demucs (PyTorch) against demucs-rs (Burn/wgpu) on the same
# input, for speed and for output parity.
#
# The point of demucs-rs is its Vulkan path, and that path has a trap: when no
# usable GPU is present, wgpu silently falls back to a software rasteriser
# (llvmpipe / lavapipe) and still runs to completion. A benchmark taken that
# way measures a slow CPU renderer while looking exactly like a GPU result, so
# this script refuses to call a run "GPU" unless it can name the adapter.
#
# Usage:
#   scripts/compare-demucs-backends.sh INPUT.wav [options]
#
#   --demucs-rs PATH     demucs-rs checkout (default: ./third_party/demucs-rs,
#                        cloned if missing)
#   --out DIR            results directory (default: ./demucs-comparison)
#   --model NAME         htdemucs | htdemucs_6s | htdemucs_ft (default: htdemucs_6s)
#   --python-device DEV  cpu | cuda | mps (default: cuda if visible, else cpu)
#   --rust-backend B     vulkan | cpu (default: vulkan). "cpu" builds the
#                        NdArray backend and skips every Vulkan check, which
#                        is the way to get a pure numerical parity run on a
#                        machine with no GPU.
#   --overlap N          python demucs overlap (default: 0.25)
#   --shifts N           python demucs random shifts (default: 0 - see below)
#   --min-si-sdr DB      parity gate, dB (default: 15)
#   --min-correlation R  parity gate, 0-1 (default: 0.98)
#   --with-rust-cpu      also run the NdArray CPU build, as a numerical control
#   --skip-build         reuse binaries already in demucs-rs/target/release
#   -h, --help
#
# Why overlap defaults to 0.25: demucs-rs hardcodes a 25% chunk overlap
# (stride = segment * 3/4), and comparing outputs is only meaningful when both
# sides use the same value. Note that StemLab ships --overlap 0.10 for speed,
# so the python timing below is NOT our production timing; it is the setting
# that makes the parity numbers comparable.
#
# Why shifts defaults to 0: demucs' own default is --shifts 1, which applies an
# UNSEEDED random time offset (apply.py: offset = random.randint(0, max_shift)).
# That makes python demucs non-deterministic run to run - two consecutive runs
# of the same file disagree about as much as python and demucs-rs do, which is
# enough to manufacture a "porting bug" out of nothing. With --shifts 0 python
# reproduces itself exactly (correlation 1.0, infinite SI-SDR), so any
# remaining difference is genuinely the other implementation. demucs-rs
# implements no shifts, so 0 is also the like-for-like setting.

set -euo pipefail

RS_DIR=""
OUT_DIR="$PWD/demucs-comparison"
MODEL="htdemucs_6s"
PY_DEVICE=""
OVERLAP="0.25"
SHIFTS="0"
MIN_SI_SDR="15"
MIN_CORR="0.98"
WITH_RUST_CPU=0
RUST_BACKEND="vulkan"
SKIP_BUILD=0
INPUT=""

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --demucs-rs)     RS_DIR="$2"; shift 2 ;;
    --out)           OUT_DIR="$2"; shift 2 ;;
    --model)         MODEL="$2"; shift 2 ;;
    --python-device) PY_DEVICE="$2"; shift 2 ;;
    --overlap)       OVERLAP="$2"; shift 2 ;;
    --shifts)        SHIFTS="$2"; shift 2 ;;
    --min-si-sdr)    MIN_SI_SDR="$2"; shift 2 ;;
    --min-correlation) MIN_CORR="$2"; shift 2 ;;
    --rust-backend)  RUST_BACKEND="$2"; shift 2 ;;
    --with-rust-cpu) WITH_RUST_CPU=1; shift ;;
    --skip-build)    SKIP_BUILD=1; shift ;;
    -h|--help)       sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)              die "unknown option: $1" ;;
    *)               [[ -n "$INPUT" ]] && die "only one input file"; INPUT="$1"; shift ;;
  esac
done

case "$RUST_BACKEND" in
  vulkan|cpu) ;;
  *) die "--rust-backend must be 'vulkan' or 'cpu' (got: $RUST_BACKEND)" ;;
esac
[[ -n "$INPUT" ]] || die "no input file given (try --help)"
[[ -f "$INPUT" ]] || die "input not found: $INPUT"
INPUT="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -n "$RS_DIR" ]] || RS_DIR="$REPO_ROOT/third_party/demucs-rs"

# ---------------------------------------------------------------- preflight

command -v cargo >/dev/null || die "cargo not found - install Rust from https://rustup.rs"

PYTHON="${PYTHON:-}"
if [[ -z "$PYTHON" ]]; then
  for candidate in \
      "$HOME/.local/share/StemLab/Engine/bin/python" \
      "$REPO_ROOT/.venv/bin/python" \
      python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && \
       "$candidate" -c "import demucs" >/dev/null 2>&1; then
      PYTHON="$candidate"; break
    fi
  done
fi
[[ -n "$PYTHON" ]] || die "no python with the 'demucs' package found (set PYTHON=/path/to/python)"
info "python:  $PYTHON ($("$PYTHON" -c 'import demucs; print("demucs", demucs.__version__)'))"

if [[ -z "$PY_DEVICE" ]]; then
  PY_DEVICE="$("$PYTHON" - <<'PY'
try:
    import torch
    print("cuda" if torch.cuda.is_available()
          else ("mps" if torch.backends.mps.is_available() else "cpu"))
except Exception:
    print("cpu")
PY
)"
fi
info "python device: $PY_DEVICE"

# Is there a real Vulkan GPU, or only a software rasteriser? Both answers are
# usable, but they must not be confused for each other in the results.
GPU_NAME="n/a"
GPU_IS_SOFTWARE=0
if [[ "$RUST_BACKEND" != "vulkan" ]]; then
  info "rust backend: ndarray cpu (Vulkan checks skipped)"
elif command -v vulkaninfo >/dev/null 2>&1; then
  GPU_NAME="$(vulkaninfo --summary 2>/dev/null \
    | awk '/deviceName/ {sub(/.*= */,""); print; exit}')"
  [[ -n "$GPU_NAME" ]] || GPU_NAME="unknown"
  if grep -qiE 'llvmpipe|lavapipe|swiftshader|softwar' <<<"$GPU_NAME"; then
    GPU_IS_SOFTWARE=1
  fi
elif ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
  warn "vulkaninfo not installed - cannot name the adapter (install vulkan-tools)"
else
  die "no Vulkan ICD found in /usr/share/vulkan/icd.d - install your GPU's Vulkan driver.
       Without one, wgpu has nothing to run on and the GPU leg is meaningless."
fi

if [[ "$RUST_BACKEND" != "vulkan" ]]; then
  :
elif (( GPU_IS_SOFTWARE )); then
  warn "Vulkan adapter is '$GPU_NAME' - a SOFTWARE rasteriser, not a GPU.
         The 'vulkan' timing below will be a slow CPU render. Treat it as a
         correctness check only, never as a performance number."
else
  info "vulkan adapter: $GPU_NAME"
fi

# ------------------------------------------------------------------- build

if [[ ! -d "$RS_DIR" ]]; then
  info "cloning demucs-rs into $RS_DIR"
  mkdir -p "$(dirname "$RS_DIR")"
  git clone --depth 1 https://github.com/jacobmarks/demucs-rs "$RS_DIR" \
    || die "clone failed - pass an existing checkout with --demucs-rs PATH"
fi
[[ -f "$RS_DIR/Cargo.toml" ]] || die "not a demucs-rs checkout: $RS_DIR"

BIN_VULKAN="$RS_DIR/target/release/demucs"
BIN_CPU="$RS_DIR/target/cpu-build/release/demucs"

# The feature flag changes the backend type all the way down, so the two
# builds get separate target directories - sharing one would trigger a full
# rebuild every time you alternate between them.
build_vulkan() {
  info "building demucs-rs (wgpu/vulkan) - the first build takes a while"
  ( cd "$RS_DIR" && cargo build --release --bin demucs )
}
build_cpu() {
  info "building demucs-rs (ndarray cpu)"
  ( cd "$RS_DIR" && CARGO_TARGET_DIR="$RS_DIR/target/cpu-build" \
      cargo build --release --bin demucs --features cpu )
}

if [[ "$RUST_BACKEND" == "vulkan" ]]; then
  PRIMARY_LABEL="vulkan"; PRIMARY_BIN="$BIN_VULKAN"
else
  PRIMARY_LABEL="rust-cpu"; PRIMARY_BIN="$BIN_CPU"
  # Asking for the CPU control on top of a CPU primary would run it twice.
  WITH_RUST_CPU=0
fi

if (( ! SKIP_BUILD )); then
  if [[ "$RUST_BACKEND" == "vulkan" ]]; then build_vulkan; else build_cpu; fi
  (( WITH_RUST_CPU )) && build_cpu
fi
[[ -x "$PRIMARY_BIN" ]] || die "missing binary: $PRIMARY_BIN (drop --skip-build?)"

# -------------------------------------------------------------------- run

mkdir -p "$OUT_DIR"
STEM_NAME="$(basename "${INPUT%.*}")"
RESULTS="$OUT_DIR/results.json"

# bash's SECONDS is integer-only; these runs are minutes long but the ratio
# deserves better than 1 s granularity.
now() { date +%s.%N; }
elapsed() { "$PYTHON" -c 'import sys; print(f"{float(sys.argv[2])-float(sys.argv[1]):.2f}")' "$1" "$2"; }

run_timed() {  # name, output_dir, command...
  local name="$1" outdir="$2"; shift 2
  info "running $name"
  rm -rf "$outdir"; mkdir -p "$outdir"
  local t0 t1 rc=0
  t0="$(now)"
  "$@" > "$OUT_DIR/$name.log" 2>&1 || rc=$?
  t1="$(now)"
  if (( rc != 0 )); then
    warn "$name failed (exit $rc) - see $OUT_DIR/$name.log"
    tail -15 "$OUT_DIR/$name.log" >&2
    echo "FAILED" > "$OUT_DIR/$name.seconds"
    return 1
  fi
  elapsed "$t0" "$t1" > "$OUT_DIR/$name.seconds"
  info "$name took $(cat "$OUT_DIR/$name.seconds")s"
}

run_timed python "$OUT_DIR/python" \
  "$PYTHON" -m demucs.separate --name "$MODEL" --device "$PY_DEVICE" \
            --overlap "$OVERLAP" --shifts "$SHIFTS" \
            --out "$OUT_DIR/python" "$INPUT" || true

if [[ "$SHIFTS" != "0" ]]; then
  warn "--shifts $SHIFTS makes python demucs non-deterministic; the parity
         numbers below will include its own run-to-run variance."
fi

# --debug makes demucs-rs emit plain per-checkpoint progress lines on stderr
# (it costs ~2% and, unlike its indicatif bar, survives a non-tty pipe).
run_timed "$PRIMARY_LABEL" "$OUT_DIR/$PRIMARY_LABEL" \
  "$PRIMARY_BIN" --debug -m "$MODEL" -o "$OUT_DIR/$PRIMARY_LABEL" "$INPUT" || true

if (( WITH_RUST_CPU )); then
  run_timed rust-cpu "$OUT_DIR/rust-cpu" \
    "$BIN_CPU" --debug -m "$MODEL" -o "$OUT_DIR/rust-cpu" "$INPUT" || true
fi

# ---------------------------------------------------------------- compare

PY_STEMS="$OUT_DIR/python/$MODEL/$STEM_NAME"
[[ -d "$PY_STEMS" ]] || warn "python stems not where expected: $PY_STEMS"

compare() {  # candidate_dir, label
  local dir="$1" label="$2"
  [[ -d "$PY_STEMS" && -d "$dir" ]] || return 0
  ls "$dir"/*.wav >/dev/null 2>&1 || return 0
  echo
  info "parity: $label vs python"
  # These gates are a cross-implementation tolerance, not a bit-exactness
  # check: two independent implementations of the same network agree to a
  # few dB, never to the harness's strict defaults.
  PYTHONPATH="$REPO_ROOT/src" "$PYTHON" -m stemlab.regression \
    --reference "$PY_STEMS" --candidate "$dir" \
    --min-si-sdr "$MIN_SI_SDR" --min-correlation "$MIN_CORR" \
    --json "$OUT_DIR/parity-$label.json" || true
}

compare "$OUT_DIR/$PRIMARY_LABEL" "$PRIMARY_LABEL"
(( WITH_RUST_CPU )) && compare "$OUT_DIR/rust-cpu" rust-cpu

# A caveat the table above cannot express: a stem that is silent in BOTH
# engines compares noise floor against noise floor, and its correlation is
# meaningless however bad it looks. Flag those explicitly so a "FAIL" on an
# empty stem is not read as a real regression.
echo
info "stem levels (a stem far below the mixture carries no content to compare)"
PYTHONPATH="$REPO_ROOT/src" "$PYTHON" - "$INPUT" "$PY_STEMS" "$OUT_DIR/$PRIMARY_LABEL" <<'PY' || true
import sys, pathlib
import numpy as np, soundfile as sf

mix_path, ref_dir, cand_dir = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
mix, _ = sf.read(mix_path, always_2d=True)
mix_rms = float(np.sqrt((mix ** 2).mean()))

def rms_db(path):
    audio, _ = sf.read(str(path), always_2d=True)
    r = float(np.sqrt((audio ** 2).mean()))
    return 20 * np.log10(r / mix_rms) if r > 0 and mix_rms > 0 else float("-inf")

print(f"  {'stem':10s} {'python':>10s} {'rust':>10s}   (dB relative to the mixture)")
for ref in sorted(ref_dir.glob("*.wav")):
    cand = cand_dir / ref.name
    if not cand.exists():
        continue
    a, b = rms_db(ref), rms_db(cand)
    loudest = max(a, b)
    if loudest < -40:
        note = "  <- silent on both; parity here is meaningless"
    elif loudest < -20:
        note = "  <- bleed only; parity here is unreliable"
    else:
        note = ""
    print(f"  {ref.stem:10s} {a:10.1f} {b:10.1f}{note}")
PY

# ---------------------------------------------------------------- summary

echo
info "timings"
read_secs() { [[ -f "$OUT_DIR/$1.seconds" ]] && cat "$OUT_DIR/$1.seconds" || echo "-"; }
PY_T="$(read_secs python)"; VK_T="$(read_secs "$PRIMARY_LABEL")"
CPU_T="-"; (( WITH_RUST_CPU )) && CPU_T="$(read_secs rust-cpu)"
printf '  %-24s %s s\n' "python ($PY_DEVICE)" "$PY_T"
printf '  %-24s %s s%s\n' "demucs-rs ($PRIMARY_LABEL)" "$VK_T" \
  "$( (( GPU_IS_SOFTWARE )) && echo '   [SOFTWARE RASTERISER - not a GPU number]')"
(( WITH_RUST_CPU )) && printf '  %-24s %s s\n' "demucs-rs (ndarray cpu)" "$CPU_T"

"$PYTHON" - "$RESULTS" "$PY_T" "$VK_T" "$CPU_T" "$PY_DEVICE" "$GPU_NAME" \
           "$GPU_IS_SOFTWARE" "$MODEL" "$OVERLAP" "$SHIFTS" "$INPUT" <<'PY'
import json, sys
keys = ["python_seconds","rust_seconds","rust_cpu_control_seconds","python_device",
        "vulkan_adapter","vulkan_is_software","model","overlap","shifts","input"]
out, vals = sys.argv[1], sys.argv[2:]
def num(v):
    try: return float(v)
    except (TypeError, ValueError): return None
d = dict(zip(keys, vals))
for k in ("python_seconds","rust_seconds","rust_cpu_control_seconds"):
    d[k] = num(d[k])
d["vulkan_is_software"] = d["vulkan_is_software"] == "1"
if d["python_seconds"] and d["rust_seconds"]:
    d["rust_speedup_vs_python"] = round(d["python_seconds"] / d["rust_seconds"], 2)
json.dump(d, open(out, "w"), indent=2)
print(f"\nwrote {out}")
PY

echo
info "outputs in $OUT_DIR"
