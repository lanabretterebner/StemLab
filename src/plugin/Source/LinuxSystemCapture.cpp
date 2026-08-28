#include "LinuxSystemCapture.h"

#if JUCE_LINUX

#include <dlfcn.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr int captureSampleRate = 48000;
    constexpr int captureChannels = 2;
    constexpr int chunkFrames = 4800; // 100 ms per read

    /*  The subset of libpulse-simple/libpulse this capture needs, resolved
        with dlopen so neither library is a load-time dependency of the
        plugin. Types and enum values come from the real PulseAudio headers
        (a build-time dependency only), so the ABI cannot drift from
        hand-copied definitions.
    */
    struct PulseClient
    {
        PulseClient()
        {
            simpleHandle = dlopen (
                "libpulse-simple.so.0",
                RTLD_NOW | RTLD_LOCAL);

            pulseHandle = dlopen (
                "libpulse.so.0",
                RTLD_NOW | RTLD_LOCAL);

            if (simpleHandle == nullptr)
                return;

            paSimpleNew =
                reinterpret_cast<decltype (paSimpleNew)> (
                    dlsym (simpleHandle, "pa_simple_new"));

            paSimpleRead =
                reinterpret_cast<decltype (paSimpleRead)> (
                    dlsym (simpleHandle, "pa_simple_read"));

            paSimpleFree =
                reinterpret_cast<decltype (paSimpleFree)> (
                    dlsym (simpleHandle, "pa_simple_free"));

            if (pulseHandle != nullptr)
            {
                paStrerror =
                    reinterpret_cast<decltype (paStrerror)> (
                        dlsym (pulseHandle, "pa_strerror"));
            }
        }

        ~PulseClient()
        {
            if (simpleHandle != nullptr)
                dlclose (simpleHandle);

            if (pulseHandle != nullptr)
                dlclose (pulseHandle);
        }

        bool isUsable() const noexcept
        {
            return paSimpleNew != nullptr
                && paSimpleRead != nullptr
                && paSimpleFree != nullptr;
        }

        juce::String describeError (int error) const
        {
            if (paStrerror != nullptr)
            {
                if (const auto* text = paStrerror (error))
                    return juce::String (
                        juce::CharPointer_UTF8 (text));
            }

            return "error " + juce::String (error);
        }

        pa_simple* (*paSimpleNew) (
            const char* server,
            const char* name,
            pa_stream_direction_t dir,
            const char* dev,
            const char* streamName,
            const pa_sample_spec* ss,
            const pa_channel_map* map,
            const pa_buffer_attr* attr,
            int* error) = nullptr;

        int (*paSimpleRead) (
            pa_simple* s,
            void* data,
            size_t bytes,
            int* error) = nullptr;

        void (*paSimpleFree) (pa_simple* s) = nullptr;

        const char* (*paStrerror) (int error) = nullptr;

        void* simpleHandle = nullptr;
        void* pulseHandle = nullptr;

        JUCE_DECLARE_NON_COPYABLE (PulseClient)
    };

    /*  Everything the pulse reader thread touches, owned by shared_ptr.

        pa_simple_new and pa_simple_read are unbounded blocking calls with no
        timeout or cancellation hook. If the server hangs or the monitored
        device stalls, a read may simply never return - and a thread stuck
        inside libpulse must never be pthread_cancel'd (JUCE's stopThread
        timeout path), because forced unwinding through C frames aborts the
        process.

        So the blocking calls live on a detached std::thread whose entire
        state is this self-owned block. The JUCE thread only drains the
        chunk queue with bounded waits and can always exit promptly; a
        genuinely wedged reader keeps its shared_ptr alive, touches nothing
        owned by the plugin, and evaporates when the read finally returns or
        the process ends.
    */
    struct PulseReader
    {
        PulseClient client;

        std::atomic<bool> stopRequested { false };
        std::atomic<bool> finished { false };
        std::atomic<bool> failed { false };

        std::mutex queueMutex;
        std::deque<std::vector<float>> chunks;

        juce::String failureMessage; // written once before 'failed' is set

        void run()
        {
            if (! client.isUsable())
            {
                fail (
                    "PulseAudio client library not found - install libpulse0 "
                    "(PipeWire desktops include it as pipewire-pulse)");
                return;
            }

            pa_sample_spec spec {};
            spec.format = PA_SAMPLE_FLOAT32NE;
            spec.rate = captureSampleRate;
            spec.channels = captureChannels;

            // Small fragments keep chunk delivery prompt; an unbounded
            // maxlength lets the server pick its own internal buffering.
            pa_buffer_attr attr {};
            attr.maxlength = static_cast<juce::uint32> (-1);
            attr.tlength = static_cast<juce::uint32> (-1);
            attr.prebuf = static_cast<juce::uint32> (-1);
            attr.minreq = static_cast<juce::uint32> (-1);
            attr.fragsize = chunkFrames * captureChannels * sizeof (float);

            int error = 0;

            // "@DEFAULT_MONITOR@" is the server-maintained monitor of the
            // default output, and follows it when the user switches devices.
            // Both PulseAudio and PipeWire's Pulse server implement it.
            auto* stream = client.paSimpleNew (
                nullptr,
                "StemLab",
                PA_STREAM_RECORD,
                "@DEFAULT_MONITOR@",
                "System audio capture",
                &spec,
                nullptr,
                &attr,
                &error);

            if (stream == nullptr)
            {
                fail (
                    "Could not open the system output monitor: "
                    + client.describeError (error)
                    + " (is a PipeWire or PulseAudio desktop session"
                      " running?)");
                return;
            }

            std::vector<float> interleaved (
                static_cast<size_t> (chunkFrames * captureChannels));

            while (! stopRequested.load())
            {
                if (client.paSimpleRead (
                        stream,
                        interleaved.data(),
                        interleaved.size() * sizeof (float),
                        &error) < 0)
                {
                    if (! stopRequested.load())
                        fail (
                            "Lost the system output monitor: "
                            + client.describeError (error));
                    break;
                }

                const std::lock_guard<std::mutex> lock (queueMutex);

                chunks.emplace_back (interleaved);

                // If the consumer vanished or stalled, cap the backlog at
                // ~30 s instead of growing without bound.
                if (chunks.size() > 300)
                    chunks.pop_front();
            }

            client.paSimpleFree (stream);
            finished.store (true);
        }

    private:
        void fail (const juce::String& message)
        {
            failureMessage = message;
            failed.store (true);
            finished.store (true);
        }
    };
}

StemLabSystemLoopbackThread::StemLabSystemLoopbackThread (
    StemLabAudioProcessor& ownerIn,
    juce::File outputFileIn)
    : juce::Thread ("StemLab Pulse loopback"),
      owner (ownerIn),
      outputFile (std::move (outputFileIn))
{
}

StemLabSystemLoopbackThread::~StemLabSystemLoopbackThread()
{
    signalThreadShouldExit();
    notify();

    /*
        The loop waits only in bounded sleeps, but the last thing run() does
        is destroy the ThreadedWriter, which synchronously flushes its FIFO
        and finalises the WAV header - disk I/O of no fixed duration on a
        spun-down external drive or a stalled network mount.

        stopThread()'s timeout would escalate that to pthread_cancel, and a
        forced unwind through noexcept destructor frames calls
        std::terminate: the whole host dies, and the recording with it. So
        wait generously first, and keep the timed stop only as a
        last-resort backstop for a genuinely wedged filesystem.
    */
    if (! waitForThreadToExit (30000))
        stopThread (2000);
}

namespace
{
/*
    Keep this module mapped for the rest of the process.

    The reader below runs detached and self-owned, which is deliberate:
    pa_simple_read blocks with no cancellation point, so nothing may join
    it. Its heap state outlives the plugin safely - but the code it
    executes lives in this shared object, and a host that unloads the
    plugin (JUCE hosts, plug-in scanners) would pull those pages out from
    under a reader still inside its final read. Holding an extra
    RTLD_NODELETE reference costs one leaked handle and removes the
    entire class of crash.
*/
void pinModuleForDetachedThreads()
{
    static const bool pinned = []
    {
        Dl_info info{};

        if (dladdr (reinterpret_cast<const void*> (&pinModuleForDetachedThreads), &info) == 0)
            return false;

        if (info.dli_fname == nullptr)
            return false;

        // Intentionally leaked: this reference is what keeps the module
        // mapped, so it must never be released.
        return dlopen (info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
    }();

    juce::ignoreUnused (pinned);
}
} // namespace

void StemLabSystemLoopbackThread::run()
{
    pinModuleForDetachedThreads();

    auto reader = std::make_shared<PulseReader>();

    std::thread ([reader] { reader->run(); }).detach();

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::AudioBuffer<float> converted (captureChannels, chunkFrames);

    bool sawFirstChunk = false;
    bool captureFailed = false;

    const auto drainQueue = [&]() -> bool
    {
        for (;;)
        {
            std::vector<float> chunk;

            {
                const std::lock_guard<std::mutex> lock (
                    reader->queueMutex);

                if (reader->chunks.empty())
                    return true;

                chunk = std::move (reader->chunks.front());
                reader->chunks.pop_front();
            }

            if (! sawFirstChunk)
            {
                sawFirstChunk = true;

                if (! openWriter (writer))
                    return false;
            }

            const auto frames =
                static_cast<int> (
                    chunk.size()
                    / static_cast<size_t> (captureChannels));

            for (int channel = 0; channel < captureChannels; ++channel)
            {
                auto* destination = converted.getWritePointer (channel);

                for (int frame = 0; frame < frames; ++frame)
                {
                    destination[frame] = juce::jlimit (
                        -1.0f,
                        1.0f,
                        chunk[
                            static_cast<size_t> (frame) * captureChannels
                            + static_cast<size_t> (channel)]);
                }
            }

            // ThreadedWriter::write does not block: when its FIFO is full
            // it returns false and DISCARDS the block. Silently dropping
            // audio here produced a time-compressed recording that still
            // reported success, so wait for the disk instead - and only
            // give up (loudly) if it never catches up.
            bool written = writer->write (
                converted.getArrayOfReadPointers(),
                frames);

            for (int attempt = 0; ! written && attempt < 200; ++attempt)
            {
                wait (10);

                written = writer->write (
                    converted.getArrayOfReadPointers(),
                    frames);
            }

            if (! written)
            {
                fail (
                    "The recording disk cannot keep up - stopping before more"
                    " audio is lost");
                return false;
            }

            owner.capturedSamples.fetch_add (
                static_cast<juce::int64> (frames));
        }
    };

    while (! threadShouldExit())
    {
        if (! drainQueue())
        {
            captureFailed = true;
            break;
        }

        if (reader->finished.load())
            break;

        wait (25);
    }

    reader->stopRequested.store (true);

    // Collect whatever arrived up to the stop click.
    if (! captureFailed)
        drainQueue();

    if (reader->failed.load() && ! captureFailed)
    {
        fail (reader->failureMessage);
        captureFailed = true;
    }

    writer.reset();

    if (! captureFailed)
        successful.store (true);
}

bool StemLabSystemLoopbackThread::openWriter (
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>& writer)
{
    auto fileStream =
        std::make_unique<juce::FileOutputStream> (outputFile);

    if (! fileStream->openedOk())
    {
        fail ("Could not create the system-audio recording file");
        return false;
    }

    juce::WavAudioFormat wav;

    auto* rawWriter = wav.createWriterFor (
        fileStream.get(),
        captureSampleRate,
        static_cast<unsigned int> (captureChannels),
        24,
        {},
        0);

    if (rawWriter == nullptr)
    {
        fail ("Could not create the system-audio WAV writer");
        return false;
    }

    fileStream.release();

    writer =
        std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            rawWriter,
            owner.diskWriterThread,
            65536);

    owner.systemCaptureSampleRate.store (captureSampleRate);
    owner.capturedSamples.store (0);
    owner.droppedCaptureSamples.store (0);

    // Only now is the recording certain, so only now does the loaded source
    // give way to it. Everything that can fail - dlopen'ing libpulse,
    // finding the monitor source, pa_simple_new, creating this file and
    // this writer - has already happened, and none of those failures can
    // put a source back that the Record PC click had discarded.
    owner.beginSystemCaptureSource (outputFile);

    return true;
}

void StemLabSystemLoopbackThread::fail (const juce::String& message)
{
    successful.store (false);
    owner.capturing.store (false);
    owner.standaloneRecordingMode.store (
        StemLabAudioProcessor::recordingNone);

    owner.appendEngineLog (
        "System audio recording: "
        + message
        + "\n");

    owner.setStatus (
        "System audio recording failed - "
        + message);
}

#endif
