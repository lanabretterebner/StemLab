# StemLab

StemLab is a Windows and Linux application and VST3 for separating music into
vocals, drums, bass, guitar, piano, and other stems, with direct Ableton Live
integration on Windows and direct REAPER integration on Linux.

The repository contains the source-development workflow only - no generated
builds, model weights, or virtual environments.

## Install

### Linux: [Download Linux Installer](https://github.com/lanabretterebner/StemLab/releases/latest/download/StemLab-Linux-setup.sh)

```bash
cd Downloads
bash StemLab-Linux-setup.sh cpu|xpu|cuda|rocm
```

    
### Windows: [Download Windows Installer](https://github.com/lanabretterebner/StemLab/releases/latest/download/StemLab-Windows-setup.ps1)

  Right-click StemLab-Windows-setup.ps1, "Run with PowerShell", and pick a backend (cpu|xpu|cuda)
  
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

## Documentation

Building, running, testing, and releasing are covered in `docs/`:

- [docs/development.md](docs/development.md) - setup and build on both
  platforms, architecture, module map, contracts, releases
- [docs/linux.md](docs/linux.md) - Linux build, install, GPU flavors, REAPER
- [docs/ableton.md](docs/ableton.md) - Ableton Live setup and workflow
- [docs/engine-measurements.md](docs/engine-measurements.md) - separation
  quality measurements: the method, and what has since been settled without them
- [docs/third-party.md](docs/third-party.md) - third-party licenses

## Repository Map

```text
docs/                        The guides listed above
scripts/                     Setup, build, install, and release commands,
                             plus the Windows installer definition
src/integrations/ableton/    Ableton Live control-surface bridge
src/plugin/                  JUCE C++ frontend, assets, CMake definition, tests
src/stemlab/                 Python separation and DSP engine
tests/                       Fast unit tests using generated audio
pyproject.toml               Python package, commands, and tool settings
```

Generated directories (`.venv/`, `.portable-cache/`, `src/plugin/build/`,
`dist/`, ...) are ignored by Git.

## License

StemLab's original code is MIT licensed. JUCE, FFmpeg, model runtimes, and
pretrained checkpoints retain their own terms; see
[docs/third-party.md](docs/third-party.md).
