# Code analysis, September 2026: verified findings inventory

Companion to [code-analysis-2026-09.md](code-analysis-2026-09.md), which explains the method and the proposed fixes. This file is the complete list, generated from the review data, one row per distinct issue. Where several review units reported the same location, the row carries every ID.

Line numbers refer to the analysis base commit `ba8cad8` (the merge of PR #125). PR #126 merged while the analysis ran; it shifts lines in `PluginEditor.cpp`, `PluginProcessor.cpp` and `WaveformGrid.h`, and its new code (`LoopQuantize.h` and its tests) was not reviewed.

| | Raw findings | Distinct issues |
|---|---:|---:|
| Confirmed | 172 | 152 |
| Plausible | 63 | 57 |
| Refuted | 21 | 21 |
| Superseded | 2 | 1 |
| Total | 258 | 231 |

Verdicts: **Confirmed** means the verifier re-derived the mechanism from the code (and reproduced it where a script could). **Plausible** means the mechanism is real but the impact is marginal, or it could not be fully confirmed without a build or a DAW. **Refuted** rows are kept so the same idea is not re-raised; the reason is in the last column. **Superseded** rows were true at the base commit and were made moot by a change merged during the analysis. Severity is the verifier's, not the reporter's. Fix size: trivial (a line or two), small (one function), medium (one module), large.


## Confirmed

> **Status, updated after the fixes landed.** Every one of the 152 confirmed
> issues below has been addressed: 150 in
> [PR #128](https://github.com/lanabretterebner/StemLab/pull/128), and the
> remaining two - C8-2 and P9-6 - in the follow-up that also wrote this note.
> The Status column on each row says which. A few rows had a smaller
> remainder recorded in the pull request rather than in this table; the
> Plausible section further down has not been worked at all.
>
> **The line numbers in this file are historical.** They refer to the
> analysis base commit `ba8cad8` and no longer point at the code the row
> describes. Find a finding by its description, not by its line.



### Plugin: processor

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| C1-1, X1-2 | high | bug | `src/plugin/Source/PluginProcessor.h:1031` | MIDI audition synth has no voices or sounds and never gets a sample rate: 'Audition MIDI' mutes the monitor and renders silence | small | fixed (#128) |
| C1-2, X1-1 | high | bug | `src/plugin/Source/PluginProcessor.cpp:1867` | modelInventoryThread/modelJobThread are not reset in ~StemLabAudioProcessor, so their teardown finish() writes members that are already destroyed | trivial | fixed (#128) |
| C3-1 | high | bug | `src/plugin/Source/PluginProcessor.cpp:7878` | abletonBridgeActive is never set, so Send MIDI to Ableton always fails | trivial | fixed (#128) |
| C4-1 | high | bug | `src/plugin/Source/PluginProcessor.cpp:6841` | sourceTempoSegments read unlocked on the message thread while the analysis worker move-assigns it under stateLock | small | fixed (#128) |
| C1-3, X1-4 | medium | bug | `src/plugin/Source/PluginProcessor.cpp:2039` | MIDI audition early-return stops pulling the transport, so a subsequent stop() of a playing stemMixTransport spins the message thread for ~1 s | small | fixed (#128) |
| C3-2 | medium | bug | `src/plugin/Source/PluginProcessor.cpp:5471` | retryAbletonImport arms the import wait before the send; a failed send leaves a phantom 12 s wait and perpetual ack poll | trivial | fixed (#128) |
| C3-3 | medium | bug | `src/plugin/Source/PluginProcessor.cpp:4173` | Re-running a split deletes the previous children's files while they stay in recursiveItems on cancel/failure | small | fixed (#128) |
| C4-2 | medium | bug | `src/plugin/Source/PluginProcessor.cpp:6329` | finishSourceAnalysis's superseded-source early return leaves sourceAnalysisRunning (and engineCancelRequested) stuck true | trivial | fixed (#128) |
| C4-3 | medium | bug | `src/plugin/Source/PluginProcessor.cpp:6541` | Beat This off-switch's 2 s kill timer kills whichever analysisThread is current, not the one it cancelled | small | fixed (#128) |
| C4-4 | medium | bug | `src/plugin/Source/PluginProcessor.cpp:6849` | setSourceAnalysisMode and the maintenance follow-up start source analysis during a separation and reset the shared engine progress/ETA/cancel atomics | small | fixed (#128) |
| X1-5 | medium | performance | `src/plugin/Source/PluginProcessor.cpp:6234` | setInputAudioFile -> startSourceAnalysis destroys a running analysis thread synchronously on the message thread (up to ~1.5 s grace + join) | medium | fixed (#128) |
| C1-10 | low | code-growth | `src/plugin/Source/PluginProcessor.h:1187` | lastPolledFileStage is write-only and its comment describes removed behaviour | trivial | fixed (#128) |
| C1-11 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:1370` | Dead members: StemLabUtilityThread::cancelStartedMs and Windows-only struct EventHandle | trivial | fixed (#128) |
| C1-12 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:496` | Two Doxygen blocks attached to the wrong declarations | trivial | fixed (#128) |
| C1-9 | low | bug | `src/plugin/Source/PluginProcessor.cpp:1741` | WASAPI loopback fail() publishes its failure with statusInfo severity | trivial | fixed (#128) |
| C2-4 | low | bug | `src/plugin/Source/PluginProcessor.cpp:3251` | beginSystemCaptureSource keeps stale REAPER item geometry when a Record PC take fails mid-run | trivial | fixed (#128) |
| C2-5 | low | bug | `src/plugin/Source/PluginProcessor.cpp:2379` | Play on an undecodable source silently no-ops on every click | trivial | fixed (#128) |
| C2-6 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:3140` | stopStandaloneRecording and stopHostAudioCapture duplicate the threaded-writer teardown | small | fixed (#128) |
| C2-7 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:3740` | Interpreter-prefix logic duplicated between launchSeparationAndExport and makePythonModuleCommand | small | fixed (#128) |
| C2-8 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:3824` | Job-clock reset block duplicated across three launchers (plus a near-copy in startSourceAnalysis) | small | fixed (#128) |
| C2-9 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:2203` | currentInputChannels is effectively write-only; setInputAudioFile's store is dead | trivial | fixed (#128) |
| C3-6 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:4519` | lastPolledFileStage is a write-only member with a stale comment | trivial | fixed (#128) |
| C3-7 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:5821` | Lone juce::WeakReference guard duplicates the lifetimeToken idiom | trivial | fixed (#128) |
| C3-8 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:5132` | isStandaloneApp() guards are dead after an AbletonLive host-integration check | trivial | fixed (#128) |
| C3-9 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:5036` | Unreachable 24-bit fallback in exportLoopedRegions bit-depth selection | trivial | fixed (#128) |
| C4-11 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:6701` | setAbletonTempo and requestAbletonClip check/delete a UUID-named reply file that cannot exist | trivial | fixed (#128) |
| C4-5, C5-1, X4-4 | low | performance | `src/plugin/Source/PluginProcessor.cpp:7722` | hasMidiInfo copies the full StemLabMidiInfo (all notes) and stats the file once per converted lane per UI tick | trivial | fixed (#128) |
| C4-6 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:7965` | Dead public API: getCurrentPreviewSelectionId, get/setWaveformLaneHeight (+laneHeightLock/waveformLaneHeights), getSourceKey, getSourceAnalysisDetails | small | fixed (#128) |
| C7-8 | low | bug | `src/plugin/Source/PluginProcessor.h:396` | Footer busy state omits model jobs: summary line and idle/done spinner take over during a download or compile | trivial | fixed (#128) |
| X1-6 | low | overhead | `src/plugin/Source/PluginProcessor.cpp:4489` | stemlab_progress.txt is stat'ed and read at 20 Hz for the whole separation even after STEMLAB_PROGRESS lines are flowing | small | fixed (#128) |
| X2-2 | low | code-growth | `src/plugin/Source/PluginProcessor.h:636` | Nineteen C++ members and the state only they read are unreachable | small | fixed (#128) |
| X2-8 | low | code-growth | `src/plugin/Source/PluginProcessor.cpp:1173` | StemLabUtilityThread::stopChildProcess re-implements stopJobProcess; isChildRunning copied in three thread classes | trivial | fixed (#128) |

### Plugin: editor

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| C5-2, X4-3 | medium | performance | `src/plugin/Source/PluginEditor.cpp:849` | paint() issues a fillRoundedRectangle for every in-view note on 9 px playhead-strip repaints | small | fixed (#128) |
| C5-3 | medium | bug | `src/plugin/Source/PluginEditor.cpp:1200` | Lane registered as its own deep mouse listener: wheel over lane pixels scrolls the list twice | small | fixed (#128) |
| C6-1, C7-1, C7-2, X4-1 | medium | performance | `src/plugin/Source/PluginEditor.cpp:3983` | refreshFromProcessor() calls resized() every tick once a source is loaded: glow cache wiped and folder button moved twice per tick | small | fixed (#128) |
| X4-2 | medium | performance | `src/plugin/Source/PluginEditor.cpp:764` | Lane paint() re-shapes every grid label per paint with no clip test, even for playhead-strip repaints | small | fixed (#128) |
| C5-10, X4-11 | low | code-growth | `src/plugin/Source/PluginEditor.h:17` | StemLaneWaveform class comment describes removed drag-export and 2px rounded bars | trivial | fixed (#128) |
| C5-4 | low | bug | `src/plugin/Source/PluginEditor.cpp:976` | samePicture ignores midiNoteCount, so a re-conversion repaints only inside playhead strips | trivial | fixed (#128) |
| C5-5 | low | bug | `src/plugin/Source/PluginEditor.cpp:1104` | Click on an enabled well whose profile has not landed seeks the transport to 0:00 | trivial | fixed (#128) |
| C5-7 | low | performance | `src/plugin/Source/PluginEditor.cpp:1564` | Child lanes stat their stem file twice on every UI tick (setChildInfo + refresh) | small | fixed (#128) |
| C5-8 | low | code-growth | `src/plugin/Source/PluginEditor.cpp:95` | Nine settings-menu id constants and their comment are dead since the menu became a panel | trivial | fixed (#128) |
| C5-9 | low | code-growth | `src/plugin/Source/PluginEditor.h:374` | StemLaneComponent::hasChildren is written but never read | trivial | fixed (#128) |
| C6-2 | low | performance | `src/plugin/Source/PluginEditor.cpp:1415` | Child lanes refresh() twice per tick: setChildInfo's trailing refresh plus refreshFromProcessor's lane loop | trivial | fixed (#128) |
| C6-3, X4-5 | low | performance | `src/plugin/Source/PluginEditor.cpp:3506` | getRecursiveStemItems() snapshot fetched 9 times per tick despite the 'one fetch for the whole tick' comment | small | fixed (#128) |
| C6-6 | low | code-growth | `src/plugin/Source/PluginEditor.cpp:3287` | Lane construction wiring duplicated at two sites; root name-to-index loop duplicated twice | small | fixed (#128) |
| C6-7 | low | bug | `src/plugin/Source/PluginEditor.cpp:3207` | Editor collapse state (collapsedRecursiveIds / rootExpanded) survives processor clearing the recursive tree, so a collapse from a previous job hides the next job's identically-named children | trivial | fixed (#128) |
| C7-10 | low | code-growth | `src/plugin/Source/PluginEditor.cpp:4924` | Grid-mode and tempo-reading page tables duplicated between wireSettingsPage and refreshSettingsPage | small | fixed (#128) |
| C7-12 | low | code-growth | `src/plugin/Source/PluginEditor.cpp:4703` | showSettingsMenu() is a misnamed single-caller wrapper around showSettingsPanel() | trivial | fixed (#128) |
| C7-3 | low | performance | `src/plugin/Source/PluginEditor.cpp:4237` | Per-tick job-root resolution: two getJobRootDirectory() calls (each re-reading ~/.config/user-dirs.dirs on Linux) plus stats every refresh | small | fixed (#128) |
| C7-5 | low | code-growth | `src/plugin/Source/PluginEditor.cpp:4290` | selectedCount duplicates includedLanes from laneSelectionCounts() | trivial | fixed (#128) |
| C7-6 | low | performance | `src/plugin/Source/PluginEditor.cpp:4031` | File-name tooltip re-measures the name with a fresh Font and GlyphArrangement every tick; the same path measurement below is cached | trivial | fixed (#128) |
| X4-7 | low | bug | `src/plugin/Source/PluginEditor.cpp:965` | timerRefresh's samePicture omits midiNoteCount, so a re-conversion's notes are not repainted until an unrelated repaint | trivial | fixed (#128) |

### Plugin: other, build and CI

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| C12-1 | high | bug | `src/plugin/Source/LinuxSystemCapture.cpp:420` | Linux system capture reports success without ever opening a writer, so the stop path reloads the old source as a 'System audio recording' | trivial | fixed (#128) |
| C10-2, C10-3, X4-8 | medium | bug | `src/plugin/Source/SettingsPanel.cpp:454` | Credits column height is measured at the stale (zero -> 120px floor) content width and never re-measured | small | fixed (#128) |
| C11-2 | medium | overhead | `.github/workflows/release.yml:472` | Each of the three Windows release legs rebuilds the LTO plugin and reruns ctest | small | fixed (#128) |
| C8-2 | medium | code-growth | `src/plugin/Source/WaveformAnalysis.h:496` | analysePeaks duplicates the peak reduction in WaveformCache::analyse and is test-only, so the shipping reduction is untested | small | fixed (follow-up) |
| C10-10 | low | code-growth | `src/plugin/Source/SettingsPanel.cpp:851` | SettingsPanel::mouseUp override and setInterceptsMouseClicks(true,true) are JUCE defaults, each annotated as doing something else | trivial | fixed (#128) |
| C10-4 | low | bug | `src/plugin/Source/SettingsPanel.cpp:377` | ActionRow caption does not repaint on enablement change | trivial | fixed (#128) |
| C10-5 | low | code-growth | `src/plugin/Source/SettingsPanel.h:98` | Dead settings plumbing left behind when the Analyse row moved to the source line | small | fixed (#128) |
| C10-7 | low | code-growth | `src/plugin/Source/SettingsPanel.cpp:222` | Caption painting duplicated in ChoiceRow, SwatchRow and ActionRow; ActionRow doc comment stranded above SwatchRow | trivial | fixed (#128) |
| C10-8 | low | code-growth | `src/plugin/Source/SettingsPanel.cpp:885` | Thirteen callbacks declared twice and relayed by hand between Preferences and SettingsPanel; onAnalysisToggle and three Settings fields are dead | small | fixed (#128) |
| C10-9 | low | code-growth | `src/plugin/Tests/AccentPaletteTests.cpp:75` | AccentPaletteTests comments promise an sRGB-gamut check the code does not make | trivial | fixed (#128) |
| C11-10 | low | code-growth | `.github/workflows/release.yml:159` | fail-fast comments promise a shippable partial release, but the publish job is skipped when any leg fails | trivial | fixed (#128) |
| C11-4 | low | bug | `src/plugin/Source/StemLabPaths.cpp:84` | userMusicRoot() treats the XDG 'disabled' sentinel ($HOME) as a music folder and puts media in ~/StemLab | trivial | fixed (#128) |
| C11-7, X5-7 | low | overhead | `.github/workflows/release.yml:183` | Flavor legs apt-install compiler and X11/audio dev headers that --skip-plugin-build never uses | trivial | fixed (#128) |
| C11-8 | low | code-growth | `src/plugin/Source/ModelManagerPanel.h:84` | Unused closeButton member in ModelManagerPanel | trivial | fixed (#128) |
| C11-9 | low | code-growth | `src/plugin/CMakeLists.txt:256` | Threads/dl link block is redundant with juce_core's linuxLibs and its comment is wrong | trivial | fixed (#128) |
| C12-3 | low | bug | `src/plugin/Source/ReaperBridge.cpp:311` | A failed close is applied to an earlier track instead of cancelling a carried open, leaving an unclosed folder | trivial | fixed (#128) |
| C12-5 | low | code-growth | `src/plugin/Source/LinuxSystemCapture.cpp:474` | System-loopback fail() teardown and status prefix duplicated across the Linux and Windows threads | small | fixed (#128) |
| C12-6 | low | code-growth | `src/plugin/Source/ReaperBridge.h:141` | PeakBuilder::isFinished() and its `finished` flag are write-only dead code | trivial | fixed (#128) |
| C12-7 | low | code-growth | `src/plugin/Source/LinuxSystemCapture.cpp:34` | Separate dlopen of libpulse.so.0 is redundant: dlsym(simpleHandle, "pa_strerror") resolves through libpulse-simple's DT_NEEDED tree | trivial | fixed (#128) |
| C8-3 | low | bug | `src/plugin/Source/WaveformAnalysis.h:329` | Skipping bin 0 does not remove DC: the Hann window leaks half of it into bin 1, defeating the silent-frame inheritance rule | small | fixed (#128) |
| C8-4 | low | code-growth | `src/plugin/Source/HostIntegrationPolicy.h:14` | Three of four HostIntegrationPolicy helpers have no production caller; the editor hardcodes the same strings itself | trivial | fixed (#128) |
| C8-5 | low | code-growth | `src/plugin/Source/WaveformAnalysis.h:62` | SpectralProfile::lengthSeconds is write-only; WaveformProfile carries the length the editor reads | trivial | fixed (#128) |
| C8-6 | low | code-growth | `src/plugin/Source/SelfFileDragGuard.h:5` | SelfFileDragGuard comment names a different product ('FI-STEM') | trivial | fixed (#128) |
| C9-1 | low | code-growth | `src/plugin/Source/StemLabWidgets.cpp:71` | IconButton hover-fill accentTint10 branch is unreachable (no IconButton toggles or is named "layers") | trivial | fixed (#128) |
| C9-2 | low | code-growth | `src/plugin/Source/StemLabLookAndFeel.cpp:973` | icons::layers is dead since the initial commit; two comments still describe the removed palette icon | small | fixed (#128) |
| C9-4 | low | bug | `src/plugin/Source/StemLabLookAndFeel.cpp:620` | Tooltip wraps at width-16 when sized but width-17 when drawn, so long paths can gain a clipped extra line | trivial | fixed (#128) |
| C9-5 | low | bug | `src/plugin/Source/StemLabWidgets.cpp:538` | SeparateSplitControl::mouseUp decides the segment by release position only, so a slipped click crosses segments | small | fixed (#128) |
| C9-6 | low | code-growth | `src/plugin/Source/StemLabLookAndFeel.cpp:139` | Comments misstate JUCE 9 font resolution: the default LookAndFeel's getTypefaceForFont is consulted for typeface-less fonts | trivial | fixed (#128) |
| X2-9 | low | code-growth | `src/plugin/Source/StemLabTheme.h:434` | Dead layout tokens in StemLabTheme.h (palette button, bar waveform, thumbnail cache), seven unused neutral ramp steps, and a duplicated Ableton device token | trivial | fixed (#128) |
| X4-10 | low | code-growth | `src/plugin/Source/StemLabWidgets.cpp:461` | SeparateSplitControl measures the constant 'Refine' label separately in refineArea() and paint() | small | fixed (#128) |
| X5-6 | low | overhead | `.github/workflows/release.yml:441` | Release runs the full Python suite once per Windows flavor (3x) with no skip switch in setup_dev.ps1 | small | fixed (#128) |

### Python engine

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| P4-1, X3-2 | high | bug | `src/stemlab/model_manager.py:701` | _run_child iterates the pipe by '\n', so cancel and progress are blind during tqdm ('\r') transfers | small | fixed (#128) |
| P5-1, P6-1 | high | bug | `src/stemlab/source_analysis.py:484` | Cache hit rebuilds BeatAnalysis with tempo_segments as dicts; consumer raises AttributeError | small | fixed (#128) |
| P8-1 | high | bug | `src/stemlab/regression/metrics.py:66` | si_sdr/correlation flatten (channels, samples) before trimming, so any length difference misaligns channels; --truth scoring hits it | small | fixed (#128) |
| P1-1 | medium | performance | `src/stemlab/hybrid.py:74` | Hybrid fusion spends ~360 ms per 16 s chunk, ~half of it in scipy's legacy stft/istft | medium | fixed (#128) |
| P3-1 | medium | bug | `src/stemlab/model_compile.py:191` | Warm-up progress treats the child's 0-100 percent as a 0-1 fraction | trivial | fixed (#128) |
| P4-2, P4-5 | medium | bug | `src/stemlab/model_manager.py:497` | 'HuggingFace hub' cache row points at HF_HOME, not the hub directory | trivial | fixed (#128) |
| P4-4 | medium | bug | `src/stemlab/model_manager.py:952` | delete_model on the HF Demucs copy unlinks the snapshot symlink only; blob and reported bytes remain | small | fixed (#128) |
| P6-2, X3-1 | medium | bug | `src/stemlab/source_analysis.py:703` | STEMLAB_CANCELLED/STEMLAB_ERROR wrappers live under __main__, so console-script launches (dev installs) never emit them | small | fixed (#128) |
| P8-2 | medium | performance | `src/stemlab/refinement/adaptive_cancel.py:213` | adaptive_cancel fits, smooths, caps and inverse-transforms even when confidence is below threshold and the caller discards the result | trivial | fixed (#128) |
| P8-4 | medium | bug | `src/stemlab/regression/metrics.py:237` | NaN/Inf in the reference is never checked; nan metrics pass the < gates and best_lag misreports it as an STFT centring difference | small | fixed (#128) |
| P1-4 | low | bug | `src/stemlab/pipeline.py:617` | PipelineResult.baseline_dir names a directory _discard_intermediates just deleted on every refine run | trivial | fixed (#128) |
| P1-6 | low | performance | `src/stemlab/audio.py:85` | load_audio's .astype(np.float32) after soxr.resample copies an already-float32 array | trivial | fixed (#128) |
| P1-7 | low | code-growth | `src/stemlab/hybrid.py:230` | Chunk-schedule tail pull-back in fuse_stem_pair is redundant with the _fuse_channels noverlap clamp; pad/trim and size==0 guards are unreachable | small | fixed (#128) |
| P1-8 | low | code-growth | `src/stemlab/pipeline.py:424` | Four identical backend construct-and-separate blocks in pipeline.separate() | small | fixed (#128) |
| P2-1 | low | bug | `src/stemlab/pretrained.py:90` | Track name is interpolated unescaped into Path.glob, so bracketed names skip output canonicalisation | trivial | fixed (#128) |
| P2-4 | low | bug | `src/stemlab/demucs_backend.py:136` | Previous stems are deleted before the install, ffmpeg and cancellation preconditions are checked | small | fixed (#128) |
| P3-4 | low | bug | `src/stemlab/device.py:124` | A probe that raised is cached as 'unavailable' for 24 hours | trivial | fixed (#128) |
| P3-7 | low | code-growth | `src/stemlab/model_compile.py:206` | _warm_up_model_name() lazily re-imports a module already imported at top level | trivial | fixed (#128) |
| P3-8 | low | bug | `src/stemlab/compile_support.py:331` | arm_torch_compile setdefaults TORCHINDUCTOR_CACHE_DIR while the warm-up assigns it, so a pre-set value splits the caches | trivial | fixed (#128) |
| P4-3 | low | bug | `src/stemlab/model_manager.py:545` | _directory_bytes double-counts HuggingFace blobs through snapshot symlinks | trivial | fixed (#128) |
| P4-7 | low | bug | `src/stemlab/model_manager.py:792` | download() for recursive models calls .mkdir on None when _recursive_model_dir() has no home | trivial | fixed (#128) |
| P5-10 | low | code-growth | `src/stemlab/beat_tracking.py:117` | Beat This! model cache/lock can never serve a second lookup in any current process | small | fixed (#128) |
| P5-7 | low | code-growth | `src/stemlab/analysis_cache.py:68` | PRAGMA foreign_keys on every connection is a no-op and sits outside the close guard | trivial | fixed (#128) |
| P5-8 | low | code-growth | `src/stemlab/analysis_cache.py:20` | managed_analysis_dir() is a pass-through shim for paths.analysis_dir() | small | fixed (#128) |
| P6-4, X2-3 | low | code-growth | `src/stemlab/midi.py:632` | build_ableton_midi_payload has no production caller; the plugin builds the payload in C++ | small | fixed (#128) |
| P6-5 | low | code-growth | `src/stemlab/source_analysis.py:471` | estimate_key compatibility wrapper has zero callers | trivial | fixed (#128) |
| P6-6 | low | code-growth | `src/stemlab/midi.py:408` | midi._file_hash duplicates analysis_cache.source_identity | trivial | fixed (#128) |
| P7-2 | low | overhead | `src/stemlab/recursive.py:702` | A rejected foreground peel pass (pass > 1) leaves two full-length float32 WAVs orphaned in the job directory | trivial | fixed (#128) |
| P7-6 | low | bug | `src/stemlab/adaptive/analysis.py:87` | _spectral_flatness inspects only the first ~4.5 s (44.1 kHz) of the 24 s start/middle/end excerpt | trivial | fixed (#128) |
| P7-7 | low | code-growth | `src/stemlab/recursive.py:730` | split_lead_group threads a DSP confidence that _with_adaptive_metadata always overwrites; its None branch is unreachable | small | fixed (#128) |
| P7-8 | low | code-growth | `src/stemlab/adaptive/policy.py:36` | should_offer_split lists 'vocal.lead_group', which nothing emits; adjacent comment is stale | trivial | fixed (#128) |
| P8-10 | low | code-growth | `src/stemlab/regression/compare.py:12` | compare.py docstring advertises the module-direct invocation that __main__.py exists to avoid, and keeps a dead __main__ shim | trivial | fixed (#128) |
| P8-11 | low | bug | `src/stemlab/regression/compare.py:240` | Regression JSON report emits bare Infinity/-Infinity for the common identical-stem case | small | fixed (#128) |
| P8-3 | low | bug | `src/stemlab/refinement/kick.py:266` | Edge window is sliced from the front, so an event within reference_pre_ms of the file start keeps full correction strength at the region end | trivial | fixed (#128) |
| P8-5 | low | performance | `src/stemlab/refinement/adaptive_cancel.py:222` | Phase is recovered with exp(1j*angle(h)) per event, far slower than normalising by magnitude; channel and real/imag loops add smaller per-event overhead | small | fixed (#128) |
| P8-7 | low | code-growth | `src/stemlab/refinement/kick.py:139` | build_kick_reference re-extracts every reference region to recompute a peak the first loop already has | trivial | fixed (#128) |
| P8-8 | low | overhead | `src/stemlab/refinement/events.py:70` | detect_kick_events squares in float64 on a false premise about scipy's accumulator, adding an avoidable full-length temporary | trivial | fixed (#128) |
| P8-9 | low | overhead | `src/stemlab/refinement/kick.py:289` | refine_kick_bleed returns a redundant full-length copy via astype on an already-float32 array | trivial | fixed (#128) |
| X3-3 | low | overhead | `src/stemlab/hybrid.py:13` | Eager scipy.signal import adds ~0.8 s to every stemlab-plugin-job / stemlab-models startup | small | fixed (#128) |
| X3-7 | low | overhead | `src/stemlab/__init__.py:4` | stemlab/__init__ imports importlib.metadata eagerly (~33 ms of a ~40 ms package import) for a __version__ only a test reads | trivial | fixed (#128) |
| X3-9 | low | code-growth | `src/stemlab/beat_tracking.py:399` | beat_tracking _MODEL_CACHE/_MODEL_LOCK never hit: analyse_beats runs once per worker process | trivial | fixed (#128) |

### Ableton remote script

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| P9-1 | medium | bug | `src/integrations/ableton/StemLabRemote/__init__.py:1357` | Heartbeat status file written to %USERPROFILE%\Documents, plugin reads the shell-resolved Documents folder | small | fixed (#128) |
| P9-2 | medium | bug | `src/integrations/ableton/StemLabRemote/__init__.py:907` | No in-flight guard: a second stemlab_ready during an import chain runs a concurrent chain against the same source track | small | fixed (#128) |
| P9-10 | low | code-growth | `src/integrations/ableton/StemLabRemote/__init__.py:643` | Arrangement-clip selection policy duplicated between two resolvers | small | fixed (#128) |
| P9-12 | low | code-growth | `src/integrations/ableton/StemLabRemote/__init__.py:74` | Write-only members, duplicated token, and README/docstring omit tempo and MIDI commands | trivial | fixed (#128) |
| P9-5 | low | bug | `src/integrations/ableton/StemLabRemote/__init__.py:472` | Reply writers are unguarded in the except branches and their Documents fallbacks are dead code | small | fixed (#128) |
| P9-6 | low | bug | `src/integrations/ableton/StemLabRemote/__init__.py:863` | stemlab_ableton_midi_ack.json is written by the script but never read by the plugin | medium | fixed (follow-up) |
| P9-8 | low | code-growth | `src/integrations/ableton/StemLabRemote/__init__.py:253` | Clip-request dedupe targets a legacy fallback removed in 0124690; dict never pruned | small | fixed (#128) |
| P9-9 | low | code-growth | `src/integrations/ableton/StemLabRemote/__init__.py:163` | stemlab_toggle_transport handler has never had a sender | trivial | fixed (#128) |

### Setup and build scripts

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| P10-1 | high | bug | `scripts/linux/build.sh:147` | Generated install.sh ignores STEMLAB_INSTALL_DIR: custom installs lose their Engine and can delete the default one | small | fixed (#128) |
| C11-3 | medium | overhead | `scripts/linux/build_plugin.sh:228` | Release linux-plugin job builds seven test executables (one LTO-linked against the plugin) it never runs | small | fixed (#128) |
| P10-10 | low | code-growth | `scripts/win/verify_windows_backend.py:69` | verify_windows_backend.py --metadata-file / write_metadata is never invoked | trivial | fixed (#128) |
| P10-11 | low | code-growth | `scripts/linux/build_plugin.sh:10` | --help output of build_plugin.sh and install_backend.sh prints stale pre-scripts/linux paths; --juce-version undocumented | trivial | fixed (#128) |
| P10-2 | low | bug | `scripts/linux/update.sh:133` | update.sh release lookup is curl-only, so wget-only hosts get a false network error | small | fixed (#128) |
| P10-3 | low | bug | `scripts/linux/install_backend.sh:321` | Reused Engine never picks up a bumped CPython pin (READY_MARKER carries no version) | small | fixed (#128) |
| P10-5 | low | bug | `scripts/win/install_ableton.ps1:162` | install_ableton.ps1 can nest a second bundle inside a locked StemLab.vst3 and still pass verification | trivial | fixed (#128) |
| P10-7 | low | code-growth | `scripts/linux/build.sh:232` | build.sh re-runs a subset of the Engine check install_backend.sh just performed | trivial | fixed (#128) |
| P10-9 | low | code-growth | `scripts/win/windows_backend.ps1:7` | Suffix entries in Get-StemLabBackendConfiguration have no consumer besides their own test | trivial | fixed (#128) |

### Tests

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| X5-1 | medium | performance | `tests/test_model_compile.py:140` | Eight test_model_compile tests each regenerate the 25 s warm-up WAV via a 1.1M-iteration Python loop (~8-10 s) | trivial | fixed (#128) |
| X5-2 | medium | performance | `tests/test_dsp_edge_lengths.py:45` | Slowest test (7.5 s here) runs a 508150-frame hybrid case that is a single 16 s chunk; TAIL_CRASHING_LENGTHS is dead | small | fixed (#128) |
| X5-5 | medium | code-growth | `tests/test_model_manager.py:229` | Empty tracked test module, a 5 s probe test that cannot fail, and tautological assertions | small | fixed (#128) |
| X5-10 | low | code-growth | `tests/test_beat_tempo_quantisation.py:22` | Five near-identical synthetic beat-grid generators in test_beat_tempo_quantisation.py | small | fixed (#128) |
| X5-3 | low | performance | `tests/test_beat_this_download.py:44` | Six 0.50 s HTTPServer teardowns in test_beat_this_download (3 s) from default serve_forever poll interval; socket never closed | trivial | fixed (#128) |
| X5-4 | low | code-growth | `tests/test_stem_ready.py:31` | Test helpers duplicated across files (stem writers, fake backends, demucs stubs, BeatAnalysis fakes, child env) belong in conftest.py | medium | fixed (#128) |
| X5-8 | low | bug | `tests/test_recursive_sample_rate.py:220` | Two tests read source files via cwd-relative paths; loud_pair seeds its RNG with a salted str hash | trivial | fixed (#128) |
| X5-9 | low | code-growth | `tests/test_device_resolution.py:13` | Per-file autouse fixtures duplicate conftest.py's STEMLAB_ANALYSIS_HOME isolation | trivial | fixed (#128) |

### Repository files

| ID | Sev | Kind | Location | Finding | Fix | Status |
|---|---|---|---|---|---|---|
| P2-6, X2-4 | low | code-growth | `pyproject.toml:21` | soxr is declared twice in [project] dependencies with different floors | trivial | fixed (#128) |
| P5-9 | low | code-growth | `docs/development.md:238` | Module map row for analysis_cache.py still lists corrections and a MIDI cache | trivial | fixed (#128) |
| X2-10 | low | code-growth | `docs/development.md:227` | docs/development.md module table duplicates two rows and omits paths.py | trivial | fixed (#128) |
## Plausible

> **Triaged, not worked.** These 57 were read again after the confirmed
> fixes landed. Four turned out to be worth taking straight away and are
> marked `fixed`; four more had already been made moot by that work and are
> marked `already fixed` - the lazy-import change removed the scipy startup
> cost (P1-3, P3-3, P2-8) and the duplicate `soxr` pin was consolidated
> (P2-7). The rest are marked `open`.
>
> `open` is a judgement, not a backlog: every one is low severity, and the
> bigger ones among them - the audio-thread locking in C1-4/X1-3 and X1-9,
> the CUDA out-of-memory fallback in P5-11, the Windows download retry in
> P10-6 - want a DAW, a GPU or a Windows machine to change safely, which
> this container has none of. P9-3 was deliberately left: dropping
> `SO_REUSEADDR` is the right fix on paper, but it changes bind semantics
> inside Live and a bridge that fails to rebind after a reload is worse than
> the two-listener case it prevents.



### Plugin: processor

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| C1-4, X1-3 | low | bug | `src/plugin/Source/PluginProcessor.cpp:7744` | auditionMidi copies and sorts the whole take under midiAuditionLock, which the audio thread takes in renderMidiAudition; renderMidiAudition also frees midiAuditionId on the audio thread | small | open |
| C1-5 | low | bug | `src/plugin/Source/PluginProcessor.h:973` | currentSampleRate is a non-atomic double shared across host/message/audio threads; the currentInputChannels stores at 1888 and 2203 are dead | trivial | open |
| C1-6, C3-4 | low | performance | `src/plugin/Source/PluginProcessor.cpp:2093` | isAbletonHost() constructs juce::PluginHostType on every call, ~4 times per 20 Hz editor tick in non-REAPER hosts | trivial | open |
| C1-7 | low | bug | `src/plugin/Source/PluginProcessor.cpp:6147` | Stale per-instance torchCompileEnabled copies re-export a process-wide env var; project-state fight claim is wrong | small | open |
| C1-8, X1-7 | low | performance | `src/plugin/Source/PluginProcessor.cpp:2060` | Redundant full-buffer previewScratch.clear() every processBlock | trivial | open |
| C2-1 | low | bug | `src/plugin/Source/PluginProcessor.cpp:3157` | Stopping input/host capture flushes the ThreadedWriter FIFO synchronously on the message thread | medium | open |
| C2-2 | low | bug | `src/plugin/Source/PluginProcessor.cpp:3691` | Processor guards use capturing.load() instead of isCapturing(), so the system-capture flush window counts as idle | trivial | open |
| C2-3 | low | bug | `src/plugin/Source/PluginProcessor.cpp:3455` | createJobDirectory can hand two same-second launches the same folder | trivial | fixed |
| C4-9 | low | bug | `src/plugin/Source/PluginProcessor.cpp:7242` | A model-job completion refresh is silently dropped when an inventory is already running | small | open |
| C6-4 | low | performance | `src/plugin/Source/PluginProcessor.cpp:8138` | Repeat drags re-render every selected stem's loop-region WAV instead of reusing an unchanged render | small | open |
| X1-9 | low | bug | `src/plugin/Source/PluginProcessor.cpp:3079` | currentSampleRate is a non-atomic member written by startThreadedInputCapture on the message thread and read by renderMidiAudition on the audio thread | small | open |

### Plugin: editor

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| C5-6 | low | bug | `src/plugin/Source/PluginEditor.cpp:1076` | Sub-threshold sweep that clears nothing leaves the live selection tint until the next full repaint | trivial | open |
| C7-11 | low | bug | `src/plugin/Source/PluginEditor.cpp:4440` | Linux revealJobFolder leaves the file manager with a closed stdout/stderr pipe and never reaps it | small | open |
| C7-4 | low | performance | `src/plugin/Source/PluginEditor.cpp:4090` | getRecursiveStemItems() is fetched nine times per refresh despite the 'one fetch per tick' intent | small | open |
| X4-6 | low | overhead | `src/plugin/Source/PluginEditor.cpp:4237` | Per-tick job-root stats and source-name shaping for values that change only on user action | small | open |

### Plugin: other, build and CI

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| C10-1 | low | performance | `src/plugin/Source/StemLabAccent.h:276` | accents::swatch() rotates the whole ten-step ramp to return one colour, on every SwatchRow paint | trivial | open |
| C10-6 | low | code-growth | `src/plugin/Source/StemLabTheme.h:434` | Header palette-button tokens and comment describe a control that moved to Settings; thumbnail tokens outlive AudioThumbnail | trivial | open |
| C11-1 | low | performance | `src/plugin/Source/StemLabPaths.cpp:58` | getJobRootDirectory() fallback re-reads ~/.config/user-dirs.dirs on every editor tick until a job root is set | trivial | open |
| C11-5 | low | bug | `.github/workflows/release.yml:316` | Release never verifies that @RELEASE_URL@/@VERSION@ were actually substituted in the setup scripts | trivial | open |
| C12-2 | low | code-growth | `src/plugin/Source/ReaperBridge.cpp:17` | readTrackName/readTakeName and the two P_NAME set blocks duplicate the same buffer plumbing | small | open |
| C9-3 | low | performance | `src/plugin/Source/StemLabWidgets.cpp:527` | SeparateSplitControl repaints on every mouse move and re-measures the constant "Refine" label per paint | small | open |

### Python engine

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| P1-2 | low | overhead | `src/stemlab/hybrid.py:92` | _fuse_channels peaks at 238 MB per 16 s chunk, but most of it is scipy stft scratch, not the held intermediates | small | open |
| P1-3, P3-3 | low | overhead | `src/stemlab/hybrid.py:13` | scipy.signal (~0.75 s warm) is imported at job startup via hybrid and refinement although first used minutes later | small | already fixed |
| P1-5 | low | bug | `src/stemlab/hybrid.py:455` | Cancellation during fusion waits for both in-flight stems to finish (CLI only); fuse_stem_pair never checks abort | small | open |
| P2-2 | low | performance | `src/stemlab/demucs_backend.py:150` | Demucs copies the input into its scratch directory and RoFormer copies before resampling, neither copy being needed | small | open |
| P2-3 | low | performance | `src/stemlab/pretrained.py:123` | FLAC to WAV staging decodes the whole track into one int32 array instead of streaming | trivial | open |
| P2-5 | low | bug | `src/stemlab/paths.py:59` | STEMLAB_RECURSIVE_MODEL_DIR is not expanduser'd in paths.py but is in model_manager's mirror | trivial | fixed |
| P2-8 | low | overhead | `src/stemlab/cli.py:8` | cli.py imports pipeline (scipy.signal, ~0.9 s) at module load, taxing stemlab-models which only spawns a subprocess | trivial | already fixed |
| P2-9 | low | code-growth | `src/stemlab/bs_roformer_cli.py:23` | bs_roformer_cli.main duplicates run_console_entry's load/call/exit-status body to insert arm_torch_compile | small | open |
| P3-2 | low | performance | `src/stemlab/runtime.py:519` | Every tqdm redraw frame emits a STEMLAB_ETA line; eta() is never deduplicated | small | open |
| P3-5 | low | overhead | `src/stemlab/device.py:178` | CUDA probe cached under the raw 'gpu' alias instead of 'cuda' | trivial | open |
| P3-6 | low | bug | `src/stemlab/plugin_job.py:241` | Unguarded notify_ableton() can fail a job whose deliverables are already written | trivial | fixed |
| P4-6, X3-4 | low | performance | `src/stemlab/model_manager.py:543` | status() sizes every cache with a two-stat-per-file rglob on each editor open | small | open |
| P4-8 | low | bug | `src/stemlab/model_manager.py:1013` | --download ids dropped under --download-missing; unknown --delete-cache ids rejected only after other work has run | small | open |
| P5-11 | low | bug | `src/stemlab/beat_tracking.py:951` | CUDA-to-CPU fallback covers model loading only; OOM during spectrogram or inference aborts the job | medium | open |
| P5-2 | low | bug | `src/stemlab/beat_tracking.py:306` | Concurrent downloads of one checkpoint share a fixed .partial path opened 'wb' | trivial | fixed |
| P5-4 | low | bug | `src/stemlab/beat_tracking.py:205` | Per-user download fallback for Beat This! weights exists only on Windows; docstring claims otherwise | small | open |
| P5-5 | low | code-growth | `src/stemlab/beat_tracking.py:746` | Whole-track dominant-grid sweep computed twice per analysis | trivial | open |
| P6-7 | low | code-growth | `src/stemlab/midi.py:616` | Drag MIDI is re-serialised instead of copied from the identical export just written | trivial | open |
| P7-1 | low | bug | `src/stemlab/recursive.py:164` | _load_model purges a healthy cached model and re-downloads on any unrelated load failure (cancel-restart claim does not hold) | small | open |
| P7-3 | low | performance | `src/stemlab/adaptive/foreground.py:182` | backing_sum accumulator is redundant: backing_audio == audio - foreground_audio by construction | small | open |
| P7-4 | low | bug | `src/stemlab/recursive.py:425` | Post-separation assess_children probes the parent with unguarded sf.info, undoing _source_rate_and_frames' tolerance on the model split paths | trivial | open |
| P7-9 | low | code-growth | `src/stemlab/recursive.py:500` | split_drums, split_vocals and deverb_vocal duplicate one model-split procedure | medium | open |
| P8-6 | low | performance | `src/stemlab/refinement/adaptive_cancel.py:83` | Frame clamp keeps the full overlap when the region is shorter than n_fft, collapsing hop to 1 at sample rates below ~10.9 kHz | trivial | open |
| X2-5 | low | code-growth | `src/stemlab/pipeline.py:215` | separate()/refinement knobs (demucs_model, refinement_config, min_interval_ms, kick_targets) are never set by any caller | small | open |
| X2-6, X3-8 | low | code-growth | `src/stemlab/midi.py:408` | Duplicated SHA-256 file hash and atomic-JSON idiom in midi/beat_tracking/source_analysis | small | open |
| X2-7 | low | code-growth | `src/stemlab/model_manager.py:73` | Pass-through wrappers (managed_analysis_dir, default_model_dir, model_manager.inductor_cache_dir/_torch_version) and a two-line launcher duplicate; _recursive_model_dir is not a pure duplicate | small | open |
| X3-5 | low | overhead | `src/stemlab/device.py:125` | Once per cache miss the supervisor imports torch for the GPU probe and keeps it resident for the job | small | open |
| X3-6 | low | performance | `src/stemlab/source_analysis.py:228` | Whole-track HPSS, tuning and bass CQT computed although key scoring uses at most 240 s of windows | medium | open |

### Ableton remote script

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| P9-11 | low | code-growth | `src/integrations/ableton/StemLabRemote/__init__.py:842` | Py2/Live-10 shims are unreachable in a Py3-only, Live-11+ script | trivial | open |
| P9-3 | low | bug | `src/integrations/ableton/StemLabRemote/__init__.py:123` | SO_REUSEADDR on the UDP listener lets a second listener bind 39277 silently instead of hitting the bind-failure status path | trivial | open |
| P9-4 | low | bug | `src/integrations/ableton/StemLabRemote/__init__.py:163` | UDP thread calls schedule_message/log_message on the ControlSurface off Live's main thread | small | open |

### Setup and build scripts

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| P10-12 | low | bug | `scripts/win/build_plugin.ps1:55` | Launch-VsDevShell.ps1 called without -SkipAutomaticLocation leaves the calling process cwd in the VS install directory | trivial | open |
| P10-6 | low | bug | `scripts/win/StemLab-Windows-setup.ps1:104` | Windows setup downloads have no retry, unlike the Linux twin and build_plugin.ps1 | small | open |
| P10-8 | low | code-growth | `scripts/linux/install_backend.sh:501` | onnx-weekly pin literal duplicated in Linux installer and Windows constraints with no sync tripwire | trivial | open |

### Tests

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| X5-11 | low | code-growth | `tests/test_fusion_normalisation.py:126` | CLI flag plumbing and recursive call-count tests are pinned to source text | small | open |

### Repository files

| ID | Sev | Kind | Location | Finding | Fix | Triage |
|---|---|---|---|---|---|---|
| P2-7 | low | overhead | `pyproject.toml:30` | pyyaml and tqdm declared as direct dependencies without any first-party import (and soxr is declared twice) | trivial | already fixed |
## Refuted

| ID | Location | Claim | Why it does not hold |
|---|---|---|---|
| C11-6 | `src/plugin/Source/ModelManagerPanel.cpp:431` | Inventory digest omits row strings, but no omitted string can change without a digested field changing | The digest does omit label/purpose/path/compileReason/approxBytes/warning, but none can change independently of a digested field within a session. From model_manager.status(): label, purpose, approxBytes, compile_note, cache label/path and every cache warning... |
| C12-4 | `src/plugin/Source/ReaperBridge.cpp:300` | insertStemTracks leaves empty tracks when nothing could be inserted | The cited scenario is filtered upstream: PluginProcessor.cpp:5689 keeps a node only if `selected && file.existsAsFile()` or a kept descendant exists, and 5708 substitutes File() only for such nodes - i.e. a File() entry is always a folder parent whose... |
| C2-10 | `src/plugin/Source/PluginProcessor.cpp:2564` | ensureStemMixLoaded re-probes stems on every UI tick when the mix cannot be built | Mechanism exists but the cost is marginal and the fix adds risk. refreshStemMixIfNeeded runs from the editor timer, which idles at uiIdleRefreshHz = 2 Hz (StemLabTheme.h:788) when nothing changes. On the failure path getCompletedStemFile is already cached... |
| C3-5 | `src/plugin/Source/PluginProcessor.cpp:4483` | Progress file is re-read on every UI tick during the main job | The read happens as described (cpp:4477-4495 on every 20 Hz tick while the main engine runs), but the cost is two stats plus a ~30-byte page-cached read - microseconds per tick, negligible next to the Demucs job. The premise that the file changes 'a handful... |
| C4-10 | `src/plugin/Source/PluginProcessor.cpp:6183` | Legacy waveformColour/waveformColor state keys are still parsed and cost a file stat on every setStateInformation | The block is a documented compatibility contract (docs/development.md:189 describes exactly this carry-forward) and the comment at 6173-6182 explains why getStateInformation stopped writing the key. Cost is one stat per setStateInformation, which runs once... |
| C4-7 | `src/plugin/Source/PluginProcessor.cpp:7636` | finishMidiConversion misreports a zero-note transcription as failure and leaves the sidecar JSON behind | The zero-note case never reaches finishMidiConversion with exitCode 0: create_transcription raises RuntimeError('No sufficiently confident notes...') at midi.py:403, main() re-raises, so the worker exits non-zero and 'conversion failed - see diagnostics' is... |
| C4-8 | `src/plugin/Source/PluginProcessor.cpp:6904` | setenv from setStateInformation/setTorchCompileEnabled can race child-process launches on worker threads | The environ-array realloc that fork/execvp could observe only happens when setenv inserts a NEW name. STEMLAB_TORCH_COMPILE is first inserted in the constructor (line 1799, after STEMLAB_PARENT_PID at 654) before this instance has any engine/utility thread;... |
| C5-11 | `src/plugin/Source/PluginEditor.cpp:1183` | setRepaintsOnMouseActivity(true) redundant with updateHover() - negligible cost | Mechanism partly right but impact does not hold. setRepaintsOnMouseActivity only fires when the lane itself is the event target, and the constructor comment notes almost none of the lane is the lane (well and buttons cover it). On the lane's own enter/exit,... |
| C6-5 | `src/plugin/Source/PluginEditor.cpp:1830` | Constructor re-probes the model inventory on every editor open (intentional: keeps auto-show and Model Manager fresh) | The constructor probe is deliberate and needed. refreshModelInventory is only otherwise called from the Models page open (5204, whose comment says the inventory is cheap and must be re-asked because models change outside StemLab) and from model-job completion... |
| C7-7 | `src/plugin/Source/PluginEditor.cpp:5222` | Settings panel refresh recomputes cheap, deduped state every tick while visible | Mechanism is as described: refreshFromProcessor() (line 3750) calls refreshSettingsPanel() every tick while the panel is visible, which stats update.sh on Linux, copies the tempo segments and model/cache vectors, rebuilds the digest and calls... |
| C7-9 | `src/plugin/Source/PluginEditor.cpp:3684` | Both record buttons repainted per tick while capturing | The code does repaint both buttons per tick while capturing (3684-3685), and only the button with recording==true animates (RecordButton::paintButton, StemLabWidgets.cpp:316). But the surplus is one repaint() of a small button rectangle at 20 Hz for the... |
| P10-4 | `scripts/win/StemLab-Windows-setup.ps1:134` | Flavor case-insensitivity in Windows setup has no user-visible effect | The mechanism (case-insensitive -contains, asset name built from user spelling) is accurate, but the consequences are not: GitHub release asset URLs are case-insensitive - verified live: .../releases/download/20250818/SHA256SUMS, sha256sums and Sha256Sums all... |
| P4-9 | `src/stemlab/model_manager.py:719` | process.wait() after terminate() lacks a kill fallback (children do not ignore SIGTERM, so no hang in practice) | The premise - a child that ignores SIGTERM - does not hold for these children. All three are plain Python processes (`-m stemlab.bs_roformer_download_cli` runs the console entry in-process; demucs and audio-separator via `-c`). Neither src/stemlab nor those... |
| P5-3 | `src/stemlab/model_manager.py:247` | Duplicate candidate-directory list is deliberate (stdlib-only status probe) and test-guarded | The duplication is a documented, deliberate decision: model_manager.py:205-216 states locate()/status must work when the engine's optional deps are broken, and importing beat_tracking costs numpy (it does `import numpy as np` at module top, line 18). The... |
| P5-6 | `src/stemlab/analysis_cache.py:101` | Unconditional schema-version write on every cache open is negligible per-process overhead | Mechanism is as described - the unconditional INSERT OR REPLACE at analysis_cache.py:101-104 is a real write (journal + fsync) on every AnalysisCache() construction, and nothing but the test reads the 'schema' row. But AnalysisCache is only instantiated by... |
| P6-3 | `src/stemlab/source_analysis.py:316` | Bass-role full-track chroma_cqt duplicates windowed CQT bins | The mechanism is real (windowed CQT starts at C1 with 36 bins/octave, so bins 0-143 cover what the bass chroma recomputes), but the cost is not: it runs once per bass stem, is cached by signal hash via _cached_tonal_evidence, and a 144-bin hop-2048 CQT of a... |
| P7-10 | `src/stemlab/recursive.py:254` | _conform_sample_rate does not pin length when the rate already matches | Checked audio-separator 0.47.0 (downloaded wheel, not installed). mdxc_separator.demix slices the MDX-path result back to the input length (`accumulated_outputs[..., chunk_size-hop_size : -(pad_size+chunk_size-hop_size)]`, line 656); the roformer path... |
| P7-5 | `src/stemlab/recursive.py:668` | Repeated analyse_audio/_excerpt_mono calls in split_lead_group cost <5% of the job | The redundancy exists but the count and cost are overstated. With two accepted passes the unique files are mix, fg1, bg1, fg2, bg2 (the reporter's 4x per name comes from counting foreground_pass_1 and foreground_pass_2 files by basename): 11 analyses for 6... |
| P9-7 | `src/plugin/Source/PluginProcessor.cpp:5171` | Remote status file is a load marker, not a heartbeat - plugin already treats it that way | Mechanism as described exists (status written at init/disconnect/import end, plugin accepts active && age<24h), but the plugin comment at PluginProcessor.cpp:5171 explicitly treats it as an 'installed/loaded recently' indication, not liveness. The VST3 runs... |
| X1-8 | `src/plugin/Source/PluginProcessor.cpp:3691` | launchSeparationAndExport/transportTogglePlay gate on `capturing` rather than isCapturing() (masked by editor enablement) | The processor-level gates do use raw `capturing` (3691, 2341, 2801) while isCapturing() adds isSystemCaptureStopPending(), so the model layer is inconsistent with the header comment. But the scenario is not reachable: the only callers are the editor... |
| X4-9 | `src/plugin/Source/ModelManagerPanel.cpp:546` | ModelManagerPanel::setActivity's extra activityBar.repaint() and drawProgressBar's DropShadow are negligible | The repaint is a no-op while the bar is hidden (Component::internalRepaintUnchecked checks flags.visibleFlag), so it only fires while a model job runs. Model jobs stream through handleEngineOutputLine (StemLabUtilityThread kind modelMaintenance,... |

## Superseded during the analysis

| ID | Location | Claim at base commit | What changed |
|---|---|---|---|
| C8-1, X2-1 | `src/plugin/Source/WaveformGrid.h:119` | Beat-grid line API in WaveformGrid.h is dead: editor draws its grid with its own inline loop | Superseded after the analysis snapshot: PR #126 (merged 2026-09-02, after base commit ba8cad8) routes the lane grid through makeGridLines/GridRequest and feeds it the detected beats, so the beat-grid API and the StemLabGridInfo beat vectors are now live. The three HostIntegrationPolicy text helpers named here remain unused (see C8-4). |

## Coverage

Twenty-seven review units. Each unit's files were read in full by one reviewer and every finding was then handed to an independent verifier with instructions to refute it.

| Unit | Scope | Candidates | Confirmed | Plausible | Refuted |
|---|---|---:|---:|---:|---:|
| P1 | Python core: pipeline / audio / resample / hybrid | 8 | 5 | 3 | 0 |
| P2 | Python backends & launchers: pretrained / demucs_backend / cli / console_entry / bs_roformer_* / paths / pyproject | 9 | 3 | 6 | 0 |
| P3 | Python job bridge: runtime / plugin_job / recursive_job / device / compile_support / model_compile | 8 | 4 | 4 | 0 |
| P4 | Python model manager | 9 | 6 | 2 | 1 |
| P5 | Python beat tracking & analysis cache | 11 | 5 | 4 | 2 |
| P6 | Python source analysis & MIDI | 7 | 5 | 1 | 1 |
| P7 | Python recursive/adaptive splitting | 10 | 4 | 4 | 2 |
| P8 | Python refinement & regression harness | 11 | 10 | 1 | 0 |
| P9 | Ableton Live remote script | 12 | 8 | 3 | 1 |
| P10 | Setup, build and installer scripts | 12 | 8 | 3 | 1 |
| X2 | Cross-cutting: dead code and duplication across the repository | 10 | 6 | 3 | 0 |
| X3 | Cross-cutting: Python child-process startup and repeated work | 9 | 5 | 4 | 0 |
| X5 | Cross-cutting: test suite and CI/release bloat | 11 | 10 | 1 | 0 |
| C8 | Waveform helpers and header-only components | 6 | 5 | 0 | 0 |
| C9 | Widgets and LookAndFeel | 6 | 5 | 1 | 0 |
| C10 | Theme, accent and settings panel | 10 | 8 | 2 | 0 |
| C11 | Model manager panel, paths, CMake and CI | 10 | 7 | 2 | 1 |
| C12 | REAPER bridge/host and Linux system capture | 7 | 5 | 1 | 1 |
| X1 | Cross-cutting: real-time safety and threading in the plugin | 9 | 5 | 3 | 1 |
| X4 | Cross-cutting: UI rendering overhead | 11 | 9 | 1 | 1 |
| C1 | PluginProcessor part 1: header + audio path, capture, preview (lines 1-2110) | 12 | 7 | 5 | 0 |
| C2 | PluginProcessor part 2: preview, transport, jobs (lines 2111-4029) | 10 | 6 | 3 | 1 |
| C3 | PluginProcessor part 3: recursive jobs, manifests, exports (lines 4030-6072) | 9 | 7 | 1 | 1 |
| C4 | PluginProcessor part 4: state, preferences, engine thread, remote (lines 6073-8160) | 11 | 7 | 1 | 3 |
| C5 | PluginEditor part 1: header + lane components (lines 1-1760) | 11 | 9 | 1 | 1 |
| C6 | PluginEditor part 2: editor construction and layout (lines 1761-3494) | 7 | 5 | 1 | 1 |
| C7 | PluginEditor part 3: timers, refresh, footer (lines 3495-5272) | 12 | 8 | 2 | 2 |
