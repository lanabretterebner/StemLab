#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>

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
        recordingSystem = 2
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

    enum TempoInterpretation
    {
        tempoHalf = 0,
        tempoDetected = 1,
        tempoDouble = 2
    };

    enum WaveformGridMode
    {
        gridHost = 0,
        gridSource = 1,
        gridManual = 2
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

    bool isCapturing() const noexcept { return capturing.load(); }

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

    /**
     * Ask the running separation (main or adaptive) to stop. Writes the
     * cancel sentinel the engine's watchdog honors - the engine shuts down
     * its own model subprocesses, which a direct kill would orphan. Engines
     * without the watchdog are killed after a short grace period.
     */
    void cancelSeparation();
    bool isCancelRequested() const noexcept { return engineCancelRequested.load(); }

    juce::String getStatus() const;
    juce::String getEngineLog() const;
    juce::File getLastJobDirectory() const;

    /** Compact key/BPM text for the currently loaded original source. */
    juce::String getSourceAnalysisText() const;
    juce::String getSourceAnalysisDetails() const;
    juce::String getSourceKey() const;
    double getSourceBpm() const noexcept { return sourceBpm.load(); }
    double getDetectedSourceBpm() const noexcept { return sourceDetectedBpm.load(); }
    double getHalfTimeSourceBpm() const noexcept { return sourceHalfBpm.load(); }
    double getDoubleTimeSourceBpm() const noexcept { return sourceDoubleBpm.load(); }
    double getSourceBarOne() const noexcept { return sourceBarOne.load(); }
    int getSourceMeterNumerator() const noexcept { return sourceMeterNumerator.load(); }
    int getSourceMeterDenominator() const noexcept { return sourceMeterDenominator.load(); }
    void setBeatThisEnabled(bool enabled);
    bool isBeatThisEnabled() const noexcept { return beatThisEnabled.load(); }
    void setSourceAnalysisMode(int mode);
    int getSourceAnalysisMode() const noexcept { return sourceAnalysisMode.load(); }
    void setTempoInterpretation(int interpretation);
    int getTempoInterpretation() const noexcept { return tempoInterpretation.load(); }
    bool saveSourceCorrection(double bpm, const juce::String& key, int numerator, int denominator,
                              double barOne);
    bool forgetSourceCorrection();
    bool clearAnalysisCache();

    void setWaveformGridMode(int mode) noexcept;
    int getWaveformGridMode() const noexcept { return waveformGridMode.load(); }
    void setManualGrid(double bpm, int numerator, int denominator, double barOne) noexcept;
    StemLabGridInfo getWaveformGridInfo() const;

    int getWaveformLaneHeight(const juce::String& id) const;
    void setWaveformLaneHeight(const juce::String& id, int height);

    /** One highlighted time range per stem. Dragging a waveform sets it. */
    StemLabSelectionRange getStemSelectionRange(const juce::String& id) const;
    void setStemSelectionRange(const juce::String& id, double start, double end);
    void clearStemSelectionRange(const juce::String& id);
    void clearAllStemSelectionRanges();

    bool launchStemMidiConversion(int stemIndex);
    bool launchRecursiveMidiConversion(const juce::String& itemId);
    bool isMidiConversionRunning() const noexcept;
    StemLabMidiInfo getMidiInfo(const juce::String& id) const;
    bool hasMidiInfo(const juce::String& id) const;
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
    /** Publish a short status message from a UI callback. */
    void postUiStatus(const juce::String& message);

    /** Copy selected completed stems to a Standalone export directory. */
    int saveSelectedStemsTo(const juce::File& destination);
    juce::File getCompletedStemFile(int index) const;

    /** Override or query the executable used for the main Python worker. */
    void setEngineCommand(const juce::String&);
    juce::String getEngineCommand() const;
    void resetEngineCommandToAutoDiscover();

    void setRefinementEnabled(bool enabled) noexcept { refinementEnabled.store(enabled); }

    bool isRefinementEnabled() const noexcept { return refinementEnabled.load(); }

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

    void setWaveformColourIndex(int index);
    int getWaveformColourIndex() const noexcept { return waveformColourIndex.load(); }

    /** Number of selectable lane waveform palettes; the index persists in
        plugin state, so this must stay in step with the theme's palette. */
    static constexpr int waveformColourCount = 7;

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
    void setStatus(const juce::String&);
    void setEngineProgress(double progress);
    void handleEngineOutputLine(const juce::String& line);
    juce::StringArray makePythonModuleCommand(const juce::String& moduleName) const;
    void finishRecursiveJob(const juce::File& manifestFile);
    void clearRecursiveResults();

    void startSourceAnalysis(const juce::File& source);
    void finishSourceAnalysis(const juce::File& source, const juce::File& result, int exitCode);
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

    /** Which lane a highlighted range applies to, in Lanes terms. */
    juce::String getCurrentPreviewSelectionId() const;
    void updatePreviewLoopForId(const juce::String& id);

    using MonitorFlags = StemLabLaneMonitorFlags;

    std::shared_ptr<MonitorFlags> monitorFlagsForStem(int index) const;
    std::shared_ptr<MonitorFlags> monitorFlagsForRecursive(const juce::String& itemId) const;
    void clearAllMonitorFlags();

    /** Solo on a lane is only audible in the stem mix; switch to it. */
    void followSoloIntoStemMix();
    juce::String discoverEngineCommand() const;
    void appendEngineLog(const juce::String&);
    bool sendAbletonBridgeNotification(const juce::File& manifestFile);
    bool sendAbletonControlMessage(const juce::String& message);

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
    std::atomic<double> captureStartPpq{-1.0};
    std::atomic<double> lastKnownHostPpq{0.0};

    std::atomic<bool> abletonClipRequestPending{false};
    std::atomic<double> abletonClipRequestStartMs{0.0};
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
    juce::File abletonLegacyClipReplyFile;
    juce::String inputSourceLabel;
    juce::String abletonClipRequestId;

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
    std::atomic<bool> sourceAnalysisCorrected{false};
    std::atomic<bool> sourceAnalysisRunning{false};
    std::atomic<bool> beatThisEnabled{false};
    std::atomic<int> sourceAnalysisMode{analysisFast};
    std::atomic<int> tempoInterpretation{tempoDetected};
    std::atomic<int> waveformGridMode{gridSource};
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
    std::vector<StemLabMidiNoteInfo> midiAuditionNotes;
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

    // Looping the monitored lane over its highlighted range. Read on the
    // audio thread, written from the message thread.
    std::atomic<bool> previewLoopEnabled{false};
    std::atomic<double> previewLoopStart{0.0};
    std::atomic<double> previewLoopEnd{0.0};

    std::atomic<bool> abletonBridgeActive{false};

    juce::String engineCommand{"stemlab-plugin-job"};
    juce::String status{"Ready"};
    juce::String engineLog;

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
    std::atomic<int> separatorEngineIndex{separatorRoFormer};
    std::atomic<double> waveformZoom{1.0};
    std::atomic<int> waveformColourIndex{0};
    std::atomic<int> editorScalePercent{100};

    std::atomic<double> engineProgress{0.0};
    std::atomic<double> engineStartMs{0.0};
    std::atomic<double> engineProgressUpdateMs{0.0};
    std::atomic<double> lastEngineDurationSeconds{0.0};

    // How long the last successful six-stem job took, untouched by the
    // shorter adaptive-split jobs that may follow it.
    std::atomic<double> mainJobDurationSeconds{0.0};
    std::atomic<bool> engineCompletedSuccessfully{false};

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
     * Resolved stem files for the current job. The editor asks for these
     * many times per redraw; without a cache each answer costs a recursive
     * enumeration of the job tree. Invalidated by a change of job directory
     * or of completion state.
     */
    mutable juce::CriticalSection stemFileCacheLock;
    mutable juce::File stemFileCacheJob;
    mutable bool stemFileCacheJobDone = false;
    mutable std::array<juce::File, stemCount> stemFileCache;

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

    std::unique_ptr<StemLabEngineThread> engineThread;
    std::unique_ptr<StemLabRecursiveThread> recursiveThread;

    /** Short side jobs: source analysis, cache maintenance, MIDI. */
    std::unique_ptr<StemLabUtilityThread> analysisThread;
    std::unique_ptr<StemLabUtilityThread> midiThread;

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
