#pragma once

#include <JuceHeader.h>
#include <vector>
#include <functional>
#include "PluginProcessor.h"
#include "StemLabLookAndFeel.h"
#include "StemLabWidgets.h"

/**
 * One lane's waveform well: rounded ground-coloured well, 2px rounded bars
 * from the real audio peaks (played portion in accent, unplayed neutral),
 * and the shared playhead. Clicks seek the shared transport; dragging
 * exports the stem file to any DAW or file manager.
 */
class StemLaneWaveform final : public juce::Component
{
public:
    StemLaneWaveform(StemLabAudioProcessor& processor, juce::AudioFormatManager& formatManager,
                     juce::AudioThumbnailCache& thumbnailCache);

    void setFile(const juce::File& file);
    void setMutedAppearance(bool muted);

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    StemLabAudioProcessor& processor;
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;
    bool mutedAppearance = false;
    bool externalDragStarted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLaneWaveform)
};

/**
 * One stem lane: include checkbox | name | waveform well | controls.
 *
 * Root lanes (stemIndex 0..5) carry Solo, Mute, and the adaptive-split
 * layers menu. Adaptive child lanes (childId non-empty) indent under their
 * root and carry an exclusive-audition Solo plus their own layers menu.
 */
class StemLaneComponent final : public juce::Component
{
public:
    StemLaneComponent(StemLabAudioProcessor& processor, int stemIndex, juce::String childId,
                      juce::AudioFormatManager& formatManager,
                      juce::AudioThumbnailCache& thumbnailCache,
                      std::function<void()> refreshEditor,
                      std::function<void(int)> showRootMenu,
                      std::function<void(const juce::String&)> showChildMenu);

    void refresh();

    /** Root lanes: whether the layers menu has anything to offer (adaptive
        split supported, or children exist). Hidden otherwise (e.g. Bass). */
    void setLayersAvailable(bool available);

    bool isChildLane() const noexcept { return childId.isNotEmpty(); }
    juce::String getChildId() const { return childId; }
    juce::String getRootStem() const { return childInfo.rootStem; }

    void setChildInfo(const StemLabRecursiveStemInfo& info);

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    StemLabAudioProcessor& processor;
    int stemIndex;
    juce::String childId;
    StemLabRecursiveStemInfo childInfo;
    juce::File laneFile;

    stemlab::widgets::IncludeCheckbox include;
    juce::Label nameLabel;
    std::unique_ptr<StemLaneWaveform> waveform;
    juce::TextButton soloButton{"S"};
    juce::TextButton muteButton{"M"};
    std::unique_ptr<stemlab::widgets::IconButton> layersButton;
    bool layersAvailable = true;

    std::function<void()> refreshEditor;
    std::function<void(int)> showRootMenu;
    std::function<void(const juce::String&)> showChildMenu;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLaneComponent)
};

/**
 * StemLab's complete JUCE interface: the Nocturne 1a "Lanes" panel
 * (docs/redesign/README.md). This class owns controls and layout only.
 * Audio state and background jobs live in StemLabAudioProcessor;
 * separation algorithms live in the Python package.
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

    void showRootLayersMenu(int stemIndex);
    void showChildLayersMenu(const juce::String& itemId);
    bool rootSupportsAdaptiveSplit(int stemIndex) const;
    bool rootHasChildren(int stemIndex) const;
    void toggleRootExpanded(int stemIndex);
    std::vector<StemLabRecursiveStemInfo> getVisibleRecursiveItems() const;
    void syncLanes();

    juce::String jobSummaryLine() const;
    juce::String displayPath(const juce::File& directory) const;

    static bool isSupportedAudioFile(const juce::File& file);

    StemLabAudioProcessor& processor;

    StemLabLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow{this};

    // Header.
    juce::Label titleLabel;
    std::unique_ptr<stemlab::widgets::IconButton> settingsButton;

    // Source strip.
    juce::Label fileNameLabel;
    juce::Label fileMetaLabel;
    juce::TextButton captureButton;
    stemlab::widgets::RecordButton recordSystemButton{"Record PC"};
    stemlab::widgets::RecordButton recordInputButton{"Record In"};
    stemlab::widgets::SeparateSplitControl separateControl;

    // Lanes.
    juce::AudioFormatManager waveformFormats;
    juce::AudioThumbnailCache waveformCache{
        stemlab::theme::metrics::waveform::thumbnailCacheSize};
    juce::Viewport laneViewport;
    juce::Component laneContent;
    std::array<std::unique_ptr<StemLaneComponent>, StemLabAudioProcessor::stemCount> rootLanes;
    std::vector<std::unique_ptr<StemLaneComponent>> childLanes;
    std::array<bool, StemLabAudioProcessor::stemCount> rootExpanded{};
    juce::StringArray collapsedRecursiveIds;
    void layoutLanes();

    // Transport.
    stemlab::widgets::PlayCircleButton playButton;
    juce::Label timeLabel;
    stemlab::widgets::Scrubber scrubber;
    stemlab::widgets::SegmentedControl abControl{"Original", "Stems"};
    bool sawSuccessfulJob = false;

    // Keeps transient status messages visible for a few seconds after a
    // job is done, before the summary line takes back over.
    juce::String lastRawStatus;
    juce::uint32 lastStatusChangeMs = 0;

    int lastSeparatorEngine = -1;
    bool lastSeparateGlow = false;
    bool lastPrimaryGlow = false;

    // Footer.
    stemlab::widgets::FadingDivider footerDivider;
    juce::Label statusLabel;
    double progressValue = 0.0;
    juce::ProgressBar progressBar{progressValue};
    juce::Label progressLabel;
    juce::Label pathLabel;
    juce::TextButton changeFolderButton{"Change"};
    juce::TextButton saveButton{"Save Stems"};
    juce::TextButton retryButton{"Retry"};
    juce::TextButton insertButton;

    // Layout rectangles paint() needs (computed in resized()).
    juce::Rectangle<int> panelBounds;
    juce::Rectangle<int> brandGlyphBounds;
    juce::Rectangle<int> sourceStripBounds;
    juce::Rectangle<int> statusIconBounds;
    juce::Rectangle<int> folderIconBounds;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    std::unique_ptr<juce::FileChooser> outputFolderChooser;
    std::unique_ptr<juce::FileChooser> jobFolderChooser;

    bool dragActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessorEditor)
};
