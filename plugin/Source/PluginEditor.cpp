#include "PluginEditor.h"
#include "BinaryData.h"
#include "WaveformGrid.h"

#include <algorithm>
#include <cmath>

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace
{
juce::Colour background() { return juce::Colour::fromRGB(14, 17, 22); }

juce::Colour panel() { return juce::Colour::fromRGB(22, 27, 34); }

juce::Colour accent() { return juce::Colour::fromRGB(113, 93, 255); }

juce::Colour textMuted() { return juce::Colour::fromRGB(145, 154, 168); }

juce::File stemLabSettingsDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("FI-STEM");
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

juce::Colour solidWaveformColour(int index)
{
    switch (index)
    {
    case 1:
        return juce::Colour::fromRGB(132, 102, 255);

    case 2:
        return juce::Colour::fromRGB(52, 210, 255);

    case 3:
        return juce::Colour::fromRGB(66, 225, 154);

    case 4:
        return juce::Colour::fromRGB(255, 179, 66);

    case 5:
        return juce::Colour::fromRGB(255, 91, 176);

    case 6:
        return juce::Colour::fromRGB(224, 234, 244);

    default:
        return juce::Colour::fromRGB(132, 102, 255);
    }
}

juce::Colour interpolateRamp(const juce::Colour& first, const juce::Colour& second, float amount)
{
    return first.interpolatedWith(second, juce::jlimit(0.0f, 1.0f, amount));
}

juce::Colour spectrumColourForLevel(float level)
{
    // Level is a perceptual 0..1 value derived from local dBFS.
    // Quiet material starts violet/blue, medium material moves through
    // cyan/green, and strong peaks reach yellow/orange.
    const auto value = juce::jlimit(0.0f, 1.0f, level);

    const juce::Colour violet = juce::Colour::fromRGB(119, 92, 255);

    const juce::Colour blue = juce::Colour::fromRGB(61, 124, 255);

    const juce::Colour cyan = juce::Colour::fromRGB(46, 220, 255);

    const juce::Colour green = juce::Colour::fromRGB(70, 231, 151);

    const juce::Colour yellow = juce::Colour::fromRGB(245, 235, 89);

    const juce::Colour orange = juce::Colour::fromRGB(255, 154, 66);

    if (value < 0.18f)
        return interpolateRamp(violet, blue, value / 0.18f);

    if (value < 0.38f)
        return interpolateRamp(blue, cyan, (value - 0.18f) / 0.20f);

    if (value < 0.62f)
        return interpolateRamp(cyan, green, (value - 0.38f) / 0.24f);

    if (value < 0.84f)
        return interpolateRamp(green, yellow, (value - 0.62f) / 0.22f);

    return interpolateRamp(yellow, orange, (value - 0.84f) / 0.16f);
}

juce::Colour waveformColourForLevel(int paletteIndex, float level)
{
    const auto value = juce::jlimit(0.0f, 1.0f, level);

    if (paletteIndex == 0)
        return spectrumColourForLevel(value).withAlpha(0.96f);

    // Solid palettes remain the selected hue, but still react to volume:
    // quieter sections are darker/desaturated and peaks become brighter.
    auto base = solidWaveformColour(paletteIndex);

    const auto muted = base.withSaturation(juce::jlimit(0.20f, 1.0f, base.getSaturation() * 0.58f))
                           .withMultipliedBrightness(0.50f);

    const auto hot = base.withSaturation(juce::jlimit(0.0f, 1.0f, base.getSaturation() * 1.10f))
                         .withMultipliedBrightness(1.18f);

    return muted.interpolatedWith(hot, value).withAlpha(0.94f);
}

float perceptualWaveformLevel(float peak)
{
    const auto safePeak = juce::jlimit(0.0f, 1.0f, peak);

    // dB mapping makes the colour changes useful across real musical
    // dynamics instead of bunching almost everything near "quiet".
    const auto decibels = juce::Decibels::gainToDecibels(safePeak, -54.0f);

    return juce::jlimit(0.0f, 1.0f, juce::jmap(decibels, -48.0f, 0.0f, 0.0f, 1.0f));
}

void startExternalMidiDrag(StemLabAudioProcessor& processor, const juce::String& id,
                           juce::Component* source)
{
    const auto info = processor.getMidiInfo(id);
    const auto file = info.dragFile.existsAsFile() ? info.dragFile : info.midiFile;
    if (!file.existsAsFile())
    {
        processor.postUiStatus("Convert this stem to MIDI first");
        return;
    }

    juce::StringArray files{file.getFullPathName()};
    if (!juce::DragAndDropContainer::performExternalDragDropOfFiles(files, false, source))
        processor.postUiStatus("Could not start the MIDI file drag");
}

void chooseMidiSaveAs(StemLabAudioProcessor& processor, const juce::String& id,
                      juce::Component* source)
{
    const auto midi = processor.getMidiInfo(id).midiFile;
    if (!midi.existsAsFile())
    {
        processor.postUiStatus("Convert this stem to MIDI first");
        return;
    }

    auto chooser = std::make_shared<juce::FileChooser>("Save MIDI As", midi, "*.mid");
    juce::Component::SafePointer<juce::Component> safeSource(source);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode |
                             juce::FileBrowserComponent::canSelectFiles |
                             juce::FileBrowserComponent::warnAboutOverwriting,
                         [chooser, safeSource, midi, &processor](const juce::FileChooser& dialog)
                         {
                             if (safeSource == nullptr)
                                 return;
                             auto destination = dialog.getResult();
                             if (destination == juce::File{})
                                 return;
                             destination = destination.withFileExtension("mid");
                             if (destination == midi || midi.copyFileTo(destination))
                                 processor.postUiStatus("MIDI saved: " +
                                                        destination.getFullPathName());
                             else
                                 processor.postUiStatus("Could not save the MIDI file");
                         });
}

} // namespace

StemWaveformComponent::StemWaveformComponent(StemLabAudioProcessor& processorIn, int stemIndexIn,
                                             juce::AudioFormatManager& formatManager,
                                             juce::AudioThumbnailCache& thumbnailCache)
    : processor(processorIn), stemIndex(stemIndexIn), thumbnail(512, formatManager, thumbnailCache)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, false);
}

StemWaveformComponent::StemWaveformComponent(StemLabAudioProcessor& processorIn,
                                             juce::String recursiveIdIn,
                                             juce::AudioFormatManager& formatManager,
                                             juce::AudioThumbnailCache& thumbnailCache)
    : processor(processorIn), stemIndex(-3), recursive(true), recursiveId(std::move(recursiveIdIn)),
      thumbnail(512, formatManager, thumbnailCache)
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
    viewStart = 0.0;
    viewEnd = 1.0;
    panning = false;

    if (currentFile.existsAsFile())
    {
        thumbnail.setSource(new juce::FileInputSource(currentFile));
    }

    repaint();
}

void StemWaveformComponent::setResizeCallback(std::function<void(int, bool)> callback)
{
    resizeCallback = std::move(callback);
}

juce::String StemWaveformComponent::selectionId() const
{
    return recursive ? recursiveId : StemLabAudioProcessor::getStemName(stemIndex);
}

double StemWaveformComponent::normalisedPositionForX(float x) const
{
    const auto local = juce::jlimit(
        0.0, 1.0, static_cast<double>(x) / static_cast<double>(juce::jmax(1, getWidth())));
    return juce::jlimit(0.0, 1.0, viewStart + local * (viewEnd - viewStart));
}

void StemWaveformComponent::paint(juce::Graphics& g)
{
    const auto full = getLocalBounds().toFloat();

    g.setColour(juce::Colour::fromRGB(12, 15, 20));
    g.fillRoundedRectangle(full, 6.0f);

    const auto bounds = getLocalBounds().reduced(4);

    if (bounds.isEmpty())
        return;

    const auto length = thumbnail.getTotalLength();

    if (length > 0.0)
    {
        const auto visibleStart = viewStart * length;
        const auto visibleEnd = viewEnd * length;
        const auto grid = processor.getWaveformGridInfo();
        stemlab::waveform::GridRequest request;
        request.visibleStart = visibleStart;
        request.visibleEnd = visibleEnd;
        request.pixelWidth = bounds.getWidth();
        request.bpm = grid.bpm;
        request.numerator = grid.numerator;
        request.denominator = grid.denominator;
        request.barOne = grid.barOne;
        request.useDetectedBeats = grid.mode == StemLabAudioProcessor::gridSource;
        request.beats = grid.beats;
        request.downbeats = grid.downbeats;

        float lastLabelRight = static_cast<float>(bounds.getX()) - 40.0f;
        for (const auto& line : stemlab::waveform::makeGridLines(request))
        {
            const bool bar = line.kind == stemlab::waveform::GridLineKind::bar;
            const bool subdivision =
                line.kind == stemlab::waveform::GridLineKind::subdivision;
            const auto x = static_cast<float>(bounds.getX()) +
                           static_cast<float>(stemlab::waveform::timeToPixel(
                               line.seconds, visibleStart, visibleEnd, bounds.getWidth()));
            g.setColour(juce::Colour::fromRGB(78, 91, 112)
                            .withAlpha(bar ? 0.65f : (subdivision ? 0.16f : 0.34f)));
            g.drawLine(x, static_cast<float>(bounds.getY()), x,
                       static_cast<float>(bounds.getBottom()), bar ? 1.25f : 1.0f);

            if (line.barNumber > 0 && x >= lastLabelRight + 5.0f && x < bounds.getRight() - 20.0f)
            {
                g.setColour(textMuted().withAlpha(0.72f));
                g.setFont(juce::FontOptions(9.0f));
                g.drawText(juce::String(line.barNumber),
                           juce::Rectangle<float>(x + 3.0f, static_cast<float>(bounds.getY()),
                                                  34.0f, 11.0f),
                           juce::Justification::centredLeft);
                lastLabelRight = x + 37.0f;
            }
        }
    }

    g.setColour(juce::Colour::fromRGB(56, 63, 74));
    g.drawHorizontalLine(bounds.getCentreY(), static_cast<float>(bounds.getX()),
                         static_cast<float>(bounds.getRight()));

    if (length > 0.0 && thumbnail.getNumChannels() > 0)
    {
        const int channelCount = juce::jlimit(1, 2, thumbnail.getNumChannels());

        // Two-pixel slices retain plenty of visual detail while keeping the
        // six simultaneous waveform previews cheap to repaint at 20 Hz.
        constexpr int sliceWidth = 2;

        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto channelTop = bounds.getY() + bounds.getHeight() * channel / channelCount;

            const auto channelBottom =
                bounds.getY() + bounds.getHeight() * (channel + 1) / channelCount;

            const auto channelHeight = juce::jmax(1, channelBottom - channelTop);

            const auto centreY =
                static_cast<float>(channelTop) + static_cast<float>(channelHeight) * 0.5f;

            const auto halfHeight = static_cast<float>(channelHeight) * 0.46f;

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

                const auto visibleSpan = viewEnd - viewStart;

                const auto startTime = (viewStart + normalisedStart * visibleSpan) * length;

                const auto endTime = juce::jmax(startTime + 0.000001,
                                                (viewStart + normalisedEnd * visibleSpan) * length);

                float minimum = 0.0f;
                float maximum = 0.0f;

                thumbnail.getApproximateMinMax(startTime, endTime, channel, minimum, maximum);

                const auto localPeak = juce::jmax(std::abs(minimum), std::abs(maximum));

                const auto level = perceptualWaveformLevel(localPeak);

                const auto colour =
                    waveformColourForLevel(processor.getWaveformColourIndex(), level);

                const auto yTop = centreY - juce::jlimit(0.0f, 1.0f, maximum) * halfHeight;

                const auto yBottom = centreY - juce::jlimit(-1.0f, 0.0f, minimum) * halfHeight;

                // Keep extremely quiet material visible without pretending it
                // is loud. This matches the dense, thin low-level trace style
                // used by meter-oriented waveform displays.
                const auto visibleTop = juce::jmin(yTop, centreY - 0.55f);

                const auto visibleBottom = juce::jmax(yBottom, centreY + 0.55f);

                g.setColour(colour);

                g.drawLine(static_cast<float>(x), visibleTop, static_cast<float>(x), visibleBottom,
                           1.45f);
            }
        }
    }
    else
    {
        g.setColour(textMuted().withAlpha(0.65f));
        g.setFont(juce::FontOptions(11.0f));

        g.drawText("waveform", bounds, juce::Justification::centred);
    }

    if (length > 0.0)
    {
        const auto selection = processor.getStemSelectionRange(selectionId());
        if (selection.active)
        {
            const auto span = juce::jmax(0.000001, viewEnd - viewStart);
            const auto leftNormalised = (selection.start - viewStart) / span;
            const auto rightNormalised = (selection.end - viewStart) / span;
            if (rightNormalised >= 0.0 && leftNormalised <= 1.0)
            {
                const auto x1 = static_cast<float>(bounds.getX()) +
                                static_cast<float>(juce::jlimit(0.0, 1.0, leftNormalised)) *
                                    static_cast<float>(bounds.getWidth());
                const auto x2 = static_cast<float>(bounds.getX()) +
                                static_cast<float>(juce::jlimit(0.0, 1.0, rightNormalised)) *
                                    static_cast<float>(bounds.getWidth());
                const juce::Rectangle<float> selectedArea(
                    juce::jmin(x1, x2), static_cast<float>(bounds.getY()),
                    juce::jmax(1.0f, std::abs(x2 - x1)), static_cast<float>(bounds.getHeight()));
                g.setColour(accent().withAlpha(0.20f));
                g.fillRect(selectedArea);
                g.setColour(accent().withAlpha(0.90f));
                g.drawLine(x1, static_cast<float>(bounds.getY()), x1,
                           static_cast<float>(bounds.getBottom()), 1.5f);
                g.drawLine(x2, static_cast<float>(bounds.getY()), x2,
                           static_cast<float>(bounds.getBottom()), 1.5f);

                const auto duration = (selection.end - selection.start) * length;
                if (selectedArea.getWidth() > 62.0f)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.90f));
                    g.setFont(juce::FontOptions(10.0f));
                    g.drawText(formatSeconds(duration), selectedArea.reduced(4, 2).toNearestInt(),
                               juce::Justification::topLeft);
                }
            }
        }
    }

    const auto midiId = recursive ? recursiveId : StemLabAudioProcessor::getStemName(stemIndex);
    const auto midi = processor.getMidiInfo(midiId);
    if (length > 0.0 && !midi.notes.empty())
    {
        const auto visibleStart = viewStart * length;
        const auto visibleEnd = viewEnd * length;
        const auto visibleLength = juce::jmax(0.000001, visibleEnd - visibleStart);
        int minimumPitch = 127;
        int maximumPitch = 0;
        for (const auto& note : midi.notes)
        {
            minimumPitch = juce::jmin(minimumPitch, note.pitch);
            maximumPitch = juce::jmax(maximumPitch, note.pitch);
        }
        const auto pitchSpan = juce::jmax(1, maximumPitch - minimumPitch + 1);
        const auto noteHeight = juce::jlimit(2.0f, 8.0f,
                                             static_cast<float>(bounds.getHeight()) /
                                                 static_cast<float>(pitchSpan + 2));

        for (const auto& note : midi.notes)
        {
            if (note.start > visibleEnd)
                break;
            if (note.end < visibleStart || note.start > visibleEnd)
                continue;
            const auto x1 = static_cast<float>(bounds.getX()) +
                            static_cast<float>((juce::jmax(note.start, visibleStart) - visibleStart) /
                                               visibleLength) *
                                static_cast<float>(bounds.getWidth());
            const auto x2 = static_cast<float>(bounds.getX()) +
                            static_cast<float>((juce::jmin(note.end, visibleEnd) - visibleStart) /
                                               visibleLength) *
                                static_cast<float>(bounds.getWidth());
            const auto pitchPosition = static_cast<float>(note.pitch - minimumPitch + 1) /
                                       static_cast<float>(pitchSpan + 1);
            const auto y = static_cast<float>(bounds.getBottom()) -
                           pitchPosition * static_cast<float>(bounds.getHeight());
            const juce::Rectangle<float> noteBounds(
                x1, y - noteHeight * 0.5f, juce::jmax(1.5f, x2 - x1), noteHeight);
            g.setColour(accent().withAlpha(0.34f));
            g.fillRoundedRectangle(noteBounds, 1.5f);
            g.setColour(juce::Colours::white.withAlpha(0.42f));
            g.drawRoundedRectangle(noteBounds, 1.5f, 0.7f);
        }
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
            const auto fullPosition = juce::jlimit(0.0, 1.0, previewPosition / previewLength);
            const auto normalised =
                (fullPosition - viewStart) / juce::jmax(0.000001, viewEnd - viewStart);

            if (normalised >= 0.0 && normalised <= 1.0)
            {
                const auto x =
                    static_cast<float>(bounds.getX()) +
                    static_cast<float>(normalised) * static_cast<float>(bounds.getWidth());

                g.setColour(juce::Colours::white.withAlpha(0.95f));
                g.drawLine(x, static_cast<float>(bounds.getY()), x,
                           static_cast<float>(bounds.getBottom()), 1.5f);
            }

            const auto timeText =
                formatSeconds(previewPosition) + " / " + formatSeconds(previewLength);

            auto badgeArea = bounds;
            auto badgeRow = badgeArea.removeFromTop(17);

            auto badge = badgeRow.removeFromRight(82);

            g.setColour(juce::Colour::fromRGB(9, 11, 16).withAlpha(0.78f));

            g.fillRoundedRectangle(badge.toFloat(), 4.0f);

            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.setFont(juce::FontOptions(10.5f));

            g.drawText(timeText, badge.reduced(4, 0), juce::Justification::centredRight);
        }
    }

    g.setColour(juce::Colour::fromRGB(54, 61, 73));

    g.drawRoundedRectangle(full.reduced(0.5f), 6.0f, 1.0f);

    g.setColour(textMuted().withAlpha(0.48f));
    g.drawHorizontalLine(getHeight() - 2, full.getX() + full.getWidth() * 0.43f,
                         full.getX() + full.getWidth() * 0.57f);
}

void StemWaveformComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isLeftButtonDown() && event.position.y >= getHeight() - 7 && resizeCallback)
    {
        if (event.getNumberOfClicks() > 1)
        {
            resizeCallback(stemlab::waveform::defaultLaneHeight, true);
            return;
        }
        resizing = true;
        resizeMouseY = static_cast<float>(event.getScreenPosition().getY());
        resizeStartHeight = processor.getWaveformLaneHeight(selectionId());
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        return;
    }

    if (!currentFile.existsAsFile() || getWidth() <= 0)
        return;

    if (event.mods.isRightButtonDown())
    {
        processor.clearStemSelectionRange(selectionId());
        processor.postUiStatus("Waveform selection cleared");
        repaint();
        return;
    }

    if (event.mods.isMiddleButtonDown() && viewEnd - viewStart < 0.999)
    {
        panning = true;
        panStart = viewStart;
        panMouseX = event.position.x;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (!event.mods.isLeftButtonDown())
        return;

    selecting = true;
    selectionMoved = false;
    selectionMouseX = event.position.x;
    selectionAnchor = normalisedPositionForX(event.position.x);
}

void StemWaveformComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (resizing)
    {
        const auto delta = static_cast<float>(event.getScreenPosition().getY()) - resizeMouseY;
        resizeCallback(stemlab::waveform::clampLaneHeight(
                           resizeStartHeight + juce::roundToInt(delta)),
                       false);
        return;
    }

    if (selecting)
    {
        if (std::abs(event.position.x - selectionMouseX) >= 3.0f)
            selectionMoved = true;
        if (selectionMoved)
        {
            processor.setStemSelectionRange(selectionId(), selectionAnchor,
                                            normalisedPositionForX(event.position.x));
            repaint();
        }
        return;
    }

    if (!panning || getWidth() <= 0)
        return;

    const auto span = viewEnd - viewStart;
    const auto delta =
        static_cast<double>(event.position.x - panMouseX) / static_cast<double>(getWidth());
    viewStart = juce::jlimit(0.0, 1.0 - span, panStart - delta * span);
    viewEnd = viewStart + span;
    repaint();
}

void StemWaveformComponent::mouseUp(const juce::MouseEvent& event)
{
    if (selecting)
    {
        if (!selectionMoved)
        {
            const auto normalised = normalisedPositionForX(event.position.x);
            if (recursive)
                processor.seekRecursiveStem(recursiveId, normalised);
            else
                processor.seekCompletedStem(stemIndex, normalised);
        }
        else
        {
            processor.postUiStatus("Selection ready - Play loops it; export saves only it");
        }
    }

    resizing = false;
    panning = false;
    selecting = false;
    selectionMoved = false;
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    repaint();
}

void StemWaveformComponent::mouseWheelMove(const juce::MouseEvent& event,
                                           const juce::MouseWheelDetails& wheel)
{
    if (!event.mods.isCtrlDown() || thumbnail.getTotalLength() <= 0.0 || getWidth() <= 0)
    {
        juce::Component::mouseWheelMove(event, wheel);
        return;
    }

    const auto oldSpan = viewEnd - viewStart;
    const auto zoomFactor = std::pow(1.25, -static_cast<double>(wheel.deltaY) * 10.0);
    auto newSpan = juce::jlimit(0.01, 1.0, oldSpan * zoomFactor);
    if (newSpan > 0.995)
        newSpan = 1.0;

    const auto mouseFraction = juce::jlimit(
        0.0, 1.0, static_cast<double>(event.position.x) / static_cast<double>(getWidth()));
    const auto anchor = viewStart + mouseFraction * oldSpan;
    viewStart = juce::jlimit(0.0, 1.0 - newSpan, anchor - mouseFraction * newSpan);
    viewEnd = viewStart + newSpan;
    repaint();
}

RecursiveStemRowComponent::RecursiveStemRowComponent(
    StemLabAudioProcessor& processorIn, const StemLabRecursiveStemInfo& info,
    juce::AudioFormatManager& formatManager, juce::AudioThumbnailCache& thumbnailCache,
    std::function<void(const juce::String&)> toggleExpanded,
    std::function<bool(const juce::String&)> isExpanded, std::function<void()> laneResized)
    : processor(processorIn), item(info), toggleExpandedCallback(std::move(toggleExpanded)),
      isExpandedCallback(std::move(isExpanded)), laneResizedCallback(std::move(laneResized))
{
    selectButton.onClick = [this]
    { processor.setRecursiveStemEnabled(item.id, selectButton.getToggleState()); };
    selectButton.onRightClick = [this] { processor.soloRecursiveStemForExport(item.id); };

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

    actionButton.setTooltip("Stem actions");
    actionButton.onClick = [this] { showActionMenu(); };

    waveform =
        std::make_unique<StemWaveformComponent>(processor, item.id, formatManager, thumbnailCache);
    waveform->setResizeCallback(
        [this](int height, bool)
        {
            processor.setWaveformLaneHeight(item.id, height);
            if (laneResizedCallback)
                laneResizedCallback();
        });

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
                            juce::String(juce::roundToInt(item.confidence * 100.0)) +
                            "% | Right-click: solo export; right-click again to restore");
    expandButton.setVisible(item.hasChildren);
    expandButton.setButtonText(
        item.hasChildren && isExpandedCallback && isExpandedCallback(item.id) ? "v" : ">");
    selectButton.setToggleState(item.selected, juce::dontSendNotification);

    actionButton.setVisible(item.file.existsAsFile());

    if (waveform != nullptr)
        waveform->setFile(item.file);

    resized();
}

void RecursiveStemRowComponent::refresh(bool engineRunning, bool previewPlaying)
{
    const bool ready = !engineRunning && item.file.existsAsFile();

    selectButton.setEnabled(ready);
    playButton.setEnabled(ready);
    actionButton.setEnabled(ready && !processor.isMidiConversionRunning());
    actionButton.setVisible(item.file.existsAsFile());
    expandButton.setVisible(item.hasChildren);
    expandButton.setEnabled(item.hasChildren);
    expandButton.setButtonText(
        item.hasChildren && isExpandedCallback && isExpandedCallback(item.id) ? "v" : ">");

    selectButton.setToggleState(processor.isRecursiveStemEnabled(item.id),
                                juce::dontSendNotification);

    const auto hasSelection = processor.getStemSelectionRange(item.id).active;
    playButton.setButtonText(previewPlaying && processor.getPreviewRecursiveId() == item.id
                                 ? "Pause"
                                 : (hasSelection ? "Loop" : "Play"));

    if (waveform != nullptr)
    {
        waveform->setFile(item.file);
        waveform->setEnabled(ready);
        waveform->repaint();
    }
}

void RecursiveStemRowComponent::resized()
{
    auto row = getLocalBounds();

    const int indent = juce::jlimit(12, 54, item.depth * 14);
    row.removeFromLeft(indent);

    if (item.hasChildren)
    {
        expandButton.setBounds(row.removeFromLeft(22).reduced(1, 3));
        row.removeFromLeft(2);
    }
    else
    {
        expandButton.setBounds(0, 0, 0, 0);
        row.removeFromLeft(24);
    }

    const int actionWidth = item.file.existsAsFile() ? 30 : 0;

    if (actionWidth > 0)
    {
        actionButton.setBounds(row.removeFromRight(actionWidth).reduced(1, 2));
        row.removeFromRight(3);
    }
    else
    {
        actionButton.setBounds(0, 0, 0, 0);
    }

    playButton.setBounds(row.removeFromRight(50).reduced(1, 2));
    row.removeFromRight(4);

    const int labelWidth = juce::jlimit(96, 160, getWidth() / 4);
    selectButton.setBounds(row.removeFromLeft(labelWidth).reduced(0, 1));
    row.removeFromLeft(4);

    if (waveform != nullptr)
        waveform->setBounds(row.reduced(0, 1));
}

void RecursiveStemRowComponent::showActionMenu()
{
    juce::PopupMenu menu;

    constexpr int deverbId = 1;
    constexpr int splitFurtherId = 2;
    constexpr int midiId = 3;
    constexpr int dragMidiId = 4;
    constexpr int sendMidiId = 5;
    constexpr int revealMidiId = 6;
    constexpr int auditionMidiId = 7;
    constexpr int saveMidiId = 8;

    bool hasAdaptiveActions = false;

    if (item.actions.contains("deverb"))
    {
        menu.addItem(deverbId, "De-Reverb");
        hasAdaptiveActions = true;
    }

    if (item.actions.contains("split"))
    {
        menu.addItem(splitFurtherId, "Adaptive Split Further");
        hasAdaptiveActions = true;
    }

    if (hasAdaptiveActions)
        menu.addSeparator();
    const bool hasMidi = processor.hasMidiInfo(item.id);
    menu.addItem(midiId, hasMidi ? "Reconvert MIDI" : "Convert to MIDI",
                 !processor.isMidiConversionRunning());
    if (hasMidi)
    {
        menu.addItem(auditionMidiId,
                     processor.isMidiAuditioning(item.id) ? "Stop MIDI Audition"
                                                          : "Audition MIDI");
        menu.addItem(dragMidiId, "Drag MIDI File");
        menu.addItem(saveMidiId, "Save MIDI As...");
        if (!processor.isStandaloneApp())
            menu.addItem(sendMidiId, "Create MIDI Clip in Ableton",
                         processor.isAbletonBridgeActive());
        menu.addItem(revealMidiId, "Show MIDI File");
    }

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
            else if (result == midiId)
                safeThis->processor.launchRecursiveMidiConversion(safeThis->item.id);
            else if (result == dragMidiId)
                startExternalMidiDrag(safeThis->processor, safeThis->item.id,
                                      &safeThis->actionButton);
            else if (result == auditionMidiId)
                safeThis->processor.auditionMidi(safeThis->item.id);
            else if (result == saveMidiId)
                chooseMidiSaveAs(safeThis->processor, safeThis->item.id,
                                 &safeThis->actionButton);
            else if (result == sendMidiId)
                safeThis->processor.sendMidiToAbleton(safeThis->item.id);
            else if (result == revealMidiId)
                safeThis->processor.getMidiInfo(safeThis->item.id).midiFile.revealToUser();
        });
}

StemLabAudioProcessorEditor::StemLabAudioProcessorEditor(StemLabAudioProcessor& processorIn)
    : AudioProcessorEditor(&processorIn), processor(processorIn)
{
    setSize(680, 680);
    setResizable(true, true);

    // The UI is intentionally fluid. At the minimum size the waveform rows
    // collapse to compact strips; extra vertical space is given directly to
    // the six waveform rows instead of becoming dead space.
    setResizeLimits(540, 540, 1400, 1200);

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
                    window->setName("FI-STEM");

                    const auto appIcon = juce::ImageFileFormat::loadFrom(
                        BinaryData::FIStemIcon_png, BinaryData::FIStemIcon_pngSize);

                    if (appIcon.isValid())
                        window->setIcon(appIcon);
                }
            });
    }

    titleLabel.setText("FI-STEM", juce::dontSendNotification);

    titleLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));

    addAndMakeVisible(titleLabel);

    subtitleLabel.setColour(juce::Label::textColourId, textMuted());

    subtitleLabel.setText(
        processor.isStandaloneApp()
            ? "Load or record audio, split it, audition stems, then save"
            : "Use a Live clip or record PC audio, split it, audition stems, then send",
        juce::dontSendNotification);

    addAndMakeVisible(subtitleLabel);

    settingsButton.onClick = [this] { showSettingsMenu(); };
    addAndMakeVisible(settingsButton);

    captureButton.setColour(juce::TextButton::buttonColourId, accent());

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
                                     juce::Colour::fromRGB(194, 66, 94));

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

        recordInputButton.setButtonText("Record Input");
        recordInputButton.setColour(juce::TextButton::buttonColourId,
                                    juce::Colour::fromRGB(87, 102, 126));

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
        captureButton.setButtonText("Use Live Clip");
        captureButton.onClick = [this]
        {
            processor.requestAbletonSourceClip();
            refreshFromProcessor();
        };

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
                                     juce::Colour::fromRGB(194, 66, 94));

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

        recordInputButton.setVisible(false);
    }

    addAndMakeVisible(captureButton);
    addAndMakeVisible(stopButton);

    captureTimeLabel.setJustificationType(juce::Justification::centredLeft);

    captureTimeLabel.setColour(juce::Label::textColourId, textMuted());
    captureTimeLabel.setMinimumHorizontalScale(0.65f);

    addAndMakeVisible(captureTimeLabel);

    refinementButton.setToggleState(processor.isRefinementEnabled(), juce::dontSendNotification);

    refinementButton.setTooltip("Runs after " + processor.getSeparatorEngineDisplayName() +
                                " separation");

    refinementButton.onClick = [this]
    { processor.setRefinementEnabled(refinementButton.getToggleState()); };

    addAndMakeVisible(refinementButton);

    beatThisButton.setToggleState(processor.isBeatThisEnabled(), juce::dontSendNotification);
    beatThisButton.setTooltip(
        "Optional key/BPM/beat analysis. Uses CUDA automatically when available.");
    beatThisButton.onClick = [this]
    {
        processor.setBeatThisEnabled(beatThisButton.getToggleState());
        refreshFromProcessor();
    };
    addAndMakeVisible(beatThisButton);

    separateButton.setColour(juce::TextButton::buttonColourId, accent());

    separateButton.setButtonText("Separate All Stems");

    separateButton.onClick = [this]
    {
        processor.launchSeparationAndExport();
        refreshFromProcessor();
    };

    addAndMakeVisible(separateButton);

    cancelButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(168, 54, 74));
    cancelButton.onClick = [this]
    {
        processor.cancelRunningJob();
        refreshFromProcessor();
    };
    cancelButton.setVisible(false);
    addAndMakeVisible(cancelButton);

    progressBar.setColour(juce::ProgressBar::foregroundColourId, accent());

    progressBar.setColour(juce::ProgressBar::backgroundColourId, juce::Colour::fromRGB(35, 42, 52));

    progressBar.setPercentageDisplay(true);
    addAndMakeVisible(progressBar);

    statusLabel.setFont(juce::FontOptions(14.0f, juce::Font::bold));

    addAndMakeVisible(statusLabel);

    timingLabel.setColour(juce::Label::textColourId, textMuted());

    addAndMakeVisible(timingLabel);

    stemsLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));

    stemsLabel.setText(
        "Drag waveform = select/loop/export range  |  Right-click checkbox = toggle solo export",
        juce::dontSendNotification);

    addAndMakeVisible(stemsLabel);

    const juce::StringArray gridNames{"Host", "Source", "Manual"};
    for (int i = 0; i < static_cast<int>(gridModeButtons.size()); ++i)
    {
        auto& button = gridModeButtons[static_cast<size_t>(i)];
        button.setButtonText(gridNames[i]);
        button.setClickingTogglesState(false);
        button.setConnectedEdges((i > 0 ? juce::Button::ConnectedOnLeft : 0) |
                                 (i + 1 < static_cast<int>(gridModeButtons.size())
                                      ? juce::Button::ConnectedOnRight
                                      : 0));
        button.onClick = [this, i]
        {
            processor.setWaveformGridMode(i);
            if (i == StemLabAudioProcessor::gridManual)
                showManualGridDialog();
            refreshFromProcessor();
        };
        addAndMakeVisible(button);
    }

    rootExpanded.fill(true);
    stemViewport.setViewedComponent(&stemTreeContent, false);
    stemViewport.setScrollBarsShown(true, false);
    stemViewport.setScrollBarThickness(10);
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
        button.onRightClick = [this, i]
        {
            processor.soloStemForExport(i);
            refreshFromProcessor();
        };
        button.setTooltip("Right-click: solo export; right-click again to restore previous selection");

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
        recursiveButton.setTooltip("Stem actions");
        recursiveButton.setVisible(rootSupportsAdaptiveSplit(i));
        recursiveButton.onClick = [this, i] { showRootRecursiveMenu(i); };

        stemTreeContent.addAndMakeVisible(expandButton);
        stemTreeContent.addAndMakeVisible(button);
        stemTreeContent.addAndMakeVisible(preview);
        stemTreeContent.addAndMakeVisible(recursiveButton);

        waveformComponents[static_cast<size_t>(i)] =
            std::make_unique<StemWaveformComponent>(processor, i, waveformFormats, waveformCache);
        waveformComponents[static_cast<size_t>(i)]->setResizeCallback(
            [this, name](int height, bool)
            {
                processor.setWaveformLaneHeight(name, height);
                resized();
            });

        stemTreeContent.addAndMakeVisible(*waveformComponents[static_cast<size_t>(i)]);
    }

    saveSelectedButton.setVisible(processor.isStandaloneApp());

    saveSelectedButton.onClick = [this] { chooseSaveFolder(); };

    addAndMakeVisible(saveSelectedButton);

    sendSelectedButton.setVisible(!processor.isStandaloneApp());

    sendSelectedButton.setColour(juce::TextButton::buttonColourId, accent());

    sendSelectedButton.onClick = [this]
    {
        processor.sendSelectedStemsToAbleton();
        refreshFromProcessor();
    };

    addAndMakeVisible(sendSelectedButton);

    retryImportButton.setVisible(!processor.isStandaloneApp());

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

    startTimerHz(20);
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
    processor.removeChangeListener(this);
    stopTimer();
}

bool StemLabAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (processor.isStandaloneApp())
        return false;

    const auto modifiers = key.getModifiers();
    if (modifiers.isCtrlDown() || modifiers.isAltDown() || modifiers.isCommandDown())
        return false;

    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;

    const auto character = key.getTextCharacter();
    if (character != 'v' && character != 'V')
        return false;

    return processor.toggleHostTransport();
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

        if (isSupportedAudioFile(file) &&
            (processor.isStandaloneApp()
                 ? processor.setStandaloneInputFile(file)
                 : processor.setInputAudioFile(
                       file,
                       processor.getCaptureStartPpq() >= 0.0 ? processor.getCaptureStartPpq() : 0.0,
                       file.getFileName())))
        {
            refreshFromProcessor();
            return;
        }
    }
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
    g.fillAll(background());

    auto area = getLocalBounds().toFloat().reduced(18.0f);

    auto panelArea = area.withTrimmedTop(78.0f);

    g.setColour(panel());
    g.fillRoundedRectangle(panelArea, 12.0f);

    g.setColour(dragActive ? accent() : juce::Colour::fromRGB(43, 50, 61));

    g.drawRoundedRectangle(panelArea, 12.0f, dragActive ? 2.5f : 1.0f);

    if (dragActive)
    {
        g.setColour(accent().withAlpha(0.08f));

        g.fillRoundedRectangle(panelArea, 12.0f);

        g.setColour(juce::Colours::white);

        g.setFont(juce::FontOptions(18.0f, juce::Font::bold));

        g.drawFittedText("Drop audio to load", getLocalBounds().reduced(60),
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

    if (supportsSplit || hasChildren)
        menu.addSeparator();

    const bool midiReady = processor.getCompletedStemFile(stemIndex).existsAsFile() &&
                           !processor.isEngineRunning() && !processor.isMidiConversionRunning();
    const bool hasMidi = processor.hasMidiInfo(stemName);
    menu.addItem(3, hasMidi ? "Reconvert MIDI" : "Convert to MIDI", midiReady);
    if (hasMidi)
    {
        menu.addItem(7, processor.isMidiAuditioning(stemName) ? "Stop MIDI Audition"
                                                              : "Audition MIDI");
        menu.addItem(4, "Drag MIDI File");
        menu.addItem(8, "Save MIDI As...");
        if (!processor.isStandaloneApp())
            menu.addItem(5, "Create MIDI Clip in Ableton", processor.isAbletonBridgeActive());
        menu.addItem(6, "Show MIDI File");
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
                            else if (result == 3)
                                safeThis->processor.launchStemMidiConversion(stemIndex);
                            else if (result == 4)
                                startExternalMidiDrag(
                                    safeThis->processor,
                                    StemLabAudioProcessor::getStemName(stemIndex),
                                    &safeThis->stemRecursiveButtons[static_cast<size_t>(stemIndex)]);
                            else if (result == 7)
                                safeThis->processor.auditionMidi(
                                    StemLabAudioProcessor::getStemName(stemIndex));
                            else if (result == 8)
                                chooseMidiSaveAs(
                                    safeThis->processor,
                                    StemLabAudioProcessor::getStemName(stemIndex),
                                    &safeThis->stemRecursiveButtons[static_cast<size_t>(stemIndex)]);
                            else if (result == 5)
                                safeThis->processor.sendMidiToAbleton(
                                    StemLabAudioProcessor::getStemName(stemIndex));
                            else if (result == 6)
                                safeThis->processor
                                    .getMidiInfo(StemLabAudioProcessor::getStemName(stemIndex))
                                    .midiFile.revealToUser();
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
                { return safeEditor != nullptr ? safeEditor->isRecursiveExpanded(id) : true; },
                [safeEditor]
                {
                    if (safeEditor != nullptr)
                        safeEditor->resized();
                });

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
    const int width = getWidth();

    const int height = getHeight();

    // Slightly smaller outside padding at compact sizes.
    const int outerPadding = width < 620 || height < 620 ? 12 : 18;

    auto area = getLocalBounds().reduced(outerPadding);

    const int headerHeight = height < 620 ? 46 : 56;

    auto header = area.removeFromTop(headerHeight);

    auto titleRow = header.removeFromTop(height < 620 ? 27 : 32);

    settingsButton.setBounds(titleRow.removeFromRight(width < 620 ? 74 : 82));

    titleRow.removeFromRight(6);
    titleLabel.setBounds(titleRow);
    subtitleLabel.setBounds(header);

    area.removeFromTop(height < 620 ? 4 : 8);

    const int panelInset = width < 620 ? 7 : 12;

    area.reduce(panelInset, height < 620 ? 5 : 8);

    const int compact = height < 620 ? 1 : 0;

    const int inputHeight = compact ? 30 : 34;

    auto inputRow = area.removeFromTop(inputHeight);

    const int useClipWidth =
        processor.isStandaloneApp() ? (width < 620 ? 92 : 102) : (width < 620 ? 98 : 108);

    captureButton.setBounds(inputRow.removeFromLeft(useClipWidth));

    inputRow.removeFromLeft(5);

    playButton.setBounds(inputRow.removeFromLeft(width < 620 ? 52 : 58));

    inputRow.removeFromLeft(5);

    if (!processor.isStandaloneApp())
    {
        recordSystemButton.setBounds(inputRow.removeFromLeft(width < 620 ? 82 : 92));

        inputRow.removeFromLeft(6);
    }

    captureTimeLabel.setBounds(inputRow);

    if (processor.isStandaloneApp())
    {
        area.removeFromTop(compact ? 2 : 4);

        auto recordingRow = area.removeFromTop(compact ? 28 : 32);

        recordSystemButton.setBounds(recordingRow.removeFromLeft(width < 620 ? 104 : 116));

        recordingRow.removeFromLeft(5);

        recordInputButton.setBounds(recordingRow.removeFromLeft(width < 620 ? 96 : 108));
    }

    area.removeFromTop(compact ? 3 : 5);

    auto optionRow = area.removeFromTop(compact ? 22 : 25);
    const int optionGap = 6;
    const int optionWidth = juce::jmax(0, (optionRow.getWidth() - optionGap) / 2);
    refinementButton.setBounds(optionRow.removeFromLeft(optionWidth));
    optionRow.removeFromLeft(optionGap);
    beatThisButton.setBounds(optionRow);

    area.removeFromTop(compact ? 3 : 5);

    auto processRow = area.removeFromTop(compact ? 31 : 36);
    if (cancelButton.isVisible())
    {
        cancelButton.setBounds(processRow.removeFromRight(width < 620 ? 92 : 108));
        processRow.removeFromRight(5);
    }
    else
    {
        cancelButton.setBounds(0, 0, 0, 0);
    }
    separateButton.setBounds(processRow);

    area.removeFromTop(compact ? 3 : 5);

    statusLabel.setBounds(area.removeFromTop(compact ? 18 : 20));

    progressBar.setBounds(area.removeFromTop(compact ? 15 : 18));

    timingLabel.setBounds(area.removeFromTop(compact ? 17 : 20));

    area.removeFromTop(compact ? 2 : 4);

    auto stemsRow = area.removeFromTop(compact ? 22 : 25);
    const int gridButtonWidth = width < 620 ? 48 : 58;
    for (int i = static_cast<int>(gridModeButtons.size()) - 1; i >= 0; --i)
        gridModeButtons[static_cast<size_t>(i)].setBounds(
            stemsRow.removeFromRight(gridButtonWidth).reduced(0, 1));
    stemsRow.removeFromRight(7);
    stemsLabel.setBounds(stemsRow);

    // Reserve the bottom action row first. Everything between the stem label
    // and that row becomes waveform space.
    const int bottomGap = compact ? 3 : 5;

    const int actionHeight = compact ? 30 : 34;

    auto actionRow = area.removeFromBottom(actionHeight);

    area.removeFromBottom(bottomGap);

    int requestedContentHeight = 0;
    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        requestedContentHeight +=
            processor.getWaveformLaneHeight(StemLabAudioProcessor::getStemName(i));
    for (const auto& row : recursiveRows)
        if (row != nullptr)
            requestedContentHeight += processor.getWaveformLaneHeight(row->getItemId());
    const int contentHeight = juce::jmax(area.getHeight(), requestedContentHeight);

    stemViewport.setBounds(area);
    const int contentWidth = juce::jmax(320, stemViewport.getWidth() - 12);
    stemTreeContent.setSize(contentWidth, contentHeight);
    auto treeArea = stemTreeContent.getLocalBounds().reduced(2, 0);

    const int checkboxWidth = width < 620 ? 88 : 116;
    const int playWidth = width < 620 ? 48 : 55;
    const int actionWidth = width < 620 ? 28 : 32;
    const int expandWidth = 24;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        const auto rootName = StemLabAudioProcessor::getStemName(i);
        const int rowHeight = processor.getWaveformLaneHeight(rootName);
        auto row = treeArea.removeFromTop(rowHeight);
        const int rowPad = rowHeight < 50 ? 2 : 4;

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
        row.removeFromLeft(4);

        auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];
        if (rootSupportsAdaptiveSplit(i) || hasChildren || processor.hasSuccessfulJob())
        {
            recursiveButton.setBounds(row.removeFromRight(actionWidth).reduced(0, rowPad));
            row.removeFromRight(3);
        }
        else
            recursiveButton.setBounds(0, 0, 0, 0);

        stemPlayButtons[static_cast<size_t>(i)].setBounds(
            row.removeFromRight(playWidth).reduced(0, rowPad));
        row.removeFromRight(5);

        if (auto* waveform = waveformComponents[static_cast<size_t>(i)].get())
            waveform->setBounds(row.reduced(0, juce::jmax(1, rowPad - 1)));

        for (auto& recursiveRow : recursiveRows)
        {
            if (recursiveRow != nullptr && recursiveRow->getRootStem().equalsIgnoreCase(rootName))
            {
                recursiveRow->setBounds(treeArea.removeFromTop(
                    processor.getWaveformLaneHeight(recursiveRow->getItemId())));
            }
        }
    }

    if (processor.isStandaloneApp())
    {
        saveSelectedButton.setBounds(actionRow.removeFromLeft(width < 620 ? 112 : 128));

        actionRow.removeFromLeft(5);
    }
    else
    {
        sendSelectedButton.setBounds(actionRow.removeFromLeft(width < 620 ? 108 : 126));

        actionRow.removeFromLeft(5);

        retryImportButton.setBounds(actionRow.removeFromLeft(width < 620 ? 60 : 70));

        actionRow.removeFromLeft(5);
    }

    const int locationWidth = juce::jmin(width < 620 ? 132 : 150, actionRow.getWidth());

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

    beatThisButton.setToggleState(processor.isBeatThisEnabled(), juce::dontSendNotification);
    beatThisButton.setEnabled(
        !capturing && !processor.isMidiConversionRunning() &&
        (!engineRunning || processor.isBeatThisEnabled()));

    syncRecursiveRows();

    for (int i = 0; i < static_cast<int>(gridModeButtons.size()); ++i)
        gridModeButtons[static_cast<size_t>(i)].setToggleState(
            processor.getWaveformGridMode() == i, juce::dontSendNotification);

    captureTimeLabel.setTooltip(processor.getSourceAnalysisDetails());

    captureButton.setEnabled(!capturing && !engineRunning);

    stopButton.setEnabled(false);

    separateButton.setEnabled(!capturing && !processor.isAwaitingAbletonSourceClip() &&
                              !engineRunning && !processor.isMidiConversionRunning() &&
                              captureExists);

    const bool cancelVisibilityChanged = cancelButton.isVisible() != engineRunning;
    cancelButton.setVisible(engineRunning);
    cancelButton.setEnabled(engineRunning && !processor.isCancelRequested());
    cancelButton.setButtonText(processor.isCancelRequested() ? "Cancelling..." : "Cancel");
    if (cancelVisibilityChanged)
        resized();

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

        if (captureExists && !capturing)
            captureText += " | " + processor.getSourceAnalysisText();

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

            preview.setButtonText(
                previewPlaying && previewIndex == i
                    ? "Pause"
                    : (processor.getStemSelectionRange(StemLabAudioProcessor::getStemName(i)).active
                           ? "Loop"
                           : "Play"));

            auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];

            recursiveButton.setVisible(rootSupportsAdaptiveSplit(i) || rootHasChildren(i) ||
                                       jobDone);
            recursiveButton.setEnabled(jobDone && !engineRunning && !capturing &&
                                       !processor.isMidiConversionRunning() &&
                                       stemFile.existsAsFile());

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
            captureText = "Select a Live audio clip, then Use Live Clip";
        }

        if (captureExists && !capturing && !processor.isAwaitingAbletonSourceClip())
            captureText += " | " + processor.getSourceAnalysisText();

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

            preview.setButtonText(
                previewPlaying && previewIndex == i
                    ? "Pause"
                    : (processor.getStemSelectionRange(StemLabAudioProcessor::getStemName(i)).active
                           ? "Loop"
                           : "Play"));

            auto& recursiveButton = stemRecursiveButtons[static_cast<size_t>(i)];

            recursiveButton.setVisible(rootSupportsAdaptiveSplit(i) || rootHasChildren(i) ||
                                       jobDone);
            recursiveButton.setEnabled(jobDone && !engineRunning && !capturing &&
                                       !processor.isMidiConversionRunning() &&
                                       stemFile.existsAsFile());

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
    if (!processor.isStandaloneApp() || processor.isCapturing())
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
                                          processor.setStandaloneInputFile(result);

                                          refreshFromProcessor();
                                      }
                                  });
}

void StemLabAudioProcessorEditor::chooseSaveFolder()
{
    if (!processor.isStandaloneApp() || !processor.hasSuccessfulJob())
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

    jobFolderChooser = std::make_unique<juce::FileChooser>("Choose FI-STEM file location", start);

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

    menu.addSectionHeader("Beat This! analysis");

    juce::PopupMenu analysisModeMenu;
    analysisModeMenu.addItem(300, "Accurate (final0)", !processor.isEngineRunning(),
                             processor.getSourceAnalysisMode() ==
                                 StemLabAudioProcessor::analysisAccurate);
    analysisModeMenu.addItem(301, "Fast (small0)", !processor.isEngineRunning(),
                             processor.getSourceAnalysisMode() ==
                                 StemLabAudioProcessor::analysisFast);
    menu.addSubMenu("Analysis Mode", analysisModeMenu);

    const auto tempoLabel = [](const juce::String& name, double bpm)
    {
        return bpm > 0.0 ? name + " (" + juce::String(bpm, 1) + " BPM)" : name;
    };
    juce::PopupMenu tempoMenu;
    tempoMenu.addItem(310, tempoLabel("Half-time", processor.getHalfTimeSourceBpm()), true,
                      processor.getTempoInterpretation() == StemLabAudioProcessor::tempoHalf);
    tempoMenu.addItem(311, tempoLabel("Detected", processor.getDetectedSourceBpm()), true,
                      processor.getTempoInterpretation() == StemLabAudioProcessor::tempoDetected);
    tempoMenu.addItem(312, tempoLabel("Double-time", processor.getDoubleTimeSourceBpm()), true,
                      processor.getTempoInterpretation() == StemLabAudioProcessor::tempoDouble);
    menu.addSubMenu("Tempo Interpretation", tempoMenu,
                    processor.getCaptureFile().existsAsFile());
    menu.addItem(320, "Analysis Details...", processor.getCaptureFile().existsAsFile());
    menu.addItem(321, "Correct Analysis...",
                 processor.getCaptureFile().existsAsFile() && !processor.isEngineRunning());
    menu.addItem(322, "Forget Local Correction",
                 processor.getCaptureFile().existsAsFile() && !processor.isEngineRunning());
    menu.addItem(323, "Clear Analysis Cache", !processor.isEngineRunning());

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

    menu.addSectionHeader("FI-STEM engine");

    menu.addItem(2, "Choose engine executable...");

    menu.addItem(3, "Auto-detect engine");

    menu.addSeparator();

    menu.addItem(4, "Copy diagnostics to clipboard", processor.getEngineLog().isNotEmpty());

    if (processor.isStandaloneApp())
    {
        menu.addSeparator();
        menu.addSectionHeader("Ableton Live");
        menu.addItem(5, "Install / Repair Ableton Integration...");
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
            else if (result == 300 || result == 301)
            {
                safeThis->processor.setSourceAnalysisMode(result - 300);
            }
            else if (result >= 310 && result <= 312)
            {
                safeThis->processor.setTempoInterpretation(result - 310);
            }
            else if (result == 320)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::InfoIcon, "Source Analysis",
                    safeThis->processor.getSourceAnalysisDetails(), "OK", safeThis.getComponent());
            }
            else if (result == 321)
            {
                safeThis->showAnalysisCorrectionDialog();
            }
            else if (result == 322)
            {
                safeThis->processor.forgetSourceCorrection();
            }
            else if (result == 323)
            {
                safeThis->processor.clearAnalysisCache();
            }

            safeThis->refreshFromProcessor();
        });
}

void StemLabAudioProcessorEditor::showManualGridDialog()
{
    const auto grid = processor.getWaveformGridInfo();
    auto window = std::make_shared<juce::AlertWindow>(
        "Manual Waveform Grid", "Set the grid used for waveform alignment and MIDI placement.",
        juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("bpm", juce::String(grid.bpm, 3), "BPM");
    window->addTextEditor("numerator", juce::String(grid.numerator), "Meter numerator");
    window->addTextEditor("denominator", juce::String(grid.denominator), "Meter denominator");
    window->addTextEditor("barOne", juce::String(grid.barOne, 4), "Bar one (seconds)");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis, window](int result)
            {
                if (result != 1 || safeThis == nullptr)
                    return;
                const auto bpm = window->getTextEditorContents("bpm").getDoubleValue();
                const auto numerator = window->getTextEditorContents("numerator").getIntValue();
                const auto denominator =
                    window->getTextEditorContents("denominator").getIntValue();
                const auto barOne = window->getTextEditorContents("barOne").getDoubleValue();
                if (bpm < 20.0 || bpm > 400.0 || numerator < 1 || denominator < 1 || barOne < 0.0)
                {
                    safeThis->processor.postUiStatus("Manual grid values are invalid");
                    return;
                }
                safeThis->processor.setManualGrid(bpm, numerator, denominator, barOne);
                safeThis->refreshFromProcessor();
            }),
        false);
}

void StemLabAudioProcessorEditor::showAnalysisCorrectionDialog()
{
    auto window = std::make_shared<juce::AlertWindow>(
        "Correct Source Analysis", "Corrections are stored locally for this exact source file.",
        juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("bpm", juce::String(processor.getSourceBpm(), 3), "BPM");
    window->addTextEditor("key", processor.getSourceKey(), "Key");
    window->addTextEditor("numerator", juce::String(processor.getSourceMeterNumerator()),
                          "Meter numerator");
    window->addTextEditor("denominator", juce::String(processor.getSourceMeterDenominator()),
                          "Meter denominator");
    window->addTextEditor("barOne", juce::String(processor.getSourceBarOne(), 4),
                          "Bar one (seconds)");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis, window](int result)
            {
                if (result != 1 || safeThis == nullptr)
                    return;
                const auto bpm = window->getTextEditorContents("bpm").getDoubleValue();
                const auto key = window->getTextEditorContents("key").trim();
                const auto numerator = window->getTextEditorContents("numerator").getIntValue();
                const auto denominator =
                    window->getTextEditorContents("denominator").getIntValue();
                const auto barOne = window->getTextEditorContents("barOne").getDoubleValue();
                if (bpm < 20.0 || bpm > 400.0 || numerator < 1 || denominator < 1 || barOne < 0.0)
                {
                    safeThis->processor.postUiStatus("Analysis correction values are invalid");
                    return;
                }
                safeThis->processor.saveSourceCorrection(bpm, key, numerator, denominator, barOne);
            }),
        false);
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
                       .withTitle("Welcome to FI-STEM")
                       .withMessage("FI-STEM is ready to use as a standalone app.\n\n"
                                    "If you use Ableton Live, FI-STEM can set up its VST3 and "
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
                                         "FI-STEM portable onboarding completed.\n");

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
            "scripts/install_ableton.ps1 was not found in the FI-STEM source tree.", "OK",
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
            "FI-STEM could not start the Ableton setup helper.", "OK", this);
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
            "Run scripts/install_ableton.ps1 from the FI-STEM source folder instead.", "OK",
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
