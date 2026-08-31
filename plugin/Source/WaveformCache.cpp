#include "WaveformCache.h"

#include <algorithm>
#include <vector>

#if JUCE_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
/*
 * Throttling during a separation is by work, not by scheduler priority.
 *
 * juce::Thread::setPriority does nothing at all on Linux: JUCE 9 stores the
 * value, returns true for source compatibility with the other platforms, and
 * leaves the thread alone "until we implement Nice awareness"; the priority
 * handed to createNativeThread is likewise ignored for a non-realtime thread.
 * A priority drop would therefore have thrown the analysis clear of a running
 * job on macOS only. Resting for a multiple of what the block just cost works
 * the same everywhere, needs no privileges, and sizes itself to the machine
 * and the file rather than to a guessed number of milliseconds.
 */
constexpr double throttleRestFactor = 2.0;

/** Ceiling on one rest, so a slow read cannot park the worker for a visible
    stretch and the end of a separation is picked up within a block. */
constexpr int throttleMaxRestMillis = 25;

#if JUCE_LINUX
/** The hint setPriority cannot give here. Raising niceness needs no privilege
    but lowering it back does, so it is asked for once for the life of the
    worker rather than around each separation - the resting is what follows a
    job starting and ending. */
constexpr int workerNiceness = 10;
#endif
}

StemLabWaveformCache::StemLabWaveformCache(juce::AudioFormatManager& formatsIn)
    : juce::Thread("StemLab waveform"), formats(formatsIn)
{
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

    // Wake a resting or parked worker so a separation that has just ended
    // gives back full speed now rather than after one more rest.
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

    /*
     * The spectrum takes its windows as the blocks arrive rather than a mono
     * copy of the file: only the windows the spectrum hop lands on are ever
     * read, while a copy of an hour-long capture is 635MB of float resident
     * for the whole analysis.
     */
    waveform::MonoSpectrumScanner spectrum(static_cast<std::size_t>(total), rate);

    // One window of mono, refilled in place: sized by the window rather than
    // by the file, which is the point of feeding the scanner in pieces.
    std::vector<float> monoWindow(static_cast<std::size_t>(waveform::spectrumFftSize), 0.0f);

    const auto monoScale = 1.0f / static_cast<float>(channels);

    /*
     * Blocks are a whole number of peak frames, as above, and bounded in
     * samples: the hop a long file stretches to must not drag the read buffer
     * up with it, which is the other way a duration turns into memory.
     */
    constexpr juce::int64 blockTargetSamples = 1 << 16;

    const auto blockFrames = juce::jmax<juce::int64>(1, blockTargetSamples / hop);
    const auto blockSize = static_cast<int>(hop * blockFrames);

    juce::AudioBuffer<float> block(channels, blockSize);

    std::size_t frame = 0;

    for (juce::int64 position = 0; position < total;)
    {
        if (threadShouldExit())
            return {};

        const auto startedAt = juce::Time::getMillisecondCounterHiRes();

        const auto count = static_cast<int>(juce::jmin<juce::int64>(blockSize, total - position));

        if (!reader->read(&block, 0, count, position, true, channels > 1))
            return {};

        /*
         * A window can straddle two blocks: the scanner keeps the part it
         * already has and takes the rest from the next block, so what is
         * summed here is the intersection of the window with this block -
         * hence a file position rather than an offset in the block.
         */
        for (auto wanted = spectrum.samplesWanted(); wanted > 0;
             wanted = spectrum.samplesWanted())
        {
            const auto start = spectrum.nextSample();

            if (start >= static_cast<std::size_t>(position + count))
                break;

            const auto from = static_cast<int>(start - static_cast<std::size_t>(position));
            const auto taken = juce::jmin(static_cast<int>(wanted), count - from);

            const float* channelSamples[waveform::peakMaxChannels] = {};

            for (int channel = 0; channel < channels; ++channel)
                channelSamples[channel] = block.getReadPointer(channel) + from;

            for (int i = 0; i < taken; ++i)
            {
                auto sum = 0.0f;

                for (int channel = 0; channel < channels; ++channel)
                    sum += channelSamples[channel][i];

                monoWindow[static_cast<std::size_t>(i)] = channels > 1 ? sum * monoScale : sum;
            }

            spectrum.push(start, monoWindow.data(), static_cast<std::size_t>(taken));
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

        /*
         * One block is a bounded slice of work with nothing half-read across
         * it, so it is where the worker can hand the CPU back. Resting for
         * longer than the block took leaves a separation the larger share
         * while the analysis still finishes on its own.
         */
        if (separationActive.load())
        {
            const auto workedMs = juce::Time::getMillisecondCounterHiRes() - startedAt;

            wait(juce::jlimit(1, throttleMaxRestMillis,
                              static_cast<int>(workedMs * throttleRestFactor)));
        }
    }

    profile.spectrum = spectrum.finish();

    return profile;
}

void StemLabWaveformCache::run()
{
#if JUCE_LINUX
    // Niceness is per-thread on Linux and settable only from the thread
    // itself, so it is asked for here rather than at construction. A refusal
    // costs nothing: the resting in analyse() is what actually throttles.
    juce::ignoreUnused(::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)),
                                     workerNiceness));
#endif

    // Matches the priority the thread was started with; setPriority only
    // works from the target thread, so the flag is applied here. It is a hint
    // on top of the resting in analyse(), not the mechanism: see above for
    // the platforms where it does nothing.
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
