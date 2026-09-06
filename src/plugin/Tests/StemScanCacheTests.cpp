/*
 * The scan cache's freshness rules, and the one that was wrong.
 *
 * A lookup that finds the output folder gone must publish that emptiness -
 * key and stamp included. Without it, isFresh had already advanced its own
 * recheck clock, so the first lane asked answered "gone" and the next five
 * went on handing out paths to deleted files for the rest of the interval;
 * asking the first lane again brought its path back.
 *
 * The clock and the folder's stamp are both parameters, so this is arithmetic
 * rather than a race: no sleeping, no filesystem timing.
 */

#include "StemScanCache.h"

#include <array>
#include <cstdlib>

namespace
{
constexpr std::size_t stemCount = 6;

using Cache = stemlab::scan::StemFileCache<stemCount>;
using Paths = Cache::Paths;

void check(bool condition)
{
    if (!condition)
        std::abort();
}

Paths sixStems(const juce::File& folder)
{
    Paths paths;

    for (std::size_t i = 0; i < stemCount; ++i)
        paths[i] = folder.getChildFile("stem" + juce::String(static_cast<int>(i)) + ".wav");

    return paths;
}

/** What getCompletedStemFile does, with the filesystem and clock handed in. */
juce::File lookup(Cache& cache, std::size_t index, const juce::File& job, bool jobDone,
                  juce::uint32 nowMs, const juce::Time& stamp, bool outputExists)
{
    if (cache.isFresh(job, jobDone, nowMs, stamp))
        return cache.get(index);

    const auto publish = [&](Paths resolved)
    {
        cache.publish(job, jobDone, nowMs, stamp, resolved);
        return resolved[index];
    };

    if (!outputExists)
        return publish({});

    return publish(sixStems(job.getChildFile("refined")));
}

bool everyLaneEmpty(Cache& cache, const juce::File& job, bool jobDone, juce::uint32 nowMs,
                    const juce::Time& stamp, bool outputExists)
{
    for (std::size_t i = 0; i < stemCount; ++i)
        if (lookup(cache, i, job, jobDone, nowMs, stamp, outputExists) != juce::File())
            return false;

    return true;
}
} // namespace

int main()
{
    const juce::File job("/jobs/job_1");
    const juce::File otherJob("/jobs/job_2");

    const juce::Time full(1000);
    const juce::Time changed(2000);
    const juce::Time gone;

    // A cold cache scans, and then answers for every lane without scanning.
    {
        Cache cache;

        check(!cache.isFresh(job, true, 0, full));

        const auto first = lookup(cache, 0, job, true, 0, full, true);

        check(first != juce::File());

        for (std::size_t i = 0; i < stemCount; ++i)
            check(cache.get(i) != juce::File());

        check(cache.isFresh(job, true, 0, full));
    }

    // Inside the recheck interval the stamp is not even consulted: that is
    // the whole point of the throttle.
    {
        Cache cache;
        lookup(cache, 0, job, true, 0, full, true);

        check(cache.isFresh(job, true, Cache::recheckIntervalMs - 1, changed));
    }

    // A different job, or the same job now finished, is never fresh.
    {
        Cache cache;
        lookup(cache, 0, job, true, 0, full, true);

        check(!cache.isFresh(otherJob, true, 0, full));
        check(!cache.isFresh(job, false, 0, full));
    }

    /*  The regression. Prime all six, delete the output folder, let the
        interval pass, and every lane must stay empty however often it is
        asked - not one empty answer followed by five stale paths.
    */
    {
        Cache cache;

        check(!everyLaneEmpty(cache, job, true, 0, full, true));

        const juce::uint32 later = Cache::recheckIntervalMs + 1;

        check(everyLaneEmpty(cache, job, true, later, gone, false));

        // Again, immediately: the first lane must not resurrect its path.
        check(everyLaneEmpty(cache, job, true, later, gone, false));

        // And still empty once the interval passes a second time.
        check(everyLaneEmpty(cache, job, true, later + Cache::recheckIntervalMs + 1, gone,
                             false));
    }

    // The same for the whole job directory going away, which reaches the
    // cache through the same missing-output exit.
    {
        Cache cache;
        lookup(cache, 0, job, true, 0, full, true);

        const juce::uint32 later = Cache::recheckIntervalMs + 1;

        check(everyLaneEmpty(cache, job, true, later, gone, false));
        check(everyLaneEmpty(cache, job, true, later + 1, gone, false));
    }

    // A folder that comes back is picked up rather than staying empty.
    {
        Cache cache;
        lookup(cache, 0, job, true, 0, full, true);

        const juce::uint32 deleted = Cache::recheckIntervalMs + 1;
        check(everyLaneEmpty(cache, job, true, deleted, gone, false));

        const auto restored = deleted + Cache::recheckIntervalMs + 1;
        check(lookup(cache, 3, job, true, restored, changed, true) != juce::File());

        for (std::size_t i = 0; i < stemCount; ++i)
            check(cache.get(i) != juce::File());
    }

    // reset() forgets a job entirely, whatever the clock says.
    {
        Cache cache;
        lookup(cache, 0, job, true, 0, full, true);
        cache.reset();

        check(!cache.isFresh(job, true, 0, full));
        check(cache.get(0) == juce::File());
    }

    return 0;
}
