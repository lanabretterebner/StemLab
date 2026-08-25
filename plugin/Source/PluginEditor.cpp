#include "PluginEditor.h"
#include "StemLabPaths.h"
#include "StemLabTheme.h"
#include "BinaryData.h"

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace theme = stemlab::theme;

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

} // namespace

StemWaveformComponent::StemWaveformComponent(StemLabAudioProcessor& processorIn, int stemIndexIn,
                                             juce::AudioFormatManager& formatManager,
                                             juce::AudioThumbnailCache& thumbnailCache)
    : processor(processorIn), stemIndex(stemIndexIn),
      thumbnail(theme::metrics::waveform::thumbnailResolution, formatManager, thumbnailCache)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, false);
}

StemWaveformComponent::StemWaveformComponent(StemLabAudioProcessor& processorIn,
                                             juce::String recursiveIdIn,
                                             juce::AudioFormatManager& formatManager,
                                             juce::AudioThumbnailCache& thumbnailCache)
    : processor(processorIn), stemIndex(-3), recursive(true), recursiveId(std::move(recursiveIdIn)),
      thumbnail(theme::metrics::waveform::thumbnailResolution, formatManager, thumbnailCache)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, false);
}

void StemWaveformComponent::setFile(const juce::File& file)
{
    if (file == currentFile)
        return;

    currentFile = file;
    thumbnail.clear();

    if (currentFile.existsAsFile())
    {
        thumbnail.setSource(new juce::FileInputSource(currentFile));
    }

    repaint();
}

void StemWaveformComponent::paint(juce::Graphics& g)
{
    namespace waveform = theme::metrics::waveform;

    const auto full = getLocalBounds().toFloat();

    g.setColour(theme::colours::waveformBackground());
    g.fillRoundedRectangle(full, waveform::cornerRadius);

    const auto bounds = getLocalBounds().reduced(waveform::inset);

    if (bounds.isEmpty())
        return;

    // Mini-meter-style faint timing grid.
    g.setColour(theme::colours::waveformGrid());

    for (int division = 1; division < waveform::gridDivisions; ++division)
    {
        const auto x = bounds.getX() + bounds.getWidth() * division / waveform::gridDivisions;

        g.drawVerticalLine(x, static_cast<float>(bounds.getY()),
                           static_cast<float>(bounds.getBottom()));
    }

    g.setColour(theme::colours::waveformCentreLine());
    g.drawHorizontalLine(bounds.getCentreY(), static_cast<float>(bounds.getX()),
                         static_cast<float>(bounds.getRight()));

    const auto length = thumbnail.getTotalLength();

    if (length > 0.0 && thumbnail.getNumChannels() > 0)
    {
        const int channelCount =
            juce::jlimit(1, waveform::maxChannelLanes, thumbnail.getNumChannels());

        // Slice width is chosen to keep the six simultaneous waveform
        // previews cheap to repaint at the UI refresh rate; see the token's
        // note in StemLabTheme.h.
        constexpr int sliceWidth = waveform::sliceWidth;

        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto channelTop = bounds.getY() + bounds.getHeight() * channel / channelCount;

            const auto channelBottom =
                bounds.getY() + bounds.getHeight() * (channel + 1) / channelCount;

            const auto channelHeight = juce::jmax(1, channelBottom - channelTop);

            const auto centreY =
                static_cast<float>(channelTop) + static_cast<float>(channelHeight) * 0.5f;

            const auto halfHeight =
                static_cast<float>(channelHeight) * waveform::channelHalfHeightRatio;

            // The waveform colour is calculated per horizontal time slice,
            // so local volume determines the colour at that point in time.
            for (int x = bounds.getX(); x < bounds.getRight(); x += sliceWidth)
            {
                const auto normalisedStart = static_cast<double>(x - bounds.getX()) /
                                             static_cast<double>(juce::jmax(1, bounds.getWidth()));

                const auto normalisedEnd =
                    static_cast<double>(juce::jmin(bounds.getRight(), x + sliceWidth) -
                                        bounds.getX()) /
                    static_cast<double>(juce::jmax(1, bounds.getWidth()));

                const auto startTime = normalisedStart * length;

                const auto endTime = juce::jmax(startTime + 0.000001, normalisedEnd * length);

                float minimum = 0.0f;
                float maximum = 0.0f;

                thumbnail.getApproximateMinMax(startTime, endTime, channel, minimum, maximum);

                const auto localPeak = juce::jmax(std::abs(minimum), std::abs(maximum));

                const auto level = theme::palette::perceptualWaveformLevel(localPeak);

                const auto colour = theme::palette::waveformColourForLevel(
                    processor.getWaveformColourIndex(), level);

                const auto yTop = centreY - juce::jlimit(0.0f, 1.0f, maximum) * halfHeight;

                const auto yBottom = centreY - juce::jlimit(-1.0f, 0.0f, minimum) * halfHeight;

                // Keep extremely quiet material visible without pretending it
                // is loud. This matches the dense, thin low-level trace style
                // used by meter-oriented waveform displays.
                const auto visibleTop =
                    juce::jmin(yTop, centreY - waveform::minVisibleHalfHeight);

                const auto visibleBottom =
                    juce::jmax(yBottom, centreY + waveform::minVisibleHalfHeight);

                g.setColour(colour);

                g.drawLine(static_cast<float>(x), visibleTop, static_cast<float>(x), visibleBottom,
                           waveform::traceThickness);
            }
        }
    }
    else
    {
        g.setColour(theme::colours::waveformPlaceholderText());
        g.setFont(theme::fonts::waveformPlaceholder());

        g.drawText("waveform", bounds, juce::Justification::centred);
    }

    const auto previewIndex = processor.getPreviewStemIndex();

    const bool isCurrentPreview =
        recursive ? processor.getPreviewRecursiveId() == recursiveId : previewIndex == stemIndex;

    if (isCurrentPreview)
    {
        const auto previewLength = processor.getPreviewLengthSeconds();

        const auto previewPosition = processor.getPreviewPositionSeconds();

        if (previewLength > 0.0)
        {
            const auto normalised = juce::jlimit(0.0, 1.0, previewPosition / previewLength);

            const auto x = static_cast<float>(bounds.getX()) +
                           static_cast<float>(normalised) * static_cast<float>(bounds.getWidth());

            g.setColour(theme::colours::waveformPlayhead());
            g.drawLine(x, static_cast<float>(bounds.getY()), x,
                       static_cast<float>(bounds.getBottom()), waveform::playheadThickness);

            const auto timeText =
                formatSeconds(previewPosition) + " / " + formatSeconds(previewLength);

            auto badgeArea = bounds;
            auto badgeRow = badgeArea.removeFromTop(waveform::badgeRowHeight);

            auto badge = badgeRow.removeFromRight(waveform::badgeWidth);

            g.setColour(theme::colours::badgeFill());

            g.fillRoundedRectangle(badge.toFloat(), waveform::badgeCornerRadius);

            g.setColour(theme::colours::badgeText());
            g.setFont(theme::fonts::badge());

            g.drawText(timeText, badge.reduced(waveform::badgeTextInsetX, 0),
                       juce::Justification::centredRight);
        }
    }

    g.setColour(theme::colours::waveformOutline());

    g.drawRoundedRectangle(full.reduced(waveform::outlineInset), waveform::cornerRadius,
                           waveform::outlineThickness);
}

void StemWaveformComponent::mouseDown(const juce::MouseEvent&)
{
    // The gesture is ambiguous at press time: a click seeks, a drag exports.
    // Deciding at release keeps a drag from disturbing whatever the preview
    // transport is doing.
    externalDragStarted = false;
}

void StemWaveformComponent::mouseUp(const juce::MouseEvent& event)
{
    if (externalDragStarted || !currentFile.existsAsFile() || getWidth() <= 0)
        return;

    if (event.getDistanceFromDragStart() >= theme::metrics::waveform::clickVersusDragThreshold)
        return;

    const auto normalised = juce::jlimit(
        0.0, 1.0,
        static_cast<double>(event.mouseDownPosition.x) / static_cast<double>(getWidth()));

    if (recursive)
        processor.seekRecursiveStem(recursiveId, normalised);
    else
        processor.seekCompletedStem(stemIndex, normalised);

    repaint();
}

void StemWaveformComponent::mouseDrag(const juce::MouseEvent& event)
{
    /*
     * Dragging a completed waveform exports it to whatever the pointer lands
     * on - a DAW arrangement, a file manager, an editor. This is what makes
     * StemLab useful in hosts that have no integration path at all: every
     * host accepts an audio-file drop.
     *
     * A small threshold keeps click-to-seek working; canMoveFiles is false
     * because the job directory must keep its copy for Insert/Send/Save.
     */
    if (externalDragStarted || !currentFile.existsAsFile())
        return;

    if (event.getDistanceFromDragStart() < theme::metrics::waveform::clickVersusDragThreshold)
        return;

    // Keep the return value: a false start (e.g. the previous drag's XDND
    // transaction still pending) must not latch and kill the rest of the
    // gesture - the next mouseDrag event simply retries.
    externalDragStarted = juce::DragAndDropContainer::performExternalDragDropOfFiles(
        juce::StringArray{currentFile.getFullPathName()}, false, this);
}

RecursiveStemRowComponent::RecursiveStemRowComponent(
    StemLabAudioProcessor& processorIn, const StemLabRecursiveStemInfo& info,
    juce::AudioFormatManager& formatManager, juce::AudioThumbnailCache& thumbnailCache,
    std::function<void(const juce::String&)> toggleExpanded,
    std::function<bool(const juce::String&)> isExpanded)
    : processor(processorIn), item(info), toggleExpandedCallback(std::move(toggleExpanded)),
      isExpandedCallback(std::move(isExpanded))
{
    selectButton.onClick = [this]
    { processor.setRecursiveStemEnabled(item.id, selectButton.getToggleState()); };

    expandButton.onClick = [this]
    {
        const auto id = item.id;
        const auto callback = toggleExpandedCallback;
        juce::MessageManager::callAsync(
            [callback, id]
            {
                if (callback)
                    callback(id);
            });
    };

    playButton.onClick = [this] { processor.playRecursiveStem(item.id); };

    actionButton.setTooltip("Adaptive actions");
    actionButton.onClick = [this] { showActionMenu(); };

    waveform =
        std::make_unique<StemWaveformComponent>(processor, item.id, formatManager, thumbnailCache);

    addAndMakeVisible(selectButton);
    addAndMakeVisible(expandButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(actionButton);
    addAndMakeVisible(*waveform);

    setInfo(info);
}

void RecursiveStemRowComponent::setInfo(const StemLabRecursiveStemInfo& info)
{
    item = info;
    auto displayLabel = item.label;
    if (item.estimatedSourceCount > 1 && (item.actions.contains("split") || item.hasChildren))
    {
        displayLabel += " (est. " + juce::String(item.estimatedSourceCount) + " sources)";
    }
    selectButton.setButtonText(displayLabel);
    selectButton.setTooltip("Category: " + item.category + " | confidence " +
                            juce::String(juce::roundToInt(item.confidence * 100.0)) + "%");
    expandButton.setVisible(item.hasChildren);
    expandButton.setButtonText(
        item.hasChildren && isExpandedCallback && isExpandedCallback(item.id) ? "v" : ">");
    selectButton.setToggleState(item.selected, juce::dontSendNotification);

    actionButton.setVisible(!item.actions.isEmpty());

    if (waveform != nullptr)
        waveform->setFile(item.file);

    resized();
}

void RecursiveStemRowComponent::refresh(bool engineRunning, bool previewPlaying)
{
    const bool ready = !engineRunning && item.file.existsAsFile();

    selectButton.setEnabled(ready);
    playButton.setEnabled(ready);
    actionButton.setEnabled(ready && !item.actions.isEmpty());
    actionButton.setVisible(!item.actions.isEmpty());
    expandButton.setVisible(item.hasChildren);
    expandButton.setEnabled(item.hasChildren);
    expandButton.setButtonText(
        item.hasChildren && isExpandedCallback && isExpandedCallback(item.id) ? "v" : ">");

    selectButton.setToggleState(processor.isRecursiveStemEnabled(item.id),
                                juce::dontSendNotification);

    playButton.setButtonText(
        previewPlaying && processor.getPreviewRecursiveId() == item.id ? "Pause" : "Play");

    if (waveform != nullptr)
    {
        waveform->setFile(item.file);
        waveform->setEnabled(ready);
        waveform->repaint();
    }
}

void RecursiveStemRowComponent::resized()
{
    namespace adaptive = theme::metrics::adaptiveRow;

    auto row = getLocalBounds();

    const int indent = juce::jlimit(adaptive::indentMin, adaptive::indentMax,
                                    item.depth * adaptive::indentPerDepth);
    row.removeFromLeft(indent);

    if (item.hasChildren)
    {
        expandButton.setBounds(row.removeFromLeft(adaptive::expandWidth)
                                   .reduced(adaptive::expandPadX, adaptive::expandPadY));
        row.removeFromLeft(adaptive::expandGap);
    }
    else
    {
        expandButton.setBounds(0, 0, 0, 0);
        row.removeFromLeft(adaptive::noExpandIndent);
    }

    const int actionWidth = item.actions.isEmpty() ? 0 : adaptive::menuWidth;

    if (actionWidth > 0)
    {
        actionButton.setBounds(row.removeFromRight(actionWidth)
                                   .reduced(adaptive::buttonPadX, adaptive::buttonPadY));
        row.removeFromRight(adaptive::menuGap);
    }
    else
    {
        actionButton.setBounds(0, 0, 0, 0);
    }

    playButton.setBounds(row.removeFromRight(adaptive::playWidth)
                             .reduced(adaptive::buttonPadX, adaptive::buttonPadY));
    row.removeFromRight(adaptive::playGap);

    const int labelWidth = juce::jlimit(adaptive::labelMinWidth, adaptive::labelMaxWidth,
                                        getWidth() / adaptive::labelWidthDivisor);
    selectButton.setBounds(row.removeFromLeft(labelWidth).reduced(0, adaptive::contentPadY));
    row.removeFromLeft(adaptive::labelGap);

    if (waveform != nullptr)
        waveform->setBounds(row.reduced(0, adaptive::contentPadY));
}

void RecursiveStemRowComponent::showActionMenu()
{
    juce::PopupMenu menu;

    constexpr int deverbId = 1;
    constexpr int splitFurtherId = 2;

    bool hasActions = false;

    if (item.actions.contains("deverb"))
    {
        menu.addItem(deverbId, "De-Reverb");
        hasActions = true;
    }

    if (item.actions.contains("split"))
    {
        menu.addItem(splitFurtherId, "Adaptive Split Further");
        hasActions = true;
    }

    if (!hasActions)
        return;

    auto safeThis = juce::Component::SafePointer<RecursiveStemRowComponent>(this);

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&actionButton),
        [safeThis](int result)
        {
            if (safeThis == nullptr || result == 0)
                return;

            if (result == deverbId)
                safeThis->processor.launchRecursiveAction(safeThis->item.id, "deverb");
            else if (result == splitFurtherId)
                safeThis->processor.launchRecursiveAction(safeThis->item.id, "split");
        });
}

StemLabAudioProcessorEditor::StemLabAudioProcessorEditor(StemLabAudioProcessor& processorIn)
    : AudioProcessorEditor(&processorIn), processor(processorIn)
{
    namespace window = theme::metrics::window;

    setLookAndFeel(&lookAndFeel);

    setSize(window::defaultWidth, window::defaultHeight);
    setResizable(true, true);

    // The UI is intentionally fluid. At the minimum size the waveform rows
    // collapse to compact strips; extra vertical space is given directly to
    // the six waveform rows instead of becoming dead space.
    setResizeLimits(window::minWidth, window::minHeight, window::maxWidth, window::maxHeight);

    if (processor.isStandaloneApp())
    {
        auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

        juce::MessageManager::callAsync(
            [safeThis]
            {
                if (safeThis == nullptr)
                    return;

                if (auto* window = safeThis->findParentComponentOfClass<juce::DocumentWindow>())
                {
                    window->setUsingNativeTitleBar(true);
                    window->setName("StemLab");

                    const auto appIcon = juce::ImageFileFormat::loadFrom(
                        BinaryData::StemLabIcon_png, BinaryData::StemLabIcon_pngSize);

                    if (appIcon.isValid())
                        window->setIcon(appIcon);
                }
            });
    }

    titleLabel.setText("StemLab", juce::dontSendNotification);

    titleLabel.setFont(theme::fonts::title());

    addAndMakeVisible(titleLabel);

    subtitleLabel.setColour(juce::Label::textColourId, theme::colours::textMuted());

    subtitleLabel.setText(
        [this]() -> juce::String
        {
            if (processor.isStandaloneApp())
                return "Load or record audio, split it, audition stems, then save";

            switch (processor.getHostIntegration())
            {
            case StemLabAudioProcessor::hostIntegrationReaper:
                return "Use the selected REAPER item, split it, audition stems, then insert";

            case StemLabAudioProcessor::hostIntegrationAbletonLive:
                return "Use a Live clip or record PC audio, split it, audition stems, then send";

            case StemLabAudioProcessor::hostIntegrationNone:
            default:
                return "Drop or select audio, split it, audition stems, then save";
            }
        }(),
        juce::dontSendNotification);

    addAndMakeVisible(subtitleLabel);

    settingsButton.onClick = [this] { showSettingsMenu(); };
    addAndMakeVisible(settingsButton);

    captureButton.setColour(juce::TextButton::buttonColourId, theme::colours::accent());

    if (processor.isStandaloneApp())
    {
        captureButton.setButtonText("Select File");
        captureButton.onClick = [this] { chooseStandaloneAudioFile(); };

        stopButton.setVisible(false);

        playButton.setButtonText("Play");
        playButton.onClick = [this]
        {
            processor.toggleStandalonePlayback();
            refreshFromProcessor();
        };
        addAndMakeVisible(playButton);

        recordSystemButton.setButtonText("Record System");
        recordSystemButton.setColour(juce::TextButton::buttonColourId,
                                     theme::colours::recordSystem());

        recordSystemButton.onClick = [this]
        {
            if (processor.getStandaloneRecordingMode() == StemLabAudioProcessor::recordingSystem)
            {
                processor.stopSystemAudioRecording();
            }
            else
            {
                processor.startSystemAudioRecording();
            }

            refreshFromProcessor();
        };
        addAndMakeVisible(recordSystemButton);
        recordSystemButton.setVisible(StemLabAudioProcessor::isSystemAudioCaptureSupported());

        recordInputButton.setButtonText("Record Input");
        recordInputButton.setColour(juce::TextButton::buttonColourId,
                                    theme::colours::recordInput());

        recordInputButton.onClick = [this]
        {
            if (processor.getStandaloneRecordingMode() == StemLabAudioProcessor::recordingInput)
            {
                processor.stopStandaloneRecording();
            }
            else
            {
                processor.startStandaloneRecording();
            }

            refreshFromProcessor();
        };
        addAndMakeVisible(recordInputButton);
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

        stopButton.setVisible(false);

        playButton.setButtonText("Play");
        playButton.onClick = [this]
        {
            processor.toggleStandalonePlayback();
            refreshFromProcessor();
        };
        addAndMakeVisible(playButton);

        recordSystemButton.setButtonText("Record PC");
        recordSystemButton.setColour(juce::TextButton::buttonColourId,
                                     theme::colours::recordSystem());

        recordSystemButton.onClick = [this]
        {
            if (processor.getStandaloneRecordingMode() == StemLabAudioProcessor::recordingSystem)
            {
                processor.stopSystemAudioRecording();
            }
            else
            {
                processor.startSystemAudioRecording();
            }

            refreshFromProcessor();
        };
        addAndMakeVisible(recordSystemButton);
        recordSystemButton.setVisible(StemLabAudioProcessor::isSystemAudioCaptureSupported());

        recordInputButton.setVisible(false);
    }

    addAndMakeVisible(captureButton);
    addAndMakeVisible(stopButton);

    captureTimeLabel.setJustificationType(juce::Justification::centredLeft);

    captureTimeLabel.setColour(juce::Label::textColourId, theme::colours::textMuted());

    addAndMakeVisible(captureTimeLabel);

    refinementButton.setToggleState(processor.isRefinementEnabled(), juce::dontSendNotification);

    refinementButton.setTooltip("Runs after " + processor.getSeparatorEngineDisplayName() +
                                " separation");

    refinementButton.onClick = [this]
    { processor.setRefinementEnabled(refinementButton.getToggleState()); };

    addAndMakeVisible(refinementButton);

    separateButton.setColour(juce::TextButton::buttonColourId, theme::colours::accent());

    separateButton.setButtonText("Separate All Stems");

    separateButton.onClick = [this]
    {
        processor.launchSeparationAndExport();
        refreshFromProcessor();
    };

    addAndMakeVisible(separateButton);

    progressBar.setColour(juce::ProgressBar::foregroundColourId, theme::colours::accent());

    progressBar.setColour(juce::ProgressBar::backgroundColourId, theme::colours::progressTrack());

    progressBar.setPercentageDisplay(true);
    addAndMakeVisible(progressBar);

    statusLabel.setFont(theme::fonts::status());

    addAndMakeVisible(statusLabel);

    timingLabel.setColour(juce::Label::textColourId, theme::colours::textMuted());

    addAndMakeVisible(timingLabel);

    stemsLabel.setFont(theme::fonts::sectionHeading());

    stemsLabel.setText(
        [this]() -> juce::String
        {
            if (processor.isStandaloneApp())
                return "Audition stems, then choose what to save";

            switch (processor.getHostIntegration())
            {
            case StemLabAudioProcessor::hostIntegrationAbletonLive:
                return "Audition stems, then choose what to send to Ableton";

            case StemLabAudioProcessor::hostIntegrationReaper:
                return "Audition stems, then choose what to insert";

            case StemLabAudioProcessor::hostIntegrationNone:
            default:
                return "Audition stems, then choose what to save";
            }
        }(),
        juce::dontSendNotification);

    addAndMakeVisible(stemsLabel);

    rootExpanded.fill(true);
    stemViewport.setViewedComponent(&stemTreeContent, false);
    stemViewport.setScrollBarsShown(true, false);
    stemViewport.setScrollBarThickness(theme::metrics::stemTree::scrollbarThickness);
    addAndMakeVisible(stemViewport);

    waveformFormats.registerBasicFormats();

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        auto& button = stemButtons[static_cast<size_t>(i)];

        auto& expandButton = stemExpandButtons[static_cast<size_t>(i)];

        auto& preview = stemPlayButtons[static_cast<size_t>(i)];

        auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];

        const auto name = StemLabAudioProcessor::getStemName(i);

        button.setButtonText(name.substring(0, 1).toUpperCase() + name.substring(1));

        button.setToggleState(processor.isStemEnabled(i), juce::dontSendNotification);

        button.onClick = [this, i]
        { processor.setStemEnabled(i, stemButtons[static_cast<size_t>(i)].getToggleState()); };

        expandButton.setButtonText(">");
        expandButton.setTooltip("Expand/collapse adaptive children");
        expandButton.onClick = [this, i] { toggleRootExpanded(i); };

        preview.setButtonText("Play");
        preview.onClick = [this, i]
        {
            processor.playCompletedStem(i);
            refreshFromProcessor();
        };

        recursiveButton.setButtonText("...");
        recursiveButton.setTooltip("Adaptive split options");
        recursiveButton.setVisible(rootSupportsAdaptiveSplit(i));
        recursiveButton.onClick = [this, i] { showRootRecursiveMenu(i); };

        stemTreeContent.addAndMakeVisible(expandButton);
        stemTreeContent.addAndMakeVisible(button);
        stemTreeContent.addAndMakeVisible(preview);
        stemTreeContent.addAndMakeVisible(recursiveButton);

        waveformComponents[static_cast<size_t>(i)] =
            std::make_unique<StemWaveformComponent>(processor, i, waveformFormats, waveformCache);

        stemTreeContent.addAndMakeVisible(*waveformComponents[static_cast<size_t>(i)]);
    }

    // Save-to-folder is useful in every mode except the Ableton bridge,
    // whose workflow is send-only by design.
    saveSelectedButton.setVisible(processor.getHostIntegration() !=
                                  StemLabAudioProcessor::hostIntegrationAbletonLive);

    saveSelectedButton.onClick = [this] { chooseSaveFolder(); };

    addAndMakeVisible(saveSelectedButton);

    sendSelectedButton.setVisible(!processor.usesLocalFileWorkflow());

    sendSelectedButton.setButtonText(processor.getHostIntegration() ==
                                             StemLabAudioProcessor::hostIntegrationReaper
                                         ? "Insert Stems"
                                         : "Send Selected");

    sendSelectedButton.setColour(juce::TextButton::buttonColourId, theme::colours::accent());

    sendSelectedButton.onClick = [this]
    {
        if (processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationReaper)
            processor.insertSelectedStemsIntoReaper();
        else
            processor.sendSelectedStemsToAbleton();

        refreshFromProcessor();
    };

    addAndMakeVisible(sendSelectedButton);

    // Retry exists for the Ableton bridge's asynchronous import; REAPER
    // insertion is synchronous, so there is nothing to retry.
    retryImportButton.setVisible(processor.getHostIntegration() ==
                                 StemLabAudioProcessor::hostIntegrationAbletonLive);

    retryImportButton.onClick = [this]
    {
        processor.retryAbletonImport();
        refreshFromProcessor();
    };

    addAndMakeVisible(retryImportButton);

    openJobButton.onClick = [this] { chooseJobRootFolder(); };

    addAndMakeVisible(openJobButton);

    bridgeLabel.setVisible(false);

    processor.addChangeListener(this);

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
    for (const auto& path : files)
    {
        if (isSupportedAudioFile(juce::File(path)))
        {
            return true;
        }
    }

    return false;
}

void StemLabAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    dragActive = false;
    repaint();

    if (processor.isCapturing())
        return;

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
    repaint();
}

void StemLabAudioProcessorEditor::fileDragExit(const juce::StringArray&)
{
    dragActive = false;
    repaint();
}

void StemLabAudioProcessorEditor::paint(juce::Graphics& g)
{
    namespace panel = theme::metrics::panel;

    g.fillAll(theme::colours::windowBackground());

    auto area = getLocalBounds().toFloat().reduced(panel::paintMargin);

    auto panelArea = area.withTrimmedTop(panel::headerReserve);

    g.setColour(theme::colours::panel());
    g.fillRoundedRectangle(panelArea, panel::cornerRadius);

    g.setColour(dragActive ? theme::colours::dragBorder() : theme::colours::panelOutline());

    g.drawRoundedRectangle(panelArea, panel::cornerRadius,
                           dragActive ? panel::dragOutlineThickness : panel::outlineThickness);

    if (dragActive)
    {
        g.setColour(theme::colours::dragOverlay());

        g.fillRoundedRectangle(panelArea, panel::cornerRadius);

        g.setColour(theme::colours::dragPromptText());

        g.setFont(theme::fonts::dragPrompt());

        g.drawFittedText("Drop audio to load", getLocalBounds().reduced(panel::dragPromptInset),
                         juce::Justification::centred, 1);
    }
}

void StemLabAudioProcessorEditor::showRootRecursiveMenu(int stemIndex)
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
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(
                           &stemRecursiveButtons[static_cast<size_t>(stemIndex)]),
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
    syncRecursiveRows();
    resized();
}

void StemLabAudioProcessorEditor::toggleRecursiveExpanded(const juce::String& itemId)
{
    const int index = collapsedRecursiveIds.indexOf(itemId);
    if (index >= 0)
        collapsedRecursiveIds.remove(index);
    else
        collapsedRecursiveIds.addIfNotAlreadyThere(itemId);
    syncRecursiveRows();
    resized();
}

bool StemLabAudioProcessorEditor::isRecursiveExpanded(const juce::String& itemId) const
{
    return !collapsedRecursiveIds.contains(itemId);
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

void StemLabAudioProcessorEditor::syncRecursiveRows()
{
    const auto items = getVisibleRecursiveItems();

    bool rebuild = items.size() != recursiveRows.size();

    if (!rebuild)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (recursiveRows[i] == nullptr || recursiveRows[i]->getItemId() != items[i].id)
            {
                rebuild = true;
                break;
            }
        }
    }

    if (rebuild)
    {
        recursiveRows.clear();
        recursiveRows.reserve(items.size());

        for (const auto& item : items)
        {
            auto safeEditor = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

            auto row = std::make_unique<RecursiveStemRowComponent>(
                processor, item, waveformFormats, waveformCache,
                [safeEditor](const juce::String& id)
                {
                    if (safeEditor != nullptr)
                        safeEditor->toggleRecursiveExpanded(id);
                },
                [safeEditor](const juce::String& id)
                { return safeEditor != nullptr ? safeEditor->isRecursiveExpanded(id) : true; });

            stemTreeContent.addAndMakeVisible(*row);
            recursiveRows.push_back(std::move(row));
        }

        resized();
    }
    else
    {
        for (size_t i = 0; i < items.size(); ++i)
            recursiveRows[i]->setInfo(items[i]);
    }
}

void StemLabAudioProcessorEditor::resized()
{
    namespace metrics = theme::metrics;
    namespace controls = metrics::controls;
    namespace stemTree = metrics::stemTree;

    const int width = getWidth();

    const int height = getHeight();

    const bool narrow = width < metrics::compactWidth;

    const bool shallow = height < metrics::compactHeight;

    // Slightly smaller outside padding at compact sizes.
    auto area = getLocalBounds().reduced(metrics::window::outerPadding(narrow || shallow));

    auto header = area.removeFromTop(metrics::header::height(shallow));

    auto titleRow = header.removeFromTop(metrics::header::titleRowHeight(shallow));

    settingsButton.setBounds(titleRow.removeFromRight(metrics::header::settingsButtonWidth(narrow)));

    titleRow.removeFromRight(metrics::header::settingsButtonGap);
    titleLabel.setBounds(titleRow);
    subtitleLabel.setBounds(header);

    area.removeFromTop(metrics::header::gapBelow(shallow));

    area.reduce(metrics::panel::insetX(narrow), metrics::panel::insetY(shallow));

    auto inputRow = area.removeFromTop(controls::inputRowHeight(shallow));

    const int useClipWidth = processor.isStandaloneApp()
                                 ? controls::captureButtonWidthStandalone(narrow)
                                 : controls::captureButtonWidthHosted(narrow);

    captureButton.setBounds(inputRow.removeFromLeft(useClipWidth));

    inputRow.removeFromLeft(controls::buttonGap);

    playButton.setBounds(inputRow.removeFromLeft(controls::playButtonWidth(narrow)));

    inputRow.removeFromLeft(controls::buttonGap);

    if (!processor.isStandaloneApp() && StemLabAudioProcessor::isSystemAudioCaptureSupported())
    {
        recordSystemButton.setBounds(
            inputRow.removeFromLeft(controls::recordSystemWidthHosted(narrow)));

        inputRow.removeFromLeft(controls::buttonGapWide);
    }

    captureTimeLabel.setBounds(inputRow);

    if (processor.isStandaloneApp())
    {
        area.removeFromTop(controls::tightRowGap(shallow));

        auto recordingRow = area.removeFromTop(controls::recordingRowHeight(shallow));

        if (StemLabAudioProcessor::isSystemAudioCaptureSupported())
        {
            recordSystemButton.setBounds(
                recordingRow.removeFromLeft(controls::recordSystemWidthStandalone(narrow)));

            recordingRow.removeFromLeft(controls::buttonGap);
        }

        recordInputButton.setBounds(
            recordingRow.removeFromLeft(controls::recordInputWidth(narrow)));
    }

    area.removeFromTop(controls::rowGap(shallow));

    refinementButton.setBounds(area.removeFromTop(controls::refinementHeight(shallow)));

    area.removeFromTop(controls::rowGap(shallow));

    separateButton.setBounds(area.removeFromTop(controls::separateButtonHeight(shallow)));

    area.removeFromTop(controls::rowGap(shallow));

    statusLabel.setBounds(area.removeFromTop(controls::statusHeight(shallow)));

    progressBar.setBounds(area.removeFromTop(controls::progressHeight(shallow)));

    timingLabel.setBounds(area.removeFromTop(controls::timingHeight(shallow)));

    area.removeFromTop(controls::tightRowGap(shallow));

    stemsLabel.setBounds(area.removeFromTop(controls::stemsHeadingHeight(shallow)));

    // Reserve the bottom action row first. Everything between the stem label
    // and that row becomes waveform space.
    auto actionRow = area.removeFromBottom(metrics::actionRow::height(shallow));

    area.removeFromBottom(metrics::actionRow::gapAbove(shallow));

    const int totalStemRows =
        StemLabAudioProcessor::stemCount + static_cast<int>(recursiveRows.size());
    const int preferredRowHeight =
        juce::jlimit(stemTree::minRowHeight(shallow), stemTree::maxRowHeight,
                     area.getHeight() / juce::jmax(1, totalStemRows));
    const int contentHeight = juce::jmax(area.getHeight(), preferredRowHeight * totalStemRows);

    stemViewport.setBounds(area);
    const int contentWidth = juce::jmax(stemTree::contentMinWidth,
                                        stemViewport.getWidth() - stemTree::viewportWidthInset);
    stemTreeContent.setSize(contentWidth, contentHeight);
    auto treeArea = stemTreeContent.getLocalBounds().reduced(stemTree::treeInsetX, 0);

    const int checkboxWidth = stemTree::checkboxWidth(narrow);
    const int playWidth = stemTree::playWidth(narrow);
    const int actionWidth = stemTree::menuWidth(narrow);
    const int expandWidth = stemTree::expandWidth;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        auto row = treeArea.removeFromTop(preferredRowHeight);
        const int rowPad = stemTree::rowPad(preferredRowHeight < stemTree::rowPadThreshold);

        auto& expandButton = stemExpandButtons[static_cast<size_t>(i)];
        const bool hasChildren = rootHasChildren(i);
        expandButton.setVisible(hasChildren);
        expandButton.setButtonText(hasChildren && rootExpanded[static_cast<size_t>(i)] ? "v" : ">");
        if (hasChildren)
            expandButton.setBounds(row.removeFromLeft(expandWidth).reduced(1, rowPad));
        else
        {
            expandButton.setBounds(0, 0, 0, 0);
            row.removeFromLeft(expandWidth);
        }

        auto checkboxArea = row.removeFromLeft(checkboxWidth).reduced(0, rowPad);
        stemButtons[static_cast<size_t>(i)].setBounds(checkboxArea);
        row.removeFromLeft(stemTree::checkboxGap);

        auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];
        if (rootSupportsAdaptiveSplit(i) || hasChildren)
        {
            recursiveButton.setBounds(row.removeFromRight(actionWidth).reduced(0, rowPad));
            row.removeFromRight(stemTree::menuGap);
        }
        else
            recursiveButton.setBounds(0, 0, 0, 0);

        stemPlayButtons[static_cast<size_t>(i)].setBounds(
            row.removeFromRight(playWidth).reduced(0, rowPad));
        row.removeFromRight(stemTree::playGap);

        if (auto* waveform = waveformComponents[static_cast<size_t>(i)].get())
            waveform->setBounds(row.reduced(0, juce::jmax(1, rowPad - 1)));

        const auto rootName = StemLabAudioProcessor::getStemName(i);
        for (auto& recursiveRow : recursiveRows)
        {
            if (recursiveRow != nullptr && recursiveRow->getRootStem().equalsIgnoreCase(rootName))
            {
                recursiveRow->setBounds(treeArea.removeFromTop(preferredRowHeight));
            }
        }
    }

    switch (processor.isStandaloneApp() ? StemLabAudioProcessor::hostIntegrationNone
                                        : processor.getHostIntegration())
    {
    case StemLabAudioProcessor::hostIntegrationAbletonLive:
        sendSelectedButton.setBounds(
            actionRow.removeFromLeft(metrics::actionRow::sendWidthAbleton(narrow)));

        actionRow.removeFromLeft(controls::buttonGap);

        retryImportButton.setBounds(
            actionRow.removeFromLeft(metrics::actionRow::retryWidth(narrow)));

        actionRow.removeFromLeft(controls::buttonGap);
        break;

    case StemLabAudioProcessor::hostIntegrationReaper:
        sendSelectedButton.setBounds(
            actionRow.removeFromLeft(metrics::actionRow::sendWidthReaper(narrow)));

        actionRow.removeFromLeft(controls::buttonGap);

        saveSelectedButton.setBounds(
            actionRow.removeFromLeft(metrics::actionRow::saveWidth(narrow)));

        actionRow.removeFromLeft(controls::buttonGap);
        break;

    case StemLabAudioProcessor::hostIntegrationNone:
    default:
        saveSelectedButton.setBounds(
            actionRow.removeFromLeft(metrics::actionRow::saveWidth(narrow)));

        actionRow.removeFromLeft(controls::buttonGap);
        break;
    }

    const int locationWidth =
        juce::jmin(metrics::actionRow::locationWidth(narrow), actionRow.getWidth());

    openJobButton.setBounds(actionRow.removeFromLeft(juce::jmax(0, locationWidth)));
}

void StemLabAudioProcessorEditor::timerCallback()
{
    processor.refreshEngineProgressFromDisk();

    if (!processor.isStandaloneApp())
    {
        processor.refreshAbletonSourceClipFromDisk();
        processor.refreshAbletonBridgeStatusFromDisk();
    }

    refreshFromProcessor();

    for (auto& waveform : waveformComponents)
    {
        if (waveform != nullptr)
            waveform->repaint();
    }
}

void StemLabAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    refreshFromProcessor();
}

void StemLabAudioProcessorEditor::refreshFromProcessor()
{
    const auto capturing = processor.isCapturing();

    const auto recordingMode = processor.getStandaloneRecordingMode();

    const auto engineRunning = processor.isEngineRunning();

    const auto captureFile = processor.getCaptureFile();

    const auto captureExists = captureFile.existsAsFile();

    const auto jobDone = processor.hasSuccessfulJob();

    syncRecursiveRows();

    captureButton.setEnabled(!capturing && !engineRunning);

    stopButton.setEnabled(false);

    separateButton.setEnabled(!capturing && !processor.isAwaitingAbletonSourceClip() &&
                              !engineRunning && captureExists);

    openJobButton.setEnabled(!engineRunning);

    if (processor.isStandaloneApp())
    {
        juce::String captureText;

        if (capturing)
        {
            captureText = recordingMode == StemLabAudioProcessor::recordingSystem
                              ? "System recording - "
                              : "Input recording - ";

            captureText += formatSeconds(processor.getCapturedSeconds());
        }
        else if (captureExists)
        {
            captureText =
                captureFile.getFileName() + " - " + formatSeconds(processor.getCapturedSeconds());
        }
        else
        {
            captureText = "No file selected";
        }

        captureTimeLabel.setText(captureText, juce::dontSendNotification);

        captureButton.setEnabled(!capturing && !engineRunning);

        recordSystemButton.setEnabled(!engineRunning &&
                                      (recordingMode == StemLabAudioProcessor::recordingNone ||
                                       recordingMode == StemLabAudioProcessor::recordingSystem));

        recordInputButton.setEnabled(!engineRunning &&
                                     (recordingMode == StemLabAudioProcessor::recordingNone ||
                                      recordingMode == StemLabAudioProcessor::recordingInput));

        recordSystemButton.setButtonText(recordingMode == StemLabAudioProcessor::recordingSystem
                                             ? "Stop System"
                                             : "Record System");

        recordInputButton.setButtonText(
            recordingMode == StemLabAudioProcessor::recordingInput ? "Stop Input" : "Record Input");

        playButton.setEnabled(captureExists && !engineRunning && !capturing);

        const auto previewIndex = processor.getPreviewStemIndex();

        const auto previewPlaying = processor.isStandalonePlaying();

        playButton.setButtonText(previewPlaying && previewIndex == -1 ? "Pause" : "Play");

        for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        {
            const auto stemFile = jobDone ? processor.getCompletedStemFile(i) : juce::File{};

            stemButtons[static_cast<size_t>(i)].setToggleState(processor.isStemEnabled(i),
                                                               juce::dontSendNotification);

            stemButtons[static_cast<size_t>(i)].setEnabled(jobDone && !engineRunning && !capturing);

            auto& preview = stemPlayButtons[static_cast<size_t>(i)];

            preview.setEnabled(jobDone && !engineRunning && !capturing && stemFile.existsAsFile());

            preview.setButtonText(previewPlaying && previewIndex == i ? "Pause" : "Play");

            auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];

            recursiveButton.setVisible(rootSupportsAdaptiveSplit(i) || rootHasChildren(i));
            recursiveButton.setEnabled(rootSupportsAdaptiveSplit(i) && jobDone && !engineRunning &&
                                       !capturing && stemFile.existsAsFile());

            auto& expandButton = stemExpandButtons[static_cast<size_t>(i)];
            const bool hasChildren = rootHasChildren(i);
            expandButton.setVisible(hasChildren);
            expandButton.setEnabled(hasChildren);
            expandButton.setButtonText(hasChildren && rootExpanded[static_cast<size_t>(i)] ? "v"
                                                                                           : ">");

            if (auto* waveform = waveformComponents[static_cast<size_t>(i)].get())
            {
                waveform->setFile(stemFile);
                waveform->setEnabled(jobDone && !engineRunning && !capturing &&
                                     stemFile.existsAsFile());
            }
        }

        for (auto& row : recursiveRows)
            if (row != nullptr)
                row->refresh(engineRunning || capturing, processor.isStandalonePlaying());

        saveSelectedButton.setEnabled(jobDone && !engineRunning && !capturing);
    }
    else
    {
        juce::String captureText;

        if (processor.isAwaitingAbletonSourceClip())
        {
            captureText = "Reading Live clip...";
        }
        else if (recordingMode == StemLabAudioProcessor::recordingSystem && capturing)
        {
            captureText = "Recording PC - " + formatSeconds(processor.getCapturedSeconds());
        }
        else if (captureExists)
        {
            const auto label = processor.getInputSourceLabel();

            captureText = (label.isNotEmpty() ? label : captureFile.getFileName());

            const auto duration = processor.getCapturedSeconds();

            if (duration > 0.0)
            {
                captureText += " - " + formatSeconds(duration);
            }

            captureText += " - beat " + juce::String(processor.getCaptureStartPpq(), 3);
        }
        else
        {
            switch (processor.getHostIntegration())
            {
            case StemLabAudioProcessor::hostIntegrationAbletonLive:
                captureText = "Select a Live audio clip, then Use Live Clip";
                break;

            case StemLabAudioProcessor::hostIntegrationReaper:
                captureText = "Select an item in REAPER, then Use Selected Item";
                break;

            case StemLabAudioProcessor::hostIntegrationNone:
            default:
                captureText = "Drop an audio file here, or click Select File";
                break;
            }
        }

        captureTimeLabel.setText(captureText, juce::dontSendNotification);

        captureButton.setEnabled(!capturing && !engineRunning &&
                                 !processor.isAwaitingAbletonSourceClip());

        recordSystemButton.setEnabled(!engineRunning && !processor.isAwaitingAbletonSourceClip() &&
                                      (recordingMode == StemLabAudioProcessor::recordingNone ||
                                       recordingMode == StemLabAudioProcessor::recordingSystem));

        recordSystemButton.setButtonText(
            recordingMode == StemLabAudioProcessor::recordingSystem ? "Stop PC" : "Record PC");

        playButton.setEnabled(captureExists && !engineRunning && !capturing);

        const auto previewIndex = processor.getPreviewStemIndex();

        const auto previewPlaying = processor.isStandalonePlaying();

        playButton.setButtonText(previewPlaying && previewIndex == -1 ? "Pause" : "Play");

        int selectedCount = 0;

        for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        {
            const auto stemFile = jobDone ? processor.getCompletedStemFile(i) : juce::File{};

            if (processor.isStemEnabled(i))
                ++selectedCount;

            stemButtons[static_cast<size_t>(i)].setToggleState(processor.isStemEnabled(i),
                                                               juce::dontSendNotification);

            stemButtons[static_cast<size_t>(i)].setEnabled(jobDone && !engineRunning && !capturing);

            auto& preview = stemPlayButtons[static_cast<size_t>(i)];

            preview.setVisible(true);

            preview.setEnabled(jobDone && !engineRunning && !capturing && stemFile.existsAsFile());

            preview.setButtonText(previewPlaying && previewIndex == i ? "Pause" : "Play");

            auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];

            recursiveButton.setVisible(rootSupportsAdaptiveSplit(i) || rootHasChildren(i));
            recursiveButton.setEnabled(rootSupportsAdaptiveSplit(i) && jobDone && !engineRunning &&
                                       !capturing && stemFile.existsAsFile());

            auto& expandButton = stemExpandButtons[static_cast<size_t>(i)];
            const bool hasChildren = rootHasChildren(i);
            expandButton.setVisible(hasChildren);
            expandButton.setEnabled(hasChildren);
            expandButton.setButtonText(hasChildren && rootExpanded[static_cast<size_t>(i)] ? "v"
                                                                                           : ">");

            if (auto* waveform = waveformComponents[static_cast<size_t>(i)].get())
            {
                waveform->setFile(stemFile);

                waveform->setEnabled(jobDone && !engineRunning && !capturing &&
                                     stemFile.existsAsFile());
            }
        }

        for (const auto& item : processor.getRecursiveStemItems())
            if (item.selected)
                ++selectedCount;

        for (auto& row : recursiveRows)
            if (row != nullptr)
                row->refresh(engineRunning || capturing, processor.isStandalonePlaying());

        saveSelectedButton.setEnabled(jobDone && !engineRunning && !capturing);

        sendSelectedButton.setEnabled(jobDone && !engineRunning && !capturing && selectedCount > 0);

        retryImportButton.setEnabled(jobDone && !engineRunning);

        bridgeLabel.setText(processor.getAbletonBridgeStatus(), juce::dontSendNotification);
    }

    progressValue = processor.getEngineProgress();

    statusLabel.setText(processor.getStatus(), juce::dontSendNotification);

    if (engineRunning)
    {
        const auto elapsed = processor.getEngineElapsedSeconds();

        const auto eta = processor.getEngineEstimatedRemainingSeconds();

        timingLabel.setText("Elapsed " + formatSeconds(elapsed) + "   |   ETA " +
                                (eta >= 0.0 ? formatSeconds(eta) : "estimating..."),
                            juce::dontSendNotification);
    }
    else if (jobDone)
    {
        timingLabel.setText("Completed in " + formatSeconds(processor.getEngineElapsedSeconds()),
                            juce::dontSendNotification);
    }
    else
    {
        timingLabel.setText("Elapsed 00:00   |   ETA --:--", juce::dontSendNotification);
    }
}

void StemLabAudioProcessorEditor::chooseStandaloneAudioFile()
{
    if (!processor.usesLocalFileWorkflow() || processor.isCapturing())
    {
        return;
    }

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
    {
        start = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    }

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

    menu.addSectionHeader("Display");

    juce::PopupMenu waveformMenu;

    const juce::StringArray colourNames{
        "Spectrum (Volume)", "Violet", "Cyan", "Emerald", "Amber", "Pink", "Ice"};

    for (int i = 0; i < colourNames.size(); ++i)
    {
        waveformMenu.addItem(100 + i, colourNames[i], true,
                             processor.getWaveformColourIndex() == i);
    }

    menu.addSubMenu("Waveform Color", waveformMenu);

    menu.addSeparator();

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
        juce::PopupMenu::Options().withTargetComponent(&settingsButton),
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
                juce::SystemClipboard ::copyTextToClipboard(safeThis->processor.getEngineLog());

                safeThis->processor.postUiStatus("Diagnostics copied to clipboard");
            }
            else if (result == 5)
            {
                safeThis->launchAbletonSetup();
            }
            else if (result >= 100 && result < 100 + StemLabAudioProcessor ::waveformColourCount)
            {
                safeThis->processor.setWaveformColourIndex(result - 100);
            }
            else if (result >= 200 && result < 200 + StemLabAudioProcessor ::separatorEngineCount)
            {
                safeThis->processor.setSeparatorEngineIndex(result - 200);

                safeThis->processor.postUiStatus(
                    "Separator: " + safeThis->processor.getSeparatorEngineDisplayName());
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
    {
        start = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    }

    fileChooser =
        std::make_unique<juce::FileChooser>("Choose stemlab-plugin-job executable", start, "*.exe");

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
