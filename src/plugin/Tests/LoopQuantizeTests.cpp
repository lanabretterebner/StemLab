#include "LoopQuantize.h"

#include <cassert>
#include <cmath>
#include <vector>

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

    // ------------------------------------- snapping to the detected beats

    /*
     * The same drifting track the grid test rules: 0.5 s beats for two bars,
     * then 0.4 s. A loop snapped by the constant tempo would drift out of the
     * music exactly as the lane's old constant grid did.
     */
    const std::vector<double> driftBeats{0.0, 0.5, 1.0, 1.5,   // bar 1
                                         2.0, 2.5, 3.0, 3.5,   // bar 2
                                         4.0, 4.4, 4.8, 5.2,   // bar 3, faster
                                         5.6, 6.0, 6.4, 6.8};  // bar 4
    const std::vector<double> driftDownbeats{0.0, 2.0, 4.0, 5.6};

    Grid drift;
    drift.secondsPerBeat = 0.5;
    drift.beatsPerBar = 4;
    drift.useDetectedBeats = true;
    drift.beats = driftBeats;
    drift.downbeats = driftDownbeats;

    // Bars land on the downbeats, including the one a constant grid would
    // have put at 6.0 rather than 5.6.
    {
        const auto out =
            snapRange(normalisedFromSeconds(4.1, 5.5), minute, drift, Resolution::bar);
        assert(near(secondsOf(out.start), 4.0));
        assert(near(secondsOf(out.end), 5.6));
    }

    // Beats land on measured beats, at the spacing that was measured there.
    {
        const auto out =
            snapRange(normalisedFromSeconds(4.35, 6.3), minute, drift, Resolution::beat);
        assert(near(secondsOf(out.start), 4.4));
        assert(near(secondsOf(out.end), 6.4));
    }

    // Halves and quarters split the real interval, so a subdivision inside a
    // 0.4 s beat is 0.2 s along, not 0.25 s.
    {
        const auto out =
            snapRange(normalisedFromSeconds(4.19, 4.75), minute, drift, Resolution::halfBeat);
        assert(near(secondsOf(out.start), 4.2));
        assert(near(secondsOf(out.end), 4.8));
    }

    {
        const auto out =
            snapRange(normalisedFromSeconds(4.09, 4.32), minute, drift, Resolution::quarterBeat);
        assert(near(secondsOf(out.start), 4.1));
        assert(near(secondsOf(out.end), 4.3));
    }

    /*  A collapsed sweep opens by one real line, which past the tempo change
        is 0.4 s rather than the nominal 0.5 s - and it opens away from
        whichever edge landed nearest its line, so the deliberate edge is the
        one that survives.
    */
    {
        // The start is nearest 4.4, so the loop grows forwards from it.
        const auto forwards =
            snapRange(normalisedFromSeconds(4.41, 4.43), minute, drift, Resolution::beat);
        assert(near(secondsOf(forwards.start), 4.4));
        assert(near(secondsOf(forwards.end), 4.8));

        // The end is nearest 4.4, so it grows backwards into the slower bar,
        // where one line back is 0.5 s rather than 0.4 s.
        const auto backwards =
            snapRange(normalisedFromSeconds(4.37, 4.39), minute, drift, Resolution::beat);
        assert(near(secondsOf(backwards.start), 4.0));
        assert(near(secondsOf(backwards.end), 4.4));
    }

    // Snapping stays idempotent on a beat-ruled grid too - the lane's live
    // preview and the stored range both run through it.
    {
        const auto once =
            snapRange(normalisedFromSeconds(4.35, 6.3), minute, drift, Resolution::beat);
        const auto twice = snapRange(once, minute, drift, Resolution::beat);
        assert(near(once.start, twice.start) && near(once.end, twice.end));
    }

    // Past either end of the analysed span the edge interval continues, so a
    // loop swept into the run-in still lands on the rule instead of being
    // dragged to the first beat.
    {
        const auto out =
            snapRange(normalisedFromSeconds(-1.1, -0.4), minute, drift, Resolution::beat);
        assert(near(secondsOf(out.start), -1.0));
        assert(near(secondsOf(out.end), -0.5));
    }

    // With beats but no downbeats a bar is beatsPerBar beats along the list.
    {
        Grid noDown = drift;
        noDown.downbeats = {};

        const auto out =
            snapRange(normalisedFromSeconds(1.9, 4.1), minute, noDown, Resolution::bar);
        assert(near(secondsOf(out.start), 2.0));
        assert(near(secondsOf(out.end), 4.0));
    }

    // useDetectedBeats with nothing behind it falls back to the constant
    // tempo rather than refusing to snap.
    {
        Grid empty;
        empty.secondsPerBeat = 0.5;
        empty.beatsPerBar = 4;
        empty.useDetectedBeats = true;

        assert(!empty.rulingFromBeats());
        assert(canQuantize(empty, Resolution::bar));

        const auto out =
            snapRange(normalisedFromSeconds(2.3, 7.6), minute, empty, Resolution::bar);
        assert(near(secondsOf(out.start), 2.0));
        assert(near(secondsOf(out.end), 8.0));
    }

    // Beats can rule even with no constant tempo to fall back on at all.
    {
        Grid beatsOnly;
        beatsOnly.secondsPerBeat = 0.0;
        beatsOnly.useDetectedBeats = true;
        beatsOnly.beats = driftBeats;
        beatsOnly.downbeats = driftDownbeats;

        assert(canQuantize(beatsOnly, Resolution::beat));

        const auto out =
            snapRange(normalisedFromSeconds(4.35, 6.3), minute, beatsOnly, Resolution::beat);
        assert(near(secondsOf(out.start), 4.4));
        assert(near(secondsOf(out.end), 6.4));
    }

    // Off still means off, beats or no beats.
    {
        const auto raw = normalisedFromSeconds(4.37, 6.31);
        const auto out = snapRange(raw, minute, drift, Resolution::off);
        assert(near(out.start, raw.start) && near(out.end, raw.end));
    }

    // A beatsPerBar the analysis never filled in must not divide by zero or
    // make a bar shorter than a beat.
    {
        constexpr Grid degenerate{0.0, 0.5, 0};
        assert(near(unitSeconds(degenerate, Resolution::bar), 0.5));
    }

    return 0;
}
