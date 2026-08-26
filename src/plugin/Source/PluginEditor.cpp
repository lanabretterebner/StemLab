#include "PluginEditor.h"
#include "StemLabPaths.h"
#include "StemLabTheme.h"
#include "BinaryData.h"

#include <algorithm>

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
        // A source checkout keeps it under scripts/win/; the portable
        // payload's flat scripts/ layout predates that split and is what
        // installed copies already have.
        for (const auto& candidate :
             {root.getChildFile("scripts").getChildFile("win").getChildFile(
                  "install_ableton.ps1"),
              root.getChildFile("scripts").getChildFile("install_ableton.ps1")})
        {
            if (candidate.existsAsFile())
                return candidate;
        }

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
constexpr int versionItemId = 440;

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
                                   StemLabWaveformCache& waveformCacheIn)
    : processor(processorIn), waveformCache(waveformCacheIn)
{
    setMouseCursor(juce::MouseCursor::IBeamCursor);
    setInterceptsMouseClicks(true, false);
}

void StemLaneWaveform::setFile(const juce::File& file)
{
    if (file == currentFile)
        return;

    currentFile = file;
    profile.reset();
    columns.clear();
    columnsFile = juce::File();

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

void StemLaneWaveform::setSelectionId(const juce::String& id) { selectionId = id; }

double StemLaneWaveform::normalisedForX(float x) const
{
    namespace lanes = theme::metrics::lanes;

    const auto inner = getLocalBounds().toFloat().reduced(lanes::wellPadX, lanes::wellPadY);

    if (inner.getWidth() <= 0.0f || profile == nullptr || !(profile->lengthSeconds > 0.0))
        return 0.0;

    const auto across =
        juce::jlimit(0.0, 1.0, static_cast<double>(x - inner.getX()) /
                                   static_cast<double>(inner.getWidth()));

    const auto view = processor.getWaveformViewRange(profile->lengthSeconds);

    return juce::jlimit(0.0, 1.0,
                        (view.getStart() + across * view.getLength()) / profile->lengthSeconds);
}

void StemLaneWaveform::refreshColumns(juce::Rectangle<float> inner, double viewStart,
                                      double viewLength)
{
    const auto width = juce::jmax(0, static_cast<int>(inner.getWidth()));

    if (profile == nullptr || profile->peaks.isEmpty() || width <= 0)
    {
        columns.clear();
        return;
    }

    const auto channels = juce::jlimit(1, 2, profile->peaks.channels);

    // Nothing about the picture changed, so neither do the columns. This is
    // what keeps a still lane free and a scrolling one cheap.
    if (columnsFile == currentFile && columnsWidth == width && columnsChannels == channels &&
        std::abs(columnsStart - viewStart) < 1.0e-9 &&
        std::abs(columnsLength - viewLength) < 1.0e-9)
    {
        return;
    }

    columnsFile = currentFile;
    columnsWidth = width;
    columnsChannels = channels;
    columnsStart = viewStart;
    columnsLength = viewLength;

    columns.assign(static_cast<std::size_t>(width), {});

    const auto secondsPerColumn = viewLength / static_cast<double>(width);

    for (int x = 0; x < width; ++x)
    {
        const auto from = viewStart + secondsPerColumn * static_cast<double>(x);
        const auto to = from + secondsPerColumn;

        auto& column = columns[static_cast<std::size_t>(x)];

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto range =
                stemlab::waveform::peakBetween(profile->peaks, channel, from, to);

            column.minimum[channel] = range.minimum;
            column.maximum[channel] = range.maximum;
        }

        column.brightness =
            stemlab::waveform::brightnessAt(profile->spectrum, (from + to) * 0.5);

        column.bands = stemlab::waveform::bandsAt(profile->spectrum, (from + to) * 0.5);
    }
}

void StemLaneWaveform::paint(juce::Graphics& g)
{
    namespace lanes = theme::metrics::lanes;

    const auto full = getLocalBounds().toFloat();

    g.setColour(theme::colours::laneWell());
    g.fillRoundedRectangle(full, lanes::wellRadius);

    if (profile == nullptr && currentFile.existsAsFile())
        profile = waveformCache.get(currentFile);

    const auto inner = full.reduced(lanes::wellPadX, lanes::wellPadY);

    if (profile == nullptr || !(profile->lengthSeconds > 0.0) || inner.isEmpty())
        return;

    const auto length = profile->lengthSeconds;

    /*
     * Snap the window's start to a whole column of time.
     *
     * Without this a scrolling view re-buckets the audio every frame: each
     * column covers a slightly different span, its peak jumps to a
     * neighbouring sample, and the whole waveform crawls and shimmers even
     * though the audio is not moving. Snapped, the picture translates by
     * whole columns and every column keeps exactly the audio it had.
     */
    const auto view = processor.getWaveformViewRange(length);
    const auto viewLength = juce::jmax(1.0e-6, view.getLength());

    const auto secondsPerColumn = viewLength / static_cast<double>(inner.getWidth());

    const auto snappedStart =
        secondsPerColumn > 0.0
            ? std::floor(view.getStart() / secondsPerColumn) * secondsPerColumn
            : view.getStart();

    refreshColumns(inner, snappedStart, viewLength);

    const auto secondsToX = [&inner, snappedStart, viewLength](double seconds)
    {
        return inner.getX() +
               static_cast<float>((seconds - snappedStart) / viewLength) * inner.getWidth();
    };

    const int palette = processor.getWaveformColourIndex();

    const auto transportLength = processor.getTransportLengthSeconds();
    const auto transportPosition = processor.getTransportPositionSeconds();

    const double playNormalised =
        transportLength > 0.0 ? juce::jlimit(0.0, 1.0, transportPosition / transportLength)
                              : -1.0;

    // Beat grid behind the waveform: bars read stronger than beats, and the
    // whole thing stays subordinate to the audio it sits behind.
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

                const auto secondsPerBar = secondsPerBeat * beatsPerBar;

                /*
                 * Number every Nth bar, where N is the smallest power-of-two
                 * step whose labels do not collide. Zoomed out that lands on
                 * 4 or 8; zoomed in, every bar gets its number.
                 */
                int barLabelStep = 1;

                while (secondsPerBar * barLabelStep * pixelsPerSecond <
                           lanes::gridLabelMinSpacing &&
                       barLabelStep < 512)
                {
                    barLabelStep *= 2;
                }

                // Beats get their own bar.beat labels once they are far
                // enough apart to carry one.
                const bool labelBeats =
                    drawBeats && secondsPerBeat * pixelsPerSecond >= lanes::gridLabelMinSpacing;

                /*
                 * Start at the first beat in view rather than at bar one: at
                 * 64x on a long track that is thousands of iterations that
                 * would each be computed only to be discarded.
                 */
                int beatIndex = static_cast<int>(
                    std::floor((snappedStart - grid.barOne) / secondsPerBeat));

                g.setFont(theme::fonts::gridLabel());

                for (double t = grid.barOne + beatIndex * secondsPerBeat;
                     t <= snappedStart + viewLength && t < length;
                     t += secondsPerBeat, ++beatIndex)
                {
                    if (t < 0.0)
                        continue;

                    // beatIndex is negative before bar one, and % keeps that
                    // sign in C++; fold it back before testing for a bar.
                    const int withinBar =
                        ((beatIndex % beatsPerBar) + beatsPerBar) % beatsPerBar;

                    const bool bar = withinBar == 0;

                    if (!bar && !drawBeats)
                        continue;

                    const auto x = secondsToX(t);

                    if (x < inner.getX() || x > inner.getRight())
                        continue;

                    g.setColour(theme::colours::text().withAlpha(bar ? 0.22f : 0.10f));
                    g.fillRect(x, inner.getY(), bar ? 1.4f : 1.0f, inner.getHeight());

                    if (!bar && !labelBeats)
                        continue;

                    // Bar one is bar 1, not bar 0, and bars before it count
                    // backwards rather than wrapping to a huge number.
                    const int barNumber =
                        static_cast<int>(std::floor(static_cast<double>(beatIndex) /
                                                    static_cast<double>(beatsPerBar))) +
                        1;

                    if (bar && barNumber % barLabelStep != 0 && barLabelStep > 1)
                        continue;

                    const auto text = bar ? juce::String(barNumber)
                                          : juce::String(barNumber) + "." +
                                                juce::String(withinBar + 1);

                    const auto label = juce::Rectangle<float>(
                        x + 2.0f, inner.getY(), lanes::gridLabelWidth, lanes::gridLabelHeight);

                    if (label.getRight() > inner.getRight())
                        continue;

                    g.setColour(theme::colours::text().withAlpha(bar ? 0.34f : 0.20f));
                    g.drawText(text, label, juce::Justification::topLeft, false);
                }
            }
        }
    }

    if (columns.empty())
        return;

    const auto channels = juce::jlimit(1, 2, profile->peaks.channels);

    const float playheadX = secondsToX(juce::jmax(0.0, playNormalised) * length);

    /*
     * Stereo draws as two half-height waveforms rather than one summed
     * envelope: which side a part sits on is information, and summing hides
     * anything that cancels.
     */
    const auto channelHeight =
        channels > 1 ? (inner.getHeight() - lanes::channelGap) * 0.5f : inner.getHeight();

    for (int channel = 0; channel < channels; ++channel)
    {
        const auto top =
            inner.getY() + static_cast<float>(channel) * (channelHeight + lanes::channelGap);

        const auto centreY = top + channelHeight * 0.5f;
        const auto halfHeight = channelHeight * 0.5f;

        // The whole waveform draws in its full palette colour: position is
        // the playhead's job, and dimming everything ahead of it greyed
        // most of the picture out for most of every playback.
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            const auto& column = columns[i];
            const auto x = inner.getX() + static_cast<float>(i);

            const auto lowest = juce::jlimit(-1.0f, 1.0f, column.minimum[channel]);
            const auto highest = juce::jlimit(-1.0f, 1.0f, column.maximum[channel]);

            auto topY = centreY - highest * halfHeight;
            auto bottomY = centreY - lowest * halfHeight;

            // Silence still draws a hairline, so an empty stem reads as a
            // flat line rather than as a lane that failed to load.
            if (bottomY - topY < lanes::waveMinHeight)
            {
                const auto centre = (topY + bottomY) * 0.5f;
                topY = centre - lanes::waveMinHeight * 0.5f;
                bottomY = centre + lanes::waveMinHeight * 0.5f;
            }

            /*
             * 3-Band draws one bar per band, nested: the dominant band (a
             * share of 1) owns the column's full extent and the others
             * scale within it, drawn strongest-first so each remains
             * visible inside the last. A kick column reads as blue with a
             * thin core, a hi-hat column as white through and through.
             */
            if (!mutedAppearance && palette == theme::waveform::paletteThreeBand)
            {
                struct BandBar
                {
                    float share;
                    juce::Colour colour;
                };

                BandBar bars[3] = {{column.bands.low, theme::waveform::bandLowColour()},
                                   {column.bands.mid, theme::waveform::bandMidColour()},
                                   {column.bands.high, theme::waveform::bandHighColour()}};

                std::sort(std::begin(bars), std::end(bars),
                          [](const BandBar& a, const BandBar& b) { return a.share > b.share; });

                const auto centre = (topY + bottomY) * 0.5f;
                const auto halfExtent = (bottomY - topY) * 0.5f;

                for (const auto& bar : bars)
                {
                    // An inaudible band draws nothing: forcing a hairline
                    // here would etch a white core through every pure-bass
                    // bar. The silence hairline is the outer clamp's job.
                    if (bar.share <= 0.04f)
                        continue;

                    const auto half = halfExtent * bar.share;

                    g.setColour(bar.colour);
                    g.fillRect(x, centre - half, 1.0f, half * 2.0f);
                }

                continue;
            }

            juce::Colour colour;

            if (mutedAppearance)
                colour = theme::colours::waveMuted();
            else if (palette == theme::waveform::paletteRgb)
                colour = theme::waveform::rgbColour(column.bands.low, column.bands.mid,
                                                    column.bands.high);
            else
                colour = theme::waveform::playedColour(palette, stemIdentity, column.brightness);

            g.setColour(colour);
            g.fillRect(x, topY, 1.0f, bottomY - topY);
        }
    }

    // Selection / loop range, live while dragging and persistent after.
    {
        auto range = processor.getStemSelectionRange(selectionId);

        double from = 0.0, to = 0.0;
        bool show = false;

        if (selecting)
        {
            from = juce::jmin(selectionAnchor, selectionHead);
            to = juce::jmax(selectionAnchor, selectionHead);
            show = to - from > 0.0;
        }
        else if (range.active)
        {
            from = range.start;
            to = range.end;
            show = true;
        }

        if (show)
        {
            const auto left = secondsToX(from * length);
            const auto right = secondsToX(to * length);

            const auto clippedLeft = juce::jmax(left, inner.getX());
            const auto clippedRight = juce::jmin(right, inner.getRight());

            if (clippedRight > clippedLeft)
            {
                g.setColour(theme::colours::accent().withAlpha(0.16f));
                g.fillRect(clippedLeft, inner.getY(), clippedRight - clippedLeft,
                           inner.getHeight());

                g.setColour(theme::colours::accent().withAlpha(0.75f));

                if (left >= inner.getX() && left <= inner.getRight())
                    g.fillRect(left, inner.getY(), 1.0f, inner.getHeight());

                if (right >= inner.getX() && right <= inner.getRight())
                    g.fillRect(right - 1.0f, inner.getY(), 1.0f, inner.getHeight());
            }
        }
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

void StemLaneWaveform::mouseDown(const juce::MouseEvent& event)
{
    if (!isEnabled())
        return;

    // Both gestures start here: a drag sweeps out a loop range, a click that
    // never moves falls through to a seek on release.
    selecting = false;
    selectionAnchor = normalisedForX(event.position.x);
    selectionHead = selectionAnchor;
}

void StemLaneWaveform::mouseDrag(const juce::MouseEvent& event)
{
    /*
     * Dragging the well sweeps a selection, which is also the preview loop.
     * Exporting the stem moved to the lane's own drag handle: one gesture
     * cannot both scrub out a range and carry a file to another application.
     */
    if (!isEnabled() || profile == nullptr)
        return;

    if (!selecting &&
        event.getDistanceFromDragStart() < theme::metrics::waveform::clickVersusDragThreshold)
    {
        return;
    }

    selecting = true;
    selectionHead = normalisedForX(event.position.x);

    repaint();
}

void StemLaneWaveform::mouseUp(const juce::MouseEvent& event)
{
    // Plain Components still receive mouse events while disabled.
    if (!isEnabled())
        return;

    if (selecting)
    {
        selecting = false;

        const auto from = juce::jmin(selectionAnchor, selectionHead);
        const auto to = juce::jmax(selectionAnchor, selectionHead);

        // A sweep that collapses to nothing clears rather than storing an
        // empty loop the transport would sit inside forever.
        if (to - from >= 0.0005)
            processor.setStemSelectionRange(selectionId, from, to);
        else
            processor.clearStemSelectionRange(selectionId);

        repaint();
        return;
    }

    if (event.getDistanceFromDragStart() >= theme::metrics::waveform::clickVersusDragThreshold)
        return;

    processor.transportSeekNormalised(normalisedForX(event.mouseDownPosition.x));
    repaint();
}

void StemLaneWaveform::mouseDoubleClick(const juce::MouseEvent&)
{
    // The way back out of a loop, without having to sweep a zero-width drag.
    if (isEnabled())
    {
        processor.clearStemSelectionRange(selectionId);
        repaint();
    }
}

void StemLaneWaveform::mouseWheelMove(const juce::MouseEvent& event,
                                      const juce::MouseWheelDetails& wheel)
{
    // With nothing to zoom the wheel keeps its stock meaning and scrolls
    // the lane list.
    if (!isEnabled() || profile == nullptr || onZoomStep == nullptr ||
        wheel.deltaY == 0.0f)
    {
        Component::mouseWheelMove(event, wheel);
        return;
    }

    // A mouse notch is ~0.1, a trackpad tick far less; gather deltas until
    // they amount to a whole notch, so a notch steps one detent and a
    // trackpad swipe is not a leap to either end of the range.
    constexpr float notch = 0.1f;

    wheelAccumulator += wheel.deltaY;

    const auto steps = static_cast<int>(wheelAccumulator / notch);

    if (steps != 0)
    {
        wheelAccumulator -= static_cast<float>(steps) * notch;
        onZoomStep(steps);
    }
}

// ======================================================================== lane

StemLaneComponent::StemLaneComponent(StemLabAudioProcessor& processorIn, int stemIndexIn,
                                     juce::String childIdIn,
                                     StemLabWaveformCache& waveformCache,
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

    waveform = std::make_unique<StemLaneWaveform>(processor, waveformCache);

    if (!isChildLane())
    {
        waveform->setStemIdentity(StemLabAudioProcessor::getStemName(stemIndex));
        waveform->setSelectionId(StemLabAudioProcessor::getStemName(stemIndex));
    }
    else
    {
        waveform->setSelectionId(childId);
    }

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

    /*
     * The stem's own drag handle. Dragging the waveform used to carry the
     * file out, which left no gesture for sweeping a loop range - and the
     * two cannot share one, since a sweep and a drag-to-another-application
     * look identical until the pointer leaves the window.
     */
    dragButton = std::make_unique<widgets::IconButton>(
        "drag-out", [](juce::Rectangle<float> b) { return stemlab::icons::dragOut(b); },
        static_cast<float>(theme::metrics::lanes::layersIcon), true,
        theme::metrics::lanes::smRadius, false);

    dragButton->setTooltip("Drag this stem to a DAW or a folder");
    dragButton->setMouseCursor(juce::MouseCursor::DraggingHandCursor);

    // Drags on the button are handled by the lane, which owns the file.
    dragButton->addMouseListener(this, false);

    // A plain click is a dead end here, so say what the button is for
    // rather than doing nothing at all.
    dragButton->onClick = [this]
    {
        if (laneFile.existsAsFile())
            processor.postUiStatus("Drag this button onto a DAW track or a folder");
    };

    addAndMakeVisible(*dragButton);

    /*
     * A kebab, not a layers glyph: this menu stopped being about splitting
     * when MIDI conversion, audition and export moved into it. The old icon
     * promised one of its entries and hid the rest.
     */
    menuButton = std::make_unique<widgets::IconButton>(
        "lane-menu", [](juce::Rectangle<float> b) { return stemlab::icons::kebab(b); },
        static_cast<float>(theme::metrics::lanes::layersIcon), false,
        theme::metrics::lanes::smRadius, false);

    menuButton->setTooltip("Stem actions");

    menuButton->onClick = [this]
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

    addAndMakeVisible(*menuButton);
}

void StemLaneComponent::setChildState(bool laneHasChildren, bool expanded)
{
    hasChildren = laneHasChildren;

    twisty.setExpanded(expanded);
    twisty.setVisible(laneHasChildren);
}

void StemLaneComponent::setZoomStepHandler(std::function<void(int)> handler)
{
    if (waveform != nullptr)
        waveform->onZoomStep = std::move(handler);
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
        waveform->setSelectionId(info.id);
    }

    refresh();
}

void StemLaneComponent::mouseDrag(const juce::MouseEvent& event)
{
    /*
     * The drag handle's own drags reach the lane, because a juce::Button
     * does not forward them anywhere useful. Starting the external drag from
     * here also means the whole lane is a valid drag source once the gesture
     * has begun, rather than the pointer having to stay inside 22 pixels.
     */
    if (externalDragStarted || dragButton == nullptr || !dragButton->isEnabled())
        return;

    if (event.eventComponent != dragButton.get())
        return;

    if (!laneFile.existsAsFile())
        return;

    if (event.getDistanceFromDragStart() < theme::metrics::waveform::clickVersusDragThreshold)
        return;

    externalDragStarted = juce::DragAndDropContainer::performExternalDragDropOfFiles(
        juce::StringArray{laneFile.getFullPathName()}, false, this);
}

void StemLaneComponent::mouseUp(const juce::MouseEvent&) { externalDragStarted = false; }

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

    /*
     * Always offered on a ready lane, both kinds. It used to be hidden when
     * the stem had no adaptive split to offer - which also hid Convert to
     * MIDI, Audition and Save MIDI, leaving them unreachable on Bass. The
     * menu itself decides which entries it can show.
     */
    menuButton->setEnabled(ready);
    dragButton->setEnabled(ready && laneFile.existsAsFile());

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

    row.removeFromLeft(lanes::twistyGap);

    auto includeArea = row.removeFromLeft(lanes::includeColumn);
    include.setBounds(includeArea);

    row.removeFromLeft(lanes::columnGap);

    nameLabel.setBounds(row.removeFromLeft(
        lanes::nameColumn - (isChildLane() ? lanes::childIndent : 0)));

    row.removeFromLeft(lanes::columnGap);

    // The drag handle leads the waveform it carries, rather than sitting
    // across the lane among the controls that act on playback.
    dragButton->setBounds(row.removeFromLeft(lanes::smButton)
                              .withSizeKeepingCentre(lanes::smButton, lanes::smButton));

    row.removeFromLeft(lanes::dragGap);

    auto controls = row.removeFromRight(lanes::controlsColumn);

    // Controls sit vertically centred: S, M, layers.
    auto centred = controls.withSizeKeepingCentre(controls.getWidth(), lanes::smButton);

    soloButton.setBounds(centred.removeFromLeft(lanes::smButton));
    centred.removeFromLeft(lanes::smGap);

    muteButton.setBounds(centred.removeFromLeft(lanes::smButton));
    centred.removeFromLeft(lanes::smGap);

    menuButton->setBounds(centred.removeFromLeft(lanes::smButton));

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
     * readout to their left says where that left things.
     */
    selectAllButton.setComponentID("neutral");
    selectAllButton.setTooltip("Include every stem");
    selectAllButton.onClick = [this] { setAllLanesIncluded(true); };
    panelContent.addAndMakeVisible(selectAllButton);

    deselectAllButton.setComponentID("neutral");
    deselectAllButton.setTooltip("Exclude every stem");
    deselectAllButton.onClick = [this] { setAllLanesIncluded(false); };
    panelContent.addAndMakeVisible(deselectAllButton);

    // User-action feedback, doubling as the selection readout. It hugs the
    // Select all pill on its right, so fresh messages grow leftward into
    // the header's free middle instead of pushing anything around.
    userStatusLabel.setFont(theme::fonts::meta());
    userStatusLabel.setColour(juce::Label::textColourId, theme::colours::text50());
    userStatusLabel.setJustificationType(juce::Justification::centredRight);
    panelContent.addAndMakeVisible(userStatusLabel);

    {
        // Measure once: the pills and the title then hold still while the
        // readout between them changes.
        const juce::Font pillFont{theme::fonts::smallButton()};

        const auto textWidth = [](const juce::Font& font, const juce::String& text)
        { return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)); };

        selectAllWidth =
            textWidth(pillFont, selectAllButton.getButtonText()) +
            2 * theme::metrics::header::selectButtonPadX;

        deselectAllWidth =
            textWidth(pillFont, deselectAllButton.getButtonText()) +
            2 * theme::metrics::header::selectButtonPadX;

        // The title used to absorb all the slack; now the readout needs it,
        // so the title is bounded to its own text (plus the label's border).
        titleWidth = textWidth(juce::Font(theme::fonts::title())
                                   .withExtraKerningFactor(theme::fonts::titleKerning),
                               titleLabel.getText()) +
                     12;
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

    /*
     * In the Standalone this records the selected physical input. In a
     * generic VST host - no Ableton bridge, no REAPER API - the same slot
     * records the audio the host is already sending through the plugin
     * (upstream's host-audio capture): that host's equivalent of "use what
     * is on this track".
     */
    recordInputButton.onClick = [this]
    {
        if (!processor.isStandaloneApp())
        {
            if (processor.isHostAudioCapturing())
                processor.stopHostAudioCapture();
            else
                processor.startHostAudioCapture();
        }
        else if (processor.getStandaloneRecordingMode() == StemLabAudioProcessor::recordingInput)
            processor.stopStandaloneRecording();
        else
            processor.startStandaloneRecording();

        refreshFromProcessor();
    };

    panelContent.addAndMakeVisible(recordInputButton);

    recordInputButton.setVisible(
        processor.isStandaloneApp() ||
        processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationNone);

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
            processor, i, juce::String{}, waveformProfiles,
            [this] { refreshFromProcessor(); },
            [this](int stemIndex) { showRootLayersMenu(stemIndex); },
            [this](const juce::String& id) { showChildLayersMenu(id); },
            [this](int stemIndex, juce::String id) { toggleLaneExpanded(stemIndex, id); });

        rootLanes[static_cast<size_t>(i)]->setZoomStepHandler(
            [this](int delta) { stepWaveformZoom(delta); });

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

    progressLabel.setFont(theme::fonts::progress());
    progressLabel.setColour(juce::Label::textColourId, theme::colours::text45());
    progressLabel.setJustificationType(juce::Justification::centredLeft);
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
        if (processor.isStandaloneApp())
        {
            insertButton.setVisible(false);
            saveButton.setComponentID("primary");
        }
        else
        {
            /*
             * Generic VST host (upstream's genericVst UI mode): there is
             * no bridge to send through, so the primary action is a drag
             * source for every selected stem at once, honouring each
             * lane's selection range. Click gets the hint; dragging
             * exports, exactly like the per-lane handles.
             */
            insertButton.setButtonText("Drag Stems");
            insertButton.setTooltip("Drag the selected stems into the DAW");
            insertButton.addMouseListener(this, false);
            insertButton.onClick = [this]
            { processor.postUiStatus("Drag this button onto a DAW track or a folder"); };
        }
        break;
    }

    processor.addChangeListener(this);

    // A reopened editor must not re-trigger the switch-to-Stems that runs
    // when a job is first observed finishing: seed from processor state.
    sawSuccessfulJob = processor.hasSuccessfulJob();

    // Same for user-action feedback: a message posted before this editor
    // opened is old news, not something to replay in the header.
    lastActionStatusRevision = processor.getActionStatusRevision();

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
        // Files of our own in-flight outbound drag are not an invitation
        // to reload the source they were split from.
        if (!selfFileDragGuard.shouldIgnore(path) && isSupportedAudioFile(juce::File(path)))
            return true;
    }

    return false;
}

void StemLabAudioProcessorEditor::startSelectedStemsDrag()
{
    const auto files = processor.getSelectedStemFilesForDrag();

    if (files.isEmpty())
        return;

    selfFileDragGuard.begin(files);

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    const bool started = juce::DragAndDropContainer::performExternalDragDropOfFiles(
        files, false, &insertButton,
        [safeThis]
        {
            if (safeThis != nullptr)
                safeThis->selfFileDragGuard.clear();
        });

    if (!started)
    {
        selfFileDragGuard.clear();
        processor.postUiStatus("Could not start the stem drag");
    }
}

void StemLabAudioProcessorEditor::mouseDrag(const juce::MouseEvent& event)
{
    // The Drag Stems pill is a drag source the way the lane handles are:
    // the gesture starts on the button and the whole selected set rides it.
    if (footerDragStarted || !insertButton.isVisible() || !insertButton.isEnabled())
        return;

    if (event.eventComponent != &insertButton ||
        processor.getHostIntegration() != StemLabAudioProcessor::hostIntegrationNone)
        return;

    if (event.getDistanceFromDragStart() < theme::metrics::waveform::clickVersusDragThreshold)
        return;

    footerDragStarted = true;
    startSelectedStemsDrag();
}

void StemLabAudioProcessorEditor::mouseUp(const juce::MouseEvent&) { footerDragStarted = false; }

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
        if (selfFileDragGuard.shouldIgnore(path))
            continue;

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

void StemLabAudioProcessorEditor::fileDragEnter(const juce::StringArray& files, int, int)
{
    // No drop glow for a drag the editor would refuse - most importantly
    // our own outbound stem drag passing back over the window.
    dragActive = isInterestedInFileDrag(files);
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

    deselectAllButton.setBounds(
        headerRow.removeFromRight(deselectAllWidth)
            .withSizeKeepingCentre(deselectAllWidth, header::selectButtonHeight));

    headerRow.removeFromRight(header::selectButtonGap);

    selectAllButton.setBounds(
        headerRow.removeFromRight(selectAllWidth)
            .withSizeKeepingCentre(selectAllWidth, header::selectButtonHeight));

    headerRow.removeFromRight(header::selectCountGap);

    brandGlyphBounds = headerRow.removeFromLeft(header::glyphSize)
                           .withSizeKeepingCentre(header::glyphSize, header::glyphSize);

    headerRow.removeFromLeft(header::glyphGap);
    titleLabel.setBounds(headerRow.removeFromLeft(titleWidth));

    headerRow.removeFromLeft(header::glyphGap);

    // Everything left between the title and the pills is the readout's.
    // Kept one line tall: a message too long for the space must ellipsize,
    // not wrap across the header.
    userStatusLabel.setBounds(
        headerRow.withSizeKeepingCentre(headerRow.getWidth(), header::userStatusHeight));

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

        if (recordInputButton.isVisible())
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

        // Left block: status line above, progress row below. The rows
        // rearrange when a job starts or ends, so the placement lives in
        // layoutStatusArea(), which reruns on every status refresh.
        statusAreaBounds = row;
        layoutStatusArea();
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

void StemLabAudioProcessorEditor::layoutStatusArea()
{
    namespace footer = theme::metrics::footer;

    if (statusAreaBounds.isEmpty())
        return;

    auto area = statusAreaBounds;

    const bool progressVisible = progressBar.isVisible();

    // Everything anchors on the area's left edge: the spinner sits flush
    // with the progress bar below it and the text runs left from there.
    // With no progress row the status line centres vertically instead.
    auto statusLine =
        progressVisible
            ? area.removeFromTop(footer::statusLineHeight)
            : area.withSizeKeepingCentre(area.getWidth(), footer::statusLineHeight);

    statusIndicator.setBounds(statusLine.removeFromLeft(footer::statusLineHeight));
    statusLine.removeFromLeft(footer::statusTextGap);
    statusLabel.setBounds(statusLine);

    if (!progressVisible)
        return;

    area.removeFromTop(footer::statusLineGap);

    auto progressRow = area.removeFromTop(footer::progressRowHeight);

    progressBar.setBounds(progressRow.removeFromLeft(
        juce::jmin(footer::progressBarWidth,
                   progressRow.getWidth() - footer::progressLabelGap)));

    progressRow.removeFromLeft(footer::progressLabelGap);
    progressLabel.setBounds(progressRow);
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
                processor, -1, item.id, waveformProfiles,
                [this] { refreshFromProcessor(); },
                [this](int stemIndex) { showRootLayersMenu(stemIndex); },
                [this](const juce::String& id) { showChildLayersMenu(id); },
                [this](int stemIndex, juce::String id) { toggleLaneExpanded(stemIndex, id); });

            lane->setChildInfo(item);
            lane->setChildState(item.hasChildren, isLaneExpanded(-1, item.id));
            lane->setZoomStepHandler([this](int delta) { stepWaveformZoom(delta); });
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

    /*
     * Anchor to the button, not the lane. A lane runs the full width of the
     * panel, so targeting it put the menu against the lane's left edge -
     * across the far side of the window from the button that opened it.
     */
    auto* target = rootLanes[static_cast<size_t>(stemIndex)]->getMenuButton();

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

    // Same as the root menu: anchor to the button that opened it. With no
    // target at all this fell back to wherever the mouse happened to be.
    juce::Component* target = nullptr;

    for (const auto& lane : childLanes)
    {
        if (lane != nullptr && lane->getChildId() == itemId)
        {
            target = lane->getMenuButton();
            break;
        }
    }

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(target),
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
    const auto nowMs = juce::Time::getMillisecondCounter();

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

    // The header readout: a fresh user-action message holds it for a few
    // seconds, then the selection count takes back over. Work the plugin
    // is doing never appears here - that is the bottom status line's job.
    {
        const auto actionRevision = processor.getActionStatusRevision();

        if (actionRevision != lastActionStatusRevision)
        {
            lastActionStatusRevision = actionRevision;
            actionStatusShownMs = nowMs;
        }

        const auto actionText = processor.getActionStatus();

        const bool actionFresh =
            actionText.isNotEmpty() && nowMs - actionStatusShownMs < 4000;

        userStatusLabel.setText(actionFresh ? actionText
                                : lanesLive ? juce::String(includedLanes) + " of " +
                                                  juce::String(totalLanes) + " selected"
                                            : juce::String(),
                                juce::dontSendNotification);
    }

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

    // The input slot answers for the physical input in the Standalone and
    // for host-audio capture in a generic VST host.
    const bool hostRecording = recordingMode == StemLabAudioProcessor::recordingHost;

    if (processor.isStandaloneApp())
    {
        recordInputButton.setEnabled(!engineRunning &&
                                     (recordingMode == StemLabAudioProcessor::recordingNone ||
                                      inputRecording));

        recordInputButton.setButtonText(inputRecording ? "Stop In" : "Record In");
        recordInputButton.setRecordingActive(inputRecording && capturing);
    }
    else
    {
        recordInputButton.setEnabled(!engineRunning &&
                                     (recordingMode == StemLabAudioProcessor::recordingNone ||
                                      hostRecording));

        recordInputButton.setButtonText(hostRecording ? "Stop Capture" : "Capture Host");
        recordInputButton.setRecordingActive(hostRecording && capturing);
    }

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
        fileName = systemRecording  ? "Recording PC audio"
                   : hostRecording  ? "Capturing host audio"
                                    : "Recording input";
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
            fileMeta = processor.isStandaloneApp()
                           ? "Drop audio here, or click Select File"
                           : "Capture Host, drop audio here, or click Select File";
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

    // Status line, reserved for the work the plugin is doing: while
    // separating, stream the engine status; once a job is done, its final
    // words stand for a few seconds before the summary takes back over.
    // User-action feedback reports in the header readout instead.
    const auto rawStatus = processor.getStatus();

    if (rawStatus != lastRawStatus)
    {
        lastRawStatus = rawStatus;
        lastStatusChangeMs = nowMs;
    }

    /*
     * "Busy" is wider than the engine: source analysis (Beat This!
     * downloads and inference), MIDI conversion, captures, and cache
     * maintenance all narrate on this line too. The summary must not
     * shoulder past a quiet stretch of any of them, and the spinner must
     * not claim idle - or worse, done - while one is still working.
     */
    const bool busy = processor.isBackgroundWorkRunning();

    const bool showSummary = jobDone && !busy && nowMs - lastStatusChangeMs > 5000;

    auto statusText = showSummary ? jobSummaryLine() : rawStatus;

    // Animated dots make long silent phases (imports, first-run downloads,
    // big CPU chunks, model inference) visibly alive instead of frozen.
    // Wall-clock phase: the engine's elapsed clock stands still for the
    // non-engine work this also covers.
    if (busy)
    {
        statusText = statusText.trimCharactersAtEnd(".");

        const auto phase = 1 + static_cast<int>((nowMs / 500u) % 3u);

        statusText += juce::String::repeatedString(".", phase);
    }

    statusLabel.setText(statusText, juce::dontSendNotification);

    statusIndicator.setState(busy      ? widgets::StatusIndicator::State::running
                             : jobDone ? widgets::StatusIndicator::State::done
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

    // The status block rearranges when the progress row appears or goes,
    // so it follows every refresh.
    layoutStatusArea();

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

    // The version this binary was built as (project VERSION in CMakeLists,
    // stamped by JUCE). Informational, so never selectable.
    menu.addItem(versionItemId, "StemLab v" JucePlugin_VersionString, false);

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
                // No feedback post: the launch already reports the clearing
                // job on the work status line.
                safeThis->processor.clearAnalysisCache();
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
            "install_ableton.ps1 was not found beside the app or in the StemLab "
            "source tree (scripts/win/).",
            "OK",
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
            "Run install_ableton.ps1 from StemLab's scripts folder instead.", "OK",
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
