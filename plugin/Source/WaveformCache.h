#pragma once

#include <JuceHeader.h>

#include <map>
#include <memory>
#include <vector>

#include "WaveformAnalysis.h"

/**
 * Waveform profiles - peak envelope and spectral colour - analysed off the
 * message thread and kept for as long as the processor lives.
 *
 * Analysis reads the whole stem once, reduces it to a peak envelope, and runs
 * a few thousand FFTs over it. The read is a stream: neither the file nor a
 * mono copy of it is ever resident, so an hour-long capture costs the same
 * memory as a ten-second one. That is well under a second per file and
 * nowhere near a paint call's budget, so lanes ask for a profile and draw
 * nothing until one arrives. Completion is still not signalled: a lane
 * without a profile polls this cache from the editor's timer, and a poll
 * that comes back with something holds the editor at its full refresh rate
 * so the rest of the stems land at the same pace. Owned by the processor
 * rather than the editor, so closing and reopening the window does not
 * re-read and re-FFT every stem.
 */
class StemLabWaveformCache final : private juce::Thread
{
public:
    using Profile = stemlab::waveform::WaveformProfile;
    using ProfilePtr = std::shared_ptr<const Profile>;

    explicit StemLabWaveformCache(juce::AudioFormatManager& formats);
    ~StemLabWaveformCache() override;

    /**
     * The profile for a file, or nullptr while it is still being analysed.
     *
     * An unknown file is queued on the first call. A file that cannot be
     * read is remembered as unanalysable rather than retried every frame.
     */
    ProfilePtr get(const juce::File& file);

    /**
     * Queue a file that is known to be wanted soon - a job just produced
     * it, or it was just loaded - so its analysis runs before the first
     * paint asks. Already-known and missing files are ignored; paint's
     * lazy get() stays the fallback. Callable from any thread.
     */
    void warm(const juce::File& file);

    /**
     * Growth bound: drop every profile and queued analysis whose file is
     * not in keep. Eviction is keyed by file path (the stored keys carry
     * size/mtime after the path, so every version of a kept file stays).
     * Called when a new job starts, which is the moment the previous job's
     * stems stop being drawn.
     */
    void retainOnly(const juce::Array<juce::File>& keep);

    /**
     * Throttled while a separation runs - analysis must not compete with a
     * job the user is actually waiting for - and back to full speed when it
     * ends, so freshly finished stems get their profiles promptly. Callable
     * from any thread; the worker reads the flag between read blocks, so a
     * change lands within one block either way and a queued file still
     * finishes while the flag is set.
     */
    void setSeparationActive(bool active);

private:
    /** Path plus size and timestamp, so a rewritten stem is re-analysed
        rather than served from a stale entry at the same path. */
    static juce::String keyFor(const juce::File& file);

    /** Read one file and reduce it. Returns an empty profile if it cannot
        be read, which still counts as analysed. */
    Profile analyse(const juce::File& file);

    void run() override;

    juce::AudioFormatManager& formats;

    juce::CriticalSection lock;
    std::map<juce::String, ProfilePtr> profiles;
    std::vector<juce::File> pending;

    /** Set for the length of a separation; the worker rests between read
        blocks while it is set. */
    std::atomic<bool> separationActive{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabWaveformCache)
};
