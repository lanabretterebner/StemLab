#pragma once

#include <JuceHeader.h>
#include <vector>
#include <functional>
#include "PluginProcessor.h"

/** Draws one source/stem waveform and converts mouse clicks into preview seeks. */
class StemWaveformComponent final : public juce::Component
{
public:
    StemWaveformComponent(StemLabAudioProcessor& processor, int stemIndex,
                          juce::AudioFormatManager& formatManager,
                          juce::AudioThumbnailCache& thumbnailCache);

    StemWaveformComponent(StemLabAudioProcessor& processor, juce::String recursiveId,
                          juce::AudioFormatManager& formatManager,
                          juce::AudioThumbnailCache& thumbnailCache);

    void setFile(const juce::File& file);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    StemLabAudioProcessor& processor;
    int stemIndex = 0;
    bool recursive = false;
    juce::String recursiveId;
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemWaveformComponent)
};

/** One selectable, playable row in the adaptive stem tree. */
class RecursiveStemRowComponent final : public juce::Component
{
public:
    RecursiveStemRowComponent(StemLabAudioProcessor& processor,
                              const StemLabRecursiveStemInfo& info,
                              juce::AudioFormatManager& formatManager,
                              juce::AudioThumbnailCache& thumbnailCache,
                              std::function<void(const juce::String&)> toggleExpanded,
                              std::function<bool(const juce::String&)> isExpanded);

    void setInfo(const StemLabRecursiveStemInfo& info);
    void refresh(bool engineRunning, bool previewPlaying);
    void resized() override;

    juce::String getItemId() const { return item.id; }
    juce::String getRootStem() const { return item.rootStem; }

private:
    void showActionMenu();

    StemLabAudioProcessor& processor;
    StemLabRecursiveStemInfo item;
    juce::ToggleButton selectButton;
    juce::TextButton expandButton{">"};
    juce::TextButton playButton{"Play"};
    juce::TextButton actionButton{"..."};
    std::function<void(const juce::String&)> toggleExpandedCallback;
    std::function<bool(const juce::String&)> isExpandedCallback;
    std::unique_ptr<StemWaveformComponent> waveform;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecursiveStemRowComponent)
};

/**
 * StemLab's complete JUCE interface.
 *
 * This class owns controls and layout only. Audio state and background jobs live
 * in StemLabAudioProcessor; separation algorithms live in the Python package.
 */
class StemLabAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          public juce::FileDragAndDropTarget,
                                          private juce::Timer,
                                          private juce::ChangeListener
{
public:
    explicit StemLabAudioProcessorEditor(StemLabAudioProcessor&);
    ~StemLabAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void chooseEngineExecutable();
    void chooseStandaloneAudioFile();
    bool loadSourceFile(const juce::File& file);
    void chooseSaveFolder();
    void chooseJobRootFolder();
    void showSettingsMenu();
    void showStandaloneAudioSettings();
    void showFirstRunWelcome();
    void launchAbletonSetup();
    void refreshFromProcessor();
    void syncRecursiveRows();
    void showRootRecursiveMenu(int stemIndex);
    bool rootSupportsAdaptiveSplit(int stemIndex) const;
    bool rootHasChildren(int stemIndex) const;
    void toggleRootExpanded(int stemIndex);
    void toggleRecursiveExpanded(const juce::String& itemId);
    bool isRecursiveExpanded(const juce::String& itemId) const;
    std::vector<StemLabRecursiveStemInfo> getVisibleRecursiveItems() const;

    static bool isSupportedAudioFile(const juce::File& file);

    StemLabAudioProcessor& processor;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::TextButton settingsButton{"Settings"};

    juce::TextButton captureButton{"Capture"};
    juce::TextButton recordInputButton{"Record Input"};
    juce::TextButton recordSystemButton{"Record System"};
    juce::TextButton stopButton{"Stop"};
    juce::TextButton playButton{"Play"};
    juce::Label captureTimeLabel;

    juce::ToggleButton refinementButton{"StemLab refinement"};

    juce::TextButton separateButton{"Separate"};

    double progressValue = 0.0;
    juce::ProgressBar progressBar{progressValue};
    juce::Label statusLabel;
    juce::Label timingLabel;

    juce::Label stemsLabel;
    std::array<juce::ToggleButton, StemLabAudioProcessor::stemCount> stemButtons;
    std::array<juce::TextButton, StemLabAudioProcessor::stemCount> stemExpandButtons;
    std::array<juce::TextButton, StemLabAudioProcessor::stemCount> stemPlayButtons;
    std::array<juce::TextButton, StemLabAudioProcessor::stemCount> stemRecursiveButtons;
    std::array<bool, StemLabAudioProcessor::stemCount> rootExpanded{};

    juce::AudioFormatManager waveformFormats;
    juce::AudioThumbnailCache waveformCache{24};

    std::array<std::unique_ptr<StemWaveformComponent>, StemLabAudioProcessor::stemCount>
        waveformComponents;

    juce::Component stemTreeContent;
    juce::Viewport stemViewport;
    juce::StringArray collapsedRecursiveIds;
    std::vector<std::unique_ptr<RecursiveStemRowComponent>> recursiveRows;

    juce::TextButton saveSelectedButton{"Save Selected..."};
    juce::TextButton sendSelectedButton{"Send Selected"};
    juce::TextButton retryImportButton{"Retry"};
    juce::TextButton openJobButton{"Choose File Location"};

    juce::Label bridgeLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    std::unique_ptr<juce::FileChooser> outputFolderChooser;
    std::unique_ptr<juce::FileChooser> jobFolderChooser;

    bool dragActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessorEditor)
};
