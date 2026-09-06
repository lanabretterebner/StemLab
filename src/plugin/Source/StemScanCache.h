#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>

/*
 * One job's resolved stem files, and the rules for when to stop trusting them.
 *
 * The lanes ask for all six of these several times per redraw, at UI rate, for
 * as long as the editor is open, and enumerating the job tree each time pegged
 * a core once a job had finished - worse on a network share. So one scan
 * serves every lookup until something makes it wrong.
 *
 * Three things make it wrong: a different job, a job that has since finished,
 * and the stems themselves being deleted or replaced underneath it. The last
 * one is a stat of the output folder, so it is asked at most a few times a
 * second rather than once per lane per redraw - which is the cost the cache
 * exists to avoid.
 *
 * Its own header because the rule is easy to get subtly wrong and hard to see
 * from the outside: a lookup that finds the folder gone must publish that
 * emptiness, key and stamp included. Returning early without publishing left
 * the freshness clock advanced and the old paths in place, so one lane
 * answered "gone" and the next five went on handing out paths to files that
 * were not there.
 */
namespace stemlab::scan
{

template <std::size_t Count>
class StemFileCache
{
public:
    using Paths = std::array<juce::File, Count>;

    /** How long a snapshot is trusted before the folder is stat'ed again. */
    static constexpr juce::uint32 recheckIntervalMs = 500;

    /**
     * Whether the snapshot still answers for this job.
     *
     * `stampNow` is what the caller reads off the output folder; it is only
     * consulted once the recheck interval has passed, and asking advances that
     * interval, so a caller that gets `false` must publish before returning.
     */
    bool isFresh(const juce::File& job, bool jobDone, juce::uint32 nowMs,
                 const juce::Time& stampNow)
    {
        if (!published || job != cachedJob || jobDone != cachedJobDone)
            return false;

        if (nowMs - checkedMs < recheckIntervalMs)
            return true;

        checkedMs = nowMs;

        return stampNow == cachedStamp;
    }

    /** Adopt a snapshot - including an empty one, which is what a job whose
        output has gone away looks like. */
    void publish(const juce::File& job, bool jobDone, juce::uint32 nowMs,
                 const juce::Time& stamp, Paths paths)
    {
        cachedJob = job;
        cachedJobDone = jobDone;
        cachedStamp = stamp;
        checkedMs = nowMs;
        files = std::move(paths);
        published = true;
    }

    juce::File get(std::size_t index) const
    {
        return index < Count ? files[index] : juce::File();
    }

    /** Forget everything, so the next lookup scans whatever is there now. */
    void reset()
    {
        published = false;
        cachedJob = juce::File();
        cachedJobDone = false;
        cachedStamp = juce::Time();
        files = {};
    }

private:
    bool published = false;
    juce::File cachedJob;
    bool cachedJobDone = false;
    juce::Time cachedStamp;
    juce::uint32 checkedMs = 0;
    Paths files{};
};

} // namespace stemlab::scan
