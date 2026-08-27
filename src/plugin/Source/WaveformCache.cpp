#include "WaveformCache.h"

#include <algorithm>

StemLabWaveformCache::StemLabWaveformCache(juce::AudioFormatManager& formatsIn)
    : juce::Thread("StemLab waveform"), formats(formatsIn)
{
    // Normal while no separation runs; setSeparationActive drops the worker
    // to low for the duration of one, where analysis would compete with a
    // job the user is actually waiting for.
    startThread(juce::Thread::Priority::normal);
}

StemLabWaveformCache::~StemLabWaveformCache()
{
    signalThreadShouldExit();
    notify();

    // The worker holds the lock only briefly and checks for exit inside the
    // read loop, so a bounded wait is enough.
    stopThread(4000);
}

juce::String StemLabWaveformCache::keyFor(const juce::File& file)
{
    return file.getFullPathName() + "|" + juce::String(file.getSize()) + "|" +
           juce::String(file.getLastModificationTime().toMilliseconds());
}

StemLabWaveformCache::ProfilePtr StemLabWaveformCache::get(const juce::File& file)
{
    if (!file.existsAsFile())
        return nullptr;

    const auto key = keyFor(file);

    {
        const juce::ScopedLock scoped(lock);

        const auto entry = profiles.find(key);

        if (entry != profiles.end())
            return entry->second;

        // An in-flight or queued file is marked present-but-null, so a lane
        // repainting at the UI rate queues it once rather than every frame.
        profiles.emplace(key, nullptr);
        pending.push_back(file);
    }

    notify();
    return nullptr;
}

void StemLabWaveformCache::warm(const juce::File& file)
{
    // get() already queues an unknown file exactly once and ignores a
    // missing one; warming is asking without wanting the answer yet.
    get(file);
}

void StemLabWaveformCache::retainOnly(const juce::Array<juce::File>& keep)
{
    // Keys are "path|size|mtime"; matching on the "path|" prefix keeps
    // every version of a kept file without another stat here.
    juce::StringArray keepPrefixes;

    for (const auto& file : keep)
        if (file != juce::File())
            keepPrefixes.add(file.getFullPathName() + "|");

    const juce::ScopedLock scoped(lock);

    for (auto entry = profiles.begin(); entry != profiles.end();)
    {
        bool kept = false;

        for (const auto& prefix : keepPrefixes)
            kept = kept || entry->first.startsWith(prefix);

        if (kept)
            ++entry;
        else
            entry = profiles.erase(entry);
    }

    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&keep](const juce::File& file)
                                 { return !keep.contains(file); }),
                  pending.end());
}

void StemLabWaveformCache::setSeparationActive(bool active)
{
    separationActive.store(active);

    // Wake a parked worker so the priority change is applied now rather
    // than when the next file happens to be queued.
    notify();
}

StemLabWaveformCache::Profile StemLabWaveformCache::analyse(const juce::File& file)
{
    namespace waveform = stemlab::waveform;

    Profile profile;

    std::unique_ptr<juce::AudioFormatReader> reader{formats.createReaderFor(file)};

    if (reader == nullptr || !(reader->sampleRate > 0.0) || reader->lengthInSamples <= 0)
        return profile;

    const auto total = static_cast<juce::int64>(reader->lengthInSamples);
    const auto rate = reader->sampleRate;
    const auto channels = juce::jlimit(1, waveform::peakMaxChannels,
                                       static_cast<int>(reader->numChannels));

    profile.lengthSeconds = static_cast<double>(total) / rate;

    /*
     * Frames are sized here rather than inside analysePeaks because the read
     * blocks below are a whole number of frames: a frame that straddled two
     * blocks would need carrying across, and getting that wrong shows up as
     * a stripe of wrong peaks every block boundary.
     */
    auto hop = static_cast<juce::int64>(juce::jmax(1.0, rate * waveform::peakSecondsPerFrame));

    if (static_cast<std::size_t>(total / hop) > waveform::peakMaxFrames)
        hop = juce::jmax(hop, total / static_cast<juce::int64>(waveform::peakMaxFrames));

    profile.peaks.channels = channels;
    profile.peaks.secondsPerFrame = static_cast<double>(hop) / rate;

    const auto frameCount = static_cast<std::size_t>((total + hop - 1) / hop);
    const auto slots = frameCount * static_cast<std::size_t>(channels);

    profile.peaks.minima.assign(slots, 0.0f);
    profile.peaks.maxima.assign(slots, 0.0f);

    // The spectrum wants the whole file summed to mono; the peaks want it a
    // block at a time. One read serves both.
    std::vector<float> mono(static_cast<std::size_t>(total), 0.0f);

    const auto blockFrames = juce::int64{512};
    const auto blockSize = static_cast<int>(hop * blockFrames);

    juce::AudioBuffer<float> block(channels, blockSize);

    std::size_t frame = 0;

    for (juce::int64 position = 0; position < total;)
    {
        if (threadShouldExit())
            return {};

        const auto count = static_cast<int>(juce::jmin<juce::int64>(blockSize, total - position));

        if (!reader->read(&block, 0, count, position, true, channels > 1))
            return {};

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* samples = block.getReadPointer(channel);

            for (int i = 0; i < count; ++i)
                mono[static_cast<std::size_t>(position + i)] += samples[i];
        }

        for (int offset = 0; offset < count && frame < frameCount;
             offset += static_cast<int>(hop), ++frame)
        {
            const auto stop = juce::jmin(count, offset + static_cast<int>(hop));

            for (int channel = 0; channel < channels; ++channel)
            {
                const auto* samples = block.getReadPointer(channel);

                auto lowest = samples[offset];
                auto highest = samples[offset];

                for (int i = offset + 1; i < stop; ++i)
                {
                    lowest = juce::jmin(lowest, samples[i]);
                    highest = juce::jmax(highest, samples[i]);
                }

                const auto index = frame * static_cast<std::size_t>(channels) +
                                   static_cast<std::size_t>(channel);

                profile.peaks.minima[index] = lowest;
                profile.peaks.maxima[index] = highest;
            }
        }

        position += count;
    }

    if (channels > 1)
    {
        const auto scale = 1.0f / static_cast<float>(channels);

        for (auto& sample : mono)
            sample *= scale;
    }

    profile.spectrum = waveform::analyseMono(mono.data(), mono.size(), rate);

    return profile;
}

void StemLabWaveformCache::run()
{
    // Matches the priority the thread was started with; setPriority only
    // works from the target thread, so the flag is applied here.
    bool appliedSeparationActive = false;

    while (!threadShouldExit())
    {
        const bool active = separationActive.load();

        if (active != appliedSeparationActive)
        {
            appliedSeparationActive = active;
            setPriority(active ? juce::Thread::Priority::low
                               : juce::Thread::Priority::normal);
        }

        juce::File next;

        {
            const juce::ScopedLock scoped(lock);

            if (!pending.empty())
            {
                // FIFO: lanes queue top to bottom, so the top lane's
                // profile lands first instead of last.
                next = pending.front();
                pending.erase(pending.begin());
            }
        }

        if (next == juce::File())
        {
            wait(-1);
            continue;
        }

        auto profile = std::make_shared<Profile>(analyse(next));

        if (threadShouldExit())
            return;

        {
            const juce::ScopedLock scoped(lock);

            // An unreadable file lands as an empty profile rather than a
            // null one: empty still answers "analysed", so it is not queued
            // again on the next repaint. Store into the existing slot only:
            // an entry evicted mid-analysis stays evicted, and a later
            // get() re-queues it if anything still wants it.
            const auto entry = profiles.find(keyFor(next));

            if (entry != profiles.end())
                entry->second = std::move(profile);
        }
    }
}
