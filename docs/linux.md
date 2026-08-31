# StemLab on Linux

StemLab builds a native VST3 and Standalone application on Linux, with
in-process REAPER integration and PipeWire/PulseAudio system-audio recording.
This page covers building, installing, and what behaves differently from the
Windows release.

The quickest path is the self-contained bundle, which includes the Engine:

```bash
./scripts/linux/build.sh      # builds dist/StemLab-<version>-Linux-<flavor>/
cd dist/StemLab-*-Linux-* && ./install.sh
```

The sections below cover the individual pieces.

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

On Fedora or Arch, just run `./scripts/linux/build_plugin.sh` - it checks every
dependency with `pkg-config` first and prints the exact install command for
your distribution.

`libwebkit2gtk` and `libcurl` are deliberately **not** required: the plugin
builds with `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0`.

## 2. Build the plugin

```bash
./scripts/linux/build_plugin.sh
```

This downloads the pinned JUCE 9.0.0 source into `.portable-cache/` and passes
it straight to CMake, exactly as `scripts/win/build_plugin.ps1` does on Windows.
Git is not needed for the JUCE step.

Useful flags:

```bash
./scripts/linux/build_plugin.sh --juce-source /path/to/JUCE   # reuse a local checkout
./scripts/linux/build_plugin.sh --build-type Debug
./scripts/linux/build_plugin.sh --clean
```

Outputs land in:

```text
src/plugin/build/StemLabPlugin_artefacts/Release/VST3/StemLab.vst3
src/plugin/build/StemLabPlugin_artefacts/Release/Standalone/StemLab
```

Installing goes through the self-contained bundle: `./scripts/linux/build.sh`
then the bundle's `install.sh`, which registers the VST3 in `~/.vst3` and the
Engine in one step. In REAPER, make sure `~/.vst3` is listed under
**Options > Preferences > Plug-ins > VST**, then **Re-scan**.

## 3. Install the separation backend

The plugin shells out to the Python backend. One script sets it up - no venv,
no system Python, nothing to configure:

```bash
./scripts/linux/install_backend.sh
```

It downloads a relocatable CPython into `~/.local/share/StemLab/Engine`,
installs StemLab and its ML dependencies into it, and writes the pointer file
the plugin auto-discovers. Re-run it any time to update.

### GPU flavors

The installer picks a PyTorch build automatically only when the choice is
safe (NVIDIA present → CUDA, otherwise CPU) and prints a hint when it spots
AMD or Intel hardware; force a flavor with a flag:

| Flag | Hardware | Notes |
| --- | --- | --- |
| `--cuda` | NVIDIA | PyPI's default torch build. Auto-detected. |
| `--rocm` | AMD (RDNA2+ dGPU) | Official Linux ROCm wheels; the wheel bundles its own ROCm userspace, so the stock `amdgpu` kernel driver is enough. Suggested when AMD hardware is seen, never auto-picked - APUs/iGPUs report HIP as available but crash, so opt in only on a discrete card. |
| `--xpu` | Intel Arc / Xe | Official torch XPU wheels; needs Intel's compute runtime (`intel-compute-runtime` / level-zero). Suggested, not auto-picked. |
| `--cpu` | none | Smallest install; slow but correct. The no-GPU default. |

The plugin always asks the backend for the best device, and the backend
resolves it at run time: CUDA (which is also how ROCm answers), then XPU,
then CPU. An unsupported GPU therefore degrades to CPU with a log line
instead of failing.

Adaptive/recursive splitting runs through onnxruntime, which has no
ROCm/XPU build on PyPI - on those flavors the recursive stage runs on CPU
while the main separation offloads.

Developers can use a plain venv instead - `python3 -m venv .venv &&
.venv/bin/pip install -e .` next to the repository is found automatically.
Torch is deliberately not a declared dependency (the Windows backend
installers pin their own build), so install one first:
`.venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu
torch torchaudio` for CPU, or drop the index override for CUDA.

The plugin does not search for the engine. It is at

```text
$XDG_DATA_HOME/StemLab/Engine/bin/python3      # ~/.local/share/… by default
```

which is exactly where `scripts/linux/install_backend.sh` builds it and where
the bundle's `install.sh` moves it, and nowhere else. `$STEMLAB_ENGINE`
overrides it with an absolute path — that is the whole of it, and the only
thing a developer running from a checkout needs.

Searching for it is what was here before: a pointer file, a sibling runtime,
two venv layouts and `$PATH`, each of which could answer first and be wrong.
Which one had answered was invisible, and a DAW launched from a desktop
launcher does not inherit your shell's `PATH` anyway, so the last resort was
the one that behaved differently depending on how the host was started.

`ffmpeg` (any system package) is used to normalise compressed input. The
installer also sets up adaptive stem splitting with the `audio-separator`
build matching your torch flavor.

## Where StemLab writes files

Linux follows the XDG Base Directory spec rather than the Windows
`Documents\StemLab` layout, and keeps your audio out of the application's
own directory:

| What | Location |
| --- | --- |
| Captures | `~/Music/StemLab/Captures` |
| Recordings | `~/Music/StemLab/Recordings` |
| Separated stems | `~/Music/StemLab/Jobs` |
| The app and its Engine | `$XDG_DATA_HOME/StemLab` (`~/.local/share/…`) |
| The VST3 | `~/.vst3/StemLab.vst3` |
| Settings | `$XDG_CONFIG_HOME/StemLab` (`~/.config/…`) |
| Model weights | `~/.cache/bs-roformer-infer`, `~/.cache/huggingface`, `$XDG_DATA_HOME/StemLab/models` |
| Analysis, MIDI staging, compiled kernels | `$XDG_CACHE_HOME/StemLab/analysis` |
| Setup downloads, while a bundle is installing | `$XDG_CACHE_HOME/StemLab/setup` |

Everything a person made or asked for lives in your music folder under
`StemLab/`; the application lives under `~/.local/share/StemLab` and holds
nothing of theirs. That split is why removing the app cannot take your
recordings with it - on Linux the bundle unpacks into the same folder captures
used to be written to.

Your music folder is `$XDG_MUSIC_DIR`, or what `~/.config/user-dirs.dirs` says
it is - `~/Musik`, `~/Musique`, or wherever you moved it. StemLab never invents
an English `~/Music`: if neither says anything, it keeps your audio in
`$XDG_DATA_HOME/StemLab` rather than create a folder in a language you do not
use. Override the job location at runtime with **Choose File Location** in the
plugin.

Only the finished stems are kept. A hybrid run separates with both models and
then fuses them, and a refined run improves that result again; those working
folders are removed once the final one is written. Set
`STEMLAB_KEEP_INTERMEDIATES=1` to keep them for comparison.

## Removing and updating

The bundle carries both scripts next to the app:

```bash
~/.local/share/StemLab/update.sh              # update to the latest release
~/.local/share/StemLab/update.sh --check      # just say whether one is out

~/.local/share/StemLab/uninstall.sh                # all of it
~/.local/share/StemLab/uninstall.sh --keep-models  # ... but keep the weights
~/.local/share/StemLab/uninstall.sh --everything   # ... and take your audio
~/.local/share/StemLab/uninstall.sh --dry-run      # print what would go
```

**Settings > Check for Updates...** runs `update.sh --check` and reports what
it says. It deliberately stops there: installing an update replaces
`StemLab.vst3`, and replacing a plug-in binary underneath a host that has it
loaded is how the next scan finds a half-written bundle. Close the DAW and run
the script to install.

`update.sh` keeps the GPU flavor the install already has, so a `cuda` install
does not quietly become a `cpu` one.

`uninstall.sh` with no arguments removes everything StemLab put on the
machine: the standalone app, the VST3, the Engine, the settings, the model
weights, the analysis cache and the compiled kernels. The single exception is
your audio under `<music>/StemLab`, which a DAW project may reference by path
- `--everything` is the only mode that takes that, and it says so first.

Two of the caches are shared. `~/.cache/huggingface` and
`~/.cache/torch/hub/checkpoints` hold every torch application's downloads, so
only StemLab's own entries in them are removed; anything else on the machine
keeps its models. `--keep-models` is there for a reinstall that should not
re-download several gigabytes.

Both replace the install through the release's own `StemLab-Linux-setup.sh`,
which stages everything it downloads in `$XDG_CACHE_HOME/StemLab/setup` and
removes that directory when the install succeeds. Nothing is written into the
folder you ran the script from. A run that fails leaves the staging directory
on purpose, so re-running resumes instead of downloading the bundle again;
`uninstall.sh` clears it if it is ever orphaned. Set `STEMLAB_SETUP_STAGE` to
stage somewhere else if your cache is on a small partition.

## REAPER

Inside REAPER the plugin talks to the host directly - REAPER exposes its
whole API to plugins, so there is no script to install and nothing to
configure. Add StemLab to any track and the buttons become:

1. Select an audio item in the arrangement.
2. **Use Selected Item** - StemLab reads the item's audio file, timeline
   position, and take geometry (trim offset, play rate).
3. **Separate**, then audition the lanes (solo/mute per stem, A/B
   **Original | Stems**).
4. **Insert Stems** - one new track per selected stem appears directly under
   the source track, colour-coded, aligned with the original item, in a
   single undo block. **Save Stems** also works if you would rather place
   the files yourself.

Items whose take is trimmed or rate-shifted are re-created with the same
trim and rate, so the inserted stems play exactly in sync with the item you
selected. In-project and section sources (glued/reversed items, project-in-
project) need a render/glue first - the readout in the header says so.

### What Insert Stems does to the project

- **Adaptive sub-stems come too.** A stem you split further is inserted as a
  REAPER **folder track** holding its sub-stems, nested as deeply as you
  split it. The folder's own item is the unsplit stem, inserted **muted** -
  unmute it (and mute the folder's contents) to A/B the whole stem against
  the parts it was split into.
- **The source item is muted.** Otherwise the project would play the
  original and its separation on top of each other. It is part of the same
  undo block, so one Ctrl+Z puts everything back. If you deleted or replaced
  the item since **Use Selected Item**, StemLab leaves the project alone.
- **Peaks are built for the new files.** REAPER only builds a `.reapeaks`
  cache for media it imported itself, so stems placed through the API would
  otherwise draw as empty lanes. StemLab builds them right after the insert,
  a slice at a time so the UI never stalls, and redraws the arrangement when
  the last one lands.

Requires REAPER 5.02 or later (the API handshake); tested against 7.42. If a
future or heavily stripped REAPER stops exposing a function StemLab needs,
the plugin falls back to the Select File / Save Stems workflow and lists
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
app: drop or select a file, separate, audition, **Save Stems**.

**A GPU is optional.** The plugin asks the backend for the best device and
the backend probes at run time (CUDA/ROCm, then XPU, then CPU), logging which
one it chose. CPU separation works but is considerably slower — Demucs is
tolerable, BS-RoFormer much less so. Check the engine log under **Settings >
Copy diagnostics** if a job takes longer than you expect.

**Drag stems anywhere.** Every completed waveform — root stems and adaptive
children — can be dragged straight out of the plugin into any application
that accepts audio files: a DAW arrangement, a file manager, an editor. Click
still seeks; drag exports.

**Progress, ETA, and Cancel.** During a job the status line, the bar, and
the ETA all update live — a one-time model download shows as its own stage,
and BS-RoFormer's per-chunk time estimates feed the ETA directly. While a
job runs, **Separate** becomes **Cancel**: the engine shuts down
its own model workers, so nothing keeps burning CPU. The same watchdog fires
if the plugin or the whole host disappears mid-job — closing REAPER cannot
leave a separation running in the background.

## Wayland

JUCE renders through X11; under Wayland everything runs via XWayland -
worth knowing when you file a bug.

## Verifying a build

```bash
ldd src/plugin/build/StemLabPlugin_artefacts/Release/VST3/StemLab.vst3/Contents/x86_64-linux/StemLab.so | grep "not found"
python3 -m pytest tests -q
```
