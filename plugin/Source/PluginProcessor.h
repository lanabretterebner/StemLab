#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "HostIntegrationPolicy.h"

class StemLabEngineThread;
class StemLabRecursiveThread;
class StemLabSystemLoopbackThread;
class StemLabUtilityThread;

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
 * Owns FI-STEM's audio capture, preview player, Python jobs, and Ableton bridge.
 *
 * JUCE calls the AudioProcessor overrides on audio/host threads. The editor calls
 * the remaining public methods from the message thread. Expensive separation is
 * always delegated to a child Python process.
 */
class StemLabAudioProcessor final : public juce::AudioProcessor,
                                    public juce::ChangeBroadcaster,
                                    public juce::AudioSource
{
public:
    enum StandaloneRecordingMode
    {
        recordingNone = 0,
        recordingInput = 1,
        recordingSystem = 2,
        recordingHost = 3
    };

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

    const juce::String getName() const override { return "FI-STEM"; }
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

    /** Ask FI-STEM Remote for Ableton's selected Arrangement audio clip. */
    bool requestAbletonSourceClip();
    void refreshAbletonSourceClipFromDisk();

    bool isAwaitingAbletonSourceClip() const noexcept { return abletonClipRequestPending.load(); }

    juce::String getInputSourceLabel() const;

    /** Start/stop recording the Standalone app's selected physical input. */
    bool startStandaloneRecording();
    void stopStandaloneRecording();

    /** Explicitly capture VST3 input audio in non-Ableton hosts. */
    bool startHostAudioCapture();
    void stopHostAudioCapture();
    bool isHostAudioCapturing() const noexcept
    {
        return standaloneRecordingMode.load() == recordingHost && capturing.load();
    }

    /** Start/stop Windows WASAPI loopback recording of the default output. */
    bool startSystemAudioRecording();
    void stopSystemAudioRecording();

    int getStandaloneRecordingMode() const noexcept { return standaloneRecordingMode.load(); }

    void toggleStandalonePlayback();
    bool playCompletedStem(int index);
    bool seekCompletedStem(int index, double normalisedPosition);
    void stopStandalonePlayback();

    bool isStandalonePlaying() const noexcept;

    int getPreviewStemIndex() const noexcept { return previewStemIndex.load(); }

    double getPreviewPositionSeconds() const noexcept;
    double getPreviewLengthSeconds() const noexcept;

    bool isCapturing() const noexcept { return capturing.load(); }

    double getCapturedSeconds() const noexcept;
    juce::File getCaptureFile() const;
    double getCaptureStartPpq() const noexcept { return captureStartPpq.load(); }

    /** Launch the main six-stem Python job for the currently loaded source. */
    bool launchSeparationAndExport();
    bool cancelRunningJob();
    bool isCancelRequested() const noexcept { return engineCancelRequested.load(); }

    /** Launch the default adaptive action for one completed root stem. */
    bool launchRecursiveStemSplit(int rootStemIndex);

    /** Launch an action advertised by an existing adaptive-tree node. */
    bool launchRecursiveAction(const juce::String& itemId, const juce::String& action);
    bool isRecursiveEngineRunning() const noexcept;
    std::vector<StemLabRecursiveStemInfo> getRecursiveStemItems() const;
    juce::File getRecursiveStemFile(const juce::String& itemId) const;
    void setRecursiveStemEnabled(const juce::String& itemId, bool enabled);
    bool isRecursiveStemEnabled(const juce::String& itemId) const;
    bool playRecursiveStem(const juce::String& itemId);
    bool seekRecursiveStem(const juce::String& itemId, double normalisedPosition);
    juce::String getPreviewRecursiveId() const;

    bool isEngineRunning() const noexcept;
    bool hasSuccessfulJob() const noexcept { return engineCompletedSuccessfully.load(); }

    double getEngineProgress() const noexcept { return engineProgress.load(); }
    double getEngineElapsedSeconds() const noexcept;
    double getEngineEstimatedRemainingSeconds() const noexcept;
    void refreshEngineProgressFromDisk();

    juce::String getStatus() const;
    juce::String getEngineLog() const;

    /** Whether getEngineLog() would return anything, without paying for the
        join - the editor asks this every time it builds the settings menu. */
    bool hasEngineLog() const;
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

    /** Right-click export selection: solo this stem; right-click the same solo again to restore the previous export selection. */
    void soloStemForExport(int index);
    void soloRecursiveStemForExport(const juce::String& itemId);

    bool launchStemMidiConversion(int stemIndex);
    bool launchRecursiveMidiConversion(const juce::String& itemId);
    bool isMidiConversionRunning() const noexcept;
    StemLabMidiInfo getMidiInfo(const juce::String& id) const;
    bool hasMidiInfo(const juce::String& id) const;
    bool auditionMidi(const juce::String& id);
    bool isMidiAuditioning(const juce::String& id) const;
    void stopMidiAudition();
    bool sendMidiToAbleton(const juce::String& id);

    /** Toggle the host transport through JUCE or FI-STEM Remote when supported. */
    bool toggleHostTransport();

    void setJobRootDirectory(const juce::File& directory);
    juce::File getJobRootDirectory() const;

    /** Poll FI-STEM Remote's status/acknowledgement files. */
    void refreshAbletonBridgeStatusFromDisk();

    /** Ask FI-STEM Remote to import all currently selected completed stems. */
    bool sendSelectedStemsToAbleton();
    bool retryAbletonImport();
    juce::String getAbletonBridgeStatus() const;
    bool isAbletonBridgeActive() const noexcept { return abletonBridgeActive.load(); }
    /** Publish a short status message from a UI callback. */
    void postUiStatus(const juce::String& message);

    /** Copy selected completed stems to a Standalone export directory. */
    int saveSelectedStemsTo(const juce::File& destination);
    /** Return selected WAVs, rendering existing waveform ranges when needed. */
    juce::StringArray getSelectedStemFilesForDrag();
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

    static constexpr int waveformColourCount = 7;

    static juce::String getStemName(int index);
    static constexpr int stemCount = 6;

private:
    friend class StemLabEngineThread;
    friend class StemLabRecursiveThread;
    friend class StemLabSystemLoopbackThread;
    friend class StemLabUtilityThread;

    void stopCapture();
    void setStatus(const juce::String&);
    void setEngineProgress(double progress);
    void handleEngineOutputLine(const juce::String& line);
    juce::StringArray makePythonModuleCommand(const juce::String& moduleName) const;
    void finishRecursiveJob(const juce::File& manifestFile);
    void finishCancelledJob(const juce::File& cleanupDirectory, bool mainJob);
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
    juce::String discoverEngineCommand() const;
    void appendEngineLog(const juce::String&);
    bool sendAbletonBridgeNotification(const juce::File& manifestFile);
    bool sendAbletonControlMessage(const juce::String& message);

    bool startThreadedInputCapture(const juce::String& prefix, double sampleRate, int channels,
                                   double startPpq, int recordingMode,
                                   const juce::String& recordingStatus);

    juce::File createRecordingFile(const juce::String& prefix) const;
    juce::File createJobDirectory() const;
    bool loadPreviewFile(const juce::File& file, int previewStem);
    void renderPreviewAudioBlock(const juce::AudioSourceChannelInfo& info);
    void updatePreviewLoopForId(const juce::String& id);
    juce::String getCurrentPreviewSelectionId() const;
    juce::File exportSelectedRegion(const juce::File& source, const juce::File& destination,
                                    const juce::String& selectionId, double* startSeconds = nullptr,
                                    double* endSeconds = nullptr);

    juce::TimeSliceThread diskWriterThread{"FI-STEM capture writer"};
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter{nullptr};

    std::atomic<bool> capturing{false};
    std::atomic<int> standaloneRecordingMode{recordingNone};
    std::atomic<juce::int64> capturedSamples{0};

    /*  The rate the system capture opened its device at, which need not be
        the rate the host prepared this plugin at. Written by the capture
        thread before it reports any samples, read by getCapturedSeconds. */
    std::atomic<double> systemCaptureSampleRate{0.0};

    /*  Samples the threaded writer refused because its FIFO was full.
        ThreadedWriter::write does not block - it returns false and discards
        the block - so without this counter a disk hiccup produced a
        time-compressed recording that still reported success, and the
        duration readout hid the gap because dropped blocks were counted as
        written. Non-zero at stop means the file is short by this much. */
    std::atomic<juce::int64> droppedCaptureSamples{0};
    std::atomic<double> captureStartPpq{-1.0};
    std::atomic<double> lastKnownHostPpq{0.0};
    std::atomic<bool> lastHostPlaying{false};
    std::atomic<double> lastHostBpm{120.0};
    std::atomic<int> lastHostNumerator{4};
    std::atomic<int> lastHostDenominator{4};

    std::atomic<bool> abletonClipRequestPending{false};
    std::atomic<double> abletonClipRequestStartMs{0.0};
    std::atomic<double> inputDurationSeconds{0.0};

    double currentSampleRate = 44100.0;
    int currentInputChannels = 2;

    mutable juce::CriticalSection stateLock;
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

    juce::String engineCommand{"stemlab-plugin-job"};
    juce::String status{"Ready"};
    /*  Chunks in arrival order rather than one juce::String: appending to a
        50 KB String re-measured it, reallocated it to an exact fit and
        copied it on every line, and the character-count trim walked the
        whole UTF-8 buffer once the cap was reached. Only getEngineLog()
        pays for the join. Guarded by stateLock; engineLogBytes tracks the
        UTF-8 size the trim budget is spent against. */
    std::deque<juce::String> engineLogChunks;
    size_t engineLogBytes = 0;

    std::array<std::atomic<bool>, stemCount> stemEnabled;
    std::atomic<bool> refinementEnabled{true};
    std::atomic<int> separatorEngineIndex{separatorRoFormer};
    std::atomic<int> waveformColourIndex{0};

    std::atomic<double> engineProgress{0.0};
    std::atomic<double> engineStartMs{0.0};
    std::atomic<double> engineProgressUpdateMs{0.0};
    std::atomic<double> lastEngineDurationSeconds{0.0};
    std::atomic<bool> engineCompletedSuccessfully{false};
    std::atomic<bool> engineCancelRequested{false};

    mutable juce::CriticalSection abletonBridgeLock;
    juce::String abletonBridgeStatus{"Bridge not confirmed yet"};
    std::atomic<double> abletonBridgeWaitStartMs{0.0};
    std::atomic<bool> abletonBridgeActive{false};

    std::unique_ptr<StemLabEngineThread> engineThread;
    std::unique_ptr<StemLabRecursiveThread> recursiveThread;
    std::unique_ptr<StemLabSystemLoopbackThread> systemLoopbackThread;
    std::unique_ptr<StemLabUtilityThread> analysisThread;
    std::unique_ptr<StemLabUtilityThread> midiThread;

    mutable juce::CriticalSection recursiveLock;
    std::vector<StemLabRecursiveStemInfo> recursiveItems;
    juce::String previewRecursiveId;

    // Export-solo is a reversible UI mode: the first right-click snapshots the
    // current root/recursive export selections, and a second right-click on the
    // same stem restores that snapshot. Switching the solo target keeps the
    // original snapshot so the user can still exit solo cleanly.
    mutable juce::CriticalSection exportSoloLock;
    bool exportSoloActive = false;
    bool exportSoloRecursive = false;
    int exportSoloStemIndex = -1;
    juce::String exportSoloRecursiveId;
    std::array<bool, stemCount> exportSoloStemSnapshot{};
    std::unordered_map<std::string, bool> exportSoloRecursiveSnapshot;

    mutable juce::CriticalSection selectionLock;
    std::unordered_map<std::string, StemLabSelectionRange> stemSelections;
    std::atomic<bool> previewLoopEnabled{false};
    std::atomic<double> previewLoopStart{0.0};
    std::atomic<double> previewLoopEnd{0.0};

    juce::AudioFormatManager previewFormats;
    std::unique_ptr<juce::AudioFormatReaderSource> previewReaderSource;
    juce::AudioTransportSource previewTransport;
    juce::AudioSourcePlayer previewPlayer;
    juce::AudioBuffer<float> previewScratch;

    // In Standalone mode this points at JUCE's real wrapper device manager,
    // so preview, physical-input recording, and the native Audio/MIDI Settings
    // dialog all refer to the same selected device.
    juce::AudioDeviceManager* standaloneDeviceManager = nullptr;

    std::atomic<int> previewStemIndex{-2}; // -2 none, -1 source, 0..5 stem

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessor)
};
