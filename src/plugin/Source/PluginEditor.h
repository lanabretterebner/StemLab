#pragma once

#include <JuceHeader.h>
#include <vector>
#include <functional>
#include <map>
#include <utility>
#include "PluginProcessor.h"
#include "SelfFileDragGuard.h"
#include "StemLabLookAndFeel.h"
#include "SettingsPanel.h"
#include "StemLabWidgets.h"
#include "WaveformCache.h"

/**
 * One lane's waveform well: rounded ground-colored well, 2px rounded bars
 * from the real audio peaks in the selected palette's full color, and the
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

    /** Which stem identity this lane draws under the Stem Color palette
        ("vocals", "drums", ...); a child lane uses its root's. */
    void setStemIdentity(const juce::String& stemName);

    /** Steps the shared waveform zoom by whole detents; wheel events over
        the well land here instead of scrolling the lane list. */
    std::function<void(int)> onZoomStep;

    /** Fired after this well has moved the shared transport - a click-seek
        or a swept loop range. Only the clicked well knows about it
        synchronously; every other lane's playhead, the clock and the
        scrubber would otherwise wait for the editor's next tick, which is
        half a second away once the editor has demoted itself. */
    std::function<void()> onTransportSeek;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** The editor's UI timer calls this instead of a blanket repaint: it asks
        the processor what the well would show now and repaints only when that
        differs from the last tick - and when just the playhead moved, only
        the two thin strips it left and entered. A still lane costs nothing.

        Returns whether anything actually changed. The editor uses that to
        hold its full refresh rate while wells are still coming to life -
        an arriving analysis is the one thing here that is not driven by a
        user action or by a processor state the editor already polls. */
    bool timerRefresh();

private:
    /** One drawn column: the shape of the audio under it, per channel, and
        the color that audio calls for. */
    struct Column
    {
        float minimum[2] = {0.0f, 0.0f};
        float maximum[2] = {0.0f, 0.0f};
        float brightness = 0.5f;
        stemlab::waveform::BandLevels bands;
    };

    /** The well interior and the view window with its start snapped to a
        whole column of time; viewLength is 0 when there is nothing to draw.
        Snapping is what keeps a scrolling view from re-bucketing the audio
        every frame (each column would cover a slightly different span and
        the whole waveform would crawl and shimmer): snapped, the picture
        translates by whole columns and every column keeps its audio. */
    struct ViewGeometry
    {
        juce::Rectangle<float> inner;
        double snappedStart = 0.0;
        double viewLength = 0.0;
    };

    /** Geometry for a given view window, so paint and timerRefresh frame
        the same captured window rather than each reading a fresher one. */
    ViewGeometry viewGeometryFor(double viewStart, double viewLength) const;

    /** Rebuild the column cache if the view, the size or the file moved. */
    void refreshColumns(juce::Rectangle<float> inner, double viewStart, double viewLength);

    /** Draw columns [first, first + count) into columnImage. */
    void renderColumnStrip(int first, int count);

    /** Where a point in the well falls in the file, 0 to 1. */
    double normalisedForX(float x) const;

    /** Ask the cache for currentFile's profile and arm the next poll.
        True when a profile landed. */
    bool fetchProfile();

    StemLabAudioProcessor& processor;
    StemLabWaveformCache& waveformCache;

    /** Held rather than re-fetched per column: paint asks the cache once,
        and only while it is still analysing. */
    StemLabWaveformCache::ProfilePtr profile;

    std::vector<Column> columns;

    /** One beat-grid number waiting to be drawn. The rules belong behind
        the audio, but the numbers have to survive it, so they are gathered
        on the way through the grid and painted after the waveform blit
        rather than under it. Held here rather than made per paint so a
        repaint at the UI rate does not churn the heap. */
    struct GridLabel
    {
        juce::Rectangle<float> bounds;
        juce::String text;
        bool bar = false;
    };

    std::vector<GridLabel> gridLabels;

    /** The columns pre-rendered as pixels, so a paint is one blit instead of
        thousands of one-pixel fills. When the view slides by whole columns
        (zoomed playback) the image scrolls and only the newly exposed
        columns are recomputed and drawn. */
    juce::Image columnImage;

    /*
        What the cached columns were built for. A scrolling view snaps to
        whole pixels, so these change only when the picture genuinely does -
        which is also what stops the waveform crawling.
    */
    juce::File columnsFile;
    double columnsStart = -1.0;
    double columnsLength = -1.0;
    int columnsWidth = 0;
    int columnsHeight = 0;
    int columnsChannels = 0;
    int columnsPalette = -1;

    /*  The accent the cached image was drawn with.
     *
     *  Tracked separately from the palette because the Accent palette draws
     *  with whatever accent is set: changing the accent leaves the palette
     *  index, the file, the size and the view identical, so without this the
     *  cached pixels are reused and the waveform keeps the old colour until
     *  something else happens to invalidate it.
     */
    int columnsAccent = -1;

    bool columnsMuted = false;
    juce::String columnsIdentity;

    /** Everything the well drew at the last timer tick, so timerRefresh can
        tell a genuinely changed picture from one that only needs its
        playhead strip refreshed - or nothing at all. */
    struct DisplayState
    {
        const void* profilePtr = nullptr;
        double viewStart = 0.0;
        double viewLength = 0.0;
        double transportPosition = 0.0;
        double transportLength = 0.0;
        double gridBpm = 0.0;
        double gridBarOne = 0.0;
        int gridNumerator = 0;
        int palette = 0;

        /*  The accent, for the same reason the palette is here: the Accent
            palette draws with whichever accent is set, so a change to it
            changes the picture without moving anything else in this struct.
            Without it the tick decides nothing happened and never repaints,
            and the cache key on columnsAccent is never even consulted.
        */
        int accent = 0;

        bool selectionActive = false;
        double selectionStart = 0.0;
        double selectionEnd = 0.0;

        /** Cheap stand-in for "has the conversion changed": a count, never
            the notes. A conversion landing, being redone, or the lane being
            reused for a stem with none all move it. */
        size_t midiNoteCount = 0;
    };

    /** One live read of everything the well draws. */
    DisplayState readDisplayState() const;

    /*
        paint draws lastDisplay, never a fresher read: a strip repaint's clip
        only covers where the captured state put the playhead, so reading the
        transport again at paint time would draw it where the clip may not
        reach and erase it where it never was - ghost playheads, worst zoomed
        in at the file's ends where the view is pinned while the playhead
        crosses pixels fastest.
    */
    DisplayState lastDisplay;
    bool lastDisplayValid = false;

    /*
        The notes this well draws over its audio, copied once per conversion
        rather than per paint: getMidiInfo returns the whole vector by value,
        and a busy stem carries thousands of them.
    */
    std::vector<StemLabMidiNoteInfo> midiNotes;
    juce::String midiNotesId;
    size_t midiNotesCount = 0;

    /** Re-copies midiNotes when lastDisplay says the conversion moved. */
    void refreshMidiNotes();

    juce::File currentFile;

    /** Whether currentFile was seen on disk. It only changes through
        setFile, and File::existsAsFile is a syscall the waiting path would
        otherwise pay at the UI rate. A stem announced before it is written
        is the one case this starts out wrong, and only in that direction,
        so the poll below re-checks until it turns true. */
    bool currentFileExists = false;

    /** Whether the cache has been asked for currentFile yet. paint asks
        once, so a profile already analysed draws on the first frame; every
        ask after that belongs to the rate-limited poll. */
    bool profileRequested = false;

    /** When the cache was last asked about currentFile. */
    juce::uint32 lastProfilePollMs = 0;

    /*
        Analysis completion is not signalled, so a lane without a profile
        polls the cache - and each ask costs a handful of stats, in a
        window where every lane is waiting because stems now arrive while
        later ones are still separating. This trades poll latency for that
        traffic.

        Denominated in milliseconds rather than ticks on purpose: the
        editor's timer changes rate (see theme::metrics::uiIdleRefreshHz),
        and a cadence counted in ticks would have quietly become ten times
        slower the moment it did.
    */
    static constexpr int profilePollIntervalMs = 200;

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

    ~StemLaneComponent() override = default;

    void refresh();

    /** Forwards the editor's UI tick to the waveform well's change-detecting
        refresh; the lane's own widgets repaint through their setters.
        Returns whether the well actually redrew anything. */
    bool timerRefreshWaveform()
    {
        return waveform != nullptr && waveform->timerRefresh();
    }

    /** What a lane menu anchors to, so it opens under the button that
        was clicked rather than against the whole lane row. */
    juce::Component* getMenuButton() const { return menuButton.get(); }

    /** Drives the disclosure twisty: shown only when there is something to
        collapse, pointing down while those children are on screen.
        hiddenActivity marks a collapsed row whose hidden descendants are
        soloed or muted, so state cannot disappear with the rows. */
    void setChildState(bool hasChildren, bool expanded,
                       bool hiddenActivity = false, bool hiddenSolo = false);

    /** Forwards wheel-zoom from this lane's waveform well to the editor's
        shared zoom stepper. */
    void setZoomStepHandler(std::function<void(int)> handler);

    /** Forwards a seek made in this lane's well to the editor, so the rest
        of the interface catches up in the same event rather than on the
        next timer tick. */
    void setTransportSeekHandler(std::function<void()> handler);

    bool isChildLane() const noexcept { return childId.isNotEmpty(); }
    juce::String getChildId() const { return childId; }
    juce::String getRootStem() const { return childInfo.rootStem; }

    void setChildInfo(const StemLabRecursiveStemInfo& info);

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

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

    /** Only present once this lane has been converted, and it carries the
        .mid rather than the audio. A second handle rather than a modifier
        on the first: a modifier that changes what a drag delivers is
        invisible until after the drop. */
    std::unique_ptr<stemlab::widgets::IconButton> midiDragButton;

    /** What refresh() last showed the MIDI handle for, so a conversion
        landing (or a lane being reused for another stem) re-lays the row
        out instead of leaving a handle that drags the wrong file. */
    bool midiHandleShown = false;
    std::unique_ptr<stemlab::widgets::IconButton> menuButton;
    bool hasChildren = false;
    bool externalDragStarted = false;
    bool hiddenDescendantActive = false;
    bool hiddenDescendantSoloed = false;

    /** Whether the pointer is anywhere in this row, children included.
        Cached rather than read in paint(), so a crossing that changes it
        can schedule the repaint that redraws it. */
    bool hovered = false;
    void updateHover();

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


    /** Open the Model Manager, or bring what it shows up to date. */
    void wireSettingsPage();
    void refreshSettingsPage();
    void showSettingsPanel(stemlab::widgets::SettingsPanel::Page page);
    void closeSettingsPanel();
    void refreshSettingsPanel();

    /** Decides whether the manager should let itself in on this refresh. */
    void considerAutoShowingModelManager();
    void chooseStandaloneAudioFile();
    bool loadSourceFile(const juce::File& file);
    void chooseSaveFolder();
    void chooseJobRootFolder();

    /** Show the job output folder in the desktop's file manager. */
    void revealJobFolder();
    void showSettingsMenu();
    void showStandaloneAudioSettings();

    /** Ask for the beat grid's tempo, and switch to the manual grid on OK. */
    void promptForManualTempo();

    /** update.sh, as the bundle's install.sh leaves it beside the app. */
    static juce::File updaterScript();
    /** Runs it with --check, off the message thread. */
    void checkForUpdates();
    /** Shows what --check said, plus the command that would install it. */
    void showUpdateCheckResult(const juce::String& scriptPath,
                               const juce::String& output);
    /** One check at a time; the row stays pressable while one is in flight. */
    bool updateCheckRunning = false;
    void launchAbletonSetup();
    void refreshFromProcessor();

    /** Retunes the UI timer, cheaply: a no-op unless the rate actually
        moves, so the steady state never resets JUCE's timer counter. */
    void applyRefreshRate(int hz);

    /** Hold the full refresh rate for theme::metrics::uiIdleHoldMs. For
        anything that changes the interface without going through
        refreshFromProcessor's own decision - an asynchronous announcement
        from the processor, a seek, an analysis landing. */
    void requestFastFrames();

    /** Ticks every lane's waveform well. Returns whether any of them
        actually redrew, which is what keeps the editor at full rate while
        stems are still arriving. */
    bool refreshLaneWaveforms();

    /** Someone moved the shared playhead from inside the interface. Brings
        the whole editor - every lane, the clock, the scrubber - level with
        it in the same event instead of on the next tick. */
    void handleTransportMoved();

    /** Draws and lays out the panel in its own (design-size) coordinates. */
    void paintPanel(juce::Graphics&);
    void layoutPanel();

    /** The accent glow behind a primary action, from a cache keyed by size:
        juce::DropShadow re-runs its Gaussian blur on every draw call, which
        is pure waste for a glow that only changes when the layout does. */
    void drawCachedGlow(juce::Graphics&, juce::Rectangle<int> area);

    std::map<std::pair<int, int>, juce::Image> glowCache;

    /** Lays out the footer status line and progress row; the rows rearrange
        when a job starts or ends, so it runs on every status refresh. */
    void layoutStatusArea();

    /** A menu window is not a child of the editor, so it would otherwise
        draw with JUCE's default look. Every popup starts here. */
    juce::PopupMenu makeMenu();

    void showEngineMenu();
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
    /** Filters an already-fetched tree, so the caller pays for one
        getRecursiveStemItems() rather than a second lock-and-copy. */
    std::vector<StemLabRecursiveStemInfo>
    getVisibleRecursiveItems(const std::vector<StemLabRecursiveStemInfo>& all) const;
    void syncLanes();

    juce::String jobSummaryLine() const;
    juce::String displayPath(const juce::File& directory) const;

    static bool isSupportedAudioFile(const juce::File& file);

    StemLabAudioProcessor& processor;

    /*
        Shared across every open editor: the constructor registers the two
        Inter faces, which on the FreeType backend drags in a system font
        directory scan. Declared here, ahead of every component that can
        point at it, so the last reference outlives the last of them.
    */
    juce::SharedResourcePointer<StemLabLookAndFeel> lookAndFeel;

    juce::TooltipWindow tooltipWindow{this};

    // Declared before every control below: they are its children, so it has
    // to outlive them.
    StemLabPanelContent panelContent;

    /*
     * A child of panelContent, not of the editor: it inherits the panel's
     * scale transform that way, so it needs no sizing rules of its own and
     * looks the same at every window size.
     */
    stemlab::widgets::SettingsPanel settingsPanel;

    /*
     * Dismissal lasts for the session, not forever. Nothing is persisted: a
     * user who wants to look around first should not be asked again this
     * run, but one who quits with nothing downloaded should be met by it
     * next time rather than by a separation that cannot work.
     */
    bool modelManagerDismissed = false;
    bool modelManagerAutoShown = false;

    /*
     * Set once a model job has run while the manager was open. The outcome of
     * one lands on the processor's status line, which is behind the overlay's
     * scrim and unreadable there, so the panel keeps showing that line after
     * the job ends rather than clearing it the moment it stops being busy.
     */
    bool modelJobReported = false;

    // Header.
    juce::Label titleLabel;
    std::unique_ptr<stemlab::widgets::IconButton> enginePrevButton;
    std::unique_ptr<stemlab::widgets::SelectorButton> engineSelector;
    std::unique_ptr<stemlab::widgets::IconButton> engineNextButton;
    std::unique_ptr<stemlab::widgets::IconButton> settingsButton;

    // Which lanes the job carries forward, in one gesture rather than six.
    juce::TextButton selectAllButton{"Select all"};
    juce::TextButton deselectAllButton{"Deselect all"};

    // Feedback for things the user changes - selection, model, palette,
    // transport, rejected clicks. Falls back to the selection count.
    juce::Label userStatusLabel;

    // Horizontal waveform zoom, shared by every lane.
    std::unique_ptr<stemlab::widgets::IconButton> zoomResetButton;

    /*  The folder beside the footer path. It was painted straight onto
        panelContent and so could not be clicked; as a button it hit-tests,
        takes a hover cursor and carries a tooltip. Its bounds still track
        the right-aligned path text, which is why they are set in the
        refresh rather than in resized(). */
    std::unique_ptr<stemlab::widgets::IconButton> openFolderButton;
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

    // Lanes. The waveform cache lives on the processor so profiles survive
    // the editor - reopening the window does not re-read and re-FFT every
    // stem; the lanes borrow it through this reference.
    StemLabWaveformCache& waveformProfiles;
    juce::Viewport laneViewport;
    juce::Component laneContent;
    std::array<std::unique_ptr<StemLaneComponent>, StemLabAudioProcessor::stemCount> rootLanes;
    std::vector<std::unique_ptr<StemLaneComponent>> childLanes;
    std::array<bool, StemLabAudioProcessor::stemCount> rootExpanded{};
    juce::StringArray collapsedRecursiveIds;

    /** Lanes (root stem names and child item ids) that are collapsed and
        are hiding a soloed or muted descendant, and whether the hidden
        state includes a solo. Rebuilt by syncLanes(), read by both lane
        loops in refreshFromProcessor(). */
    juce::StringArray hiddenActiveParents;
    juce::StringArray hiddenSoloParents;
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

    // Rate-limits the Ableton bridge-status poll: status text does not need
    // 50 ms latency, and the poll costs file I/O. A wall-clock deadline
    // rather than a tick divider, because the UI timer changes rate.
    juce::uint32 lastAbletonStatusPollMs = 0;

    /** What startTimerHz was last given; 0 until the constructor arms it.
        The editor runs at theme::metrics::uiRefreshHz while something can
        change on its own and drops to uiIdleRefreshHz when nothing can. */
    int currentRefreshHz = 0;

    /** Full rate is held until this stamp. juce::uint32 like every other
        millisecond counter here, and compared through a signed difference
        so the ~49-day wrap does not read as "forever". */
    juce::uint32 fastFramesUntilMs = 0;

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
    bool lastStatusWasError = false;
    double progressValue = 0.0;
    juce::ProgressBar progressBar{progressValue};
    juce::Label progressLabel;
    juce::Label pathLabel;

    /** The last measured job path, and the width its text shaped to inside
        a label of lastJobPathLabelWidth. Shaping a whole path through
        GlyphArrangement on every refresh - to place one folder icon - is
        pure waste for a string that only changes when the user picks a
        different job folder. */
    juce::String lastJobPath;
    int lastJobPathWidth = 0;
    int lastJobPathLabelWidth = 0;

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

    /** What the action segment last rendered as, so a click acts on the
        state the user actually saw. */
    bool separateControlShowsCancel = false;
    juce::String lastSeparateActionText;

    /** When the action segment last changed from "Separate" to "Cancel".
        A click that lands inside the double-click window after that change
        was aimed at the label the user could still see, not at Cancel. */
    juce::uint32 separateCancelArmedMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessorEditor)
};
