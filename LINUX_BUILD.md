# StemLab on Linux

StemLab builds a native VST3 and Standalone application on Linux. This page
covers building, installing, and what behaves differently from the Windows
release.

Tested on Ubuntu 24.04 with GCC 13, CMake 3.28 and JUCE 9.0.0.

## 1. Install the build dependencies

**Debian / Ubuntu**

```bash
sudo apt install \
  build-essential cmake pkg-config curl unzip \
  libasound2-dev libjack-jackd2-dev \
  libx11-dev libxext-dev libxi-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxcomposite-dev libxrender-dev \
  libfreetype-dev libfontconfig1-dev libgl1-mesa-dev
```

**Fedora**

```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config curl unzip \
  alsa-lib-devel jack-audio-connection-kit-devel \
  libX11-devel libXext-devel libXi-devel libXrandr-devel libXinerama-devel \
  libXcursor-devel libXcomposite-devel libXrender-devel \
  freetype-devel fontconfig-devel mesa-libGL-devel
```

**Arch**

```bash
sudo pacman -S --needed base-devel cmake pkgconf curl unzip \
  alsa-lib jack2 libx11 libxext libxi libxrandr libxinerama \
  libxcursor libxcomposite libxrender freetype2 fontconfig mesa
```

`build_linux.sh` checks for all of these with `pkg-config` before configuring,
and prints the exact install command for your distribution if anything is
missing.

`libwebkit2gtk` and `libcurl` are deliberately **not** required: the plugin
builds with `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0`.

## 2. Build the plugin

```bash
./plugin/build_linux.sh
```

This downloads the pinned JUCE 9.0.0 source into `.portable-cache/` and passes
it straight to CMake, exactly as `build_windows.ps1` does on Windows. Git is
not needed for the JUCE step.

Useful flags:

```bash
./plugin/build_linux.sh --juce-source /path/to/JUCE   # reuse a local checkout
./plugin/build_linux.sh --build-type Debug
./plugin/build_linux.sh --clean
```

Outputs land in:

```text
plugin/build/StemLabPlugin_artefacts/Release/VST3/StemLab.vst3
plugin/build/StemLabPlugin_artefacts/Release/Standalone/StemLab
```

## 3. Install the VST3

```bash
./plugin/install_vst3.sh
```

That copies the bundle to `~/.vst3/StemLab.vst3` and verifies the module inside
it. Use `--prefix /usr/local/lib/vst3` for a system-wide install.

In REAPER, make sure `~/.vst3` is listed under
**Options > Preferences > Plug-ins > VST**, then **Re-scan**.

## 4. Install the separation backend

The plugin shells out to the Python backend, so that has to exist somewhere it
can find. A virtualenv beside the repository is the simplest option:

```bash
python3 -m venv .venv
.venv/bin/pip install -e .
```

The plugin discovers the engine in this order:

1. `$STEMLAB_ENGINE`, if it points at an existing file
2. a sibling portable runtime — `Engine/bin/python3` — searched upward from
   the plugin binary
3. `.venv/bin/stemlab-plugin-job` or `venv/bin/stemlab-plugin-job`, searched
   upward from the plugin binary, the repo root, and the working directory
4. the pointer written by the Standalone app at
   `$XDG_CONFIG_HOME/StemLab/portable_engine_path.txt`
5. `stemlab-plugin-job` on `$PATH`

Step 5 matters because a DAW launched from a desktop launcher does not
necessarily inherit your shell's `PATH`. If discovery picks the wrong one, set
it explicitly under **Settings > Choose StemLab engine**, or export
`STEMLAB_ENGINE` before starting the host.

`ffmpeg` is used to normalise compressed input. The system package is fine —
install it with your package manager.

## Where StemLab writes files

Linux follows the XDG Base Directory spec rather than the Windows
`Documents\StemLab` layout:

| What | Location |
| --- | --- |
| Captures | `$XDG_DATA_HOME/StemLab/Captures` (`~/.local/share/…`) |
| Recordings | `$XDG_DATA_HOME/StemLab/Recordings` |
| Separation jobs | `$XDG_CACHE_HOME/StemLab/jobs` (`~/.cache/…`) |
| Settings | `$XDG_CONFIG_HOME/StemLab` (`~/.config/…`) |

Jobs live in the cache directory because they are large and can always be
regenerated from the source audio. Override the job location at runtime with
**Choose File Location** in the plugin.

## Differences from the Windows build

**System audio recording is not available yet.** The Windows build captures the
default output device through WASAPI loopback. There is no Linux backend for
that yet, so the *Record System* / *Record PC* control is hidden rather than
offered as a button that always fails. Record into your host instead, or use
your existing PipeWire/PulseAudio routing. A monitor-source backend is the
planned replacement.

**Ableton Live integration does not apply.** Live has no Linux build, so the
*Install / Repair Ableton Integration* menu entry is hidden, and so are the
*Send Selected* / *Retry* controls that hand stems back to a Remote Script.

In their place the Linux plugin uses the same local-file workflow as the
Standalone app:

1. Drop an audio file onto the plugin window, or click **Select File**.
2. **Separate All Stems**.
3. Audition the results, tick the stems you want.
4. **Save Selected...** and drag them into your project.

Stems are written with the source's timeline position preserved, so they line
up when you drop them back onto the arrangement. Direct REAPER integration -
reading the selected item and inserting stem tracks without leaving the
plugin - is planned separately.

**CUDA is optional.** The plugin asks for `cuda`, and the backend now falls
back to CPU when CUDA is unavailable, logging which device it chose. CPU
separation works but is considerably slower — Demucs is tolerable, BS-RoFormer
much less so. Check the engine log under **Settings > Copy diagnostics** if a
job takes longer than you expect.

## Wayland

JUCE renders through X11. Under a Wayland session the plugin and standalone app
run via XWayland, which works but is worth knowing when you file a bug.

## Verifying a build

```bash
# The bundle should contain a loadable module:
ls plugin/build/StemLabPlugin_artefacts/Release/VST3/StemLab.vst3/Contents/x86_64-linux/

# Nothing should be unresolved:
ldd plugin/build/StemLabPlugin_artefacts/Release/VST3/StemLab.vst3/Contents/x86_64-linux/StemLab.so | grep "not found"

# Backend tests:
python3 -m pytest tests -q
```
