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

// Settings-menu ids: 1..5 are the fixed entries, 200+ the separators.
constexpr int waveformColourMenuBase = 300;

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

    if (length > 0.0 && thumbnail.getNumChannels() > 0 && !inner.isEmpty())
    {
        const int channels = juce::jmin(2, thumbnail.getNumChannels());

        const float playheadX =
            inner.getX() + static_cast<float>(juce::jmax(0.0, playNormalised)) * inner.getWidth();

        const auto centreY = inner.getCentreY();

        for (float x = inner.getX(); x < inner.getRight(); x += lanes::barPitch)
        {
            const auto start =
                static_cast<double>(x - inner.getX()) / static_cast<double>(inner.getWidth()) *
                length;

            const auto end = juce::jmax(
                start + 0.000001,
                static_cast<double>(x + lanes::barPitch - inner.getX()) /
                    static_cast<double>(inner.getWidth()) * length);

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
        if (playNormalised >= 0.0)
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

    const auto normalised = juce::jlimit(
        0.0, 1.0,
        static_cast<double>(event.mouseDownPosition.x) / static_cast<double>(getWidth()));

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

    settingsButton = std::make_unique<widgets::IconButton>(
        "settings", [](juce::Rectangle<float> b) { return stemlab::icons::sliders(b); },
        static_cast<float>(theme::metrics::header::settingsIcon), false,
        theme::metrics::header::settingsRadius, true, true);

    settingsButton->setTooltip("Settings");
    settingsButton->onClick = [this] { showSettingsMenu(); };
    panelContent.addAndMakeVisible(*settingsButton);

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
        case StemLabAudioProcessor::hostIntegrationAbletonLive:
            captureButton.setButtonText("Use Live Clip");
            captureButton.onClick = [this]
            {
                processor.requestAbletonSourceClip();
                refreshFromProcessor();
            };
            break;

        case StemLabAudioProcessor::hostIntegrationReaper:
            captureButton.setButtonText("Use Selected Item");
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
    namespace panel = theme::metrics::panel;

    g.fillAll(theme::colours::ground());

    // Panel: surface, 1px edge, soft drop shadow (Nocturne shadow-md).
    juce::DropShadow(theme::colours::panelShadow(), panel::shadowRadius,
                     {0, panel::shadowOffsetY})
        .drawForRectangle(g, panelBounds);

    g.setColour(theme::colours::surface());
    g.fillRoundedRectangle(panelBounds.toFloat(), panel::cornerRadius);

    // Only the drag signal draws an edge; at rest the shadow is the panel.
    if (dragActive)
    {
        g.setColour(theme::colours::accent());
        g.drawRoundedRectangle(panelBounds.toFloat().reduced(1.0f), panel::cornerRadius, 2.0f);
    }

    // Brand glyph.
    g.setColour(theme::colours::accent());
    g.fillPath(stemlab::icons::waveformBars(brandGlyphBounds.toFloat()));

    // Recessed source strip.
    g.setColour(theme::colours::ground());
    g.fillRoundedRectangle(sourceStripBounds.toFloat(), theme::metrics::source::radius);

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

    // Footer status check icon.
    if (!statusIconBounds.isEmpty() && processor.hasSuccessfulJob() &&
        !processor.isEngineRunning())
    {
        g.setColour(theme::colours::statusCheck());
        g.strokePath(stemlab::icons::check(statusIconBounds.toFloat().reduced(1.5f)),
                     juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
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
        g.fillRoundedRectangle(panelBounds.toFloat(), panel::cornerRadius);

        g.setColour(theme::colours::text());
        g.setFont(theme::fonts::title());
        g.drawFittedText("Drop audio to load", panelBounds.reduced(60),
                         juce::Justification::centred, 1);
    }
}

void StemLabAudioProcessorEditor::resized()
{
    // The host can resize before the constructor finishes building children.
    if (settingsButton == nullptr)
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

    // Header: brand glyph, title, settings icon.
    auto headerRow = inner.removeFromTop(header::settingsButton);

    settingsButton->setBounds(headerRow.removeFromRight(header::settingsButton));

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

        strip.removeFromRight(source::gap + source::separateExtraLeftGap);

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
        statusIconBounds = statusLine.removeFromLeft(footer::statusLineHeight);
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

    if (!supportsSplit && !hasChildren)
        return;

    juce::PopupMenu menu;
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

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    auto* target = rootLanes[static_cast<size_t>(stemIndex)].get();

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),
                       [safeThis, stemIndex](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;
                           if (result == 1)
                               safeThis->processor.launchRecursiveStemSplit(stemIndex);
                           else if (result == 2)
                               safeThis->toggleRootExpanded(stemIndex);
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

    juce::PopupMenu menu;

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

    // The record dot's pulse is a function of the clock at paint time; keep
    // it animating while a recording is running.
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

    return "Separated " + juce::String(readyCount) + " stems in " +
           formatSeconds(processor.getEngineElapsedSeconds()) + " · refinement " +
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

    // The status check icon is painted by the editor, not a child, so state
    // flips need an explicit repaint of its little region.
    panelContent.repaint(statusIconBounds);

    // Same for the accent glows painted behind the primary actions.
    if (separateControl.isSeparateActionEnabled() != lastSeparateGlow)
    {
        lastSeparateGlow = separateControl.isSeparateActionEnabled();
        panelContent.repaint(separateControl.getBounds().expanded(14));
    }

    // Tooltip tracks the engine chosen in the settings menu.
    if (processor.getSeparatorEngineIndex() != lastSeparatorEngine)
    {
        lastSeparatorEngine = processor.getSeparatorEngineIndex();
        separateControl.setTooltip("Runs after " + processor.getSeparatorEngineDisplayName() +
                                   " separation");
    }

    progressValue = processor.getEngineProgress();

    progressBar.setVisible(engineRunning);
    progressLabel.setVisible(engineRunning);

    if (engineRunning)
    {
        const auto eta = processor.getEngineEstimatedRemainingSeconds();

        progressLabel.setText(juce::String(juce::roundToInt(progressValue * 100.0)) + "% · ETA " +
                                  (eta >= 0.0 ? formatSeconds(eta) : "--:--"),
                              juce::dontSendNotification);
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

void StemLabAudioProcessorEditor::showSettingsMenu()
{
    juce::PopupMenu menu;

    if (processor.isStandaloneApp())
    {
        menu.addSectionHeader("Audio");
        menu.addItem(1, "Audio/MIDI Settings...");
        menu.addSeparator();
    }

    menu.addSectionHeader("Separator");

    juce::PopupMenu separatorMenu;

    const juce::StringArray separatorNames{"BS-RoFormer", "Demucs (htdemucs_6s)",
                                           "Hybrid (RoFormer + Demucs)"};

    for (int i = 0; i < separatorNames.size(); ++i)
    {
        separatorMenu.addItem(200 + i, separatorNames[i], !processor.isEngineRunning(),
                              processor.getSeparatorEngineIndex() == i);
    }

    menu.addSubMenu("Separation Engine", separatorMenu);

    menu.addSeparator();

    menu.addSectionHeader("Appearance");

    juce::PopupMenu waveformMenu;

    static_assert(StemLabAudioProcessor::waveformColourCount == theme::waveform::paletteCount,
                  "The persisted waveform-colour range and the palette must stay in step");

    for (int i = 0; i < theme::waveform::paletteCount; ++i)
    {
        waveformMenu.addItem(waveformColourMenuBase + i, theme::waveform::paletteName(i), true,
                             processor.getWaveformColourIndex() == i);
    }

    menu.addSubMenu("Waveform Colour", waveformMenu);

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
            else if (result >= 200 && result < 200 + StemLabAudioProcessor::separatorEngineCount)
            {
                safeThis->processor.setSeparatorEngineIndex(result - 200);

                safeThis->processor.postUiStatus(
                    "Separator: " + safeThis->processor.getSeparatorEngineDisplayName());
            }
            else if (result >= waveformColourMenuBase &&
                     result < waveformColourMenuBase + theme::waveform::paletteCount)
            {
                const int palette = result - waveformColourMenuBase;

                safeThis->processor.setWaveformColourIndex(palette);

                safeThis->processor.postUiStatus("Waveform colour: " +
                                                 theme::waveform::paletteName(palette));
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
