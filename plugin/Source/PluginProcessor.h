#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>

class StemLabEngineThread;
class StemLabSystemLoopbackThread;

class StemLabAudioProcessor final : public juce::AudioProcessor,
                                    public juce::ChangeBroadcaster,
                                    public juce::AudioSource,
                                    private juce::AsyncUpdater
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

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    // AudioSource callbacks used only by the Standalone preview player.
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResourcesForAudioSource();
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "StemLab"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Capture / standalone input ------------------------------------------
    bool startCapture();
    void stopCapture();

    // Generic source loading used by Standalone, Ableton clip retrieval, and
    // Windows system-audio recording.
    bool setInputAudioFile (
        const juce::File& file,
        double startPpq = 0.0,
        const juce::String& sourceLabel = {});

    bool setStandaloneInputFile (const juce::File& file);
    bool isStandaloneApp() const noexcept;

    // Ask the invisible StemLabRemote script for the selected/current
    // Arrangement clip's real underlying audio file.
    bool requestAbletonSourceClip();
    void refreshAbletonSourceClipFromDisk();

    bool isAwaitingAbletonSourceClip() const noexcept
    {
        return abletonClipRequestPending.load();
    }

    juce::String getInputSourceLabel() const;

    // Physical/interface input recording uses the JUCE standalone input.
    bool startStandaloneRecording();
    void stopStandaloneRecording();

    // System recording uses Windows WASAPI loopback on the current default
    // Windows render/output endpoint.
    bool startSystemAudioRecording();
    void stopSystemAudioRecording();

    int getStandaloneRecordingMode() const noexcept
    {
        return standaloneRecordingMode.load();
    }

    void toggleStandalonePlayback();
    bool playCompletedStem (int index);
    bool seekCompletedStem (int index, double normalisedPosition);
    void stopStandalonePlayback();

    bool isStandalonePlaying() const noexcept;

    int getPreviewStemIndex() const noexcept
    {
        return previewStemIndex.load();
    }

    double getPreviewPositionSeconds() const noexcept;
    double getPreviewLengthSeconds() const noexcept;

    bool isCapturing() const noexcept { return capturing.load(); }
    bool isCaptureArmed() const noexcept { return captureArmed.load(); }
    bool isCaptureFinalizing() const noexcept
    {
        return captureFinalizeRequested.load();
    }

    double getCapturedSeconds() const noexcept;
    juce::File getCaptureFile() const;
    double getCaptureStartPpq() const noexcept { return captureStartPpq.load(); }

    // Separation -----------------------------------------------------------
    bool launchSeparationAndExport();
    bool isEngineRunning() const noexcept;
    bool hasSuccessfulJob() const noexcept
    {
        return engineCompletedSuccessfully.load();
    }

    double getEngineProgress() const noexcept { return engineProgress.load(); }
    double getEngineElapsedSeconds() const noexcept;
    double getEngineEstimatedRemainingSeconds() const noexcept;
    void refreshEngineProgressFromDisk();

    juce::String getStatus() const;
    juce::String getEngineLog() const;
    juce::File getLastJobDirectory() const;

    void setJobRootDirectory (const juce::File& directory);
    juce::File getJobRootDirectory() const;

    // Ableton bridge -------------------------------------------------------
    void refreshAbletonBridgeStatusFromDisk();
    bool sendSelectedStemsToAbleton();
    bool retryAbletonImport();
    juce::String getAbletonBridgeStatus() const;
    int getAbletonImportedStemCount() const noexcept
    {
        return abletonImportedStemCount.load();
    }

    // UI-safe wrapper. Keeps the internal status implementation private while
    // allowing the editor to surface small user-facing confirmations.
    void postUiStatus (const juce::String& message);

    // Standalone export: split all stems internally, then choose which
    // completed files to copy out.
    int saveSelectedStemsTo (const juce::File& destination);
    juce::File getCompletedStemFile (int index) const;

    // Settings -------------------------------------------------------------
    void setEngineCommand (const juce::String&);
    juce::String getEngineCommand() const;
    void resetEngineCommandToAutoDiscover();

    void setRefinementEnabled (bool enabled) noexcept
    {
        refinementEnabled.store (enabled);
    }

    bool isRefinementEnabled() const noexcept
    {
        return refinementEnabled.load();
    }

    enum SeparatorEngine
    {
        separatorRoFormer = 0,
        separatorDemucs = 1,
        separatorHybrid = 2
    };

    void setSeparatorEngineIndex (int index) noexcept
    {
        separatorEngineIndex.store (
            juce::jlimit (
                0,
                separatorEngineCount - 1,
                index));
    }

    int getSeparatorEngineIndex() const noexcept
    {
        return separatorEngineIndex.load();
    }

    juce::String getSeparatorEngineId() const;
    juce::String getSeparatorEngineDisplayName() const;

    static constexpr int separatorEngineCount = 3;

    void setStemEnabled (int index, bool enabled);
    bool isStemEnabled (int index) const;

    void setWaveformColourIndex (int index);
    int getWaveformColourIndex() const noexcept
    {
        return waveformColourIndex.load();
    }

    static constexpr int waveformColourCount = 7;

    static juce::String getStemName (int index);
    static constexpr int stemCount = 6;

private:
    friend class StemLabEngineThread;
    friend class StemLabSystemLoopbackThread;

    void handleAsyncUpdate() override;
    void setStatus (const juce::String&);
    void setEngineProgress (double progress);
    void handleEngineOutputLine (const juce::String& line);
    juce::String discoverEngineCommand() const;
    void appendEngineLog (const juce::String&);
    void finalizeHostCapture();
    bool sendAbletonBridgeNotification (const juce::File& manifestFile);
    bool sendAbletonControlMessage (const juce::String& message);
    juce::File getAbletonClipReplyFile() const;

    juce::File createCaptureFile() const;
    juce::File createRecordingFile() const;
    juce::File createSystemRecordingFile() const;
    juce::File createJobDirectory() const;
    bool loadPreviewFile (const juce::File& file, int previewStem);

    juce::TimeSliceThread diskWriterThread { "StemLab capture writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };

    std::atomic<bool> capturing { false };
    std::atomic<bool> captureArmed { false };
    std::atomic<bool> captureFinalizeRequested { false };
    std::atomic<bool> hostWasPlayingDuringCapture { false };
    std::atomic<int> standaloneRecordingMode { recordingNone };
    std::atomic<juce::int64> capturedSamples { 0 };
    std::atomic<double> captureStartPpq { -1.0 };
    std::atomic<juce::int64> captureStartTimelineSample { -1 };
    std::atomic<double> lastKnownHostPpq { 0.0 };
    std::atomic<juce::int64> lastKnownHostTimelineSample { 0 };

    std::atomic<bool> abletonClipRequestPending { false };
    std::atomic<double> abletonClipRequestStartMs { 0.0 };
    std::atomic<double> inputDurationSeconds { 0.0 };

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

    juce::String engineCommand { "stemlab-plugin-job" };
    juce::String status { "Ready" };
    juce::String engineLog;

    std::array<std::atomic<bool>, stemCount> stemEnabled;
    std::atomic<bool> refinementEnabled { true };
    std::atomic<int> separatorEngineIndex { separatorRoFormer };
    std::atomic<int> waveformColourIndex { 0 };

    std::atomic<double> engineProgress { 0.0 };
    std::atomic<double> engineStartMs { 0.0 };
    std::atomic<double> lastEngineDurationSeconds { 0.0 };
    std::atomic<bool> engineCompletedSuccessfully { false };

    mutable juce::CriticalSection abletonBridgeLock;
    juce::String abletonBridgeStatus { "Bridge not confirmed yet" };
    std::atomic<int> abletonImportedStemCount { 0 };
    std::atomic<double> abletonBridgeWaitStartMs { 0.0 };

    std::unique_ptr<StemLabEngineThread> engineThread;
    std::unique_ptr<StemLabSystemLoopbackThread> systemLoopbackThread;

    juce::AudioFormatManager previewFormats;
    std::unique_ptr<juce::AudioFormatReaderSource> previewReaderSource;
    juce::AudioTransportSource previewTransport;
    juce::AudioSourcePlayer previewPlayer;
    juce::AudioBuffer<float> previewScratch;

    // In Standalone mode this points at JUCE's real wrapper device manager,
    // so preview, physical-input recording, and the native Audio/MIDI Settings
    // dialog all refer to the same selected device.
    juce::AudioDeviceManager* standaloneDeviceManager = nullptr;

    std::atomic<int> previewStemIndex { -2 }; // -2 none, -1 source, 0..5 stem

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemLabAudioProcessor)
};
