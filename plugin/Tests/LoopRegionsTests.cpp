#include "LoopRegions.h"

#include <cassert>
#include <cmath>

using namespace stemlab::loops;

namespace
{
bool near(double a, double b) { return std::abs(a - b) < 1.0e-9; }
}

int main()
{
    // Overlapping ranges from two lanes play through as one region.
    {
        const auto merged = mergeRegions({{0.2, 0.5}, {0.4, 0.7}});
        assert(merged.size() == 1);
        assert(near(merged[0].start, 0.2) && near(merged[0].end, 0.7));

        // Inside the merged stretch playback just continues.
        assert(!repositionFor(merged, 0.45).has_value());
    }

    // Back-to-back ranges count as overlapping: no jump at the seam.
    {
        const auto merged = mergeRegions({{0.1, 0.3}, {0.3, 0.6}});
        assert(merged.size() == 1);
        assert(!repositionFor(merged, 0.3).has_value());
    }

    // A gap between loops is skipped by jumping to the next one.
    {
        const auto merged = mergeRegions({{0.6, 0.8}, {0.1, 0.3}});
        assert(merged.size() == 2);
        assert(near(merged[0].start, 0.1)); // sorted regardless of sweep order

        const auto jump = repositionFor(merged, 0.4);
        assert(jump.has_value() && near(*jump, 0.6));
    }

    // Past the last region, playback wraps to the first: that is the loop.
    {
        const auto merged = mergeRegions({{0.1, 0.3}, {0.6, 0.8}});
        const auto wrap = repositionFor(merged, 0.9);
        assert(wrap.has_value() && near(*wrap, 0.1));

        // The region end itself is already outside.
        const auto atEnd = repositionFor(merged, 0.8);
        assert(atEnd.has_value() && near(*atEnd, 0.1));
    }

    // Before the first region, playback snaps forward into it.
    {
        const auto merged = mergeRegions({{0.5, 0.9}});
        const auto snap = repositionFor(merged, 0.1);
        assert(snap.has_value() && near(*snap, 0.5));
    }

    // No regions, or only degenerate ones: no looping at all.
    {
        assert(repositionFor({}, 0.5) == std::nullopt);
        assert(mergeRegions({{0.4, 0.4}, {0.7, 0.2}}).empty());
    }

    // Three lanes, two of them overlapping: two regions, gap skipped once.
    {
        const auto merged = mergeRegions({{0.0, 0.2}, {0.15, 0.35}, {0.7, 0.75}});
        assert(merged.size() == 2);
        assert(near(merged[0].end, 0.35));
        const auto jump = repositionFor(merged, 0.5);
        assert(jump.has_value() && near(*jump, 0.7));
    }

    return 0;
}
