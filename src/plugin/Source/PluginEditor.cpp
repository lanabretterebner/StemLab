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
/** The directory the running binary sits in. */
juce::File applicationDirectory()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
}

juce::File abletonSetupScript()
{
    auto root = applicationDirectory();

    for (int depth = 0; depth < 6 && root.exists(); ++depth)
    {
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

/** A tempo for display: "128", "128.5", never "128.00".

    Not String(bpm, 2).trimCharactersAtEnd("0.") - that walks back over every
    trailing character in the set, so "100.00" loses its zeros AND its point
    AND the zeros before it, leaving "1". Whole tempos take the integer path
    instead, which leaves the fractional trim only cases that end in a
    non-zero digit.
*/
juce::String formatBpmForDisplay(double bpm)
{
    if (std::abs(bpm - std::round(bpm)) < 0.005)
        return juce::String(static_cast<int>(std::llround(bpm)));

    return juce::String(bpm, 2).trimCharactersAtEnd("0");
}

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

/** Which root lane an adaptive item's rootStem name belongs to, or -1.

    The name comes out of the engine's manifest and is matched
    case-insensitively wherever it is read, which is why this is a search
    rather than an index. */
int rootIndexForStemName(const juce::String& rootStem)
{
    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
        if (StemLabAudioProcessor::getStemName(i).equalsIgnoreCase(rootStem))
            return i;

    return -1;
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
    geometry.start = viewStart;

    geometry.snappedStart = stemlab::waveform::snappedViewStart(
        viewStart, geometry.viewLength, static_cast<double>(geometry.inner.getWidth()));

    return geometry;
}

void StemLaneWaveform::refreshMidiNotes()
{
    // Both halves matter: the count moves when a stem is converted or
    // re-converted, and the id moves when this well is reused for another
    // lane, which can carry the same count as the one it replaced.
    if (midiNotesId == selectionId && midiNotesCount == lastDisplay.midiNoteCount)
        return;

    midiNotesId = selectionId;
    midiNotesCount = lastDisplay.midiNoteCount;

    if (midiNotesCount == 0)
    {
        midiNotes.clear();
        return;
    }

    midiNotes = processor.getMidiInfo(selectionId).notes;
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
    state.palette = processor.getWaveformColorIndex();
    state.accent = theme::accents::index();

    // Scalars only: this runs per lane per tick, and the full grid info
    // takes the processor's state lock to copy beat vectors nothing here
    // reads.
    const auto grid = processor.getWaveformGridScalars();

    state.gridBpm = grid.bpm;
    state.gridBarOne = grid.barOne;
    state.gridNumerator = grid.numerator;
    state.gridDenominator = grid.denominator;

    // A pointer and a number, not the beats themselves - see DisplayState.
    state.useDetectedBeats = processor.isRulingFromDetectedBeats();
    state.beatRevision = processor.getBeatSnapshotRevision();
    state.beats = processor.getBeatSnapshot();

    state.midiNoteCount = processor.getMidiNoteCount(selectionId);

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
    const auto palette = processor.getWaveformColorIndex();
    const auto accent = theme::accents::index();

    // Everything that shapes or colors the pixels; a change in any of it
    // means neither the columns nor their rendering can be reused.
    const bool sameSetup = columnsFile == currentFile && columnsWidth == width &&
                           columnsHeight == height && columnsChannels == channels &&
                           columnsPalette == palette && columnsAccent == accent &&
                           columnsMuted == mutedAppearance &&
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
    columnsAccent = accent;
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

        // The whole waveform draws in its full palette color: position is
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
                    juce::Colour color;
                };

                BandBar bars[3] = {{column.bands.low, theme::waveform::bandLowColor()},
                                   {column.bands.mid, theme::waveform::bandMidColor()},
                                   {column.bands.high, theme::waveform::bandHighColor()}};

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

                    g.setColour(bar.color);
                    g.fillRect(x, centre - half, 1.0f, half * 2.0f);
                }

                continue;
            }

            juce::Colour color;

            if (columnsMuted)
                color = theme::colors::waveMuted();
            else if (columnsPalette == theme::waveform::paletteRgb)
                color = theme::waveform::rgbColor(column.bands.low, column.bands.mid,
                                                    column.bands.high);
            else
                color = theme::waveform::playedColor(columnsPalette, columnsIdentity,
                                                       column.brightness);

            g.setColour(color);
            g.fillRect(x, topY, 1.0f, bottomY - topY);
        }
    }
}

void StemLaneWaveform::paint(juce::Graphics& g)
{
    namespace lanes = theme::metrics::lanes;

    const auto full = getLocalBounds().toFloat();

    g.setColour(theme::colors::laneWell());
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

    /*  Measured from the true start, not the snapped one.

        The window is centred on the playhead, so mid-file the playhead
        belongs at the middle of the well and should not move at all. Against
        the snapped origin it did: the floor lags the true start by up to a
        column and that lag grows every frame until the snap catches up, so
        the playhead crept backwards a fraction of a pixel per frame and
        jumped forward a whole one when it did. Zoomed in that reads as
        jitter; at 1x the window never scrolls, so it never showed.

        Everything on absolute time shares this origin, so the grid, the
        notes and the selection stay with the playhead. The column image is
        still blitted from snappedStart and is therefore up to one column out
        - a sub-pixel offset on a picture that is scrolling anyway, against a
        playhead that is now still.
    */
    const auto viewStart = geometry.start;

    const auto secondsToX = [&inner, viewStart, viewLength](double seconds)
    {
        return inner.getX() +
               static_cast<float>((seconds - viewStart) / viewLength) * inner.getWidth();
    };

    const auto transportLength = lastDisplay.transportLength;
    const auto transportPosition = lastDisplay.transportPosition;

    const double playNormalised =
        transportLength > 0.0 ? juce::jlimit(0.0, 1.0, transportPosition / transportLength)
                              : -1.0;

    /*  What this paint is actually allowed to touch.

        A repaint that only follows the playhead clips to the two 9 px strips
        it left and entered (see timerRefresh). The column image is one blit
        and JUCE clips it for free, but everything else below here is drawn
        per item - a rule and a shaped number per grid line, a rounded
        rectangle per note - and would otherwise be built in full for the
        sake of eighteen pixels, on every tick of every playing lane.
    */
    const auto clip = g.getClipBounds().toFloat();

    // Beat grid behind the waveform: bars read stronger than beats, and the
    // whole thing stays subordinate to the audio it sits behind. The rules
    // draw here; the numbers are only gathered, and go on after the audio.
    gridLabels.clear();

    {
        const auto gridBpm = lastDisplay.gridBpm;

        if (gridBpm > 0.0)
        {
            const auto secondsPerBeat = 60.0 / gridBpm;
            const auto beatsPerBar = juce::jmax(1, lastDisplay.gridNumerator);

            if (secondsPerBeat > 0.0 && secondsPerBeat * beatsPerBar * 3.0 < length)
            {
                /*
                 * The lines themselves come from makeGridLines, which is
                 * also what the loop-quantise snap measures against - one
                 * definition of where a beat is, so a snapped loop lands on
                 * a line the lane actually drew.
                 *
                 * With an analysed source it rules from the detected beats,
                 * which is what makes a track whose tempo moves rule
                 * correctly: the spacing is whatever was measured there, so
                 * nothing here needs to know the tempo changed. Everywhere
                 * else it walks a constant tempo from bar one, as before.
                 */
                stemlab::waveform::GridRequest request;

                request.visibleStart = snappedStart;
                request.visibleEnd = snappedStart + viewLength;
                request.pixelWidth = juce::jmax(1, static_cast<int>(inner.getWidth()));
                request.bpm = gridBpm;
                request.numerator = beatsPerBar;
                request.denominator = juce::jmax(1, lastDisplay.gridDenominator);
                request.barOne = lastDisplay.gridBarOne;
                request.useDetectedBeats = lastDisplay.useDetectedBeats;

                if (lastDisplay.useDetectedBeats && lastDisplay.beats != nullptr)
                {
                    // Spans over the snapshot this lane is holding, which
                    // outlives the call by construction.
                    request.beats = lastDisplay.beats->beats;
                    request.downbeats = lastDisplay.beats->downbeats;
                }

                const auto lines = stemlab::waveform::makeGridLines(request);

                const auto pixelsPerSecond =
                    static_cast<double>(inner.getWidth()) / viewLength;

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
                    secondsPerBeat * pixelsPerSecond >= lanes::gridLabelMinSpacing;

                for (const auto& line : lines)
                {
                    const auto bar = line.kind == stemlab::waveform::GridLineKind::bar;

                    const auto subdivision =
                        line.kind == stemlab::waveform::GridLineKind::subdivision;

                    const auto x = secondsToX(line.seconds);

                    if (x < inner.getX() || x > inner.getRight())
                        continue;

                    /*  Outside this paint's clip there is nothing to draw and
                        nothing to measure. The window reaches back only as
                        far as a line whose number could still lean into the
                        clip: the rule sits at x and its number runs to the
                        right of it, so a line just off the left edge can
                        still owe one.

                        The reach is the label rect doubled rather than the
                        rect itself, because the plate behind the number is
                        sized from the text's own ink (ink +
                        gridLabelPlatePadding, see below) and a long
                        "1024.4" at high zoom outgrows the 26 px rect the
                        glyphs are laid into. Reaching back by the rect alone
                        would drop that line, and the strip repaint would
                        leave the waveform showing through where its plate
                        should be. One extra line kept per paint is cheaper
                        than getting that wrong.
                    */
                    if (x > clip.getRight() ||
                        x + 2.0f + 2.0f * lanes::gridLabelWidth < clip.getX())
                    {
                        continue;
                    }

                    /*  Three weights, not two: subdivisions only appear once
                        a beat is wide enough to hold them, and at this alpha
                        they read as a ruler inside the beat rather than as
                        more beats.
                    */
                    g.setColour(theme::colors::text().withAlpha(
                        bar ? 0.22f : (subdivision ? 0.05f : 0.10f)));
                    g.fillRect(x, inner.getY(), bar ? 1.4f : 1.0f, inner.getHeight());

                    if (subdivision)
                        continue;

                    if (!bar && !labelBeats)
                        continue;

                    if (bar && barLabelStep > 1 && line.barNumber % barLabelStep != 0)
                        continue;

                    const auto text = bar ? juce::String(line.barNumber)
                                          : juce::String(line.barNumber) + "." +
                                                juce::String(line.beatInBar + 1);

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
    // draw at the current color's opacity, and the grid rules left a
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
     * Each number carries a small plate of the well's own ground color, so
     * it reads against the surface its contrast was chosen against - 5.15:1
     * for a bar, 3.11:1 for a beat - instead of against whatever the audio
     * happens to be doing underneath. On a quiet lane the plate is the
     * color that was already there and nothing shows; on a loud one it is
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
                    theme::colors::laneWell().withAlpha(lanes::gridLabelPlateAlpha));
                g.fillRoundedRectangle(plate, lanes::gridLabelPlateRadius);
            }

            g.setColour(theme::colors::text().withAlpha(item.bar ? 0.55f : 0.38f));
            g.drawText(item.text, item.bounds, juce::Justification::topLeft, false);
        }
    }

    if (!haveColumns)
        return;

    /*
     * The transcription, over the audio it came from. Drawn after the
     * waveform blit so the notes read as an overlay rather than something
     * the audio is painted on top of, and before the playhead and the
     * selection, which have to stay legible over both.
     */
    refreshMidiNotes();

    if (!midiNotes.empty())
    {
        const auto viewEnd = snappedStart + viewLength;

        /*
         * The pitch span is taken from the whole transcription, not from
         * what is on screen: a span recomputed per view would make notes
         * slide up and down the well as the view scrolls past the highest
         * and lowest of them, which reads as the pitches changing.
         */
        int lowest = 127;
        int highest = 0;

        for (const auto& note : midiNotes)
        {
            lowest = juce::jmin(lowest, note.pitch);
            highest = juce::jmax(highest, note.pitch);
        }

        // A stem on one pitch - a kick, a single held note - would divide by
        // zero. Give it an octave of room and let it sit in the middle.
        if (highest - lowest < 12)
        {
            const auto centre = (lowest + highest) / 2;
            lowest = juce::jmax(0, centre - 6);
            highest = lowest + 12;
        }

        const auto span = static_cast<float>(highest - lowest);

        // Inset so the top and bottom rows are not clipped by the well edge.
        const auto band = inner.reduced(0.0f, lanes::wellRadius);
        const auto rowHeight = juce::jmax(1.5f, band.getHeight() / (span + 1.0f));

        g.setColour(theme::colors::midiOverlay());

        for (const auto& note : midiNotes)
        {
            if (note.end <= snappedStart || note.start >= viewEnd)
                continue;

            const auto x1 = secondsToX(juce::jmax(note.start, snappedStart));
            const auto x2 = secondsToX(juce::jmin(note.end, viewEnd));

            // A note shorter than a pixel still has to be visible: a hi-hat
            // at 64x zoomed out is the whole part.
            const auto width = juce::jmax(1.0f, x2 - x1);

            // A busy stem carries thousands of these, and a playhead-strip
            // repaint has room for a handful of them.
            if (x1 > clip.getRight() || x1 + width < clip.getX())
                continue;

            const auto fromTop = static_cast<float>(highest - note.pitch) / span;
            const auto y = band.getY() + fromTop * (band.getHeight() - rowHeight);

            g.fillRoundedRectangle(x1, y, width, rowHeight,
                                   juce::jmin(1.5f, rowHeight * 0.5f));
        }
    }

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

            /*  Snapped as it is drawn, through the same call that stores it
                on release: the highlight has to show where the loop will
                land, not where the pointer happens to be. Without this the
                range visibly jumps the moment the button comes up.
            */
            if (show)
            {
                const auto snapped = processor.quantizeLoopRange({from, to});

                from = snapped.start;
                to = snapped.end;
            }
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
                g.setColour(theme::colors::accent().withAlpha(0.16f));
                g.fillRect(clippedLeft, inner.getY(), clippedRight - clippedLeft,
                           inner.getHeight());

                g.setColour(theme::colors::accent().withAlpha(0.75f));

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
        g.setColour(theme::colors::playheadGlow());
        g.fillRect(playheadX - lanes::playheadGlowWidth * 0.5f, inner.getY(),
                   lanes::playheadGlowWidth, inner.getHeight());

        g.setColour(theme::colors::playhead());
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
        now.palette == lastDisplay.palette && now.accent == lastDisplay.accent &&
        juce::exactlyEqual(now.gridBpm, lastDisplay.gridBpm) &&
        juce::exactlyEqual(now.gridBarOne, lastDisplay.gridBarOne) &&
        now.gridNumerator == lastDisplay.gridNumerator &&
        now.gridDenominator == lastDisplay.gridDenominator &&
        now.useDetectedBeats == lastDisplay.useDetectedBeats &&
        now.beatRevision == lastDisplay.beatRevision &&
        // The transcription is part of the picture too. Without this a
        // conversion landing, or a re-conversion, changed only what the next
        // paint would draw - and nothing asked for one, so the new notes
        // appeared a strip at a time behind the playhead, or not at all on a
        // stopped transport.
        now.midiNoteCount == lastDisplay.midiNoteCount &&
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

        // geometry.start, matching paint: this decides which strip to
        // invalidate, so an origin paint does not use would repaint pixels
        // beside the playhead and leave the ones under it stale.
        return geometry.inner.getX() +
               static_cast<float>((normalised * profile->lengthSeconds - geometry.start) /
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

        /*  A sweep that collapses to nothing clears rather than storing an
            empty loop the transport would sit inside forever. Measured on
            the raw sweep, before any snapping: quantise opens a short range
            up to one unit, so testing the snapped one would turn a stray
            click into a loop instead of clearing.
        */
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

    /*
     * A well is enabled as soon as the job is done and its file is on disk,
     * which is before the cache has finished analysing it. Without a profile
     * normalisedForX has no length to map the pointer into and answers 0, so
     * a click anywhere in a still-blank well threw the shared transport - and
     * every other lane with it - back to 0:00. The same test normalisedForX
     * makes, so a click it could only answer with a lie does nothing instead.
     */
    if (profile == nullptr || !(profile->lengthSeconds > 0.0))
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
     * The price: the four events the relay carries now fire for every
     * descendant's events, and twice for the lane's own. Keep them all
     * idempotent. The relay is what keeps the wheel out of that bargain -
     * see DescendantMouseRelay.
     */
    addMouseListener(&descendantMouseRelay, true);

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
    nameLabel.setColour(juce::Label::textColourId, theme::colors::text());
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

    midiDragButton = std::make_unique<widgets::IconButton>(
        "drag-midi", [](juce::Rectangle<float> b) { return stemlab::icons::midiDragOut(b); },
        static_cast<float>(theme::metrics::lanes::layersIcon), true,
        theme::metrics::lanes::smRadius, false);

    midiDragButton->setTooltip("Drag this stem's MIDI to a DAW or a folder");
    midiDragButton->setMouseCursor(juce::MouseCursor::DraggingHandCursor);

    midiDragButton->onClick = [this]
    {
        processor.postUiStatus("Drag this button onto a DAW track or a folder");
    };

    // Hidden until there is a .mid to carry; refresh() owns that from here.
    midiDragButton->setVisible(false);
    addChildComponent(*midiDragButton);

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

        // A child lane carries its root's identity color, so a split stem
        // still reads as one family down the tree.
        waveform->setStemIdentity(info.rootStem);
        waveform->setSelectionId(info.id);
    }

    /*
     * No refresh() from here. syncLanes() calls this for every child lane on
     * every UI tick and refreshFromProcessor() then walks the same lanes and
     * refreshes them, so a trailing refresh here made each child lane pay for
     * two - two rounds of processor queries and, because a child lane is the
     * one kind that has to stat its own file, two stats per lane per tick.
     * Every path that reaches syncLanes() ends in refreshFromProcessor(), so
     * the lane is still brought up to date inside the same call.
     */
}

void StemLaneComponent::mouseDrag(const juce::MouseEvent& event)
{
    /*
     * The drag handle's own drags reach the lane, because a juce::Button
     * does not forward them anywhere useful. Starting the external drag from
     * here also means the whole lane is a valid drag source once the gesture
     * has begun, rather than the pointer having to stay inside 22 pixels.
     */
    if (externalDragStarted || dragButton == nullptr)
        return;

    const bool fromAudio = event.eventComponent == dragButton.get() && dragButton->isEnabled();

    const bool fromMidi = midiDragButton != nullptr &&
                          event.eventComponent == midiDragButton.get() &&
                          midiDragButton->isVisible() && midiDragButton->isEnabled();

    if (!fromAudio && !fromMidi)
        return;

    if (event.getDistanceFromDragStart() < theme::metrics::waveform::clickVersusDragThreshold)
        return;

    const auto selectionId =
        isChildLane() ? childId : StemLabAudioProcessor::getStemName(stemIndex);

    juce::File dragFile;

    if (fromMidi)
    {
        /*
         * The job's own midi/ copy, never the Engine's managed MidiDrag one.
         * That directory is a cache and StemLab's own housekeeping deletes
         * from it after seven days (midi.cleanup_stale_midi_drag_files), so
         * a project holding a path into it loses its notes in a week. The
         * job directory is permanent - that is what jobsDirectory now
         * guarantees - and it is the same place the audio handle beside this
         * one drags from, so a dropped pair lands from one folder.
         */
        const auto info = processor.getMidiInfo(selectionId);

        dragFile = info.midiFile;
    }
    else
    {
        if (!laneFile.existsAsFile())
            return;

        // A highlighted range means this drag carries that range, not the
        // whole stem - the same rule the footer's Drag Stems pill follows.
        // Ranges are an audio idea; the MIDI handle above always carries the
        // whole conversion, which is what the notes in it describe.
        dragFile = processor.getStemDragFile(laneFile, selectionId);
    }

    if (!dragFile.existsAsFile())
    {
        if (fromMidi)
            processor.postUiStatus("That MIDI file is no longer on disk");

        return;
    }

    externalDragStarted = juce::DragAndDropContainer::performExternalDragDropOfFiles(
        juce::StringArray{dragFile.getFullPathName()}, false, this);
}

void StemLaneComponent::mouseUp(const juce::MouseEvent&)
{
    externalDragStarted = false;
}

void StemLaneComponent::mouseEnter(const juce::MouseEvent&) { updateHover(); }
void StemLaneComponent::mouseExit(const juce::MouseEvent&) { updateHover(); }

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
     * The MIDI handle appears the moment a conversion lands and goes away
     * again if the lane is reused for a stem that has none. Laying the row
     * out is what makes room for it, so that only happens when the answer
     * actually changes - refresh runs on every UI tick.
     */
    const auto midiId = isChildLane() ? childId : StemLabAudioProcessor::getStemName(stemIndex);
    const bool haveMidi = ready && processor.hasMidiInfo(midiId);

    if (haveMidi != midiHandleShown)
    {
        midiHandleShown = haveMidi;
        midiDragButton->setVisible(haveMidi);
        resized();
    }

    midiDragButton->setEnabled(haveMidi);

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

    // Beside the audio handle, and only when there is MIDI to drag: an
    // always-reserved gap would leave a hole in every lane that has not
    // been converted, which is most of them most of the time.
    if (midiHandleShown)
    {
        row.removeFromLeft(lanes::smGap);

        midiDragButton->setBounds(row.removeFromLeft(lanes::smButton)
                                      .withSizeKeepingCentre(lanes::smButton, lanes::smButton));
    }

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
        g.setColour(theme::colors::rowHoverFill());
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

        g.setColour(hiddenDescendantSoloed ? theme::colors::accent()
                                           : theme::colors::text75());

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

    panelContent.addChildComponent(settingsPanel);

    settingsPanel.onClose = [this] { closeSettingsPanel(); };

    wireSettingsPage();

    settingsPanel.models().onCompileEnabled = [this](bool enabled)
    { processor.setTorchCompileEnabled(enabled); };

    settingsPanel.models().onCancel = [this] { processor.cancelModelJob(); };

    /*
     * Each of these marks the status line worth showing from here on.
     * Watching isModelJobRunning instead would miss the short ones outright:
     * clearing a cache, or a compile refused because compiling is off,
     * begins and ends between two refreshes, and its outcome would land only
     * on the footer behind the scrim where it cannot be read.
     */
    settingsPanel.models().onDownload = [this](juce::StringArray ids)
    {
        modelJobReported = true;
        processor.startModelDownload(ids);
    };

    settingsPanel.models().onCompile = [this](juce::StringArray ids)
    {
        modelJobReported = true;
        processor.startModelCompile(ids);
    };

    settingsPanel.models().onRemove = [this](juce::StringArray modelIds, juce::StringArray cacheIds)
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

        // Read before the window exists, because the title-bar swap below
        // makes resized() overwrite it - which is the very thing the resize
        // at the end of that callback undoes.
        const auto openScalePercent = processor.getEditorScalePercent();

        juce::MessageManager::callAsync(
            [safeThis, openScalePercent]
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
                     * color and the surplus reads as margin rather than as a
                     * black band torn out of the panel.
                     */
                    windowComponent->setBackgroundColour(theme::colors::ground());

                    windowComponent->setUsingNativeTitleBar(true);
                    windowComponent->setName("StemLab");

                    /*
                     * The taskbar icon, and on X11 the only place it can come
                     * from. A Linux executable has nowhere to carry an icon
                     * the way a .exe or an .app bundle does, so a window that
                     * publishes no _NET_WM_ICON gets whatever placeholder the
                     * desktop draws for an unknown application - which is
                     * what StemLab showed.
                     *
                     * The peer's setIcon, not DocumentWindow::setIcon: that
                     * one only sets the image JUCE paints into its own title
                     * bar, which a window using the native one never draws.
                     * Set after the title bar swap because that recreates the
                     * peer, and the icon lives on the peer.
                     *
                     * The desktop entry scripts/linux/build.sh installs
                     * carries the same artwork, and matches this window by
                     * its WM_CLASS - JUCE sets that from the application
                     * name, "StemLab", which is what StartupWMClass says.
                     */
                    if (auto* peer = windowComponent->getPeer())
                    {
                        const auto icon = juce::ImageCache::getFromMemory(
                            BinaryData::stemlab256_png, BinaryData::stemlab256_pngSize);

                        if (icon.isValid())
                            peer->setIcon(icon);
                    }

                    /*
                     * Ask for the size again, because the swap above did not
                     * give the window back.
                     *
                     * The standalone window is built with JUCE's own title
                     * bar and border, so it is a decoration larger than the
                     * editor asked for. Swapping to the native title bar
                     * keeps those outer bounds and moves the decoration out
                     * of them, and the content component hands the surplus -
                     * eight pixels of width here - straight to this editor.
                     * resized() then reads a scale nobody chose out of a
                     * width nobody asked for and remembers it, so every
                     * launch opened one percent larger than the one before:
                     * 888, 897, 906, 914 ... measured from a fresh config.
                     *
                     * Setting the size the editor actually wants refits the
                     * window around it, because the standalone's content
                     * component sizes itself to whatever the editor does.
                     * openScalePercent is the value from before the swap;
                     * reading it here would read the inflated one back.
                     */
                    const auto openScale = juce::jlimit(window::minScale, window::maxScale,
                                                        openScalePercent / 100.0);

                    safeThis->setSize(juce::roundToInt(window::width * openScale),
                                      juce::roundToInt(window::height * openScale));
                }
            });
    }

    // ------------------------------------------------------------- header

    titleLabel.setText("StemLab", juce::dontSendNotification);
    titleLabel.setFont(
        juce::Font(theme::fonts::title()).withExtraKerningFactor(theme::fonts::titleKerning));
    titleLabel.setColour(juce::Label::textColourId, theme::colors::text());
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

    settingsButton = std::make_unique<widgets::IconButton>(
        "settings", [](juce::Rectangle<float> b) { return stemlab::icons::sliders(b); },
        static_cast<float>(theme::metrics::header::settingsIcon), false,
        theme::metrics::header::settingsRadius, true, true);

    settingsButton->setTooltip("Settings");

    // The gear opens the settings window, not a menu: everything the old menu
    // held is a page in it, and a setting you can see beats one you have to go
    // looking for.
    settingsButton->onClick = [this]
    { showSettingsPanel(stemlab::widgets::SettingsPanel::Page::settings); };
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
    userStatusLabel.setColour(juce::Label::textColourId, theme::colors::text50());
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
    zoomLabel.setColour(juce::Label::textColourId, theme::colors::text50());
    zoomLabel.setJustificationType(juce::Justification::centredLeft);
    panelContent.addAndMakeVisible(zoomLabel);

    // -------------------------------------------------------- source strip

    fileNameLabel.setFont(theme::fonts::bodyMedium());
    fileNameLabel.setColour(juce::Label::textColourId, theme::colors::text());
    panelContent.addAndMakeVisible(fileNameLabel);

    fileMetaLabel.setFont(theme::fonts::meta());
    fileMetaLabel.setColour(juce::Label::textColourId, theme::colors::text50());
    panelContent.addAndMakeVisible(fileMetaLabel);

    hostTempoButton.onClick = [this] { setHostTempo(); };
    panelContent.addChildComponent(hostTempoButton);

    analyseButton.onClick = [this]
    {
        // Stop what is running; otherwise start, which is what switching the
        // setting on does for a source that has not been analysed yet.
        processor.setBeatThisEnabled(!processor.isSourceAnalysisRunning());
        refreshFromProcessor();
    };
    panelContent.addChildComponent(analyseButton);

    // One label whatever the tempo mode is. Static and dynamic do differ -
    // one number against a whole tempo map - but the switch that chooses
    // between them is two clicks away in Settings, and a button that renames
    // itself when a setting elsewhere changes costs more attention than the
    // distinction is worth on a line this narrow.
    hostTempoButton.setButtonText("Set BPM");

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
        rootLanes[static_cast<size_t>(i)] = makeLane(i, {});
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
    timeLabel.setColour(juce::Label::textColourId, theme::colors::text75());
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
    statusLabel.setColour(juce::Label::textColourId, theme::colors::text50());
    panelContent.addAndMakeVisible(statusLabel);

    panelContent.addAndMakeVisible(progressBar);

    progressLabel.setFont(theme::fonts::progress());
    progressLabel.setColour(juce::Label::textColourId, theme::colors::text45());
    progressLabel.setJustificationType(juce::Justification::centredLeft);
    panelContent.addAndMakeVisible(progressLabel);

    pathLabel.setFont(theme::fonts::footerPath());
    pathLabel.setColour(juce::Label::textColourId, theme::colors::text50());
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
    g.fillAll(theme::colors::surface());
}

void StemLabAudioProcessorEditor::paintPanel(juce::Graphics& g)
{
    g.fillAll(theme::colors::ground());

    // The surface is the window: no inset, no corners, nothing behind it to
    // cast a shadow onto. Only the drag signal draws an edge.
    g.setColour(theme::colors::surface());
    g.fillRect(panelBounds);

    if (dragActive)
    {
        g.setColour(theme::colors::accent());
        g.drawRect(panelBounds.toFloat().reduced(1.0f), 2.0f);
    }

    // Brand glyph.
    g.setColour(theme::colors::accent());
    g.fillPath(stemlab::icons::waveformBars(brandGlyphBounds.toFloat()));

    // Recessed source strip.
    g.setColour(theme::colors::ground());
    g.fillRoundedRectangle(sourceStripBounds.toFloat(), theme::metrics::source::radius);

    if (!sourceDividerBounds.isEmpty() || !sourceInlineDividerBounds.isEmpty())
    {
        g.setColour(theme::colors::divider());

        if (!sourceDividerBounds.isEmpty())
            g.fillRect(sourceDividerBounds);

        if (!sourceInlineDividerBounds.isEmpty())
            g.fillRect(sourceInlineDividerBounds);
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
        g.setColour(theme::colors::accentTint10());
        g.fillRect(panelBounds);

        g.setColour(theme::colors::text());
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

        juce::DropShadow(theme::colors::accentGlow(), radius, {})
            .drawForRectangle(glow, {margin, margin, area.getWidth(), area.getHeight()});
    }

    // Images draw at the current color's opacity; the glow must not.
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

    /*
        Only when the window itself moved. Cached glows are keyed by the size
        of the button they sit behind, so a relayout at the same window size
        asks for the very keys the cache already holds - and emptying it
        anyway made the next paint re-run the Gaussian blurs it exists to
        avoid. It is still emptied here rather than never, because a glow for
        a size nothing draws any more would only pile up.
    */
    if (getWidth() != glowCacheWidth || getHeight() != glowCacheHeight)
    {
        glowCacheWidth = getWidth();
        glowCacheHeight = getHeight();
        glowCache.clear();
    }

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
    settingsPanel.setBounds(panelContent.getLocalBounds());
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

    // Header, right to left: the settings icon, the model selector, the zoom
    // group, and the lane selection group. The brand glyph and title take the
    // left, and the title absorbs whatever is left over between them.
    auto headerRow = inner.removeFromTop(header::settingsButton);

    settingsButton->setBounds(headerRow.removeFromRight(header::settingsButton));

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

        auto separateArea = strip.removeFromRight(juce::jmax(
            source::separateMinWidth, strip.getWidth() / source::separateWidthDivisor));

        separateControl.setBounds(
            separateArea.withSizeKeepingCentre(separateArea.getWidth(),
                                               source::separateHeight));

        /*  Right to left, the strip is three groups of controls and the
            source's own name:

                name  [Analyse][Set BPM] | [Import from DAW][Record PC] | [Refine|Separate]

            The first pair acts on the source named beside it, the second
            picks a different source, and the last consumes it. Members of a
            group are groupButtonGap apart; a hairline centred in
            dividerSpan stands between groups. Only the name is elastic:
            everything else takes the width it needs and the name keeps the
            rest, because it is the one thing here whose length StemLab does
            not choose.
        */
        sourceDividerBounds =
            strip.removeFromRight(source::dividerSpan)
                .withSizeKeepingCentre(source::dividerWidth, source::dividerHeight);

        if (recordInputButton.isVisible())
        {
            recordInputButton.setBounds(
                strip.removeFromRight(source::recordButtonWidth)
                    .withSizeKeepingCentre(source::recordButtonWidth,
                                           theme::metrics::buttons::height));

            strip.removeFromRight(source::groupButtonGap);
        }

        if (recordSystemButton.isVisible())
        {
            recordSystemButton.setBounds(
                strip.removeFromRight(source::recordButtonWidth)
                    .withSizeKeepingCentre(source::recordButtonWidth,
                                           theme::metrics::buttons::height));

            strip.removeFromRight(source::groupButtonGap);
        }

        captureButton.setBounds(
            strip.removeFromRight(source::captureButtonWidth)
                .withSizeKeepingCentre(source::captureButtonWidth,
                                       theme::metrics::buttons::height));

        /*  Analyse and Set BPM take from the right of the whole strip, and
            the two lines of text keep whatever is left. Taking from the left
            would push the text along as Analyse became Stop and back, and a
            line that shifts while a job runs reads as a glitch.

            From the whole strip, not from the meta line: the buttons are
            taller than one of the two lines, so a rect taken from the meta
            row alone would centre them over its edges and let them cover the
            name above. Taking the full height first keeps them inside the
            strip and leaves the text a column of its own.
        */
        const auto buttonWidth = [](const juce::TextButton& button)
        {
            return juce::jlimit(
                source::inlineButtonMinWidth, source::inlineButtonMaxWidth,
                juce::roundToInt(juce::GlyphArrangement::getStringWidth(
                    juce::Font(theme::fonts::meta()), button.getButtonText()))
                    + source::inlineButtonPadX);
        };

        sourceInlineDividerBounds = {};

        /*  The hairline is only worth its space if something will stand on
            the far side of it, so the room for one button is checked before
            it is reserved rather than after - a divider with nothing behind
            it would read as a dangling stroke in the middle of the card.

            metaMinWidth is the floor the name keeps out of all of this: past
            it the buttons give way instead, and vanish rather than being
            drawn as slivers too narrow to read.
        */
        const auto inlineButtons = {&hostTempoButton, &analyseButton};

        auto wantsInlineButton = false;

        for (auto* button : inlineButtons)
            wantsInlineButton = wantsInlineButton || button->isVisible();

        const auto inlineRoom =
            strip.getWidth() - source::metaMinWidth - source::dividerSpan - source::gap;

        if (wantsInlineButton && inlineRoom >= source::inlineButtonMinWidth)
            sourceInlineDividerBounds =
                strip.removeFromRight(source::dividerSpan)
                    .withSizeKeepingCentre(source::dividerWidth, source::dividerHeight);

        auto placedInlineButton = false;

        for (auto* button : inlineButtons)
        {
            if (!button->isVisible() || sourceInlineDividerBounds.isEmpty())
            {
                button->setBounds({});
                continue;
            }

            if (placedInlineButton)
                strip.removeFromRight(source::groupButtonGap);

            /*  Never wider than the strip still holds. removeFromRight
                clamps its own result, but withSizeKeepingCentre would then
                grow it back to the width that did not fit and hang it off
                the left of the card - which is exactly what a narrow window
                used to do.
            */
            const auto room = strip.getWidth() - source::metaMinWidth;
            const auto width = juce::jmin(buttonWidth(*button), room);

            if (width < source::inlineButtonMinWidth)
            {
                button->setBounds({});
                continue;
            }

            button->setBounds(strip.removeFromRight(width).withSizeKeepingCentre(
                width, theme::metrics::buttons::height));

            placedInlineButton = true;
        }

        // The strip's own gap between the pair and the text they act on:
        // wider than the gap within the pair, narrower than a divider.
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

std::unique_ptr<StemLaneComponent> StemLabAudioProcessorEditor::makeLane(int stemIndex,
                                                                        juce::String childId)
{
    auto lane = std::make_unique<StemLaneComponent>(
        processor, stemIndex, std::move(childId), waveformProfiles,
        [this] { refreshFromProcessor(); },
        [this](int index) { showRootLayersMenu(index); },
        [this](const juce::String& id) { showChildLayersMenu(id); },
        [this](int index, juce::String id) { toggleLaneExpanded(index, id); });

    lane->setZoomStepHandler([this](int delta) { stepWaveformZoom(delta); });
    lane->setTransportSeekHandler([this] { handleTransportMoved(); });

    return lane;
}

std::vector<StemLabRecursiveStemInfo> StemLabAudioProcessorEditor::getVisibleRecursiveItems(
    const std::vector<StemLabRecursiveStemInfo>& all) const
{
    std::vector<StemLabRecursiveStemInfo> visible;
    visible.reserve(all.size());

    for (const auto& item : all)
    {
        const auto rootIndex = rootIndexForStemName(item.rootStem);

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

std::vector<StemLabRecursiveStemInfo> StemLabAudioProcessorEditor::syncLanes()
{
    // One fetch for the whole tick: getRecursiveStemItems takes a lock,
    // copies the vector and re-orders it, and this runs at 20 Hz. It is
    // handed back to the caller rather than re-fetched further down the
    // refresh, which is what the promise above used to mean and did not.
    // Not const, so handing it back is a move rather than a second copy.
    auto all = processor.getRecursiveStemItems();

    /*
     * Collapse state is the editor's own, and it outlived the tree it
     * describes: a new job clears the processor's items but the ids the next
     * job builds are made from the same stem names, so a row collapsed
     * before a re-split came back already hidden, with no twisty on screen
     * yet to say why. An empty tree is the one moment that state can be
     * dropped without losing a collapse anyone still cares about - with no
     * children there is nothing collapsed and nothing to collapse.
     */
    if (all.empty())
    {
        collapsedRecursiveIds.clearQuick();
        rootExpanded.fill(true);
    }

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
            const auto rootIndex = rootIndexForStemName(item.rootStem);

            if (rootIndex >= 0)
                owner = StemLabAudioProcessor::getStemName(rootIndex);
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
            auto lane = makeLane(-1, item.id);

            lane->setChildInfo(item);
            lane->setChildState(item.hasChildren, isLaneExpanded(-1, item.id),
                                hiddenActiveParents.contains(item.id),
                                hiddenSoloParents.contains(item.id));
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

    return all;
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
     * letterbox band paint() fills with surface(), the same color the menu
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
        processor.refreshAbletonTempoReplyFromDisk();
        processor.refreshAbletonMidiAckFromDisk();

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
    refreshSettingsPanel();

    const auto capturing = processor.isCapturing();
    const auto recordingMode = processor.getStandaloneRecordingMode();
    const auto engineRunning = processor.isEngineRunning();
    const auto captureFile = processor.getCaptureFile();
    const auto captureExists = captureFile.existsAsFile();
    const auto jobDone = processor.hasSuccessfulJob();
    const auto nowMs = juce::Time::getMillisecondCounter();

    /*
     * The stem tree, once, for the whole refresh. It cost a lock, a copy and
     * a quadratic re-ordering per ask, and this function used to ask nine
     * times a tick - once here, once for the selection counts, once per root
     * lane to find out whether it had children, and once more for a count the
     * selection counts had already worked out.
     */
    const auto recursiveItems = syncLanes();

    // ------------------------------------------------------------- header

    /*
     * The lanes only carry stems once a job has produced them, so both
     * pills and the readout stay dark until then - "0 of 6 stems will be
     * saved" over six empty lanes is noise, not information.
     */
    const auto [includedLanes, totalLanes] = laneSelectionCounts(recursiveItems);

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

    // The readout dims through its text color rather than component alpha:
    // 50% text at 45% alpha is 1.9:1 against the panel, and the floor in
    // dimDisabled only applies to a color. Label::colourChanged repaints
    // only when the value actually moves, so this is free on most ticks.
    zoomLabel.setColour(juce::Label::textColourId,
                        theme::colors::dimIfDisabled(theme::colors::text50(), haveWaveform));

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

    const auto awaitingClip = processor.isAwaitingAbletonSourceClip();

    /*
     * Analyse and Set BPM act on the source named beside them, so they come
     * and go with it. Both were made visible inside the "a source is loaded"
     * branch below and never hidden again, which left the pair standing over
     * "No audio loaded" once a source had been unloaded or a capture started
     * over it - Analyse offering to analyse nothing. Decided once, above the
     * chain, so every branch has an answer.
     */
    const bool sourceControlsVisible = captureExists && !capturing && !awaitingClip;

    /*  Only where there is an API that can actually write the tempo.
        The standalone has no host at all, and a plain VST3 host has no
        way to be told a tempo - so in both the button could never do
        anything, and a control that is permanently dead is worse than
        no control. Where it can work but has nothing to write yet, it
        is greyed instead (below).
    */
    const auto tempoHost =
        !processor.isStandaloneApp()
        && (processor.getHostIntegration() == StemLabAudioProcessor::hostIntegrationReaper
            || processor.getHostIntegration()
                   == StemLabAudioProcessor::hostIntegrationAbletonLive);

    const bool showHostTempoButton = sourceControlsVisible && tempoHost;

    /*  isSourceAnalysisRunning, not isBeatThisEnabled. The second is the
        setting - analysis is switched on for this source - and it stays
        true once the analysis has finished, which is why the old row read
        "Stop" forever after the first run. The first is the job.
    */
    const auto analysing = processor.isSourceAnalysisRunning();

    const juce::String analyseText{analysing ? "Stop" : "Analyse"};

    /*
     * The source strip's layout depends on exactly these three: which of the
     * pair is on screen, and how wide the Analyse pill's own text makes it
     * (layoutPanel measures the text to size the pill). So a relayout is
     * worth its cost only when one of them moves.
     *
     * It used to run on every tick that had a source loaded, which is twenty
     * full editor layouts a second: about a hundred components repositioned,
     * both header labels re-shaped, and glowCache emptied - so every paint
     * that followed re-ran the DropShadow blurs the cache exists to avoid.
     * All of it for a strip that only changes when somebody clicks something.
     */
    const bool sourceStripMoved = analyseButton.isVisible() != sourceControlsVisible ||
                                  hostTempoButton.isVisible() != showHostTempoButton ||
                                  analyseText != lastAnalyseButtonText;

    lastAnalyseButtonText = analyseText;

    // Out of the settings window and onto the line it describes: Analyse acts
    // on this source, and it is the one control there anybody reaches for
    // repeatedly.
    analyseButton.setVisible(sourceControlsVisible);
    analyseButton.setButtonText(analyseText);
    analyseButton.setEnabled(analysing || !engineRunning);

    hostTempoButton.setVisible(showHostTempoButton);

    /*  Greyed rather than hidden without an analysis: the button is where the
        tempo goes, and hiding it would leave no sign that it is. Only asked
        for while it is on screen, because canSetHostTempo takes the
        processor's state lock to look at what the analysis found.
    */
    if (showHostTempoButton)
        hostTempoButton.setEnabled(processor.canSetHostTempo());

    if (sourceStripMoved)
        resized();

    if (awaitingClip)
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
        const auto available =
            fileNameLabel.getWidth() - fileNameLabel.getBorderSize().getLeftAndRight();

        /*  Measured only when there is something new to measure, the same way
            the footer path below is. Constructing a Font and shaping a whole
            name through GlyphArrangement is not free, and this runs on every
            refresh for two values that move when a different source is loaded
            or the window is resized - which is to say almost never.
        */
        if (fileName != lastFileNameMeasured || available != lastFileNameLabelWidth)
        {
            lastFileNameMeasured = fileName;
            lastFileNameLabelWidth = available;

            const juce::Font nameFont{theme::fonts::bodyMedium()};

            lastFileNameClipped =
                available > 0 && juce::GlyphArrangement::getStringWidth(nameFont, fileName) >
                                     static_cast<float>(available);
        }

        fileNameLabel.setTooltip(lastFileNameClipped && captureExists
                                     ? captureFile.getFullPathName()
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

    /*  Which roots have adaptive children, from the tree this tick already
        holds. rootHasChildren() answers the same question by fetching the
        whole tree again, once per root, and the loop below asks it six times.
    */
    std::array<bool, StemLabAudioProcessor::stemCount> rootHasChildLanes{};

    for (const auto& item : recursiveItems)
        if (const auto rootIndex = rootIndexForStemName(item.rootStem); rootIndex >= 0)
            rootHasChildLanes[static_cast<size_t>(rootIndex)] = true;

    for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
    {
        if (auto* lane = rootLanes[static_cast<size_t>(i)].get())
        {
            const bool hasChildren = rootHasChildLanes[static_cast<size_t>(i)];
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
     * the main job's success summary in the failure color beside a red
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
                                                             ? theme::colors::statusError()
                                                             : theme::colors::text50());
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

    /*  One resolution for the refresh. getJobRootDirectory takes the
        processor's state lock and stats its directory, and on Linux the
        fallback it returns re-reads ~/.config/user-dirs.dirs to find out
        where the user's Music folder is - twice a tick, for a path that
        changes when somebody picks a different job folder.
    */
    const auto jobRoot = processor.getJobRootDirectory();

    const auto jobPath = displayPath(jobRoot);

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
        openFolderButton->setEnabled(jobRoot.isDirectory());
    }

    changeFolderButton.setEnabled(!engineRunning);

    // includedLanes, not a second count of its own: laneSelectionCounts works
    // out exactly this above, over the same roots and the same children.
    saveButton.setEnabled(jobDone && !engineRunning && !capturing && includedLanes > 0);
    insertButton.setEnabled(jobDone && !engineRunning && !capturing && includedLanes > 0);
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
     * Handing the chooser the file itself rather than its folder both
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

std::pair<int, int> StemLabAudioProcessorEditor::laneSelectionCounts(
    const std::vector<StemLabRecursiveStemInfo>& all) const
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
     *
     * Straight off the passed-in tree: item.selected is the very field
     * isRecursiveStemEnabled looks the id up to return, so asking would be
     * one lock and one linear search per child for an answer already in hand.
     */
    for (const auto& item : all)
    {
        ++total;

        if (item.selected)
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

namespace
{
    /*  The settings page's palette order mapped onto the theme's indices.
        The page lists Spectrum first because it is the default; the theme
        numbers them in the order they were added, with the accent look at 0.
        One table, read in both directions, so the two cannot drift apart.
    */
    constexpr int waveformPalettePages[] = {2, 3, 4, 1, 0};

    /*  The same arrangement for the two rows below, and for the same reason:
        wireSettingsPage reads them forwards to turn a click into a processor
        value, refreshSettingsPage reads them backwards to put the processor's
        value back on the page. Held once so a row reordered on the page
        cannot leave the two halves disagreeing about what index three means.
    */
    constexpr int gridModePages[] = {StemLabAudioProcessor::gridHost,
                                     StemLabAudioProcessor::gridSource,
                                     StemLabAudioProcessor::gridManual,
                                     StemLabAudioProcessor::gridOff};

    constexpr int tempoReadingPages[] = {StemLabAudioProcessor::tempoHalf,
                                         StemLabAudioProcessor::tempoDetected,
                                         StemLabAudioProcessor::tempoDouble};
}

void StemLabAudioProcessorEditor::wireSettingsPage()
{
    /*
     * Every index the page reports is translated here rather than passed
     * through. The page numbers what it draws, from zero, in reading order;
     * the processor numbers what it means. Those two agree today for the beat
     * grid and the tempo reading, and disagree for analysis quality, where
     * accurate is 0 and fast is 1. Passing the index straight through would
     * silently invert that one setting, and would invert any other the day an
     * enum is renumbered.
     */
    /*  The accent is the one index that is not translated, because the page
        and the theme number the same list: the swatches are drawn straight
        from theme::accents::presets, in that order.
    */
    settingsPanel.onAccent = [this](int index)
    {
        if (index == StemLabAudioProcessor::getAccentIndex())
            return;

        StemLabAudioProcessor::setAccentIndex(index);

        // Rendered with accentGlow() baked in and keyed only by size, so a
        // repaint alone would draw the old accent's glow around the new
        // accent's button. The lane waveforms have the same problem and solve
        // it by keying their cached image on the accent as well.
        glowCache.clear();

        /*  The editor, not getTopLevelComponent(). In the Standalone those
            are one window and either works; inside a host the top level is
            the host's own plug-in window, and asking it to repaint is asking
            somebody else's component to redraw ours. The editor is the
            highest thing we own, and the settings window that was clicked in
            is a child of it, so this covers the swatch's own ring too.

            The lanes do not depend on this reaching them: their timer tick
            now watches the accent as well, so they refresh on their own.
        */
        repaint();

        refreshSettingsPage();

        processor.postUiStatus("Accent: "
                               + stemlab::theme::accents::name(
                                   StemLabAudioProcessor::getAccentIndex()));
    };

    /*  Translated, like every other index here. The page lists the palettes
        in the order the header menu used - Spectrum first, because it is the
        default - and the theme numbers them in the order they were added.
    */
    settingsPanel.onWaveformPalette = [this](int index)
    {
        static_assert(StemLabAudioProcessor::waveformColorCount
                          == theme::waveform::paletteCount,
                      "The remembered waveform-palette range and the theme's "
                      "list must stay in step");

        if (!juce::isPositiveAndBelow(index,
                                      juce::numElementsInArray(waveformPalettePages)))
            return;

        const auto palette = waveformPalettePages[index];

        processor.setWaveformColorIndex(palette);

        processor.postUiStatus("Waveform: " + theme::waveform::paletteName(palette));

        refreshFromProcessor();
    };

    settingsPanel.onLoopQuantize = [this](int index)
    {
        static constexpr int modes[] = {StemLabAudioProcessor::quantizeOff,
                                        StemLabAudioProcessor::quantizeQuarterBeat,
                                        StemLabAudioProcessor::quantizeHalfBeat,
                                        StemLabAudioProcessor::quantizeBeat,
                                        StemLabAudioProcessor::quantizeBar};

        if (!juce::isPositiveAndBelow(index, juce::numElementsInArray(modes)))
            return;

        processor.setLoopQuantizeMode(modes[index]);

        // Named rather than left to be discovered by sweeping: the setting
        // only shows itself the next time a loop is drawn.
        static constexpr const char* names[] = {"Loop snapping off",
                                                "Loops snap to 1/4 beat",
                                                "Loops snap to 1/2 beat",
                                                "Loops snap to the beat",
                                                "Loops snap to the bar"};

        if (index > 0 && !processor.canQuantizeLoops())
            processor.postUiStatus("Loops will snap once the grid has a tempo");
        else
            processor.postUiStatus(names[index]);

        refreshFromProcessor();
    };

    settingsPanel.onGridMode = [this](int index)
    {
        if (!juce::isPositiveAndBelow(index, juce::numElementsInArray(gridModePages)))
            return;

        const auto mode = gridModePages[index];

        processor.setWaveformGridMode(mode);

        if (mode == StemLabAudioProcessor::gridOff)
        {
            processor.postUiStatus("Beat grid off");
        }
        else if (mode == StemLabAudioProcessor::gridSource && processor.getSourceBpm() <= 0.0)
        {
            // The grid draws nothing until an analysis exists, so say that
            // rather than leave an empty lane looking broken.
            processor.postUiStatus(
                "Beat grid follows the analysed source - none yet, so no grid is drawn");
        }
        else
        {
            const juce::StringArray names{"host tempo", "analysed source", "manual tempo"};

            processor.postUiStatus("Beat grid follows " + names[mode]);
        }

        refreshFromProcessor();
    };

    settingsPanel.onSetManualTempo = [this] { promptForManualTempo(); };

    settingsPanel.onAnalysisQuality = [this](int index)
    {
        // The one that does not line up: 0 is Accurate in the processor.
        static constexpr int qualities[] = {StemLabAudioProcessor::analysisFast,
                                            StemLabAudioProcessor::analysisAccurate};

        if (!juce::isPositiveAndBelow(index, juce::numElementsInArray(qualities)))
            return;

        processor.setSourceAnalysisMode(qualities[index]);
        refreshFromProcessor();
    };

    settingsPanel.onTempoInterpretation = [this](int index)
    {
        if (!juce::isPositiveAndBelow(index, juce::numElementsInArray(tempoReadingPages)))
            return;

        processor.setTempoInterpretation(tempoReadingPages[index]);
        refreshFromProcessor();
    };

    settingsPanel.onClearAnalysisCache = [this]
    {
        // No feedback post: the launch already reports the clearing job on
        // the work status line.
        processor.clearAnalysisCache();
        refreshFromProcessor();
    };

    settingsPanel.onFusedNormalise = [this](bool on)
    {
        processor.setFusedStemNormalisation(on);

        processor.postUiStatus(on
                                   ? "Fused stems will be normalised to 0.999 each"
                                   : "Fused stems keep their level and sum back to the source");

        refreshFromProcessor();
    };

    settingsPanel.onTempoMode = [this](int index)
    {
        processor.setTempoAnalysisMode(index == 1 ? StemLabAudioProcessor::tempoDynamic
                                                  : StemLabAudioProcessor::tempoStatic);
        refreshSettingsPage();
    };

    settingsPanel.onCheckUpdates = [this] { checkForUpdates(); };

    settingsPanel.onCopyDiagnostics = [this]
    {
        juce::SystemClipboard::copyTextToClipboard(processor.getEngineLog());

        processor.postUiStatus("Diagnostics copied to clipboard");
    };

    settingsPanel.onAudioSettings = [this] { showStandaloneAudioSettings(); };
    settingsPanel.onAbletonIntegration = [this] { launchAbletonSetup(); };
}

namespace
{
    /** Where a processor value sits in the order the settings page draws. */
    template <size_t N>
    int pageIndexOf(const int (&values)[N], int value)
    {
        for (size_t index = 0; index < N; ++index)
            if (values[index] == value)
                return static_cast<int>(index);

        return 0;
    }
}

void StemLabAudioProcessorEditor::refreshSettingsPage()
{
    stemlab::widgets::SettingsPanel::Settings settings;

    settings.standalone = processor.isStandaloneApp();
    settings.accent = StemLabAudioProcessor::getAccentIndex();

    settings.waveformPalette =
        pageIndexOf(waveformPalettePages, processor.getWaveformColorIndex());

    settings.gridMode = pageIndexOf(gridModePages, processor.getWaveformGridMode());

    settings.loopQuantize = processor.getLoopQuantizeMode();

    /*  Whether a grid exists at all, not whether snapping is switched on:
        the row has to stay live at Off, or there would be no way back to a
        resolution once it was turned off. Detected beats count as a grid
        even with no constant tempo behind them.
    */
    {
        const auto snapshot = processor.getBeatSnapshot();
        const auto grid = processor.getLoopQuantizeGrid(snapshot);

        settings.loopQuantizeAvailable = grid.secondsPerBeat > 0.0 || grid.rulingFromBeats();
    }

    settings.manualBpm = processor.getManualGridBpm();

    settings.analysisQuality =
        processor.getSourceAnalysisMode() == StemLabAudioProcessor::analysisAccurate ? 1 : 0;

    settings.tempoInterpretation =
        pageIndexOf(tempoReadingPages, processor.getTempoInterpretation());

    settings.detectedBpm = processor.getDetectedSourceBpm();
    settings.halfBpm = processor.getHalfTimeSourceBpm();
    settings.doubleBpm = processor.getDoubleTimeSourceBpm();
    settings.tempoAvailable = settings.detectedBpm > 0.0;
    settings.tempoSteady = processor.isSourceTempoSteady();
    settings.tempoMode = processor.getTempoAnalysisMode();

    // Dynamic names what the track actually does; static keeps the single
    // reading a host tempo field takes, even when the track wanders.
    if (settings.tempoMode == StemLabAudioProcessor::tempoDynamic)
    {
        const auto sections = processor.getSourceTempoSegments();

        if (sections.size() > 1)
        {
            juce::StringArray parts;

            for (const auto& section : sections)
                parts.add(juce::String(section.bpm, 1).trimCharactersAtEnd("0.") + " from " +
                          formatSeconds(section.start));

            settings.tempoSections =
                juce::String(sections.size()) + " sections: " + parts.joinIntoString(", ");
        }
    }

    settings.fusedNormalise = processor.isFusedStemNormalisation();
    settings.fusedNormaliseAvailable =
        processor.getSeparatorEngineIndex() == StemLabAudioProcessor::separatorHybrid;

    // Linux only: update.sh is the Linux bundle's updater. The row is hidden
    // everywhere else, and in a build run from a checkout, where there is no
    // install for it to sit beside.
   #if JUCE_LINUX
    settings.updaterAvailable = updaterScript().existsAsFile();
   #else
    settings.updaterAvailable = false;
   #endif

    settings.updateCheckRunning = updateCheckRunning;

    settings.hasDiagnostics = processor.hasEngineLog();

#if JUCE_WINDOWS
    settings.abletonAvailable = processor.isStandaloneApp();
#else
    settings.abletonAvailable = false;
#endif

    settings.version = JucePlugin_VersionString;

    settingsPanel.setSettings(settings);
}

juce::File StemLabAudioProcessorEditor::updaterScript()
{
    return stemlab::paths::userDataDirectory().getChildFile("update.sh");
}

void StemLabAudioProcessorEditor::setHostTempo()
{
    // The processor does the work and says what happened; this only relays
    // it, so the host-specific parts stay on the side that owns the bridge.
    processor.postUiStatus(processor.setHostTempo());
    refreshFromProcessor();
}

void StemLabAudioProcessorEditor::checkForUpdates()
{
    const auto script = updaterScript();

    if (updateCheckRunning || !script.existsAsFile())
        return;

    updateCheckRunning = true;
    refreshSettingsPage();

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);
    const auto path = script.getFullPathName();

    /*
     * Off the message thread, without exception. --check asks github.com which
     * release is newest, and readAllProcessOutput is documented to block until
     * the process finishes: on a network that is down, that is however long
     * curl waits before giving up, with the host's UI frozen behind it.
     */
    juce::Thread::launch(
        [safeThis, path]
        {
            juce::ChildProcess process;
            juce::String output;

            if (process.start(juce::StringArray{path, "--check"}))
                output = process.readAllProcessOutput().trim();
            else
                output = "Could not run " + path;

            juce::MessageManager::callAsync(
                [safeThis, path, output]
                {
                    if (safeThis == nullptr)
                        return;

                    safeThis->updateCheckRunning = false;
                    safeThis->refreshSettingsPage();
                    safeThis->showUpdateCheckResult(path, output);
                });
        });
}

void StemLabAudioProcessorEditor::showUpdateCheckResult(const juce::String& scriptPath,
                                                        const juce::String& output)
{
    /*
     * Reporting only. Installing an update replaces StemLab.vst3, and replacing
     * a plug-in binary underneath a host that has it loaded is how the next
     * scan finds a half-written bundle - so the command is handed over instead
     * of run.
     */
    const auto body =
        (output.isNotEmpty() ? output : juce::String("The updater said nothing.")) +
        "\n\nTo install an update, close your DAW and run:\n" + scriptPath;

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                           "StemLab updates", body, "OK", this);
}

void StemLabAudioProcessorEditor::promptForManualTempo()
{
    // A plain AlertWindow rather than the MessageBoxOptions form used
    // elsewhere in this file: that one cannot carry a text field.
    auto* window = new juce::AlertWindow("Manual Tempo",
                                         "Tempo for the beat grid, in BPM (20 to 400).",
                                         juce::MessageBoxIconType::NoIcon, this);

    window->addTextEditor("bpm", juce::String(processor.getManualGridBpm(), 2), "BPM");
    window->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto safeThis = juce::Component::SafePointer<StemLabAudioProcessorEditor>(this);
    juce::Component::SafePointer<juce::AlertWindow> safeWindow(window);

    // deleteWhenDismissed, and the field is read inside the callback: JUCE
    // runs every modal callback before deleting the component
    // (ModalComponentManager::handleAsyncUpdate), so the editor is still
    // there to be read.
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis, safeWindow](int result)
            {
                if (safeThis == nullptr || safeWindow == nullptr || result != 1)
                    return;

                const auto typed = safeWindow->getTextEditorContents("bpm").trim();
                const auto bpm = typed.getDoubleValue();

                // getDoubleValue answers 0 for anything unparseable, which is
                // also below the range, so one test covers both.
                if (bpm < 20.0 || bpm > 400.0)
                {
                    safeThis->processor.postUiStatus(
                        "Tempo must be between 20 and 400 BPM - grid unchanged");
                    return;
                }

                safeThis->processor.setManualGrid(bpm, 4, 4, 0.0);
                safeThis->processor.setWaveformGridMode(StemLabAudioProcessor::gridManual);

                safeThis->processor.postUiStatus(
                    "Beat grid follows manual tempo, " + formatBpmForDisplay(bpm) + " BPM");
            }),
        true);
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

void StemLabAudioProcessorEditor::showSettingsPanel(
    stemlab::widgets::SettingsPanel::Page page)
{
    modelManagerDismissed = false;

    settingsPanel.setBounds(panelContent.getLocalBounds());
    settingsPanel.showPage(page);
    settingsPanel.setVisible(true);
    settingsPanel.toFront(true);

    refreshSettingsPanel();

    // Ask the engine again on every open. The inventory is cheap, and a user
    // who has just downloaded a model outside StemLab should not be told it
    // is missing because the answer is from when the editor opened.
    processor.refreshModelInventory();
}

void StemLabAudioProcessorEditor::closeSettingsPanel()
{
    modelManagerDismissed = true;
    modelJobReported = false;
    settingsPanel.setVisible(false);

    // Give the keyboard back, or the panel behind stays deaf to shortcuts.
    grabKeyboardFocus();
}

void StemLabAudioProcessorEditor::refreshSettingsPanel()
{
    if (!settingsPanel.isVisible())
        return;

    refreshSettingsPage();

    if (processor.modelInventoryFailed())
    {
        settingsPanel.models().setUnavailable(
            "The StemLab engine could not report its models.\n"
            "Check the engine under Settings, then reopen this.");
    }
    else if (processor.hasModelInventory())
    {
        settingsPanel.models().setInventory(processor.getManagedModels(),
                                       processor.getManagedCaches());
    }
    else
    {
        settingsPanel.models().setUnavailable("Asking the engine what is installed...");
    }

    settingsPanel.models().setCompileState(processor.isTorchCompileEnabled(),
                                          processor.isCompileSupported(),
                                          processor.getCompileReason());

    const auto busy = processor.isModelJobRunning();

    // Once something has been asked for, the status line is the only account
    // of how it went that the user can actually see from here.
    settingsPanel.models().setActivity(
        busy || modelJobReported ? processor.getStatus() : juce::String{},
        processor.getEngineProgress(), busy);
}

void StemLabAudioProcessorEditor::considerAutoShowingModelManager()
{
    // Once per editor, and never over a dismissal. Everything else about
    // when it opens is the engine's answer rather than a rule repeated here.
    if (modelManagerAutoShown || modelManagerDismissed)
        return;

    if (!processor.hasModelInventory() || settingsPanel.isVisible())
        return;

    // Only a missing essential model. Compiling being available but not yet
    // done is not a reason to interrupt anyone - it is a thing to go and do,
    // not a thing that is wrong.
    if (!processor.isEssentialModelMissing())
        return;

    modelManagerAutoShown = true;
    showSettingsPanel(stemlab::widgets::SettingsPanel::Page::models);
}

