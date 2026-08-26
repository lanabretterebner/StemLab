#include "PluginEditor.h"
#include "StemLabPaths.h"
#include "StemLabTheme.h"
#include "BinaryData.h"

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace theme = stemlab::theme;
namespace widgets = stemlab::widgets;

namespace
{
juce::File stemLabSettingsDirectory()
{
    return stemlab::paths::configDirectory();
}

juce::File firstRunMarkerFile()
{
    return stemLabSettingsDirectory().getChildFile("portable-first-run-0.9.9.txt");
}

juce::File portableRootDirectory()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
}

juce::File abletonSetupScript()
{
    auto root = portableRootDirectory();

    for (int depth = 0; depth < 6 && root.exists(); ++depth)
    {
        const auto candidate =
            root.getChildFile("scripts").getChildFile("install_ableton.ps1");

        if (candidate.existsAsFile())
            return candidate;

        const auto parent = root.getParentDirectory();
        if (parent == root)
            break;

        root = parent;
    }

    return {};
}

juce::String formatSeconds(double seconds)
{
    if (seconds < 0.0)
        return "--:--";

    const int total = juce::jmax(0, static_cast<int>(seconds + 0.5));

    const int minutes = total / 60;
    const int secs = total % 60;

    return juce::String::formatted("%02d:%02d", minutes, secs);
}

// Settings-menu ids: 1..5 are the fixed entries.
constexpr int gridModeMenuBase = 400;
constexpr int analysisModeMenuBase = 410;
constexpr int tempoMenuBase = 420;
constexpr int analysisEnableId = 430;
constexpr int analysisForgetId = 431;
constexpr int analysisClearCacheId = 432;

// Lane-menu ids for MIDI, above the per-menu action ids.
constexpr int midiConvertId = 500;
constexpr int midiAuditionId = 501;
constexpr int midiSaveId = 502;
constexpr int midiSendId = 503;

/*
    Waveform zoom detents.

    A continuous zoom reads back as "3.7x", which tells nobody anything and
    never lands on a round number twice. These are the stops the slider
    snaps to, so the readout beside it is always a clean multiplier and
    stepping is repeatable.
*/
constexpr double waveformZoomSteps[] = {1.0,  1.5,  2.0,  3.0,  4.0,  6.0, 8.0,
                                        12.0, 16.0, 24.0, 32.0, 48.0, 64.0};

constexpr int waveformZoomStepCount =
    static_cast<int>(sizeof(waveformZoomSteps) / sizeof(waveformZoomSteps[0]));

/** The detent nearest a stored zoom, which need not be one of them. */
int waveformZoomIndexFor(double zoom)
{
    int best = 0;
    double bestDistance = std::abs(zoom - waveformZoomSteps[0]);

    for (int i = 1; i < waveformZoomStepCount; ++i)
    {
        const auto distance = std::abs(zoom - waveformZoomSteps[i]);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

juce::String waveformZoomText(double zoom)
{
    // The half-steps at the low end are the only ones with a fraction.
    const auto rounded = juce::roundToInt(zoom);

    const auto number = std::abs(zoom - static_cast<double>(rounded)) < 0.01
                            ? juce::String(rounded)
                            : juce::String(zoom, 1);

    return number + juce::String::fromUTF8("\xc3\x97");
}

juce::String stemDisplayName(int index)
{
    const auto name = StemLabAudioProcessor::getStemName(index);
    return name.substring(0, 1).toUpperCase() + name.substring(1);
}

} // namespace

// ==================================================================== waveform

StemLaneWaveform::StemLaneWaveform(StemLabAudioProcessor& processorIn,
                                   juce::AudioFormatManager& formatManager,
                                   juce::AudioThumbnailCache& thumbnailCache)
    : processor(processorIn),
      thumbnail(theme::metrics::waveform::thumbnailResolution, formatManager, thumbnailCache)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, false);
}

void StemLaneWaveform::setFile(const juce::File& file)
{
    if (file == currentFile)
        return;

    currentFile = file;
    thumbnail.clear();

    if (currentFile.existsAsFile())
        thumbnail.setSource(new juce::FileInputSource(currentFile));

    repaint();
}

void StemLaneWaveform::setMutedAppearance(bool muted)
{
    if (mutedAppearance != muted)
    {
        mutedAppearance = muted;
        repaint();
    }
}

void StemLaneWaveform::setStemIdentity(const juce::String& stemName)
{
    if (stemIdentity != stemName)
    {
        stemIdentity = stemName;
        repaint();
    }
}

void StemLaneWaveform::paint(juce::Graphics& g)
{
    namespace lanes = theme::metrics::lanes;

    const auto full = getLocalBounds().toFloat();

    g.setColour(theme::colours::laneWell());
    g.fillRoundedRectangle(full, lanes::wellRadius);

    const auto length = thumbnail.getTotalLength();

    const auto transportLength = processor.getTransportLengthSeconds();
    const auto transportPosition = processor.getTransportPositionSeconds();

    const double playNormalised =
        transportLength > 0.0 ? juce::jlimit(0.0, 1.0, transportPosition / transportLength)
                              : -1.0;

    const auto inner = full.reduced(6.0f, 5.0f);

    const int palette = processor.getWaveformColourIndex();

    /*
     * At zoom 1 this is the whole file, as it always was. Above that it is a
     * window of it, centred on the playhead - and every lane asks the
     * processor for it, so they all scroll together.
     */
    const auto view = processor.getWaveformViewRange(length);
    const auto viewLength = juce::jmax(1.0e-6, view.getLength());

    const auto secondsToX = [&inner, &view, viewLength](double seconds)
    {
        return inner.getX() +
               static_cast<float>((seconds - view.getStart()) / viewLength) * inner.getWidth();
    };

    // Beat grid behind the waveform: bars read stronger than beats, and the
    // whole thing stays subordinate to the audio it sits behind.
    if (length > 0.0 && !inner.isEmpty())
    {
        const auto grid = processor.getWaveformGridInfo();

        if (grid.bpm > 0.0)
        {
            const auto secondsPerBeat = 60.0 / grid.bpm;
            const auto beatsPerBar = juce::jmax(1, grid.numerator);

            if (secondsPerBeat > 0.0 && secondsPerBeat * beatsPerBar * 3.0 < length)
            {
                const auto pixelsPerSecond =
                    static_cast<double>(inner.getWidth()) / viewLength;

                // Thin out beat lines that would be closer than a few pixels.
                const bool drawBeats = secondsPerBeat * pixelsPerSecond >= 7.0;

                /*
                 * Start at the first beat in view rather than at bar one: at
                 * 64x on a long track that is thousands of iterations that
                 * would each be computed only to be discarded.
                 */
                int beatIndex = static_cast<int>(
                    std::floor((view.getStart() - grid.barOne) / secondsPerBeat));

                for (double t = grid.barOne + beatIndex * secondsPerBeat;
                     t <= view.getEnd() && t < length;
                     t += secondsPerBeat, ++beatIndex)
                {
                    if (t < 0.0)
                        continue;

                    // beatIndex is negative before bar one, and % keeps that
                    // sign in C++; fold it back before testing for a bar.
                    const bool bar = (((beatIndex % beatsPerBar) + beatsPerBar) % beatsPerBar) == 0;

                    if (!bar && !drawBeats)
                        continue;

                    const auto x = secondsToX(t);

                    if (x < inner.getX() || x > inner.getRight())
                        continue;

                    g.setColour(theme::colours::text().withAlpha(bar ? 0.22f : 0.10f));
                    g.fillRect(x, inner.getY(), bar ? 1.4f : 1.0f, inner.getHeight());
                }
            }
        }
    }

    if (length > 0.0 && thumbnail.getNumChannels() > 0 && !inner.isEmpty())
    {
        const int channels = juce::jmin(2, thumbnail.getNumChannels());

        const float playheadX =
            secondsToX(juce::jmax(0.0, playNormalised) * length);

        const auto centreY = inner.getCentreY();

        for (float x = inner.getX(); x < inner.getRight(); x += lanes::barPitch)
        {
            const auto start =
                view.getStart() + static_cast<double>(x - inner.getX()) /
                                      static_cast<double>(inner.getWidth()) * viewLength;

            const auto end = juce::jmax(
                start + 0.000001,
                view.getStart() + static_cast<double>(x + lanes::barPitch - inner.getX()) /
                                      static_cast<double>(inner.getWidth()) * viewLength);

            float peak = 0.0f;

            for (int channel = 0; channel < channels; ++channel)
            {
                float minimum = 0.0f, maximum = 0.0f;
                thumbnail.getApproximateMinMax(start, end, channel, minimum, maximum);
                peak = juce::jmax(peak, std::abs(minimum), std::abs(maximum));
            }

            // Square-root lift keeps quiet material visible without faking
            // loudness; the spec's bars render from the real peaks.
            const auto level = std::sqrt(juce::jlimit(0.0f, 1.0f, peak));

            const auto barHeight =
                juce::jmax(lanes::barMinHeight, level * inner.getHeight() * 0.96f);

            const float across = inner.getWidth() > 0.0f
                                     ? (x - inner.getX()) / inner.getWidth()
                                     : 0.0f;

            juce::Colour barColour;

            if (mutedAppearance)
                barColour = theme::colours::waveMuted();
            else if (playNormalised >= 0.0 && x < playheadX)
                barColour = theme::waveform::playedColour(palette, stemIdentity, across);
            else
                barColour = theme::waveform::unplayedColour(palette, stemIdentity, across);

            g.setColour(barColour);
            g.fillRoundedRectangle(x, centreY - barHeight * 0.5f, lanes::barWidth, barHeight,
                                   lanes::barWidth * 0.5f);
        }

        // Shared playhead: every lane draws it at the same x (one clock).
        // Zoomed in it can sit outside the window at either end of the file,
        // where the view stops following it.
        if (playNormalised >= 0.0 && playheadX >= inner.getX() && playheadX <= inner.getRight())
        {
            g.setColour(theme::colours::playheadGlow());
            g.fillRect(playheadX - lanes::playheadGlowWidth * 0.5f, inner.getY(),
                       lanes::playheadGlowWidth, inner.getHeight());

            g.setColour(theme::colours::playhead());
            g.fillRect(playheadX - lanes::playheadWidth * 0.5f, inner.getY(),
                       lanes::playheadWidth, inner.getHeight());
        }
    }
}

void StemLaneWaveform::mouseDown(const juce::MouseEvent&)
{
    // The gesture is ambiguous at press time: a click seeks, a drag exports.
    // Deciding at release keeps a drag from disturbing the transport.
    externalDragStarted = false;
}

void StemLaneWaveform::mouseUp(const juce::MouseEvent& event)
{
    // Plain Components still receive mouse events while disabled.
    if (!isEnabled() || externalDragStarted || getWidth() <= 0)
        return;

    if (event.getDistanceFromDragStart() >= theme::metrics::waveform::clickVersusDragThreshold)
        return;

    /*
     * Measured against the same inset rectangle paint() draws into, not the
     * whole component: at zoom 1 the difference is a pixel, but zoomed in
     * the well's 6px margin is a meaningful slice of the window.
     */
    const auto inner = getLocalBounds().toFloat().reduced(6.0f, 5.0f);

    if (inner.getWidth() <= 0.0f)
        return;

    const auto across = juce::jlimit(
        0.0, 1.0,
        static_cast<double>(event.mouseDownPosition.x - inner.getX()) /
            static_cast<double>(inner.getWidth()));

    const auto length = thumbnail.getTotalLength();

    // Zoomed in, the click lands somewhere in the visible window rather than
    // somewhere in the file.
    const auto view = processor.getWaveformViewRange(length);

    const auto normalised =
        length > 0.0
            ? juce::jlimit(0.0, 1.0, (view.getStart() + across * view.getLength()) / length)
            : across;

    processor.transportSeekNormalised(normalised);
    repaint();
}

void StemLaneWaveform::mouseDrag(const juce::MouseEvent& event)
{
    /*
     * Dragging a lane's waveform exports its file to whatever the pointer
     * lands on - a DAW arrangement, a file manager, an editor. This is what
     * makes StemLab useful in hosts with no integration path at all: every
     * host accepts an audio-file drop.
     */
    if (externalDragStarted || !currentFile.existsAsFile())
        return;

    if (event.getDistanceFromDragStart() < theme::metrics::waveform::clickVersusDragThreshold)
        return;

    externalDragStarted = juce::DragAndDropContainer::performExternalDragDropOfFiles(
        juce::StringArray{currentFile.getFullPathName()}, false, this);
}

// ======================================================================== lane

StemLaneComponent::StemLaneComponent(StemLabAudioProcessor& processorIn, int stemIndexIn,
                                     juce::String childIdIn,
                                     juce::AudioFormatManager& formatManager,
                                     juce::AudioThumbnailCache& thumbnailCache,
                                     std::function<void()> refreshEditorIn,
                                     std::function<void(int)> showRootMenuIn,
                                     std::function<void(const juce::String&)> showChildMenuIn,
                                     std::function<void(int, juce::String)> toggleExpandedIn)
    : processor(processorIn), stemIndex(stemIndexIn), childId(std::move(childIdIn)),
      refreshEditor(std::move(refreshEditorIn)), showRootMenu(std::move(showRootMenuIn)),
      showChildMenu(std::move(showChildMenuIn)), toggleExpanded(std::move(toggleExpandedIn))
{
    setRepaintsOnMouseActivity(true);

    twisty.onClick = [this]
    {
        if (toggleExpanded)
            toggleExpanded(stemIndex, childId);
    };

    addChildComponent(twisty);

    include.onClick = [this]
    {
        if (isChildLane())
            processor.setRecursiveStemEnabled(childId, include.getToggleState());
        else
            processor.setStemEnabled(stemIndex, include.getToggleState());

        if (refreshEditor)
            refreshEditor();
    };

    addAndMakeVisible(include);

    nameLabel.setFont(theme::fonts::laneName());
    nameLabel.setColour(juce::Label::textColourId, theme::colours::text());
    nameLabel.setInterceptsMouseClicks(false, false);

    if (!isChildLane())
        nameLabel.setText(stemDisplayName(stemIndex), juce::dontSendNotification);

    addAndMakeVisible(nameLabel);

    waveform = std::make_unique<StemLaneWaveform>(processor, formatManager, thumbnailCache);

    if (!isChildLane())
        waveform->setStemIdentity(StemLabAudioProcessor::getStemName(stemIndex));

    addAndMakeVisible(*waveform);

    soloButton.setComponentID("solo");
    soloButton.setTooltip("Solo");

    soloButton.onClick = [this]
    {
        if (isChildLane())
            processor.setRecursiveStemSolo(childId, !processor.isRecursiveStemSoloed(childId));
        else
            processor.setStemSolo(stemIndex, !processor.isStemSoloed(stemIndex));

        if (refreshEditor)
            refreshEditor();
    };

    addAndMakeVisible(soloButton);

    muteButton.setComponentID("mute");
    muteButton.setTooltip("Mute");

    muteButton.onClick = [this]
    {
        if (isChildLane())
            processor.setRecursiveStemMute(childId, !processor.isRecursiveStemMuted(childId));
        else
            processor.setStemMute(stemIndex, !processor.isStemMuted(stemIndex));

        if (refreshEditor)
            refreshEditor();
    };

    addAndMakeVisible(muteButton);

    layersButton = std::make_unique<widgets::IconButton>(
        "layers", [](juce::Rectangle<float> b) { return stemlab::icons::layers(b); },
        static_cast<float>(theme::metrics::lanes::layersIcon), true,
        theme::metrics::lanes::smRadius, false);

    layersButton->setTooltip("Split this stem further");

    layersButton->onClick = [this]
    {
        if (isChildLane())
        {
            if (showChildMenu)
                showChildMenu(childId);
        }
        else if (showRootMenu)
        {
            showRootMenu(stemIndex);
        }
    };

    addAndMakeVisible(*layersButton);
}

void StemLaneComponent::setLayersAvailable(bool available)
{
    if (layersAvailable != available)
    {
        layersAvailable = available;
        refresh();
    }
}

void StemLaneComponent::setChildState(bool laneHasChildren, bool expanded)
{
    hasChildren = laneHasChildren;

    twisty.setExpanded(expanded);
    twisty.setVisible(laneHasChildren);
}

void StemLaneComponent::setChildInfo(const StemLabRecursiveStemInfo& info)
{
    childInfo = info;
    laneFile = info.file;

    auto label = info.label;

    if (info.estimatedSourceCount > 1 && (info.actions.contains("split") || info.hasChildren))
        label += " (est. " + juce::String(info.estimatedSourceCount) + ")";

    nameLabel.setText(label, juce::dontSendNotification);

    nameLabel.setTooltip("Category: " + info.category + " | confidence " +
                         juce::String(juce::roundToInt(info.confidence * 100.0)) + "%");

    if (waveform != nullptr)
    {
        waveform->setFile(laneFile);

        // A child lane carries its root's identity colour, so a split stem
        // still reads as one family down the tree.
        waveform->setStemIdentity(info.rootStem);
    }

    refresh();
}

void StemLaneComponent::refresh()
{
    const bool capturing = processor.isCapturing();
    const bool engineRunning = processor.isEngineRunning();
    const bool jobDone = processor.hasSuccessfulJob();

    if (!isChildLane())
        laneFile = jobDone ? processor.getCompletedStemFile(stemIndex) : juce::File{};

    const bool ready = jobDone && !engineRunning && !capturing && laneFile.existsAsFile();
    const bool laneLive = jobDone && !engineRunning && !capturing;

    if (waveform != nullptr)
    {
        waveform->setFile(laneFile);
        waveform->setEnabled(ready);
    }

    twisty.setEnabled(laneLive);

    // Root and child lanes read the same three states from different sides
    // of the processor; everything below them is identical.
    const bool included = isChildLane() ? processor.isRecursiveStemEnabled(childId)
                                        : processor.isStemEnabled(stemIndex);

    const bool soloed = isChildLane() ? processor.isRecursiveStemSoloed(childId)
                                      : processor.isStemSoloed(stemIndex);

    const bool muted = isChildLane() ? processor.isRecursiveStemMuted(childId)
                                     : processor.isStemMuted(stemIndex);

    include.setToggleState(included, juce::dontSendNotification);
    include.setEnabled(laneLive);

    soloButton.setEnabled(ready);
    soloButton.setToggleState(soloed, juce::dontSendNotification);

    muteButton.setEnabled(ready);
    muteButton.setToggleState(muted, juce::dontSendNotification);

    if (isChildLane())
    {
        layersButton->setEnabled(ready && !childInfo.actions.isEmpty());
        layersButton->setVisible(!childInfo.actions.isEmpty() || childInfo.hasChildren);
    }
    else
    {
        layersButton->setEnabled(ready && layersAvailable);
        layersButton->setVisible(layersAvailable);
    }

    if (waveform != nullptr)
        waveform->setMutedAppearance(muted && !soloed);

    // An excluded lane drops to 45% opacity per the spec; only meaningful
    // once stems exist.
    setAlpha(jobDone && !included ? theme::metrics::lanes::excludedOpacity : 1.0f);
}

void StemLaneComponent::resized()
{
    namespace lanes = theme::metrics::lanes;

    auto row = getLocalBounds().reduced(0, lanes::rowPadY);

    if (isChildLane())
        row.removeFromLeft(lanes::childIndent);

    // Every lane reserves the twisty column, whether or not it has children,
    // so checkboxes and names stay on one grid down the whole list.
    twisty.setBounds(row.removeFromLeft(lanes::twistyColumn));

    auto includeArea = row.removeFromLeft(lanes::includeColumn);
    include.setBounds(includeArea);

    row.removeFromLeft(lanes::columnGap);

    nameLabel.setBounds(row.removeFromLeft(
        lanes::nameColumn - (isChildLane() ? lanes::childIndent : 0)));

    row.removeFromLeft(lanes::columnGap);

    auto controls = row.removeFromRight(lanes::controlsColumn);

    // Controls sit vertically centred: S, M, layers.
    auto centred = controls.withSizeKeepingCentre(controls.getWidth(), lanes::smButton);

    soloButton.setBounds(centred.removeFromLeft(lanes::smButton));
    centred.removeFromLeft(lanes::smGap);

    muteButton.setBounds(centred.removeFromLeft(lanes::smButton));
    centred.removeFromLeft(lanes::smGap);

    layersButton->setBounds(centred.removeFromLeft(lanes::smButton));

    row.removeFromRight(lanes::columnGap);

    if (waveform != nullptr)
        waveform->setBounds(row.withSizeKeepingCentre(row.getWidth(), lanes::wellHeight));
}

void StemLaneComponent::paint(juce::Graphics& g)
{
    if (isMouseOver(true) && isEnabled())
    {
        g.setColour(theme::colours::rowHoverFill());
        g.fillRoundedRectangle(getLocalBounds().toFloat(),
                               theme::metrics::lanes::rowRadius);
    }
}

// ====================================================================== editor

StemLabAudioProcessorEditor::StemLabAudioProcessorEditor(StemLabAudioProcessor& processorIn)
    : AudioProcessorEditor(&processorIn), processor(processorIn)
{
    namespace window = theme::metrics::window;

    setLookAndFeel(&lookAndFeel);

    /*
     * The interface scales instead of reflowing: one design-size layout in
     * panelContent, warped by a single transform. The constrainer pins the
     * aspect ratio so the two never disagree, and the limits keep the panel
     * between legible and absurd.
     */
    panelContent.onPaint = [this](juce::Graphics& g) { paintPanel(g); };
    addAndMakeVisible(panelContent);

    setResizable(true, true);

    setResizeLimits(juce::roundToInt(window::width * window::minScale),
                    juce::roundToInt(window::height * window::minScale),
                    juce::roundToInt(window::width * window::maxScale),
                    juce::roundToInt(window::height * window::maxScale));

    if (auto* boundsConstrainer = getConstrainer())
    {
        boundsConstrainer->setFixedAspectRatio(static_cast<double>(window::width) /
                                               static_cast<double>(window::height));
    }

    if (processor.isStandaloneApp())
    {
        auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

        juce::MessageManager::callAsync(
            [safeThis]
            {
                if (safeThis == nullptr)
                    return;

                if (auto* windowComponent =
                        safeThis->findParentComponentOfClass<juce::DocumentWindow>())
                {
                    windowComponent->setUsingNativeTitleBar(true);
                    windowComponent->setName("StemLab");

                    const auto appIcon = juce::ImageFileFormat::loadFrom(
                        BinaryData::StemLabIcon_png, BinaryData::StemLabIcon_pngSize);

                    if (appIcon.isValid())
                        windowComponent->setIcon(appIcon);
                }
            });
    }

    // ------------------------------------------------------------- header

    titleLabel.setText("StemLab", juce::dontSendNotification);
    titleLabel.setFont(
        juce::Font(theme::fonts::title()).withExtraKerningFactor(theme::fonts::titleKerning));
    titleLabel.setColour(juce::Label::textColourId, theme::colours::text());
    panelContent.addAndMakeVisible(titleLabel);

    // The separation model and the waveform palette live here rather than
    // inside the settings menu: both are choices made while working, and the
    // model belongs beside the Separate button that runs it.
    enginePrevButton = std::make_unique<widgets::IconButton>(
        "engine-prev",
        [](juce::Rectangle<float> b)
        { return stemlab::icons::chevron(b, stemlab::icons::ChevronDirection::left); },
        static_cast<float>(theme::metrics::header::stepIcon), true,
        theme::metrics::lanes::smRadius, false);

    enginePrevButton->setTooltip("Previous separation model");
    enginePrevButton->onClick = [this] { stepSeparatorEngine(-1); };
    panelContent.addAndMakeVisible(*enginePrevButton);

    engineSelector = std::make_unique<widgets::SelectorButton>(
        "engine", [](juce::Rectangle<float> b) { return stemlab::icons::sparkle(b); });

    engineSelector->setTooltip("Separation model");
    engineSelector->onClick = [this] { showEngineMenu(); };

    // Measure every name up front and keep the widest: the pill then holds
    // still while the arrows step through the models.
    for (int i = 0; i < StemLabAudioProcessor::separatorEngineCount; ++i)
    {
        engineSelector->setLabel(StemLabAudioProcessor::getSeparatorEngineShortName(i));
        engineSelectorWidth = juce::jmax(engineSelectorWidth,
                                         engineSelector->getPreferredWidth());
    }

    engineSelector->setLabel(processor.getSeparatorEngineDisplayName());
    panelContent.addAndMakeVisible(*engineSelector);

    engineNextButton = std::make_unique<widgets::IconButton>(
        "engine-next",
        [](juce::Rectangle<float> b)
        { return stemlab::icons::chevron(b, stemlab::icons::ChevronDirection::right); },
        static_cast<float>(theme::metrics::header::stepIcon), true,
        theme::metrics::lanes::smRadius, false);

    engineNextButton->setTooltip("Next separation model");
    engineNextButton->onClick = [this] { stepSeparatorEngine(1); };
    panelContent.addAndMakeVisible(*engineNextButton);

    paletteButton = std::make_unique<widgets::IconButton>(
        "waveform-colour", [](juce::Rectangle<float> b) { return stemlab::icons::palette(b); },
        static_cast<float>(theme::metrics::header::paletteIcon), false,
        theme::metrics::header::settingsRadius, true, true);

    paletteButton->setTooltip("Waveform colour");
    paletteButton->onClick = [this] { showWaveformColourMenu(); };
    panelContent.addAndMakeVisible(*paletteButton);

    settingsButton = std::make_unique<widgets::IconButton>(
        "settings", [](juce::Rectangle<float> b) { return stemlab::icons::sliders(b); },
        static_cast<float>(theme::metrics::header::settingsIcon), false,
        theme::metrics::header::settingsRadius, true, true);

    settingsButton->setTooltip("Settings");
    settingsButton->onClick = [this] { showSettingsMenu(); };
    panelContent.addAndMakeVisible(*settingsButton);

    /*
     * Deciding which stems a job carries forward is a per-lane checkbox,
     * which is one click for one change and six for "actually, just the
     * drums". These two act on every lane at once - adaptive children
     * included, since they stand in for their parent in the mix - and the
     * readout beside them says where that left things.
     */
    selectAllButton.setComponentID("neutral");
    selectAllButton.setTooltip("Include every stem");
    selectAllButton.onClick = [this] { setAllLanesIncluded(true); };
    panelContent.addAndMakeVisible(selectAllButton);

    deselectAllButton.setComponentID("neutral");
    deselectAllButton.setTooltip("Exclude every stem");
    deselectAllButton.onClick = [this] { setAllLanesIncluded(false); };
    panelContent.addAndMakeVisible(deselectAllButton);

    selectionCountLabel.setFont(theme::fonts::meta());
    selectionCountLabel.setColour(juce::Label::textColourId, theme::colours::text50());
    selectionCountLabel.setJustificationType(juce::Justification::centredLeft);
    panelContent.addAndMakeVisible(selectionCountLabel);

    {
        // Measure once: the pills and the readout then hold still while the
        // selection changes underneath them.
        const juce::Font pillFont{theme::fonts::smallButton()};
        const juce::Font countFont{theme::fonts::meta()};

        const auto textWidth = [](const juce::Font& font, const juce::String& text)
        { return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)); };

        selectAllWidth =
            textWidth(pillFont, selectAllButton.getButtonText()) +
            2 * theme::metrics::header::selectButtonPadX;

        deselectAllWidth =
            textWidth(pillFont, deselectAllButton.getButtonText()) +
            2 * theme::metrics::header::selectButtonPadX;

        // The widest readout is every lane selected with the tree expanded,
        // which we cannot know here - so measure the widest two-digit form.
        selectionCountWidth = textWidth(countFont, "88 of 88 selected") + 2;
    }

    /*
     * Waveform zoom. A five-minute track across 800 pixels puts a kick and
     * the snare after it in the same column, so the lanes draw a window of
     * the file instead of all of it. The magnifier resets to 1x, and the
     * slider steps through detents so the readout is always a clean
     * multiplier rather than 3.7x.
     */
    zoomResetButton = std::make_unique<widgets::IconButton>(
        "zoom-reset", [](juce::Rectangle<float> b) { return stemlab::icons::magnifier(b); },
        static_cast<float>(theme::metrics::header::zoomIcon), true,
        theme::metrics::lanes::smRadius, false);

    zoomResetButton->setTooltip("Reset waveform zoom");
    zoomResetButton->onClick = [this] { applyWaveformZoomIndex(0); };
    panelContent.addAndMakeVisible(*zoomResetButton);

    zoomSlider.setTooltip("Waveform zoom");
    zoomSlider.onValueChanged = [this](double normalised)
    {
        applyWaveformZoomIndex(
            juce::roundToInt(normalised * static_cast<double>(waveformZoomStepCount - 1)));
    };

    panelContent.addAndMakeVisible(zoomSlider);

    zoomLabel.setFont(theme::fonts::meta());
    zoomLabel.setColour(juce::Label::textColourId, theme::colours::text50());
    zoomLabel.setJustificationType(juce::Justification::centredLeft);
    panelContent.addAndMakeVisible(zoomLabel);

    // -------------------------------------------------------- source strip

    fileNameLabel.setFont(theme::fonts::bodyMedium());
    fileNameLabel.setColour(juce::Label::textColourId, theme::colours::text());
    panelContent.addAndMakeVisible(fileNameLabel);

    fileMetaLabel.setFont(theme::fonts::meta());
    fileMetaLabel.setColour(juce::Label::textColourId, theme::colours::text50());
    panelContent.addAndMakeVisible(fileMetaLabel);

    captureButton.setComponentID("accent-outline");

    if (processor.isStandaloneApp())
    {
        captureButton.setButtonText("Select File");
        captureButton.onClick = [this] { chooseStandaloneAudioFile(); };
    }
    else
    {
        switch (processor.getHostIntegration())
        {
        // Both hosts get the same label: what the button does is pull the
        // source out of the DAW, and the meta line under it already names
        // the clip or item it came from. The tooltip keeps the specifics.
        case StemLabAudioProcessor::hostIntegrationAbletonLive:
            captureButton.setButtonText("Import from DAW");
            captureButton.setTooltip("Use the selected clip in Live");
            captureButton.onClick = [this]
            {
                processor.requestAbletonSourceClip();
                refreshFromProcessor();
            };
            break;

        case StemLabAudioProcessor::hostIntegrationReaper:
            captureButton.setButtonText("Import from DAW");
            captureButton.setTooltip("Use the selected item in REAPER");
            captureButton.onClick = [this]
            {
                processor.requestReaperSourceItem();
                refreshFromProcessor();
            };
            break;

        case StemLabAudioProcessor::hostIntegrationNone:
        default:
            captureButton.setButtonText("Select File");
            captureButton.onClick = [this] { chooseStandaloneAudioFile(); };
            break;
        }
    }

    panelContent.addAndMakeVisible(captureButton);

    recordSystemButton.onClick = [this]
    {
        if (processor.getStandaloneRecordingMode() == StemLabAudioProcessor::recordingSystem)
            processor.stopSystemAudioRecording();
        else
            processor.startSystemAudioRecording();

        refreshFromProcessor();
    };

    panelContent.addAndMakeVisible(recordSystemButton);
    recordSystemButton.setVisible(StemLabAudioProcessor::isSystemAudioCaptureSupported());

    recordInputButton.onClick = [this]
    {
        if (processor.getStandaloneRecordingMode() == StemLabAudioProcessor::recordingInput)
            processor.stopStandaloneRecording();
        else
            processor.startStandaloneRecording();

        refreshFromProcessor();
    };

    panelContent.addAndMakeVisible(recordInputButton);
    recordInputButton.setVisible(processor.isStandaloneApp());

    separateControl.setRefineOn(processor.isRefinementEnabled());

    separateControl.onRefineChanged = [this](bool on) { processor.setRefinementEnabled(on); };

    separateControl.onSeparate = [this]
    {
        // Act on what the button was showing when it was clicked, not on
        // the state a moment later: the engine can finish between the last
        // refresh and the click, and re-reading it there would turn a click
        // on "Cancel" into a fresh multi-minute separation (or a click on
        // "Separate" into cancelling a split someone just started).
        if (separateControlShowsCancel)
            processor.cancelSeparation(); // harmless if the engine just ended
        else if (!processor.isEngineRunning())
            processor.launchSeparationAndExport();

        refreshFromProcessor();
    };

    panelContent.addAndMakeVisible(separateControl);

    // --------------------------------------------------------------- lanes

    waveformFormats.registerBasicFormats();

    rootExpanded.fill(true);

    laneViewport.setViewedComponent(&laneContent, false);
    laneViewport.setScrollBarsShown(true, false);
    laneViewport.setScrollBarThickness(theme::metrics::lanes::scrollbarThickness);
    panelContent.addAndMakeVisible(laneViewport);

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        rootLanes[static_cast<size_t>(i)] = std::make_unique<StemLaneComponent>(
            processor, i, juce::String{}, waveformFormats, waveformCache,
            [this] { refreshFromProcessor(); },
            [this](int stemIndex) { showRootLayersMenu(stemIndex); },
            [this](const juce::String& id) { showChildLayersMenu(id); },
            [this](int stemIndex, juce::String id) { toggleLaneExpanded(stemIndex, id); });

        laneContent.addAndMakeVisible(*rootLanes[static_cast<size_t>(i)]);
    }

    // ----------------------------------------------------------- transport

    playButton.setTooltip("Play / pause");
    playButton.onClick = [this]
    {
        processor.transportTogglePlay();
        refreshFromProcessor();
    };
    panelContent.addAndMakeVisible(playButton);

    timeLabel.setFont(theme::fonts::time());
    timeLabel.setColour(juce::Label::textColourId, theme::colours::text75());
    panelContent.addAndMakeVisible(timeLabel);

    scrubber.onSeek = [this](double normalised)
    { processor.transportSeekNormalised(normalised); };
    panelContent.addAndMakeVisible(scrubber);

    abControl.setSelectedIndex(0);
    abControl.onSelected = [this](int index)
    {
        processor.setMonitorMode(index == 1 ? StemLabAudioProcessor::monitorStems
                                            : StemLabAudioProcessor::monitorOriginal);
        refreshFromProcessor();
    };
    panelContent.addAndMakeVisible(abControl);

    // -------------------------------------------------------------- footer

    panelContent.addAndMakeVisible(footerDivider);

    panelContent.addAndMakeVisible(statusIndicator);

    statusLabel.setFont(theme::fonts::status());
    statusLabel.setColour(juce::Label::textColourId, theme::colours::text50());
    panelContent.addAndMakeVisible(statusLabel);

    panelContent.addAndMakeVisible(progressBar);

    progressLabel.setFont(theme::fonts::meta());
    progressLabel.setColour(juce::Label::textColourId, theme::colours::text45());
    progressLabel.setJustificationType(juce::Justification::centredRight);
    panelContent.addAndMakeVisible(progressLabel);

    pathLabel.setFont(theme::fonts::footerPath());
    pathLabel.setColour(juce::Label::textColourId, theme::colours::text50());
    pathLabel.setJustificationType(juce::Justification::centredRight);
    panelContent.addAndMakeVisible(pathLabel);

    changeFolderButton.setComponentID("ghost");
    changeFolderButton.onClick = [this] { chooseJobRootFolder(); };
    panelContent.addAndMakeVisible(changeFolderButton);

    // Footer actions vary by host. Standalone / generic host: saving is the
    // primary action. REAPER: Insert Stems is primary, saving secondary.
    // Ableton: sending is primary (send-only workflow, plus Retry for the
    // bridge's asynchronous import).
    const auto host = processor.isStandaloneApp() ? StemLabAudioProcessor::hostIntegrationNone
                                                  : processor.getHostIntegration();

    saveButton.onClick = [this] { chooseSaveFolder(); };
    panelContent.addAndMakeVisible(saveButton);

    retryButton.setComponentID("ghost");
    retryButton.onClick = [this]
    {
        processor.retryAbletonImport();
        refreshFromProcessor();
    };
    panelContent.addAndMakeVisible(retryButton);
    retryButton.setVisible(host == StemLabAudioProcessor::hostIntegrationAbletonLive);

    insertButton.setComponentID("primary");
    panelContent.addAndMakeVisible(insertButton);

    switch (host)
    {
    case StemLabAudioProcessor::hostIntegrationReaper:
        insertButton.setButtonText("Insert Stems");
        insertButton.onClick = [this]
        {
            processor.insertSelectedStemsIntoReaper();
            refreshFromProcessor();
        };
        break;

    case StemLabAudioProcessor::hostIntegrationAbletonLive:
        insertButton.setButtonText("Send Stems");
        insertButton.onClick = [this]
        {
            processor.sendSelectedStemsToAbleton();
            refreshFromProcessor();
        };
        saveButton.setVisible(false);
        break;

    case StemLabAudioProcessor::hostIntegrationNone:
    default:
        insertButton.setVisible(false);
        saveButton.setComponentID("primary");
        break;
    }

    processor.addChangeListener(this);

    // A reopened editor must not re-trigger the switch-to-Stems that runs
    // when a job is first observed finishing: seed from processor state.
    sawSuccessfulJob = processor.hasSuccessfulJob();

    // Sized last: setSize() fires resized(), which needs every child above.
    const auto scale = juce::jlimit(window::minScale, window::maxScale,
                                    processor.getEditorScalePercent() / 100.0);

    setSize(juce::roundToInt(window::width * scale),
            juce::roundToInt(window::height * scale));

    startTimerHz(theme::metrics::uiRefreshHz);
    refreshFromProcessor();

    if (processor.isStandaloneApp())
    {
        auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

        juce::MessageManager::callAsync(
            [safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->showFirstRunWelcome();
            });
    }
}

StemLabAudioProcessorEditor::~StemLabAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
    processor.removeChangeListener(this);
    stopTimer();
}

bool StemLabAudioProcessorEditor::isSupportedAudioFile(const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();

    return ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".aiff" || ext == ".aif" ||
           ext == ".ogg";
}

bool StemLabAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    // Loading a source resets the job state a running engine still writes
    // to, so do not invite a drop that filesDropped would have to refuse.
    if (processor.isCapturing() || processor.isEngineRunning())
        return false;

    for (const auto& path : files)
    {
        if (isSupportedAudioFile(juce::File(path)))
            return true;
    }

    return false;
}

void StemLabAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    dragActive = false;
    panelContent.repaint();

    if (processor.isCapturing())
        return;

    if (processor.isEngineRunning())
    {
        processor.postUiStatus("Cancel the running job before loading another file");
        return;
    }

    for (const auto& path : files)
    {
        const juce::File file(path);

        if (!isSupportedAudioFile(file))
            continue;

        if (processor.isFileFromCurrentJob(file))
        {
            // A stem dragged out and released back over the window would
            // otherwise become the new source and wipe the finished job.
            processor.postUiStatus("That file is one of this job's stems");
            continue;
        }

        if (loadSourceFile(file))
        {
            refreshFromProcessor();
            return;
        }
    }
}

bool StemLabAudioProcessorEditor::loadSourceFile(const juce::File& file)
{
    if (processor.isStandaloneApp())
        return processor.setStandaloneInputFile(file);

    // Inside a host, keep whatever timeline position the transport last
    // reported so exported stems still line up with the arrangement.
    return processor.setInputAudioFile(
        file, juce::jmax(0.0, processor.getCaptureStartPpq()), file.getFileName());
}

void StemLabAudioProcessorEditor::fileDragEnter(const juce::StringArray&, int, int)
{
    dragActive = true;
    panelContent.repaint();
}

void StemLabAudioProcessorEditor::fileDragExit(const juce::StringArray&)
{
    dragActive = false;
    panelContent.repaint();
}

void StemLabAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Everything else is drawn by panelContent, which is scaled as a whole.
    // This covers only the sub-pixel margin aspect-ratio rounding leaves.
    g.fillAll(theme::colours::ground());
}

void StemLabAudioProcessorEditor::paintPanel(juce::Graphics& g)
{
    g.fillAll(theme::colours::ground());

    // The surface is the window: no inset, no corners, nothing behind it to
    // cast a shadow onto. Only the drag signal draws an edge.
    g.setColour(theme::colours::surface());
    g.fillRect(panelBounds);

    if (dragActive)
    {
        g.setColour(theme::colours::accent());
        g.drawRect(panelBounds.toFloat().reduced(1.0f), 2.0f);
    }

    // Brand glyph.
    g.setColour(theme::colours::accent());
    g.fillPath(stemlab::icons::waveformBars(brandGlyphBounds.toFloat()));

    // Recessed source strip.
    g.setColour(theme::colours::ground());
    g.fillRoundedRectangle(sourceStripBounds.toFloat(), theme::metrics::source::radius);

    if (!sourceDividerBounds.isEmpty())
    {
        g.setColour(theme::colours::divider());
        g.fillRect(sourceDividerBounds);
    }

    // Accent glows behind the enabled primary actions. Drawn here, in the
    // parent, because a shadow painted inside a component is clipped to its
    // own bounds.
    if (separateControl.isVisible() && separateControl.isSeparateActionEnabled())
        juce::DropShadow(theme::colours::accentGlow(), 11, {})
            .drawForRectangle(g, separateControl.getBounds());

    for (auto* primary : {&insertButton, &saveButton})
    {
        if (primary->isVisible() && primary->isEnabled() &&
            primary->getComponentID() == "primary")
        {
            juce::DropShadow(theme::colours::accentGlow(), 11, {})
                .drawForRectangle(g, primary->getBounds());
        }
    }

    // Folder icon beside the output path.
    if (!folderIconBounds.isEmpty())
    {
        g.setColour(theme::colours::text50());
        g.strokePath(stemlab::icons::folder(folderIconBounds.toFloat()),
                     juce::PathStrokeType(1.2f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
    }

    if (dragActive)
    {
        g.setColour(theme::colours::accentTint10());
        g.fillRect(panelBounds);

        g.setColour(theme::colours::text());
        g.setFont(theme::fonts::title());
        g.drawFittedText("Drop audio to load", panelBounds.reduced(60),
                         juce::Justification::centred, 1);
    }
}

void StemLabAudioProcessorEditor::resized()
{
    // The host can resize before the constructor finishes building children.
    // Both of these are checked because they bracket the header's owned
    // controls: laying out with either still null would dereference it.
    if (settingsButton == nullptr || zoomResetButton == nullptr)
        return;

    namespace window = theme::metrics::window;

    /*
     * The panel is laid out once at its design size and then scaled as a
     * whole, so every metric in StemLabTheme stays a real pixel value and
     * nothing has to be re-derived per size. The constrainer holds the
     * aspect ratio, so both ratios below agree to within a pixel.
     */
    const auto scale = juce::jmax(0.05, juce::jmin(static_cast<double>(getWidth()) / window::width,
                                                   static_cast<double>(getHeight()) / window::height));

    panelContent.setTransform(juce::AffineTransform::scale(static_cast<float>(scale)));
    panelContent.setBounds(0, 0, window::width, window::height);

    // Reopening the editor comes back at the size the user left it.
    processor.setEditorScalePercent(juce::roundToInt(scale * 100.0));

    layoutPanel();
}

void StemLabAudioProcessorEditor::layoutPanel()
{
    namespace window = theme::metrics::window;
    namespace panel = theme::metrics::panel;
    namespace header = theme::metrics::header;
    namespace source = theme::metrics::source;
    namespace transport = theme::metrics::transport;
    namespace footer = theme::metrics::footer;

    panelBounds = panelContent.getLocalBounds().reduced(window::groundMargin);

    auto inner = panelBounds.reduced(panel::padX, panel::padY);

    // Header, right to left: the settings icon, the waveform palette, the
    // model selector, the zoom group, and the lane selection group. The
    // brand glyph and title take the left, and the title absorbs whatever
    // is left over between them.
    auto headerRow = inner.removeFromTop(header::settingsButton);

    settingsButton->setBounds(headerRow.removeFromRight(header::settingsButton));

    headerRow.removeFromRight(header::groupGap);

    paletteButton->setBounds(
        headerRow.removeFromRight(header::paletteButton)
            .withSizeKeepingCentre(header::paletteButton, header::paletteButton));

    headerRow.removeFromRight(header::groupGap);

    engineNextButton->setBounds(
        headerRow.removeFromRight(header::stepButton)
            .withSizeKeepingCentre(header::stepButton, header::stepButton));

    engineSelector->setBounds(
        headerRow.removeFromRight(engineSelectorWidth)
            .withSizeKeepingCentre(engineSelectorWidth, header::selectorHeight));

    enginePrevButton->setBounds(
        headerRow.removeFromRight(header::stepButton)
            .withSizeKeepingCentre(header::stepButton, header::stepButton));

    headerRow.removeFromRight(header::groupGap);

    // Zoom, laid out right to left so it stays attached to the model group:
    // readout, track, magnifier.
    zoomLabel.setBounds(headerRow.removeFromRight(header::zoomLabelWidth));

    headerRow.removeFromRight(header::zoomLabelGap);

    zoomSlider.setBounds(headerRow.removeFromRight(header::zoomTrackWidth)
                             .withSizeKeepingCentre(header::zoomTrackWidth, header::stepButton));

    headerRow.removeFromRight(header::zoomIconGap);

    zoomResetButton->setBounds(
        headerRow.removeFromRight(header::stepButton)
            .withSizeKeepingCentre(header::stepButton, header::stepButton));

    headerRow.removeFromRight(header::groupGap);

    selectionCountLabel.setBounds(headerRow.removeFromRight(selectionCountWidth));

    headerRow.removeFromRight(header::selectCountGap);

    deselectAllButton.setBounds(
        headerRow.removeFromRight(deselectAllWidth)
            .withSizeKeepingCentre(deselectAllWidth, header::selectButtonHeight));

    headerRow.removeFromRight(header::selectButtonGap);

    selectAllButton.setBounds(
        headerRow.removeFromRight(selectAllWidth)
            .withSizeKeepingCentre(selectAllWidth, header::selectButtonHeight));

    headerRow.removeFromRight(header::groupGap);

    brandGlyphBounds = headerRow.removeFromLeft(header::glyphSize)
                           .withSizeKeepingCentre(header::glyphSize, header::glyphSize);

    headerRow.removeFromLeft(header::glyphGap);
    titleLabel.setBounds(headerRow);

    inner.removeFromTop(panel::stackGap);

    // Source strip.
    sourceStripBounds = inner.removeFromTop(source::height);

    {
        auto strip = sourceStripBounds.reduced(source::padX, source::padY);

        auto separateArea = strip.removeFromRight(
            juce::jmax(source::separateMinWidth, strip.getWidth() / 3));

        separateControl.setBounds(
            separateArea.withSizeKeepingCentre(separateArea.getWidth(),
                                               source::separateHeight));

        // The hairline lives in the gap that already separated the sources
        // from the action, so nothing either side of it moves.
        sourceDividerBounds = strip.removeFromRight(source::gap + source::separateExtraLeftGap)
                                  .withSizeKeepingCentre(source::dividerWidth,
                                                         source::dividerHeight);

        if (processor.isStandaloneApp())
        {
            recordInputButton.setBounds(
                strip.removeFromRight(source::recordButtonWidth)
                    .withSizeKeepingCentre(source::recordButtonWidth,
                                           theme::metrics::buttons::height));

            strip.removeFromRight(source::gap);
        }

        if (recordSystemButton.isVisible())
        {
            recordSystemButton.setBounds(
                strip.removeFromRight(source::recordButtonWidth)
                    .withSizeKeepingCentre(source::recordButtonWidth,
                                           theme::metrics::buttons::height));

            strip.removeFromRight(source::gap);
        }

        captureButton.setBounds(
            strip.removeFromRight(source::captureButtonWidth)
                .withSizeKeepingCentre(source::captureButtonWidth,
                                       theme::metrics::buttons::height));

        strip.removeFromRight(source::gap);

        fileNameLabel.setBounds(strip.removeFromTop(strip.getHeight() / 2));
        fileMetaLabel.setBounds(strip);
    }

    inner.removeFromTop(panel::stackGap);

    // Footer (from the bottom up).
    auto footerRow = inner.removeFromBottom(footer::height);
    inner.removeFromBottom(footer::dividerGap);
    footerDivider.setBounds(inner.removeFromBottom(2));
    inner.removeFromBottom(footer::dividerGap);

    {
        auto row = footerRow;

        if (insertButton.isVisible())
        {
            insertButton.setBounds(row.removeFromRight(footer::insertWidth)
                                       .withSizeKeepingCentre(footer::insertWidth,
                                                              footer::buttonHeight));
            row.removeFromRight(footer::gap);
        }

        if (retryButton.isVisible())
        {
            retryButton.setBounds(
                row.removeFromRight(footer::retryWidth)
                    .withSizeKeepingCentre(footer::retryWidth, footer::buttonHeight));
            row.removeFromRight(footer::gap);
        }

        if (saveButton.isVisible())
        {
            saveButton.setBounds(row.removeFromRight(footer::saveWidth)
                                     .withSizeKeepingCentre(footer::saveWidth,
                                                            footer::buttonHeight));
            row.removeFromRight(footer::gap);
        }

        changeFolderButton.setBounds(row.removeFromRight(footer::changeWidth)
                                         .withSizeKeepingCentre(footer::changeWidth,
                                                                footer::buttonHeight));

        pathLabel.setBounds(row.removeFromRight(footer::pathWidth));
        row.removeFromRight(footer::folderIconGap);

        folderIconBounds = row.removeFromRight(footer::folderIcon)
                               .withSizeKeepingCentre(footer::folderIcon, footer::folderIcon);

        row.removeFromRight(footer::statusRightMargin);

        // Left block: status line above, progress row below.
        auto statusArea = row;

        auto statusLine = statusArea.removeFromTop(footer::statusLineHeight);
        statusIndicator.setBounds(statusLine.removeFromLeft(footer::statusLineHeight));
        statusLine.removeFromLeft(4);
        statusLabel.setBounds(statusLine);

        statusArea.removeFromTop(footer::statusLineGap);

        auto progressRow = statusArea.removeFromTop(footer::progressRowHeight);
        progressLabel.setBounds(progressRow.removeFromRight(footer::progressLabelWidth));
        progressRow.removeFromRight(footer::progressLabelGap);
        progressBar.setBounds(progressRow);
    }

    // Transport.
    auto transportRow = inner.removeFromBottom(transport::height);
    inner.removeFromBottom(panel::stackGap);

    {
        playButton.setBounds(transportRow.removeFromLeft(transport::playButton));
        transportRow.removeFromLeft(transport::gap);

        timeLabel.setBounds(transportRow.removeFromLeft(transport::timeWidth));
        transportRow.removeFromLeft(transport::gap);

        abControl.setBounds(transportRow.removeFromRight(transport::abWidth)
                                .withSizeKeepingCentre(transport::abWidth,
                                                       transport::abHeight));
        transportRow.removeFromRight(transport::gap);

        scrubber.setBounds(transportRow);
    }

    // Lanes take everything that remains.
    laneViewport.setBounds(inner);
    layoutLanes();
}

void StemLabAudioProcessorEditor::layoutLanes()
{
    namespace lanes = theme::metrics::lanes;

    const int laneHeight = lanes::wellHeight + 2 * lanes::rowPadY;

    // Setting the content size can flip the vertical scrollbar on or off,
    // which changes the visible width; run again once so lanes always fit
    // the final width.
    for (int pass = 0; pass < 2; ++pass)
    {
        const int contentWidth = juce::jmax(320, laneViewport.getMaximumVisibleWidth());

        int y = 0;

        for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        {
            auto* root = rootLanes[static_cast<size_t>(i)].get();

            if (root == nullptr)
                continue;

            root->setBounds(0, y, contentWidth, laneHeight);
            y += laneHeight;

            const auto rootName = StemLabAudioProcessor::getStemName(i);

            for (auto& child : childLanes)
            {
                if (child != nullptr && child->getRootStem().equalsIgnoreCase(rootName))
                {
                    child->setBounds(0, y, contentWidth, laneHeight);
                    y += laneHeight;
                }
            }
        }

        laneContent.setSize(contentWidth, juce::jmax(y, laneViewport.getHeight()));

        if (laneViewport.getMaximumVisibleWidth() == contentWidth)
            break;
    }
}

std::vector<StemLabRecursiveStemInfo> StemLabAudioProcessorEditor::getVisibleRecursiveItems() const
{
    const auto all = processor.getRecursiveStemItems();
    std::vector<StemLabRecursiveStemInfo> visible;
    visible.reserve(all.size());

    for (const auto& item : all)
    {
        int rootIndex = -1;
        for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
            if (StemLabAudioProcessor::getStemName(i).equalsIgnoreCase(item.rootStem))
                rootIndex = i;

        if (rootIndex >= 0 && !rootExpanded[static_cast<size_t>(rootIndex)])
            continue;

        bool hiddenByAncestor = false;
        for (const auto& collapsed : collapsedRecursiveIds)
        {
            if (item.id.startsWith(collapsed + "/"))
            {
                hiddenByAncestor = true;
                break;
            }
        }
        if (!hiddenByAncestor)
            visible.push_back(item);
    }
    return visible;
}

void StemLabAudioProcessorEditor::syncLanes()
{
    const auto items = getVisibleRecursiveItems();

    bool rebuild = items.size() != childLanes.size();

    if (!rebuild)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (childLanes[i] == nullptr || childLanes[i]->getChildId() != items[i].id)
            {
                rebuild = true;
                break;
            }
        }
    }

    if (rebuild)
    {
        childLanes.clear();
        childLanes.reserve(items.size());

        for (const auto& item : items)
        {
            auto lane = std::make_unique<StemLaneComponent>(
                processor, -1, item.id, waveformFormats, waveformCache,
                [this] { refreshFromProcessor(); },
                [this](int stemIndex) { showRootLayersMenu(stemIndex); },
                [this](const juce::String& id) { showChildLayersMenu(id); },
                [this](int stemIndex, juce::String id) { toggleLaneExpanded(stemIndex, id); });

            lane->setChildInfo(item);
            lane->setChildState(item.hasChildren, isLaneExpanded(-1, item.id));
            laneContent.addAndMakeVisible(*lane);
            childLanes.push_back(std::move(lane));
        }

        layoutLanes();
    }
    else
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            childLanes[i]->setChildInfo(items[i]);
            childLanes[i]->setChildState(items[i].hasChildren,
                                         isLaneExpanded(-1, items[i].id));
        }
    }
}

void StemLabAudioProcessorEditor::showRootLayersMenu(int stemIndex)
{
    if (!juce::isPositiveAndBelow(stemIndex, StemLabAudioProcessor::stemCount))
        return;

    const auto stemName = StemLabAudioProcessor::getStemName(stemIndex);
    const bool supportsSplit = rootSupportsAdaptiveSplit(stemIndex);
    const bool hasChildren = rootHasChildren(stemIndex);

    const bool jobDone = processor.hasSuccessfulJob();
    const bool laneReady = jobDone && processor.getCompletedStemFile(stemIndex).existsAsFile();

    if (!supportsSplit && !hasChildren && !laneReady)
        return;

    auto menu = makeMenu();
    if (supportsSplit)
    {
        juce::String label = "Adaptive Split";
        if (stemName.equalsIgnoreCase("vocals"))
            label = "Split Lead / Backing Vocals";
        else if (stemName.equalsIgnoreCase("drums"))
            label = "Split Drum Components";
        else
            label = "Lead / Foreground Split (Experimental)";
        menu.addItem(1, label);
    }

    if (hasChildren)
    {
        if (supportsSplit)
            menu.addSeparator();
        menu.addItem(2, rootExpanded[static_cast<size_t>(stemIndex)] ? "Collapse Children"
                                                                     : "Expand Children");
    }

    if (laneReady)
    {
        menu.addSeparator();
        addMidiMenuItems(menu, stemName);
    }

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    auto* target = rootLanes[static_cast<size_t>(stemIndex)].get();

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),
                       [safeThis, stemIndex, stemName](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;
                           if (result == 1)
                               safeThis->processor.launchRecursiveStemSplit(stemIndex);
                           else if (result == 2)
                               safeThis->toggleRootExpanded(stemIndex);
                           else
                               safeThis->handleMidiMenuResult(result, stemName, stemIndex, {});
                           safeThis->refreshFromProcessor();
                       });
}

void StemLabAudioProcessorEditor::showChildLayersMenu(const juce::String& itemId)
{
    StemLabRecursiveStemInfo info;
    bool found = false;

    for (const auto& item : processor.getRecursiveStemItems())
    {
        if (item.id == itemId)
        {
            info = item;
            found = true;
            break;
        }
    }

    if (!found)
        return;

    auto menu = makeMenu();

    constexpr int deverbId = 1;
    constexpr int splitFurtherId = 2;
    constexpr int expandId = 3;

    if (info.actions.contains("deverb"))
        menu.addItem(deverbId, "De-Reverb");

    if (info.actions.contains("split"))
        menu.addItem(splitFurtherId, "Adaptive Split Further");

    if (info.hasChildren)
    {
        menu.addSeparator();
        menu.addItem(expandId, collapsedRecursiveIds.contains(itemId) ? "Expand Children"
                                                                      : "Collapse Children");
    }

    if (info.file.existsAsFile())
    {
        menu.addSeparator();
        addMidiMenuItems(menu, itemId);
    }

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    menu.showMenuAsync(
        juce::PopupMenu::Options(),
        [safeThis, itemId](int result)
        {
            if (safeThis == nullptr || result == 0)
                return;

            if (result == deverbId)
                safeThis->processor.launchRecursiveAction(itemId, "deverb");
            else if (result == splitFurtherId)
                safeThis->processor.launchRecursiveAction(itemId, "split");
            else if (result == expandId)
            {
                safeThis->toggleChildExpanded(itemId);
            }
            else
            {
                safeThis->handleMidiMenuResult(result, itemId, -1, itemId);
            }

            safeThis->refreshFromProcessor();
        });
}

bool StemLabAudioProcessorEditor::rootSupportsAdaptiveSplit(int stemIndex) const
{
    const auto name = StemLabAudioProcessor::getStemName(stemIndex);
    return name.equalsIgnoreCase("vocals") || name.equalsIgnoreCase("drums") ||
           name.equalsIgnoreCase("guitar") || name.equalsIgnoreCase("piano") ||
           name.equalsIgnoreCase("other");
}

bool StemLabAudioProcessorEditor::rootHasChildren(int stemIndex) const
{
    const auto root = StemLabAudioProcessor::getStemName(stemIndex);
    for (const auto& item : processor.getRecursiveStemItems())
        if (item.rootStem.equalsIgnoreCase(root))
            return true;
    return false;
}

void StemLabAudioProcessorEditor::toggleRootExpanded(int stemIndex)
{
    if (!juce::isPositiveAndBelow(stemIndex, StemLabAudioProcessor::stemCount))
        return;
    auto& expanded = rootExpanded[static_cast<size_t>(stemIndex)];
    expanded = !expanded;
    syncLanes();
}

void StemLabAudioProcessorEditor::toggleChildExpanded(const juce::String& itemId)
{
    const int index = collapsedRecursiveIds.indexOf(itemId);

    if (index >= 0)
        collapsedRecursiveIds.remove(index);
    else
        collapsedRecursiveIds.addIfNotAlreadyThere(itemId);

    syncLanes();
}

void StemLabAudioProcessorEditor::toggleLaneExpanded(int stemIndex, const juce::String& childId)
{
    /*
     * Collapsing rebuilds the child lanes, which destroys the very component
     * whose twisty was just clicked. Let the click finish unwinding first.
     */
    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    juce::MessageManager::callAsync(
        [safeThis, stemIndex, childId]
        {
            if (safeThis == nullptr)
                return;

            if (childId.isNotEmpty())
                safeThis->toggleChildExpanded(childId);
            else
                safeThis->toggleRootExpanded(stemIndex);

            safeThis->refreshFromProcessor();
        });
}

bool StemLabAudioProcessorEditor::isLaneExpanded(int stemIndex,
                                                 const juce::String& childId) const
{
    if (childId.isNotEmpty())
        return !collapsedRecursiveIds.contains(childId);

    return juce::isPositiveAndBelow(stemIndex, StemLabAudioProcessor::stemCount) &&
           rootExpanded[static_cast<size_t>(stemIndex)];
}

void StemLabAudioProcessorEditor::timerCallback()
{
    processor.refreshEngineProgressFromDisk();

    // A finished adaptive split hands the parent's place in the stem mix to
    // its children; this is where that reaches the monitor.
    processor.refreshStemMixIfNeeded();

    // Only the Ableton bridge ever writes those files; polling for them in
    // REAPER or a plain host is pure disk traffic at the timer rate.
    if (processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationAbletonLive)
    {
        processor.refreshAbletonSourceClipFromDisk();
        processor.refreshAbletonBridgeStatusFromDisk();
    }

    refreshFromProcessor();

    for (auto& lane : rootLanes)
        if (lane != nullptr)
            lane->repaint();

    for (auto& lane : childLanes)
        if (lane != nullptr)
            lane->repaint();

    // The record dot's pulse and the status spinner are functions of the
    // clock at paint time; keep them animating from the UI timer.
    statusIndicator.animate();

    if (processor.isCapturing())
    {
        recordSystemButton.repaint();
        recordInputButton.repaint();
    }
}

void StemLabAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    refreshFromProcessor();
}

juce::String StemLabAudioProcessorEditor::jobSummaryLine() const
{
    int readyCount = 0;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        if (processor.getCompletedStemFile(i).existsAsFile())
            ++readyCount;

    const auto duration = processor.getMainJobDurationSeconds() > 0.0
                              ? processor.getMainJobDurationSeconds()
                              : processor.getEngineElapsedSeconds();

    return "Separated " + juce::String(readyCount) + " stems in " + formatSeconds(duration) +
           juce::String::fromUTF8(" \xc2\xb7 refinement ") +
           (processor.isRefinementEnabled() ? "on" : "off");
}

juce::String StemLabAudioProcessorEditor::displayPath(const juce::File& directory) const
{
    if (directory == juce::File())
        return "-";

    auto path = directory.getFullPathName();

    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                          .getFullPathName();

    if (path.startsWith(home))
        path = "~" + path.substring(home.length());

    if (!path.endsWithChar(juce::File::getSeparatorChar()))
        path += juce::File::getSeparatorString();

    return path;
}

void StemLabAudioProcessorEditor::refreshFromProcessor()
{
    const auto capturing = processor.isCapturing();
    const auto recordingMode = processor.getStandaloneRecordingMode();
    const auto engineRunning = processor.isEngineRunning();
    const auto captureFile = processor.getCaptureFile();
    const auto captureExists = captureFile.existsAsFile();
    const auto jobDone = processor.hasSuccessfulJob();

    syncLanes();

    // ------------------------------------------------------------- header

    /*
     * The lanes only carry stems once a job has produced them, so both
     * pills and the readout stay dark until then - "0 of 6 selected" over
     * six empty lanes is noise, not information.
     */
    const auto [includedLanes, totalLanes] = laneSelectionCounts();

    const bool lanesLive = jobDone && !engineRunning && !capturing;

    selectAllButton.setEnabled(lanesLive && includedLanes < totalLanes);
    deselectAllButton.setEnabled(lanesLive && includedLanes > 0);

    selectionCountLabel.setText(lanesLive ? juce::String(includedLanes) + " of " +
                                                juce::String(totalLanes) + " selected"
                                          : juce::String(),
                                juce::dontSendNotification);

    // The zoom can also move from restored state or a second editor, so the
    // slider and the readout follow the processor rather than the last click.
    {
        const auto zoom = processor.getWaveformZoom();
        const auto index = waveformZoomIndexFor(zoom);

        zoomSlider.setValue(static_cast<double>(index) /
                            static_cast<double>(waveformZoomStepCount - 1));

        zoomLabel.setText(waveformZoomText(waveformZoomSteps[index]),
                          juce::dontSendNotification);
    }

    // Zoom is a view control, not a job control: it stays live whenever
    // there is a waveform to look at, including while a job runs.
    const bool haveWaveform = jobDone || captureExists;

    zoomResetButton->setEnabled(haveWaveform);
    zoomSlider.setEnabled(haveWaveform);
    zoomLabel.setAlpha(haveWaveform ? 1.0f : theme::metrics::disabledOpacity);

    // ------------------------------------------------------- source strip

    captureButton.setEnabled(!capturing && !engineRunning &&
                             !processor.isAwaitingAbletonSourceClip());

    const bool systemRecording = recordingMode == StemLabAudioProcessor::recordingSystem;
    const bool inputRecording = recordingMode == StemLabAudioProcessor::recordingInput;

    recordSystemButton.setEnabled(!engineRunning && !processor.isAwaitingAbletonSourceClip() &&
                                  (recordingMode == StemLabAudioProcessor::recordingNone ||
                                   systemRecording));

    recordSystemButton.setButtonText(systemRecording ? "Stop PC" : "Record PC");
    recordSystemButton.setRecordingActive(systemRecording && capturing);

    recordInputButton.setEnabled(!engineRunning &&
                                 (recordingMode == StemLabAudioProcessor::recordingNone ||
                                  inputRecording));

    recordInputButton.setButtonText(inputRecording ? "Stop In" : "Record In");
    recordInputButton.setRecordingActive(inputRecording && capturing);

    // The action segment doubles as Cancel while a job runs.
    const bool cancelPending = processor.isCancelRequested();

    separateControlShowsCancel = engineRunning;

    separateControl.setActionText(engineRunning ? (cancelPending ? "Cancelling..." : "Cancel")
                                                : "Separate");

    separateControl.setSeparateEnabled(
        engineRunning ? !cancelPending
                      : (!capturing && !processor.isAwaitingAbletonSourceClip() &&
                         captureExists));

    separateControl.setRefineOn(processor.isRefinementEnabled());

    // File block.
    juce::String fileName, fileMeta;

    if (processor.isAwaitingAbletonSourceClip())
    {
        fileName = "Reading Live clip...";
    }
    else if (capturing)
    {
        fileName = systemRecording ? "Recording PC audio" : "Recording input";
        fileMeta = formatSeconds(processor.getCapturedSeconds());
    }
    else if (captureExists)
    {
        const auto label = processor.getInputSourceLabel();
        fileName = label.isNotEmpty() ? label : captureFile.getFileName();

        juce::StringArray parts;

        if (processor.getCapturedSeconds() > 0.0)
            parts.add(formatSeconds(processor.getCapturedSeconds()));

        // Key/BPM sit with the rest of the source's facts rather than in a
        // panel of their own. Only while the analysis is on: its "off" text
        // would otherwise be permanent clutter on a line that is mostly
        // about the file itself.
        if (processor.isBeatThisEnabled())
        {
            const auto analysis = processor.getSourceAnalysisText();

            if (analysis.isNotEmpty())
                parts.add(analysis);
        }

        if (!processor.isStandaloneApp())
        {
            parts.add("beat " + juce::String(processor.getCaptureStartPpq(), 3));

            switch (processor.getHostIntegration())
            {
            case StemLabAudioProcessor::hostIntegrationReaper:
                parts.add("from selected item");
                break;
            case StemLabAudioProcessor::hostIntegrationAbletonLive:
                parts.add("from Live");
                break;
            default:
                break;
            }
        }

        fileMeta = parts.joinIntoString(" · ");
    }
    else
    {
        switch (processor.isStandaloneApp() ? StemLabAudioProcessor::hostIntegrationNone
                                            : processor.getHostIntegration())
        {
        case StemLabAudioProcessor::hostIntegrationAbletonLive:
            fileName = "No clip loaded";
            fileMeta = "Select a Live audio clip, then Use Live Clip";
            break;

        case StemLabAudioProcessor::hostIntegrationReaper:
            fileName = "No item loaded";
            fileMeta = "Select an item in REAPER, then Use Selected Item";
            break;

        case StemLabAudioProcessor::hostIntegrationNone:
        default:
            fileName = "No audio loaded";
            fileMeta = "Drop audio here, or click Select File";
            break;
        }
    }

    fileNameLabel.setText(fileName, juce::dontSendNotification);
    fileMetaLabel.setText(fileMeta, juce::dontSendNotification);

    // ---------------------------------------------------------- transport

    // The first sight of a finished job flips monitoring to the stem mix.
    if (jobDone && !sawSuccessfulJob)
    {
        sawSuccessfulJob = true;
        processor.setMonitorMode(StemLabAudioProcessor::monitorStems);
    }
    else if (!jobDone)
    {
        sawSuccessfulJob = false;
    }

    playButton.setEnabled((captureExists || jobDone) && !capturing && !engineRunning);
    playButton.setShowPause(processor.isTransportPlaying());

    const auto transportLength = processor.getTransportLengthSeconds();
    const auto transportPosition = processor.getTransportPositionSeconds();

    timeLabel.setText(formatSeconds(transportPosition) + " / " +
                          (transportLength > 0.0 ? formatSeconds(transportLength) : "--:--"),
                      juce::dontSendNotification);

    scrubber.setEnabled(transportLength > 0.0 && !capturing && !engineRunning);
    scrubber.setPosition(transportLength > 0.0 ? transportPosition / transportLength : 0.0);

    abControl.setEnabled(jobDone && !engineRunning && !capturing);
    abControl.setSelectedIndex(
        processor.getMonitorMode() == StemLabAudioProcessor::monitorStems ? 1 : 0);

    // --------------------------------------------------------------- lanes

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        if (auto* lane = rootLanes[static_cast<size_t>(i)].get())
        {
            const bool hasChildren = rootHasChildren(i);

            lane->setLayersAvailable(rootSupportsAdaptiveSplit(i) || hasChildren);
            lane->setChildState(hasChildren, isLaneExpanded(i, {}));
            lane->refresh();
        }
    }

    for (auto& lane : childLanes)
    {
        if (lane == nullptr)
            continue;

        lane->refresh();
    }

    // -------------------------------------------------------------- footer

    // Status line: while separating, stream the engine status. Once a job
    // is done, transient action feedback (send/save/rejection notices)
    // stays visible for a few seconds before the summary takes back over.
    const auto rawStatus = processor.getStatus();
    const auto nowMs = juce::Time::getMillisecondCounter();

    if (rawStatus != lastRawStatus)
    {
        lastRawStatus = rawStatus;
        lastStatusChangeMs = nowMs;
    }

    const bool showSummary =
        jobDone && !engineRunning && nowMs - lastStatusChangeMs > 5000;

    auto statusText = showSummary ? jobSummaryLine() : rawStatus;

    // Animated dots make long silent model phases (imports, first-run
    // downloads, big CPU chunks) visibly alive instead of frozen.
    if (engineRunning)
    {
        statusText = statusText.trimCharactersAtEnd(".");

        const auto phase =
            1 + (static_cast<int>(processor.getEngineElapsedSeconds() * 2.0) % 3);

        statusText += juce::String::repeatedString(".", phase);
    }

    statusLabel.setText(statusText, juce::dontSendNotification);

    statusIndicator.setState(engineRunning ? widgets::StatusIndicator::State::running
                             : jobDone     ? widgets::StatusIndicator::State::done
                                           : widgets::StatusIndicator::State::idle);

    // Same for the accent glows painted behind the primary actions.
    if (separateControl.isSeparateActionEnabled() != lastSeparateGlow)
    {
        lastSeparateGlow = separateControl.isSeparateActionEnabled();
        panelContent.repaint(separateControl.getBounds().expanded(14));
    }

    // The model cannot change under a running job.
    enginePrevButton->setEnabled(!engineRunning);
    engineNextButton->setEnabled(!engineRunning);
    engineSelector->setEnabled(!engineRunning);

    if (processor.getSeparatorEngineIndex() != lastSeparatorEngine)
    {
        lastSeparatorEngine = processor.getSeparatorEngineIndex();

        engineSelector->setLabel(processor.getSeparatorEngineDisplayName());

        separateControl.setTooltip("Runs after " + processor.getSeparatorEngineDisplayName() +
                                   " separation");
    }

    progressValue = processor.getEngineProgress();

    progressBar.setVisible(engineRunning);
    progressLabel.setVisible(engineRunning);

    if (engineRunning)
    {
        const auto eta = processor.getEngineEstimatedRemainingSeconds();

        // "34% · 02:10 · ETA 05:12": how far, how long so far, how much
        // left. The ETA drops off entirely while there is no estimate to
        // stand behind, rather than pinning "--:--" next to a live clock.
        // The separator goes through fromUTF8: JUCE's char* String
        // constructor mangles a non-ASCII literal into mojibake.
        const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");

        auto text = juce::String(juce::roundToInt(progressValue * 100.0)) + "%" + dot +
                    formatSeconds(processor.getEngineElapsedSeconds());

        if (eta >= 0.0)
            text += dot + "ETA " + formatSeconds(eta);

        progressLabel.setText(text, juce::dontSendNotification);
    }

    const auto jobPath = displayPath(processor.getJobRootDirectory());

    pathLabel.setText(jobPath, juce::dontSendNotification);

    // The folder icon hugs the start of the right-aligned path text. Parked
    // at the fixed left edge of the label it left a gap that grew with every
    // path shorter than the reserved column.
    if (!folderIconBounds.isEmpty() && pathLabel.getWidth() > 0)
    {
        const juce::Font pathFont{theme::fonts::footerPath()};

        const int textWidth = juce::jmin(
            pathLabel.getWidth(),
            juce::roundToInt(juce::GlyphArrangement::getStringWidth(pathFont, jobPath)) + 1);

        const int textLeft =
            pathLabel.getRight() - pathLabel.getBorderSize().getRight() - textWidth;

        const auto placed = folderIconBounds.withX(textLeft - theme::metrics::footer::folderIconGap -
                                                   folderIconBounds.getWidth());

        if (placed.getX() != folderIconBounds.getX())
        {
            panelContent.repaint(folderIconBounds.expanded(2));
            folderIconBounds = placed;
            panelContent.repaint(folderIconBounds.expanded(2));
        }
    }

    changeFolderButton.setEnabled(!engineRunning);

    int selectedCount = 0;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        if (processor.isStemEnabled(i))
            ++selectedCount;

    for (const auto& item : processor.getRecursiveStemItems())
        if (item.selected)
            ++selectedCount;

    saveButton.setEnabled(jobDone && !engineRunning && !capturing);
    insertButton.setEnabled(jobDone && !engineRunning && !capturing && selectedCount > 0);
    retryButton.setEnabled(jobDone && !engineRunning);

    const bool primaryGlow =
        (insertButton.isVisible() && insertButton.isEnabled()) ||
        (saveButton.isVisible() && saveButton.isEnabled() &&
         saveButton.getComponentID() == "primary");

    if (primaryGlow != lastPrimaryGlow)
    {
        lastPrimaryGlow = primaryGlow;
        panelContent.repaint(insertButton.getBounds().getUnion(saveButton.getBounds()).expanded(14));
    }
}

void StemLabAudioProcessorEditor::chooseStandaloneAudioFile()
{
    if (!processor.usesLocalFileWorkflow() || processor.isCapturing())
        return;

    audioFileChooser = std::make_unique<juce::FileChooser>(
        "Choose audio file", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.wav;*.flac;*.mp3;*.aiff;*.aif;*.ogg");

    audioFileChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                      juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& chooser)
                                  {
                                      const auto result = chooser.getResult();

                                      if (result.existsAsFile())
                                      {
                                          loadSourceFile(result);
                                          refreshFromProcessor();
                                      }
                                  });
}

void StemLabAudioProcessorEditor::chooseSaveFolder()
{
    if (processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationAbletonLive ||
        !processor.hasSuccessfulJob())
    {
        return;
    }

    outputFolderChooser = std::make_unique<juce::FileChooser>(
        "Choose where to save selected stems",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory));

    outputFolderChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                         juce::FileBrowserComponent::canSelectDirectories,
                                     [this](const juce::FileChooser& chooser)
                                     {
                                         const auto folder = chooser.getResult();

                                         if (folder.isDirectory())
                                             processor.saveSelectedStemsTo(folder);
                                     });
}

void StemLabAudioProcessorEditor::chooseJobRootFolder()
{
    auto start = processor.getJobRootDirectory();

    if (!start.isDirectory())
        start = juce::File::getSpecialLocation(juce::File::userMusicDirectory);

    jobFolderChooser = std::make_unique<juce::FileChooser>("Choose StemLab file location", start);

    jobFolderChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                      juce::FileBrowserComponent::canSelectDirectories,
                                  [this](const juce::FileChooser& chooser)
                                  {
                                      const auto folder = chooser.getResult();

                                      if (folder.isDirectory())
                                      {
                                          processor.setJobRootDirectory(folder);

                                          refreshFromProcessor();
                                      }
                                  });
}

void StemLabAudioProcessorEditor::addMidiMenuItems(juce::PopupMenu& menu, const juce::String& id)
{
    const bool converting = processor.isMidiConversionRunning();
    const bool haveMidi = processor.hasMidiInfo(id);

    menu.addSectionHeader("MIDI");

    menu.addItem(midiConvertId, haveMidi ? "Re-convert to MIDI" : "Convert to MIDI", !converting);

    if (!haveMidi)
        return;

    menu.addItem(midiAuditionId,
                 processor.isMidiAuditioning(id) ? "Stop Audition" : "Audition MIDI");

    menu.addItem(midiSaveId, "Save MIDI...");

#if JUCE_WINDOWS
    if (processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationAbletonLive)
        menu.addItem(midiSendId, "Send MIDI to Ableton");
#endif
}

void StemLabAudioProcessorEditor::handleMidiMenuResult(int result, const juce::String& id,
                                                       int stemIndex, const juce::String& childId)
{
    if (result == midiConvertId)
    {
        const bool started = childId.isNotEmpty()
                                 ? processor.launchRecursiveMidiConversion(childId)
                                 : processor.launchStemMidiConversion(stemIndex);

        if (!started)
            processor.postUiStatus("Could not start the MIDI conversion");

        return;
    }

    if (result == midiAuditionId)
    {
        if (processor.isMidiAuditioning(id))
            processor.stopMidiAudition();
        else
            processor.auditionMidi(id);

        return;
    }

    if (result == midiSendId)
    {
        processor.sendMidiToAbleton(id);
        return;
    }

    if (result != midiSaveId)
        return;

    const auto info = processor.getMidiInfo(id);

    if (!info.midiFile.existsAsFile())
    {
        processor.postUiStatus("That MIDI file is no longer on disk");
        return;
    }

    fileChooser = std::make_unique<juce::FileChooser>(
        "Save MIDI as", juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                            .getChildFile(info.midiFile.getFileName()),
        "*.mid");

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);
    const auto source = info.midiFile;

    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode |
                                 juce::FileBrowserComponent::canSelectFiles |
                                 juce::FileBrowserComponent::warnAboutOverwriting,
                             [safeThis, source](const juce::FileChooser& chooser)
                             {
                                 if (safeThis == nullptr)
                                     return;

                                 const auto target = chooser.getResult();

                                 if (target == juce::File())
                                     return;

                                 safeThis->processor.postUiStatus(
                                     source.copyFileTo(target)
                                         ? "MIDI saved to " + target.getFileName()
                                         : "Could not save the MIDI file");
                             });
}

juce::PopupMenu StemLabAudioProcessorEditor::makeMenu()
{
    juce::PopupMenu menu;

    // A top-level menu window has no parent to inherit from: JUCE reads the
    // look and feel off the PopupMenu itself, and draws stock JUCE without.
    menu.setLookAndFeel(&lookAndFeel);

    return menu;
}

void StemLabAudioProcessorEditor::setSeparatorEngine(int index)
{
    if (processor.isEngineRunning() ||
        !juce::isPositiveAndBelow(index, StemLabAudioProcessor::separatorEngineCount))
    {
        return;
    }

    processor.setSeparatorEngineIndex(index);

    processor.postUiStatus("Separation model: " + processor.getSeparatorEngineDisplayName());

    refreshFromProcessor();
}

void StemLabAudioProcessorEditor::stepSeparatorEngine(int delta)
{
    constexpr int count = StemLabAudioProcessor::separatorEngineCount;

    // Wrapping, not clamping: an arrow that dead-ends on a list of three is
    // just a button that sometimes does nothing.
    setSeparatorEngine((processor.getSeparatorEngineIndex() + delta + count) % count);
}

std::pair<int, int> StemLabAudioProcessorEditor::laneSelectionCounts() const
{
    int included = 0;
    int total = 0;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        ++total;

        if (processor.isStemEnabled(i))
            ++included;
    }

    /*
     * Every adaptive child counts, whether or not its lane is on screen: a
     * collapsed twisty hides the row, not the stem, and the job carries it
     * forward either way.
     */
    for (const auto& item : processor.getRecursiveStemItems())
    {
        ++total;

        if (processor.isRecursiveStemEnabled(item.id))
            ++included;
    }

    return {included, total};
}

void StemLabAudioProcessorEditor::setAllLanesIncluded(bool included)
{
    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        processor.setStemEnabled(i, included);

    for (const auto& item : processor.getRecursiveStemItems())
        processor.setRecursiveStemEnabled(item.id, included);

    for (auto& lane : rootLanes)
        if (lane != nullptr)
            lane->refresh();

    for (auto& lane : childLanes)
        if (lane != nullptr)
            lane->refresh();

    refreshFromProcessor();
}

void StemLabAudioProcessorEditor::applyWaveformZoomIndex(int index)
{
    const auto clamped = juce::jlimit(0, waveformZoomStepCount - 1, index);
    const auto zoom = waveformZoomSteps[clamped];

    processor.setWaveformZoom(zoom);

    zoomSlider.setValue(static_cast<double>(clamped) /
                        static_cast<double>(waveformZoomStepCount - 1));

    zoomLabel.setText(waveformZoomText(zoom), juce::dontSendNotification);

    // The lanes only redraw on the timer, which is a whole frame away and
    // does not run at all in some states; a zoom change has to show now.
    for (auto& lane : rootLanes)
        if (lane != nullptr)
            lane->repaint();

    for (auto& lane : childLanes)
        if (lane != nullptr)
            lane->repaint();
}

void StemLabAudioProcessorEditor::stepWaveformZoom(int delta)
{
    applyWaveformZoomIndex(waveformZoomIndexFor(processor.getWaveformZoom()) + delta);
}

void StemLabAudioProcessorEditor::showEngineMenu()
{
    auto menu = makeMenu();

    for (int i = 0; i < StemLabAudioProcessor::separatorEngineCount; ++i)
    {
        menu.addItem(i + 1, StemLabAudioProcessor::getSeparatorEngineMenuName(i),
                     !processor.isEngineRunning(), processor.getSeparatorEngineIndex() == i);
    }

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(engineSelector.get()),
                       [safeThis](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           safeThis->setSeparatorEngine(result - 1);
                       });
}

void StemLabAudioProcessorEditor::showWaveformColourMenu()
{
    static_assert(StemLabAudioProcessor::waveformColourCount == theme::waveform::paletteCount,
                  "The persisted waveform-colour range and the palette must stay in step");

    auto menu = makeMenu();

    for (int i = 0; i < theme::waveform::paletteCount; ++i)
    {
        menu.addItem(i + 1, theme::waveform::paletteName(i), true,
                     processor.getWaveformColourIndex() == i);
    }

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(paletteButton.get()),
                       [safeThis](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           const int palette = result - 1;

                           safeThis->processor.setWaveformColourIndex(palette);

                           safeThis->processor.postUiStatus(
                               "Waveform colour: " + theme::waveform::paletteName(palette));

                           safeThis->refreshFromProcessor();
                       });
}

void StemLabAudioProcessorEditor::showSettingsMenu()
{
    auto menu = makeMenu();

    if (processor.isStandaloneApp())
    {
        menu.addSectionHeader("Audio");
        menu.addItem(1, "Audio/MIDI Settings...");
        menu.addSeparator();
    }

    // The separation model and the waveform palette are header controls,
    // not menu items - see showEngineMenu / showWaveformColourMenu.

    // Beat grid drawn behind every lane's waveform.
    juce::PopupMenu gridMenu;

    gridMenu.addItem(gridModeMenuBase + StemLabAudioProcessor::gridHost, "Follow Host Tempo",
                     !processor.isStandaloneApp(),
                     processor.getWaveformGridMode() == StemLabAudioProcessor::gridHost);

    gridMenu.addItem(gridModeMenuBase + StemLabAudioProcessor::gridSource, "Follow Analysed Source",
                     true,
                     processor.getWaveformGridMode() == StemLabAudioProcessor::gridSource);

    gridMenu.addItem(gridModeMenuBase + StemLabAudioProcessor::gridManual, "Manual Tempo", true,
                     processor.getWaveformGridMode() == StemLabAudioProcessor::gridManual);

    menu.addSubMenu("Beat Grid", gridMenu);

    menu.addSeparator();

    menu.addSectionHeader("Source analysis");

    menu.addItem(analysisEnableId,
                 processor.isBeatThisEnabled() ? "Stop / Disable Key & BPM Analysis"
                                               : "Analyse Key & BPM",
                 !processor.isEngineRunning() || processor.isBeatThisEnabled());

    juce::PopupMenu analysisModeMenu;

    analysisModeMenu.addItem(analysisModeMenuBase + StemLabAudioProcessor::analysisFast, "Fast",
                             true,
                             processor.getSourceAnalysisMode() == StemLabAudioProcessor::analysisFast);

    analysisModeMenu.addItem(analysisModeMenuBase + StemLabAudioProcessor::analysisAccurate,
                             "Accurate (loads the Beat This! model)", true,
                             processor.getSourceAnalysisMode()
                                 == StemLabAudioProcessor::analysisAccurate);

    menu.addSubMenu("Analysis Quality", analysisModeMenu);

    juce::PopupMenu tempoMenu;

    const auto detected = processor.getDetectedSourceBpm();

    auto tempoLabel = [](const char* name, double bpm)
    {
        return bpm > 0.0 ? juce::String(name) + " (" + juce::String(bpm, 1) + " BPM)"
                         : juce::String(name);
    };

    tempoMenu.addItem(tempoMenuBase + StemLabAudioProcessor::tempoHalf,
                      tempoLabel("Half Time", processor.getHalfTimeSourceBpm()), detected > 0.0,
                      processor.getTempoInterpretation() == StemLabAudioProcessor::tempoHalf);

    tempoMenu.addItem(tempoMenuBase + StemLabAudioProcessor::tempoDetected,
                      tempoLabel("As Detected", detected), detected > 0.0,
                      processor.getTempoInterpretation() == StemLabAudioProcessor::tempoDetected);

    tempoMenu.addItem(tempoMenuBase + StemLabAudioProcessor::tempoDouble,
                      tempoLabel("Double Time", processor.getDoubleTimeSourceBpm()), detected > 0.0,
                      processor.getTempoInterpretation() == StemLabAudioProcessor::tempoDouble);

    menu.addSubMenu("Tempo Interpretation", tempoMenu, detected > 0.0);

    menu.addItem(analysisForgetId, "Forget Saved Correction For This Source",
                 processor.getSourceBpm() > 0.0);

    menu.addItem(analysisClearCacheId, "Clear Analysis Cache");

    menu.addSeparator();

    menu.addSectionHeader("StemLab engine");

    menu.addItem(2, "Choose engine executable...");

    menu.addItem(3, "Auto-detect engine");

    menu.addSeparator();

    menu.addItem(4, "Copy diagnostics to clipboard", processor.getEngineLog().isNotEmpty());

    if (processor.isStandaloneApp())
    {
        menu.addSeparator();
#if JUCE_WINDOWS
        menu.addSectionHeader("Ableton Live");
        menu.addItem(5, "Install / Repair Ableton Integration...");
#endif
    }

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(settingsButton.get()),
        [safeThis](int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)
            {
                safeThis->showStandaloneAudioSettings();
            }
            else if (result == 2)
            {
                safeThis->chooseEngineExecutable();
            }
            else if (result == 3)
            {
                safeThis->processor.resetEngineCommandToAutoDiscover();
            }
            else if (result == 4)
            {
                juce::SystemClipboard::copyTextToClipboard(safeThis->processor.getEngineLog());

                safeThis->processor.postUiStatus("Diagnostics copied to clipboard");
            }
            else if (result == 5)
            {
                safeThis->launchAbletonSetup();
            }
            else if (result >= gridModeMenuBase && result <= gridModeMenuBase + 2)
            {
                const int mode = result - gridModeMenuBase;

                safeThis->processor.setWaveformGridMode(mode);

                const juce::StringArray names{"host tempo", "analysed source", "manual tempo"};

                safeThis->processor.postUiStatus("Beat grid follows " + names[mode]);
            }
            else if (result >= analysisModeMenuBase && result <= analysisModeMenuBase + 1)
            {
                safeThis->processor.setSourceAnalysisMode(result - analysisModeMenuBase);
            }
            else if (result >= tempoMenuBase && result <= tempoMenuBase + 2)
            {
                safeThis->processor.setTempoInterpretation(result - tempoMenuBase);
            }
            else if (result == analysisEnableId)
            {
                safeThis->processor.setBeatThisEnabled(!safeThis->processor.isBeatThisEnabled());
            }
            else if (result == analysisForgetId)
            {
                if (safeThis->processor.forgetSourceCorrection())
                    safeThis->processor.postUiStatus("Saved analysis correction removed");
            }
            else if (result == analysisClearCacheId)
            {
                if (safeThis->processor.clearAnalysisCache())
                    safeThis->processor.postUiStatus("Clearing the analysis cache...");
            }

            safeThis->refreshFromProcessor();
        });
}

void StemLabAudioProcessorEditor::showFirstRunWelcome()
{
    if (!processor.isStandaloneApp())
        return;

    const auto root = portableRootDirectory();
    const auto portableEngine = root.getChildFile("Engine").getChildFile("python.exe");
    const auto setupScript = abletonSetupScript();

    // Only show onboarding for an actual extracted portable release. Normal
    // source/development builds should open directly without nagging.
    if (!portableEngine.existsAsFile() || !setupScript.existsAsFile() ||
        firstRunMarkerFile().existsAsFile())
    {
        return;
    }

    auto options = juce::MessageBoxOptions()
                       .withIconType(juce::MessageBoxIconType::InfoIcon)
                       .withTitle("Welcome to StemLab")
                       .withMessage("StemLab is ready to use as a standalone app.\n\n"
                                    "If you use Ableton Live, StemLab can set up its VST3 and "
                                    "Remote Script now. This does not copy the large ML engine a "
                                    "second time.")
                       .withButton("Set Up Ableton")
                       .withButton("Use Standalone")
                       .withAssociatedComponent(this);

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    juce::AlertWindow::showAsync(options,
                                 [safeThis](int result)
                                 {
                                     auto settings = stemLabSettingsDirectory();
                                     settings.createDirectory();
                                     firstRunMarkerFile().replaceWithText(
                                         "StemLab portable onboarding completed.\n");

                                     if (safeThis != nullptr && result == 1)
                                         safeThis->launchAbletonSetup();
                                 });
}

void StemLabAudioProcessorEditor::launchAbletonSetup()
{
    if (!processor.isStandaloneApp())
        return;

    const auto script = abletonSetupScript();

    if (!script.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Ableton setup not found",
            "scripts/install_ableton.ps1 was not found in the StemLab source tree.", "OK",
            this);
        return;
    }

    auto systemRoot = juce::SystemStats::getEnvironmentVariable("SystemRoot", "C:\\Windows");

    const auto powershell = juce::File(systemRoot)
                                .getChildFile("System32")
                                .getChildFile("WindowsPowerShell")
                                .getChildFile("v1.0")
                                .getChildFile("powershell.exe");

    if (!powershell.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "PowerShell not found",
            "StemLab could not start the Ableton setup helper.", "OK", this);
        return;
    }

    const auto arguments =
        "-NoProfile -ExecutionPolicy Bypass -File \"" + script.getFullPathName() + "\"";

    if (powershell.startAsProcess(arguments))
    {
        processor.postUiStatus("Ableton setup opened - follow the Windows prompt");
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Could not start Ableton setup",
            "Run scripts/install_ableton.ps1 from the StemLab source folder instead.", "OK",
            this);
    }
}

void StemLabAudioProcessorEditor::showStandaloneAudioSettings()
{
    if (!processor.isStandaloneApp())
        return;

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        holder->showAudioSettingsDialog();
        return;
    }
#endif

    processor.postUiStatus("Standalone audio settings are unavailable");
}

void StemLabAudioProcessorEditor::chooseEngineExecutable()
{
    auto start = juce::File(processor.getEngineCommand());

    if (!start.exists())
        start = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

    // On Linux the engine is an extensionless console script or a python
    // binary, so an "*.exe" filter showed an empty listing everywhere and
    // made manual engine selection impossible. An empty pattern shows all
    // files.
    const juce::String executablePattern {
#if JUCE_WINDOWS
        "*.exe"
#else
        ""
#endif
    };

    fileChooser = std::make_unique<juce::FileChooser>("Choose stemlab-plugin-job executable", start,
                                                      executablePattern);

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& chooser)
                             {
                                 const auto result = chooser.getResult();

                                 if (result.existsAsFile())
                                 {
                                     processor.setEngineCommand(result.getFullPathName());

                                     processor.postUiStatus("Engine path updated");

                                     refreshFromProcessor();
                                 }
                             });
}
