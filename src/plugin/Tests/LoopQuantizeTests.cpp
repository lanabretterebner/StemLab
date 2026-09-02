#include "LoopQuantize.h"

#include <cassert>
#include <cmath>

using namespace stemlab::quantize;

namespace
{
bool near(double a, double b) { return std::abs(a - b) < 1.0e-9; }

// 120 BPM, 4/4, bar one at zero: a beat is 0.5s and a bar is 2s.
constexpr Grid fourFour{0.0, 0.5, 4};

// A minute of audio keeps the normalised arithmetic readable: one second is
// exactly 1/60.
constexpr double minute = 60.0;

Range normalisedFromSeconds(double start, double end)
{
    return {start / minute, end / minute};
}

double secondsOf(double normalised) { return normalised * minute; }
} // namespace

int main()
{
    // Off leaves the sweep exactly where it was dropped.
    {
        const auto raw = normalisedFromSeconds(1.234, 5.678);
        const auto out = snapRange(raw, minute, fourFour, Resolution::off);
        assert(near(out.start, raw.start) && near(out.end, raw.end));
    }

    // Each resolution is the step it says it is.
    {
        assert(near(unitSeconds(fourFour, Resolution::quarterBeat), 0.125));
        assert(near(unitSeconds(fourFour, Resolution::halfBeat), 0.25));
        assert(near(unitSeconds(fourFour, Resolution::beat), 0.5));
        assert(near(unitSeconds(fourFour, Resolution::bar), 2.0));
    }

    // Bar: a sweep from just after bar 1 to just before bar 4 lands on both.
    {
        const auto out = snapRange(normalisedFromSeconds(2.3, 7.6), minute, fourFour,
                                   Resolution::bar);
        assert(near(secondsOf(out.start), 2.0));
        assert(near(secondsOf(out.end), 8.0));
    }

    // Beat, on the same sweep, keeps far more of it.
    {
        const auto out = snapRange(normalisedFromSeconds(2.3, 7.6), minute, fourFour,
                                   Resolution::beat);
        assert(near(secondsOf(out.start), 2.5));
        assert(near(secondsOf(out.end), 7.5));
    }

    // The snapped length is always a whole number of units.
    {
        for (const auto resolution : {Resolution::quarterBeat, Resolution::halfBeat,
                                      Resolution::beat, Resolution::bar})
        {
            const auto unit = unitSeconds(fourFour, resolution);
            const auto out =
                snapRange(normalisedFromSeconds(1.31, 9.87), minute, fourFour, resolution);
            const auto units = (secondsOf(out.end) - secondsOf(out.start)) / unit;
            assert(near(units, std::round(units)));
            assert(units >= 1.0);
        }
    }

    // A sweep too short to survive rounding opens to one unit rather than
    // collapsing into a loop the transport would sit inside forever.
    {
        // Both edges are nearest to 2.0s, so a naive snap would give nothing.
        const auto out = snapRange(normalisedFromSeconds(1.95, 2.05), minute, fourFour,
                                   Resolution::bar);
        assert(secondsOf(out.end) > secondsOf(out.start));
        assert(near(secondsOf(out.end) - secondsOf(out.start), 2.0));
    }

    // ... and it opens the way the sweep leaned. Nearer the start line, so
    // the loop grows forwards from it.
    {
        const auto out = snapRange(normalisedFromSeconds(2.02, 2.30), minute, fourFour,
                                   Resolution::bar);
        assert(near(secondsOf(out.start), 2.0));
        assert(near(secondsOf(out.end), 4.0));
    }

    // Leaning the other way grows backwards, so the edge the user put down
    // last is the one that survives.
    {
        const auto out = snapRange(normalisedFromSeconds(1.70, 1.98), minute, fourFour,
                                   Resolution::bar);
        assert(near(secondsOf(out.start), 0.0));
        assert(near(secondsOf(out.end), 2.0));
    }

    // Bar one offsets the whole rule: a track whose first downbeat is at
    // 0.3s snaps to 0.3, 2.3, 4.3 ... not to 0, 2, 4.
    {
        constexpr Grid offset{0.3, 0.5, 4};
        const auto out =
            snapRange(normalisedFromSeconds(2.4, 6.1), minute, offset, Resolution::bar);
        assert(near(secondsOf(out.start), 2.3));
        assert(near(secondsOf(out.end), 6.3));
    }

    // A sweep before bar one snaps to the rule continued backwards rather
    // than being dragged forward to it.
    {
        constexpr Grid offset{4.0, 0.5, 4};
        const auto out =
            snapRange(normalisedFromSeconds(0.4, 1.7), minute, offset, Resolution::bar);
        assert(near(secondsOf(out.start), 0.0));
        assert(near(secondsOf(out.end), 2.0));
    }

    // Meter is respected: a bar in 3/4 is three beats, not four.
    {
        constexpr Grid threeFour{0.0, 0.5, 3};
        assert(near(unitSeconds(threeFour, Resolution::bar), 1.5));

        const auto out =
            snapRange(normalisedFromSeconds(1.6, 4.4), minute, threeFour, Resolution::bar);
        assert(near(secondsOf(out.start), 1.5));
        assert(near(secondsOf(out.end), 4.5));
    }

    // No grid, nothing to snap to: the sweep is returned untouched rather
    // than collapsed onto a rule that does not exist.
    {
        constexpr Grid none{0.0, 0.0, 4};
        assert(!canQuantize(none, Resolution::bar));

        const auto raw = normalisedFromSeconds(1.234, 5.678);
        const auto out = snapRange(raw, minute, none, Resolution::bar);
        assert(near(out.start, raw.start) && near(out.end, raw.end));
    }

    // Neither does a file of no length.
    {
        const auto raw = normalisedFromSeconds(1.234, 5.678);
        const auto out = snapRange(raw, 0.0, fourFour, Resolution::bar);
        assert(near(out.start, raw.start) && near(out.end, raw.end));
    }

    // Snapping twice changes nothing: the stored range and the drag preview
    // both run through this, so it has to be idempotent.
    {
        const auto once =
            snapRange(normalisedFromSeconds(2.3, 7.6), minute, fourFour, Resolution::beat);
        const auto twice = snapRange(once, minute, fourFour, Resolution::beat);
        assert(near(once.start, twice.start) && near(once.end, twice.end));
    }

    // A beatsPerBar the analysis never filled in must not divide by zero or
    // make a bar shorter than a beat.
    {
        constexpr Grid degenerate{0.0, 0.5, 0};
        assert(near(unitSeconds(degenerate, Resolution::bar), 0.5));
    }

    return 0;
}
