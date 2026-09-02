#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

/*
 * Snapping a swept loop range onto the beat grid.
 *
 * The grid here is the one the lanes actually paint, whichever of the two
 * that is: the detected beats when the lane rules from them, and otherwise
 * a uniform rule at barOne + n * secondsPerBeat with a bar every
 * beatsPerBar. The two must not diverge - a loop snapped to a line the lane
 * did not draw would sit visibly off its own gridlines - so this reads the
 * same beats makeGridLines does, and a track whose tempo moves snaps to
 * where its beats actually fell.
 *
 * Ranges arrive and leave normalised (0..1 of the file), because that is how
 * a lane selection is stored; seconds only exist inside. JUCE-free so the
 * whole policy is exercised by a plain test binary.
 */
namespace stemlab::quantize
{

/** Off first so the enum's order is the settings row's order. */
enum class Resolution
{
    off = 0,
    quarterBeat = 1,
    halfBeat = 2,
    beat = 3,
    bar = 4
};

struct Grid
{
    double barOne = 0.0;          // seconds
    double secondsPerBeat = 0.0;  // <= 0 means there is no grid to snap to
    int beatsPerBar = 4;

    /*  When set, and beats is not empty, the analysed positions rule
        instead of the constant tempo above. secondsPerBeat is still read -
        as the fallback interval past either end of the analysed span, where
        there are no measured beats to interpolate between.
    */
    bool useDetectedBeats = false;
    std::span<const double> beats;
    std::span<const double> downbeats;

    /** Whether the analysed positions are the ones to use. */
    bool rulingFromBeats() const
    {
        return useDetectedBeats && !beats.empty();
    }

    /** The list a resolution counts in: downbeats for a bar (when the
        analysis found any), beats for everything else. */
    std::span<const double> anchorsFor(bool wholeBars) const
    {
        if (wholeBars && !downbeats.empty())
            return downbeats;

        return beats;
    }
};

struct Range
{
    double start = 0.0;
    double end = 0.0;
};

/** The snapping step in seconds, or 0 when this grid cannot provide one. */
inline double unitSeconds(const Grid& grid, Resolution resolution)
{
    if (!(grid.secondsPerBeat > 0.0) || resolution == Resolution::off)
        return 0.0;

    switch (resolution)
    {
    case Resolution::quarterBeat:
        return grid.secondsPerBeat * 0.25;
    case Resolution::halfBeat:
        return grid.secondsPerBeat * 0.5;
    case Resolution::beat:
        return grid.secondsPerBeat;
    case Resolution::bar:
        return grid.secondsPerBeat * std::max(1, grid.beatsPerBar);
    case Resolution::off:
    default:
        return 0.0;
    }
}

/** Whether a row of gridlines exists for this setting to snap to at all. */
inline bool canQuantize(const Grid& grid, Resolution resolution)
{
    if (resolution == Resolution::off)
        return false;

    return grid.rulingFromBeats() || unitSeconds(grid, resolution) > 0.0;
}

/** How many snap positions a resolution puts inside one anchor interval. */
inline int subdivisionsPer(Resolution resolution)
{
    switch (resolution)
    {
    case Resolution::quarterBeat:
        return 4;
    case Resolution::halfBeat:
        return 2;
    default:
        // A bar counts in downbeats and a beat counts in beats, so in both
        // cases the anchors themselves are the positions.
        return 1;
    }
}

/**
 * The index of the snap position nearest to a time, counted from anchor
 * zero: whole numbers land on anchors, fractions on the subdivisions
 * between them.
 *
 * Nothing is materialised - a track carries thousands of beats and this
 * runs on every tick of a drag - so the interval is found by binary search
 * and only the two anchors around it are ever touched. Past either end the
 * edge interval is continued, which is what lets a loop swept into the
 * silence before the first beat still land on the rule.
 */
inline double anchorPosition(std::span<const double> anchors, double index, double fallbackStep)
{
    const auto count = static_cast<double>(anchors.size());

    if (anchors.empty())
        return index * fallbackStep;

    if (index <= 0.0)
    {
        const auto step = anchors.size() > 1 ? anchors[1] - anchors[0] : fallbackStep;
        return anchors.front() + index * step;
    }

    if (index >= count - 1.0)
    {
        const auto step = anchors.size() > 1
                              ? anchors[anchors.size() - 1] - anchors[anchors.size() - 2]
                              : fallbackStep;
        return anchors.back() + (index - (count - 1.0)) * step;
    }

    const auto whole = static_cast<std::size_t>(index);
    const auto fraction = index - static_cast<double>(whole);

    return anchors[whole] + fraction * (anchors[whole + 1] - anchors[whole]);
}

/** The fractional anchor index a time sits at - anchorPosition inverted. */
inline double anchorIndexAt(std::span<const double> anchors, double seconds, double fallbackStep)
{
    if (anchors.empty())
        return fallbackStep > 0.0 ? seconds / fallbackStep : 0.0;

    if (seconds <= anchors.front())
    {
        const auto step = anchors.size() > 1 ? anchors[1] - anchors[0] : fallbackStep;
        return step > 0.0 ? (seconds - anchors.front()) / step : 0.0;
    }

    if (seconds >= anchors.back())
    {
        const auto step = anchors.size() > 1
                              ? anchors[anchors.size() - 1] - anchors[anchors.size() - 2]
                              : fallbackStep;
        const auto last = static_cast<double>(anchors.size()) - 1.0;
        return step > 0.0 ? last + (seconds - anchors.back()) / step : last;
    }

    const auto after = std::upper_bound(anchors.begin(), anchors.end(), seconds);
    const auto index = static_cast<std::size_t>(std::distance(anchors.begin(), after)) - 1;

    const auto span = anchors[index + 1] - anchors[index];

    return static_cast<double>(index) + (span > 0.0 ? (seconds - anchors[index]) / span : 0.0);
}

/** The nearest gridline to a time in seconds. */
inline double snapSeconds(double seconds, const Grid& grid, Resolution resolution)
{
    if (resolution == Resolution::off)
        return seconds;

    if (grid.rulingFromBeats())
    {
        const auto anchors = grid.anchorsFor(resolution == Resolution::bar);

        /*  Without downbeats a bar is beatsPerBar beats, so the step
            through the beat list is that many rather than one.
        */
        const auto stride = (resolution == Resolution::bar && grid.downbeats.empty())
                                ? static_cast<double>(std::max(1, grid.beatsPerBar))
                                : 1.0;

        const auto per = static_cast<double>(subdivisionsPer(resolution));

        const auto step = stride / per;

        const auto index = anchorIndexAt(anchors, seconds, grid.secondsPerBeat);

        return anchorPosition(anchors, std::round(index / step) * step, grid.secondsPerBeat);
    }

    const auto unit = unitSeconds(grid, resolution);

    if (!(unit > 0.0))
        return seconds;

    return grid.barOne + std::round((seconds - grid.barOne) / unit) * unit;
}

/** One snap position away from a line, in the direction given. */
inline double stepSeconds(double seconds, const Grid& grid, Resolution resolution, int direction)
{
    if (grid.rulingFromBeats())
    {
        const auto anchors = grid.anchorsFor(resolution == Resolution::bar);

        const auto stride = (resolution == Resolution::bar && grid.downbeats.empty())
                                ? static_cast<double>(std::max(1, grid.beatsPerBar))
                                : 1.0;

        const auto step = stride / static_cast<double>(subdivisionsPer(resolution));

        const auto index = anchorIndexAt(anchors, seconds, grid.secondsPerBeat);

        return anchorPosition(anchors, std::round(index / step) * step + step * direction,
                              grid.secondsPerBeat);
    }

    return seconds + unitSeconds(grid, resolution) * direction;
}

/**
 * A swept range, snapped.
 *
 * Both edges go to the nearest line, so the length is always a whole number
 * of units - except that rounding can send both edges to the same line and
 * collapse the loop. A sweep the user actually made is never nothing, so a
 * collapsed range is opened to one unit instead, growing whichever way the
 * sweep already leaned.
 *
 * lengthSeconds converts to and from the normalised form the lane stores.
 * Nothing is clamped to the file here: a loop that reaches past the end is
 * the transport's business, and clamping would quietly shorten the last bar
 * of a track that does not end on one.
 */
inline Range snapRange(Range normalised, double lengthSeconds, const Grid& grid,
                       Resolution resolution)
{
    if (!(lengthSeconds > 0.0) || !canQuantize(grid, resolution))
        return normalised;

    const auto rawStart = normalised.start * lengthSeconds;
    const auto rawEnd = normalised.end * lengthSeconds;

    auto start = snapSeconds(rawStart, grid, resolution);
    auto end = snapSeconds(rawEnd, grid, resolution);

    if (!(end > start))
    {
        /*  Both edges landed on one line. Keep the edge the sweep was
            nearer to and put the other one line away, so a flick right
            grows right and a flick left grows left rather than the loop
            always jumping one way. One line, not one unit: on a track whose
            tempo moves the two are not the same length.
        */
        const auto startPull = std::abs(rawStart - start);
        const auto endPull = std::abs(rawEnd - end);

        if (endPull <= startPull)
            start = stepSeconds(end, grid, resolution, -1);
        else
            end = stepSeconds(start, grid, resolution, 1);
    }

    return {start / lengthSeconds, end / lengthSeconds};
}

} // namespace stemlab::quantize
