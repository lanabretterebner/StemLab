# StemLab Developer Guide

This guide maps the moving parts of StemLab so changes can stay focused and
easy to verify.

## Setup

From the repository root on Windows:

```powershell
.\scripts\setup_dev.ps1
.\scripts\build_plugin.ps1
& ".\plugin\build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
```

On Linux, `./scripts/build_plugin.sh` builds the plugin and
`./scripts/install_backend.sh` sets up the engine - [linux.md](linux.md)
covers dependencies and flags.

Run the Python tests with:

```powershell
.\.venv\Scripts\python.exe -m pytest -q
```

(`python3 -m pytest tests -q` on Linux.)

`tests/` contains unit tests for behavior that should remain stable. Keep them
green before and after code changes.

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

### `plugin/Source/PluginEditor.h/.cpp`

Owns the visible UI (the Nocturne 1a "Lanes" panel):

- `StemLaneWaveform` draws one lane's bar waveform and seeks the shared
  transport; dragging it exports the stem file.
- `StemLaneComponent` renders one stem lane (root or adaptive child).
- `StemLabAudioProcessorEditor` creates controls, lays them out, and turns
  button clicks into processor calls.

Use this area for labels, menus, and layout. Colors, fonts, and dimensions
live as named tokens in `plugin/Source/StemLabTheme.h`; stock-widget drawing
and icons live in `plugin/Source/StemLabLookAndFeel.*`, and the custom
Nocturne widgets (checkbox, split control, scrubber, segmented control, ...)
in `plugin/Source/StemLabWidgets.*`. Keep file processing and
model-selection logic in the processor/Python layer.

### `plugin/Source/PluginProcessor.h/.cpp`

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

### `plugin/Source/Waveform*.h/.cpp`

Pure helpers behind the lanes: `WaveformGrid.h` (beat-grid math),
`WaveformAnalysis.h` (JUCE-free spectral analysis), and `WaveformCache.*`
(peak caching for lane drawing). The first two are exercised directly by
`plugin/Tests/` via CTest (`BUILD_TESTING`), without standing up a plugin.

## Python Engine

| Module | Responsibility |
| --- | --- |
| `audio.py` | Shared WAV/FLAC loading, saving, resampling, and stem lookup |
| `cli.py` | `stemlab-separate` / `stemlab-refine` / `stemlab-models` entries |
| `pipeline.py` | Public router for RoFormer, Demucs, hybrid, and refinement |
| `pretrained.py` | BS-RoFormer process adapter |
| `bs_roformer_cli.py` | Relocatable launcher for `bs-roformer-infer` |
| `demucs_backend.py` | Demucs process adapter and output normalization |
| `hybrid.py` | Spectral fusion of model estimates |
| `device.py` | Device selection for PyTorch backends (CUDA/XPU/CPU) |
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
3. Build with `.\scripts\build_plugin.ps1` (Linux: `./scripts/build_plugin.sh`).
4. Run the plugin unit tests via CTest when `Waveform*` helpers change.
5. Smoke-test the Standalone app.

## Generated Files

These directories are generated and ignored:

```text
.venv/
.substem-venv/
.portable-cache/
.vs/
dist/
plugin/build/
**/__pycache__/
*.egg-info/
```

Edit source files under `stemlab/`, `plugin/Source/`,
`integrations/ableton/StemLabRemote/`, or `tests/`. Do not edit generated
copies under `plugin/build`.

## Current Limits

- Release-model downloads happen during deliberate staging, never during normal
  installed runtime operation.
- Guitar/piano/other adaptive splitting is DSP-based and experimental.
- Source-count estimates are recursion heuristics, not literal musician counts.
- `PluginProcessor.cpp`, `PluginEditor.cpp`, and
  `integrations/ableton/StemLabRemote/__init__.py`
  remain the largest modules and should be split one responsibility at a time.
