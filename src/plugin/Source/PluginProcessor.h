#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>

#include "HostIntegrationPolicy.h"
#include "LoopQuantize.h"
#include "LoopRegions.h"
#include "WaveformCache.h"

namespace stemlab::reaper
{
struct Api;
struct MediaItem;
class PeakBuilder;
}

class StemLabEngineThread;
class StemLabRecursiveThread;
class StemLabStemMixSource;
class StemLabUtilityThread;

#if JUCE_WINDOWS || JUCE_LINUX
class StemLabSystemLoopbackThread;
#endif

/** One node in the adaptive stem tree returned by Python's schema-2 manifest. */
struct StemLabRecursiveStemInfo
{
    // Mirrors each child entry in stemlab.recursive schema-2 manifests.
    juce::String id;
    juce::String label;
    juce::String parentId;
    juce::String rootStem;
    juce::String category;
    juce::File file;
    juce::StringArray actions;
    bool selected = true;
    bool hasChildren = false;
    int depth = 1;
    int estimatedSourceCount = 1;
    double confidence = 0.0;
};

struct StemLabKeyCandidate
{
    juce::String key;
    double probability = 0.0;
};

struct StemLabMidiNoteInfo
{
    double start = 0.0;
    double end = 0.0;
    int pitch = 60;
    int velocity = 100;
    double confidence = 1.0;
};

struct StemLabMidiInfo
{
    juce::String id;
    juce::String sourceStem;
    juce::File midiFile;
    juce::File dragFile;
    std::vector<StemLabMidiNoteInfo> notes;
    double sourceTempo = 120.0;
    double barOne = 0.0;
    bool drums = false;
};

struct StemLabGridInfo
{
    int mode = 1;
    double bpm = 120.0;
    int numerator = 4;
    int denominator = 4;
    double barOne = 0.0;
    double captureStartPpq = 0.0;
    std::vector<double> beats;
    std::vector<double> downbeats;
};

struct StemLabSelectionRange
{
    double start = 0.0;
    double end = 1.0;
    bool active = false;

    double length() const noexcept { return juce::jmax(0.0, end - start); }
};

/**
 * Per-lane monitoring state, shared between the processor and the stem-mix
 * source that reads it on the audio thread.
 *
 * Held by shared_ptr so the audio thread keeps reading valid flags through a
 * mix it is still playing while the message thread rebuilds the lane map
 * behind it - an adaptive split can land at any moment.
 */
struct StemLabLaneMonitorFlags
{
    std::atomic<bool> solo{false};
    std::atomic<bool> mute{false};
};

/**
 * Owns StemLab's audio capture, preview player, Python jobs, and Ableton bridge.
 *
 * JUCE calls the AudioProcessor overrides on audio/host threads. The editor calls
 * the remaining public methods from the message thread. Expensive separation is
 * always delegated to a child Python process.
 */
/** One stretch of a track that a single constant tempo explains. */
struct StemLabTempoSegment
{
    double start = 0.0;
    double end = 0.0;
    double bpm = 0.0;
};

class StemLabAudioProcessor final : public juce::AudioProcessor,
                                    public juce::ChangeBroadcaster,
                                    public juce::AudioSource,
                                    public juce::VST3ClientExtensions
{
public:
    enum StandaloneRecordingMode
    {
        recordingNone = 0,
        recordingInput = 1,
        recordingSystem = 2,
        recordingHost = 3
    };

    StemLabAudioProcessor();
    ~StemLabAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    // AudioSource callbacks used only by the Standalone preview player.
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "StemLab"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    /**
     * Load the source used by the next separation job.
     *
     * @param file Audio file to load.
     * @param startPpq Arrangement beat where Ableton should place output stems.
     * @param sourceLabel Optional user-facing description of the source.
     * @return true when JUCE can read the file.
     */
    bool setInputAudioFile(const juce::File& file, double startPpq = 0.0,
                           const juce::String& sourceLabel = {});

    bool setStandaloneInputFile(const juce::File& file);
    bool isStandaloneApp() const noexcept;
    bool isAbletonHost() const noexcept;
    stemlab::host::UiMode getHostUiMode() const noexcept;

    /**
     * Which host-integration path this instance can use.
     *
     * REAPER is detected at runtime: the VST3 wrapper hands over the host
     * context during initialisation, and if it answers to
     * IReaperHostApplication the whole ReaScript API is available in-process.
     * Ableton Live has no such API, so its Windows-only UDP + Remote Script
     * bridge stays the assumption for any other Windows host. Everywhere else
     * the plugin offers the same local-file workflow as the Standalone app.
     */
    enum HostIntegration
    {
        hostIntegrationNone = 0,
        hostIntegrationAbletonLive,
        hostIntegrationReaper
    };

    HostIntegration getHostIntegration() const noexcept;

    bool usesLocalFileWorkflow() const noexcept
    {
        return isStandaloneApp() || getHostIntegration() == hostIntegrationNone;
    }

    // juce::VST3ClientExtensions
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
    void setIHostApplication(Steinberg::FUnknown* host) override;

    /**
     * REAPER bridge. Both are message-thread only - the ReaScript API has no
     * other home - and both no-op with a status update if REAPER stopped
     * providing what they need.
     */
    bool requestReaperSourceItem();
    bool insertSelectedStemsIntoReaper();

    /**
     * True when the file is one of the current job's outputs. Dropping such
     * a file back onto the editor (easily done by cancelling an outbound
     * stem drag over the window) must not replace the source and wipe the
     * completed job.
     */
    bool isFileFromCurrentJob(const juce::File& file) const;

    /** Ask StemLabRemote for Ableton's selected Arrangement audio clip. */
    bool requestAbletonSourceClip();
    void refreshAbletonSourceClipFromDisk();

    bool isAwaitingAbletonSourceClip() const noexcept { return abletonClipRequestPending.load(); }

    juce::String getInputSourceLabel() const;

    /** Start/stop recording the Standalone app's selected physical input. */
    bool startStandaloneRecording();
    void stopStandaloneRecording();

    /** Explicitly capture VST3 input audio in non-Ableton, non-REAPER hosts. */
    bool startHostAudioCapture();
    void stopHostAudioCapture();
    bool isHostAudioCapturing() const noexcept
    {
        return standaloneRecordingMode.load() == recordingHost && capturing.load();
    }

    /**
     * Start/stop system-audio recording of the default output: WASAPI
     * loopback on Windows, the PulseAudio/PipeWire monitor source on Linux.
     * Platforms without a backend hide the control instead of offering a
     * button that always fails.
     */
    static constexpr bool isSystemAudioCaptureSupported() noexcept
    {
#if JUCE_WINDOWS || JUCE_LINUX
        return true;
#else
        return false;
#endif
    }

    bool startSystemAudioRecording();
    void stopSystemAudioRecording();

    int getStandaloneRecordingMode() const noexcept { return standaloneRecordingMode.load(); }

    void toggleStandalonePlayback();
    void stopStandalonePlayback();

    /**
     * Source analysis: tempo, key, and the beat grid the waveform draws.
     *
     * Fast is the default because Accurate loads the Beat This! model; the
     * tempo interpretation lets the user overrule an octave-doubled BPM
     * without re-running anything.
     */
    enum SourceAnalysisMode
    {
        analysisAccurate = 0,
        analysisFast = 1
    };

    enum TempoAnalysisMode
    {
        // One tempo for the whole track, which is what a host's tempo
        // field takes. Dynamic reports each stretch that holds its own.
        tempoStatic = 0,
        tempoDynamic = 1
    };

    enum TempoInterpretation
    {
        tempoHalf = 0,
        tempoDetected = 1,
        tempoDouble = 2
    };

    /** Snapping step for a swept loop range. Ordered as the settings row
        reads, and matching stemlab::quantize::Resolution one for one. */
    enum LoopQuantizeMode
    {
        quantizeOff = 0,
        quantizeQuarterBeat = 1,
        quantizeHalfBeat = 2,
        quantizeBeat = 3,
        quantizeBar = 4
    };

    enum WaveformGridMode
    {
        gridHost = 0,
        gridSource = 1,
        gridManual = 2,
        gridOff = 3
    };

    /**
     * The shared monitoring transport behind the Lanes interface: one clock
     * driving either the untouched source ("Original") or a live mix of the
     * completed stems ("Stems") that honours per-lane solo/mute.
     *
     * The mix plays the tree's leaves: a root stem that was split further
     * is represented by its children rather than by itself, so nothing is
     * ever heard twice, and every lane in the interface - root or adaptive
     * child - has real solo and mute over what it contributes.
     *
     * All of these are message-thread calls; the audio thread only reads
     * the atomics they publish.
     */
    enum MonitorMode
    {
        monitorOriginal = 0,
        monitorStems = 1
    };

    void setMonitorMode(int mode);
    int getMonitorMode() const noexcept { return monitorMode.load(); }

    /** True once the completed job's stems can be mix-monitored. */
    bool isStemMonitorAvailable();

    /**
     * Rebuild the stem mix if the adaptive tree changed under it.
     *
     * A split finishing gives some lanes children, which take over their
     * parent's place in the mix. The editor's refresh calls this so the
     * change reaches the monitor without waiting for the user to touch the
     * A/B control.
     */
    void refreshStemMixIfNeeded();

    void transportTogglePlay();
    void transportSeekNormalised(double normalisedPosition);
    bool isTransportPlaying() const noexcept;
    double getTransportPositionSeconds() const noexcept;
    double getTransportLengthSeconds() const noexcept;

    void setStemSolo(int index, bool solo);
    bool isStemSoloed(int index) const;
    void setStemMute(int index, bool mute);
    bool isStemMuted(int index) const;

    /** The same solo/mute over one adaptive child stem's lane. */
    void setRecursiveStemSolo(const juce::String& itemId, bool solo);
    bool isRecursiveStemSoloed(const juce::String& itemId) const;
    void setRecursiveStemMute(const juce::String& itemId, bool mute);
    bool isRecursiveStemMuted(const juce::String& itemId) const;

    /**
     * What the monitor mix is actually doing to a lane, for the interface
     * to draw. Not the lane's own mute flag: a lane is inaudible when it is
     * muted, when an ancestor is muted, or when some other lane is soloed.
     * Answered by the loaded mix itself rather than re-derived, so the
     * picture cannot disagree with the sound. Message thread only.
     * A lane the mix does not carry reports audible, so nothing dims
     * before there is anything to hear.
     */
    bool isAnySoloActive() const;
    bool isStemAudible(int index) const;
    bool isRecursiveStemAudible(const juce::String& itemId) const;

    /*  Stays true while a stopped system capture is still flushing: the WAV
        is not finalised and has not been handed over yet, so nothing that
        consumes the recording - separation, a new take, the transport - may
        treat the plugin as idle before then.
    */
    bool isCapturing() const noexcept
    {
        return capturing.load() || isSystemCaptureStopPending();
    }

    bool isSourceAnalysisRunning() const noexcept { return sourceAnalysisRunning.load(); }

    /**
     * Any background work the status line is narrating: a separation or
     * adaptive job, source analysis (Beat This! downloads and inference,
     * cache maintenance), MIDI conversion, an active capture, or an
     * awaited Live clip. The editor's activity indicator and status
     * animation follow this rather than the engine alone, so none of
     * these runs behind an "idle" spinner.
     */
    bool isBackgroundWorkRunning() const noexcept
    {
        return isEngineRunning() || isSourceAnalysisRunning() || isMidiConversionRunning() ||
               isCapturing() || isAwaitingAbletonSourceClip() ||
               abletonBridgeWaitStartMs.load() > 0.0;
    }

    double getCapturedSeconds() const noexcept;
    juce::File getCaptureFile() const;
    double getCaptureStartPpq() const noexcept { return captureStartPpq.load(); }

    /** Launch the main six-stem Python job for the currently loaded source. */
    bool launchSeparationAndExport();

    /** Launch the default adaptive action for one completed root stem. */
    bool launchRecursiveStemSplit(int rootStemIndex);

    /** Launch an action advertised by an existing adaptive-tree node. */
    bool launchRecursiveAction(const juce::String& itemId, const juce::String& action);
    bool isRecursiveEngineRunning() const noexcept;
    std::vector<StemLabRecursiveStemInfo> getRecursiveStemItems() const;
    juce::File getRecursiveStemFile(const juce::String& itemId) const;
    void setRecursiveStemEnabled(const juce::String& itemId, bool enabled);
    bool isRecursiveStemEnabled(const juce::String& itemId) const;

    bool isEngineRunning() const noexcept;
    bool hasSuccessfulJob() const noexcept { return engineCompletedSuccessfully.load(); }

    double getEngineProgress() const noexcept { return engineProgress.load(); }

    double getEngineElapsedSeconds() const noexcept;
    double getMainJobDurationSeconds() const noexcept { return mainJobDurationSeconds.load(); }
    double getEngineEstimatedRemainingSeconds() const noexcept;
    void refreshEngineProgressFromDisk();

    /** Whether refinement was on for the job whose summary is on screen.
        The live setting belongs to the NEXT job, so quoting that one in the
        summary let a toggle afterwards rewrite what a finished job did. */
    bool wasLastJobRefined() const noexcept { return lastJobRefinement.load(); }

    /**
     * Ask the running separation (main or adaptive) to stop. Writes the
     * cancel sentinel the engine's watchdog honors - the engine shuts down
     * its own model subprocesses, which a direct kill would orphan. Engines
     * without the watchdog are killed after a short grace period.
     */
    void cancelSeparation();
    bool isCancelRequested() const noexcept { return engineCancelRequested.load(); }

    juce::String getStatus() const;

    /**
     * Whether the status line is reporting a failure rather than progress.
     * The severity travels with the string under the same lock, so an
     * ordinary setStatus resets it without any caller having to remember
     * to - the footer can never be left red next to a healthy message.
     */
    enum StatusSeverity
    {
        statusInfo = 0,
        statusFailure
    };

    StatusSeverity getStatusSeverity() const;

    /**
     * Feedback for things the user changed - model, palette, transport,
     * selection, rejected clicks. Lives in the header readout, so the main
     * status line stays reserved for the work the plugin is actually doing.
     * The revision bumps on every post, letting the editor restart its
     * display timer even when the same message repeats.
     */
    juce::String getActionStatus() const;
    int getActionStatusRevision() const;

    juce::String getEngineLog() const;

    /** Whether getEngineLog() would return anything, without paying for the
        join - the editor asks this on every menu build. */
    bool hasEngineLog() const;

    juce::File getLastJobDirectory() const;

    /** Compact key/BPM text for the currently loaded original source. */
    juce::String getSourceAnalysisText() const;
    juce::String getSourceAnalysisDetails() const;
    juce::String getSourceKey() const;
    double getSourceBpm() const noexcept { return sourceBpm.load(); }
    double getDetectedSourceBpm() const noexcept { return sourceDetectedBpm.load(); }
    /** False when the beats do not sit on one constant grid: a played or
        drifting track. The tempo is still the best single answer, but a host
        set to it will not stay aligned to the end of the track. */
    bool isSourceTempoSteady() const noexcept { return sourceTempoSteady.load(); }
    double getHalfTimeSourceBpm() const noexcept { return sourceHalfBpm.load(); }
    double getDoubleTimeSourceBpm() const noexcept { return sourceDoubleBpm.load(); }
    double getSourceBarOne() const noexcept { return sourceBarOne.load(); }
    int getSourceMeterNumerator() const noexcept { return sourceMeterNumerator.load(); }
    int getSourceMeterDenominator() const noexcept { return sourceMeterDenominator.load(); }
    void setBeatThisEnabled(bool enabled);
    bool isBeatThisEnabled() const noexcept { return beatThisEnabled.load(); }
    void setSourceAnalysisMode(int mode);
    int getSourceAnalysisMode() const noexcept { return sourceAnalysisMode.load(); }
    void setTempoAnalysisMode(int mode);
    int getTempoAnalysisMode() const noexcept { return tempoAnalysisMode.load(); }
    /** Each stretch of the source one constant tempo explains, in order. */
    std::vector<StemLabTempoSegment> getSourceTempoSegments() const;

    /** Whether Set BPM has everything it needs: an analysis to read a tempo
        from, and a host StemLab can actually write a tempo to - a REAPER new
        enough to expose the tempo calls and a source item it can still
        reach, or Live with StemLabRemote listening. */
    bool canSetHostTempo() const;
    /** Puts the analysed tempo into the host, and the source onto a timebase
        that will not follow it. Returns what to tell the user - or, for
        Live, what to tell them while the Remote Script's reply is on its
        way. */
    juce::String setHostTempo();
    /** Live's half of setHostTempo: the Remote Script answers on disk, the
        same way it answers a clip request. */
    void refreshAbletonTempoReplyFromDisk();
    void setTempoInterpretation(int interpretation);
    int getTempoInterpretation() const noexcept { return tempoInterpretation.load(); }
    bool clearAnalysisCache();

    // ------------------------------------------------------------- models

    /** One weight file the engine can need, as the Model Manager sees it. */
    struct ManagedModel
    {
        juce::String id;
        juce::String label;
        juce::String purpose;
        juce::String path;
        /** Empty when the model can be compiled; otherwise why it cannot. */
        juce::String compileReason;
        bool present = false;
        bool compiled = false;
        bool compilable = false;
        juce::int64 bytes = 0;
        /** What it will cost to fetch, for a model not on disk yet. */
        juce::int64 approxBytes = 0;
    };

    /** One directory or file the engine caches into. */
    struct ManagedCache
    {
        juce::String id;
        juce::String label;
        juce::String path;
        /** Set when clearing it costs something no download can restore. */
        juce::String warning;
        juce::int64 bytes = 0;
    };

    /** Ask the engine what is on disk. Cheap, and safe to call on open.

        @param probeCompile also ask whether this machine can compile, which
                            imports torch in the child and costs seconds. Only
                            worth it when the answer is about to be shown.
    */
    bool refreshModelInventory(bool probeCompile = false);

    bool isModelInventoryRunning() const noexcept { return modelInventoryRunning.load(); }
    bool isModelJobRunning() const noexcept { return modelJobRunning.load(); }

    /** False until the first inventory has come back, so the Model Manager
        can tell "nothing installed" apart from "nothing known yet". */
    bool hasModelInventory() const noexcept { return modelInventoryValid.load(); }

    /** True when the last inventory could not be read at all - no engine, or
        one too old to know the command. Distinct from "models are missing". */
    bool modelInventoryFailed() const noexcept { return modelInventoryBroken.load(); }

    std::vector<ManagedModel> getManagedModels() const;
    std::vector<ManagedCache> getManagedCaches() const;

    /** The auto-show condition, decided by the engine so the rule lives in
        one place. Both are false until an inventory has been read. */
    /** Whether a model the app cannot separate without is absent. */
    bool isEssentialModelMissing() const noexcept { return essentialModelMissing.load(); }

    /** Whether compiling was asked for, whether this machine can, and why
        not. Straight from the engine: an unset opt-in and a missing C++
        compiler look identical from here and need opposite advice. */
    bool isCompileRequested() const noexcept { return compileRequested.load(); }

    /** Turn compiled inference on or off for every job this plugin starts. */
    void setTorchCompileEnabled(bool enabled);

    /** Where this machine's compile preference is remembered. */
    static juce::File torchCompilePreferenceFile();
    static bool readRememberedTorchCompile();
    static void rememberTorchCompile(bool enabled);

    /*  The accent, which is how the interface looks rather than how this
        project sounds - so it belongs to the person, not the session, and
        is deliberately not in getStateInformation. Opening someone else's
        project should not restyle your editor, and reopening your own on a
        second machine should not undo the choice you made there.

        Static because the accent is one process-wide value in the theme:
        every editor in the host shares it, which is the correct behaviour
        for a preference about the look of the application.
    */
    static juce::File accentPreferenceFile();
    static int readRememberedAccent();
    static void rememberAccent(int presetIndex);

    /** Applies a preset to the theme and remembers it. */
    static void setAccentIndex(int presetIndex);
    static int getAccentIndex();
    bool isTorchCompileEnabled() const noexcept { return torchCompileEnabled.load(); }
    bool isCompileSupported() const noexcept { return compileSupported.load(); }
    juce::String getCompileReason() const;

    bool startModelDownload(const juce::StringArray& modelIds);
    bool startModelCompile(const juce::StringArray& modelIds);
    bool startModelRemoval(const juce::StringArray& modelIds, const juce::StringArray& cacheIds);

    /** Cooperative stop for whichever model job is running. */
    void cancelModelJob();

    void setWaveformGridMode(int mode) noexcept;
    int getWaveformGridMode() const noexcept { return waveformGridMode.load(); }
    void setManualGrid(double bpm, int numerator, int denominator, double barOne) noexcept;

    /** The tempo the manual grid is set to, for the editor to seed its prompt. */
    double getManualGridBpm() const noexcept { return manualGridBpm.load(); }
    StemLabGridInfo getWaveformGridInfo() const;

    /** getWaveformGridInfo without the beat vectors: every field it fills
        is backed by an atomic, so the per-lane per-tick display read pays
        no lock and no vector copies. */
    StemLabGridInfo getWaveformGridScalars() const;

    /** Waveform profiles for the lane wells. Owned here rather than by the
        editor so closing and reopening the window does not re-read and
        re-FFT every stem; the editor borrows it by reference. */
    StemLabWaveformCache& getWaveformCache() noexcept { return waveformProfiles; }

    int getWaveformLaneHeight(const juce::String& id) const;
    void setWaveformLaneHeight(const juce::String& id, int height);

    /** One highlighted time range per stem. Dragging a waveform sets it. */
    void setLoopQuantizeMode(int mode) noexcept;
    int getLoopQuantizeMode() const noexcept { return loopQuantizeMode.load(); }

    /** The rule a swept loop snaps to: the same one the lanes paint. */
    stemlab::quantize::Grid getLoopQuantizeGrid() const;

    /** False when there is no grid to snap to, whatever the setting says -
        the grid switched off, or a source with no tempo behind it. */
    bool canQuantizeLoops() const;

    /** A normalised range put onto the grid, or returned as given when the
        setting is off or no grid exists. The lane runs its live drag preview
        through this so the highlight shows where the loop will land. */
    stemlab::quantize::Range quantizeLoopRange(stemlab::quantize::Range range) const;

    StemLabSelectionRange getStemSelectionRange(const juce::String& id) const;
    void setStemSelectionRange(const juce::String& id, double start, double end);
    void clearStemSelectionRange(const juce::String& id);
    void clearAllStemSelectionRanges();

    /** The lane's stem file, trimmed to the playback loop when one is set. */
    juce::File getStemDragFile(const juce::File& source, const juce::String& selectionId);

    bool launchStemMidiConversion(int stemIndex);
    bool launchRecursiveMidiConversion(const juce::String& itemId);
    bool isMidiConversionRunning() const noexcept;
    StemLabMidiInfo getMidiInfo(const juce::String& id) const;
    bool hasMidiInfo(const juce::String& id) const;

    /** How many notes this id's conversion holds, without copying any of
        them. What the UI timer asks; getMidiInfo is for the paths that
        actually read the notes. */
    size_t getMidiNoteCount(const juce::String& id) const;
    bool auditionMidi(const juce::String& id);
    bool isMidiAuditioning(const juce::String& id) const;
    void stopMidiAudition();
    bool sendMidiToAbleton(const juce::String& id);

    void setJobRootDirectory(const juce::File& directory);
    juce::File getJobRootDirectory() const;

    /** Poll StemLabRemote's status/acknowledgement files. */
    void refreshAbletonBridgeStatusFromDisk();

    /** Ask StemLabRemote to import all currently selected completed stems. */
    bool sendSelectedStemsToAbleton();
    bool retryAbletonImport();
    juce::String getAbletonBridgeStatus() const;
    /** Publish user-action feedback from a UI callback (header readout). */
    void postUiStatus(const juce::String& message);

    /** Copy selected completed stems to a Standalone export directory. */
    int saveSelectedStemsTo(const juce::File& destination);
    /** Return selected WAVs, rendering existing waveform ranges when needed. */
    juce::StringArray getSelectedStemFilesForDrag();
    juce::File getCompletedStemFile(int index) const;

    /** Whether getCompletedStemFile(index) resolves to a file, answered
        from its scan cache rather than by another stat. */
    bool hasCompletedStemFile(int index) const;

    /**
     * The file the running separation announced for this stem, or an empty
     * File.
     *
     * The engine reports each stem as its final file lands, well before the
     * job as a whole finishes, which is what lets a lane draw its waveform
     * early. Only the running main job's own announcements ever reach here
     * (see the reset points around it), so this cannot answer with an
     * earlier job's stem.
     *
     * It carries no claim about the job: everything that acts on stems -
     * the monitor mix, the transport, save, send, drag - stays gated on
     * hasSuccessfulJob(), because a job can still be cancelled or fail
     * after announcing some of its stems.
     */
    juce::File getReadyStemFile(int index) const;

    /** Changes whenever an announcement lands or the record is reset, so a
        reader can tell one snapshot of the lanes from the next. */
    int getReadyStemRevision() const;

    /** Override or query the executable used for the main Python worker. */
    juce::String getEngineCommand() const;

    void setRefinementEnabled(bool enabled) noexcept { refinementEnabled.store(enabled); }

    bool isRefinementEnabled() const noexcept { return refinementEnabled.load(); }

    /*  Whether hybrid fusion scales each stem so its own peak sits at 0.999.

        Off by default. The factor is derived from one stem alone, so a loud
        stem is attenuated and a quiet one is not: the six stop summing back
        to the source they came from, and their balance against each other
        shifts. Stems are written as 32-bit float, where a sample above 1.0
        is exactly representable, so nothing clips in the file either way.
    */
    void setFusedStemNormalisation(bool enabled) noexcept
    {
        fusedStemNormalisation.store(enabled);
    }

    bool isFusedStemNormalisation() const noexcept { return fusedStemNormalisation.load(); }

    enum SeparatorEngine
    {
        separatorRoFormer = 0,
        separatorDemucs = 1,
        separatorHybrid = 2
    };

    void setSeparatorEngineIndex(int index) noexcept
    {
        separatorEngineIndex.store(juce::jlimit(0, separatorEngineCount - 1, index));
    }

    int getSeparatorEngineIndex() const noexcept { return separatorEngineIndex.load(); }

    juce::String getSeparatorEngineId() const;
    juce::String getSeparatorEngineDisplayName() const;

    /** What the header selector shows on its face: "Hybrid", "Demucs". */
    static juce::String getSeparatorEngineShortName(int index);

    /** What that selector's menu spells out: "Hybrid (RoFormer + Demucs)". */
    static juce::String getSeparatorEngineMenuName(int index);

    static constexpr int separatorEngineCount = 3;

    void setStemEnabled(int index, bool enabled);
    bool isStemEnabled(int index) const;

    void setWaveformColorIndex(int index);
    int getWaveformColorIndex() const noexcept { return waveformColorIndex.load(); }

    /** Number of selectable lane waveform palettes; must stay in step with
        the theme's list. A remembered palette that no longer exists - state
        saved when the solid fills did - clamps into range here. */
    static constexpr int waveformColorCount = 5;

    /*  The waveform palette is remembered the same way the accent is, and
        for the same reason: it is how you like to read a waveform, not
        something about this project's audio, so it should still be your
        palette in the next session and in the next host.

        Stored by name, so reordering the theme's list cannot silently
        repaint someone's lanes.
    */
    static juce::File waveformColorPreferenceFile();
    static int readRememberedWaveformColor();
    static void rememberWaveformColor(int index);

    /**
     * Which palette a fresh instance starts on: Spectrum, index 2.
     *
     * It shows what the audio is actually doing - violet where the spectral
     * centroid sits low, amber where it sits high - which is worth more on
     * first sight than one accent color repeated down every lane. Only
     * until someone chooses: a remembered palette always wins.
     */
    static constexpr int defaultWaveformColorIndex = 2;

    /**
     * Horizontal waveform zoom, shared by every lane so they stay in step.
     *
     * 1 draws the whole file, as the lanes always did. Above that they draw
     * a window of it centred on the playhead, which is the only way to see
     * an individual hit in a five-minute track at 800px wide.
     */
    void setWaveformZoom(double zoom);
    double getWaveformZoom() const noexcept { return waveformZoom.load(); }

    static constexpr double minWaveformZoom = 1.0;
    static constexpr double maxWaveformZoom = 64.0;

    /**
     * The seconds a lane draws for a file of totalLengthSeconds at the
     * current zoom: a window centred on the playhead, clamped so it never
     * runs past either end of the file.
     */
    juce::Range<double> getWaveformViewRange(double totalLengthSeconds) const;

    /**
     * The size the user last left the editor at, as a percentage of its
     * design size. Lives here rather than in the editor so a reopened window
     * comes back the way they left it.
     */
    void setEditorScalePercent(int percent);
    int getEditorScalePercent() const noexcept { return editorScalePercent.load(); }

    static juce::String getStemName(int index);
    static constexpr int stemCount = 6;

private:
    friend class StemLabEngineThread;
    friend class StemLabRecursiveThread;
    friend class StemLabUtilityThread;

#if JUCE_WINDOWS || JUCE_LINUX
    friend class StemLabSystemLoopbackThread;
#endif

    void stopCapture();
    void setStatus(const juce::String&, StatusSeverity = statusInfo);
    void setActionStatus(const juce::String&);
    void setEngineProgress(double progress);
    void handleEngineOutputLine(const juce::String& line);

    /** Consume one "STEMLAB_STEM_READY <stem> <path>" payload - everything
        after the keyword and its separating space. */
    void handleStemReadyLine(const juce::String& payload);

    /** Drop every per-stem announcement. Called wherever the lanes must stop
        showing an unfinished job's stems: a new launch, a cancel, a
        failure. Lazy job-mismatch dropping is not enough on its own - a job
        that fails before announcing anything never reaches the writer. */
    void resetReadyStemFiles();

    juce::StringArray makePythonModuleCommand(const juce::String& moduleName) const;

    /** Pre-queue waveform analyses for the finished job's stems. */
    void warmCompletedStemProfiles();

    /** False when the manifest was unusable - the caller must not then
        announce the split as complete over the reason this published. */
    bool finishRecursiveJob(const juce::File& manifestFile);
    void clearRecursiveResults();

    /** Launch the source-analysis worker. Returns false when no command
        could be built or the thread refused to start. */
    bool startSourceAnalysis(const juce::File& source);

    void finishSourceAnalysis(const juce::File& source, const juce::File& result, int exitCode);

    /** Shared by download, compile and removal: they differ only in argv. */
    /** Publish the compile preference where child processes will read it. */
    void exportTorchCompilePreference() const;

    bool launchModelJob(const juce::StringArray& arguments, const juce::String& label);
    void finishModelInventory(const juce::File& output, int exitCode);
    void finishModelJob(const juce::String& label, int exitCode);

    bool launchAnalysisMaintenance(const juce::StringArray& arguments, const juce::String& label);
    void finishAnalysisMaintenance(const juce::File& source, const juce::String& label,
                                   int exitCode);
    bool launchMidiConversion(const juce::File& source, const juce::String& stemType,
                              const juce::String& label, const juce::String& outputName,
                              const juce::String& resultId);
    void finishMidiConversion(const juce::String& label, const juce::File& output, int exitCode,
                              const juce::String& resultId);
    bool loadMidiInfo(const juce::String& id, const juce::File& midiFile);
    bool renderMidiAudition(juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void reserveMidiAuditionEvents();

    /*  The half of stopSystemAudioRecording() that may only run once the
        loopback thread has actually left run(): its last act is to destroy
        the ThreadedWriter, which flushes the FIFO and finalises the WAV
        header, so the file is not readable before then. Runs on the message
        thread, either inline when the thread exits promptly or from
        captureStopTimer when it does not.
    */
    void finishSystemAudioRecordingStop();

    /** True while a stopped loopback thread is still flushing to disk. */
    bool isSystemCaptureStopPending() const noexcept;

    /** Which lane a highlighted range applies to, in Lanes terms. */
    juce::String getCurrentPreviewSelectionId() const;
    void rebuildLoopRegions();
    void applyPreviewLoopTick();

    /** Arms the loop enforcer if there is anything to enforce. The timer
        stops itself once playback stops, so every path that starts a
        transport has to arm it again. */
    void startLoopTimerIfRegions();

    using MonitorFlags = StemLabLaneMonitorFlags;

    std::shared_ptr<MonitorFlags> monitorFlagsForStem(int index) const;
    std::shared_ptr<MonitorFlags> monitorFlagsForRecursive(const juce::String& itemId) const;
    void clearAllMonitorFlags();

    /** The pointer-taking core behind isStemAudible/isRecursiveStemAudible. */
    bool isLaneAudible(const StemLabLaneMonitorFlags* flags) const;

    /** Solo on a lane is only audible in the stem mix; switch to it. */
    void followSoloIntoStemMix();
    void appendEngineLog(const juce::String&);
    bool sendAbletonBridgeNotification(const juce::File& manifestFile);
    bool sendAbletonControlMessage(const juce::String& message);
    juce::String setAbletonTempo();

    bool startThreadedInputCapture(const juce::String& prefix, double sampleRate, int channels,
                                   double startPpq, int recordingMode,
                                   const juce::String& recordingStatus);

    /** Hands the loaded source over to a system capture. Called by the
        loopback thread once its writer is open, which is the first moment
        the recording is certain and therefore the first moment the previous
        source, job and diagnostics may be thrown away. */
    void beginSystemCaptureSource(const juce::File& recordingFile);

    /** The stem file itself, or the playback loop's regions rendered to WAV. */
    juce::File exportLoopedRegions(const juce::File& source, const juce::File& destination);

    /** The merged loop regions, copied under the selection lock. */
    std::vector<stemlab::loops::Region> loopRegionsSnapshot() const;

    juce::File createRecordingFile(const juce::String& prefix) const;
    juce::File createJobDirectory() const;
    bool loadPreviewFile(const juce::File& file, int previewStem);

    /**
     * Held by the audio thread around the capture write and by the message
     * thread while it clears activeWriter, so the writer can never be
     * destroyed while a write is in flight through it.
     */
    juce::CriticalSection writerLock;

    juce::TimeSliceThread diskWriterThread{"StemLab capture writer"};

    /** Feeds the monitoring read-ahead buffers so playback never reads disk
        from the audio thread. */
    juce::TimeSliceThread previewReadThread{"StemLab preview reader"};
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter{nullptr};

    std::atomic<bool> capturing{false};
    std::atomic<int> standaloneRecordingMode{recordingNone};
    std::atomic<juce::int64> capturedSamples{0};

    /*  Samples the threaded writer refused because its FIFO was full.
        ThreadedWriter::write does not block - it returns false and discards
        the block - so without this counter a disk hiccup produced a
        time-compressed recording that still reported success, and the
        duration readout hid the gap because dropped blocks were counted as
        written. Non-zero at stop means the file is short by this much.
    */
    std::atomic<juce::int64> droppedCaptureSamples{0};
    std::atomic<double> captureStartPpq{-1.0};
    std::atomic<double> lastKnownHostPpq{0.0};

    std::atomic<bool> abletonClipRequestPending{false};
    std::atomic<double> abletonClipRequestStartMs{0.0};
    std::atomic<bool> abletonTempoRequestPending{false};
    std::atomic<double> abletonTempoRequestStartMs{0.0};
    std::atomic<double> inputDurationSeconds{0.0};

    double currentSampleRate = 44100.0;
    int currentInputChannels = 2;

    // Rate of the WAV a system-capture thread is writing. Atomic because the
    // capture thread stores it while the editor timer reads it through
    // getCapturedSeconds(); the host can concurrently rewrite
    // currentSampleRate in prepareToPlay, so that field cannot be trusted for
    // the recording-time readout.
    std::atomic<double> systemCaptureSampleRate{0.0};

    mutable juce::CriticalSection stateLock;

    // Sentinel file for the running job's cancel watchdog. Guarded by
    // stateLock; set at launch, used by cancelSeparation().
    juce::File activeCancelFile;
    juce::File captureFile;
    juce::File lastJobDirectory;
    juce::File jobRootDirectory;
    juce::File abletonClipReplyFile;
    juce::File abletonTempoReplyFile;
    juce::File abletonTempoRequestFile;
    juce::String inputSourceLabel;
    juce::String abletonClipRequestId;
    juce::String abletonTempoRequestId;

    juce::String sourceKey;
    juce::String sourceHash;
    juce::String sourceAnalysisDevice;
    juce::String sourceBeatModel;
    std::vector<StemLabKeyCandidate> sourceKeyCandidates;
    std::vector<double> sourceBeats;
    std::vector<double> sourceDownbeats;
    std::atomic<double> sourceBpm{-1.0};
    std::atomic<double> sourceDetectedBpm{-1.0};
    std::atomic<double> sourceHalfBpm{-1.0};
    std::atomic<double> sourceDoubleBpm{-1.0};
    std::atomic<double> sourceBarOne{0.0};
    std::atomic<int> sourceMeterNumerator{4};
    std::atomic<int> sourceMeterDenominator{4};
    std::atomic<bool> sourceTempoSteady{true};
    std::atomic<bool> sourceAnalysisRunning{false};
    std::atomic<bool> beatThisEnabled{false};
    std::atomic<int> sourceAnalysisMode{analysisFast};
    std::atomic<int> tempoAnalysisMode{tempoStatic};
    std::vector<StemLabTempoSegment> sourceTempoSegments;
    std::atomic<int> tempoInterpretation{tempoDetected};
    std::atomic<int> waveformGridMode{gridSource};

    // Off by default: a sweep does exactly what it did before until the user
    // asks for snapping.
    std::atomic<int> loopQuantizeMode{quantizeOff};
    std::atomic<double> manualGridBpm{120.0};
    std::atomic<int> manualGridNumerator{4};
    std::atomic<int> manualGridDenominator{4};
    std::atomic<double> manualGridBarOne{0.0};

    mutable juce::CriticalSection laneHeightLock;
    std::unordered_map<std::string, int> waveformLaneHeights;

    mutable juce::CriticalSection midiInfoLock;
    std::unordered_map<std::string, StemLabMidiInfo> midiInfos;

    juce::Synthesiser midiAuditionSynth;
    mutable juce::CriticalSection midiAuditionLock;

    /*  Sorted by note start, with midiAuditionNoteOffOrder indexing the same
        notes by note end. Audition blocks are contiguous, so a cursor into
        each list replaces rescanning the whole take on every block.
    */
    std::vector<StemLabMidiNoteInfo> midiAuditionNotes;
    std::vector<size_t> midiAuditionNoteOffOrder;
    size_t midiAuditionNoteOnCursor = 0;
    size_t midiAuditionNoteOffCursor = 0;

    /*  Sized in prepareToPlay and cleared per block: MidiBuffer allocates on
        its first event, which a fresh buffer per render put on the audio
        thread. clear() keeps the allocation.
    */
    juce::MidiBuffer midiAuditionEvents;

    juce::String midiAuditionId;
    double midiAuditionPosition = 0.0;
    double midiAuditionDuration = 0.0;
    std::atomic<bool> midiAuditionActive{false};

    // Host tempo/meter seen by processBlock, so the waveform grid can draw
    // the host's bars when the user picks that mode.
    std::atomic<double> lastHostBpm{-1.0};
    std::atomic<int> lastHostNumerator{4};
    std::atomic<int> lastHostDenominator{4};

    // One highlighted range per lane, keyed by stem name or child id.
    mutable juce::CriticalSection selectionLock;
    std::map<juce::String, StemLabSelectionRange> stemSelections;

    // Every lane's highlighted range takes part in the playback loop,
    // merged into sorted disjoint regions (normalised 0..1). Guarded by
    // selectionLock; enforced by loopTimer on the message thread.
    std::vector<stemlab::loops::Region> loopRegionsNormalised;
    bool previewLoopWasPlaying = false; // message thread only

    struct LoopTimer final : juce::Timer
    {
        explicit LoopTimer(StemLabAudioProcessor& ownerIn) : owner(ownerIn) {}
        void timerCallback() override { owner.applyPreviewLoopTick(); }
        StemLabAudioProcessor& owner;
    };
    LoopTimer loopTimer{*this};

    /*  Polls a stopping loopback thread instead of joining it. stopThread()
        on the message thread blocked the UI for as long as the final flush
        took, and its timeout escalates to thread cancellation - which the
        loopback thread's own contract forbids, because a forced unwind out
        of the writer's destructor terminates the host.
    */
    struct CaptureStopTimer final : juce::Timer
    {
        explicit CaptureStopTimer(StemLabAudioProcessor& ownerIn) : owner(ownerIn) {}
        void timerCallback() override { owner.finishSystemAudioRecordingStop(); }
        StemLabAudioProcessor& owner;
    };

    CaptureStopTimer captureStopTimer{*this};

    std::atomic<bool> abletonBridgeActive{false};

    juce::String status{"Ready"};
    StatusSeverity statusSeverity = statusInfo;

    // User-action feedback (header readout), separate from the work status
    // above so a palette change can never overwrite "Separating...".
    juce::String actionStatus;
    int actionStatusRevision = 0;

    /*  Chunks in arrival order rather than one juce::String: appending to a
        50 KB String re-measured it, reallocated it to an exact fit and
        copied it on every line, and the character-count trim walked the
        whole UTF-8 buffer once the cap was reached. Only getEngineLog()
        pays for the join. Guarded by stateLock; engineLogBytes tracks the
        UTF-8 size the trim budget is spent against.
    */
    std::deque<juce::String> engineLogChunks;
    size_t engineLogBytes = 0;

    // Resolved once when the VST3 wrapper delivers the host context, before
    // any editor exists; read from the message thread afterwards.
    std::unique_ptr<stemlab::reaper::Api> reaperApi;

    /*  Builds REAPER's peak cache for the stems Insert Stems just placed.
        Declared after reaperApi so it is destroyed first - it holds a
        reference to that Api.
    */
    std::unique_ptr<stemlab::reaper::PeakBuilder> reaperPeakBuilder;

    /**
     * Geometry of the last item pulled with Use Selected Item, echoed back by
     * Insert Stems so the new items match the original selection even when
     * the take was trimmed or rate-shifted. Guarded by stateLock; cleared
     * whenever a different source is loaded.
     */
    struct ReaperSourceInfo
    {
        bool valid = false;

        /*  The arrangement item itself, so Insert Stems can mute it. Never
            dereferenced without asking REAPER whether it is still live.
        */
        stemlab::reaper::MediaItem* item = nullptr;

        double startSeconds = 0.0;
        double lengthSeconds = 0.0;
        double startOffsetSeconds = 0.0;
        double playRate = 1.0;
        bool preservePitch = true;
        int trackNumber = 0;
    };

    ReaperSourceInfo reaperSourceInfo;

    void runReaperSelfTestIfRequested();
    void runReaperSelfTestAction(const juce::String& action, const juce::File& report);

    std::array<std::atomic<bool>, stemCount> stemEnabled;
    std::atomic<bool> refinementEnabled{true};
    std::atomic<bool> fusedStemNormalisation{false};
    std::atomic<int> separatorEngineIndex{separatorRoFormer};
    std::atomic<double> waveformZoom{1.0};
    std::atomic<int> waveformColorIndex{defaultWaveformColorIndex};
    std::atomic<int> editorScalePercent{100};

    std::atomic<double> engineProgress{0.0};
    std::atomic<double> engineStartMs{0.0};
    std::atomic<double> engineProgressUpdateMs{0.0};
    std::atomic<double> lastEngineDurationSeconds{0.0};

    // How long the last successful six-stem job took, untouched by the
    // shorter adaptive-split jobs that may follow it.
    std::atomic<double> mainJobDurationSeconds{0.0};
    std::atomic<bool> engineCompletedSuccessfully{false};

    // Snapshotted at launch, where the --no-refine decision is made, so the
    // finished job's summary and the command it actually ran cannot differ.
    std::atomic<bool> lastJobRefinement{true};

    /*
        Engine-reported seconds remaining (STEMLAB_ETA lines) and when the
        report arrived; -1 when the engine has not reported one this job.

        Guarded by stateLock rather than kept as two atomics: the pair must
        be read together (a value paired with another report's timestamp
        counts down from the wrong moment), and the same lock already
        carries every other engine-reader-to-UI handoff.
    */
    double engineEtaSeconds = -1.0;
    double engineEtaUpdateMs = 0.0;

    // Stage most recently read from stemlab_progress.txt; the poll only
    // publishes a stage the file actually changed (see the poll).
    juce::String lastPolledFileStage;

    void storeEngineEta(double seconds) noexcept;
    void resetEngineEta() noexcept;

    // Smoothed progress rate (fraction per second) for the fallback ETA.
    std::atomic<double> engineProgressRate{0.0};

    std::atomic<bool> engineCancelRequested{false};

    /** True once this job emitted a STEMLAB_PROGRESS line, which retires the
        raw "NN%" scraping fallback kept for pre-protocol engines. */
    std::atomic<bool> sawEngineProgressProtocol{false};

    /**
     * True for as long as the main six-stem separation thread's run() is
     * executing.
     *
     * Three reader threads share handleEngineOutputLine - the main engine,
     * the adaptive split, and source analysis - and only the main job owns
     * the six root lanes. An atomic rather than a probe of engineThread:
     * that unique_ptr is reassigned by the message thread while the old
     * reader is still draining its last lines, so reading it from a reader
     * thread races its own destruction.
     */
    std::atomic<bool> mainEngineRunning{false};

    /**
     * Resolved stem files for the current job. The editor asks for these
     * many times per redraw; without a cache each answer costs a recursive
     * enumeration of the job tree. Invalidated by a change of job directory
     * or of completion state.
     */
    mutable juce::CriticalSection stemFileCacheLock;
    mutable juce::File stemFileCacheJob;
    mutable bool stemFileCacheJobDone = false;
    mutable std::array<juce::File, stemCount> stemFileCache;

    /**
     * Per-stem STEMLAB_STEM_READY announcements from the separation that is
     * still running: the file the engine finished writing, the job those
     * files belong to, and a revision that changes with every slot.
     *
     * Guarded by stemFileCacheLock above rather than by a lock of its own.
     * That lock already answers "which file is this stem right now", has
     * only these two records under it, and is held over nothing but plain
     * assignments; a lane reads exactly one of the two records per refresh,
     * so sharing it costs no extra contention. Every copy out of these
     * slots must happen under it: juce::File holds a reference-counted
     * String, and copying a slot while an announcement overwrites it races
     * that refcount.
     *
     * Nothing may call getLastJobDirectory() while holding this lock. That
     * takes stateLock, and nesting the two would create an ordering rule
     * this code does not otherwise have.
     */
    std::array<juce::File, stemCount> readyStemFile;
    juce::File readyStemJob;
    int readyStemRevision = 0;

    static juce::File matchStemFile(const juce::Array<juce::File>& candidates,
                                    const juce::String& stem);

    /**
     * Liveness token for delayed message-thread callbacks. A cancel arms a
     * timer that outlives nothing else: the weak copy it captures expires
     * with this processor, so a plugin removed before the grace period ends
     * cannot be called back.
     */
    std::shared_ptr<int> lifetimeToken{std::make_shared<int>(0)};

    mutable juce::CriticalSection abletonBridgeLock;
    juce::String abletonBridgeStatus{"Bridge not confirmed yet"};
    std::atomic<double> abletonBridgeWaitStartMs{0.0};

    /** Stays true from Send/Retry until an ack is consumed, so an ack that
        lands after the 12s timeout still reaches the user instead of the
        stale "timed out" message inviting a duplicate import. */
    std::atomic<bool> abletonAckExpected{false};

    // Identity (mtime + size) of the Remote Script's heartbeat file at the
    // last bridge poll, so an unchanged file is not re-read and re-parsed
    // at the poll rate. Message thread only.
    juce::int64 bridgeHeartbeatMtime = 0;
    juce::int64 bridgeHeartbeatSize = 0;

    std::unique_ptr<StemLabEngineThread> engineThread;
    std::unique_ptr<StemLabRecursiveThread> recursiveThread;

    /** Short side jobs: source analysis, cache maintenance, MIDI. */
    std::unique_ptr<StemLabUtilityThread> analysisThread;
    std::unique_ptr<StemLabUtilityThread> midiThread;

    /*
     * Two threads rather than one, because the inventory is a read the UI
     * wants promptly and a download is a transfer that can run for minutes:
     * sharing a slot would mean either refusing to refresh while a download
     * ran, or cancelling the download to refresh. They never contend for the
     * same state - the inventory writes it, the job only invalidates it.
     */
    std::unique_ptr<StemLabUtilityThread> modelInventoryThread;
    std::unique_ptr<StemLabUtilityThread> modelJobThread;

    mutable juce::CriticalSection modelInventoryLock;
    std::vector<ManagedModel> managedModels;
    std::vector<ManagedCache> managedCaches;

    std::atomic<bool> modelInventoryRunning{false};
    std::atomic<bool> modelInventoryValid{false};
    std::atomic<bool> modelInventoryBroken{false};
    std::atomic<bool> modelJobRunning{false};
    std::atomic<bool> essentialModelMissing{false};
    std::atomic<bool> compileRequested{false};
    std::atomic<bool> torchCompileEnabled{false};
    std::atomic<bool> compileSupported{false};
    juce::String compileReason;

    juce::File modelInventoryFile;
    juce::File modelJobCancelFile;

#if JUCE_WINDOWS || JUCE_LINUX
    std::unique_ptr<StemLabSystemLoopbackThread> systemLoopbackThread;
#endif

    mutable juce::CriticalSection recursiveLock;
    std::vector<StemLabRecursiveStemInfo> recursiveItems;
    // Mutable: a lane's flags are created the first time anything asks for
    // them, including a const query from the editor's refresh.
    mutable std::map<juce::String, std::shared_ptr<MonitorFlags>> recursiveMonitorFlags;
    int recursiveTreeGeneration = 0;

    juce::AudioFormatManager previewFormats;

    // Reader formats for waveform analysis, separate from previewFormats so
    // the analysis worker never shares a manager with the message thread.
    // Registered in the constructor body; the worker only touches it once a
    // file is queued, which cannot happen before construction returns.
    juce::AudioFormatManager waveformFormats;

    /** See getWaveformCache(). Its own destructor stops the analysis
        thread, the same shutdown the other worker threads get in
        ~StemLabAudioProcessor. */
    StemLabWaveformCache waveformProfiles{waveformFormats};

    std::unique_ptr<juce::AudioFormatReaderSource> previewReaderSource;
    juce::AudioTransportSource previewTransport;
    juce::AudioSourcePlayer previewPlayer;
    juce::AudioBuffer<float> previewScratch;

    // The stem-mix monitor. stemMixSource owns one reader per completed
    // stem and sums them with per-stem solo/mute gains; stemMixTransport
    // wraps it for start/stop/seek/resampling. Which transport the audio
    // thread pulls is published through audioMonitorIsMix.
    std::unique_ptr<StemLabStemMixSource> stemMixSource;
    juce::AudioTransportSource stemMixTransport;
    juce::File stemMixJobDirectory;

    // Which adaptive tree the loaded mix was built from; a split or a
    // cleared tree changes the leaf set the mix has to play.
    int stemMixTreeGeneration = -1;

    std::array<std::shared_ptr<MonitorFlags>, stemCount> rootMonitorFlags;
    std::atomic<int> monitorMode{monitorOriginal};
    std::atomic<bool> audioMonitorIsMix{false};

    bool ensureStemMixLoaded();
    void unloadStemMix();
    juce::AudioTransportSource& activeTransport() noexcept;
    const juce::AudioTransportSource& activeTransport() const noexcept;
    void switchAudioMonitor(bool useMix);

    // In Standalone mode this points at JUCE's real wrapper device manager,
    // so preview, physical-input recording, and the native Audio/MIDI Settings
    // dialog all refer to the same selected device.
    juce::AudioDeviceManager* standaloneDeviceManager = nullptr;

    std::atomic<int> previewStemIndex{-2}; // -2 none, -1 source, 0..5 stem

    JUCE_DECLARE_WEAK_REFERENCEABLE(StemLabAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessor)
};
