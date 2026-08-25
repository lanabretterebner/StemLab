# FI-STEM Developer Guide

This guide maps the moving parts of FI-STEM so changes can stay focused and
easy to verify.

## Setup

From the repository root:

```powershell
.\scripts\setup_dev.ps1
.\scripts\build_plugin.ps1
& ".\plugin\build\FIStemPlugin_artefacts\Release\Standalone\FI-STEM.exe"
```

Run the Python tests with:

```powershell
.\.venv\Scripts\python.exe -m pytest -q
```

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
    -> PluginEditor refreshes waveforms and buttons
```

Model inference runs in a child process so the audio/UI threads stay responsive.

When **Send Selected** is used in Ableton:

```text
PluginProcessor sends manifest path over localhost UDP
    -> FIStemRemote validates the manifest
    -> Ableton creates tracks and audio clips on its main thread
    -> FIStemRemote writes progress/ack JSON
    -> PluginProcessor polls that JSON and updates the UI
```

## C++ Plugin

### `plugin/Source/PluginEditor.h/.cpp`

Owns the visible UI:

- `StemWaveformComponent` draws and seeks waveforms.
- `RecursiveStemRowComponent` renders one adaptive-tree child.
- `StemLabAudioProcessorEditor` creates controls, lays them out, and turns
  button clicks into processor calls.

Use this area for labels, colors, row sizing, menus, and layout. Keep file
processing and model-selection logic in the processor/Python layer.

### `plugin/Source/PluginProcessor.h/.cpp`

Owns application state and external work:

- captures host, physical-input, or system-loopback audio;
- launches and monitors Python jobs;
- reads manifests/progress files;
- previews completed audio;
- communicates with `FIStemRemote`.

JUCE callbacks such as `processBlock`, `prepareToPlay`, and
`getStateInformation` are framework entry points. Keep their real-time and
threading constraints in mind when editing them.

## Python Engine

| Module | Responsibility |
| --- | --- |
| `audio.py` | Shared WAV/FLAC loading, saving, resampling, and stem lookup |
| `pipeline.py` | Public router for RoFormer, Demucs, hybrid, and refinement |
| `pretrained.py` | BS-RoFormer process adapter |
| `demucs_backend.py` | Demucs process adapter and output normalization |
| `hybrid.py` | Spectral fusion of model estimates |
| `plugin_job.py` | JUCE command arguments, progress files, Ableton manifest |
| `runtime.py` | Child-process output and progress handling |
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

- `stemlab_ableton_manifest.json` connects Python output to `FIStemRemote`.
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

1. Keep UI edits in `PluginEditor`.
2. Keep state, jobs, manifests, and audio-preview edits in `PluginProcessor`.
3. Build with `.\scripts\build_plugin.ps1`.
4. Smoke-test the Standalone app.

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
`integrations/ableton/FIStemRemote/`, or `tests/`. Do not edit generated
copies under `plugin/build`.

## Current Limits

- Release-model downloads happen during deliberate staging, never during normal
  installed runtime operation.
- Guitar/piano/other adaptive splitting is DSP-based and experimental.
- Source-count estimates are recursion heuristics, not literal musician counts.
- `PluginProcessor.cpp`, `PluginEditor.cpp`, and
  `integrations/ableton/FIStemRemote/__init__.py`
  remain the largest modules and should be split one responsibility at a time.
