#pragma once

#include <JuceHeader.h>
#include <vector>
#include <functional>
#include "PluginProcessor.h"
#include "SelfFileDragGuard.h"
#include "StemLabLookAndFeel.h"


/** Toggle used for export selection. Right-click invokes a separate solo callback. */
class StemSelectToggleButton final : public juce::ToggleButton
{
public:
    std::function<void()> onRightClick;

    void mouseDown(const juce::MouseEvent& event) override
    {
        suppressRightMouseUp = event.mods.isRightButtonDown();
        if (suppressRightMouseUp)
        {
            if (onRightClick)
                onRightClick();
            return;
        }
        juce::ToggleButton::mouseDown(event);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (suppressRightMouseUp || event.mods.isRightButtonDown())
        {
            suppressRightMouseUp = false;
            return;
        }
        juce::ToggleButton::mouseUp(event);
    }

private:
    bool suppressRightMouseUp = false;
};

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

    /** Put this lane on the shared zoom window, so the stack agrees. */
    void applySharedZoom();
    void setResizeCallback(std::function<void(int, bool)> callback);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    StemLabAudioProcessor& processor;
    int stemIndex = 0;
    bool recursive = false;
    juce::String recursiveId;
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;
    double viewStart = 0.0;
    double viewEnd = 1.0;
    double panStart = 0.0;
    float panMouseX = 0.0f;
    bool panning = false;
    bool selecting = false;
    bool selectionMoved = false;
    double selectionAnchor = 0.0;
    float selectionMouseX = 0.0f;
    bool resizing = false;
    float resizeMouseY = 0.0f;
    int resizeStartHeight = 58;
    std::function<void(int, bool)> resizeCallback;

    juce::String selectionId() const;
    double normalisedPositionForX(float x) const;

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
                              std::function<bool(const juce::String&)> isExpanded,
                              std::function<void()> laneResized);

    void setInfo(const StemLabRecursiveStemInfo& info);
    void refresh(bool engineRunning, bool previewPlaying);
    void resized() override;

    juce::String getItemId() const { return item.id; }
    juce::String getRootStem() const { return item.rootStem; }

private:
    void showActionMenu();

    StemLabAudioProcessor& processor;
    StemLabRecursiveStemInfo item;
    StemSelectToggleButton selectButton;
    juce::TextButton expandButton{">"};
    juce::TextButton playButton{"Play"};
    juce::TextButton actionButton{"..."};
    std::function<void(const juce::String&)> toggleExpandedCallback;
    std::function<bool(const juce::String&)> isExpandedCallback;
    std::function<void()> laneResizedCallback;
    std::unique_ptr<StemWaveformComponent> waveform;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecursiveStemRowComponent)
};

/**
 * FI-STEM's complete JUCE interface.
 *
 * This class owns controls and layout only. Audio state and background jobs live
 * in the audio processor; separation algorithms live in the Python package.
 */
/** Draws one vector glyph from stemlab::icons at a colour the theme owns. */
class IconComponent final : public juce::Component
{
public:
    using Painter = std::function<juce::Path(juce::Rectangle<float>)>;

    IconComponent() = default;

    /** ``stroked`` glyphs are drawn as outlines - a magnifier's lens and
        handle are strokes, and filling that path collapses it to a blob. */
    void setIcon(Painter painter, juce::Colour colourIn, bool stroked = false)
    {
        paint_ = std::move(painter);
        colour = colourIn;
        stroke = stroked;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (!paint_)
            return;

        const auto path = paint_(getLocalBounds().toFloat().reduced(1.5f));

        g.setColour(colour);

        if (stroke)
            g.strokePath(path, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        else
            g.fillPath(path);
    }

private:
    Painter paint_;
    juce::Colour colour;
    bool stroke = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconComponent)
};

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
    bool keyPressed(const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void chooseEngineExecutable();
    void chooseStandaloneAudioFile();
    void chooseHostAudioFile();
    void chooseSaveFolder();
    void chooseJobRootFolder();
    void showSettingsMenu();
    void showManualGridDialog();
    void showAnalysisCorrectionDialog();
    void showStandaloneAudioSettings();
    void showFirstRunWelcome();
    void launchAbletonSetup();
    void startExternalStemDrag(juce::Component* source);
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

    /*
        Declared before every component below, and that order is load-bearing:
        members are destroyed in reverse, so the look-and-feel outlives the
        widgets pointing at it. Shared because registering the bundled
        typefaces is worth doing once per process rather than once per editor.
    */
    juce::SharedResourcePointer<StemLabLookAndFeel> lookAndFeel;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::TextButton settingsButton{"Settings"};

    // Header bar. Every control here drives state the processor already
    // keeps - stem enablement, the shared zoom, the separator engine index,
    // the waveform palette - so none of it is decorative.
    IconComponent brandGlyph;
    juce::TextButton selectAllButton{"Select all"};
    juce::TextButton deselectAllButton{"Deselect all"};
    juce::Label selectionCountLabel;
    IconComponent zoomGlyph;
    juce::Slider zoomSlider{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    juce::Label zoomReadoutLabel;
    juce::TextButton enginePrevButton{"<"};
    juce::TextButton engineNextButton{">"};
    juce::Label engineLabel;
    IconComponent settingsGlyph;

    // Source card. The filename and its duration stack at the left of the
    // card, where upstream had them inline in a status string.
    // Where paint() draws the source card behind its controls.
    juce::Rectangle<int> sourceCardBounds;
    juce::Rectangle<int> transportBarBounds;

    juce::Label sourceNameLabel;
    juce::Label sourceLengthLabel;

    // Transport. Position, length and seeking all come from the preview
    // transport upstream already runs; nothing here is new state.
    IconComponent transportGlyph;
    juce::TextButton transportButton;
    juce::Label transportTimeLabel;
    juce::Slider transportScrubber{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};

    // Footer.
    IconComponent statusGlyph;
    juce::Label outputPathLabel;

    juce::TextButton captureButton{"Capture"};
    juce::TextButton importFromPcButton{"Import from PC"};
    juce::TextButton recordInputButton{"Record Input"};
    juce::TextButton recordSystemButton{"Record System"};
    juce::TextButton stopButton{"Stop"};
    juce::TextButton playButton{"Play"};
    juce::Label captureTimeLabel;
    juce::TextButton analysisDetailsButton{"Analysis Details"};

    juce::ToggleButton refinementButton{"FI-STEM refinement"};
    juce::ToggleButton beatThisButton{"Beat This! analysis"};

    juce::TextButton separateButton{"Separate"};
    juce::TextButton cancelButton{"Cancel"};

    double progressValue = 0.0;
    juce::ProgressBar progressBar{progressValue};
    juce::Label statusLabel;
    juce::Label timingLabel;

    juce::Label stemsLabel;
    std::array<juce::TextButton, 3> gridModeButtons;
    std::array<StemSelectToggleButton, StemLabAudioProcessor::stemCount> stemButtons;
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
    juce::TextButton dragSelectedButton{"Drag Selected"};
    juce::TextButton retryImportButton{"Retry"};
    juce::TextButton openJobButton{"Change"};

    juce::Label bridgeLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::FileChooser> audioFileChooser;
    std::unique_ptr<juce::FileChooser> outputFolderChooser;
    std::unique_ptr<juce::FileChooser> jobFolderChooser;

    StemLabSelfFileDragGuard selfFileDragGuard;
    bool audioFileChooserActive = false;
    bool dragActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabAudioProcessorEditor)
};
