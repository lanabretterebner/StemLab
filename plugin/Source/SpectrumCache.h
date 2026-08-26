#pragma once

#include <JuceHeader.h>

#include <map>
#include <memory>
#include <vector>

#include "WaveformSpectrum.h"

/**
 * Spectral profiles for lane waveform colouring, analysed off the message
 * thread and kept for as long as the editor lives.
 *
 * Analysis reads the whole stem and runs a few thousand FFTs, which is well
 * under a second per file but nowhere near a paint call's budget. Lanes ask
 * for a profile and draw in a neutral colour until one arrives; the editor
 * already repaints them at the UI refresh rate, so a finished analysis
 * appears on its own without any completion plumbing.
 */
class StemLabSpectrumCache final : private juce::Thread
{
public:
    using Profile = stemlab::waveform::SpectralProfile;
    using ProfilePtr = std::shared_ptr<const Profile>;

    explicit StemLabSpectrumCache(juce::AudioFormatManager& formats);
    ~StemLabSpectrumCache() override;

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

    void run() override;

    juce::AudioFormatManager& formats;

    juce::CriticalSection lock;
    std::map<juce::String, ProfilePtr> profiles;
    std::vector<juce::File> pending;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabSpectrumCache)
};
