#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class StemWaveformComponent final : public juce::Component
{
public:
    StemWaveformComponent (
        StemLabAudioProcessor& processor,
        int stemIndex,
        juce::AudioFormatManager& formatManager,
        juce::AudioThumbnailCache& thumbnailCache);

    void setFile (const juce::File& file);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    StemLabAudioProcessor& processor;
    int stemIndex = 0;
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemWaveformComponent)
};

class StemLabAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          public juce::FileDragAndDropTarget,
                                          private juce::Timer,
                                          private juce::ChangeListener
{
public:
    explicit StemLabAudioProcessorEditor (StemLabAudioProcessor&);
    ~StemLabAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void chooseEngineExecutable();
    void chooseStandaloneAudioFile();
    void chooseSaveFolder();
    void chooseJobRootFolder();
    void showSettingsMenu();
    void showStandaloneAudioSettings();
    void showFirstRunWelcome();
    void launchAbletonSetup();
    void refreshFromProcessor();

    static bool isSupportedAudioFile (const juce::File& file);

    StemLabAudioProcessor& processor;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::TextButton settingsButton { "Settings" };

    juce::TextButton captureButton { "Capture" };
    juce::TextButton recordInputButton { "Record Input" };
    juce::TextButton recordSystemButton { "Record System" };
    juce::TextButton stopButton { "Stop" };
    juce::TextButton playButton { "Play" };
    juce::Label captureTimeLabel;
    juce::Label recordHintLabel;
    juce::Label dropHintLabel;

    juce::ToggleButton refinementButton { "StemLab refinement" };

    juce::TextButton separateButton { "Separate" };

    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Label statusLabel;
    juce::Label timingLabel;

    juce::Label stemsLabel;
    std::array<juce::ToggleButton, StemLabAudioProcessor::stemCount> stemButtons;
    std::array<juce::TextButton, StemLabAudioProcessor::stemCount> stemPlayButtons;

    juce::AudioFormatManager waveformFormats;
    juce::AudioThumbnailCache waveformCache { 24 };

    std::array<
        std::unique_ptr<StemWaveformComponent>,
        StemLabAudioProcessor::stemCount> waveformComponents;

    juce::TextButton saveSelectedButton { "Save Selected..." };
    juce::TextButton sendSelectedButton { "Send Selected" };
    juce::TextButton retryImportButton { "Retry" };
    juce::TextButton openJobButton { "Choose File Location" };

    juce::Label bridgeLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    std::unique_ptr<juce::FileChooser> outputFolderChooser;
    std::unique_ptr<juce::FileChooser> jobFolderChooser;

    bool dragActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemLabAudioProcessorEditor)
};
