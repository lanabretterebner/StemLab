# Code analysis, September 2026

A read-only pass over the whole repository looking for four things:
performance problems, avoidable overhead, unnecessary code growth, and
bugs. This document explains how the analysis was run, what it found,
and the fixes proposed for each finding that matters. Nothing here has
been applied yet. The complete verified list, one row per issue with
file and line, is in
[code-analysis-2026-09-findings.md](code-analysis-2026-09-findings.md);
the IDs in brackets below point into it.

## Method

The repository was split into 27 review units: ten for the Python
engine, twelve for the C++ plugin (the two large files in four and three
line-range chunks), and five cross-cutting sweeps that read across
files: real-time safety and threading, dead code and duplication, Python
child-process startup, UI rendering, and the test suite with CI. Every
unit was read in full.

Every candidate finding then went to an independent verifier whose
instruction was to refute it: re-read the cited code and its callers,
grep the whole repository (including `pyproject.toml` entry points,
command strings in the C++, tests, scripts and workflows) before
accepting a dead-code claim, reproduce the mechanism with a script where
one could, check that the proposed fix is minimal and does not break a
test or a contract in `docs/development.md`. The verifier's verdict and
severity replace the reporter's.

Environment: this CPU-only container, Python 3.11, numpy 2.4, scipy
1.17, soundfile 0.14. The Python suite ran green before the analysis
(627 passed, 12 skipped, 102 s; `ruff check .` clean). The JUCE plugin
was not built here; claims about JUCE internals were checked against the
JUCE 9.0.0 sources where the verifier could fetch them.

The analysis snapshot is commit `ba8cad8`, the merge of PR #125, and
every line number in both documents refers to it. PR #126 ("Rule the
lane grid from the detected beats") merged while the analysis ran. Its
changes were re-checked against the findings afterwards: one finding is
superseded (marked as such below and in the inventory), the rest still
hold on `main`, and its new code was not reviewed.

| | Raw | Distinct issues |
|---|---:|---:|
| Confirmed | 172 (12 high, 35 medium, 125 low) | 152 |
| Plausible | 63 | 57 |
| Refuted | 21 | 21 |
| Superseded by PR #126 | 2 | 1 |

Of the confirmed issues, 72 are code growth, 56 are bugs, 18 are
performance and 10 are overhead. 88 have a trivial fix (a line or two),
64 a small one (one function), and 4 a medium one.

## 1. Bugs to fix first

### Plugin

**Audition MIDI plays silence** (C1-1, X1-2; related C1-3, X1-4, C1-4).
`midiAuditionSynth` never receives a sound, a voice, or a sample rate,
so the audition path mutes the monitor and renders nothing for the
length of the take. While it runs the active transport is no longer
pulled, so the next `stop()` on a playing stem mix spin-waits about a
second on the message thread; the note copy and two sorts also happen
under the lock the audio thread takes. Fix: decide whether the feature
stays. If it goes, remove the menu item, the synthesiser and both render
branches. If it stays, register a minimal sound/voice pair in the
constructor, call `setCurrentPlaybackSampleRate` from both
`prepareToPlay` overloads, keep pulling `activeTransport()` during the
audition, and build the note vectors outside the lock and swap them in.
Add a plugin test that auditions one note and asserts non-silent output.

**Processor teardown writes into destroyed members** (C1-2, X1-1). The
destructor resets the analysis and MIDI threads but not
`modelInventoryThread` and `modelJobThread`, which are declared before
the lock, vectors and cancel file their `finish()` writes into. Closing
the host during a model download or an inventory probe is a
use-after-destroy. Fix: reset both threads right after
`midiThread.reset()`. Two lines.

**Tempo segments are read without the lock** (C4-1).
`finishSourceAnalysis` runs on the worker thread and move-assigns
`sourceTempoSegments` under `stateLock`, while `getSourceTempoSegments`,
`canSetHostTempo`, `setHostTempo` and `setAbletonTempo` read it unlocked
on the message thread. Fix: return a copy under `stateLock`, snapshot it
once in the three callers, and delete the comment that claims
message-thread-only access.

**Linux system capture reports success with no audio** (C12-1). The
writer opens on the first captured chunk. Stopping before that chunk
arrives exits with `successful = true` and no file, and the stop path
then reloads the previously loaded source relabelled "System audio
recording". Fix: `if (!captureFailed && sawFirstChunk)
successful.store(true);`.

**Send MIDI to Ableton never works** (C3-1, P9-6). `abletonBridgeActive`
is declared, read once and never stored, so the action always reports
"StemLab Remote must be active", and the ack file the remote script
writes has no reader. Fix: gate on `getHostIntegration() ==
hostIntegrationAbletonLive` like the other Ableton actions, delete the
flag, and either poll the ack or stop writing it.

**Source-analysis state machine** (C4-2, C4-3, C4-4, X1-5). Four
related defects. The superseded-source early return leaves
`sourceAnalysisRunning` stuck true, so the editor stays at full tick
rate and no analysis can restart. The two-second kill timer armed when
Beat This is switched off kills whichever analysis thread is current,
so off-then-on within two seconds kills the new run. `setSourceAnalysisMode`
starts an analysis during a separation and resets the engine's shared
progress, ETA and cancel atomics, which can drop a pending Cancel. And
`startSourceAnalysis` destroys a running thread synchronously, blocking
the message thread for up to 1.5 s whenever a new file lands mid-analysis.
Fixes: clear the flags in the early return; capture an analysis
generation counter in the timer lambda; add the engine-running guard
`setBeatThisEnabled` already uses; retire a running thread into a list
drained asynchronously. Longer term, give source analysis its own
progress and cancel state instead of borrowing the engine's.

**Smaller plugin bugs.** Re-running a split deletes the children's files
while they stay listed until the new job succeeds (C3-3): drop the
parent's children from `recursiveItems` before `deleteRecursively()`.
`retryAbletonImport` arms the 12 s wait before the send succeeds, so a
failed send shows a phantom timeout and polls forever (C3-2): arm after
the send, as `sendSelectedStemsToAbleton` does. Analyse and Set BPM are
made visible in one branch and never hidden (C7-2): compute visibility
once above the branch chain. A lane registers itself as its own deep
mouse listener, so a wheel event over its own pixels scrolls the list
twice (C5-3): relay through a small listener object. The Credits page
measures its text at a 120 px floor width and leaves an empty scroll tail
(C10-2): pass the real width.

### Python engine

**The second analysis of any track crashes** (P5-1, P6-1).
`BeatAnalysis.to_dict()` flattens `TempoSegment` to dicts;
`_beat_from_cache` only re-tuples beats and downbeats, and
`analyse_source` then reads `segment.start` on a dict. Reproduced here
with a one-segment analysis; any track with 16 or more beats produces a
segment, so the cache-hit path fails for real music. The tests pass
because both fakes omit `tempo_segments`. Fix: a `BeatAnalysis.from_dict`
that rebuilds the segments, used by `_beat_from_cache`, plus a segment in
the fake in `tests/test_lazy_analysis_paths.py`.

**Model downloads cannot be cancelled and are orphaned** (P4-1, X3-2).
`_run_child` iterates stdout by newline; tqdm bars redraw with carriage
returns, so the cancel check and the progress creep run once, at the
end, and the child is never registered with the watchdog. After the
plugin's grace kill the download continues with nobody attached. Fix:
read on a daemon thread through `runtime.drain_cr_lf_stream`, poll the
queue at 100 ms, terminate then kill on cancel, and register the child.

**Model manager cache rows** (P4-2, P4-4, P4-5, P4-3). The "HuggingFace
hub" row points at `HF_HOME` rather than the hub directory, so Clear
deletes the login token and any datasets and ignores `HF_HUB_CACHE`
(one-token fix: `_huggingface_hub_cache()`). Removing the Demucs copy
unlinks only the snapshot symlink and reports the blob's size as freed.
The "bs-roformer" cache row is the model directory itself, so the panel
total counts RoFormer twice. `_directory_bytes` follows symlinks.

**Worker error markers only under `__main__`** (P6-2, X3-1).
`source_analysis` and `midi` print `STEMLAB_ERROR` and
`STEMLAB_CANCELLED` only in the `if __name__` block. The console scripts
the plugin launches on venv and dev installs call `main()` directly, so a
failed analysis shows no reason and a cancel exits 1 with a traceback.
Fix: move the wrapper into `main()`, as `plugin_job` does.

**Compile progress bar** (P3-1). `warm_up` treats the child's 0 to 100
percent as a 0 to 1 fraction, so the bar reaches 100% at 1%. One line,
plus a test that passes 50.0.

**Regression harness** (P8-1, P8-4, P8-3). `si_sdr` and `correlation`
flatten stereo before trimming, so any length difference misaligns the
channels; `compare.py`'s truth scoring passes untrimmed arrays. Measured:
identical stereo padded by 100 samples scores -19 dB instead of
infinity. NaN in a reference passes both gates because they are written
as `< threshold`. A kick event at the very start of a stem keeps its
correction at full strength at the region end. All small.

**Ableton remote script** (P9-1, P9-2). The heartbeat file is written to
`%USERPROFILE%\Documents` while the plugin reads the shell Documents
folder, which differs under OneDrive Known Folder Move. A second Send
Stems during an import starts a concurrent chain that interleaves tracks
and overwrites the ack; add an in-flight guard.

### Scripts

**`install.sh` ignores `STEMLAB_INSTALL_DIR`** (P10-1). The installer
that `build.sh` generates hardcodes the default directory, so a custom
install moves or deletes the default Engine and fails outright under the
sudo path. Fix: use the same `${STEMLAB_INSTALL_DIR:-...}` expansion as
the setup script. Also: `update.sh`'s release lookup is curl-only, so
wget-only hosts get a false "cannot reach github.com" (P10-2), and a
reused Engine never picks up a bumped CPython pin because the ready
marker records no version (P10-3).

## 2. Performance and overhead

### Editor tick

The editor timer runs at 20 Hz while a job runs or the transport plays
and 2 Hz idle. The lanes go to some length to repaint only what changed;
the following per-tick work undoes that.

- `refreshFromProcessor()` calls the editor's full `resized()` on every
  tick once a source is loaded (C7-1, C6-1, X4-1). That clears
  `glowCache`, so every following paint re-runs the DropShadow blurs the
  cache exists to avoid, re-shapes two labels and re-lays out about a
  hundred components. Fix: relayout only when the Analyse text or a
  button's visibility changed, and clear `glowCache` only on a real size
  or scale change.
- Lane `paint()` re-shapes every grid label and builds a `Path` per MIDI
  note with no clip test, even for the two 9 px playhead strips (X4-2,
  C5-2, X4-3). Fix: test against `g.getClipBounds()`; cache label ink
  widths alongside the column cache.
- `hasMidiInfo()` copies the whole note vector and stats the `.mid` per
  converted lane per tick (C4-5, C5-1, X4-4). Fix: read `midiFile` under
  the lock without copying the notes, or compare note counts first.
- `getRecursiveStemItems()` (lock, copy, quadratic tree rebuild) is
  fetched nine times per tick despite a comment promising one (C6-3,
  C7-4, X4-5), and child lanes refresh twice per tick (C6-2). Fix: keep
  `syncLanes()`'s snapshot for the tick.
- Per-tick filesystem work for values that change only on user action:
  the job root is resolved twice per tick, re-reading `user-dirs.dirs`
  on Linux (C7-3, C11-1); the Credits page re-lays out every tick while
  open (C10-3, X4-8); the file-name tooltip is re-measured every tick
  (C7-6).

### Python jobs

- Every plugin-launched job imports `scipy.signal` eagerly through
  `hybrid.py` and `refinement` (P1-3, P3-3, X3-3, P2-8). Measured here:
  importing `stemlab.plugin_job` costs 1.16 s, 0.82 s of it
  `scipy.signal`, paid before the model child is spawned and wasted
  entirely on `--no-refine` single-engine jobs and on `stemlab-models`.
  Fix: import inside `_fuse_channels`, `detect_kick_events` and
  `adaptive_cancel`.
- Hybrid fusion spends about 360 ms per 16 s chunk, roughly 8 to 12 s
  per stem on a six-minute track, most of it in scipy's legacy
  `stft`/`istft` (P1-1), and peaks near 240 MB per chunk per worker,
  mostly scipy scratch (P1-2). Fix: a private framing through
  `sliding_window_view` and `scipy.fft.rfft` with a cached overlap-added
  window envelope, guarded by an equivalence test against the scipy
  path. This is the one medium-sized DSP change; do it last, with a
  listening check.
- `adaptive_cancel` runs the fit and inverse STFT even when the
  confidence gate rejects the event and the caller discards the result,
  about 8 ms per rejected event (P8-2): return early. Phase recovery
  through `exp(1j*angle)` is a fifth of the per-event time (P8-5).
- FLAC staging decodes the whole track into one int32 array (P2-3):
  stream in blocks as `resample.py` does. The model inventory walks all
  of `HF_HOME` on every editor open (P4-6, X3-4): report sizes only when
  the Models page is open.

### Release and CI

Each of the three Windows release legs rebuilds the LTO plugin, reruns
ctest and re-downloads JUCE (C11-2): build once in a `windows-plugin`
job and pass the artefacts down. The Linux release job builds seven test
executables it never runs, one under full LTO (C11-3): add a
`--no-tests` switch. The four Linux flavor legs apt-install the full
JUCE toolchain although they skip the plugin build (C11-7, X5-7), and
the Python suite runs three times per release (X5-6).

### Tests

About 15 s of the 102 s suite is avoidable: `test_model_compile`
regenerates a 25 s WAV through a 1.1 million iteration Python loop eight
times (X5-1); the 508150-frame hybrid case is a single chunk that covers
nothing the 1536-frame case does not, while the tail pull-back branch it
is named after stays untested (X5-2); six HTTP server teardowns wait 0.5
s each (X5-3).

## 3. Unnecessary code growth

Dead code, all confirmed by repository-wide grep:

- Three `HostIntegrationPolicy` text helpers have no production caller
  and already disagree with the button copy the editor hardcodes (C8-4).
  `analysePeaks`/`analyseMono` duplicate the shipping peak reduction and
  are test-only, so the reduction that ships is the one without a test
  (C8-2). At the snapshot the beat-grid API in `WaveformGrid.h` was in
  the same position (C8-1, X2-1); PR #126 now draws the lane grid
  through it, so that finding is superseded and nothing there should be
  removed.
- Nineteen processor members and functions, the lane-height map and
  lock, and `readyStemRevision` (X2-2, C4-6); settings plumbing that is
  filled but never read, and thirteen callbacks declared twice and
  relayed by hand (C10-5, C10-8); twelve theme tokens (X2-9); a dead icon
  and hover branch (C9-1, C9-2); nine settings-menu ids (C5-8); assorted
  write-only members (C1-10, C1-11, C2-9, C5-9, C11-8, C12-6).
- Python: `estimate_key`, `build_ableton_midi_payload` (the plugin
  builds that payload in C++), `analyse_key`'s unused keyword inputs
  (X2-3, P6-4, P6-5); a foreground confidence that is always overwritten
  (P7-7); a policy category nothing produces (P7-8); unreachable hybrid
  chunk branches (P1-7); `PipelineResult.baseline_dir` naming a directory
  the pipeline just deleted (P1-4).

Duplication:

- C++: threaded-writer teardown (C2-6), interpreter prefix (C2-7), the
  job-clock reset in three launchers (C2-8), utility-thread stop (X2-8),
  caption painting in three settings rows (C10-7), lane construction
  (C6-6), grid and tempo translation tables (C7-10), REAPER name buffers
  (C12-2), loopback `fail()` (C12-5).
- Python: the SHA-256 and atomic-JSON helpers three times (X2-6); four
  identical backend blocks in `pipeline.separate` (P1-8);
  `split_drums`/`split_vocals`/`deverb_vocal` (P7-9); the Ableton clip
  policy twice, plus a legacy two-message protocol the plugin no longer
  sends (P9-10, P9-8, P9-9).
- Config and docs: `pyproject.toml` declares `soxr` twice and pins
  `pyyaml` and `tqdm` that nothing imports (P2-6); `docs/development.md`
  repeats two module rows and omits `paths.py` (X2-10, P5-9); stale
  comments (C9-6, C10-9, C8-6, C5-10).
- Tests: about 180 lines of helpers copy-pasted across files (X5-4); a
  tracked zero-byte test module and tautological assertions (X5-5);
  per-file fixtures that repeat what `conftest.py` already isolates
  (X5-9).

## 4. Suggested order of work

1. Trivial bug batch, one pull request, no behaviour risk: C1-2, C12-1,
   C3-1, C3-2, C4-2, P5-1, P3-1, P4-2, P6-2, P8-1, P8-3, P8-4, P10-1,
   P2-6.
2. Editor tick batch: C7-1, C7-2, X4-2, C5-2, C4-5, C6-3, C6-2, C7-3.
3. Job batch: lazy scipy imports, the model-manager cancel loop, the
   cache rows, FLAC streaming, the `adaptive_cancel` early return.
4. Analysis state machine (C4-1, C4-3, C4-4, X1-5) and the MIDI audition
   decision. These need a plugin build and a manual pass in REAPER and
   Live.
5. Dead code and duplication sweep, docs and `pyproject.toml`, test
   speed-ups. Build every CTest target afterwards.
6. Larger items: the hybrid STFT rewrite (P1-1), the release workflow
   restructure (C11-2), the Ableton in-flight guard (P9-2).

## 5. Checked and dismissed

Twenty-one claims did not survive verification and are listed at the end
of the inventory so they are not raised again. Among them: the
per-tick progress-file poll (two stats and a 30-byte page-cached read);
the legacy `waveformColour` state keys (a documented carry-forward); the
editor constructor's model-inventory probe (needed for auto-show);
`setenv` racing child launches (only a new-name insert reallocates
`environ`, and that happens in the constructor); and the duplicated
candidate-directory list in `model_manager` (deliberate, so the status
probe stays stdlib-only).

## Not covered

Windows-only code (the WASAPI capture thread, the PowerShell scripts)
was read but not run. No C++ build or DAW session was available, so the
plugin findings rest on reading the code and the JUCE sources rather
than on reproduction. Timings are from a four-core CPU container. The
code PR #126 added after the snapshot, `LoopQuantize.h` with its tests
and the detected-beat grid path, has not been reviewed.
