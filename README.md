# StemLab

StemLab is a Windows and Linux application and VST3 for separating music into
vocals, drums, bass, guitar, piano, and other stems, with direct Ableton Live
integration on Windows and direct REAPER integration on Linux.

The repository contains the source-development workflow only - no generated
builds, model weights, or virtual environments.

## Features

- JUCE Standalone application and VST3
- BS-RoFormer, Demucs `htdemucs_6s`, and hybrid separation, with optional
  refinement and adaptive per-stem splitting
- File, physical-input, and system-audio capture
- Lane-based audition: per-stem solo/mute, one shared transport, A/B
  against the original
- Live progress, ETA, and a Cancel that stops the whole job
- Optional key/BPM analysis, per-stem MIDI export, and a beat grid
- Insert into REAPER / send to Ableton / save or drag stems anywhere
- NVIDIA CUDA, AMD ROCm, and Intel XPU offload with runtime CPU fallback

## Windows

Requires Visual Studio (Desktop development with C++ and CMake tools),
Python 3.11, and FFmpeg on `PATH`. From the repository root:

```powershell
.\scripts\setup_dev.ps1      # venv, dependencies, unit tests
.\scripts\build_plugin.ps1   # Standalone + VST3
& ".\plugin\build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe"
.\scripts\install_ableton.ps1   # VST3 + Ableton Remote Script
```

See [docs/ableton.md](docs/ableton.md) for the Ableton setup and workflow.

## Linux

No venv or system Python needed. Everything in one bundle:

```bash
./scripts/build_portable.sh
cd dist/StemLab-*-Linux && ./install.sh
```

Or piece by piece:

```bash
./scripts/build_plugin.sh        # Standalone + VST3
./scripts/install_vst3.sh        # -> ~/.vst3
./scripts/install_backend.sh     # Engine (--cuda / --rocm / --xpu / --cpu)
```

See [docs/linux.md](docs/linux.md) for dependencies, GPU flavors, and the
REAPER workflow.

## Test

```powershell
.\.venv\Scripts\python.exe -m pytest -q    # python3 -m pytest tests -q on Linux
```

The tests use synthetic audio; they download no models and need no DAW.

## Command Line

```powershell
.\.venv\Scripts\stemlab-separate.exe --input "song.wav" --output ".\output"
```

Use `--engine roformer|demucs|hybrid`; add `--no-refine` to keep the raw
model output.

## Build A Release

Push a tag matching the version in `pyproject.toml` and
`.github/workflows/release.yml` builds, checksums, and publishes every
asset (Windows installer, Linux bundle, plugin binaries, wheel/sdist). A
mismatched tag fails the run. Run the workflow manually with **publish**
off to test packaging changes.

Locally: `.\scripts\build_portable_windows.ps1` then
`.\scripts\build_installer_windows.ps1 -SkipPortableBuild` on Windows;
`./scripts/build_portable.sh --torch-flavor cpu` on Linux.

## Documentation

- [docs/development.md](docs/development.md) - architecture, module map,
  contracts, change checklist
- [docs/linux.md](docs/linux.md) - Linux build, install, GPU flavors, REAPER
- [docs/ableton.md](docs/ableton.md) - Ableton Live setup and workflow
- [docs/third-party.md](docs/third-party.md) - third-party licenses

## Repository Map

```text
docs/                    The guides listed above
integrations/ableton/    Ableton Live control-surface bridge
packaging/               Installer definition and release model manifest
plugin/                  JUCE C++ frontend, assets, CMake definition, tests
scripts/                 Development setup, build, install, release commands
stemlab/                 Python separation and DSP engine
tests/                   Fast unit tests using generated audio
pyproject.toml           Python package, commands, and tool settings
```

Generated directories (`.venv/`, `.portable-cache/`, `plugin/build/`,
`dist/`, ...) are ignored by Git.

## License

StemLab's original code is MIT licensed. JUCE, FFmpeg, model runtimes, and
pretrained checkpoints retain their own terms; see
[docs/third-party.md](docs/third-party.md).
