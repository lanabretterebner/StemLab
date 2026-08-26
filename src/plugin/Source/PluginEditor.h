#pragma once

#include <JuceHeader.h>
#include <vector>
#include <functional>
#include <utility>
#include "PluginProcessor.h"
#include "SelfFileDragGuard.h"
#include "StemLabLookAndFeel.h"
#include "StemLabWidgets.h"
#include "WaveformCache.h"

/**
 * One lane's waveform well: rounded ground-coloured well, 2px rounded bars
 * from the real audio peaks in the selected palette's full colour, and the
 * shared playhead. Clicks seek the shared transport; dragging exports the
 * stem file to any DAW or file manager.
 */
class StemLaneWaveform final : public juce::Component
{
public:
    StemLaneWaveform(StemLabAudioProcessor& processor, StemLabWaveformCache& waveformCache);

    void setFile(const juce::File& file);
    void setMutedAppearance(bool muted);

    /** Which selection range this lane's drags write to: the stem name for a
        root lane, the item id for an adaptive child. */
    void setSelectionId(const juce::String& id);

    /** Which stem identity this lane draws under the Stem Colours palette
        ("vocals", "drums", ...); a child lane uses its root's. */
    void setStemIdentity(const juce::String& stemName);

    /** Steps the shared waveform zoom by whole detents; wheel events over
        the well land here instead of scrolling the lane list. */
    std::function<void(int)> onZoomStep;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    /** One drawn column: the shape of the audio under it, per channel, and
        the colour that audio calls for. */
    struct Column
    {
        float minimum[2] = {0.0f, 0.0f};
        float maximum[2] = {0.0f, 0.0f};
        float brightness = 0.5f;
        stemlab::waveform::BandLevels bands;
    };

    /** Rebuild the column cache if the view, the size or the file moved. */
    void refreshColumns(juce::Rectangle<float> inner, double viewStart, double viewLength);

    /** Where a point in the well falls in the file, 0 to 1. */
    double normalisedForX(float x) const;

    StemLabAudioProcessor& processor;
    StemLabWaveformCache& waveformCache;

    /** Held rather than re-fetched per column: paint asks the cache once,
        and only while it is still analysing. */
    StemLabWaveformCache::ProfilePtr profile;

    std::vector<Column> columns;

    /*
        What the cached columns were built for. A scrolling view snaps to
        whole pixels, so these change only when the picture genuinely does -
        which is also what stops the waveform crawling.
    */
    juce::File columnsFile;
    double columnsStart = -1.0;
    double columnsLength = -1.0;
    int columnsWidth = 0;
    int columnsChannels = 0;

    juce::File currentFile;
    juce::String stemIdentity;
    juce::String selectionId;
    bool mutedAppearance = false;

    /** An in-progress drag over the well, in normalised file position. */
    bool selecting = false;
    double selectionAnchor = 0.0;
    double selectionHead = 0.0;

    /** Wheel deltas gathered until they amount to a whole zoom detent, so
        trackpads with fine deltas still step at a usable rate. */
    float wheelAccumulator = 0.0f;

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
                      StemLabWaveformCache& waveformCache,
                      std::function<void()> refreshEditor,
                      std::function<void(int)> showRootMenu,
                      std::function<void(const juce::String&)> showChildMenu,
                      std::function<void(int, juce::String)> toggleExpanded);

    ~StemLaneComponent() override { releaseDragSourceGuard(); }

    void refresh();

    /** What a lane menu anchors to, so it opens under the button that
        was clicked rather than against the whole lane row. */
    juce::Component* getMenuButton() const { return menuButton.get(); }

    /** Drives the disclosure twisty: shown only when there is something to
        collapse, pointing down while those children are on screen. */
    void setChildState(bool hasChildren, bool expanded);

    /** Forwards wheel-zoom from this lane's waveform well to the editor's
        shared zoom stepper. */
    void setZoomStepHandler(std::function<void(int)> handler);

    bool isChildLane() const noexcept { return childId.isNotEmpty(); }
    juce::String getChildId() const { return childId; }
    juce::String getRootStem() const { return childInfo.rootStem; }

    void setChildInfo(const StemLabRecursiveStemInfo& info);

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

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
    std::unique_ptr<stemlab::widgets::IconButton> dragButton;
    std::unique_ptr<stemlab::widgets::IconButton> menuButton;
    bool hasChildren = false;
    bool externalDragStarted = false;
    void* dndSourceGuardToken = nullptr;

    void releaseDragSourceGuard();

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
 * StemLab's complete JUCE interface: the Nocturne 1a "Lanes" panel.
 * This class owns controls and layout only.
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

    // The footer's Drag Stems pill (generic VST hosts) starts its export
    // drag from here, the same way the lane handles do.
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // Esc clears every lane's loop range at once.
    bool keyPressed(const juce::KeyPress&) override;

private:
    /** Every selected stem as one external drag, selection ranges included. */
    void startSelectedStemsDrag();

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

    /** Lays out the footer status line and progress row; the rows rearrange
        when a job starts or ends, so it runs on every status refresh. */
    void layoutStatusArea();

    /** A menu window is not a child of the editor, so it would otherwise
        draw with JUCE's default look. Every popup starts here. */
    juce::PopupMenu makeMenu();

    void showEngineMenu();
    void showWaveformColourMenu();
    void setSeparatorEngine(int index);
    void stepSeparatorEngine(int delta);

    /** Include or exclude every lane, roots and adaptive children alike. */
    void setAllLanesIncluded(bool included);

    /** How many lanes are included, and how many there are. */
    std::pair<int, int> laneSelectionCounts() const;

    /** Move the shared waveform zoom by whole detents. */
    void stepWaveformZoom(int delta);

    /** Push a zoom detent to the processor, the readout and the lanes. */
    void applyWaveformZoomIndex(int index);

    void showRootLayersMenu(int stemIndex);

    /** MIDI entries shared by the root and child lane menus. */
    void addMidiMenuItems(juce::PopupMenu& menu, const juce::String& id);
    void handleMidiMenuResult(int result, const juce::String& id, int stemIndex,
                              const juce::String& childId);
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
    std::unique_ptr<stemlab::widgets::IconButton> enginePrevButton;
    std::unique_ptr<stemlab::widgets::SelectorButton> engineSelector;
    std::unique_ptr<stemlab::widgets::IconButton> engineNextButton;
    std::unique_ptr<stemlab::widgets::IconButton> paletteButton;
    std::unique_ptr<stemlab::widgets::IconButton> settingsButton;

    // Which lanes the job carries forward, in one gesture rather than six.
    juce::TextButton selectAllButton{"Select all"};
    juce::TextButton deselectAllButton{"Deselect all"};

    // Feedback for things the user changes - selection, model, palette,
    // transport, rejected clicks. Falls back to the selection count.
    juce::Label userStatusLabel;

    // Horizontal waveform zoom, shared by every lane.
    std::unique_ptr<stemlab::widgets::IconButton> zoomResetButton;
    stemlab::widgets::ZoomSlider zoomSlider;
    juce::Label zoomLabel;

    // Sized once for the longest model name, so switching models does not
    // shuffle the arrows either side of the pill.
    int engineSelectorWidth = 0;

    // Sized once for their own labels and the title, so the header holds
    // still and the user readout gets exactly the space left between them.
    int selectAllWidth = 0;
    int deselectAllWidth = 0;
    int titleWidth = 0;

    // Source strip.
    juce::Label fileNameLabel;
    juce::Label fileMetaLabel;
    juce::TextButton captureButton;
    stemlab::widgets::RecordButton recordSystemButton{"Record PC"};
    stemlab::widgets::RecordButton recordInputButton{"Record In"};
    stemlab::widgets::SeparateSplitControl separateControl;

    // Lanes.
    juce::AudioFormatManager waveformFormats;
    StemLabWaveformCache waveformProfiles{waveformFormats};
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

    // The header readout shows a freshly posted user-action message for a
    // few seconds before reverting to the selection count. The revision
    // (not the text) marks freshness, so an identical message posted again
    // - a second rejected click - restarts the clock.
    int lastActionStatusRevision = 0;
    juce::uint32 actionStatusShownMs = 0;

    int lastSeparatorEngine = -1;
    bool lastSeparateGlow = false;
    bool lastPrimaryGlow = false;

    // Footer.
    stemlab::widgets::FadingDivider footerDivider;
    stemlab::widgets::StatusIndicator statusIndicator;
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
    juce::Rectangle<int> sourceDividerBounds;
    juce::Rectangle<int> folderIconBounds;
    juce::Rectangle<int> statusAreaBounds;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    std::unique_ptr<juce::FileChooser> outputFolderChooser;
    std::unique_ptr<juce::FileChooser> jobFolderChooser;

    bool dragActive = false;

    // Files of our own in-flight outbound drag, so releasing them back
    // over the window cannot reload the source they were split from.
    StemLabSelfFileDragGuard selfFileDragGuard;
    bool footerDragStarted = false;
    void* dndSourceGuardToken = nullptr;

    void releaseDragSourceGuard();

    /** What the action segment last rendered as, so a click acts on the
        state the user actually saw. */
    bool separateControlShowsCancel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessorEditor)
};
