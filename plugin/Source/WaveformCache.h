#pragma once

#include <JuceHeader.h>

#include <map>
#include <memory>
#include <vector>

#include "WaveformAnalysis.h"

/**
 * Waveform profiles - peak envelope and spectral colour - analysed off the
 * message thread and kept for as long as the editor lives.
 *
 * Analysis reads the whole stem once, reduces it to a peak envelope, and runs
 * a few thousand FFTs over it. That is well under a second per file and
 * nowhere near a paint call's budget, so lanes ask for a profile and draw
 * nothing until one arrives. The editor already repaints them at the UI
 * refresh rate, so a finished analysis appears on its own without any
 * completion plumbing.
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabWaveformCache)
};
