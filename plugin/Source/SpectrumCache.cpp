#include "SpectrumCache.h"

StemLabSpectrumCache::StemLabSpectrumCache(juce::AudioFormatManager& formatsIn)
    : juce::Thread("StemLab spectrum"), formats(formatsIn)
{
    // Below normal: colour is the least urgent thing on screen, and this
    // competes with a separation that the user is actually waiting for.
    startThread(juce::Thread::Priority::low);
}

StemLabSpectrumCache::~StemLabSpectrumCache()
{
    signalThreadShouldExit();
    notify();

    // The worker only ever holds the lock briefly and checks for exit
    // between files, so a bounded wait is enough.
    stopThread(4000);
}

juce::String StemLabSpectrumCache::keyFor(const juce::File& file)
{
    return file.getFullPathName() + "|" + juce::String(file.getSize()) + "|" +
           juce::String(file.getLastModificationTime().toMilliseconds());
}

StemLabSpectrumCache::ProfilePtr StemLabSpectrumCache::get(const juce::File& file)
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

void StemLabSpectrumCache::run()
{
    while (!threadShouldExit())
    {
        juce::File next;

        {
            const juce::ScopedLock scoped(lock);

            if (!pending.empty())
            {
                next = pending.back();
                pending.pop_back();
            }
        }

        if (next == juce::File())
        {
            wait(-1);
            continue;
        }

        auto profile = std::make_shared<Profile>();

        if (std::unique_ptr<juce::AudioFormatReader> reader{formats.createReaderFor(next)};
            reader != nullptr && reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
        {
            const auto total = static_cast<juce::int64>(reader->lengthInSamples);

            // Summed to mono up front: a centroid is a property of what you
            // hear, and analysing two channels to average them afterwards
            // would double the work for the same answer.
            std::vector<float> mono(static_cast<std::size_t>(total), 0.0f);

            const int blockSize = 1 << 16;
            const int channels = static_cast<int>(reader->numChannels);

            juce::AudioBuffer<float> block(juce::jmax(1, channels), blockSize);

            bool readFailed = false;

            for (juce::int64 position = 0; position < total && !threadShouldExit();)
            {
                const auto count =
                    static_cast<int>(juce::jmin<juce::int64>(blockSize, total - position));

                if (!reader->read(&block, 0, count, position, true, channels > 1))
                {
                    readFailed = true;
                    break;
                }

                for (int channel = 0; channel < channels; ++channel)
                {
                    const auto* source = block.getReadPointer(channel);

                    for (int i = 0; i < count; ++i)
                        mono[static_cast<std::size_t>(position) + static_cast<std::size_t>(i)] +=
                            source[i];
                }

                position += count;
            }

            if (!readFailed && !threadShouldExit())
            {
                if (channels > 1)
                {
                    const auto scale = 1.0f / static_cast<float>(channels);

                    for (auto& sample : mono)
                        sample *= scale;
                }

                *profile = stemlab::waveform::analyseMono(mono.data(), mono.size(),
                                                          reader->sampleRate);
            }
        }

        if (threadShouldExit())
            return;

        {
            const juce::ScopedLock scoped(lock);

            // An unreadable file lands as an empty profile rather than a
            // null one: empty still answers "analysed", so it is not queued
            // again on the next repaint.
            profiles[keyFor(next)] = std::move(profile);
        }
    }
}
