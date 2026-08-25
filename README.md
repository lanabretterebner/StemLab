<p align="center">
  <img src="plugin/Resources/FIStemIcon.png" alt="FI-STEM" width="220">
</p>

# FI-STEM

FI-STEM is a Windows application and Ableton Live VST3 for separating music into
vocals, drums, bass, guitar, piano, and other stems.

The repository contains the source-development workflow. It deliberately does
not contain generated builds, model weights, or virtual environments. Release
scripts and checksum manifests are source-controlled; their generated output is not.

## Features

- JUCE Standalone application and VST3
- BS-RoFormer, Demucs `htdemucs_6s`, and hybrid separation
- Optional kick-bleed refinement
- Adaptive vocal, drum, and foreground stem trees
- File, physical-input, and Windows system-audio capture
- Waveform preview and selective export
- Optional Beat This! key/BPM analysis (Fast by default, Accurate available)
- Per-stem MIDI export, waveform zoom/range selection, and cancellable jobs
- Direct import into Ableton through `FIStemRemote`

## Set Up Development

FI-STEM currently targets 64-bit Windows and Python 3.11. Install:

- Visual Studio with **Desktop development with C++** and CMake tools
- Python 3.11
- FFmpeg on `PATH` when working with compressed audio
- An NVIDIA/CUDA setup for practical model inference

From an ordinary PowerShell window in the repository root:

```powershell
.\scripts\setup_dev.ps1
```

This creates `.venv`, installs FI-STEM plus its development/recursive
dependencies, and runs the unit tests. The first install is large because the
audio backends depend on PyTorch and pretrained-model tooling.

To reuse a different environment:

```powershell
.\scripts\setup_dev.ps1 -EnvironmentPath C:\path\to\venv
```

## Build And Run

Build the Standalone application and VST3:

```powershell
.\scripts\build_plugin.ps1
```

The script finds Visual Studio, downloads the pinned JUCE source when needed,
and performs an incremental Release build. Use `-Clean` only when you need to
discard the CMake build directory.

Run the Standalone app:

```powershell
& ".\plugin\build\FIStemPlugin_artefacts\Release\Standalone\FI-STEM.exe"
```

Install the development VST3 and Ableton Remote Script:

```powershell
.\scripts\install_ableton.ps1
```

The installer asks for administrator permission to copy the VST3 into the
standard Windows plug-in folder. It never closes Ableton automatically.

See [docs/ableton.md](docs/ableton.md) for Ableton configuration.

## Test

`tests/` is the unit-test suite and should stay in source control. Run it after
every behavior change:

```powershell
.\.venv\Scripts\python.exe -m pytest -q
```

The tests use synthetic audio, so they do not download model weights or require
Ableton.

## How It Fits Together

```text
JUCE button / host audio
        |
        v
PluginEditor -> PluginProcessor -> stemlab-plugin-job
                                      |
                                      v
                                  pipeline.py
                           /          |          \
                    RoFormer       Demucs       Hybrid
                           \          |          /
                                      v
                                  WAV stems
                                      |
                        Plugin preview / FIStemRemote
```

- `PluginEditor` owns controls, layout, and waveforms.
- `PluginProcessor` owns capture, playback, child processes, and bridge state.
- `stemlab/plugin_job.py` translates between JUCE arguments and Python.
- `stemlab/pipeline.py` chooses the separation engine and optional refinement.
- `stemlab/recursive.py` routes adaptive child-stem operations.
- `stemlab/source_analysis.py` provides optional original-source key/BPM analysis.
- `stemlab/midi.py` transcribes one completed stem at a time.
- `FIStemRemote` is the only code allowed to manipulate Ableton tracks/clips.

The module and runtime reference is in [docs/development.md](docs/development.md).

## Command Line

After setup, separation can also run without the JUCE app:

```powershell
.\.venv\Scripts\stemlab-separate.exe `
    --input "song.wav" `
    --output ".\output"
```

Use `--engine roformer`, `--engine demucs`, or `--engine hybrid`. Add
`--no-refine` to keep the raw model output.

Beat This! is off by default in the app and never blocks separation when
disabled. Fast uses `small0`; Accurate uses `final0`. Release builds stage both
checkpoints locally and validate their sizes and SHA-256 hashes.

## Build A Release

Build the portable folder and then the Inno Setup installer:

```powershell
.\build_portable_windows.ps1
.\build_installer_windows.ps1 -SkipPortableBuild
```

Outputs are `dist\FI-STEM-Portable-0.9.9` and
`dist\FI-STEM-Setup-0.9.9.exe` (plus installer data slices). Model downloads
occur only while staging a release; the installed runtime uses packaged models.

## Repository Map

```text
docs/                    Development, Ableton, and licensing notes
integrations/ableton/    Ableton Live control-surface bridge
plugin/                  JUCE C++ frontend, assets, and CMake definition
scripts/                 Development setup, build, and install commands
stemlab/                 Python separation and DSP engine
tests/                   Fast unit tests using generated audio
pyproject.toml           Python package, commands, and tool settings
```

Directories such as `.venv/`, `.portable-cache/`, `plugin/build/`, `dist/`,
`__pycache__/`, and `.vs/` are generated locally and ignored by Git. Edit only
the source files listed above.

## License

FI-STEM's original code is MIT licensed. JUCE, FFmpeg, model runtimes, and
pretrained checkpoints retain their own terms; see
[docs/third-party.md](docs/third-party.md).
