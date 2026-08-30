#include "PluginEditor.h"
#include "StemLabPaths.h"
#include "StemLabTheme.h"
#include "BinaryData.h"

#include <utility>

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
    // Written as !(>= 0) so a NaN - an unfinished duration, a division by a
    // zero sample rate - lands here rather than in the cast below, where it
    // would be undefined behaviour.
    if (!(seconds >= 0.0))
        return "--:--";

    // A 0.2 s file is a real file. Rounding to whole seconds printed it as
    // "00:00" in both the source strip and the transport, so a valid source
    // read as empty; anything under a second therefore carries a tenth. At
    // least one tenth, so a 40 ms file does not read as zero either - and
    // exactly zero stays "00:00", which is the one case that is honest.
    if (seconds > 0.0 && seconds < 1.0)
        return juce::String::formatted("00:00.%d",
                                       juce::jlimit(1, 9, static_cast<int>(seconds * 10.0)));

    // Floored, not rounded. Rounding ran the clock up to half a second ahead
    // of the audio, so a position could read past a length that had not been
    // reached yet. seconds is non-negative here, so the truncating cast IS
    // the floor and needs no <cmath>.
    const int total = static_cast<int>(seconds);

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
constexpr int modelManagerId = 441;
constexpr int fusedNormaliseId = 442;

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
    currentFileExists = file.existsAsFile();
    profileRequested = false;

    // Zero, not "now": the next tick should ask about the new file at once
    // rather than serving out the previous file's poll interval.
    lastProfilePollMs = 0;
    profile.reset();
    columns.clear();
    columnImage = juce::Image();
    columnsFile = juce::File();
    lastDisplayValid = false;

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

StemLaneWaveform::ViewGeometry StemLaneWaveform::viewGeometryFor(double viewStart,
                                                                 double viewLength) const
{
    namespace lanes = theme::metrics::lanes;

    ViewGeometry geometry;

    geometry.inner = getLocalBounds().toFloat().reduced(lanes::wellPadX, lanes::wellPadY);

    if (!(viewLength > 0.0) || geometry.inner.isEmpty())
        return geometry;

    geometry.viewLength = juce::jmax(1.0e-6, viewLength);

    const auto secondsPerColumn =
        geometry.viewLength / static_cast<double>(geometry.inner.getWidth());

    geometry.snappedStart = secondsPerColumn > 0.0
                                ? std::floor(viewStart / secondsPerColumn) * secondsPerColumn
                                : viewStart;

    return geometry;
}

StemLaneWaveform::DisplayState StemLaneWaveform::readDisplayState() const
{
    DisplayState state;

    state.profilePtr = profile.get();

    if (profile != nullptr && profile->lengthSeconds > 0.0)
    {
        const auto view = processor.getWaveformViewRange(profile->lengthSeconds);

        state.viewStart = view.getStart();
        state.viewLength = view.getLength();
    }

    state.transportPosition = processor.getTransportPositionSeconds();
    state.transportLength = processor.getTransportLengthSeconds();
    state.palette = processor.getWaveformColourIndex();

    // Scalars only: this runs per lane per tick, and the full grid info
    // takes the processor's state lock to copy beat vectors nothing here
    // reads.
    const auto grid = processor.getWaveformGridScalars();

    state.gridBpm = grid.bpm;
    state.gridBarOne = grid.barOne;
    state.gridNumerator = grid.numerator;

    const auto range = processor.getStemSelectionRange(selectionId);

    state.selectionActive = range.active;
    state.selectionStart = range.start;
    state.selectionEnd = range.end;

    return state;
}

void StemLaneWaveform::refreshColumns(juce::Rectangle<float> inner, double viewStart,
                                      double viewLength)
{
    const auto width = juce::jmax(0, static_cast<int>(inner.getWidth()));
    const auto height = juce::jmax(0, static_cast<int>(inner.getHeight()));

    if (profile == nullptr || profile->peaks.isEmpty() || width <= 0 || height <= 0)
    {
        columns.clear();
        columnImage = juce::Image();
        columnsFile = juce::File();
        return;
    }

    const auto channels = juce::jlimit(1, 2, profile->peaks.channels);
    const auto palette = processor.getWaveformColourIndex();

    // Everything that shapes or colours the pixels; a change in any of it
    // means neither the columns nor their rendering can be reused.
    const bool sameSetup = columnsFile == currentFile && columnsWidth == width &&
                           columnsHeight == height && columnsChannels == channels &&
                           columnsPalette == palette && columnsMuted == mutedAppearance &&
                           columnsIdentity == stemIdentity &&
                           std::abs(columnsLength - viewLength) < 1.0e-9;

    // Nothing about the picture changed, so neither do the columns. This is
    // what keeps a still lane free and a scrolling one cheap.
    if (sameSetup && std::abs(columnsStart - viewStart) < 1.0e-9)
        return;

    const auto secondsPerColumn = viewLength / static_cast<double>(width);

    const auto computeColumn = [this, viewStart, secondsPerColumn, channels](int x)
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
    };

    if (sameSetup && secondsPerColumn > 0.0)
    {
        /*
         * The view slid along the same file at the same zoom. Both starts
         * are snapped to whole columns, so the slide is a whole number of
         * columns and the picture translates: scroll it and rebuild only
         * what was exposed. This is the path a zoomed-in view takes on
         * every tick of playback, where the window follows the playhead.
         */
        const auto shift =
            static_cast<int>(std::llround((viewStart - columnsStart) / secondsPerColumn));

        const bool wholeColumns =
            std::abs(columnsStart + shift * secondsPerColumn - viewStart) <
            secondsPerColumn * 1.0e-6;

        if (wholeColumns && shift == 0)
            return;

        if (wholeColumns && std::abs(shift) < width && columnImage.isValid())
        {
            columnsStart = viewStart;

            if (shift > 0)
            {
                std::move(columns.begin() + shift, columns.end(), columns.begin());
                columnImage.moveImageSection(0, 0, shift, 0, width - shift, height);

                for (int x = width - shift; x < width; ++x)
                    computeColumn(x);

                renderColumnStrip(width - shift, shift);
            }
            else
            {
                std::move_backward(columns.begin(), columns.end() + shift, columns.end());
                columnImage.moveImageSection(-shift, 0, 0, 0, width + shift, height);

                for (int x = 0; x < -shift; ++x)
                    computeColumn(x);

                renderColumnStrip(0, -shift);
            }

            return;
        }

        // A seek jumped further than the window is wide: rebuild the lot.
    }

    columnsFile = currentFile;
    columnsWidth = width;
    columnsHeight = height;
    columnsChannels = channels;
    columnsPalette = palette;
    columnsMuted = mutedAppearance;
    columnsIdentity = stemIdentity;
    columnsStart = viewStart;
    columnsLength = viewLength;

    columns.assign(static_cast<std::size_t>(width), {});

    for (int x = 0; x < width; ++x)
        computeColumn(x);

    if (columnImage.getWidth() != width || columnImage.getHeight() != height)
        columnImage = juce::Image(juce::Image::ARGB, width, height, true);

    renderColumnStrip(0, width);
}

void StemLaneWaveform::renderColumnStrip(int first, int count)
{
    namespace lanes = theme::metrics::lanes;

    if (!columnImage.isValid() || count <= 0)
        return;

    // moveImageSection leaves the vacated pixels behind; start the strip
    // from transparent so stale columns cannot show around thin bars.
    columnImage.clear({first, 0, count, columnsHeight});

    juce::Graphics g(columnImage);

    /*
     * Stereo draws as two half-height waveforms rather than one summed
     * envelope: which side a part sits on is information, and summing hides
     * anything that cancels.
     */
    const auto channelHeight =
        columnsChannels > 1
            ? (static_cast<float>(columnsHeight) - lanes::channelGap) * 0.5f
            : static_cast<float>(columnsHeight);

    for (int channel = 0; channel < columnsChannels; ++channel)
    {
        const auto top = static_cast<float>(channel) * (channelHeight + lanes::channelGap);
        const auto centreY = top + channelHeight * 0.5f;
        const auto halfHeight = channelHeight * 0.5f;

        // The whole waveform draws in its full palette colour: position is
        // the playhead's job, and dimming everything ahead of it greyed
        // most of the picture out for most of every playback.
        for (int i = 0; i < count; ++i)
        {
            const auto& column = columns[static_cast<std::size_t>(first + i)];
            const auto x = static_cast<float>(first + i);

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
            if (!columnsMuted && columnsPalette == theme::waveform::paletteThreeBand)
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

            if (columnsMuted)
                colour = theme::colours::waveMuted();
            else if (columnsPalette == theme::waveform::paletteRgb)
                colour = theme::waveform::rgbColour(column.bands.low, column.bands.mid,
                                                    column.bands.high);
            else
                colour = theme::waveform::playedColour(columnsPalette, columnsIdentity,
                                                       column.brightness);

            g.setColour(colour);
            g.fillRect(x, topY, 1.0f, bottomY - topY);
        }
    }
}

void StemLaneWaveform::paint(juce::Graphics& g)
{
    namespace lanes = theme::metrics::lanes;

    const auto full = getLocalBounds().toFloat();

    g.setColour(theme::colours::laneWell());
    g.fillRoundedRectangle(full, lanes::wellRadius);

    // One ask from here per file; the timer's poll owns every ask after
    // that, so a paint arriving for any other reason costs no syscalls.
    if (profile == nullptr && !profileRequested)
        fetchProfile();

    if (profile == nullptr || !(profile->lengthSeconds > 0.0))
        return;

    // Everything below draws the captured state timerRefresh invalidated
    // for - never a fresher read; see the note on lastDisplay. The capture
    // here only covers the first paint and a profile that just landed.
    if (!lastDisplayValid || lastDisplay.profilePtr != profile.get())
    {
        lastDisplay = readDisplayState();
        lastDisplayValid = true;
    }

    // The snapping that keeps a scrolling view from re-bucketing the audio
    // every frame lives in viewGeometryFor, which timerRefresh shares.
    const auto geometry = viewGeometryFor(lastDisplay.viewStart, lastDisplay.viewLength);
    const auto inner = geometry.inner;

    if (!(geometry.viewLength > 0.0) || inner.isEmpty())
        return;

    const auto length = profile->lengthSeconds;
    const auto viewLength = geometry.viewLength;
    const auto snappedStart = geometry.snappedStart;

    refreshColumns(inner, snappedStart, viewLength);

    const auto secondsToX = [&inner, snappedStart, viewLength](double seconds)
    {
        return inner.getX() +
               static_cast<float>((seconds - snappedStart) / viewLength) * inner.getWidth();
    };

    const auto transportLength = lastDisplay.transportLength;
    const auto transportPosition = lastDisplay.transportPosition;

    const double playNormalised =
        transportLength > 0.0 ? juce::jlimit(0.0, 1.0, transportPosition / transportLength)
                              : -1.0;

    // Beat grid behind the waveform: bars read stronger than beats, and the
    // whole thing stays subordinate to the audio it sits behind. The rules
    // draw here; the numbers are only gathered, and go on after the audio.
    gridLabels.clear();

    {
        const auto gridBpm = lastDisplay.gridBpm;
        const auto gridBarOne = lastDisplay.gridBarOne;

        if (gridBpm > 0.0)
        {
            const auto secondsPerBeat = 60.0 / gridBpm;
            const auto beatsPerBar = juce::jmax(1, lastDisplay.gridNumerator);

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
                    std::floor((snappedStart - gridBarOne) / secondsPerBeat));

                for (double t = gridBarOne + beatIndex * secondsPerBeat;
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

                    gridLabels.push_back({label, text, bar});
                }
            }
        }
    }

    const bool haveColumns = !columns.empty() && columnImage.isValid();

    // The columns land in one blit; the per-column drawing itself lives in
    // renderColumnStrip, which only runs when the picture changes. Images
    // draw at the current colour's opacity, and the grid rules left a
    // mostly-transparent one behind.
    if (haveColumns)
    {
        g.setOpacity(1.0f);
        g.drawImageAt(columnImage, static_cast<int>(inner.getX()),
                      static_cast<int>(inner.getY()));
    }

    /*
     * The numbers go on over the audio, not under it. Drawn with the rules
     * they were simply painted out: a near-full-scale passage fills the
     * well top to bottom and the blit took the ruler with it, so the labels
     * lost most of their ink whatever alpha they were given.
     *
     * Each number carries a small plate of the well's own ground colour, so
     * it reads against the surface its contrast was chosen against - 5.15:1
     * for a bar, 3.11:1 for a beat - instead of against whatever the audio
     * happens to be doing underneath. On a quiet lane the plate is the
     * colour that was already there and nothing shows; on a loud one it is
     * what keeps the number off the waveform. The rules stay behind the
     * audio, where rhythm belongs.
     */
    if (!gridLabels.empty())
    {
        const juce::Font labelFont {theme::fonts::gridLabel()};

        g.setFont(labelFont);

        for (const auto& item : gridLabels)
        {
            const auto ink = juce::GlyphArrangement::getStringWidth(labelFont, item.text);

            // The plate starts exactly where drawText starts laying glyphs
            // and only pads its trailing edge: a plate that reached back
            // over the bar rule would notch the rule for the label's height,
            // and the rules are meant to be continuous.
            const auto plate = item.bounds.withWidth(ink + lanes::gridLabelPlatePadding)
                                   .getIntersection(inner);

            if (!plate.isEmpty())
            {
                g.setColour(
                    theme::colours::laneWell().withAlpha(lanes::gridLabelPlateAlpha));
                g.fillRoundedRectangle(plate, lanes::gridLabelPlateRadius);
            }

            g.setColour(theme::colours::text().withAlpha(item.bar ? 0.55f : 0.38f));
            g.drawText(item.text, item.bounds, juce::Justification::topLeft, false);
        }
    }

    if (!haveColumns)
        return;

    const float playheadX = secondsToX(juce::jmax(0.0, playNormalised) * length);

    // Selection / loop range, live while dragging and persistent after.
    {
        double from = 0.0, to = 0.0;
        bool show = false;

        if (selecting)
        {
            from = juce::jmin(selectionAnchor, selectionHead);
            to = juce::jmax(selectionAnchor, selectionHead);
            show = to - from > 0.0;
        }
        else if (lastDisplay.selectionActive)
        {
            from = lastDisplay.selectionStart;
            to = lastDisplay.selectionEnd;
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

bool StemLaneWaveform::fetchProfile()
{
    profileRequested = true;
    lastProfilePollMs = juce::Time::getMillisecondCounter();

    // A stem can be announced before its file is written, so an existence
    // that was false is the only one worth re-testing.
    if (!currentFileExists)
    {
        if (!currentFile.existsAsFile())
            return false;

        currentFileExists = true;
    }

    profile = waveformCache.get(currentFile);
    return profile != nullptr;
}

bool StemLaneWaveform::timerRefresh()
{
    if (profile == nullptr)
    {
        // Still waiting on the analysis thread. Asking the cache is what
        // both queues the file and picks the answer up, and the repaint
        // is only worth scheduling once there is something to draw.
        if (juce::Time::getMillisecondCounter() - lastProfilePollMs <
            static_cast<juce::uint32>(profilePollIntervalMs))
        {
            return false;
        }

        /*
         * Only an ARRIVAL is reported, never the waiting. A lane whose file
         * does not exist keeps polling forever (StemLabWaveformCache::get
         * answers nullptr for it), and profile == nullptr is trivially true
         * for all six lanes with nothing loaded - so "still waiting" as a
         * signal would pin the editor at full rate for the life of the
         * process. What lands here is bounded by construction.
         */
        if (!fetchProfile())
            return false;

        repaint();
        return true;
    }

    const auto now = readDisplayState();

    // Exact comparisons on purpose: these are change detectors on values
    // re-read from one source, and the worst a stray bit costs is a repaint.
    const bool samePicture =
        lastDisplayValid && now.profilePtr == lastDisplay.profilePtr &&
        juce::exactlyEqual(now.viewStart, lastDisplay.viewStart) &&
        juce::exactlyEqual(now.viewLength, lastDisplay.viewLength) &&
        juce::exactlyEqual(now.transportLength, lastDisplay.transportLength) &&
        now.palette == lastDisplay.palette &&
        juce::exactlyEqual(now.gridBpm, lastDisplay.gridBpm) &&
        juce::exactlyEqual(now.gridBarOne, lastDisplay.gridBarOne) &&
        now.gridNumerator == lastDisplay.gridNumerator &&
        now.selectionActive == lastDisplay.selectionActive &&
        juce::exactlyEqual(now.selectionStart, lastDisplay.selectionStart) &&
        juce::exactlyEqual(now.selectionEnd, lastDisplay.selectionEnd);

    const bool playheadMoved =
        !juce::exactlyEqual(now.transportPosition, lastDisplay.transportPosition);

    const auto previous = lastDisplay;

    lastDisplay = now;
    lastDisplayValid = true;

    if (samePicture && !playheadMoved)
        return false;

    if (!samePicture)
    {
        repaint();
        return true;
    }

    /*
     * Only the playhead moved. The zoomed view follows the playhead, so
     * this is the zoom-1 and clamped-at-either-end case, where the columns
     * hold still: repaint the strip the playhead left and the strip it
     * entered instead of the whole well.
     */
    const auto geometry = viewGeometryFor(now.viewStart, now.viewLength);

    if (!(geometry.viewLength > 0.0) || !(now.transportLength > 0.0))
    {
        repaint();
        return true;
    }

    const auto xFor = [this, &geometry, &now](double transportPosition)
    {
        const auto normalised =
            juce::jlimit(0.0, 1.0, transportPosition / now.transportLength);

        return geometry.inner.getX() +
               static_cast<float>(
                   (normalised * profile->lengthSeconds - geometry.snappedStart) /
                   geometry.viewLength) *
                   geometry.inner.getWidth();
    };

    // Half the glow plus a pixel of slack on either side of each strip.
    const auto margin = theme::metrics::lanes::playheadGlowWidth * 0.5f + 1.0f;

    for (const auto x : {xFor(previous.transportPosition), xFor(now.transportPosition)})
        repaint(juce::Rectangle<float>(x - margin, 0.0f, margin * 2.0f,
                                       static_cast<float>(getHeight()))
                    .getSmallestIntegerContainer());

    return true;
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

        // Through timerRefresh rather than a plain repaint, so the captured
        // state paint draws picks the change up immediately.
        timerRefresh();

        // Storing or clearing a range pulls the shared playhead into it
        // (rebuildLoopRegions), which is a seek every other lane has to
        // hear about.
        if (onTransportSeek)
            onTransportSeek();

        return;
    }

    if (event.getDistanceFromDragStart() >= theme::metrics::waveform::clickVersusDragThreshold)
        return;

    processor.transportSeekNormalised(normalisedForX(event.mouseDownPosition.x));
    timerRefresh();

    if (onTransportSeek)
        onTransportSeek();
}

void StemLaneWaveform::mouseDoubleClick(const juce::MouseEvent&)
{
    // The way back out of a loop, without having to sweep a zero-width drag.
    if (isEnabled())
    {
        processor.clearStemSelectionRange(selectionId);
        timerRefresh();

        if (onTransportSeek)
            onTransportSeek();
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

    /*
     * A physical wheel arrives as one discrete event per notch, and the
     * per-notch delta is a platform constant nothing like the 0.1 the
     * accumulator below assumes: X11 sends 50/256 = 0.195
     * (juce_XWindowSystem_linux.cpp), Windows 60/256 = 0.234. Accumulating
     * those stepped just under two detents per notch, which left 2x, 4x,
     * 8x, 16x and 32x unreachable by wheel, and carried the leftover per
     * lane so the step size depended on which lane the pointer was over.
     * JUCE already says which kind of device this is: a notch is one
     * detent, and the accumulator is only for a trackpad's fine deltas.
     */
    if (!wheel.isSmooth)
    {
        wheelAccumulator = 0.0f;
        onZoomStep(wheel.deltaY > 0.0f ? 1 : -1);
        return;
    }

    // The trackpad path: a fine tick is far less than a detent, so gather
    // deltas until they amount to a whole one and a swipe is not a leap to
    // either end of the range.
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

    /*
     * The row's hover highlight covers the whole lane, but almost none of
     * the lane is the lane: the well fills the row's full height and the
     * buttons take the rest, so moving between two lanes is a crossing
     * between two CHILDREN and neither parent is an event target.
     * setRepaintsOnMouseActivity only ever fires for the target
     * (juce_Component.cpp internalMouseEnter/Exit), which left the old row
     * lit and the new one dark. Listening deeply makes every crossing into
     * or out of a child count as a hover change for the row.
     *
     * The price: every mouse override on this class now fires for every
     * descendant's events, and twice for the lane's own. Keep them all
     * idempotent.
     */
    addMouseListener(this, true);

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
    /*
     * A child lane's name carries the category/confidence tooltip that
     * setChildInfo attaches, and juce::TooltipWindow only ever asks the
     * component under the mouse - a label the mouse passes straight
     * through is never that component, so the tooltip could not appear at
     * any dwell time. A root name has no tooltip and stays transparent.
     * Children of the label stay transparent either way: there are none,
     * and the label must not start swallowing anything else.
     */
    nameLabel.setInterceptsMouseClicks(isChildLane(), false);

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

    // Drags on the button are handled by the lane, which owns the file;
    // the lane's deep mouse listener above already delivers them. Adding a
    // second, direct registration here would run mouseDrag twice per event.

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

void StemLaneComponent::setChildState(bool laneHasChildren, bool expanded,
                                      bool hiddenActivity, bool hiddenSolo)
{
    hasChildren = laneHasChildren;

    twisty.setExpanded(expanded);
    twisty.setVisible(laneHasChildren);

    // Called for every lane on every 20 Hz tick, so only an actual change
    // may repaint: an unconditional one here is a permanent row-repaint
    // storm that drags each waveform well's image blit along with it.
    if (hiddenDescendantActive != hiddenActivity || hiddenDescendantSoloed != hiddenSolo)
    {
        hiddenDescendantActive = hiddenActivity;
        hiddenDescendantSoloed = hiddenSolo;
        repaint();
    }
}

void StemLaneComponent::setZoomStepHandler(std::function<void(int)> handler)
{
    if (waveform != nullptr)
        waveform->onZoomStep = std::move(handler);
}

void StemLaneComponent::setTransportSeekHandler(std::function<void()> handler)
{
    if (waveform != nullptr)
        waveform->onTransportSeek = std::move(handler);
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

    // A highlighted range means this drag carries that range, not the whole
    // stem - the same rule the footer's Drag Stems pill already follows.
    const auto selectionId =
        isChildLane() ? childId : StemLabAudioProcessor::getStemName(stemIndex);
    const auto dragFile = processor.getStemDragFile(laneFile, selectionId);

    if (!dragFile.existsAsFile())
        return;

    externalDragStarted = juce::DragAndDropContainer::performExternalDragDropOfFiles(
        juce::StringArray{dragFile.getFullPathName()}, false, this);
}

void StemLaneComponent::mouseUp(const juce::MouseEvent&)
{
    externalDragStarted = false;
}

void StemLaneComponent::mouseEnter(const juce::MouseEvent&) { updateHover(); }
void StemLaneComponent::mouseExit(const juce::MouseEvent&) { updateHover(); }

void StemLaneComponent::mouseWheelMove(const juce::MouseEvent& event,
                                       const juce::MouseWheelDetails& wheel)
{
    /*
     * Listening deeply for the row's hover highlight (see the constructor)
     * has JUCE replay every descendant's mouse event here as well - the
     * wheel included, through MouseListenerList::sendMouseEvent. The
     * descendant's own chain has already decided what the wheel meant: the
     * waveform well zooms and consumes it, and anything else forwards it to
     * the viewport. Component::mouseWheelMove then forwarded the replay to
     * the viewport a second time, so a wheel over the well zoomed AND
     * scrolled the lane list, and a wheel anywhere else on the lane scrolled
     * it twice over.
     *
     * Only the lane's own events carry on up. The replays are already spoken
     * for, and eventComponent still names the component the wheel actually
     * landed on (HierarchyChecker keeps it there while that component is
     * alive), so the two are told apart by asking.
     *
     * It took a deep enough stem tree to make the list scrollable before any
     * of this was visible - with everything on screen the extra scroll had
     * nowhere to go.
     */
    if (event.eventComponent != this)
        return;

    Component::mouseWheelMove(event, wheel);
}

void StemLaneComponent::updateHover()
{
    /*
     * Asked, not assumed. A mouseExit is dispatched after JUCE has already
     * assigned the new component under the mouse
     * (juce_MouseInputSourceImpl.h setComponentUnderMouse), so this reads
     * the post-move truth - which is what keeps a crossing between two
     * children of the SAME row from blanking a row that is still hovered.
     */
    const bool now = isMouseOver(true) && isEnabled();

    if (now != hovered)
    {
        hovered = now;
        repaint();
    }
}

void StemLaneComponent::refresh()
{
    const bool capturing = processor.isCapturing();
    const bool engineRunning = processor.isEngineRunning();
    const bool jobDone = processor.hasSuccessfulJob();

    if (!isChildLane())
    {
        /*
         * Mid-job the ready record is the only source: getCompletedStemFile
         * caches its directory scan on (job, completion), so asking it before
         * the job finishes would pin one partial scan for the rest of the run
         * - and it switches to refined/ the moment that folder exists, which
         * is before anything has been written into it.
         */
        laneFile = jobDone ? processor.getCompletedStemFile(stemIndex)
                           : (engineRunning ? processor.getReadyStemFile(stemIndex) : juce::File{});
    }

    // One existence answer per refresh - this runs for every lane on every
    // UI tick. Root lanes take the processor's scan-cache answer instead
    // of a stat; a stem the engine has announced was on disk when it said
    // so, which is the same standing that answer has. Child lanes stat
    // their own file once.
    const bool laneFileReady = isChildLane() ? laneFile.existsAsFile()
                               : jobDone     ? processor.hasCompletedStemFile(stemIndex)
                                             : laneFile != juce::File();

    /*
     * An announced stem is file-ready long before the job is. Only the
     * picture goes live that early: everything these two gate - solo, mute,
     * the lane menu, the drag handle, and through it the lane's own
     * mouseDrag - still waits for the whole job, because the job can still
     * be cancelled or fail after announcing this stem, and because the stem
     * mix behind the transport is built once, from a finished job.
     */
    const bool ready = jobDone && !engineRunning && !capturing && laneFileReady;
    const bool laneLive = jobDone && !engineRunning && !capturing;

    if (waveform != nullptr)
    {
        // A disabled well still paints; setEnabled only withholds the mouse.
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

    // ready already carries this refresh's existence answer.
    dragButton->setEnabled(ready);

    /*
     * Dimmed means "you are not hearing this", not "this lane's own M is
     * down". Soloing one lane silences the other five and muting an
     * ancestor silences everything below it; the processor answers from
     * the mix that is actually playing, so a lane cannot look audible
     * while the mixer has its gain at zero.
     */
    if (waveform != nullptr)
    {
        const bool audible = isChildLane() ? processor.isRecursiveStemAudible(childId)
                                           : processor.isStemAudible(stemIndex);

        waveform->setMutedAppearance(!audible);
    }

    // An excluded lane drops to 45% opacity per the spec; only meaningful
    // once stems exist.
    setAlpha(jobDone && !included ? theme::metrics::lanes::excludedOpacity : 1.0f);

    // The lanes are rebuilt and re-laid-out under a stationary pointer
    // (a split finishing, a twisty collapsing), and a component that did
    // not exist when the pointer arrived never gets an enter.
    updateHover();
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

    // A row laid out under a stationary pointer gets no enter for the
    // children that just moved beneath it.
    updateHover();
}

void StemLaneComponent::paint(juce::Graphics& g)
{
    if (hovered)
    {
        g.setColour(theme::colours::rowHoverFill());
        g.fillRoundedRectangle(getLocalBounds().toFloat(),
                               theme::metrics::lanes::rowRadius);
    }

    /*
     * A collapsed row is still steering the mix through the rows it is
     * hiding. Mark it, in the gap beside the twisty that hid them, so
     * a solo cannot go on shaping what you hear with nothing on screen
     * saying so. Accent for a hidden solo, neutral for a hidden mute:
     * the same pairing the S and M buttons use.
     */
    if (hiddenDescendantActive)
    {
        namespace lanes = theme::metrics::lanes;

        const auto size = lanes::hiddenActivityDot;

        g.setColour(hiddenDescendantSoloed ? theme::colours::accent()
                                           : theme::colours::text75());

        g.fillEllipse(static_cast<float>(twisty.getRight()) + 1.0f,
                      static_cast<float>(getHeight()) * 0.5f - size * 0.5f,
                      size, size);
    }
}

// ====================================================================== editor

StemLabAudioProcessorEditor::StemLabAudioProcessorEditor(StemLabAudioProcessor& processorIn)
    : AudioProcessorEditor(&processorIn), processor(processorIn),
      waveformProfiles(processorIn.getWaveformCache())
{
    namespace window = theme::metrics::window;

    setLookAndFeel(&lookAndFeel.get());

    // Clicking anywhere in the interface focuses the editor, so Esc can
    // reach keyPressed and clear the loop ranges.
    setWantsKeyboardFocus(true);

    /*
     * The interface scales instead of reflowing: one design-size layout in
     * panelContent, warped by a single transform. Nothing pins the aspect
     * ratio: resized() fits the whole panel inside whatever shape the window
     * is given and centres it, so the window is free to take any shape a host
     * or a window manager hands it. The limits below keep the panel between
     * legible and absurd.
     */
    panelContent.onPaint = [this](juce::Graphics& g) { paintPanel(g); };

    // paintPanel starts from a full fillAll, so promising opacity here stops
    // every child repaint from also invalidating whatever sits behind it.
    panelContent.setOpaque(true);

    addAndMakeVisible(panelContent);

    panelContent.addChildComponent(modelManagerPanel);

    modelManagerPanel.onClose = [this] { closeModelManager(); };

    modelManagerPanel.onCompileEnabled = [this](bool enabled)
    { processor.setTorchCompileEnabled(enabled); };

    modelManagerPanel.onCancel = [this] { processor.cancelModelJob(); };

    /*
     * Each of these marks the status line worth showing from here on.
     * Watching isModelJobRunning instead would miss the short ones outright:
     * clearing a cache, or a compile refused because compiling is off,
     * begins and ends between two refreshes, and its outcome would land only
     * on the footer behind the scrim where it cannot be read.
     */
    modelManagerPanel.onDownload = [this](juce::StringArray ids)
    {
        modelJobReported = true;
        processor.startModelDownload(ids);
    };

    modelManagerPanel.onCompile = [this](juce::StringArray ids)
    {
        modelJobReported = true;
        processor.startModelCompile(ids);
    };

    modelManagerPanel.onRemove = [this](juce::StringArray modelIds, juce::StringArray cacheIds)
    {
        modelJobReported = true;
        processor.startModelRemoval(modelIds, cacheIds);
    };

    /*
     * The first inventory read. It is a child process, so the answer lands
     * later on the message thread and the auto-show decision is made from
     * refreshFromProcessor once there is something to decide on.
     */
    processor.refreshModelInventory();

    setResizable(true, true);

    setResizeLimits(juce::roundToInt(window::width * window::minScale),
                    juce::roundToInt(window::height * window::minScale),
                    juce::roundToInt(window::width * window::maxScale),
                    juce::roundToInt(window::height * window::maxScale));

    if (processor.isStandaloneApp())
    {
        /*
         * setLookAndFeel above only reaches this editor's own subtree.
         * Everything JUCE builds for the standalone app itself - the
         * Audio/MIDI settings dialog, alert windows, its own file chooser -
         * is created outside that tree and reads the process default instead,
         * which is why the settings dialog came up in JUCE's slate grey.
         * Publishing the same instance as the default is what reaches them.
         *
         * Gated on the wrapper type, not compiled out: the shared code is
         * built once and linked into every format, so in a host this would be
         * hijacking the host's own default - and leaving a dangling one behind
         * when the plugin is unloaded. The destructor takes it back down.
         */
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel.get());

        auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

        juce::MessageManager::callAsync(
            [safeThis]
            {
                if (safeThis == nullptr)
                    return;

                if (auto* windowComponent =
                        safeThis->findParentComponentOfClass<juce::DocumentWindow>())
                {
                    /*
                     * The limits above sit on the editor's constrainer. The
                     * standalone window has its own, and that is the one the
                     * X11 peer reads to publish the window manager's size
                     * hints: it forwards checkBounds to ours but not the
                     * numbers, so without this the window manager is told the
                     * window may be any size at all and a drag can take it
                     * below anything legible. The numbers are a few pixels
                     * generous because the window's border - title bar and
                     * notification strip - is inside them, which only makes
                     * the advertised minimum safer than the real one.
                     * Set before the title bar swap: that recreates the peer,
                     * which is what republishes the hints.
                     */
                    if (auto* windowConstrainer = windowComponent->getConstrainer())
                    {
                        windowConstrainer->setSizeLimits(
                            juce::roundToInt(window::width * window::minScale),
                            juce::roundToInt(window::height * window::minScale),
                            juce::roundToInt(window::width * window::maxScale),
                            juce::roundToInt(window::height * window::maxScale));
                    }

                    /*
                     * The size hints above are only advice: a window manager
                     * that ignores maximum-size hints, as tiling ones do, can
                     * still hand the window more room than maxScale lets the
                     * editor take. The editor stays capped and the surplus is
                     * the window's own background, so paint that the ground
                     * colour and the surplus reads as margin rather than as a
                     * black band torn out of the panel.
                     */
                    windowComponent->setBackgroundColour(theme::colours::ground());

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
        {
            /*
             * Double-clicking a primary button is a common habit, and the
             * segment becomes "Cancel" the instant the job starts - inside
             * this very call stack, via the refreshFromProcessor() below.
             * A click that arrives before the OS double-click interval has
             * passed since that change was aimed at "Separate", so it is
             * dropped rather than turned into a cancel. Unsigned wrap makes
             * the subtraction correct across the counter's ~49-day rollover.
             */
            const auto sinceArmed =
                juce::Time::getMillisecondCounter() - separateCancelArmedMs;

            if (sinceArmed <
                static_cast<juce::uint32>(juce::MouseEvent::getDoubleClickTimeout()))
            {
                return;
            }

            processor.cancelSeparation(); // harmless if the engine just ended
        }
        else if (!processor.isEngineRunning())
            processor.launchSeparationAndExport();

        refreshFromProcessor();
    };

    panelContent.addAndMakeVisible(separateControl);

    // --------------------------------------------------------------- lanes

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

        rootLanes[static_cast<size_t>(i)]->setTransportSeekHandler(
            [this] { handleTransportMoved(); });

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
    {
        processor.transportSeekNormalised(normalised);

        // The thumb repaints itself inside Scrubber::applySeek; this is for
        // the six wells and the clock, which otherwise trail the pointer by
        // a whole tick - half a second once the editor has demoted.
        handleTransportMoved();
    };
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

    openFolderButton = std::make_unique<widgets::IconButton>(
        "open-job-folder", [](juce::Rectangle<float> b) { return stemlab::icons::folder(b); },
        static_cast<float>(theme::metrics::footer::folderIcon), true, 0.0f, false);

    openFolderButton->setTooltip("Open the output folder");
    openFolderButton->onClick = [this] { revealJobFolder(); };
    panelContent.addAndMakeVisible(*openFolderButton);

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

    /*
     * Armed at full rate and left for the first refresh to judge: opening
     * onto an idle processor demotes on the spot, opening onto a running
     * job leaves the full rate standing. Neither case has to be guessed at
     * here, before anything has been read.
     */
    applyRefreshRate(theme::metrics::uiRefreshHz);
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

    /*
     * Take the process default back down while the shared look and feel is
     * still alive. lookAndFeel is a SharedResourcePointer, so the object it
     * names dies with the last editor's member - after this body runs - and a
     * default left pointing at it would be dangling for the rest of shutdown.
     * The identity check is what makes this safe to run unconditionally: in a
     * host we never installed it, so the default belongs to someone else and
     * is left alone. Passing nullptr restores JUCE's own default, so a window
     * still open - a settings dialog left up - falls back to that rather than
     * following a dead pointer.
     */
    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &lookAndFeel.get())
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);

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
        const juce::File candidate(path);

        // Files of our own in-flight outbound drag - and anything else this
        // job produced - are not an invitation to reload the source they
        // were split from.
        if (!selfFileDragGuard.shouldIgnore(path) && isSupportedAudioFile(candidate) &&
            !candidate.isAChildOf(processor.getLastJobDirectory()))
        {
            return true;
        }
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

void StemLabAudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    footerDragStarted = false;
}

bool StemLabAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    // One key to sweep every lane's loop range away, however many lanes
    // were dragged over - the double-click escape only clears one.
    if (key == juce::KeyPress::escapeKey)
    {
        processor.clearAllStemSelectionRanges();
        return true;
    }

    // Space is the transport key everywhere else. It asks the play button
    // whether there is anything to play rather than repeating the condition,
    // so the key can never start playback the button itself would refuse -
    // mid-capture, mid-job, or with no audio loaded. Unhandled when it
    // cannot act, leaving the key to whatever else wants it.
    if (key == juce::KeyPress::spaceKey)
    {
        if (!playButton.isEnabled())
            return false;

        processor.transportTogglePlay();
        refreshFromProcessor();
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
    // Everything else is drawn by panelContent, which is scaled as a whole and
    // keeps its own proportions, so at any window shape but the design one this
    // paints the band along the two edges the centred panel does not reach.
    // surface() is what paintPanel lays along the panel's own outer edge, so
    // the band reads as more window rather than as a hole behind the panel -
    // and painting every pixel here is what stops a host's backdrop, which is
    // plain black in the VST3 wrapper, from being what fills that gap.
    g.fillAll(theme::colours::surface());
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
        drawCachedGlow(g, separateControl.getBounds());

    for (auto* primary : {&insertButton, &saveButton})
    {
        if (primary->isVisible() && primary->isEnabled() &&
            primary->getComponentID() == "primary")
        {
            drawCachedGlow(g, primary->getBounds());
        }
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

void StemLabAudioProcessorEditor::drawCachedGlow(juce::Graphics& g,
                                                 juce::Rectangle<int> area)
{
    // Matches DropShadow(accentGlow, 11, {}) exactly; the Gaussian blur just
    // runs once per size instead of on every paint.
    constexpr int radius = 11;
    constexpr int margin = radius * 2;

    auto& image = glowCache[{area.getWidth(), area.getHeight()}];

    if (!image.isValid())
    {
        image = juce::Image(juce::Image::ARGB, area.getWidth() + margin * 2,
                            area.getHeight() + margin * 2, true);

        juce::Graphics glow(image);

        juce::DropShadow(theme::colours::accentGlow(), radius, {})
            .drawForRectangle(glow, {margin, margin, area.getWidth(), area.getHeight()});
    }

    // Images draw at the current colour's opacity; the glow must not.
    g.setOpacity(1.0f);
    g.drawImageAt(image, area.getX() - margin, area.getY() - margin);
}

void StemLabAudioProcessorEditor::resized()
{
    // The host can resize before the constructor finishes building children.
    // Both of these are checked because they bracket the header's owned
    // controls: laying out with either still null would dereference it.
    if (settingsButton == nullptr || zoomResetButton == nullptr)
        return;

    // Buttons take new sizes with the layout, so cached glows for the old
    // ones would only pile up.
    glowCache.clear();

    namespace window = theme::metrics::window;

    /*
     * The panel is laid out once at its design size and then scaled as a
     * whole, so every metric in StemLabTheme stays a real pixel value and
     * nothing has to be re-derived per size. The smaller of the two ratios
     * wins, which is what lets the panel fit inside any window shape: the
     * axis that ran out sets the scale, the other keeps the leftover, and
     * paint() fills that band. Because the scale is read straight out of the
     * current size, the same window always yields the same scale however the
     * user got there.
     */
    const auto scale = juce::jmax(0.05, juce::jmin(static_cast<double>(getWidth()) / window::width,
                                                   static_cast<double>(getHeight()) / window::height));

    // The offset rides the transform rather than the bounds: a component's
    // position is applied before its transform, so an offset written into
    // setBounds would come back out multiplied by the scale. Rounded to whole
    // pixels so the panel does not land on a half one and blur.
    const auto offsetX = juce::roundToInt((getWidth() - window::width * scale) * 0.5);
    const auto offsetY = juce::roundToInt((getHeight() - window::height * scale) * 0.5);

    panelContent.setTransform(juce::AffineTransform::scale(static_cast<float>(scale))
                                  .translated(static_cast<float>(offsetX),
                                              static_cast<float>(offsetY)));
    panelContent.setBounds(0, 0, window::width, window::height);

    // Reopening the editor comes back at the scale the user left it. One
    // number cannot carry a shape, so a window left off the design aspect
    // reopens without its band rather than with it.
    processor.setEditorScalePercent(juce::roundToInt(scale * 100.0));

    layoutPanel();

    // Covers the panel at its design size, so the editor's own transform
    // scales it with everything else.
    modelManagerPanel.setBounds(panelContent.getLocalBounds());
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

    /*
        The eye reads the footer as sitting in the band between the divider
        and the panel's bottom edge, and the panel's own bottom padding is
        part of that band. Laid flush against that padding the row cleared
        the divider by dividerGap but the edge by dividerGap + padY, which
        left the buttons visibly pinned to the divider. Centring the row in
        the band splits the difference. The lanes above do not move: the
        row keeps the height it already took out of inner.
    */
    footerRow.translate(0, (panel::padY - footer::dividerGap) / 2);

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

        if (openFolderButton != nullptr)
            openFolderButton->setBounds(folderIconBounds);

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
    if (progressVisible)
        area.removeFromTop(footer::statusTopInset);

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

    // withHeight rather than removeFromTop: the inset pushed the row's
    // tail past the nominal footer height, and removeFromTop would clamp
    // the row (and mis-centre the track) instead of letting the readout's
    // descender overhang into the empty padding below.
    auto progressRow = area.withHeight(footer::progressRowHeight);

    /*
        The readout's slot is a constant, sized for the widest text the
        readout can show rather than for what it shows now. Deriving it
        from the live text resized the bar every time the clock ticked
        over or the ETA appeared, so the whole row visibly jumped around
        all run long. The slot must also come off the top before the bar
        is sized: reserving only the gap squeezed the readout to zero
        wherever the footer's buttons leave the status area narrow.

        Measured once: both the token and the text are fixed for the
        process, and this runs on every status refresh. The LookAndFeel
        publishes the font tokens from its constructor and the editor holds
        it as a member declared ahead of everything laid out here, so the
        first measurement cannot capture a fallback face.
    */
    static const int labelSlot = []
    {
        const juce::Font progressFont{theme::fonts::progress()};

        const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");

        return juce::roundToInt(juce::GlyphArrangement::getStringWidth(
                   progressFont, "100%" + dot + "888:88" + dot + "ETA 88:88")) +
               4;
    }();

    const int barWidth =
        juce::jlimit(0, footer::progressBarWidth,
                     progressRow.getWidth() - labelSlot - footer::progressLabelGap);

    progressBar.setBounds(progressRow.removeFromLeft(barWidth));
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

std::vector<StemLabRecursiveStemInfo> StemLabAudioProcessorEditor::getVisibleRecursiveItems(
    const std::vector<StemLabRecursiveStemInfo>& all) const
{
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
    // One fetch for the whole tick: getRecursiveStemItems takes a lock,
    // copies the vector and re-orders it, and this runs at 20 Hz.
    const auto all = processor.getRecursiveStemItems();
    const auto items = getVisibleRecursiveItems(all);

    /*
     * A collapsed row keeps its hidden descendants in the mix: the flags
     * live on the processor and reach the audio through each entry's
     * ancestor chain, which knows nothing about which rows are on screen.
     * Work out which visible row is doing the hiding for every soloed or
     * muted row that is off screen, so that row can say so.
     */
    hiddenActiveParents.clearQuick();
    hiddenSoloParents.clearQuick();

    for (const auto& item : all)
    {
        const bool onScreen = std::any_of(items.begin(), items.end(),
                                          [&item](const auto& v) { return v.id == item.id; });

        if (onScreen)
            continue;

        const bool soloed = processor.isRecursiveStemSoloed(item.id);
        const bool muted = processor.isRecursiveStemMuted(item.id);

        if (!soloed && !muted)
            continue;

        /*
         * The row that is actually on screen and doing the hiding: the
         * deepest visible ancestor, or the root row when the whole root is
         * collapsed. Depth order falls out of prefix length, because a
         * child's id is its parent's id plus "/name".
         */
        juce::String owner;

        for (const auto& candidate : items)
            if (item.id.startsWith(candidate.id + "/"))
                if (candidate.id.length() > owner.length())
                    owner = candidate.id;

        if (owner.isEmpty())
        {
            // Canonicalise: rootStem comes from the manifest and is matched
            // case-insensitively everywhere else in this file, while
            // StringArray::contains below is case-sensitive.
            for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
                if (StemLabAudioProcessor::getStemName(i).equalsIgnoreCase(item.rootStem))
                    owner = StemLabAudioProcessor::getStemName(i);
        }

        if (owner.isEmpty())
            continue;

        hiddenActiveParents.addIfNotAlreadyThere(owner);

        if (soloed)
            hiddenSoloParents.addIfNotAlreadyThere(owner);
    }

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
            lane->setChildState(item.hasChildren, isLaneExpanded(-1, item.id),
                                hiddenActiveParents.contains(item.id),
                                hiddenSoloParents.contains(item.id));
            lane->setZoomStepHandler([this](int delta) { stepWaveformZoom(delta); });
            lane->setTransportSeekHandler([this] { handleTransportMoved(); });
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
                                         isLaneExpanded(-1, items[i].id),
                                         hiddenActiveParents.contains(items[i].id),
                                         hiddenSoloParents.contains(items[i].id));
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

    /*
     * Parent the menu to panelContent so it is a child component rather than a
     * free-floating desktop window: JUCE only clamps a parentless menu against
     * the whole display, which is how a menu opened near the right-hand edge
     * ended up mostly outside the window.
     *
     * panelContent and not the editor, for two reasons. The scale lives on
     * panelContent's transform, and setting any parent makes JUCE stop
     * deriving the menu's scale from the target component - parented to the
     * editor, which carries no transform, every menu would draw at 1.0 while
     * the interface ran between 0.70x and 2.50x. And the editor includes the
     * letterbox band paint() fills with surface(), the same colour the menu
     * background uses, so a menu could spill into it with no visible edge.
     *
     * The same applies to the other four menus below; submenus inherit it.
     */
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(target).withParentComponent(&panelContent),
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

    /*
     * Parented to panelContent for the reasons given in showRootLayersMenu.
     * The mouse position stands in when the lane could not be found: a target
     * component is what fills the area the menu is placed against, and an
     * empty one inside a parent resolves to the parent's top-left corner
     * rather than to anything the user pointed at.
     */
    auto options = juce::PopupMenu::Options().withParentComponent(&panelContent);

    options = target != nullptr ? options.withTargetComponent(target) : options.withMousePosition();

    menu.showMenuAsync(
        options,
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

void StemLabAudioProcessorEditor::applyRefreshRate(int hz)
{
    /*
     * Message thread only - every caller is already on it, which is what
     * juce::Timer asserts. Retuning from inside timerCallback is legal: a
     * running timer just has its counter reset. The equality guard is what
     * keeps a steady state from resetting that counter on every tick, which
     * would postpone the next callback forever.
     */
    if (hz == currentRefreshHz)
        return;

    currentRefreshHz = hz;
    startTimerHz(hz);
}

void StemLabAudioProcessorEditor::requestFastFrames()
{
    fastFramesUntilMs = juce::Time::getMillisecondCounter() + theme::metrics::uiIdleHoldMs;
    applyRefreshRate(theme::metrics::uiRefreshHz);
}

bool StemLabAudioProcessorEditor::refreshLaneWaveforms()
{
    // Not a blanket repaint: each well compares what it would draw against
    // the last tick and repaints only what actually changed, so idle lanes
    // cost nothing and a moving playhead costs two thin strips.
    bool changed = false;

    // Call first, accumulate second: || would short-circuit past every lane
    // after the first one that redrew.
    for (auto& lane : rootLanes)
        if (lane != nullptr)
            changed = lane->timerRefreshWaveform() || changed;

    for (auto& lane : childLanes)
        if (lane != nullptr)
            changed = lane->timerRefreshWaveform() || changed;

    return changed;
}

void StemLabAudioProcessorEditor::handleTransportMoved()
{
    /*
     * Order matters, and so does the guard. Promoting first means the timer
     * takes over within 50 ms; the synchronous catch-up is only needed for
     * the event that finds the editor slow, which is the one whose lag
     * would otherwise be half a second.
     *
     * Scrubber::applySeek fires from mouseDrag as well as mouseDown, at
     * whatever rate the pointer reports - well above 20 Hz on most devices.
     * Refreshing the whole editor on every one of those would cost more
     * during a drag than the timer this stage is here to slow down, and buy
     * nothing: after the first event the promoted timer is already
     * refreshing at exactly the rate a drag used to see.
     */
    const bool wasSlow = currentRefreshHz != theme::metrics::uiRefreshHz;

    requestFastFrames();

    if (wasSlow)
    {
        refreshFromProcessor();
        refreshLaneWaveforms();
    }
}

void StemLabAudioProcessorEditor::timerCallback()
{
    const auto nowMs = juce::Time::getMillisecondCounter();

    processor.refreshEngineProgressFromDisk();

    // A finished adaptive split hands the parent's place in the stem mix to
    // its children; this is where that reaches the monitor.
    processor.refreshStemMixIfNeeded();

    // Only the Ableton bridge ever writes those files; polling for them in
    // REAPER or a plain host is pure disk traffic at the timer rate.
    if (processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationAbletonLive)
    {
        /*
         * The clip reply is what Import from DAW is actively waiting on, so
         * it keeps the full tick rate - and the wait it serves lives inside
         * isBackgroundWorkRunning(), so the editor is at full rate for
         * exactly that window anyway. The bridge status feeds a status line,
         * which does not need 50 ms latency: a 250 ms deadline divides its
         * steady-state file traffic accordingly.
         *
         * A deadline rather than a tick divider because the tick itself
         * changes rate: "every 5th tick" would have meant 2.5 s at idle.
         */
        processor.refreshAbletonSourceClipFromDisk();

        if (nowMs - lastAbletonStatusPollMs >= 250)
        {
            lastAbletonStatusPollMs = nowMs;
            processor.refreshAbletonBridgeStatusFromDisk();
        }
    }

    // Decides the rate for the next tick, from the state it just read.
    refreshFromProcessor();

    // An analysis landing is the one change here that nothing announces and
    // no processor poll covers, so arrival holds the full rate over the
    // moment a finished job fills its six wells.
    if (refreshLaneWaveforms())
        requestFastFrames();

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
    /*
     * Deliberately not a refresh. A chatty engine - sendChangeMessage per
     * stdout line - would multiply this into a refresh storm, and user
     * actions that want feedback inside the same event keep their own
     * direct refreshFromProcessor() calls.
     *
     * What it does do is hold the full refresh rate for a moment. The
     * editor drops to theme::metrics::uiIdleRefreshHz when nothing on
     * screen can change on its own, and an announcement from the processor
     * is precisely the case that is not visible from here: promoting
     * restores the 50 ms bound on status latency that the old unconditional
     * 20 Hz tick used to provide, and costs at most one startTimerHz.
     */
    requestFastFrames();
}

juce::String StemLabAudioProcessorEditor::jobSummaryLine() const
{
    int readyCount = 0;

    // The scan-cache answer, not a stat: once a job is done this line
    // renders every tick.
    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        if (processor.hasCompletedStemFile(i))
            ++readyCount;

    const auto duration = processor.getMainJobDurationSeconds() > 0.0
                              ? processor.getMainJobDurationSeconds()
                              : processor.getEngineElapsedSeconds();

    return "Separated " + juce::String(readyCount) + " stems in " + formatSeconds(duration) +
           juce::String::fromUTF8(" \xc2\xb7 refinement ") +
           (processor.wasLastJobRefined() ? "on" : "off");
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
    // Ahead of the rest: the manager can open itself here, and it should be
    // showing current state on the frame it appears rather than one later.
    considerAutoShowingModelManager();
    refreshModelManager();

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
     * pills and the readout stay dark until then - "0 of 6 stems will be
     * saved" over six empty lanes is noise, not information.
     */
    const auto [includedLanes, totalLanes] = laneSelectionCounts();

    const bool lanesLive = jobDone && !engineRunning && !capturing;

    selectAllButton.setEnabled(lanesLive && includedLanes < totalLanes);
    deselectAllButton.setEnabled(lanesLive && includedLanes > 0);

    // The header readout: a fresh user-action message holds it for a few
    // seconds, then the selection count takes back over. Work the plugin
    // is doing never appears here - that is the bottom status line's job.
    // Hoisted out of the block below because the refresh-rate decision at
    // the end of this function needs it: a readout on a 4 s clock is one of
    // the few things that expires without anyone touching anything.
    bool actionFresh = false;

    {
        const auto actionRevision = processor.getActionStatusRevision();

        if (actionRevision != lastActionStatusRevision)
        {
            lastActionStatusRevision = actionRevision;
            actionStatusShownMs = nowMs;
        }

        const auto actionText = processor.getActionStatus();

        actionFresh = actionText.isNotEmpty() && nowMs - actionStatusShownMs < 4000;

        userStatusLabel.setText(actionFresh ? actionText
                                : lanesLive ? juce::String(includedLanes) + " of " +
                                                  juce::String(totalLanes) +
                                                  " stems will be saved"
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

    /*
     * Zoom is a view control, not a job control: it stays live whenever
     * there is a waveform to look at, including while a job runs - which is
     * when the lanes begin drawing stems as the engine announces them. A
     * loaded source is not a drawn waveform: before the first job every
     * lane's file is empty, so the wells are blank and walking the slider
     * from 1x to 64x repaints nothing at all.
     */
    const bool haveWaveform = jobDone || engineRunning;

    zoomResetButton->setEnabled(haveWaveform);
    zoomSlider.setEnabled(haveWaveform);

    // The readout dims through its text colour rather than component alpha:
    // 50% text at 45% alpha is 1.9:1 against the panel, and the floor in
    // dimDisabled only applies to a colour. Label::colourChanged repaints
    // only when the value actually moves, so this is free on most ticks.
    zoomLabel.setColour(juce::Label::textColourId,
                        theme::colours::dimIfDisabled(theme::colours::text50(), haveWaveform));

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

    // Stamped on the transition rather than at launch, so the guard also
    // covers adaptive splits started from a lane's kebab menu - those flip
    // the segment to "Cancel" without ever passing through onSeparate.
    if (engineRunning && !separateControlShowsCancel)
        separateCancelArmedMs = nowMs;

    separateControlShowsCancel = engineRunning;

    const juce::String actionText{engineRunning ? (cancelPending ? "Cancelling..." : "Cancel")
                                                : "Separate"};

    separateControl.setActionText(actionText);

    // The action segment's own tip. Guarded because this runs on every timer
    // tick, and because the segment means two different things: it starts a
    // job, or it stops the one running.
    if (actionText != lastSeparateActionText)
    {
        lastSeparateActionText = actionText;

        separateControl.setTooltip(engineRunning ? "Stop the running separation"
                                                 : "Split the loaded audio into stems");
    }

    separateControl.setSeparateEnabled(
        engineRunning ? !cancelPending
                      : (!capturing && !processor.isAwaitingAbletonSourceClip() &&
                         captureExists));

    separateControl.setRefineOn(processor.isRefinementEnabled());

    // Refine feeds the engine command line at launch, so it locks with the
    // rest of the job controls rather than flipping under a running job.
    separateControl.setRefineInteractive(!engineRunning);

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

    /*
     * The strip gives the name whatever the buttons leave over - about 20
     * characters at design size - and the full path appears nowhere else in
     * the window, so a long name loses the part that identifies it. Carry
     * the path in a tooltip, but only while the text does not fit: an
     * always-on tooltip over a name that is already fully readable is just
     * noise. The test is "does not fit" rather than "was ellipsized"
     * because drawFittedText squeezes to 70% before it ellipsizes, and a
     * squeezed 120-character name is no more readable than a clipped one.
     */
    {
        const juce::Font nameFont{theme::fonts::bodyMedium()};

        const auto available = static_cast<float>(
            fileNameLabel.getWidth() - fileNameLabel.getBorderSize().getLeftAndRight());

        const bool clipped =
            available > 0.0f &&
            juce::GlyphArrangement::getStringWidth(nameFont, fileName) > available;

        fileNameLabel.setTooltip(clipped && captureExists ? captureFile.getFullPathName()
                                                          : juce::String());
    }

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

    /*
        Two ways the stop control vanished from under playing audio, and
        playing is reason enough to keep the button live against both.

        A source deleted underneath a running transport, whatever the file
        system now says. And a job started while a stem was auditioning:
        starting playback is rightly gated on !engineRunning, but stopping it
        never is, so that gate has to sit inside the parentheses rather than
        around the whole condition.

        A disabled Button never enters buttonDown, so its onClick never
        fires. Greying this out is what made the sound unstoppable rather
        than merely awkward.
    */
    const bool transportPlaying = processor.isTransportPlaying();

    playButton.setEnabled(transportPlaying ||
                          ((captureExists || jobDone) && !capturing && !engineRunning));
    playButton.setShowPause(transportPlaying);

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
            const auto rootName = StemLabAudioProcessor::getStemName(i);

            // syncLanes() ran first this tick, so the hidden-activity sets
            // are fresh; do not move this loop above it.
            lane->setChildState(hasChildren, isLaneExpanded(i, {}),
                                hiddenActiveParents.contains(rootName),
                                hiddenSoloParents.contains(rootName));
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

    /*
     * The severity describes rawStatus, and showSummary has just replaced
     * it with a sentence about the finished job. Every failure that leaves
     * hasSuccessfulJob() standing - an adaptive split that will not start,
     * one whose manifest is rejected, a STEMLAB_ERROR from the recursive
     * worker - would otherwise latch red and, five seconds later, repaint
     * the main job's success summary in the failure colour beside a red
     * cross, and stay that way until some later status changed severity.
     * A failure with no summary behind it still reads as one: launching a
     * main job clears hasSuccessfulJob() first, so showSummary is false
     * for the whole of a failed separation.
     */
    const bool statusIsError =
        !showSummary && processor.getStatusSeverity() == StemLabAudioProcessor::statusFailure;

    // Only on change: juce::Label::setColour repaints, and this runs at 20 Hz.
    if (statusIsError != lastStatusWasError)
    {
        lastStatusWasError = statusIsError;
        statusLabel.setColour(juce::Label::textColourId, statusIsError
                                                             ? theme::colours::statusError()
                                                             : theme::colours::text50());
    }

    // busy outranks error, so a failure line left over from a previous job
    // cannot freeze the spinner while new work is already running.
    statusIndicator.setState(busy            ? widgets::StatusIndicator::State::running
                             : statusIsError ? widgets::StatusIndicator::State::error
                             : jobDone       ? widgets::StatusIndicator::State::done
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

        // Refine's tip, not the whole control's: setting it here used to
        // describe Refine while hovering Separate.
        separateControl.setRefineTooltip("Runs after " +
                                         processor.getSeparatorEngineDisplayName() +
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
        // Measured only when there is something new to measure. Constructing
        // a Font and shaping a whole path through GlyphArrangement is not
        // free, and this runs on every refresh for a string that changes
        // when the user picks a different job folder - which is to say
        // almost never.
        if (jobPath != lastJobPath || pathLabel.getWidth() != lastJobPathLabelWidth)
        {
            const juce::Font pathFont{theme::fonts::footerPath()};

            lastJobPath = jobPath;
            lastJobPathLabelWidth = pathLabel.getWidth();
            lastJobPathWidth = juce::jmin(
                pathLabel.getWidth(),
                juce::roundToInt(juce::GlyphArrangement::getStringWidth(pathFont, jobPath)) + 1);
        }

        const int textWidth = lastJobPathWidth;

        const int textLeft =
            pathLabel.getRight() - pathLabel.getBorderSize().getRight() - textWidth;

        const auto placed = folderIconBounds.withX(textLeft - theme::metrics::footer::folderIconGap -
                                                   folderIconBounds.getWidth());

        if (placed.getX() != folderIconBounds.getX())
        {
            folderIconBounds = placed;

            // Moving the button repaints both the vacated and the new
            // rectangle by itself, so this no longer does it by hand.
            if (openFolderButton != nullptr)
                openFolderButton->setBounds(folderIconBounds);
        }
    }

    if (openFolderButton != nullptr)
    {
        // The path is only meaningful once there is a folder to point at,
        // and the icon should not invite a click that opens nothing.
        openFolderButton->setEnabled(processor.getJobRootDirectory().isDirectory());
    }

    changeFolderButton.setEnabled(!engineRunning);

    int selectedCount = 0;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        if (processor.isStemEnabled(i))
            ++selectedCount;

    for (const auto& item : processor.getRecursiveStemItems())
        if (item.selected)
            ++selectedCount;

    saveButton.setEnabled(jobDone && !engineRunning && !capturing && selectedCount > 0);
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

    // ------------------------------------------------------- refresh rate

    /*
     * Last, because it reads state this pass computed. An open window with
     * nothing happening in it used to cost twenty full refreshes a second
     * forever; almost every user action already refreshes synchronously
     * from its own handler, so the timer only has to keep pace with things
     * that move without being touched.
     *
     * These four are all of them. Everything animated on screen - the
     * spinner, the record dot, the status dots - is a function of the wall
     * clock drawn only while busy or capturing, which busy covers. The
     * playhead, the elapsed clock and the ETA move under a running
     * transport or a running job. And two readouts expire on a deadline
     * somebody is watching: the header's 4 s action message, and the 5 s
     * wait before the summary line replaces a finished job's last words.
     * Once the summary has taken over, showSummary stays true and there is
     * nothing left to wait for.
     *
     * Hover is deliberately absent: every hover in this interface repaints
     * from its own mouse events, so none of it needs a frame from here.
     */
    const bool needsFrames = busy // engine, analysis, MIDI, capture, an awaited Live clip
                             || processor.isTransportPlaying() // playhead, clock, scrubber
                             || actionFresh                // header readout still on its 4 s clock
                             || (jobDone && !showSummary); // the 5 s swap to the summary line

    if (needsFrames)
        fastFramesUntilMs = nowMs + theme::metrics::uiIdleHoldMs;

    // Signed on purpose: getMillisecondCounter wraps about every 49 days,
    // and an unsigned comparison would read the wrap as "held forever" and
    // pin the editor at full rate for the rest of the session.
    applyRefreshRate(static_cast<juce::int32>(fastFramesUntilMs - nowMs) > 0
                         ? theme::metrics::uiRefreshHz
                         : theme::metrics::uiIdleRefreshHz);
}

void StemLabAudioProcessorEditor::chooseStandaloneAudioFile()
{
    if (!processor.usesLocalFileWorkflow() || processor.isCapturing())
        return;

    /*
     * Start where the current source lives, the way chooseEngineExecutable
     * does. Handing the chooser the file itself rather than its folder both
     * opens that folder and preselects the file, so working through several
     * takes from one folder does not mean navigating back from $HOME every
     * time. After a capture the source is a recording under the job root and
     * the dialog opens there - still where the audio actually is.
     */
    auto start = processor.getCaptureFile();

    if (!start.existsAsFile())
        start = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

    audioFileChooser = std::make_unique<juce::FileChooser>(
        "Choose audio file", start, "*.wav;*.flac;*.mp3;*.aiff;*.aif;*.ogg");

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

void StemLabAudioProcessorEditor::revealJobFolder()
{
    const auto folder = processor.getJobRootDirectory();

    if (!folder.isDirectory())
    {
        processor.postUiStatus("No output folder yet");
        return;
    }

    /*
        revealToUser() opens the file manager with the item selected on
        Windows and macOS. On Linux JUCE shells out to xdg-open, which has no
        notion of selecting an item and which is absent on a minimal desktop -
        and a failure there is silent, so the click would do nothing at all
        with no explanation. Hence the explicit fallbacks and the status line.
    */
#if JUCE_LINUX
    const char* const openers[] = {"xdg-open", "gio", "nautilus", "dolphin", "thunar", "nemo"};

    for (const auto* opener : openers)
    {
        juce::ChildProcess process;
        juce::StringArray command{opener};

        if (juce::String(opener) == "gio")
            command.add("open");

        command.add(folder.getFullPathName());

        if (process.start(command))
        {
            processor.postUiStatus("Opened the output folder");
            return;
        }
    }

    juce::SystemClipboard::copyTextToClipboard(folder.getFullPathName());
    processor.postUiStatus("No file manager found - path copied to the clipboard");
#else
    folder.revealToUser();
    processor.postUiStatus("Opened the output folder");
#endif
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
    menu.setLookAndFeel(&lookAndFeel.get());

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

    // The lanes only redraw on the timer, which is a whole frame away; a
    // zoom change has to show now, and it has to go through the wells'
    // captured display state or the paint would still frame the old view.
    refreshLaneWaveforms();
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

    // Parented to panelContent for the reasons given in showRootLayersMenu.
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(engineSelector.get())
                           .withParentComponent(&panelContent),
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

    // The menu's order and spelling are the design's: Spectrum, RGB,
    // 3-Band, Stem Color, Nocturne. Persisted indices stay put; only the
    // listing order differs from them.
    static constexpr int menuPalettes[] = {2, 3, 4, 1, 0};

    for (const int palette : menuPalettes)
    {
        menu.addItem(palette + 1, theme::waveform::paletteName(palette), true,
                     processor.getWaveformColourIndex() == palette);
    }

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);

    // Parented to panelContent for the reasons given in showRootLayersMenu.
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(paletteButton.get())
                           .withParentComponent(&panelContent),
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

    menu.addItem(modelManagerId, "Model Manager...");

    // Ticked = on. Only the hybrid engine fuses, so it is greyed out for the
    // single-model engines rather than hidden: the state still persists, and
    // hiding it would make the setting look like it had been lost.
    menu.addItem(fusedNormaliseId, "Normalise Fused Stems",
                 processor.getSeparatorEngineIndex() == StemLabAudioProcessor::separatorHybrid,
                 processor.isFusedStemNormalisation());

    menu.addSeparator();

    menu.addItem(4, "Copy diagnostics to clipboard", processor.hasEngineLog());

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

    // Parented to panelContent for the reasons given in showRootLayersMenu.
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(settingsButton.get())
            .withParentComponent(&panelContent),
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
            else if (result == modelManagerId)
            {
                safeThis->showModelManager();
            }
            else if (result == fusedNormaliseId)
            {
                const bool on = !safeThis->processor.isFusedStemNormalisation();

                safeThis->processor.setFusedStemNormalisation(on);

                safeThis->processor.postUiStatus(
                    on ? "Fused stems will be normalised to 0.999 each"
                       : "Fused stems keep their level and sum back to the source");
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

void StemLabAudioProcessorEditor::showModelManager()
{
    modelManagerDismissed = false;

    modelManagerPanel.setBounds(panelContent.getLocalBounds());
    modelManagerPanel.setVisible(true);
    modelManagerPanel.toFront(true);

    refreshModelManager();

    // Ask the engine again on every open. The inventory is cheap, and a user
    // who has just downloaded a model outside StemLab should not be told it
    // is missing because the answer is from when the editor opened.
    processor.refreshModelInventory();
}

void StemLabAudioProcessorEditor::closeModelManager()
{
    modelManagerDismissed = true;
    modelJobReported = false;
    modelManagerPanel.setVisible(false);

    // Give the keyboard back, or the panel behind stays deaf to shortcuts.
    grabKeyboardFocus();
}

void StemLabAudioProcessorEditor::refreshModelManager()
{
    if (!modelManagerPanel.isVisible())
        return;

    if (processor.modelInventoryFailed())
    {
        modelManagerPanel.setUnavailable(
            "The StemLab engine could not report its models.\n"
            "Check the engine under Settings, then reopen this.");
    }
    else if (processor.hasModelInventory())
    {
        modelManagerPanel.setInventory(processor.getManagedModels(),
                                       processor.getManagedCaches());
    }
    else
    {
        modelManagerPanel.setUnavailable("Asking the engine what is installed...");
    }

    modelManagerPanel.setCompileState(processor.isTorchCompileEnabled(),
                                      processor.isCompileSupported(),
                                      processor.getCompileReason());

    const auto busy = processor.isModelJobRunning();

    // Once something has been asked for, the status line is the only account
    // of how it went that the user can actually see from here.
    modelManagerPanel.setActivity(
        busy || modelJobReported ? processor.getStatus() : juce::String{},
        processor.getEngineProgress(), busy);
}

void StemLabAudioProcessorEditor::considerAutoShowingModelManager()
{
    // Once per editor, and never over a dismissal. Everything else about
    // when it opens is the engine's answer rather than a rule repeated here.
    if (modelManagerAutoShown || modelManagerDismissed)
        return;

    if (!processor.hasModelInventory() || modelManagerPanel.isVisible())
        return;

    // Only a missing essential model. Compiling being available but not yet
    // done is not a reason to interrupt anyone - it is a thing to go and do,
    // not a thing that is wrong.
    if (!processor.isEssentialModelMissing())
        return;

    modelManagerAutoShown = true;
    showModelManager();
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
