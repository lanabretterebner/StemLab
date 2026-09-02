#pragma once

#include <algorithm>
#include <cmath>

/*
 * Snapping a swept loop range onto the beat grid.
 *
 * The grid here is the one the lanes actually paint: a uniform rule at
 * barOne + n * secondsPerBeat, with a bar every beatsPerBar of them. It is
 * deliberately not the detected beat positions - a loop that snapped to
 * beats the lane does not draw would land visibly off its own gridlines on
 * anything the model read as slightly uneven.
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
    return unitSeconds(grid, resolution) > 0.0;
}

/** The nearest gridline to a time in seconds. */
inline double snapSeconds(double seconds, const Grid& grid, Resolution resolution)
{
    const auto unit = unitSeconds(grid, resolution);

    if (!(unit > 0.0))
        return seconds;

    return grid.barOne + std::round((seconds - grid.barOne) / unit) * unit;
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

    const auto unit = unitSeconds(grid, resolution);

    const auto rawStart = normalised.start * lengthSeconds;
    const auto rawEnd = normalised.end * lengthSeconds;

    auto start = snapSeconds(rawStart, grid, resolution);
    auto end = snapSeconds(rawEnd, grid, resolution);

    if (!(end > start))
    {
        /*  Both edges landed on one line. Keep the edge the sweep was
            nearer to and put the other a unit away, so a flick right grows
            right and a flick left grows left rather than the loop always
            jumping one way.
        */
        const auto startPull = std::abs(rawStart - start);
        const auto endPull = std::abs(rawEnd - end);

        if (endPull <= startPull)
            start = end - unit;
        else
            end = start + unit;
    }

    return {start / lengthSeconds, end / lengthSeconds};
}

} // namespace stemlab::quantize
