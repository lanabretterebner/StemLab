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

    /** Which stem identity this lane draws under the Stem Colours palette
        ("vocals", "drums", ...); a child lane uses its root's. */
    void setStemIdentity(const juce::String& stemName);

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    StemLabAudioProcessor& processor;
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;
    juce::String stemIdentity;
    bool mutedAppearance = false;
    bool externalDragStarted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLaneWaveform)
};

/**
 * One stem lane: twisty | include checkbox | name | waveform well | controls.
 *
 * Root lanes (stemIndex 0..5) and adaptive child lanes (childId non-empty)
 * carry the same controls - Solo, Mute, and the adaptive-split layers menu -
 * because a child stands in for its parent in the monitor mix and needs the
 * same reach over it. Child lanes indent under their root, and any lane with
 * children shows the disclosure twisty that collapses them.
 */
class StemLaneComponent final : public juce::Component
{
public:
    StemLaneComponent(StemLabAudioProcessor& processor, int stemIndex, juce::String childId,
                      juce::AudioFormatManager& formatManager,
                      juce::AudioThumbnailCache& thumbnailCache,
                      std::function<void()> refreshEditor,
                      std::function<void(int)> showRootMenu,
                      std::function<void(const juce::String&)> showChildMenu,
                      std::function<void(int, juce::String)> toggleExpanded);

    void refresh();

    /** Root lanes: whether the layers menu has anything to offer (adaptive
        split supported, or children exist). Hidden otherwise (e.g. Bass). */
    void setLayersAvailable(bool available);

    /** Drives the disclosure twisty: shown only when there is something to
        collapse, pointing down while those children are on screen. */
    void setChildState(bool hasChildren, bool expanded);

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

    stemlab::widgets::DisclosureButton twisty;
    stemlab::widgets::IncludeCheckbox include;
    juce::Label nameLabel;
    std::unique_ptr<StemLaneWaveform> waveform;
    juce::TextButton soloButton{"S"};
    juce::TextButton muteButton{"M"};
    std::unique_ptr<stemlab::widgets::IconButton> layersButton;
    bool layersAvailable = true;
    bool hasChildren = false;

    std::function<void()> refreshEditor;
    std::function<void(int)> showRootMenu;
    std::function<void(const juce::String&)> showChildMenu;
    std::function<void(int, juce::String)> toggleExpanded;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLaneComponent)
};

/**
 * The panel itself, as one component that is laid out at the fixed design
 * size and then scaled by the editor.
 *
 * Every control lives in here rather than directly on the editor, so
 * resizing is a single AffineTransform instead of a second, size-dependent
 * set of layout rules: StemLabTheme's metrics stay real pixels. The editor
 * keeps the drawing and layout code and hands it over through these hooks.
 */
class StemLabPanelContent final : public juce::Component
{
public:
    StemLabPanelContent() = default;

    std::function<void(juce::Graphics&)> onPaint;

    void paint(juce::Graphics& g) override
    {
        if (onPaint)
            onPaint(g);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabPanelContent)
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

    /** Draws and lays out the panel in its own (design-size) coordinates. */
    void paintPanel(juce::Graphics&);
    void layoutPanel();

    void showRootLayersMenu(int stemIndex);
    void showChildLayersMenu(const juce::String& itemId);
    bool rootSupportsAdaptiveSplit(int stemIndex) const;
    bool rootHasChildren(int stemIndex) const;
    void toggleRootExpanded(int stemIndex);
    void toggleChildExpanded(const juce::String& itemId);

    /** One entry point for both twisties and both menu items. */
    void toggleLaneExpanded(int stemIndex, const juce::String& childId);
    bool isLaneExpanded(int stemIndex, const juce::String& childId) const;
    std::vector<StemLabRecursiveStemInfo> getVisibleRecursiveItems() const;
    void syncLanes();

    juce::String jobSummaryLine() const;
    juce::String displayPath(const juce::File& directory) const;

    static bool isSupportedAudioFile(const juce::File& file);

    StemLabAudioProcessor& processor;

    StemLabLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow{this};

    // Declared before every control below: they are its children, so it has
    // to outlive them.
    StemLabPanelContent panelContent;

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

    /** What the action segment last rendered as, so a click acts on the
        state the user actually saw. */
    bool separateControlShowsCancel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessorEditor)
};
