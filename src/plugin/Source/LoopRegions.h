#pragma once

#include <algorithm>
#include <optional>
#include <vector>

/*
 * The playback loop, when several lanes each hold a highlighted range.
 *
 * Every lane's range takes part: overlapping or touching ranges play through
 * as one stretch, and a gap between ranges is skipped by jumping to the next
 * one. Past the last range, playback wraps to the first. Deliberately
 * JUCE-free so the whole policy is exercised by a plain test binary.
 */
namespace stemlab::loops
{

struct Region
{
    double start = 0.0;
    double end = 0.0;
};

/** Sorted, disjoint playback regions from the raw per-lane ranges. */
inline std::vector<Region> mergeRegions(std::vector<Region> regions)
{
    regions.erase(std::remove_if(regions.begin(), regions.end(),
                                 [](const Region& r) { return !(r.end > r.start); }),
                  regions.end());

    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.start < b.start; });

    std::vector<Region> merged;

    for (const auto& region : regions)
    {
        // Touching counts as overlapping: back-to-back loops play through.
        if (!merged.empty() && region.start <= merged.back().end)
            merged.back().end = std::max(merged.back().end, region.end);
        else
            merged.push_back(region);
    }

    return merged;
}

/**
 * Where the playhead belongs, given where it is. Inside a region: nowhere
 * else (empty optional). In a gap: the start of the next region. Past the
 * last region: the start of the first, which is what makes it a loop.
 */
inline std::optional<double> repositionFor(const std::vector<Region>& merged, double position)
{
    if (merged.empty())
        return std::nullopt;

    for (const auto& region : merged)
    {
        if (position < region.start)
            return region.start;

        if (position < region.end)
            return std::nullopt;
    }

    return merged.front().start;
}

} // namespace stemlab::loops
