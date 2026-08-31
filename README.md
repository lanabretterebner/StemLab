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
- Waveform preview and selective export, with per-lane auditioning
- Optional Beat This! key/BPM analysis (Fast by default, Accurate available)
- Per-stem MIDI export, shared waveform zoom, range selection, cancellable jobs
- Direct import into Ableton through `FIStemRemote`

## Set Up Development

FI-STEM currently targets 64-bit Windows. NVIDIA and CPU builds use Python
3.11; experimental AMD development uses Python 3.12. Install:

- Visual Studio with **Desktop development with C++** and CMake tools
- Python 3.11 (NVIDIA/CPU), or Python 3.12 for experimental AMD ROCm
- FFmpeg on `PATH` when working with compressed audio
- A compatible NVIDIA GPU/driver for recommended GPU inference

From an ordinary PowerShell window in the repository root, choose a runtime
backend. NVIDIA is the recommended default:

```powershell
# NVIDIA — recommended/default
.\scripts\setup_dev.ps1 -Backend nvidia

# CPU — universal fallback, but slower
.\scripts\setup_dev.ps1 -Backend cpu

# AMD ROCm — experimental
.\scripts\setup_dev.ps1 -Backend amd
```

NVIDIA and CPU setup create `.venv`. AMD setup creates a separate `.venv-amd`
and never converts an existing Python 3.11 environment. Setup installs and
verifies a pinned backend-specific PyTorch build before installing FI-STEM plus
its development/recursive dependencies, then runs the unit tests. A separate
CUDA Toolkit is not required because the NVIDIA wheel packages its CUDA runtime.

To reuse a different environment:

```powershell
.\scripts\setup_dev.ps1 -Backend nvidia -EnvironmentPath C:\path\to\venv
```

AMD ROCm 7.2.1 support is experimental and currently requires Windows 11,
Python 3.12, AMD's supported 26.2.2 driver, and hardware listed in AMD's current
compatibility matrix. Not every AMD GPU is supported.

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

The C++ side has its own suite, run through CTest after a build:

```powershell
ctest --test-dir plugin\build --output-on-failure
```

It covers the JUCE-free parts of the interface - the beat-grid and shared-zoom
maths, the host-capture policy, the source-label join, loop ranges, waveform
analysis, and the theme's contrast guarantees. Those run without a display.

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

Build one backend per portable folder and installer. NVIDIA is recommended and
is the default when `-Backend` is omitted:

```powershell
# NVIDIA — recommended/default
.\scripts\setup_dev.ps1 -Backend nvidia
.\build_installer_windows.ps1 -Backend nvidia

# CPU
.\scripts\setup_dev.ps1 -Backend cpu
.\build_installer_windows.ps1 -Backend cpu

# AMD — experimental
.\scripts\setup_dev.ps1 -Backend amd
.\build_installer_windows.ps1 -Backend amd
```

NVIDIA outputs are `dist\FI-STEM-Portable-0.9.9-NVIDIA` and
`dist\FI-STEM-Setup-0.9.9-NVIDIA.exe` (plus matching installer data slices).
CPU outputs use the `CPU` suffix. Each portable root includes
`RUNTIME_BACKEND.txt` with the packaged Python, Torch, CUDA/HIP, and build
details. Model downloads occur only while staging a release; the installed
runtime uses packaged models.

AMD development setup and ROCm/HIP validation are implemented. AMD portable and
installer commands currently stop with a concise error because the existing
release architecture has only a checksum-pinned Python 3.11 embedded runtime;
it would be unsafe to claim a portable Python 3.12 ROCm payload until that
separate runtime is reproducibly packaged. NVIDIA and CPU releases are fully
supported.

## Repository Map

```text
docs/                    Development, Ableton, and licensing notes
integrations/ableton/    Ableton Live control-surface bridge
plugin/Source/           JUCE C++ interface, theme, look-and-feel, widgets
plugin/Tests/            C++ unit tests, run through CTest
plugin/Resources/        Icon and the bundled Inter faces
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
