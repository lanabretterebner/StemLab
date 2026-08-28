# FI-STEM on Linux

FI-STEM builds a native VST3 and Standalone application on Linux. This page
covers building it, installing the Python engine, and what behaves differently
from the Windows release.

Tested on Ubuntu 24.04 with GCC 13, CMake 3.28 and JUCE 9.0.0.

## 1. Install the build dependencies

**Debian / Ubuntu**

```bash
sudo apt install \
  build-essential cmake pkg-config curl unzip \
  libasound2-dev libjack-jackd2-dev libpulse-dev \
  libx11-dev libxext-dev libxi-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxcomposite-dev libxrender-dev \
  libfreetype-dev libfontconfig1-dev libgl1-mesa-dev
```

On Fedora the PulseAudio headers are `libpulse-devel`; on Arch they are in
`libpulse`. `scripts/build_plugin.sh` checks every dependency with
`pkg-config` before configuring and prints the install command for your
distribution.

`libwebkit2gtk` and `libcurl` are deliberately **not** required: the plugin
builds with `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0`.

## 2. Build the plugin

```bash
./scripts/build_plugin.sh
```

This downloads the pinned JUCE 9.0.0 source into `.portable-cache/` and passes
it straight to CMake, exactly as `scripts/build_plugin.ps1` does on Windows, so
Git is not needed for the JUCE step.

Useful flags: `--juce-source /path/to/JUCE` to reuse an existing checkout,
`--build-type Debug`, and `--clean`.

Artefacts land in `plugin/build/FIStemPlugin_artefacts/Release/`:

| Target | Path |
| --- | --- |
| Standalone | `Standalone/FI-STEM` |
| VST3 | `VST3/FI-STEM.vst3` |

Copy the VST3 bundle to `~/.vst3/` for your host to find it.

### Drag and drop

JUCE 9.0.0's X11 drag-and-drop implementation waits for the drop target to
send `XdndFinished` before it considers the gesture over, and a target that
never sends one leaves the source window grabbing the pointer.
`scripts/juce-linux-dnd.patch` ends the gesture when the drop is sent and lets
the data handshake finish on its own. `build_plugin.sh` applies it to the
JUCE copy in `.portable-cache/` automatically; apply it by hand if you build
against your own checkout.

## 3. Install the separation backend

The plugin shells out to the Python backend:

```bash
./scripts/linux_backend.sh
```

This downloads a relocatable CPython into `~/.local/share/FI-STEM/Engine`,
installs the `stemlab` package and its ML dependencies into it, and writes the
pointer file the plugin discovers at
`~/.config/FI-STEM/portable_engine_path.txt`. Re-run it any time to update.

### GPU flavors

The installer only auto-picks when the choice is safe — NVIDIA present means
CUDA, otherwise CPU. It prints a hint when it sees AMD or Intel hardware but
never selects those itself, because APUs and iGPUs report HIP as available and
then crash. Force a flavor with a flag:

| Flag | Hardware | Notes |
| --- | --- | --- |
| `--cuda` | NVIDIA | PyPI's default torch build. Auto-detected. |
| `--rocm` | AMD (RDNA2+ discrete) | Official ROCm wheels; the wheel bundles its own ROCm userspace, so the stock `amdgpu` kernel driver is enough. Opt in only on a discrete card. |
| `--xpu` | Intel Arc / Xe | Official torch XPU wheels; needs Intel's compute runtime (`intel-compute-runtime` / level-zero). |
| `--cpu` | none | Smallest install; slow but correct. The no-GPU default. |

The backend resolves the device at run time — CUDA (which is also how ROCm
answers), then XPU, then CPU — so an unsupported GPU degrades to CPU with a log
line rather than failing.

Developers can use a plain venv instead: `python3 -m venv .venv &&
.venv/bin/pip install -e .` next to the repository is discovered
automatically. Torch is deliberately not a declared dependency, so install one
first — `.venv/bin/pip install --index-url
https://download.pytorch.org/whl/cpu torch torchaudio` for CPU, or drop the
index override for CUDA.

`ffmpeg` (any system package) is used to normalise compressed input.

### Engine discovery order

1. `$STEMLAB_ENGINE`, if it points at an existing file
2. a sibling portable runtime — `Engine/bin/python3` — searched upward from the
   plugin binary
3. `.venv/bin/stemlab-plugin-job` or `venv/bin/stemlab-plugin-job`, searched
   upward from the plugin binary and the working directory
4. the pointer at `$XDG_CONFIG_HOME/FI-STEM/portable_engine_path.txt`
5. `stemlab-plugin-job` on `$PATH`

Step 4 matters because a DAW launched from a desktop launcher does not
necessarily inherit your shell's `PATH`. If discovery picks the wrong one, set
it explicitly under **Settings**, or export `STEMLAB_ENGINE` before starting
the host.

## Differences from the Windows build

**System audio recording uses the desktop audio server.** *Record System*
captures the monitor of your default output through the PulseAudio client API,
which PipeWire also implements, so it works on both stacks with no setup. It
records what the desktop is playing at 48 kHz stereo. `libpulse` is `dlopen`ed
rather than linked, so on a system running neither server (bare ALSA, or
JACK-only) the button reports what is missing instead of taking the plugin
down.

**Ableton Live integration does not apply.** Live has no Linux build. The
plugin offers the same local-file workflow as the Standalone app: select a
file, separate, audition, save.

**A GPU is optional.** CPU separation works but is considerably slower —
Demucs is tolerable, BS-RoFormer much less so.

**File locations follow JUCE's `userDocumentsDirectory`,** which resolves to
`~/Documents` (or `$HOME` when XDG has no documents directory configured), the
same layout as the Windows build rather than an XDG-native one.

## Wayland

JUCE renders through X11; under Wayland everything runs via XWayland — worth
knowing when you file a bug.

## Verifying a build

```bash
ldd plugin/build/FIStemPlugin_artefacts/Release/VST3/FI-STEM.vst3/Contents/x86_64-linux/FI-STEM.so | grep "not found"
cd plugin/build && ctest --output-on-failure
python3 -m pytest tests -q
```
