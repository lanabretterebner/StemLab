# StemLab on Linux

StemLab builds a native VST3 and Standalone application on Linux, with
in-process REAPER integration and PipeWire/PulseAudio system-audio recording.
This page covers building, installing, and what behaves differently from the
Windows release.

Tested on Ubuntu 24.04 with GCC 13, CMake 3.28, JUCE 9.0.0 and REAPER 7.42.

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
it straight to CMake, exactly as `scripts/build_plugin.ps1` does on Windows.
Git is not needed for the JUCE step.

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

The plugin shells out to the Python backend. One script sets it up - no venv,
no system Python, nothing to configure:

```bash
./install_backend_linux.sh
```

It downloads a relocatable CPython into `~/.local/share/StemLab/Engine`,
installs StemLab and its ML dependencies into it, and writes the pointer file
the plugin auto-discovers. Re-run it any time to update. It auto-detects
NVIDIA and picks the CUDA or CPU torch build; force one with `--cuda` or
`--cpu` (the CPU build is several gigabytes smaller).

Developers who prefer a venv can still use one - `python3 -m venv .venv &&
.venv/bin/pip install -e .` next to the repository is found automatically.

The plugin discovers the engine in this order:

1. `$STEMLAB_ENGINE`, if it points at an existing file
2. a sibling portable runtime — `Engine/bin/python3` — searched upward from
   the plugin binary
3. `.venv/bin/stemlab-plugin-job` or `venv/bin/stemlab-plugin-job`, searched
   upward from the plugin binary, the repo root, and the working directory
4. the pointer written by `install_backend_linux.sh` (and by the Standalone
   app) at `$XDG_CONFIG_HOME/StemLab/portable_engine_path.txt`
5. `stemlab-plugin-job` on `$PATH`

Step 5 matters because a DAW launched from a desktop launcher does not
necessarily inherit your shell's `PATH`. If discovery picks the wrong one, set
it explicitly under **Settings > Choose StemLab engine**, or export
`STEMLAB_ENGINE` before starting the host.

`ffmpeg` is used to normalise compressed input. The system package is fine —
install it with your package manager.

The installer also sets up adaptive/recursive stem splitting (the per-stem
"split further" actions) with the CPU or GPU build of `audio-separator`
matching your torch flavor.

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

## REAPER

Inside REAPER the plugin talks to the host directly - REAPER exposes its
whole API to plugins, so there is no script to install and nothing to
configure. Add StemLab to any track and the buttons become:

1. Select an audio item in the arrangement.
2. **Use Selected Item** - StemLab reads the item's audio file, timeline
   position, and take geometry (trim offset, play rate).
3. **Separate All Stems**, then audition the results.
4. **Insert Stems** - one new track per selected stem appears directly under
   the source track, colour-coded, aligned with the original item, in a
   single undo block. **Save Selected...** also works if you would rather
   place the files yourself.

Items whose take is trimmed or rate-shifted are re-created with the same
trim and rate, so the inserted stems play exactly in sync with the item you
selected. In-project and section sources (glued/reversed items, project-in-
project) need a render/glue first - the status line says so.

Requires REAPER 5.02 or later (the API handshake); tested against 7.42. If a
future or heavily stripped REAPER stops exposing a function StemLab needs,
the plugin falls back to the Select File / Save Selected workflow and lists
what was missing under **Settings > Copy diagnostics**.

## Differences from the Windows build

**System audio recording uses the desktop audio server.** *Record System*
captures the monitor of your default output through the PulseAudio client
API, which PipeWire also implements - so it works on both stacks with no
setup. It records whatever the desktop is playing at 48 kHz stereo. On a
system running neither (bare ALSA or JACK-only), the button reports what is
missing instead of recording.

**Ableton Live integration does not apply.** Live has no Linux build, so the
*Install / Repair Ableton Integration* menu entry is hidden. In hosts other
than REAPER the plugin offers the same local-file workflow as the Standalone
app: drop or select a file, separate, audition, **Save Selected...**.

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

The repository also contains no-hardware harnesses used during the port
(fake VST3 host, REAPER selftest hooks via `STEMLAB_REAPER_SELFTEST`); see
the pull-request history for how they are driven.
