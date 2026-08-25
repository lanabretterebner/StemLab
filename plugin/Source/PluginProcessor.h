#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <map>
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

    static constexpr int separatorEngineCount = 3;

    void setStemEnabled(int index, bool enabled);
    bool isStemEnabled(int index) const;

    void setWaveformColourIndex(int index);
    int getWaveformColourIndex() const noexcept { return waveformColourIndex.load(); }

    /** Number of selectable lane waveform palettes; the index persists in
        plugin state, so this must stay in step with the theme's palette. */
    static constexpr int waveformColourCount = 7;

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

    juce::TimeSliceThread diskWriterThread{"StemLab capture writer"};
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
    std::atomic<int> waveformColourIndex{0};
    std::atomic<int> editorScalePercent{100};

    std::atomic<double> engineProgress{0.0};
    std::atomic<double> engineStartMs{0.0};
    std::atomic<double> engineProgressUpdateMs{0.0};
    std::atomic<double> lastEngineDurationSeconds{0.0};
    std::atomic<bool> engineCompletedSuccessfully{false};

    // Engine-reported seconds remaining (STEMLAB_ETA lines) and when the
    // report arrived; -1 when the engine has not reported one this job.
    std::atomic<double> engineEtaSeconds{-1.0};
    std::atomic<double> engineEtaUpdateMs{0.0};

    // Smoothed progress rate (fraction per second) for the fallback ETA.
    std::atomic<double> engineProgressRate{0.0};

    std::atomic<bool> engineCancelRequested{false};

    mutable juce::CriticalSection abletonBridgeLock;
    juce::String abletonBridgeStatus{"Bridge not confirmed yet"};
    std::atomic<double> abletonBridgeWaitStartMs{0.0};

    std::unique_ptr<StemLabEngineThread> engineThread;
    std::unique_ptr<StemLabRecursiveThread> recursiveThread;

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
