# StemLab JUCE Plugin

This directory contains the JUCE source used to build both:

```text
StemLab.exe
StemLab.vst3
```

The Standalone and VST3 frontends share the same processor/editor code.

Heavy neural separation does not run on Ableton's real-time audio thread.
StemLab launches the external Python/ML engine and communicates with it through
job files and progress/status messages.

For Ableton, `StemLabRemote` provides the background Live Object Model
integration used by **Use Live Clip** and **Send Selected**.

## Build

Requirements:

- Visual Studio C++ toolchain
- CMake
- Git/internet access for the pinned JUCE dependency

From PowerShell:

```powershell
cd plugin
.\build_windows.ps1
```

The build outputs:

```text
build\StemLabPlugin_artefacts\Release\Standalone\StemLab.exe
build\StemLabPlugin_artefacts\Release\VST3\StemLab.vst3
```

## Application artwork

The Windows Standalone icon is embedded from:

```text
Resources\StemLabIcon.png
```

## Development engine discovery

Development builds can discover a local StemLab Python environment.

Portable release builds instead prefer the bundled runtime located in
`Engine/`, allowing end users to run StemLab without installing Python.
