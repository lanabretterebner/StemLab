#!/usr/bin/env bash
#
# Compare StemLab's BS-RoFormer (PyTorch) against BSRoformer.cpp (ggml) on the
# same input, for speed and for output parity, across quantization types.
#
# This exists to answer one question cheaply: is BSRoformer.cpp worth adopting?
# Two things decide it, and the second is the one most likely to kill it:
#
#   1. Throughput. On a machine with no CUDA, PyTorch falls back to CPU, so
#      ggml only has to beat CPU PyTorch to be worth having.
#   2. Quantization quality. ggml's speed comes largely from Q8_0, and audio
#      separation is a worse fit for that than an LLM: a language model only
#      needs the argmax token to survive quantization, whereas here the
#      numbers ARE the output. Nobody has measured this for BS-RoFormer.
#
# So the script converts at several dtypes and scores each one, and it also
# scores q8_0 against the C++ fp32 run rather than only against PyTorch. That
# separates quantization damage from porting error - two different problems
# with two different owners.
#
# Usage:
#   scripts/compare-roformer-backends.sh INPUT.wav [options]
#
#   --dtypes LIST        comma-separated: fp32,fp16,q8_0,q4_0,q5_0 (default fp32,q8_0)
#   --backends LIST      comma-separated: cpu,vulkan,sycl (default: cpu, plus
#                        vulkan when a non-software adapter is present).
#                        sycl is Intel's own stack and is usually the faster
#                        path on Intel graphics than generic Vulkan, but it
#                        needs oneAPI installed and its environment sourced
#                        (setvars.sh), so it is never selected automatically.
#   --repeat N           run each C++ configuration N times (default 2; the
#                        first Vulkan run compiles shaders, so 1 measures setup)
#   --out DIR            results directory (default ./roformer-comparison)
#   --src DIR            BSRoformer.cpp checkout (default third_party/BSRoformer.cpp)
#   --model NAME         cached model id (default roformer-model-bs-roformer-sw-by-jarredou)
#   --timeout SECONDS    kill any single separation that exceeds this (default 1800)
#   --skip-build         reuse existing build directories
#   --probe              report the environment and exit, separating nothing
#   -h, --help
#
# A note on why the timeout exists: on an Intel iGPU, a wgpu-based port of the
# other model hung the GPU hard enough for the kernel to reset the device. A
# Vulkan run that wedges must not take the whole comparison with it.

set -uo pipefail

OUT_DIR="$PWD/roformer-comparison"
SRC_DIR=""
MODEL="roformer-model-bs-roformer-sw-by-jarredou"
DTYPES="fp32,q8_0"
BACKENDS=""
REPEAT=2
RUN_TIMEOUT=1800
SKIP_BUILD=0
PROBE=0
INPUT=""

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
step() { printf '\n\033[1m%s\033[0m\n' "$*"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dtypes)     DTYPES="$2"; shift 2 ;;
    --backends)   BACKENDS="$2"; shift 2 ;;
    --repeat)     REPEAT="$2"; shift 2 ;;
    --out)        OUT_DIR="$2"; shift 2 ;;
    --src)        SRC_DIR="$2"; shift 2 ;;
    --model)      MODEL="$2"; shift 2 ;;
    --timeout)    RUN_TIMEOUT="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --probe)      PROBE=1; shift ;;
    -h|--help)    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)           die "unknown option: $1" ;;
    *)            [[ -n "$INPUT" ]] && die "only one input file"; INPUT="$1"; shift ;;
  esac
done

[[ "$REPEAT" =~ ^[1-9][0-9]*$ ]] || die "--repeat must be a positive integer"
(( PROBE )) || [[ -n "$INPUT" ]] || die "no input file given (try --help)"
if [[ -n "$INPUT" ]]; then
  [[ -f "$INPUT" ]] || die "input not found: $INPUT"
  INPUT="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -n "$SRC_DIR" ]] || SRC_DIR="$REPO_ROOT/third_party/BSRoformer.cpp"
GGML_DIR="$(dirname "$SRC_DIR")/ggml"

# ------------------------------------------------------------- interpreter

step "Environment"

PYTHON="${PYTHON:-}"
TRIED=()
try_python() {
  local c="$1"
  [[ -n "$c" ]] || return 1
  command -v "$c" >/dev/null 2>&1 || { TRIED+=("$c  (not found)"); return 1; }
  "$c" -c "import bs_roformer" >/dev/null 2>&1 || { TRIED+=("$c  (no bs_roformer)"); return 1; }
  PYTHON="$c"; return 0
}

if [[ -n "$PYTHON" ]]; then
  "$PYTHON" -c "import bs_roformer" >/dev/null 2>&1 \
    || die "PYTHON=$PYTHON cannot import bs_roformer"
else
  CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
  DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
  POINTER="$CONFIG_HOME/StemLab/portable_engine_path.txt"
  if [[ -r "$POINTER" ]]; then
    try_python "$(head -1 "$POINTER" | tr -d '\r')" || true
  else
    TRIED+=("$POINTER  (no pointer file)")
  fi
  if [[ -z "$PYTHON" ]]; then
    for c in "$DATA_HOME/StemLab/Engine/bin/python" \
             "${STEMLAB_INSTALL_DIR:-/opt/StemLab}/Engine/bin/python" \
             "$REPO_ROOT/.venv/bin/python" python3 python; do
      try_python "$c" && break
    done
  fi
fi

if [[ -z "$PYTHON" ]]; then
  printf '\033[31merror:\033[0m no python with StemLab'"'"'s BS-RoFormer engine.\n' >&2
  printf '       Tried:\n' >&2
  printf '         %s\n' "${TRIED[@]}" >&2
  printf '\n       Install the Engine, or point at an interpreter:\n' >&2
  printf '         ./scripts/linux/install_backend.sh --cpu\n' >&2
  printf '         PYTHON=/path/to/python %s ...\n' "$0" >&2
  exit 1
fi
info "python: $PYTHON"

# ------------------------------------------------------------------ model

CACHE_DIR="${BS_ROFORMER_MODELS_PATH:-$HOME/.cache/bs-roformer-infer}/$MODEL"
CKPT="$(ls "$CACHE_DIR"/*.ckpt 2>/dev/null | head -1 || true)"
CONFIG="$(ls "$CACHE_DIR"/*.yaml 2>/dev/null | head -1 || true)"

if [[ -z "$CKPT" || -z "$CONFIG" ]]; then
  warn "model not cached at $CACHE_DIR
         Run one separation through StemLab first so the weights download,
         then re-run this script."
  (( PROBE )) || die "cannot convert without the checkpoint"
else
  info "checkpoint: $CKPT"
  info "config:     $CONFIG"
fi

# The stem order is the config's own instrument list, so a model with a
# different ordering still lands in the right files.
if [[ -n "$CONFIG" ]]; then
  mapfile -t STEMS < <("$PYTHON" - "$CONFIG" <<'PY'
import sys, yaml
cfg = yaml.safe_load(open(sys.argv[1]))
for name in cfg["training"]["instruments"]:
    print(name)
PY
)
  info "stems (in model order): ${STEMS[*]}"
fi

# The converter is a script from another project and pulls in modules the
# StemLab Engine has no reason to carry. Check them now: finding this after a
# long build costs an afternoon, and the fix is one pip install.
CONVERTER_MISSING=()
for module in gguf einops yaml librosa torch numpy; do
  "$PYTHON" -c "import $module" >/dev/null 2>&1 || CONVERTER_MISSING+=("$module")
done
if (( ${#CONVERTER_MISSING[@]} )); then
  warn "the GGUF converter needs modules this interpreter does not have:
           ${CONVERTER_MISSING[*]}
         Install them into the same interpreter:
           $PYTHON -m pip install ${CONVERTER_MISSING[*]}
         (gguf in particular is not part of StemLab's Engine.)"
  CONVERTER_READY=0
else
  info "converter dependencies: present"
  CONVERTER_READY=1
fi

# ----------------------------------------------------------------- vulkan

GPU_NAME="n/a"; GPU_IS_SOFTWARE=0; VULKAN_USABLE=0

is_software_adapter() { [[ "$1" == *CPU* ]] && return 0; grep -qiE 'llvmpipe|lavapipe|swiftshader|softpipe' <<<"$2"; }

if command -v vulkaninfo >/dev/null 2>&1; then
  ADAPTERS="$(vulkaninfo --summary 2>/dev/null | awk '
      /deviceType/ { sub(/.*= */, ""); gsub(/[ \t\r]+$/, ""); t = $0 }
      /deviceName/ { sub(/.*= */, ""); gsub(/[ \t\r]+$/, ""); print t "\t" $0 }')"
  if [[ -n "$ADAPTERS" ]]; then
    info "vulkan adapters:"
    while IFS=$'\t' read -r atype aname; do
      [[ -n "$aname" ]] || continue
      if is_software_adapter "$atype" "$aname"; then
        printf '      %-44s %s  [software]\n' "$aname" "${atype#PHYSICAL_DEVICE_TYPE_}"
      else
        printf '      %-44s %s\n' "$aname" "${atype#PHYSICAL_DEVICE_TYPE_}"
        VULKAN_USABLE=1; GPU_NAME="$aname"
      fi
    done <<< "$ADAPTERS"
    (( VULKAN_USABLE )) || { GPU_IS_SOFTWARE=1; GPU_NAME="$(head -1 <<<"$ADAPTERS" | cut -f2)"; }
  fi
fi
if (( ! VULKAN_USABLE )); then
  if (( GPU_IS_SOFTWARE )); then
    warn "the only Vulkan adapter is '$GPU_NAME', a SOFTWARE rasteriser. It will
         run and produce a timing, and that timing will measure a CPU renderer
         rather than a GPU. The vulkan backend is skipped unless you ask for it
         explicitly with --backends vulkan, and if you do, treat its number as
         a correctness check only."
  else
    warn "no Vulkan adapter found; the vulkan backend will be skipped unless you
         ask for it explicitly with --backends vulkan."
  fi
fi

if [[ -z "$BACKENDS" ]]; then
  BACKENDS="cpu"
  (( VULKAN_USABLE )) && BACKENDS="cpu,vulkan"
fi
info "backends: $BACKENDS    dtypes: $DTYPES"

# --------------------------------------------------------------- input rate

if [[ -n "$INPUT" ]]; then
  RATE="$("$PYTHON" -c "import soundfile as sf,sys; print(sf.info(sys.argv[1]).samplerate)" "$INPUT" 2>/dev/null || echo 0)"
  info "input: $(basename "$INPUT")  ${RATE} Hz"
  if [[ "$RATE" != "44100" ]]; then
    warn "input is ${RATE} Hz, and this model is trained at 44100. Both engines
         receive the same file so the comparison is still fair, but neither
         result reflects what the model can do. Prefer a 44.1 kHz clip."
  fi
fi

if (( PROBE )); then
  info "probe only - nothing separated."
  exit 0
fi

# ------------------------------------------------------------------ build

(( CONVERTER_READY )) || die "cannot convert the checkpoint until those modules are installed.
       Stopping before the build rather than after it."

step "Build"

need() { command -v "$1" >/dev/null || die "$1 not found - install it before building"; }
need cmake
need git

if [[ ! -d "$SRC_DIR" ]]; then
  info "cloning BSRoformer.cpp"
  mkdir -p "$(dirname "$SRC_DIR")"
  GIT_TERMINAL_PROMPT=0 git clone --depth 1 \
    https://github.com/chenmozhijin/BSRoformer.cpp "$SRC_DIR" \
    || die "clone failed; clone it yourself and pass --src PATH"
fi
if [[ ! -d "$GGML_DIR" ]]; then
  # BSRoformer.cpp does not vendor ggml; it resolves a sibling directory.
  info "cloning ggml beside it"
  GIT_TERMINAL_PROMPT=0 git clone --depth 1 \
    https://github.com/ggerganov/ggml "$GGML_DIR" \
    || die "could not clone ggml"
fi
SRC_REV="$(git -C "$SRC_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GGML_REV="$(git -C "$GGML_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
info "BSRoformer.cpp $SRC_REV   ggml $GGML_REV"

# The backend is chosen at build time, not by a CLI flag, so each one needs
# its own build tree. GGML_CUDA defaults ON upstream, which fails or silently
# misbuilds on a machine without CUDA - turn it off explicitly every time.
build_backend() {
  # Separate statements: within one `local`, bash expands every word before
  # applying any assignment, so `local a=$1 b=$a` reads an unset `a` and dies
  # under `set -u`.
  local backend="$1"
  local dir="$SRC_DIR/build-$backend"
  local flags=(-DGGML_CUDA=OFF)
  case "$backend" in
    cpu)    ;;
    vulkan)
      local vk_missing=()
      echo '#include <vulkan/vulkan.h>' | cc -E - >/dev/null 2>&1 || vk_missing+=("Vulkan headers")
      command -v glslc >/dev/null 2>&1 || vk_missing+=("glslc (shader compiler)")
      # ggml-vulkan does find_package(SPIRV-Headers CONFIG REQUIRED).
      [[ -n "$(find /usr/share /usr/lib /usr/local -maxdepth 4 -iname "SPIRV-HeadersConfig*.cmake" 2>/dev/null | head -1)" ]] \
        || vk_missing+=("SPIRV-Headers")
      if (( ${#vk_missing[@]} )); then
        warn "the vulkan backend needs build-time packages this machine lacks:
           ${vk_missing[*]}
         A working driver is not enough - ggml compiles its own shaders.
           Debian/Ubuntu  sudo apt install libvulkan-dev glslc spirv-headers
           Fedora         sudo dnf install vulkan-headers glslc spirv-headers
           Arch           sudo pacman -S vulkan-headers shaderc spirv-headers"
        return 1
      fi
      flags+=(-DGGML_VULKAN=ON) ;;
    sycl)   flags+=(-DGGML_SYCL=ON)
            command -v icpx >/dev/null \
              || warn "sycl needs oneAPI's icpx on PATH - source setvars.sh first" ;;
    *)      warn "unknown backend '$backend'"; return 1 ;;
  esac
  if (( SKIP_BUILD )) && [[ -n "$(binary_for "$backend")" ]]; then
    info "reusing existing $backend build"
    return 0
  fi
  info "building $backend"
  cmake -B "$dir" -S "$SRC_DIR" -DCMAKE_BUILD_TYPE=Release "${flags[@]}" \
      > "$OUT_DIR/build-$backend.log" 2>&1 \
    && cmake --build "$dir" -j "$(nproc)" >> "$OUT_DIR/build-$backend.log" 2>&1 \
    || { warn "$backend build failed - see $OUT_DIR/build-$backend.log"; tail -20 "$OUT_DIR/build-$backend.log" >&2; return 1; }
}

binary_for() {
  local dir="$SRC_DIR/build-$1"
  # The CMake target is bs_roformer-cli. Look for that first, then fall back
  # to anything roformer-shaped so an upstream rename degrades to a warning
  # rather than a silent skip.
  local found
  found="$(find "$dir" -maxdepth 3 -type f -name "bs_roformer-cli" -perm -u+x 2>/dev/null | head -1)"
  [[ -n "$found" ]] || found="$(find "$dir" -maxdepth 3 -type f -iname "*roformer*" -perm -u+x 2>/dev/null | head -1)"
  printf '%s' "$found"
}

mkdir -p "$OUT_DIR"
AVAILABLE=()
IFS=',' read -ra WANTED_BACKENDS <<< "$BACKENDS"
for b in "${WANTED_BACKENDS[@]}"; do
  if ! build_backend "$b"; then
    warn "skipping backend '$b': it did not build"
    continue
  fi
  bin="$(binary_for "$b")"
  if [[ -z "$bin" ]]; then
    warn "backend '$b' built but no executable was found under
         $SRC_DIR/build-$b
         Expected the CMake target bs_roformer-cli. What is actually there:"
    find "$SRC_DIR/build-$b" -maxdepth 3 -type f -perm -u+x 2>/dev/null \
      | head -10 | sed 's/^/           /' >&2
    continue
  fi
  info "$b -> $bin"
  AVAILABLE+=("$b")
done
(( ${#AVAILABLE[@]} )) || die "no backend built successfully"

# ---------------------------------------------------------------- convert

step "Convert weights"

MODELS_DIR="$OUT_DIR/gguf"; mkdir -p "$MODELS_DIR"
CONVERTED=()
IFS=',' read -ra WANTED_DTYPES <<< "$DTYPES"
for dt in "${WANTED_DTYPES[@]}"; do
  gguf="$MODELS_DIR/model-$dt.gguf"
  if [[ -f "$gguf" ]]; then
    info "$dt already converted"; CONVERTED+=("$dt"); continue
  fi
  info "converting $dt"
  # --arch bs is explicit: auto-detection exists, but this checkpoint is
  # BS-RoFormer and a mis-detected architecture fails confusingly later.
  if "$PYTHON" "$SRC_DIR/scripts/convert_to_gguf.py" \
        --ckpt "$CKPT" --config "$CONFIG" --out "$gguf" \
        --dtype "$dt" --arch bs > "$OUT_DIR/convert-$dt.log" 2>&1; then
    CONVERTED+=("$dt")
    info "  $(du -h "$gguf" | cut -f1)"
  else
    warn "conversion to $dt failed - see $OUT_DIR/convert-$dt.log"
    tail -15 "$OUT_DIR/convert-$dt.log" >&2
  fi
done
(( ${#CONVERTED[@]} )) || die "no weights converted"

# -------------------------------------------------------------------- run

now() { date +%s.%N; }
elapsed() { "$PYTHON" -c 'import sys; print(f"{float(sys.argv[2])-float(sys.argv[1]):.2f}")' "$1" "$2"; }

step "Reference: StemLab BS-RoFormer (PyTorch)"

PY_DIR="$OUT_DIR/python"
rm -rf "$PY_DIR" "$OUT_DIR/_pyin"; mkdir -p "$PY_DIR" "$OUT_DIR/_pyin"
cp "$INPUT" "$OUT_DIR/_pyin/input.wav"

PY_DEVICE="$("$PYTHON" -c "
try:
    import torch
    print('cuda' if torch.cuda.is_available() else 'cpu')
except Exception:
    print('cpu')" 2>/dev/null || echo cpu)"
info "device: $PY_DEVICE"

t0="$(now)"
PYTHONPATH="$REPO_ROOT/src" "$PYTHON" -m stemlab.bs_roformer_cli \
    --input_folder "$OUT_DIR/_pyin" --store_dir "$PY_DIR" \
    --device "$PY_DEVICE" --model "$MODEL" \
    > "$OUT_DIR/python.log" 2>&1
PY_RC=$?
t1="$(now)"
if (( PY_RC != 0 )); then
  tail -20 "$OUT_DIR/python.log" >&2
  die "the PyTorch reference failed; there is nothing to compare against"
fi
PY_SECONDS="$(elapsed "$t0" "$t1")"
info "took ${PY_SECONDS}s"

# StemLab's own resolver knows how each backend names its stems, so use it
# rather than guessing at filename conventions here.
normalise_python_stems() {
  PYTHONPATH="$REPO_ROOT/src" "$PYTHON" - "$PY_DIR" "$OUT_DIR/reference" "${STEMS[@]}" <<'PY'
import shutil, sys, pathlib
from stemlab.audio import find_stem_file
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
dst.mkdir(parents=True, exist_ok=True)
missing = []
for stem in sys.argv[3:]:
    found = find_stem_file(src, stem)
    if found is None:
        missing.append(stem)
        continue
    shutil.copy2(found, dst / f"{stem}.wav")
if missing:
    print("missing from the PyTorch run: " + ", ".join(missing), file=sys.stderr)
PY
}
normalise_python_stems

step "Candidates: BSRoformer.cpp"

RESULTS=()
for backend in "${AVAILABLE[@]}"; do
  BIN="$(binary_for "$backend")"
  for dt in "${CONVERTED[@]}"; do
    label="$backend-$dt"
    run_dir="$OUT_DIR/$label"
    times=()
    ok=1
    for (( run = 1; run <= REPEAT; run++ )); do
      rm -rf "$run_dir"; mkdir -p "$run_dir"
      info "running $label (run $run/$REPEAT)"
      t0="$(now)"
      timeout "$RUN_TIMEOUT" "$BIN" "$MODELS_DIR/model-$dt.gguf" \
          "$INPUT" "$run_dir/out.wav" > "$OUT_DIR/$label.log" 2>&1
      rc=$?
      t1="$(now)"
      if (( rc == 124 )); then
        warn "$label exceeded ${RUN_TIMEOUT}s and was killed. On an integrated
         GPU this usually means the device wedged rather than that the work is
         slow; check dmesg for a driver reset."
        ok=0; break
      elif (( rc != 0 )); then
        warn "$label failed (exit $rc) - see $OUT_DIR/$label.log"
        tail -15 "$OUT_DIR/$label.log" >&2
        ok=0; break
      fi
      secs="$(elapsed "$t0" "$t1")"
      times+=("$secs")
      info "  ${secs}s"
    done
    (( ok )) || { RESULTS+=("$label|FAILED||"); continue; }

    # The CLI writes out_stem_0.wav .. out_stem_N.wav in the config's
    # instrument order; give them names the harness can match.
    for i in "${!STEMS[@]}"; do
      src="$run_dir/out_stem_$i.wav"
      [[ -f "$src" ]] && mv "$src" "$run_dir/${STEMS[$i]}.wav"
    done
    rm -f "$run_dir/out.wav"

    steady="${times[-1]}"
    RESULTS+=("$label|$steady|$run_dir|$(IFS=,; echo "${times[*]}")")
  done
done

# ---------------------------------------------------------------- compare

step "Parity against PyTorch"

compare_pair() {  # reference_dir, candidate_dir, json_name, heading
  echo
  info "$4"
  PYTHONPATH="$REPO_ROOT/src" "$PYTHON" -m stemlab.regression \
    --reference "$1" --candidate "$2" \
    --min-si-sdr 15 --min-correlation 0.98 \
    --json "$OUT_DIR/parity-$3.json" || true
}

for entry in "${RESULTS[@]}"; do
  IFS='|' read -r label steady dir _ <<< "$entry"
  [[ "$steady" == "FAILED" || -z "$dir" ]] && continue
  compare_pair "$OUT_DIR/reference" "$dir" "$label" "$label vs PyTorch"
done

# Quantization damage, isolated. Scoring q8_0 only against PyTorch conflates
# two independent errors: whatever the port gets wrong, and whatever the
# quantization costs. Comparing each quantized run against the C++ fp32 run on
# the same backend answers the second question on its own.
FP32_DIR=""
for entry in "${RESULTS[@]}"; do
  IFS='|' read -r label steady dir _ <<< "$entry"
  [[ "$label" == *"-fp32" && "$steady" != "FAILED" ]] && { FP32_DIR="$dir"; break; }
done
if [[ -n "$FP32_DIR" ]]; then
  step "Quantization cost, measured against the C++ fp32 run"
  for entry in "${RESULTS[@]}"; do
    IFS='|' read -r label steady dir _ <<< "$entry"
    [[ "$steady" == "FAILED" || -z "$dir" || "$label" == *"-fp32" ]] && continue
    compare_pair "$FP32_DIR" "$dir" "quant-$label" "$label vs C++ fp32"
  done
fi

# ---------------------------------------------------------------- summary

step "Summary"
printf '  %-22s %10s  %s\n' "configuration" "seconds" "vs PyTorch"
printf '  %-22s %10s  %s\n' "python ($PY_DEVICE)" "$PY_SECONDS" "reference"
for entry in "${RESULTS[@]}"; do
  IFS='|' read -r label steady _ _ <<< "$entry"
  if [[ "$steady" == "FAILED" ]]; then
    printf '  %-22s %10s  %s\n' "$label" "-" "did not complete"
  else
    ratio="$("$PYTHON" -c "print(f'{float('$PY_SECONDS')/float('$steady'):.2f}x')" 2>/dev/null || echo "?")"
    printf '  %-22s %10s  %s\n' "$label" "$steady" "$ratio"
  fi
done

"$PYTHON" - "$OUT_DIR" "$PY_SECONDS" "$PY_DEVICE" "$SRC_REV" "$GGML_REV" \
           "$GPU_NAME" "$INPUT" "${RESULTS[@]}" <<'PY'
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
doc = {
    "python_seconds": float(sys.argv[2]),
    "python_device": sys.argv[3],
    "bsroformer_cpp_revision": sys.argv[4],
    "ggml_revision": sys.argv[5],
    "vulkan_adapter": sys.argv[6],
    "input": sys.argv[7],
    "runs": [],
}
for entry in sys.argv[8:]:
    label, steady, _dir, all_runs = (entry.split("|") + ["", "", ""])[:4]
    doc["runs"].append({
        "configuration": label,
        "seconds": None if steady in ("FAILED", "") else float(steady),
        "all_run_seconds": [float(x) for x in all_runs.split(",") if x],
        "completed": steady not in ("FAILED", ""),
    })
(out / "results.json").write_text(json.dumps(doc, indent=2))
print(f"\nwrote {out / 'results.json'}")
PY

echo
info "outputs in $OUT_DIR"
cat <<'NOTE'

  Reading this:
    - The PyTorch row is the bar to beat. On a machine with no CUDA it is a
      CPU number, which is the honest comparison for anyone without an
      NVIDIA card.
    - "vs PyTorch" above 1.00x means BSRoformer.cpp is faster.
    - A parity table that fails on a stem carrying only bleed is not a real
      failure; check the stem's level before believing it.
    - The quantization section is the one that decides this. If q8_0 loses
      real SI-SDR against C++ fp32, the speed it buys is not free, and the
      fp32 timing is the only one worth comparing against PyTorch.

  Two things measured while writing this, so you do not rediscover them:
    - q8_0 quantizes 881 of 1939 tensors, which sounds like partial coverage
      and is not. Counted by PARAMETER rather than by tensor it is 99.5%:
      only 0.82M weights of 174.6M escape. The count is dominated by hundreds
      of tiny per-band layers, while the transformer's large matrices - the
      mass of the model - all quantize. The band_split layers do stay at F32,
      at shapes like (256, 16) and (256, 48), so the frequency mapping is
      protected; that is worth having but it is half a percent of the weights,
      not a general reprieve. Assume the whole network runs on 8-bit weights.
    - The converted q8_0 file is 179 MB against a ~700 MB checkpoint, which is
      itself the evidence: near-complete quantization by volume.

  On Intel graphics, try --backends cpu,sycl as well. Vulkan works, but SYCL
  is Intel's first-class path and generally faster on their hardware.
NOTE
