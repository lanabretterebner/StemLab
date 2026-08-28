<!--
Track D of the engine-cleanup work order: three measurements nobody has.

This file is the measurement PLAN and the evidence for why the runs did not
happen here, not a set of results. No number in it was produced by one of the
three runs - those need real music, which this machine does not have and
cannot license. The decision rule in section 0 is stated before any data
exists on purpose, so the threshold cannot be chosen after seeing the answer.
-->

# Track D — Engine cleanup: the three measurements, and what it takes to run them

**Status: no measurement of the three was produced.** The blocker is not what the work order assumed, and the correction matters — two of the three are much closer to runnable than the brief states. Everything below was verified on this machine; every command was checked against the real code, and the parts I could execute, I executed.

---

## 0. Decision rule for `DEMUCS_OVERLAP`, fixed before any data exists

Stated first, deliberately, so the threshold cannot be reverse-engineered from the result.

**Endpoint.** For each real track and each stem with ground truth, take from the harness's `truth_si_sdr_db` block:

```
Δ = SI-SDR(overlap 0.25 vs truth) − SI-SDR(overlap 0.10 vs truth)     [dB, higher = 0.25 better]
```

Both arms run with `--shifts 0`, so each arm is deterministic and Δ contains no shift noise. The dispersion that matters is across (track, stem) pairs, not across repeats.

**Revert `DEMUCS_OVERLAP` to 0.25 only if all three hold:**
1. median Δ ≥ **+0.5 dB**, over ≥ 10 real tracks × their truth-backed stems;
2. Δ > 0 on ≥ **80 %** of (track, stem) pairs — a mean carried by two tracks is not a quality finding;
3. the measured median wall clock at 0.25 is ≤ **1.25×** that at 0.10.

**Keep 0.10 if** median Δ < +0.2 dB, whatever the timing says. Below that the change is inaudible and the burden of proof is on the revert, since 0.10 is what ships.

**Ambiguous band, 0.2 dB ≤ median Δ < 0.5 dB:** revert only if the measured time penalty is under 10 %. Otherwise keep 0.10 and record the number in the source comment.

**Separate trip-wire, independent of the above.** Run the harness a second time with `--reference` = the 0.25 stems and `--candidate` = the 0.10 stems (no `--truth`). If any stem shows a window-seam artifact — a `log_spectral_distance_db` outlier well outside the spread of the other stems, or a non-zero `lag_samples` — revert regardless of the SI-SDR average. Seams are a localised defect that a whole-track SI-SDR average will not show.

**What will *not* justify a revert:** a win on one track; a win on the synthetic corpus (`src/stemlab/regression/corpus.py:7-11` disclaims itself as a quality benchmark); or any difference in the agreement metrics (correlation, SI-SDR against a *reference* rather than truth). Agreement measures difference, not direction — `compare.py:8-10` says so itself.

The same rule shape applies to the other two, with the arms renamed: 48 kHz-path vs 44.1 kHz-native, and `num_overlap` 1 vs 2.

---

## 1. What is actually missing — the brief is wrong on the main point

I checked each claim rather than taking it on trust. **`torch`, `demucs` and `bs_roformer` are all installed, and the Engine is built.** They are not in the system interpreter, which is what a naive check sees:

```
$ python3 -c "import torch"
ModuleNotFoundError: No module named 'torch'
$ python3 -c "import demucs"
ModuleNotFoundError: No module named 'demucs'
$ python3 -c "import bs_roformer"
ModuleNotFoundError: No module named 'bs_roformer'
```

They live in the StemLab Engine virtualenv, which was built on this machine today (`.stemlab-engine-ready`, mtime 2026-08-28 04:55):

```
$ /root/.local/share/StemLab/Engine/bin/python -c "import torch; print(torch.__version__, torch.cuda.is_available())"
torch 2.11.0+cpu cuda False

$ /root/.local/share/StemLab/Engine/bin/python -c "import importlib.metadata as m
for k in ('torch','demucs','bs_roformer_infer','audio_separator'): print(k, m.version(k))"
torch              2.11.0+cpu
demucs             4.1.0
bs_roformer_infer  0.1.5
audio_separator    0.44.5
```

So the real inventory:

| Item | State | How confirmed |
|---|---|---|
| torch / demucs / bs_roformer | **Present** in `/root/.local/share/StemLab/Engine` | commands above |
| GPU | **Absent** — CPU-only torch build | `torch.cuda.is_available()` → `False` |
| `htdemucs_6s` weights (`5c90dfd2-34c22ccb.th`) | **Absent locally, fetchable** | `find / -name '*.th'` → nothing; only `models--adefossez--HTDemucs/.../955717e8.safetensors` is cached, which `demucs/remote/htdemucs.yaml` identifies as 4-stem `htdemucs`, not the 6-stem model. `curl -I https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/5c90dfd2-34c22ccb.th` → `HTTP/2 200, content-length: 54996327` |
| BS-RoFormer weights (`BS-Rofo-SW-Fixed.ckpt`) | **Absent locally, fetchable** | `ls /root/.cache/bs-roformer-infer` → *No such file or directory*. The registry's constructed URL 404s (the jarredou account was deleted), but `data/overrides.json` repoints it: `curl -IL https://huggingface.co/enerjazzer/BS-ROFO-SW-Fixed/resolve/main/BS-Rofo-SW-Fixed.ckpt` → `200`, `x-linked-size: 699412152` — matching the 699412152 recorded in `bs_roformer/data/checksums.json` |
| **Real music** | **Absent, and not fetchable from here** | `find /home /root -iname '*.wav' -o -iname '*.mp3' -o -iname '*.flac'` → nothing outside the Engine's own package data |
| ffmpeg, pwsh | Absent | `command -v ffmpeg` / `pwsh` → not found |
| numpy 2.4.6, scipy 1.17.1, soundfile 0.14.0, soxr 1.1.0, sklearn 1.9.0 | Present in system python | `pip list` |

**The one genuine blocker is real music.** The weights are two `curl`s away; the material is not. Nothing in this container is licensed music, and nothing that is *not* music will answer the question — `corpus.py:7-11` is explicit that its synthetic material "says almost nothing about how it will sound on actual music."

**What the real music has to be.** Not just any track. All three measurements ask *which is better*, and only `--truth` can answer that (`compare.py:8-10`). So the corpus must be real music **with ground-truth stems** — the MUSDB18-HQ test split is the obvious choice: 50 real tracks, native 44.1 kHz (which measurement 1 needs as its baseline rate), stems included. Its truth covers `vocals`, `drums`, `bass`, `other`; `guitar` and `piano` come out of `htdemucs_6s` with no truth to score against, and `compare.py:138-149` will simply omit them from the truth table. Say so in the write-up rather than letting four stems stand in for six.

---

## 2. The knobs, and exactly where they live

Each read, not assumed.

**Measurement 1 — RoFormer input sample rate.**
- On `main` (HEAD `aa6d7ac`): **there is no knob.** `git show HEAD:src/stemlab/pretrained.py | grep ROFORMER_SAMPLE_RATE` returns nothing. `RoFormerBackend.separate` stages the input and hands it to the CLI at whatever rate it arrived, and `bs_roformer/inference.py:98` reads it with `sf.read` and never resamples. A 48 kHz session is fed to the model at 48 kHz.
- **In the working tree right now it is already fixed, uncommitted, by another agent.** `src/stemlab/pretrained.py:21` `ROFORMER_SAMPLE_RATE = 44100`, resample at `:361-389`, stems restored to the source rate at `:433-435`. `git diff --stat` shows +177 lines in that file, unstaged.
- **Consequence for the measurement:** run it against HEAD's behaviour, or the two arms collapse into a comparison of soxr against itself. The clean way is to bypass StemLab entirely and drive `bs_roformer.inference` directly, which never resamples in either version — see §5.

**Measurement 2 — Demucs overlap.**
- `src/stemlab/demucs_backend.py:26` — `DEMUCS_OVERLAP = 0.10`; comment at `:22-25`.
- Consumed at `src/stemlab/demucs_backend.py:147-148`, `"--overlap", str(DEMUCS_OVERLAP)`, inside the command built at `:138-152`.
- Upstream default `0.25`, confirmed by resolving the parser rather than reading docs: `demucs.separate.get_parser().parse_args(['x.wav'])` → `overlap = 0.25`, `shifts = 1`.
- Mechanism, `demucs/apply.py:265`: `stride = int((1 - overlap) * segment_length)`, then `offsets = range(0, length, stride)` at `:266`. Chunk count is therefore proportional to `1/(1 − overlap)`.

  **Arithmetic, not a measurement:** `1/0.75 = 1.3333` against `1/0.90 = 1.1111` — 0.25 costs exactly **1.20×** the forward passes of 0.10; equivalently 0.10 saves 16.67 % of the 0.25 cost. The comment at `demucs_backend.py:22-25` says "~17 % extra", which is the saving expressed against the wrong baseline; commit `c818817`'s message says "a third more", which is 0.25 measured against *no* overlap, not against 0.10. Neither statement is the +20 % the code implies, and neither was ever a timing measurement. Wall clock will not match 1.20× exactly — model load, I/O and tail padding are fixed costs — which is precisely why the measurement is being asked for.

**Measurement 3 — RoFormer `num_overlap`.**
- **It is not in this repository.** `grep -rn "num_overlap"` across `src/`, `scripts/`, `tests/`, docs and build files returns nothing.
- It lives in the model config YAML: `/root/.local/share/StemLab/Engine/lib/python3.11/site-packages/bs_roformer/configs/BS-Rofo-SW-Fixed.yaml:34` → `num_overlap: 2`.
- Consumed at `bs_roformer/utils.py:69-71`: `N = config.inference.num_overlap`; `step = C // N` with `C = config.inference.chunk_size` (588800). `num_overlap: 1` is one chunk-length step with no overlap-add; `2` is half-chunk steps, i.e. **2× the chunks** (arithmetic, not measured).
- The config that gets used at runtime is a **copy of that packaged file**: `download.py:310` calls `_copy_packaged_config` *before* falling through to the override URL at `:312`, so the resolved config at `~/.cache/bs-roformer-infer/roformer-model-bs-roformer-sw-by-jarredou/BS-Rofo-SW-Fixed.yaml` is byte-identical to the packaged 686-byte file, `num_overlap` on line 34. Verify with `grep -n num_overlap` on the resolved copy before editing rather than trusting the line number.

---

## 3. Two traps that will silently invalidate the run

**`--shifts` is never passed by StemLab.** `grep -rn shifts src/ scripts/ tests/` finds one unrelated word in a docstring. `demucs_backend.py:138-152` builds `--name / --device / --overlap / --out` and nothing else, so the shipped Demucs path inherits `shifts = 1`. That is `demucs/apply.py:245` — `offset = random.randint(0, max_shift)` with `max_shift = int(0.5 * model.samplerate)` at `:237`, and `grep -n "random\.\|manual_seed\|seed("` over `apply.py` and `separate.py` finds that one line and no seeding anywhere. **An unseeded offset of up to 22 050 samples, redrawn every run.** This is the fictitious result that cost a day, and it is still live in the shipped backend.

Therefore: **measurement 2 cannot be run through `stemlab-separate` as it stands.** Either call `demucs.separate` directly with `--shifts 0` (what §6 does), or land `"--shifts", "0"` into the command list at `demucs_backend.py:138-152` first. The second is worth doing on its own merits — the shipped path is non-reproducible today — but it is a source change and belongs to whoever owns that file.

**Measurement 3 cannot be run through StemLab either.** `pretrained.py:391-403` passes `--input_folder / --store_dir / --device / --model` and never `--config_path`, and `bs_roformer/inference.py:_resolve_model_assets` rejects `--model` combined with explicit paths ("--model selects a registry model to auto-resolve; it cannot be combined with explicit --model_path/--config_path") and requires `--model_path` and `--config_path` **together**. So `num_overlap` is varied either by editing the resolved config in place, or by passing a `(checkpoint, edited-config)` pair directly to the CLI. §7 does the latter — it leaves the cache untouched and makes the varied knob visible in the command line.

---

## 4. Setup, common to all three

```bash
REPO=/home/user/StemLab
PY=/root/.local/share/StemLab/Engine/bin/python   # the only interpreter here with torch
WORK=/var/tmp/trackd                              # NOT inside $REPO
export PYTHONPATH=$REPO/src                       # see note below
mkdir -p "$WORK"/{music,out,reports}
```

**Why `PYTHONPATH`.** There are two StemLab installs. The system `python3` has an editable install pointing at `$REPO` but no torch. The Engine venv has a *copied* `stemlab` at `Engine/lib/python3.11/site-packages/stemlab`, and that copy is a snapshot of the **dirty** working tree — its `pretrained.py` already contains the uncommitted `ROFORMER_SAMPLE_RATE`, and it will drift the instant anyone edits `src/`. `PYTHONPATH=$REPO/src` makes the Engine interpreter run the live tree; confirmed:

```
$ PYTHONPATH=/home/user/StemLab/src $PY -c "import stemlab; print(stemlab.__file__)"
OK /home/user/StemLab/src/stemlab/__init__.py
```

**Fetch the weights** (about 754 MB total; both URLs verified reachable above):

```bash
# htdemucs_6s — downloads to the torch hub cache on first use
$PY -c "from demucs.pretrained import get_model; get_model('htdemucs_6s')"

# BS-RoFormer — sha256-verified against bs_roformer/data/checksums.json
$PY -m bs_roformer.download --model roformer-model-bs-roformer-sw-by-jarredou
```

**Put real music in `$WORK/music/`**, one 44.1 kHz stereo WAV per track, with truth stems in `$WORK/truth/<track>/{vocals,drums,bass,other}.wav`.

**The harness invocation**, verified by running it (§8):

```bash
PYTHONPATH=$REPO/src python3 -m stemlab.regression \
    --reference <dir> --candidate <dir> [--truth <dir>] [--json report.json]
```

`python -m stemlab.regression` resolves through `regression/__main__.py:12` to `compare.main`. `--help` confirms the flag set is exactly `--reference --candidate --truth --json --min-correlation --min-si-sdr --max-lag`; there is no stem filter and no timing — **time the separation externally**. System `python3` is fine for the harness (numpy/scipy/soundfile only); the Engine interpreter is only needed for the separations.

**Expect exit code 1 and do not treat it as an error.** The gates default to correlation ≥ 0.99, SI-SDR ≥ 20 dB, lag = 0 (`compare.py:92-94`, `metrics.py:288-292`). Two different configurations of a separator will not clear those, and are not meant to. The numbers in the table are the deliverable; the exit code is for CI.

---

## 5. Measurement 1 — what the 48 kHz mismatch actually costs

Driven through `bs_roformer.inference` directly, because neither StemLab version can express both arms: HEAD has no resample, the working tree always resamples.

```bash
T=track01
mkdir -p "$WORK/m1"/{in441,in48,out441,out48,out48_back}

# Arm A — native 44.1 kHz, the baseline
cp "$WORK/music/$T.wav" "$WORK/m1/in441/$T.wav"
$PY -m bs_roformer.inference \
    --model roformer-model-bs-roformer-sw-by-jarredou \
    --input_folder "$WORK/m1/in441" --store_dir "$WORK/m1/out441" --device cpu

# Arm B — upsample to 48 kHz, separate at 48 kHz (the shipped-on-main mismatch)
python3 "$WORK/resample.py" "$WORK/music/$T.wav" "$WORK/m1/in48/$T.wav" 48000
$PY -m bs_roformer.inference \
    --model roformer-model-bs-roformer-sw-by-jarredou \
    --input_folder "$WORK/m1/in48" --store_dir "$WORK/m1/out48" --device cpu

# Bring arm B's stems back to 44.1 kHz at the baseline's exact frame count
FRAMES=$(python3 -c "import soundfile as sf; print(sf.info('$WORK/music/$T.wav').frames)")
for f in "$WORK/m1/out48"/*.wav; do
    python3 "$WORK/resample.py" "$f" "$WORK/m1/out48_back/$(basename "$f")" 44100 "$FRAMES"
done

# Strip the "{track}_" prefix the CLI writes, as pretrained.py:427 does downstream
for d in "$WORK/m1/out441" "$WORK/m1/out48_back"; do
    for f in "$d/${T}_"*.wav; do mv "$f" "$d/$(basename "${f#"$d/${T}_"}")"; done
done

# Score
PYTHONPATH=$REPO/src python3 -m stemlab.regression \
    --reference "$WORK/m1/out441" --candidate "$WORK/m1/out48_back" \
    --truth "$WORK/truth/$T" --json "$WORK/reports/m1_$T.json"
```

**Reads the answer off** the `truth_si_sdr_db` block: `reference` is the 44.1 kHz arm, `candidate` is the 48 kHz arm, and `delta` (negative = the mismatch costs quality) is the cost, per stem, in dB.

**Notes.**
- No `--shifts` equivalent exists here — `bs_roformer/utils.py:demix_track` has no random component, so both arms are deterministic as they stand.
- The CLI also emits `{track}_instrumental.wav` (`inference.py:120-128`) = mix minus the first stem. The harness reports it under `unexpected` and does not fail on it (`compare.py:103-106`).
- The downsample back is mandatory: `compare.py:118-124` refuses a cross-rate comparison outright rather than scoring a resampling bug as a quality difference.
- **Run the control arm too** (§8): resample the *mixture* 44.1 → 48 → 44.1 without separating, and score it against the original. Without it you cannot tell the band-edge mismatch from the two soxr passes.

`$WORK/resample.py` is the helper in §8, tested here.

---

## 6. Measurement 2 — `DEMUCS_OVERLAP` 0.10 vs 0.25

```bash
T=track01
for OV in 0.10 0.25; do
  TAG=${OV/./}
  /usr/bin/time -v -o "$WORK/reports/m2_${T}_$TAG.time" \
  $PY -m demucs.separate \
      -n htdemucs_6s \
      --device cpu \
      --shifts 0 \
      --overlap "$OV" \
      --filename "{stem}.{ext}" \
      --float32 --clip-mode none \
      -o "$WORK/m2/$TAG" \
      "$WORK/music/$T.wav"
done

# Quality, each arm against ground truth
for TAG in 010 025; do
  PYTHONPATH=$REPO/src python3 -m stemlab.regression \
      --reference "$WORK/m2/025/htdemucs_6s" \
      --candidate "$WORK/m2/$TAG/htdemucs_6s" \
      --truth "$WORK/truth/$T" \
      --json "$WORK/reports/m2_${T}_$TAG.json"
done
```

**Every flag verified against `demucs/separate.py`:**
- `--shifts 0` — kills `apply.py:236-254` entirely; without it you get one unseeded random offset per run.
- `--filename "{stem}.{ext}"` — the default is `"{track}/{stem}.{ext}"` (`separate.py:158`, `:187`), which nests stems one directory deeper. `compare.discover_stems` uses `directory.iterdir()` (`compare.py:47`) and is **not recursive**, so the default layout yields an empty comparison. Stems land flat in `$WORK/m2/<tag>/htdemucs_6s/`.
- `--float32` — the default is 16-bit PCM (`separate.py:183`, `bits_per_sample = 24 if args.int24 else 16`). A 16-bit floor would cap the agreement metrics for both arms at the quantisation noise.
- `--clip-mode none` — the default is `rescale`, and `audio.save_audio:310` calls `prevent_clip` *unconditionally*, before the float32 branch. A per-arm rescale gain would leave SI-SDR and correlation untouched (both scale-invariant) but would corrupt the log-spectral distance.
- `-o` gives `{out}/{name}` (`separate.py:158`), hence the `htdemucs_6s` component in the comparison paths.

**Timing discipline.** Warm up once and discard it — the first run pays the download and load. Then run each arm at least 3× alternating, and take the **median** wall clock. Report `Elapsed (wall clock)` and `Maximum resident set size` from the `-v` output. Do not run the two arms concurrently, and do not report a single-run ratio.

Note the second harness call (`TAG=025`) compares the 0.25 arm against itself, which is the sanity check that the pipeline is deterministic: with `--shifts 0` it must come back correlation 1.0 / SI-SDR inf across all six stems. If it does not, something upstream is still random and every other number is void.

---

## 7. Measurement 3 — RoFormer `num_overlap` 2 vs 1

```bash
T=track01
MD=~/.cache/bs-roformer-infer/roformer-model-bs-roformer-sw-by-jarredou
CKPT="$MD/BS-Rofo-SW-Fixed.ckpt"

# Two configs that differ in exactly one line. Verify the line first.
grep -n num_overlap "$MD/BS-Rofo-SW-Fixed.yaml"          # expect: 34:  num_overlap: 2
mkdir -p "$WORK/m3/cfg"
sed 's/^\(  num_overlap:\).*/\1 2/' "$MD/BS-Rofo-SW-Fixed.yaml" > "$WORK/m3/cfg/n2.yaml"
sed 's/^\(  num_overlap:\).*/\1 1/' "$MD/BS-Rofo-SW-Fixed.yaml" > "$WORK/m3/cfg/n1.yaml"
diff "$WORK/m3/cfg/n2.yaml" "$WORK/m3/cfg/n1.yaml"       # must be exactly one line

mkdir -p "$WORK/m3/in"; cp "$WORK/music/$T.wav" "$WORK/m3/in/$T.wav"
for N in 1 2; do
  /usr/bin/time -v -o "$WORK/reports/m3_${T}_n$N.time" \
  $PY -m bs_roformer.inference \
      --model_path "$CKPT" \
      --config_path "$WORK/m3/cfg/n$N.yaml" \
      --input_folder "$WORK/m3/in" --store_dir "$WORK/m3/out_n$N" --device cpu
  for f in "$WORK/m3/out_n$N/${T}_"*.wav; do
      mv "$f" "$WORK/m3/out_n$N/$(basename "${f#"$WORK/m3/out_n$N/${T}_"}")"
  done
done

PYTHONPATH=$REPO/src python3 -m stemlab.regression \
    --reference "$WORK/m3/out_n2" --candidate "$WORK/m3/out_n1" \
    --truth "$WORK/truth/$T" --json "$WORK/reports/m3_$T.json"
```

`--model_path` and `--config_path` are passed together and `--model` is omitted — required by `inference.py:_resolve_model_assets`, which errors on either violation. Editing copies rather than the cached config keeps the models directory pristine and puts the varied knob in the command line where the log will record it.

Same timing discipline as §6. The expectation from `utils.py:69-71` is that `num_overlap: 1` roughly halves the chunk count; whether it costs anything audible is the open question.

---

## 8. What I did measure here

Four things, all real, none of them one of the three measurements.

**The harness works.**
```
$ cd /home/user/StemLab && python3 -m pytest tests/test_regression_harness.py -q
14 passed in 3.02s
```

**The exact invocation in §4 runs and is correct.** Built a synthetic corpus into scratch, compared it against itself:
```
$ PYTHONPATH=src python3 -m stemlab.regression --reference <stems> --candidate <stems> --truth <stems>
bass  1.000000  inf  0.00  0  3.148e-01      ...  RESULT: PASS   (exit 0)
```
Six stems, correlation 1.0, SI-SDR inf. This validates the module path, the flag names and the pass/exit semantics — nothing about separation quality.

**The resampler control, on synthetic material.** 44.1 kHz → 48 kHz → 44.1 kHz, soxr `HQ`, frame count pinned, no separation in between; scored through the harness. On the 4.8 s corpus mixture (211 680 frames, stereo, float):

| corr | SI-SDR dB | LSD dB | lag |
|---|---|---|---|
| 0.999935 | 38.85 | 10.76 | 0 |

The 10.76 dB LSD is not 10 dB of audible error. Broken out by band on the same STFT:

| band | LSD | mean reference magnitude |
|---|---|---|
| 0–5 kHz | **0.01 dB** | 3.117e-03 |
| 5–16 kHz | 4.60 dB | 1.762e-04 |
| 16–22.05 kHz | 19.35 dB | 7.754e-05 |

All of it is the resampler's stopband in bins carrying ~1/40th the magnitude of the band where the signal lives. **This is on synthetic material and one short clip — re-run it on the real track before quoting it.** Its purpose is to give measurement 1 its floor: the round trip alone scores about 39 dB SI-SDR and clears the harness's 20 dB gate comfortably, so if the 48 kHz arm lands far below that, the loss belongs to the model's band edges and not to soxr.

**`$WORK/resample.py` — tested, not sketched.** Round-tripped 211 680 → 230 400 → 211 680 frames with the count preserved exactly.

```python
"""Resample one WAV with soxr HQ, forcing an exact output frame count.

Mirrors what src/stemlab/pretrained.py does on the RoFormer path: soxr
ResampleStream, quality="HQ", block-wise, output length pinned by the caller
rather than trusted, because soxr's length is not a stable function of input
length and a stem one sample off drifts against every other track.

    python resample.py in.wav out.wav 48000 [out_frames]
"""
import sys
import numpy as np
import soundfile as sf
import soxr

BLOCK = 1 << 16


def resample(src, dst, out_rate, out_frames=None):
    info = sf.info(str(src))
    if out_frames is None:
        out_frames = int(round(info.frames * out_rate / info.samplerate))
    stream = soxr.ResampleStream(info.samplerate, out_rate, info.channels,
                                 dtype="float32", quality="HQ")
    written = 0
    with sf.SoundFile(str(src)) as r, sf.SoundFile(
            str(dst), "w", samplerate=out_rate, channels=info.channels,
            subtype="FLOAT") as w:
        while True:
            block = r.read(BLOCK, dtype="float32", always_2d=True)
            last = block.shape[0] < BLOCK
            out = stream.resample_chunk(block, last=last)
            if written + out.shape[0] > out_frames:
                out = out[: max(0, out_frames - written)]
            if out.shape[0]:
                w.write(out)
                written += out.shape[0]
            if last:
                break
        if written < out_frames:
            w.write(np.zeros((out_frames - written, info.channels), dtype="float32"))
            written = out_frames
    return written


if __name__ == "__main__":
    src, dst, rate = sys.argv[1], sys.argv[2], int(sys.argv[3])
    frames = int(sys.argv[4]) if len(sys.argv) > 4 else None
    print(src, "->", dst, resample(src, dst, rate, frames), "frames @", rate)
```

Written to `/tmp/claude-0/-home-user-StemLab/a0f8e342-dd46-528f-8eac-900d5393c777/scratchpad/resample.py`; copy it to `$WORK/resample.py`.

**Not measured, and not estimated anywhere above:** the cost of the 48 kHz mismatch; any timing or quality figure for either overlap knob; anything at all about real music. The `1.20×` and `2×` figures in §2 are arithmetic read off `apply.py:265` and `utils.py:69-71`, not clocks.

---

## 9. One thing worth fixing regardless of the outcome

`src/stemlab/demucs_backend.py:138-152` does not pass `--shifts`, so every shipped Demucs separation runs at `shifts = 1` and is irreproducible run to run by up to half a second of offset. That is not a Track D finding to be gated on a measurement — it is the mechanism that produced the fictitious result the work order warns about, and it is still in the shipped command. It sits in a file another track owns, so I have not touched it; flagging it for whoever does.

**Files read for this report** (all absolute): `/home/user/StemLab/src/stemlab/regression/compare.py`, `/home/user/StemLab/src/stemlab/regression/__main__.py`, `/home/user/StemLab/src/stemlab/regression/metrics.py`, `/home/user/StemLab/src/stemlab/regression/corpus.py`, `/home/user/StemLab/src/stemlab/regression/__init__.py`, `/home/user/StemLab/src/stemlab/cli.py`, `/home/user/StemLab/src/stemlab/demucs_backend.py`, `/home/user/StemLab/src/stemlab/pretrained.py`, `/home/user/StemLab/src/stemlab/pipeline.py`, `/home/user/StemLab/src/stemlab/bs_roformer_cli.py`, `/home/user/StemLab/src/stemlab/audio.py`, and in the Engine venv `/root/.local/share/StemLab/Engine/lib/python3.11/site-packages/{demucs/separate.py,demucs/apply.py,demucs/audio.py,demucs/pretrained.py,demucs/remote/files.txt,bs_roformer/inference.py,bs_roformer/utils.py,bs_roformer/download.py,bs_roformer/model_registry.py,bs_roformer/configs/BS-Rofo-SW-Fixed.yaml}`. No file in the repository was modified.
