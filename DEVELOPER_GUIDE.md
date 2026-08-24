# StemLab Developer Guide

This guide maps the adaptive stem tree code so one change does not require
understanding the whole app.

## The five files to learn first

| Area | File | What belongs here |
| --- | --- | --- |
| JUCE UI | `plugin/Source/PluginEditor.cpp/.h` | Layout, buttons, waveform tree, menus, collapse/expand behavior |
| Host/engine bridge | `plugin/Source/PluginProcessor.cpp/.h` | Launch Python jobs, parse manifests, playback, Ableton/DAW integration |
| Separation router | `stemlab/recursive.py` | Decides which backend runs and writes the tree manifest |
| Audio analysis | `stemlab/adaptive/analysis.py` | Composite/source-count estimates and anti-ghost child scoring |
| Recursion policy | `stemlab/adaptive/policy.py` | Thresholds that decide whether another split is offered |
| Instrument lead backend | `stemlab/adaptive/foreground.py` | Experimental foreground-vs-bed DSP separator |

A good rule: **UI decisions stay in `PluginEditor`; audio/separation decisions stay in Python.** `PluginProcessor` is the bridge between them.

## Visual Studio workflow

From the StemLab repository root:

```powershell
.\setup_recursive_dev.ps1
cd plugin
.\build_windows.ps1
```

CMake generates the Visual Studio solution under `plugin\build`. Open the generated `.sln` in Visual Studio, but edit the real files under `plugin\Source`, **not copies under `plugin\build`**. The build directory is generated and can be deleted/recreated.

For UI work, the easiest target to debug is the **Standalone** StemLab target. You do not need to launch Ableton every time you move a button or change tree behavior. Once the standalone build behaves correctly, test the VST3 in Ableton/Reason.

## Safe first edits you can own

### Change when StemLab offers another adaptive split

Open:

`stemlab/adaptive/policy.py`

The thresholds in `should_offer_split()` are intentionally readable. For example, raising the confidence threshold makes StemLab more conservative; lowering it makes it offer more branches.

### Change how many layers the experimental lead splitter may create

Open:

`stemlab/recursive.py`

Change:

```python
MAX_DYNAMIC_CHILDREN = 5
```

Do not make this huge yet. Every additional peel costs processing time and can amplify artifacts.

### Change tree row sizing/layout

Open:

`plugin/Source/PluginEditor.cpp`

Search for:

`minimumRowHeight`

That block controls the scrollable tree layout. It is isolated from the audio engine.

### Add a new splitter later

1. Put the backend in `stemlab/adaptive/`.
2. Return audio files; do not make the backend know about JUCE.
3. Register an operation in `run_recursive()` in `stemlab/recursive.py`.
4. Give each child a `category`, `actions`, and metadata.
5. JUCE will render the returned children from the manifest without needing a new hard-coded stem count.

## Adaptive manifest contract

The Python engine writes schema 2 manifests. Each child contains:

```json
{
  "id": "guitar/lead_foreground",
  "label": "Lead / Foreground",
  "path": "...wav",
  "category": "instrument.lead",
  "actions": [],
  "confidence": 0.82,
  "estimated_source_count": 1,
  "complexity": 0.34
}
```

The JUCE plugin should treat this manifest as the API boundary. This is important for future developers: replacing a model should not require redesigning the UI.

## Current limitations

- Vocal lead/backing and drum component splitting use trained `audio-separator` models.
- Guitar/Piano/Other lead splitting is currently **experimental DSP foreground extraction**, not a trained semantic lead-instrument model.
- `estimated_source_count` is a conservative compositeness heuristic. It is used to decide how far the tree may grow; it should not be presented as ground-truth musician counting.
- Adaptive depth is capped in `adaptive/policy.py` as a safety/performance guard.

## Tests

The adaptive DSP path has a model-free smoke test:

```powershell
python -m pytest tests\test_adaptive_tree.py -q
```

This does not download a model. It creates a synthetic stereo mixture, checks the analyzer, runs the foreground splitter, and validates the schema-2 manifest.

## Suggested Git workflow

Use small branches by responsibility, for example:

- `ui/tree-density`
- `engine/source-counting`
- `separator/lead-model`
- `integration/reason`

That makes it much easier for another developer to review one feature without reading the entire StemLab history.
