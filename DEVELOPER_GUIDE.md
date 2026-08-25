# StemLab Developer Guide

You do not need to understand the whole repository before making a useful
change. Start with one execution path, keep the tests green, and use the API
comments in the headers/modules as your map.

## If You Know Java

The same ideas appear under slightly different names:

| StemLab code | Rough Java equivalent |
| --- | --- |
| Python module (`audio.py`) | A small utility/service class |
| Python package (`refinement/`) | A Java package |
| `@dataclass` | A record or data-only POJO |
| Python type hint (`Path | None`) | A declared type that may be null |
| Python docstring (`"""..."""`) | Javadoc attached to a module/class/method |
| C++ header (`.h`) | Public class declaration/interface |
| C++ source (`.cpp`) | Method implementation |
| `std::unique_ptr<T>` | One owner of a heap object |
| `std::atomic<T>` | Thread-safe value shared by callbacks |
| Lambda (`[x] (...) { ... }`) | A Java lambda/callback |

Python indentation defines blocks, so there are no braces. A leading underscore
means "internal to this module" by convention. C++ is stricter than Java about
object lifetime and threading; avoid changing ownership or audio callbacks until
you are comfortable with the surrounding code.

## First Commands

From the repository root:

```powershell
.\setup_dev.ps1
.\plugin\build_windows.ps1
& ".\plugin\build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
```

Run tests by themselves with:

```powershell
.\.venv\Scripts\python.exe -m pytest -q
```

`tests/` contains real unit tests, not generated output. Keep it. Each test
creates tiny synthetic files/signals and checks one contract without running a
multi-gigabyte model.

## Main Execution Path

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

This process boundary is intentional. Neural inference must never run on the
real-time audio thread.

When **Send Selected** is used in Ableton:

```text
PluginProcessor sends manifest path over localhost UDP
    -> StemLabRemote validates the manifest
    -> Ableton creates tracks and audio clips on its main thread
    -> StemLabRemote writes progress/ack JSON
    -> PluginProcessor polls that JSON and updates the UI
```

## C++ Frontend

### `plugin/Source/PluginEditor.h/.cpp`

Owns visible behavior:

- `StemWaveformComponent` draws and seeks waveforms.
- `RecursiveStemRowComponent` renders one adaptive-tree child.
- `StemLabAudioProcessorEditor` creates controls, lays them out, and translates
  button clicks into processor calls.

Change this area for labels, colors, row sizes, menus, or control placement.
Do not put model selection or file-processing algorithms here.

### `plugin/Source/PluginProcessor.h/.cpp`

Owns application state and external work:

- captures host, physical-input, or system-loopback audio;
- launches and monitors Python jobs;
- reads manifests/progress files;
- previews completed audio;
- communicates with `StemLabRemote`.

The header is the best entry point. Its `/** ... */` blocks are Doxygen comments,
the C++ equivalent of Javadoc, and Visual Studio displays them in tooltips.

JUCE-required methods such as `processBlock`, `prepareToPlay`, and
`getStateInformation` are framework callbacks. Read JUCE documentation before
changing their signatures or thread behavior.

## Python Engine

| Module | Responsibility |
| --- | --- |
| `audio.py` | Shared WAV/FLAC loading, saving, resampling, and stem lookup |
| `pipeline.py` | Public router for RoFormer, Demucs, hybrid, and refinement |
| `pretrained.py` | BS-RoFormer process adapter |
| `demucs_backend.py` | Demucs process adapter and output normalization |
| `hybrid.py` | Spectral fusion of the two model estimates |
| `plugin_job.py` | JUCE command arguments, progress files, Ableton manifest |
| `runtime.py` | Safe child-process output/progress handling |
| `recursive.py` | Adaptive operation router and tree-manifest writer |
| `adaptive/analysis.py` | Conservative source-complexity estimates |
| `adaptive/policy.py` | Rules for offering another recursive split |
| `adaptive/foreground.py` | Experimental foreground/backing DSP splitter |
| `refinement/events.py` | Kick-event detection |
| `refinement/adaptive_cancel.py` | Constrained spectral subtraction |
| `refinement/kick.py` | Per-event kick-bleed correction |
| `refinement/pipeline.py` | Applies refinement across a stem folder |

Public modules, classes, and functions use Python docstrings. In an editor,
hover the name or use `help(name)` to read them.

## Stable Contracts

Two JSON formats connect otherwise independent parts of the program:

- `stemlab_ableton_manifest.json` connects Python output to `StemLabRemote`.
- `recursive_manifest.json` schema 2 connects Python adaptive jobs to the JUCE
  stem tree.

You can add optional JSON fields safely. Renaming/removing a field requires a
matching change on both sides plus tests.

An adaptive child currently looks like:

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

## Good First Changes

Change recursion thresholds in `stemlab/adaptive/policy.py`. The tests run in a
few seconds and the function has no JUCE dependency.

Change stem names/colors in the focused lookup functions rather than searching
through DSP code.

For UI spacing, find the relevant component's `resized()` method in
`PluginEditor.cpp`. JUCE uses `resized()` much like a manual Java layout manager.

For a Python behavior change:

1. Find the smallest owning module in the table above.
2. Add or adjust a test that demonstrates the intended result.
3. Make the change.
4. Run pytest.
5. Run the Standalone app when the change crosses the C++/Python boundary.

## Files You Do Not Edit

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

Deleting them is safe when their programs are closed, but they will be recreated.
Never edit copies of source files under `plugin/build`; edit `plugin/Source`.

## Current Limits

- Recursive vocal/drum splitting requires `audio-separator` model downloads.
- Guitar/piano/other adaptive splitting is experimental DSP, not a semantic
  instrument classifier.
- The source-count estimate is a recursion heuristic, not a literal musician
  count.
- `PluginProcessor.cpp`, `PluginEditor.cpp`, and `StemLabRemote/__init__.py` are
  still the largest modules because they coordinate framework APIs. Extract one
  responsibility at a time, build, test, and checkpoint each extraction.
