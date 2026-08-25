<p align="center">
  <img src="plugin/Resources/StemLabIcon.png" alt="StemLab" width="420">
</p>

# StemLab

StemLab is a Windows application and Ableton Live VST3 for separating music into
vocals, drums, bass, guitar, piano, and other stems.

The repository contains the source-development workflow. It deliberately does
not contain generated builds, model weights, virtual environments, or installer
packaging.

## Features

- JUCE Standalone application and VST3
- BS-RoFormer, Demucs `htdemucs_6s`, and hybrid separation
- Optional kick-bleed refinement
- Adaptive vocal, drum, and foreground stem trees
- File, physical-input, and Windows system-audio capture
- Waveform preview and selective export
- Direct import into Ableton through `StemLabRemote`

## Set Up Development

StemLab currently targets 64-bit Windows and Python 3.11. Install:

- Visual Studio with **Desktop development with C++** and CMake tools
- Python 3.11
- FFmpeg on `PATH` when working with compressed audio
- An NVIDIA/CUDA setup for practical model inference

From an ordinary PowerShell window in the repository root:

```powershell
.\setup_dev.ps1
```

This creates `.venv`, installs StemLab plus its development/recursive
dependencies, and runs the unit tests. The first install is large because the
audio backends depend on PyTorch and pretrained-model tooling.

To reuse a different environment:

```powershell
.\setup_dev.ps1 -EnvironmentPath C:\path\to\venv
```

## Build And Run

Build the Standalone application and VST3:

```powershell
.\plugin\build_windows.ps1
```

The script finds Visual Studio, downloads the pinned JUCE source when needed,
and performs an incremental Release build. Use `-Clean` only when you need to
discard the CMake build directory.

Run the Standalone app:

```powershell
& ".\plugin\build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
```

Install the development VST3 and Ableton Remote Script:

```powershell
.\install_ableton.ps1
```

The installer asks for administrator permission to copy the VST3 into the
standard Windows plug-in folder. It never closes Ableton automatically.

See [ABLETON_QUICKSTART.md](ABLETON_QUICKSTART.md) for Ableton configuration.

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
                        Plugin preview / StemLabRemote
```

- `PluginEditor` owns controls, layout, and waveforms.
- `PluginProcessor` owns capture, playback, child processes, and bridge state.
- `stemlab/plugin_job.py` translates between JUCE arguments and Python.
- `stemlab/pipeline.py` chooses the separation engine and optional refinement.
- `stemlab/recursive.py` routes adaptive child-stem operations.
- `StemLabRemote` is the only code allowed to manipulate Ableton tracks/clips.

The beginner-oriented tour is in [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).

## Command Line

After setup, separation can also run without the JUCE app:

```powershell
.\.venv\Scripts\stemlab-separate.exe `
    --input "song.wav" `
    --output ".\output"
```

Use `--engine roformer`, `--engine demucs`, or `--engine hybrid`. Add
`--no-refine` to keep the raw model output.

## Repository Map

```text
ableton_remote/          Ableton Live control-surface bridge
plugin/                  JUCE C++ frontend and build definition
stemlab/                 Python separation and DSP engine
tests/                   Fast unit tests using generated audio
ABLETON_QUICKSTART.md    Ableton setup and workflow
DEVELOPER_GUIDE.md       Code tour for new contributors
install_ableton.ps1      Development VST3/Remote Script installer
setup_dev.ps1            Python environment setup
pyproject.toml           Python package, commands, and tool settings
```

Directories such as `.venv/`, `.portable-cache/`, `plugin/build/`, `dist/`,
`__pycache__/`, and `.vs/` are generated locally and ignored by Git. Edit only
the source files listed above.

## License

StemLab's original code is MIT licensed. JUCE, FFmpeg, model runtimes, and
pretrained checkpoints retain their own terms; see
[THIRD_PARTY.md](THIRD_PARTY.md).
