# StemLab Developer Guide

This guide maps the moving parts of StemLab so changes can stay focused and
easy to verify.

## Setup

### Windows

Requires Visual Studio (Desktop development with C++ and CMake tools),
Python 3.11 (NVIDIA/CPU) or Python 3.12 (experimental AMD ROCm), and FFmpeg
on `PATH`. From the repository root, choose a runtime backend - NVIDIA is
the recommended default:

```powershell
.\scripts\win\setup_dev.ps1 -Backend nvidia   # recommended/default
.\scripts\win\setup_dev.ps1 -Backend cpu      # universal fallback, slower
.\scripts\win\setup_dev.ps1 -Backend amd      # AMD ROCm, experimental

.\scripts\win\build_plugin.ps1   # Standalone + VST3
& ".\src\plugin\build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
.\scripts\win\install_ableton.ps1   # VST3 + Ableton Remote Script
```

NVIDIA and CPU setup create `.venv`. AMD setup creates a separate
`.venv-amd` and never converts an existing Python 3.11 environment. Setup
installs and verifies a pinned backend-specific PyTorch build before
installing StemLab plus its development/recursive dependencies, then runs
the unit tests. A separate CUDA Toolkit is not required because the NVIDIA
wheel packages its CUDA runtime. AMD ROCm support is experimental and
currently requires Windows 11, Python 3.12, AMD's supported driver, and
hardware in AMD's compatibility matrix.

See [ableton.md](ableton.md) for the Ableton setup and workflow.

### Linux

No venv or system Python needed. Everything in one bundle:

```bash
./scripts/linux/build.sh
cd dist/StemLab-*-Linux-* && ./install.sh
```

`./scripts/linux/build_plugin.sh` builds the Standalone and VST3 on their
own for plugin development; the bundle's `install.sh` is the installer.

See [linux.md](linux.md) for dependencies, GPU flavors, and the REAPER
workflow.

## Test

```powershell
.\.venv\Scripts\python.exe -m pytest -q    # python3 -m pytest tests -q on Linux
```

The tests use synthetic audio; they download no models and need no DAW.
`tests/` contains unit tests for behavior that should remain stable. Keep
them green before and after code changes.

The C++ side has its own suites, which CI runs and a local build should too:

```bash
ctest --test-dir src/plugin/build --output-on-failure
```

| Target | What it holds the line on |
| --- | --- |
| `StemLabWaveformGridTests` | Bar and beat placement, and host-integration policy |
| `StemLabWaveformAnalysisTests` | The JUCE-free spectral analysis, FFT included |
| `StemLabLoopRegionsTests` | Which loop ranges merge, and where playback jumps |
| `StemLabSourceLabelTests` | Joining a track and take name without saying it twice |
| `StemLabLaneWheelDispatchTests` | Which wheel events a lane forwards to the viewport |
| `StemLabHostCaptureTests` | The self-drag guard and a real processor capturing audio |

The first four cover header-only components deliberately kept free of the
plugin, so a test can reach them without standing one up.
`StemLabLaneWheelDispatchTests` is the odd one: it pins JUCE's own dispatch
behaviour rather than code of ours, because a JUCE upgrade that changed it
would silently undo the fix that depends on it. `StemLabHostCaptureTests`
links the plugin itself.

Two of them link JUCE, but none needs a display - the wheel suite asserts
against `MouseEvent` identity rather than a window - so the whole suite runs
on a bare CI runner with `DISPLAY` unset.

## Command Line

```powershell
.\.venv\Scripts\stemlab-separate.exe --input "song.wav" --output ".\output"
```

Use `--engine roformer|demucs|hybrid`; add `--no-refine` to keep the raw
model output.

## Runtime Flow

When a user presses **Separate**:

```text
PluginEditor button callback
    -> StemLabAudioProcessor::launchSeparationAndExport()
    -> StemLabEngineThread starts stemlab-plugin-job
    -> plugin_job.run_plugin_job()
    -> pipeline.separate()
    -> RoFormerBackend / DemucsBackend / hybrid fusion
    -> optional refinement
    -> WAV files + JSON manifest
    -> PluginProcessor notices completion
    -> PluginEditor refreshes the lanes and footer
```

Model inference runs in a child process so the audio/UI threads stay
responsive. While a job runs the Separate control reads **Cancel**, which
asks the engine's watchdog to shut the whole job down.

When **Send Stems** is used in Ableton:

```text
PluginProcessor sends manifest path over localhost UDP
    -> StemLabRemote validates the manifest
    -> Ableton creates tracks and audio clips on its main thread
    -> StemLabRemote writes progress/ack JSON
    -> PluginProcessor polls that JSON and updates the UI
```

## C++ Plugin

### `src/plugin/Source/PluginEditor.h/.cpp`

Owns the visible UI (the Nocturne 1a "Lanes" panel):

- `StemLaneWaveform` draws one lane's bar waveform and seeks the shared
  transport; dragging it exports the stem file.
- `StemLaneComponent` renders one stem lane (root or adaptive child).
- `StemLabAudioProcessorEditor` creates controls, lays them out, and turns
  button clicks into processor calls.

Use this area for labels, menus, and layout. Colors, fonts, and dimensions
live as named tokens in `src/plugin/Source/StemLabTheme.h`; stock-widget drawing
and icons live in `src/plugin/Source/StemLabLookAndFeel.*`, and the custom
Nocturne widgets (checkbox, split control, scrubber, segmented control, ...)
in `src/plugin/Source/StemLabWidgets.*`. Keep file processing and
model-selection logic in the processor/Python layer.

### `src/plugin/Source/PluginProcessor.h/.cpp`

Owns application state and external work:

- captures host, physical-input, or system-loopback audio;
- launches and monitors Python jobs (progress, ETA, cancel watchdog);
- reads manifests/progress files;
- runs the shared monitoring transport: the per-stem mix with lane
  solo/mute, the Original | Stems A/B switch, and child-stem audition;
- communicates with `StemLabRemote`.

JUCE callbacks such as `processBlock`, `prepareToPlay`, and
`getStateInformation` are framework entry points. Keep their real-time and
threading constraints in mind when editing them.

### `src/plugin/Source/Waveform*.h/.cpp`

Pure helpers behind the lanes: `WaveformGrid.h` (beat-grid math),
`WaveformAnalysis.h` (JUCE-free spectral analysis), and `WaveformCache.*`
(peak caching for lane drawing). The first two are exercised directly by
`src/plugin/Tests/` via CTest (`BUILD_TESTING`), without standing up a plugin.

## Python Engine

| Module | Responsibility |
| --- | --- |
| `audio.py` | Shared WAV/FLAC loading, saving, resampling, and stem lookup |
| `resample.py` | Sample-rate conversion shared by every separation backend |
| `cli.py` | `stemlab-separate` / `stemlab-refine` / `stemlab-models` entries |
| `pipeline.py` | Public router for RoFormer, Demucs, hybrid, and refinement |
| `pretrained.py` | BS-RoFormer process adapter |
| `bs_roformer_cli.py` | Relocatable launcher for `bs-roformer-infer` |
| `bs_roformer_download_cli.py` | The same launcher for `bs-roformer-download` |
| `console_entry.py` | Runs an installed console script without pip's baked-in path |
| `bs_roformer_download_cli.py` | Relocatable launcher for `bs-roformer-download` |
| `console_entry.py` | Calls an installed console script without pip's launcher |
| `demucs_backend.py` | Demucs process adapter and output normalization |
| `hybrid.py` | Spectral fusion of model estimates |
| `device.py` | Device selection for PyTorch backends (CUDA/XPU/CPU) |
| `compile_support.py` | Opt-in `torch.compile` gate for the RoFormer models |
| `model_compile.py` | Fills the compiled-kernel cache before a job needs it |
| `model_manager.py` | `stemlab-model-manager`: model/cache inventory, fetch, removal |
| `plugin_job.py` | JUCE command arguments, progress files, Ableton manifest |
| `recursive_job.py` | JUCE command bridge for adaptive stem jobs |
| `runtime.py` | Child-process output, progress/ETA, and cancel watchdog |
| `analysis_cache.py` | Local SQLite analysis/MIDI cache and corrections |
| `beat_tracking.py` | Offline Beat This! inference and beat interpretation |
| `source_analysis.py` | Optional source key/BPM analysis |
| `midi.py` | Stem-specific transcription and MIDI output |
| `recursive.py` | Adaptive operation router and tree-manifest writer |
| `adaptive/analysis.py` | Source-complexity estimates |
| `adaptive/policy.py` | Rules for offering another recursive split |
| `adaptive/foreground.py` | Foreground/backing DSP splitter |
| `refinement/events.py` | Kick-event detection |
| `refinement/adaptive_cancel.py` | Constrained spectral subtraction |
| `refinement/kick.py` | Per-event kick-bleed correction |
| `refinement/pipeline.py` | Applies refinement across a stem folder |
| `regression/metrics.py` | SI-SDR, correlation, and spectral distance |
| `regression/compare.py` | Compares two sets of stems and gates on the difference |
| `regression/corpus.py` | Deterministic synthetic corpus, so CI needs no licensed music |
| `regression/__main__.py` | `python -m stemlab.regression` entry |

### Compiled inference (opt-in)

BS-RoFormer spends most of a forward pass in dense matmuls spread over many
small layers, so TorchInductor is worth roughly 1.5x once its kernels are
built. It is off by default because the toolchain it needs is not everywhere:

| Variable | Effect |
| --- | --- |
| `STEMLAB_TORCH_COMPILE` | `1`/`true`/`yes`/`on` compiles BS-RoFormer and Mel-Band RoFormer. Anything else, including unset, runs eager. |
| `STEMLAB_TORCH_COMPILE_CACHE` | Where generated kernels live. Defaults to `torchinductor/` under the managed analysis directory. |

`compile_support.compile_support_status()` decides whether a device can be
compiled at all and reports why when it cannot:

| Platform | Compiles? | Blocker |
| --- | --- | --- |
| Linux NVIDIA / AMD ROCm / Intel XPU | yes | - |
| Windows NVIDIA | only with the third-party `triton-windows` wheel | PyTorch's Windows CUDA wheels ship no Triton |
| Windows Intel XPU | yes | - |
| Windows AMD ROCm | no | no Triton for ROCm on Windows |
| CPU (any OS) | only with a host C++ compiler | inductor's CPU backend shells out to `g++`/`cl` |
| DirectML | no | `privateuseone` has no inductor backend |

Two properties the guards exist to preserve. A missing C++ compiler makes
inductor raise `InvalidCxxCompiler` from the *first traced call* rather than
from `torch.compile`, so the fallback wraps the call, not the wrap. And
because every separation is a fresh subprocess, the cache directory has to be
stable or compiling is a guaranteed loss - the first run pays about two
minutes, later runs load kernels instead of building them.

Who pays that first run is a separate question, answered under
[Warming the compiled-kernel cache](#warming-the-compiled-kernel-cache).

## Stable Contracts

Two JSON files connect otherwise separate pieces of the system:

- `stemlab_ableton_manifest.json` connects Python output to `StemLabRemote`.
- `recursive_manifest.json` schema 2 connects Python adaptive jobs to the JUCE
  recursive stem tree.

Adding optional fields is usually safe. Renaming or removing fields requires
matching updates on both sides plus tests.

An adaptive child entry currently looks like:

```json
{
  "id": "guitar/lead_foreground",
  "label": "Lead / Foreground",
  "path": "C:/.../lead_foreground.wav",
  "category": "instrument.lead",
  "actions": [],
  "confidence": 0.82,
  "estimated_source_count": 1
}
```

## Change Checklist

For Python changes:

1. Find the owning module in the table above.
2. Add or update a focused test when behavior changes.
3. Run `.\.venv\Scripts\python.exe -m pytest -q`.
4. Run the Standalone app when the change crosses the C++/Python boundary.

For C++ changes:

1. Keep UI edits in `PluginEditor`; take visual values from `StemLabTheme.h`.
2. Keep state, jobs, manifests, and monitoring edits in `PluginProcessor`.
3. Build with `.\scripts\win\build_plugin.ps1` (Linux: `./scripts/linux/build_plugin.sh`).
4. Run the plugin unit tests via CTest when `Waveform*` helpers change.
5. Smoke-test the Standalone app.

## Build A Release

Push a tag matching the version in `pyproject.toml` and
`.github/workflows/release.yml` builds, checksums, and publishes every asset
(Windows installers, Linux bundles, plugin binaries, wheel/sdist). A
mismatched tag fails the run. Run the workflow manually with publish off to
test packaging changes.

Bundles are built per torch flavor: `cpu`, `cuda` and `xpu` on both
platforms, plus `rocm` on Linux only - PyTorch publishes no ROCm wheel for
Windows. Each is named for the torch build it actually contains, read out of
the assembled Engine rather than from what the build was asked for.

Locally: `.\scripts\win\build_portable_windows.ps1` then
`.\scripts\win\build_installer_windows.ps1 -SkipPortableBuild` on Windows;
`./scripts/linux/build.sh --torch-flavor cpu` on Linux.

No bundle carries model weights. Each model downloads the first time it is
used, is rejected unless it matches a recorded length and digest, and is
named in the status area while it runs.

## Model Manager

`stemlab-model-manager` reports what is on disk and acts on it. The plugin
drives it; it is also usable directly:

```
python -m stemlab.model_manager --status            # JSON inventory
python -m stemlab.model_manager --download roformer
python -m stemlab.model_manager --download-missing
python -m stemlab.model_manager --delete-model demucs
python -m stemlab.model_manager --delete-cache torch-hub
```

`--status` answers using nothing but the standard library, deliberately: it
runs whenever the editor opens, and a half-installed environment is the case
the Model Manager exists to repair, so it must not need numpy or torch to
say what is missing. The cost is that it keeps its own copy of a few
filenames and search rules the engine defines elsewhere; `test_model_manager`
asserts each copy still matches whenever the owning module imports.

Fetching asks whoever owns each transfer to perform it - upstream's
downloader for RoFormer, the HuggingFace hub for Demucs, `audio-separator`
for the adaptive models - and lifts `HF_HUB_OFFLINE` for that child only,
since downloading is the one operation meant to reach the network.

In the plugin it is a modal panel over the interface, always available from
Settings and opening by itself only when a model the app cannot separate
without is absent - `ESSENTIAL_MODEL_IDS`, which is RoFormer and Demucs.
Dismissing it lasts the session only.

The rule is that narrow because the first one was not. It opened when any of
the seven models was missing, including the optional ones that fetch
themselves on first use, and it also opened whenever compiling was switched
on and a present model had no warm-up marker. Nothing writes that marker
except this module's own `--compile`, so with `STEMLAB_TORCH_COMPILE=1` the
second condition was permanently true and the panel appeared on every launch
of a fully installed app. `status()` now reports one flag,
`essentialModelMissing`, and the plugin has no second reason to open it.

### Warming the compiled-kernel cache

`compile_support` (see [Compiled inference](#compiled-inference-opt-in) for
which machines can compile at all) compiles during a real job, so without
warming the first separation after enabling `STEMLAB_TORCH_COMPILE` pays the
whole cost: 114.8 s
for the first forward pass against a cold cache, 27.4 s against a warm one,
where eager is 9.5 s. `stemlab.model_compile` moves that cost somewhere the
user chose to spend it.

```
STEMLAB_TORCH_COMPILE=1 python -m stemlab.model_manager --compile roformer
```

or the Compile button beside BS-RoFormer in the Model Manager, where the
**Compile separations** switch turns compiling on and off for every job the
plugin starts. The switch is seeded from `STEMLAB_TORCH_COMPILE` at startup,
so exporting the variable still works and shows up already on; the plugin then
publishes its own choice into the environment its engine children inherit, and
remembers it in the saved state.

Whether the machine can compile at all is only probed when the switch is
turned on (`--probe-compile` on the CLI). The answer needs torch, which costs
seconds to import, and status otherwise runs on every editor open.

The warm-up separates 25 seconds of synthetic audio through
`pretrained.build_roformer_command` - the same function a real separation
builds its argv with. That sharing is the point rather than a convenience: an
inductor entry is keyed on the graph, so a warm-up that reached the model any
other way could trace shapes no job asks for and fill the cache with kernels
nothing reads, which produces no error anywhere. Twenty-five seconds crosses a
chunk boundary more than once, because shapes settle after two passes and the
shorter final chunk costs one more compile.

Only the RoFormer separators can be warmed, because they are the only ones
`compile_support` patches. Asking for anything else - or asking with compiling
switched off, or on a machine with no toolchain - raises
`WarmUpUnavailable`, which the Model Manager reports as a state rather than a
failed job.

## Generated Files

`.venv/`, `.substem-venv/`, `.portable-cache/`, `.vs/`, `dist/`,
`src/plugin/build/`, `__pycache__/`, and `*.egg-info/` are generated and
ignored. Edit sources under `src/stemlab/`, `src/plugin/Source/`,
`src/integrations/ableton/StemLabRemote/`, or `tests/` - never the copies under
`src/plugin/build`.

## Current Limits

- Model weights are downloaded on first use, not shipped: no bundle carries
  them, each is rejected unless it matches a recorded length and digest, and
  the download is named in the status area while it runs. The first
  separation on a fresh install is therefore slower than the ones after it.
- Only the RoFormer separators are compiled, and only with
  `STEMLAB_TORCH_COMPILE=1`. Warm the cache from the Model Manager first, or
  the first compiled separation pays roughly two minutes to build kernels.
- Guitar/piano/other adaptive splitting is DSP-based and experimental.
- Source-count estimates are recursion heuristics, not literal musician counts.
- `PluginProcessor.cpp`, `PluginEditor.cpp`, and
  `src/integrations/ableton/StemLabRemote/__init__.py`
  remain the largest modules and should be split one responsibility at a time.
