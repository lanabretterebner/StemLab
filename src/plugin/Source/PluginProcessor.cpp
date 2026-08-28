#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ReaperBridge.h"
#include "StemLabPaths.h"
#include "WaveformGrid.h"

#include <algorithm>
#include <functional>
#include <thread>

#if JUCE_LINUX
#include "LinuxSystemCapture.h"
#endif

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

#if JUCE_WINDOWS
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#endif

/**
 * Sums one AudioFormatReaderSource per audible lane on a single shared
 * clock, applying that lane's solo/mute gains.
 *
 * The entries are the leaves of the stem tree: root stems, except where a
 * root was split further and its adaptive children stand in for it. That is
 * what gives an adaptive child lane a real mute - and it is why no stem is
 * ever summed together with the children it was split into.
 *
 * Built on the message thread by ensureStemMixLoaded() and handed to
 * stemMixTransport; afterwards the audio thread drives it through the
 * transport's resampler. The solo/mute atomics live in the processor and
 * are only read here. Gain changes glide at a fixed ~10 ms rate that is
 * independent of block size, so toggling S/M never clicks even when the
 * resampler splits a transport block into tiny ring-buffer chunks.
 */
class StemLabStemMixSource final : public juce::PositionableAudioSource
{
public:
    struct Entry
    {
        std::unique_ptr<juce::AudioFormatReaderSource> source;
        std::unique_ptr<juce::BufferingAudioSource> buffered;

        /*
         * The lane this entry belongs to, then each of its ancestors up to
         * the root stem. Owned by the processor, but held by shared_ptr so a
         * rebuild of the lane map cannot pull the flags out from under a mix
         * the audio thread is still playing.
         *
         * The mix plays leaves, so a split root has no entry of its own.
         * Reading the whole chain is what keeps its Solo and Mute working:
         * without it, muting Drums after splitting it into Kick and Snare
         * did nothing at all, because the Drums flags were not in the mix.
         *
         * Built on the message thread; only read here.
         */
        std::vector<std::shared_ptr<StemLabLaneMonitorFlags>> chain;

        bool soloed() const
        {
            for (const auto& flags : chain)
                if (flags->solo.load(std::memory_order_relaxed))
                    return true;

            return false;
        }

        bool muted() const
        {
            for (const auto& flags : chain)
                if (flags->mute.load(std::memory_order_relaxed))
                    return true;

            return false;
        }
    };

    // ~0.7 s at 48 kHz: enough to ride out a disk hiccup, small enough that
    // a seek refills quickly.
    static constexpr int readAheadSamples = 32768;

    StemLabStemMixSource(std::vector<Entry> entriesIn, juce::TimeSliceThread& readThread)
        : entries(std::move(entriesIn))
    {
        currentGains.resize(entries.size(), 0.0f);

        // Every entry is a WAV on disk. Reading six of them straight from
        // getNextAudioBlock puts file I/O on the audio thread, where one
        // slow read is an xrun; buffering moves that onto readThread while
        // the gain/solo/mute mixing below stays live at block rate.
        for (auto& entry : entries)
        {
            entry.buffered = std::make_unique<juce::BufferingAudioSource>(
                entry.source.get(), readThread, false, readAheadSamples, 2);
        }
    }

    ~StemLabStemMixSource() override
    {
        // The preparing thread holds references into entries, so it has to
        // be gone before any of them is destroyed.
        joinPreparation();
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        joinPreparation();

        // The wrapping ResamplingAudioSource asks for up to its ring-buffer
        // size (our expected block + 32) in one call, and hosts occasionally
        // deliver larger blocks than announced; growing the scratch inside
        // getNextAudioBlock would allocate on the audio thread.
        scratch.setSize(2, juce::jmax(4096, 2 * samplesPerBlockExpected + 64), false, false,
                        true);

        gainStepPerSample =
            sampleRate > 0.0 ? static_cast<float>(1.0 / (0.010 * sampleRate)) : 0.1f;

        /*
            BufferingAudioSource::prepareToPlay ends in a do/while whose body
            is moveToFrontOfQueue plus Thread::sleep(5)
            (juce_BufferingAudioSource.cpp), so it sleeps once even though
            these sources are constructed with prefill disabled and the loop
            condition is false. The transport calls this synchronously from
            setSource, which made every mix build cost five milliseconds of
            message-thread sleeping per lane - six for a plain job, more once
            a lane has been split further.

            Nothing can pull from this source while the prepare runs: on the
            build path AudioTransportSource::setSource prepares the new
            source before publishing it under its callback lock, and on the
            re-prepare path it holds that lock throughout. Afterwards
            sourcesReady is what keeps the audio thread out.
        */
        preparation = std::thread([this, samplesPerBlockExpected, sampleRate]
                                  {
                                      for (auto& entry : entries)
                                          entry.buffered->prepareToPlay(samplesPerBlockExpected,
                                                                        sampleRate);

                                      sourcesReady.store(true, std::memory_order_release);
                                  });
    }

    void releaseResources() override
    {
        joinPreparation();

        for (auto& entry : entries)
            entry.buffered->releaseResources();

        scratch.setSize(0, 0);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        if (info.numSamples <= 0)
            return;

        /*
            The lanes are still being prepared, which reallocates the cache
            each BufferingAudioSource reads from - reading one now would be
            reading freed memory. The shared clock is left where it is
            rather than run forward over the gap, so the wait shows up as a
            few milliseconds of latency before playback starts, not as
            audio missing from the front of it.
        */
        if (!sourcesReady.load(std::memory_order_acquire))
            return;

        // One consistent read position for every stem this call; a
        // concurrent seek from the message thread wins the final exchange
        // and simply takes effect next call.
        const auto blockStart = position.load(std::memory_order_acquire);

        // Solo is scoped to what is actually in the mix: a lane whose audio
        // never made it in must not be able to silence everything else.
        const bool anySolo = anySoloActive();

        if (scratch.getNumSamples() < info.numSamples)
            scratch.setSize(2, info.numSamples, false, false, true); // last-resort fallback

        const float maxDelta = gainStepPerSample * static_cast<float>(info.numSamples);

        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto& entry = entries[i];

            const bool audible = isEntryAudible(entry, anySolo);

            const float target = audible ? 1.0f : 0.0f;
            const float previous = currentGains[i];

            const float next =
                previous + juce::jlimit(-maxDelta, maxDelta, target - previous);

            // Silent stems are still pulled, only not mixed: a buffered
            // source that stopped being read would have to refill from a
            // jumped position when it comes back, dropping audio at the
            // start of every unmute.
            //
            // Sequential playback needs no seek: BufferingAudioSource's
            // getNextAudioBlock advances its own nextPlayPos by the samples
            // it delivered (juce_BufferingAudioSource.cpp), so every entry
            // already sits at blockStart and stays sample-aligned with the
            // shared clock. Only a real seek is pushed down - in JUCE 9 the
            // redundant call took two locks and signalled the read thread
            // per stem per block.
            juce::AudioSourceChannelInfo scratchInfo(&scratch, 0, info.numSamples);

            if (entry.buffered->getNextReadPosition() != blockStart)
                entry.buffered->setNextReadPosition(blockStart);

            entry.buffered->getNextAudioBlock(scratchInfo);

            if (next <= 0.0f && previous <= 0.0f)
            {
                currentGains[i] = next;
                continue;
            }

            const auto channels = juce::jmin(info.buffer->getNumChannels(),
                                             scratch.getNumChannels());

            for (int channel = 0; channel < channels; ++channel)
            {
                info.buffer->addFromWithRamp(channel, info.startSample,
                                             scratch.getReadPointer(channel), info.numSamples,
                                             previous, next);
            }

            currentGains[i] = next;
        }

        auto expected = blockStart;
        position.compare_exchange_strong(expected, blockStart + info.numSamples);
    }

    void setNextReadPosition(juce::int64 newPosition) override
    {
        position.store(juce::jmax(static_cast<juce::int64>(0), newPosition),
                       std::memory_order_release);
    }

    juce::int64 getNextReadPosition() const override { return position.load(); }

    juce::int64 getTotalLength() const override
    {
        juce::int64 longest = 0;

        for (const auto& entry : entries)
            longest = juce::jmax(longest, entry.buffered->getTotalLength());

        return longest;
    }

    bool isLooping() const override { return false; }
    void setLooping(bool) override {}

    /** The mixer's own audibility rule, named once so the interface and the
        audio thread cannot drift apart. Reads only relaxed atomics; safe on
        either thread, allocates nothing. */
    bool anySoloActive() const
    {
        for (const auto& entry : entries)
            if (entry.soloed())
                return true;

        return false;
    }

    static bool isEntryAudible(const Entry& entry, bool anySolo)
    {
        return anySolo ? entry.soloed() : !entry.muted();
    }

    /** True when at least one entry that answers to these flags is audible.
        `named` comes back false when no entry in the mix mentions them at
        all - a lane whose file never loaded, or a mix built before this
        lane existed - and the caller must then leave the lane alone rather
        than dim it. Message thread: `entries` is const after construction,
        but the mix itself is swapped from the message thread. */
    bool isAudibleThrough(const StemLabLaneMonitorFlags* flags, bool& named) const
    {
        const bool anySolo = anySoloActive();

        named = false;

        for (const auto& entry : entries)
        {
            const bool mentionsLane =
                std::any_of(entry.chain.begin(), entry.chain.end(),
                            [flags](const auto& f) { return f.get() == flags; });

            if (!mentionsLane)
                continue;

            named = true;

            if (isEntryAudible(entry, anySolo))
                return true;
        }

        return false;
    }

private:
    void joinPreparation()
    {
        if (preparation.joinable())
            preparation.join();

        sourcesReady.store(false, std::memory_order_release);
    }

    std::vector<Entry> entries;
    std::vector<float> currentGains;
    float gainStepPerSample = 0.1f;
    juce::AudioBuffer<float> scratch;
    std::atomic<juce::int64> position{0};

    // Set once every lane has been prepared; read by the audio thread
    // before it touches a single one of them.
    std::atomic<bool> sourcesReady{false};
    std::thread preparation;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabStemMixSource)
};

namespace
{
juce::String timestampForFilename()
{
    return juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

/*
    How close to the end counts as "already at the end" when Play is pressed.

    Scrubber::applySeek maps a click to event.position.x / getWidth(). The
    scrubber is 518 design px wide (880 window - 2*22 panel padding - 34 play
    - 14 gap - 92 time - 14 gap - 150 A/B - 14 gap), so the rightmost pixel is
    517/518 = 0.99807 and never 1.0. On a five-minute source that leaves the
    playhead 300 * (1/518) = 0.58 s inside the track - far outside the 0.01 s
    guard this replaces, which is why the first press played that 0.58 s and
    stopped, and only the second press rewound.

    Half a percent of the length is 2.6 scrubber pixels of headroom. The floor
    keeps short sources usable; the ceiling stops a deliberate seek near the
    end of a long source silently restarting, and still covers click slop for
    sources up to 2.0 / (1/518) = 1036 s (17:16).
*/
constexpr double endGuardMinSeconds = 0.25;
constexpr double endGuardMaxSeconds = 2.0;
constexpr double endGuardFraction = 0.005;

bool transportIsAtEnd(const juce::AudioTransportSource& transport)
{
    const auto length = transport.getLengthInSeconds();

    // Nothing loaded: there is no end to be at. The old inline guard called
    // setPosition(0.0) here, which was a no-op on a transport already at 0.
    if (length <= 0.0)
        return false;

    const auto guard =
        juce::jlimit(endGuardMinSeconds, endGuardMaxSeconds, length * endGuardFraction);

    return transport.getCurrentPosition() >= length - guard;
}

/*  How much audio the disk writer refused, in the terms the user cares
    about. Reported at stop rather than logged when it happens: the count is
    raised on the audio thread, where nothing may allocate or format text.
*/
juce::String describeDroppedCapture(juce::int64 samples, double sampleRate)
{
    if (sampleRate > 0.0)
        return juce::String(static_cast<double>(samples) / sampleRate, 2) + " s";

    return juce::String(samples) + " samples";
}

/*
    Splits a child process's output into lines BEFORE decoding UTF-8.

    The readers used to convert each 4 KB read chunk to a String on its
    own, so a multibyte character straddling a chunk boundary decoded as
    replacement garbage. Stage labels carry "·" on every line now, which
    turned a once-theoretical mojibake into a visible one.
*/
struct Utf8LineBuffer
{
    std::vector<char> pending;

    /*  A carriage-return progress bar carries no newline at all, so nothing
        below can retire its bytes: pip and tqdm redrawing a model download
        (report_downloads in runtime.py hands tqdm a fake TTY precisely so it
        renders one) grew this buffer for the length of the download and then
        emitted the whole thing as a single line. One screenful of redraws is
        the entire useful history of a bar.
    */
    static constexpr size_t maxPendingBytes = 8192;

    template <typename Fn>
    void feed(const char* bytes, int count, Fn&& onLine)
    {
        // Only the bytes that just arrived can complete a line, and the
        // reader feeds a byte at a time: rescanning from the front would
        // make a long run without a newline - a progress bar redrawing
        // over carriage returns - cost time in the square of its length.
        const auto scanFrom = pending.size();

        pending.insert(pending.end(), bytes, bytes + count);

        size_t start = 0;

        for (size_t i = scanFrom; i < pending.size(); ++i)
        {
            if (pending[i] != '\n')
                continue;

            const auto line =
                juce::String::fromUTF8(pending.data() + start, static_cast<int>(i - start))
                    .trimEnd();

            if (line.isNotEmpty())
                onLine(line);

            start = i + 1;
        }

        pending.erase(pending.begin(), pending.begin() + static_cast<long>(start));

        if (pending.size() > maxPendingBytes)
        {
            // The tail after the last carriage return is the bar's current
            // frame; everything before it has already been drawn over. '\r'
            // is ASCII, so cutting there can never split a UTF-8 sequence.
            const auto lastReturn = std::find(pending.rbegin(), pending.rend(), '\r');

            if (lastReturn != pending.rend())
            {
                pending.erase(pending.begin(), lastReturn.base());
            }
            else
            {
                // Not a progress bar, just a process emitting a line longer
                // than the budget: give up what is there rather than hold it.
                const auto line =
                    juce::String::fromUTF8(pending.data(), static_cast<int>(pending.size()))
                        .trimEnd();

                pending.clear();

                if (line.isNotEmpty())
                    onLine(line);
            }
        }

        // The next feed() scans from pending.size() as it stands now, so any
        // truncation above must leave the buffer, not an old offset, as the
        // single source of where scanning resumes.
    }

    template <typename Fn>
    void flush(Fn&& onLine)
    {
        if (pending.empty())
            return;

        const auto line =
            juce::String::fromUTF8(pending.data(), static_cast<int>(pending.size())).trim();

        pending.clear();

        if (line.isNotEmpty())
            onLine(line);
    }
};

/**
 * Matches python, pythonw, python3 and versioned names such as python3.11, on
 * either platform, without also matching neighbours like python-config.
 */
bool looksLikePythonInterpreter(const juce::File& file)
{
    const auto name = file.getFileNameWithoutExtension().toLowerCase();

    return name == "python" || name == "pythonw" || name.startsWith("python3");
}

/**
 * True for the relocatable interpreter shipped inside a portable release, as
 * opposed to a development venv or a system Python.
 *
 *     Windows   Engine\python.exe
 *     Linux     Engine/bin/python3
 */
bool isPortableEngineRuntime(const juce::File& file)
{
    if (!looksLikePythonInterpreter(file))
        return false;

    const auto parent = file.getParentDirectory();

#if JUCE_WINDOWS
    return parent.getFileName().equalsIgnoreCase("Engine");
#else
    return parent.getFileName().equalsIgnoreCase("bin") &&
           parent.getParentDirectory().getFileName().equalsIgnoreCase("Engine");
#endif
}

juce::String utf8ToHex(const juce::String& text)
{
    const auto utf8 = text.toUTF8();
    juce::String hex;

    for (int i = 0; i < utf8.sizeInBytes() - 1; ++i)
    {
        hex += juce::String::toHexString(
                   static_cast<int>(static_cast<unsigned char>(utf8.getAddress()[i])))
                   .paddedLeft('0', 2)
                   .toUpperCase();
    }

    return hex;
}

#if JUCE_WINDOWS
juce::String hresultText(HRESULT result)
{
    return "0x" +
           juce::String::toHexString(static_cast<juce::int64>(static_cast<unsigned long>(result)));
}

struct CoTaskMemWaveFormatDeleter
{
    void operator()(WAVEFORMATEX* value) const noexcept
    {
        if (value != nullptr)
            CoTaskMemFree(value);
    }
};

struct EventHandle
{
    HANDLE value = nullptr;

    ~EventHandle()
    {
        if (value != nullptr)
            CloseHandle(value);
    }
};

struct ComApartment
{
    explicit ComApartment(HRESULT resultIn)
        : result(resultIn), shouldUninitialise(SUCCEEDED(resultIn))
    {
    }

    ~ComApartment()
    {
        if (shouldUninitialise)
            CoUninitialize();
    }

    HRESULT result = E_FAIL;
    bool shouldUninitialise = false;
};
#endif
} // namespace

namespace
{
// Exit code the engine's watchdog uses when it honors a cancel sentinel.
constexpr juce::uint32 engineCancelExitCode = 75;

/*
    Name this process to the engines it launches.

    Their watchdog shuts a job down when the plugin disappears, but it can
    only recognise that if it knows which pid to watch. Deriving it from
    getppid() at watchdog start is too late - the engine spends seconds
    importing torch first, and a host that dies in that window has already
    had the job reparented, so the comparison baseline is the reaper.

    An environment variable carries this rather than a command-line flag:
    an engine too old to know the variable ignores it, while an unknown
    flag would make its argument parser reject the whole job.
*/
void publishParentPidForEngines()
{
    const auto pid = juce::String(static_cast<int>(
#if JUCE_WINDOWS
        GetCurrentProcessId()
#else
        getpid()
#endif
        ));

#if JUCE_WINDOWS
    _putenv_s("STEMLAB_PARENT_PID", pid.toRawUTF8());
#else
    setenv("STEMLAB_PARENT_PID", pid.toRawUTF8(), 1);
#endif
}

/*
    Stop a job process without orphaning its model subprocesses, then make
    sure it is really gone.

    The plugin can only kill its direct child; the torch worker underneath
    would keep burning CPU. Writing the cancel sentinel makes the engine's
    watchdog (polling every 0.5 s) take the whole job down from the inside.
    Engines without the watchdog - and a wedged one - are killed once the
    grace period expires.

    That final kill is also the only thing that can free the reader thread:
    it parks inside ChildProcess::readProcessOutput, which on both platforms
    returns only once the requested bytes arrive or the child closes the
    pipe. No exit flag is observable until the child is gone, so this must
    be callable from a thread other than the reader - hence the lock, which
    guards the ChildProcess object's lifetime (reads happen through a raw
    pointer taken once, which stays valid while this holds the lock).
*/
void stopJobProcess(juce::CriticalSection& processLock,
                    std::unique_ptr<juce::ChildProcess>& process, const juce::File& cancelFile,
                    int graceMilliseconds)
{
    const juce::ScopedLock lock(processLock);

    if (process == nullptr || !process->isRunning())
        return;

    if (cancelFile != juce::File())
        cancelFile.replaceWithText("cancel\n");

    for (int waited = 0; waited < graceMilliseconds && process->isRunning(); waited += 50)
        juce::Thread::sleep(50);

    if (process->isRunning())
        process->kill();
}
} // namespace

class StemLabEngineThread final : public juce::Thread
{
public:
    StemLabEngineThread(StemLabAudioProcessor& ownerIn, juce::StringArray commandIn,
                        juce::File cancelFileIn, juce::File successMarkerIn)
        : juce::Thread("StemLab engine"), owner(ownerIn), command(std::move(commandIn)),
          cancelFile(std::move(cancelFileIn)), successMarker(std::move(successMarkerIn))
    {
    }

    ~StemLabEngineThread() override
    {
        signalThreadShouldExit();

        // The reader is almost always parked inside readProcessOutput, where
        // no exit flag is visible. Take the child down first so the join
        // below is short and never reaches JUCE's force-kill fallback.
        stopChildProcess(3000);

        stopThread(6000);
    }

    /** Sentinel-then-kill; safe from any thread, including while run() reads. */
    void stopChildProcess(int graceMilliseconds)
    {
        stopJobProcess(processLock, process, cancelFile, graceMilliseconds);
    }

    void run() override
    {
        owner.setStatus("Starting...");
        owner.setEngineProgress(0.02);

        // Scoped to this run rather than set at launch, so a job that never
        // starts cannot leave the waveform worker parked at low priority.
        owner.waveformProfiles.setSeparationActive(true);

        // Marks the window in which a STEMLAB_STEM_READY line belongs to the
        // six root lanes. Cleared on every exit below, including the two
        // early ones, so no later reader inherits it.
        owner.mainEngineRunning.store(true);

        juce::ChildProcess* childProcess = nullptr;

        {
            const juce::ScopedLock lock(processLock);
            process = std::make_unique<juce::ChildProcess>();
            childProcess = process.get();
        }

        if (!childProcess->start(command,
                                 juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            owner.setStatus("Could not start StemLab engine",
                            StemLabAudioProcessor::statusFailure);
            owner.appendEngineLog("Failed to launch engine process.\n");
            owner.waveformProfiles.setSeparationActive(false);
            owner.mainEngineRunning.store(false);

            const juce::ScopedLock lock(processLock);
            process.reset();
            return;
        }

        std::array<char, 4096> buffer{};
        Utf8LineBuffer lines;

        auto onLine = [&owner = owner](const juce::String& line)
        { owner.handleEngineOutputLine(line); };

        /*
         * One byte per call, and no sleep while bytes keep coming.
         *
         * JUCE fills the whole requested count before returning on both
         * platforms - fread on POSIX, a PeekNamedPipe/ReadFile loop on
         * Windows - so asking for the buffer delivers a job's output in
         * 4KB steps and holds anything smaller back until the engine
         * exits. Asking for one byte returns as soon as the pipe has
         * anything, so protocol lines reach the UI when the engine prints
         * them rather than in bursts. The read blocks in between, which
         * parks the thread rather than spinning it; the cost is per-byte
         * call overhead on a few hundred KB of text per job, off both the
         * audio and message threads.
         */
        while (!threadShouldExit())
        {
            const auto bytes = childProcess->readProcessOutput(buffer.data(), 1);

            if (bytes > 0)
            {
                lines.feed(buffer.data(), bytes, onLine);
                continue;
            }

            if (!isChildRunning())
                break;

            wait(35);
        }

        while (true)
        {
            const auto bytes = childProcess->readProcessOutput(
                buffer.data(), static_cast<int>(buffer.size()));

            if (bytes <= 0)
                break;

            lines.feed(buffer.data(), bytes, onLine);
        }

        lines.flush(onLine);

        juce::uint32 exitCode = 0;

        {
            const juce::ScopedLock lock(processLock);
            exitCode = process->getExitCode();
            process.reset();
        }

        owner.mainEngineRunning.store(false);

        // Unloading mid-job: the processor is going away, nobody is left to
        // read status, and the members it points at are about to die.
        if (threadShouldExit())
            return;

        owner.waveformProfiles.setSeparationActive(false);

        const auto elapsed = juce::jmax(0.0, (nowMs() - owner.engineStartMs.load()) / 1000.0);

        owner.lastEngineDurationSeconds.store(elapsed);

        if (owner.engineCancelRequested.load() || exitCode == engineCancelExitCode)
        {
            owner.engineCompletedSuccessfully.store(false);
            owner.engineProgress.store(0.0);

            // A cancelled job leaves lanes live otherwise: hasSuccessfulJob()
            // is false, so nothing else would ever clear the stems it did
            // manage to announce.
            owner.resetReadyStemFiles();

            owner.appendEngineLog("Separation cancelled by user.\n");
            owner.setStatus("Separation cancelled");
        }
        else if (exitCode == 0 && successMarker.existsAsFile())
        {
            owner.engineCompletedSuccessfully.store(true);

            // The footer summary quotes this; an adaptive split running
            // later overwrites lastEngineDurationSeconds with its own,
            // much shorter time.
            owner.mainJobDurationSeconds.store(elapsed);

            owner.setEngineProgress(1.0);

            // Queue the new stems' waveform analyses now, from the thread
            // that knows the job finished, rather than waiting for the
            // lanes' first paint to ask one file at a time.
            owner.warmCompletedStemProfiles();

            switch (owner.getHostIntegration())
            {
            case StemLabAudioProcessor::hostIntegrationAbletonLive:
            {
                {
                    const juce::ScopedLock lock(owner.abletonBridgeLock);

                    owner.abletonBridgeStatus =
                        "Stems ready - audition them, choose what you want, then Send Selected";
                }

                owner.abletonBridgeWaitStartMs.store(0.0);
                owner.abletonAckExpected.store(false);

                owner.setStatus("Done - audition stems, then Send Selected");
                break;
            }

            case StemLabAudioProcessor::hostIntegrationReaper:
                owner.setStatus("Done - audition stems, then Insert Stems");
                break;

            case StemLabAudioProcessor::hostIntegrationNone:
            default:
                // A generic VST host has no import bridge: the stems leave
                // by drag, so the done line points at that instead.
                owner.setStatus(owner.isStandaloneApp()
                                    ? "Done - audition stems, then choose what to save"
                                    : "Done - audition stems, then Drag Selected");
                break;
            }
        }
        else
        {
            owner.engineCompletedSuccessfully.store(false);

            // Same reason as the cancel arm, and one case more: a job that
            // fails before announcing anything would otherwise leave the
            // previous job's slots standing.
            owner.resetReadyStemFiles();

            if (!owner.getStatus().startsWithIgnoreCase("Failed - "))
                owner.setStatus("StemLab engine failed - see Settings > Copy diagnostics",
                                StemLabAudioProcessor::statusFailure);

            if (exitCode == 0)
            {
                // A child killed by a signal (the OOM killer on a big model,
                // or a crash inside native torch code) is reaped without an
                // exit status, and getExitCode() then reports 0. Only the
                // job's own manifest proves the run actually finished.
                owner.appendEngineLog(
                    "Engine stopped before writing its manifest - it was terminated"
                    " (out of memory or a crash) rather than finishing.\n");
            }
            else
            {
                owner.appendEngineLog("Engine exit code: " + juce::String(exitCode) + "\n");
            }
        }
    }

private:
    bool isChildRunning()
    {
        const juce::ScopedLock lock(processLock);
        return process != nullptr && process->isRunning();
    }

    StemLabAudioProcessor& owner;
    juce::StringArray command;
    juce::File cancelFile;
    juce::File successMarker;

    // Guards the ChildProcess object's lifetime and its state calls. The
    // blocking read runs outside it through a raw pointer.
    juce::CriticalSection processLock;
    std::unique_ptr<juce::ChildProcess> process;
};

class StemLabRecursiveThread final : public juce::Thread
{
public:
    StemLabRecursiveThread(StemLabAudioProcessor& ownerIn, juce::StringArray commandIn,
                           juce::File manifestFileIn, juce::File cancelFileIn)
        : juce::Thread("StemLab recursive engine"), owner(ownerIn), command(std::move(commandIn)),
          manifestFile(std::move(manifestFileIn)), cancelFile(std::move(cancelFileIn))
    {
    }

    ~StemLabRecursiveThread() override
    {
        signalThreadShouldExit();

        // See StemLabEngineThread: the reader cannot observe the exit flag
        // while it is parked in readProcessOutput, so the child goes first.
        stopChildProcess(3000);

        stopThread(6000);
    }

    /** Sentinel-then-kill; safe from any thread, including while run() reads. */
    void stopChildProcess(int graceMilliseconds)
    {
        stopJobProcess(processLock, process, cancelFile, graceMilliseconds);
    }

    void run() override
    {
        owner.setEngineProgress(0.01);

        // See StemLabEngineThread: low-priority waveform analysis for the
        // duration of the split, restored on every exit from this run.
        owner.waveformProfiles.setSeparationActive(true);

        juce::ChildProcess* childProcess = nullptr;

        {
            const juce::ScopedLock lock(processLock);
            process = std::make_unique<juce::ChildProcess>();
            childProcess = process.get();
        }

        if (!childProcess->start(command,
                                 juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            owner.setStatus("Could not start Recursive Stem Splitting",
                            StemLabAudioProcessor::statusFailure);
            owner.appendEngineLog("Failed to launch recursive engine process.\n");
            owner.waveformProfiles.setSeparationActive(false);

            const juce::ScopedLock lock(processLock);
            process.reset();
            return;
        }

        std::array<char, 4096> buffer{};
        Utf8LineBuffer lines;

        auto onLine = [&owner = owner](const juce::String& line)
        { owner.handleEngineOutputLine(line); };

        /*
         * One byte per call, and no sleep while bytes keep coming.
         *
         * JUCE fills the whole requested count before returning on both
         * platforms - fread on POSIX, a PeekNamedPipe/ReadFile loop on
         * Windows - so asking for the buffer delivers a job's output in
         * 4KB steps and holds anything smaller back until the engine
         * exits. Asking for one byte returns as soon as the pipe has
         * anything, so protocol lines reach the UI when the engine prints
         * them rather than in bursts. The read blocks in between, which
         * parks the thread rather than spinning it; the cost is per-byte
         * call overhead on a few hundred KB of text per job, off both the
         * audio and message threads.
         */
        while (!threadShouldExit())
        {
            const auto bytes = childProcess->readProcessOutput(buffer.data(), 1);

            if (bytes > 0)
            {
                lines.feed(buffer.data(), bytes, onLine);
                continue;
            }

            if (!isChildRunning())
                break;

            wait(35);
        }

        while (true)
        {
            const auto bytes = childProcess->readProcessOutput(
                buffer.data(), static_cast<int>(buffer.size()));

            if (bytes <= 0)
                break;

            lines.feed(buffer.data(), bytes, onLine);
        }

        lines.flush(onLine);

        juce::uint32 exitCode = 0;

        {
            const juce::ScopedLock lock(processLock);
            exitCode = process->getExitCode();
            process.reset();
        }

        if (threadShouldExit())
            return;

        owner.waveformProfiles.setSeparationActive(false);

        const auto elapsed = juce::jmax(0.0, (nowMs() - owner.engineStartMs.load()) / 1000.0);

        owner.lastEngineDurationSeconds.store(elapsed);

        if (owner.engineCancelRequested.load() || exitCode == engineCancelExitCode)
        {
            // The main six-stem job is still complete - only this adaptive
            // split was abandoned, so the bar returns to its finished state.
            owner.engineProgress.store(1.0);
            owner.appendEngineLog("Adaptive split cancelled by user.\n");
            owner.setStatus("Adaptive split cancelled");
        }
        else if (exitCode == 0 && manifestFile.existsAsFile())
        {
            // An exit code of zero only says the engine ran. The manifest
            // can still be unusable, and announcing completion regardless
            // put the three most specific failure reasons in the codebase
            // on screen for microseconds before overwriting them.
            if (owner.finishRecursiveJob(manifestFile))
            {
                owner.setEngineProgress(1.0);
                owner.setStatus("Recursive Stem Splitting complete");
            }
        }
        else
        {
            if (!owner.getStatus().startsWithIgnoreCase("Failed - "))
                owner.setStatus("Recursive Stem Splitting failed - see diagnostics",
                                StemLabAudioProcessor::statusFailure);

            owner.appendEngineLog("Recursive engine exit code: " + juce::String(exitCode) + "\n");
        }
    }

private:
    bool isChildRunning()
    {
        const juce::ScopedLock lock(processLock);
        return process != nullptr && process->isRunning();
    }

    StemLabAudioProcessor& owner;
    juce::StringArray command;
    juce::File manifestFile;
    juce::File cancelFile;

    juce::CriticalSection processLock;
    std::unique_ptr<juce::ChildProcess> process;
};

class StemLabUtilityThread final : public juce::Thread
{
public:
    enum Kind
    {
        sourceAnalysis,
        analysisMaintenance,
        midiConversion
    };

    StemLabUtilityThread(StemLabAudioProcessor& ownerIn, Kind kindIn, juce::StringArray commandIn,
                         juce::File sourceIn, juce::File outputIn, juce::String labelIn = {},
                         juce::String contextIn = {}, juce::File cancelFileIn = {})
        : juce::Thread(kindIn == sourceAnalysis
                           ? "StemLab source analysis"
                           : (kindIn == analysisMaintenance ? "StemLab analysis maintenance"
                                                            : "StemLab MIDI")),
          owner(ownerIn), kind(kindIn), command(std::move(commandIn)), source(std::move(sourceIn)),
          output(std::move(outputIn)), label(std::move(labelIn)), context(std::move(contextIn)),
          cancelFile(std::move(cancelFileIn))
    {
    }

    ~StemLabUtilityThread() override
    {
        requestCancel();
        signalThreadShouldExit();

        // Same reason as the engine threads: this thread parks inside
        // readProcessOutput, where no exit flag is visible, so the child has
        // to go first or the join below runs to its timeout and JUCE
        // force-kills the thread.
        stopChildProcess(1500);

        stopThread(2500);
    }

    /** Sentinel-then-kill; safe from any thread, including while run() reads. */
    void stopChildProcess(int graceMilliseconds)
    {
        const juce::ScopedLock lock(processLock);

        if (process == nullptr || !process->isRunning())
            return;

        if (cancelFile.getFullPathName().isNotEmpty())
            cancelFile.replaceWithText("cancel\n");

        for (int waited = 0; waited < graceMilliseconds && process->isRunning(); waited += 50)
            juce::Thread::sleep(50);

        if (process->isRunning())
            process->kill();
    }

    bool requestCancel()
    {
        if (!isThreadRunning() || cancelRequested.exchange(true))
            return false;

        cancelStartedMs.store(nowMs());

        // Writing the sentinel lets the job stop itself cleanly; the kill
        // that follows the grace period is what frees this thread's blocked
        // read if it does not.
        if (cancelFile.getFullPathName().isNotEmpty())
            cancelFile.replaceWithText("cancel\n");

        return true;
    }


    void run() override
    {
        juce::ChildProcess* childProcess = nullptr;

        {
            const juce::ScopedLock lock(processLock);
            process = std::make_unique<juce::ChildProcess>();
            childProcess = process.get();
        }

        if (!childProcess->start(command,
                                 juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            // Every finish() below sends the user to the diagnostics, so a
            // launch that produced no output at all still has to leave
            // something there to read. The thread name says which of the
            // three workers it was - the separation thread needs no such
            // qualifier because only one job of its kind ever runs.
            owner.appendEngineLog("Failed to launch " + getThreadName() + " process.\n");

            finish(-1);

            const juce::ScopedLock lock(processLock);
            process.reset();
            return;
        }

        std::array<char, 4096> buffer{};

        // Raw bytes, decoded once at the end: a byte-at-a-time read hands
        // over single bytes, and converting each one on its own would split
        // every multibyte character and re-walk the string on each append.
        std::vector<char> processBytes;
        Utf8LineBuffer lines;

        auto onLine = [&owner = owner](const juce::String& line)
        { owner.handleEngineOutputLine(line); };

        auto consumeChunk = [&](int bytes)
        {
            processBytes.insert(processBytes.end(), buffer.data(), buffer.data() + bytes);

            if (kind == sourceAnalysis)
                lines.feed(buffer.data(), bytes, onLine);
        };

        /*
         * One byte per call, and no sleep while bytes keep coming.
         *
         * JUCE fills the whole requested count before returning on both
         * platforms - fread on POSIX, a PeekNamedPipe/ReadFile loop on
         * Windows - so asking for the buffer delivers a job's output in
         * 4KB steps and holds anything smaller back until the engine
         * exits. Asking for one byte returns as soon as the pipe has
         * anything, so protocol lines reach the UI when the engine prints
         * them rather than in bursts. The read blocks in between, which
         * parks the thread rather than spinning it; the cost is per-byte
         * call overhead on a few hundred KB of text per job, off both the
         * audio and message threads.
         */
        while (!threadShouldExit() && isChildRunning())
        {
            const auto bytes = childProcess->readProcessOutput(buffer.data(), 1);

            if (bytes > 0)
            {
                consumeChunk(bytes);
                continue;
            }

            wait(50);
        }

        if (threadShouldExit())
            stopChildProcess(0);

        while (true)
        {
            const auto bytes = childProcess->readProcessOutput(
                buffer.data(), static_cast<int>(buffer.size()));
            if (bytes <= 0)
                break;
            consumeChunk(bytes);
        }

        if (kind == sourceAnalysis)
            lines.flush(onLine);

        int exitCode = 0;

        {
            const juce::ScopedLock lock(processLock);
            exitCode = static_cast<int>(process->getExitCode());
            process.reset();
        }

        if (!processBytes.empty() && kind != sourceAnalysis)
        {
            const auto processOutput = juce::String::fromUTF8(
                processBytes.data(), static_cast<int>(processBytes.size()));

            owner.appendEngineLog(processOutput.endsWithChar('\n') ? processOutput
                                                                   : processOutput + "\n");
        }

        if (!threadShouldExit() || cancelRequested.load())
        {
            /*
             * The append above is whatever the worker chose to print, and a
             * worker can die having printed nothing: JUCE's POSIX
             * ChildProcess forks and the child _exit()s after a failed
             * execvp, so a missing or non-executable interpreter still
             * makes start() succeed and then reports 255 with an empty
             * pipe. Without this line the "see diagnostics" that
             * finishAnalysisMaintenance() and finishMidiConversion() point
             * at opens an empty log and a greyed-out Copy item.
             *
             * Same line and same reason as the separation thread. A cancel
             * is not a failure and stays out of the log; the surrounding
             * guard already keeps a teardown-driven exit quiet.
             */
            if (exitCode != 0 && !cancelRequested.load())
                owner.appendEngineLog("Engine exit code: " + juce::String(exitCode) + "\n");

            finish(exitCode);
        }
    }

private:
    bool isChildRunning()
    {
        const juce::ScopedLock lock(processLock);
        return process != nullptr && process->isRunning();
    }

    void finish(int exitCode)
    {
        // The cancellation file is a one-run sentinel. Never leave it in the temp
        // directory where a later source-analysis job could inherit it.
        if (cancelFile.existsAsFile())
            cancelFile.deleteFile();

        if (kind == sourceAnalysis)
            owner.finishSourceAnalysis(source, output, exitCode);
        else if (kind == analysisMaintenance)
            owner.finishAnalysisMaintenance(source, label, exitCode);
        else
            owner.finishMidiConversion(label, output, exitCode, context);
    }

    StemLabAudioProcessor& owner;
    Kind kind;
    juce::StringArray command;
    juce::File source;
    juce::File output;
    juce::String label;
    juce::String context;
    juce::File cancelFile;
    std::atomic<bool> cancelRequested{false};
    std::atomic<double> cancelStartedMs{0.0};

    juce::CriticalSection processLock;
    std::unique_ptr<juce::ChildProcess> process;
};

#if JUCE_WINDOWS
class StemLabSystemLoopbackThread final : public juce::Thread
{
public:
    StemLabSystemLoopbackThread(StemLabAudioProcessor& ownerIn, juce::File outputFileIn)
        : juce::Thread("StemLab WASAPI loopback"), owner(ownerIn),
          outputFile(std::move(outputFileIn))
    {
    }

    ~StemLabSystemLoopbackThread() override
    {
        signalThreadShouldExit();
        stopThread(4000);
    }

    bool wasSuccessful() const noexcept { return successful.load(); }

    void run() override
    {
        using Microsoft::WRL::ComPtr;

        const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ComApartment comApartment(comResult);

        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        {
            fail("Could not initialise Windows audio COM: " + hresultText(comResult));
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;

        auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator));

        if (FAILED(hr))
        {
            fail("Could not open Windows audio devices: " + hresultText(hr));
            return;
        }

        ComPtr<IMMDevice> renderDevice;

        // Capture the current Windows default playback endpoint. For the
        // user's current setup, if Windows is playing through the Focusrite,
        // this captures that Focusrite-bound system mix directly.
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);

        if (FAILED(hr))
        {
            fail("Could not find the default Windows output: " + hresultText(hr));
            return;
        }

        ComPtr<IAudioClient> audioClient;

        hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(audioClient.GetAddressOf()));

        if (FAILED(hr))
        {
            fail("Could not open the Windows output for loopback: " + hresultText(hr));
            return;
        }

        WAVEFORMATEX* rawMixFormat = nullptr;
        hr = audioClient->GetMixFormat(&rawMixFormat);

        std::unique_ptr<WAVEFORMATEX, CoTaskMemWaveFormatDeleter> mixFormat(rawMixFormat);

        if (FAILED(hr) || mixFormat == nullptr)
        {
            fail("Could not read the Windows output mix format: " + hresultText(hr));
            return;
        }

        enum class SourceFormat
        {
            float32,
            pcm16,
            pcm24,
            pcm32,
            unsupported
        };

        SourceFormat sourceFormat = SourceFormat::unsupported;

        bool isFloat = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = mixFormat->wFormatTag == WAVE_FORMAT_PCM;

        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mixFormat->cbSize >=
                static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
        {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat.get());

            isFloat = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

            isPcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
        }

        if (isFloat && mixFormat->wBitsPerSample == 32)
            sourceFormat = SourceFormat::float32;
        else if (isPcm && mixFormat->wBitsPerSample == 16)
            sourceFormat = SourceFormat::pcm16;
        else if (isPcm && mixFormat->wBitsPerSample == 24)
            sourceFormat = SourceFormat::pcm24;
        else if (isPcm && mixFormat->wBitsPerSample == 32)
            sourceFormat = SourceFormat::pcm32;

        if (sourceFormat == SourceFormat::unsupported)
        {
            fail("Unsupported Windows loopback format: " + juce::String(mixFormat->wBitsPerSample) +
                 "-bit");
            return;
        }

        const int sourceChannels = juce::jmax(1, static_cast<int>(mixFormat->nChannels));

        const int outputChannels = juce::jlimit(1, 2, sourceChannels);

        const double sampleRate = static_cast<double>(mixFormat->nSamplesPerSec);

        if (sampleRate <= 0.0)
        {
            fail("Windows loopback returned an invalid sample rate");
            return;
        }

        // Use timer/polling loopback instead of EVENTCALLBACK. Some WASAPI
        // endpoints reject LOOPBACK | EVENTCALLBACK with
        // AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021), even though ordinary
        // shared-mode loopback works correctly.
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0,
                                     mixFormat.get(), nullptr);

        if (FAILED(hr))
        {
            fail("Could not start WASAPI loopback mode: " + hresultText(hr));
            return;
        }

        ComPtr<IAudioCaptureClient> captureClient;

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient),
                                     reinterpret_cast<void**>(captureClient.GetAddressOf()));

        if (FAILED(hr))
        {
            fail("Could not open the WASAPI capture client: " + hresultText(hr));
            return;
        }

        auto fileStream = std::make_unique<juce::FileOutputStream>(outputFile);

        if (!fileStream->openedOk())
        {
            fail("Could not create the system-audio recording file");
            return;
        }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
        const auto options = juce::AudioFormatWriter::Options{}
                                 .withSampleRate(sampleRate)
                                 .withNumChannels(outputChannels)
                                 .withBitsPerSample(24);
        auto formatWriter = wav.createWriterFor(stream, options);

        if (formatWriter == nullptr)
        {
            fail("Could not create the system-audio WAV writer");
            return;
        }

        // Two seconds of margin at whatever rate the endpoint runs: once
        // the FIFO is full, write() discards rather than waits.
        auto writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            formatWriter.release(), owner.diskWriterThread,
            juce::jmax(65536, static_cast<int>(sampleRate * 2.0)));

        // currentSampleRate/currentInputChannels belong to the host's
        // prepareToPlay and are plain members; writing them from this
        // capture thread raced that. systemCaptureSampleRate is the atomic
        // the duration readout actually consults.
        owner.systemCaptureSampleRate.store(sampleRate);
        owner.capturedSamples.store(0);
        owner.droppedCaptureSamples.store(0);

        hr = audioClient->Start();

        if (FAILED(hr))
        {
            fail("Could not start Windows loopback capture: " + hresultText(hr));
            return;
        }

        // Only now is the recording certain, so only now does the loaded
        // source give way to it: every failure above leaves the user with
        // the file they had, and fail()'s explanation appended to that
        // file's own diagnostics.
        owner.beginSystemCaptureSource(outputFile);

        bool captureFailed = false;

        while (!threadShouldExit())
        {
            // Timer-driven loopback is intentionally conservative. A 5 ms
            // poll interval is tiny compared with stem-separation workloads
            // and avoids endpoint-specific event-callback incompatibilities.
            wait(5);

            UINT32 nextPacketFrames = 0;

            hr = captureClient->GetNextPacketSize(&nextPacketFrames);

            if (FAILED(hr))
            {
                fail("Could not read Windows loopback packet size: " + hresultText(hr));
                captureFailed = true;
                break;
            }

            while (nextPacketFrames > 0 && !threadShouldExit())
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                hr =
                    captureClient->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);

                if (FAILED(hr))
                {
                    fail("Could not read Windows loopback audio: " + hresultText(hr));
                    captureFailed = true;
                    break;
                }

                juce::AudioBuffer<float> converted(outputChannels, static_cast<int>(frames));

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    converted.clear();
                }
                else
                {
                    const auto bytesPerSample = static_cast<int>(mixFormat->wBitsPerSample / 8);

                    const auto blockAlign = static_cast<int>(mixFormat->nBlockAlign);

                    for (int channel = 0; channel < outputChannels; ++channel)
                    {
                        auto* destination = converted.getWritePointer(channel);

                        for (UINT32 frame = 0; frame < frames; ++frame)
                        {
                            const auto* sample =
                                data +
                                static_cast<size_t>(frame) * static_cast<size_t>(blockAlign) +
                                static_cast<size_t>(channel) * static_cast<size_t>(bytesPerSample);

                            float value = 0.0f;

                            switch (sourceFormat)
                            {
                            case SourceFormat::float32:
                            {
                                float input = 0.0f;
                                std::memcpy(&input, sample, sizeof(float));
                                value = input;
                                break;
                            }

                            case SourceFormat::pcm16:
                            {
                                int16_t input = 0;
                                std::memcpy(&input, sample, sizeof(int16_t));

                                value = static_cast<float>(input) / 32768.0f;
                                break;
                            }

                            case SourceFormat::pcm24:
                            {
                                int32_t input = static_cast<int32_t>(sample[0]) |
                                                (static_cast<int32_t>(sample[1]) << 8) |
                                                (static_cast<int32_t>(sample[2]) << 16);

                                if ((input & 0x00800000) != 0)
                                    input |= static_cast<int32_t>(0xff000000);

                                value = static_cast<float>(input) / 8388608.0f;
                                break;
                            }

                            case SourceFormat::pcm32:
                            {
                                int32_t input = 0;
                                std::memcpy(&input, sample, sizeof(int32_t));

                                value =
                                    static_cast<float>(static_cast<double>(input) / 2147483648.0);
                                break;
                            }

                            case SourceFormat::unsupported:
                                break;
                            }

                            destination[frame] = juce::jlimit(-1.0f, 1.0f, value);
                        }
                    }
                }

                // ThreadedWriter::write returns false and DISCARDS the block
                // when its FIFO is full. Waiting it out here is not an option
                // - the endpoint buffer is still held by GetBuffer and would
                // overrun - so the loss is counted and reported at stop
                // instead of passing for a clean recording.
                if (writer->write(converted.getArrayOfReadPointers(), static_cast<int>(frames)))
                    owner.capturedSamples.fetch_add(static_cast<juce::int64>(frames));
                else
                    owner.droppedCaptureSamples.fetch_add(static_cast<juce::int64>(frames));

                captureClient->ReleaseBuffer(frames);

                if (captureFailed)
                    break;

                nextPacketFrames = 0;
                hr = captureClient->GetNextPacketSize(&nextPacketFrames);

                if (FAILED(hr))
                {
                    fail("Could not continue Windows loopback capture: " + hresultText(hr));
                    captureFailed = true;
                    break;
                }
            }

            if (captureFailed)
                break;
        }

        audioClient->Stop();
        writer.reset();

        if (!captureFailed)
            successful.store(true);
    }

private:
    void fail(const juce::String& message)
    {
        successful.store(false);
        owner.capturing.store(false);
        owner.standaloneRecordingMode.store(StemLabAudioProcessor::recordingNone);

        owner.appendEngineLog("System audio recording: " + message + "\n");

        owner.setStatus("System audio recording failed - " + message);
    }

    StemLabAudioProcessor& owner;
    juce::File outputFile;
    std::atomic<bool> successful{false};
};
#endif

StemLabAudioProcessor::StemLabAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& value : stemEnabled)
        value.store(true);

    for (auto& flags : rootMonitorFlags)
        flags = std::make_shared<MonitorFlags>();

    previewFormats.registerBasicFormats();

    // The waveform cache's worker is already up (member construction), but
    // it cannot see this manager until the first file is queued, which no
    // caller can do before this constructor returns.
    waveformFormats.registerBasicFormats();

    diskWriterThread.startThread();
    previewReadThread.startThread();

    publishParentPidForEngines();

    const auto discoveredEngine = discoverEngineCommand();

    if (discoveredEngine.isNotEmpty())
        engineCommand = discoveredEngine;

    if (isStandaloneApp())
    {
        // When the portable Standalone app is launched, remember the exact
        // sibling Engine path for the separately installed VST3. This keeps
        // the multi-gigabyte ML runtime portable instead of copying it into
        // the config directory a second time just to make the plugin work.
        const juce::File discoveredFile(discoveredEngine);

        if (isPortableEngineRuntime(discoveredFile))
        {
            auto settingsDirectory = stemlab::paths::configDirectory();

            if (settingsDirectory.createDirectory())
            {
                settingsDirectory.getChildFile("portable_engine_path.txt")
                    .replaceWithText(discoveredFile.getFullPathName());
            }
        }

        // The processor's own AudioSource override routes between the
        // single-file transport and the stem-mix transport, so the player
        // pulls from it rather than from either transport directly.
        previewPlayer.setSource(this);

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            standaloneDeviceManager = &holder->deviceManager;
            standaloneDeviceManager->addAudioCallback(&previewPlayer);

            // Physical input recording needs JUCE's standalone input enabled.
            // StemLab clears the standalone processor output below, so that
            // live input is never monitored back to the speakers.
            holder->getMuteInputValue().setValue(false);
        }
#endif
    }
}

StemLabAudioProcessor::~StemLabAudioProcessor()
{
    stopCapture();
    stopStandalonePlayback();

    if (standaloneDeviceManager != nullptr)
        standaloneDeviceManager->removeAudioCallback(&previewPlayer);

    standaloneDeviceManager = nullptr;

    previewPlayer.setSource(nullptr);
    previewTransport.setSource(nullptr);
    previewReaderSource.reset();
    stemMixTransport.setSource(nullptr);
    stemMixSource.reset();

    if (engineThread != nullptr)
    {
        engineThread->signalThreadShouldExit();
        engineThread.reset();
    }

    if (recursiveThread != nullptr)
    {
        recursiveThread->signalThreadShouldExit();
        recursiveThread.reset();
    }

    analysisThread.reset();
    midiThread.reset();

    // Nothing left to hand over, so the poll stops before the thread it
    // watches is joined by the reset below.
    captureStopTimer.stopTimer();

#if JUCE_WINDOWS || JUCE_LINUX
    systemLoopbackThread.reset();
#endif

    diskWriterThread.stopThread(2000);

    // After both transports have been cleared above, so no buffering source
    // is still expecting to be fed.
    previewReadThread.stopThread(2000);
}

void StemLabAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentInputChannels = juce::jmax(1, getTotalNumInputChannels());

    reserveMidiAuditionEvents();

    if (!isStandaloneApp())
    {
        previewTransport.prepareToPlay(samplesPerBlock, sampleRate);
        stemMixTransport.prepareToPlay(samplesPerBlock, sampleRate);

        // Generous headroom: some hosts deliver blocks larger than they
        // announced (offline bounces especially), and growing this buffer
        // inside processBlock would allocate on the audio thread.
        previewScratch.setSize(juce::jmax(1, getTotalNumOutputChannels()),
                               juce::jmax(4096, 4 * samplesPerBlock), false, false, true);
    }
}

void StemLabAudioProcessor::releaseResources()
{
    // Do not tear down WASAPI loopback just because JUCE/Live reconfigures
    // the normal device. System audio is captured independently.
    if (standaloneRecordingMode.load() != recordingSystem)
        stopCapture();

    if (!isStandaloneApp())
    {
        previewTransport.releaseResources();
        stemMixTransport.releaseResources();
        previewScratch.setSize(0, 0);
    }
}

void StemLabAudioProcessor::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    reserveMidiAuditionEvents();

    previewTransport.prepareToPlay(samplesPerBlockExpected, sampleRate);
    stemMixTransport.prepareToPlay(samplesPerBlockExpected, sampleRate);
}

void StemLabAudioProcessor::reserveMidiAuditionEvents()
{
    // Room for the note-ons and note-offs of one block many times over; the
    // point is only that renderMidiAudition never has to grow it.
    const juce::ScopedLock lock(midiAuditionLock);
    midiAuditionEvents.ensureSize(4096);
}

void StemLabAudioProcessor::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // Same rule as the VST path: an audition replaces the monitor mix while
    // it plays rather than sounding on top of it.
    if (midiAuditionActive.load())
    {
        bufferToFill.clearActiveBufferRegion();
        renderMidiAudition(*bufferToFill.buffer, bufferToFill.startSample,
                           bufferToFill.numSamples);
        return;
    }

    activeTransport().getNextAudioBlock(bufferToFill);
}

bool StemLabAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    if (input != output)
        return false;

    return input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo();
}

void StemLabAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto inputChannels = getTotalNumInputChannels();

    const auto outputChannels = getTotalNumOutputChannels();

    for (int channel = inputChannels; channel < outputChannels; ++channel)
    {
        buffer.clear(channel, 0, buffer.getNumSamples());
    }

    if (!isStandaloneApp())
    {
        if (auto* hostPlayHead = getPlayHead())
        {
            if (auto position = hostPlayHead->getPosition())
            {
                if (auto value = position->getPpqPosition())
                    lastKnownHostPpq.store(juce::jmax(0.0, *value));

                // Feeds the waveform grid's "host" mode.
                if (auto value = position->getBpm())
                    lastHostBpm.store(juce::jlimit(20.0, 400.0, *value));

                if (auto value = position->getTimeSignature())
                {
                    lastHostNumerator.store(juce::jlimit(1, 32, value->numerator));
                    lastHostDenominator.store(juce::jlimit(1, 32, value->denominator));
                }
            }
        }
    }

    // Physical-input and explicit host capture both use this pre-preview input
    // buffer and the same threaded disk writer. System loopback uses WASAPI.
    //
    // activeWriter doubles as the armed flag: with no capture running this
    // load is the whole cost, and the lock below is never reached. A writer
    // published between this load and the lock simply starts one block
    // later.
    if (activeWriter.load(std::memory_order_acquire) != nullptr &&
        standaloneRecordingMode.load() != recordingSystem)
    {
        // Uncontended except at the moment recording stops, which is the
        // only time the writer can be destroyed (see stopStandaloneRecording).
        const juce::ScopedLock lock(writerLock);

        if (auto* writer = activeWriter.load(std::memory_order_acquire))
        {
            const auto numSamples = buffer.getNumSamples();

            // ThreadedWriter::write never blocks: a full FIFO returns false
            // and DISCARDS the block. Counting a discarded block as captured
            // is what turned a disk hiccup into a silently time-compressed
            // recording that still read as clean, so the two outcomes are
            // counted apart and the stop path reports the loss.
            if (writer->write(buffer.getArrayOfReadPointers(), numSamples))
                capturedSamples.fetch_add(numSamples);
            else
                droppedCaptureSamples.fetch_add(numSamples);
        }
    }

    // In the VST, monitoring (source, stem mix, or child audition) replaces
    // the source-track audio while its transport is playing. That makes the
    // transport behave like an actual audition/solo rather than layering
    // stems on top of the original song.
    //
    // The transport is pulled even while stopped: AudioTransportSource's
    // stop() spin-waits for its render callback to acknowledge the stop, so
    // gating this pull on isPlaying() would leave the message thread
    // blocked for the full ~1s timeout on every pause or A/B switch. A
    // stopped transport just clears the scratch and returns.
    // A MIDI audition takes over the output entirely while it plays: it is
    // an inspection of one stem's notes, not another layer over the mix.
    if (!isStandaloneApp() && midiAuditionActive.load())
    {
        buffer.clear();
        renderMidiAudition(buffer, 0, buffer.getNumSamples());
        return;
    }

    auto& monitorSource = activeTransport();

    const bool monitorAudible = monitorSource.isPlaying();

    if (!isStandaloneApp() && previewScratch.getNumChannels() > 0)
    {
        const auto requiredSamples = buffer.getNumSamples();

        if (previewScratch.getNumSamples() < requiredSamples)
        {
            previewScratch.setSize(juce::jmax(1, buffer.getNumChannels()), requiredSamples, false,
                                   false, true);
        }

        previewScratch.clear();

        juce::AudioSourceChannelInfo info(&previewScratch, 0, requiredSamples);

        monitorSource.getNextAudioBlock(info);

        if (monitorAudible)
        {
            buffer.clear();

            const auto channels =
                juce::jmin(buffer.getNumChannels(), previewScratch.getNumChannels());

            for (int channel = 0; channel < channels; ++channel)
            {
                buffer.addFrom(channel, 0, previewScratch, channel, 0, requiredSamples);
            }
        }
    }

    // Standalone recording is intentionally silent monitoring. Preview audio
    // is supplied by previewPlayer as a separate AudioDeviceManager callback.
    if (isStandaloneApp())
        buffer.clear();
}

bool StemLabAudioProcessor::isStandaloneApp() const noexcept
{
    return wrapperType == wrapperType_Standalone;
}

bool StemLabAudioProcessor::isAbletonHost() const noexcept
{
    return !isStandaloneApp() && juce::PluginHostType{}.isAbletonLive();
}

stemlab::host::UiMode StemLabAudioProcessor::getHostUiMode() const noexcept
{
    if (isStandaloneApp())
        return stemlab::host::UiMode::standalone;

    if (getHostIntegration() == hostIntegrationAbletonLive)
        return stemlab::host::UiMode::ableton;

    // REAPER's in-process integration is richer than the generic policy
    // knows about; callers dispatch on getHostIntegration() before
    // consulting the UiMode, so a REAPER instance never reaches the
    // generic capture flow.
    return stemlab::host::UiMode::genericVst;
}

bool StemLabAudioProcessor::loadPreviewFile(const juce::File& file, int previewStem)
{
    // Previewing is supported by both wrappers:
    //
    // Standalone -> previewPlayer / AudioDeviceManager callback
    // VST3       -> processBlock() pulls previewTransport into plugin output
    //
    // This used to reject every non-Standalone call, which meant the Ableton
    // Play buttons could never load a source/stem even though the VST preview
    // audio path itself was already implemented.
    if (!file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(previewFormats.createReaderFor(file));

    if (reader == nullptr)
        return false;

    const auto sourceRate = reader->sampleRate;

    previewTransport.stop();
    previewTransport.setSource(nullptr);
    previewReaderSource.reset();

    auto* readerPtr = reader.release();

    previewReaderSource = std::make_unique<juce::AudioFormatReaderSource>(readerPtr, true);

    // Read ahead on previewReadThread: with no buffer the audio thread
    // decodes the WAV itself, so one slow read is a dropout in the host.
    previewTransport.setSource(previewReaderSource.get(), StemLabStemMixSource::readAheadSamples,
                               &previewReadThread, sourceRate);

    previewTransport.setPosition(0.0);
    previewStemIndex.store(previewStem);

    return true;
}

bool StemLabAudioProcessor::setInputAudioFile(const juce::File& file, double startPpq,
                                              const juce::String& sourceLabel)
{
    if (!file.existsAsFile())
    {
        setActionStatus("Selected audio file does not exist");
        return false;
    }

    // 44 bytes is a canonical WAV header, and the two recording paths
    // already treat that as the line between a file and a recording. A
    // zero-byte file used to sail through here and light up Separate,
    // because nothing below this point looks at the file's size again.
    if (file.getSize() <= 44)
    {
        setActionStatus("Selected audio file is empty");
        return false;
    }

    // Loading a source clears the job directory and result state that the
    // running engine thread still reports into. Swapping it mid-job left the
    // finished stems unreachable while the UI announced them as done, so
    // every path in - drag-and-drop, the file chooser, a late Ableton clip
    // reply - is refused until the job ends.
    if (isEngineRunning())
    {
        setActionStatus("Cancel the running job before loading another file");
        return false;
    }

    double duration = 0.0;
    bool previewAvailable = false;

    std::unique_ptr<juce::AudioFormatReader> infoReader(previewFormats.createReaderFor(file));

    if (infoReader != nullptr)
    {
        // A header-only file is big enough to pass the size guard and
        // still decodes to nothing. Refuse it here, before the first
        // mutation below: everything past this point overwrites the
        // channel count, the sample count and finally captureFile itself,
        // and a later refusal would leave the old source destroyed with
        // no new one in its place.
        if (infoReader->lengthInSamples <= 0 || infoReader->sampleRate <= 0.0)
        {
            infoReader.reset();
            setActionStatus("Selected audio file contains no audio");
            return false;
        }

        currentInputChannels = static_cast<int>(infoReader->numChannels);

        if (infoReader->sampleRate > 0.0)
        {
            duration = static_cast<double>(infoReader->lengthInSamples) / infoReader->sampleRate;
        }

        capturedSamples.store(infoReader->lengthInSamples);

        infoReader.reset();

        previewAvailable = loadPreviewFile(file, -1);
    }
    else
    {
        // This is intentionally not fatal. The Python engine normalizes
        // compressed/container audio with FFmpeg before RoFormer, so a file
        // may be perfectly separable even when JUCE has no source-preview
        // decoder for that specific format.
        capturedSamples.store(0);
        inputDurationSeconds.store(0.0);

        previewTransport.stop();
        previewTransport.setSource(nullptr);
        previewReaderSource.reset();
        previewStemIndex.store(-2);
    }

    inputDurationSeconds.store(juce::jmax(0.0, duration));

    {
        const juce::ScopedLock lock(stateLock);
        captureFile = file;
        lastJobDirectory = juce::File();
        engineLogChunks.clear();
        engineLogBytes = 0;
        reaperSourceInfo = {};

        inputSourceLabel = sourceLabel.isNotEmpty() ? sourceLabel : file.getFileName();
    }

    captureStartPpq.store(juce::jmax(0.0, startPpq));

    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);
    clearRecursiveResults();

    // The previous job's stems are gone as far as monitoring is concerned,
    // and so are the ranges swept over them: they are normalised against a
    // file that is no longer loaded, and they keep steering the transport
    // and trimming every drag and save until something clears them.
    // Clearing after the unload is what guarantees rebuildLoopRegions takes
    // its empty-set early return instead of seeking a transport mid-swap.
    unloadStemMix();
    clearAllMonitorFlags();
    clearAllStemSelectionRanges();

    if (isAbletonHost())
    {
        const juce::ScopedLock lock(abletonBridgeLock);
        abletonBridgeStatus = "Source ready - Separate All Stems";
    }

    // Source analysis is optional, but its results must never outlive the
    // file they describe: clear them for every new source, then re-run only
    // if the user has the analysis enabled.
    sourceBpm.store(-1.0);
    sourceDetectedBpm.store(-1.0);
    sourceHalfBpm.store(-1.0);
    sourceDoubleBpm.store(-1.0);
    sourceBarOne.store(0.0);
    sourceMeterNumerator.store(4);
    sourceMeterDenominator.store(4);
    sourceAnalysisCorrected.store(false);

    {
        const juce::ScopedLock lock(stateLock);
        sourceKey.clear();
        sourceHash.clear();
        sourceAnalysisDevice.clear();
        sourceBeatModel.clear();
        sourceKeyCandidates.clear();
        sourceBeats.clear();
        sourceDownbeats.clear();
    }

    // The source file is known now; queue its waveform analysis rather
    // than leaving it for a first paint to ask.
    waveformProfiles.warm(file);

    // Loading a source is a user change, so it reports in the header; the
    // work line drops back to idle instead of keeping the last job's text.
    setStatus("Ready");
    setActionStatus(previewAvailable
                        ? "Source ready"
                        : "Source ready - preview unavailable until stems are made");

    if (beatThisEnabled.load())
        startSourceAnalysis(file);

    return true;
}

bool StemLabAudioProcessor::setStandaloneInputFile(const juce::File& file)
{
    if (!isStandaloneApp())
        return false;

    return setInputAudioFile(file, 0.0, file.getFileName());
}

juce::String StemLabAudioProcessor::getInputSourceLabel() const
{
    const juce::ScopedLock lock(stateLock);
    return inputSourceLabel;
}

void StemLabAudioProcessor::toggleStandalonePlayback()
{
    if (capturing.load())
        return;

    const auto source = getCaptureFile();

    if (!source.existsAsFile())
    {
        // The source can vanish under a running transport - deleted,
        // unmounted, a temp file swept up. Returning here left the audio
        // playing with the only stop control pointing at a file that is no
        // longer there, so this path stops instead of doing nothing.
        if (previewTransport.isPlaying())
        {
            previewTransport.stop();
            setActionStatus("Source file is gone - stopped");
        }
        else
        {
            setActionStatus("Source file is gone");
        }

        return;
    }

    if (previewStemIndex.load() != -1)
    {
        if (!loadPreviewFile(source, -1))
            return;
    }

    if (previewTransport.isPlaying())
    {
        previewTransport.stop();
        setActionStatus("Source paused");
        return;
    }

    if (transportIsAtEnd(previewTransport))
        previewTransport.setPosition(0.0);

    previewTransport.start();
    startLoopTimerIfRegions();
    setActionStatus("Playing source");
}

juce::File StemLabAudioProcessor::getCompletedStemFile(int index) const
{
    if (!juce::isPositiveAndBelow(index, stemCount))
        return {};

    const auto job = getLastJobDirectory();
    const bool jobDone = engineCompletedSuccessfully.load();

    {
        // The UI asks for all six of these several times per redraw, at
        // 20 Hz, for as long as the editor is open. Enumerating the job
        // tree each time pegged a core once a job had finished - and worse
        // on a network share - so one scan serves every lookup until the
        // job state itself changes.
        //
        // Answered before job.isDirectory(), which is itself a stat and was
        // paying for the directory on every hit. What the cache is keyed on
        // - the job and its completion state - is exactly what invalidates
        // it, so a job tree deleted underneath a loaded one surfaces the
        // same way it always did: through that key changing.
        const juce::ScopedLock lock(stemFileCacheLock);

        if (stemFileCacheJob == job && stemFileCacheJobDone == jobDone)
            return stemFileCache[static_cast<size_t>(index)];
    }

    if (!job.isDirectory())
        return {};

    auto sourceFolder = job.getChildFile("refined");

    if (!sourceFolder.isDirectory())
        sourceFolder = job.getChildFile("baseline");

    if (!sourceFolder.isDirectory())
        return {};

    juce::Array<juce::File> candidates;
    sourceFolder.findChildFiles(candidates, juce::File::findFiles, true, "*.wav");

    juce::Array<juce::File> flacs;
    sourceFolder.findChildFiles(flacs, juce::File::findFiles, true, "*.flac");

    candidates.addArray(flacs);

    std::array<juce::File, stemCount> resolved;

    for (int stemIndex = 0; stemIndex < stemCount; ++stemIndex)
        resolved[static_cast<size_t>(stemIndex)] =
            matchStemFile(candidates, getStemName(stemIndex));

    {
        const juce::ScopedLock lock(stemFileCacheLock);
        stemFileCacheJob = job;
        stemFileCacheJobDone = jobDone;
        stemFileCache = resolved;
    }

    return resolved[static_cast<size_t>(index)];
}

bool StemLabAudioProcessor::hasCompletedStemFile(int index) const
{
    // Answered from the scan cache above: a file that scan resolved was
    // seen on disk during the scan, so a non-empty answer stands in for
    // existsAsFile() without a stat per stem per tick. A stem deleted
    // externally is picked up when that cache invalidates - a change of
    // job directory or completion state - which is the invalidation the
    // cache already has.
    return getCompletedStemFile(index) != juce::File();
}

void StemLabAudioProcessor::warmCompletedStemProfiles()
{
    // Called from the engine thread the moment a job finishes, so the
    // waveform analyses queue (and the stem-file scan cache fills) before
    // the editor's next tick paints the lanes. Paint's lazy get() stays
    // the fallback.
    for (int i = 0; i < stemCount; ++i)
    {
        const auto stem = getCompletedStemFile(i);

        if (stem != juce::File())
            waveformProfiles.warm(stem);
    }
}

juce::File StemLabAudioProcessor::matchStemFile(const juce::Array<juce::File>& candidates,
                                                const juce::String& stem)
{
    // Most to least specific, mirroring stemlab.audio.find_stem_file: an
    // exact name, then the "{track}_{stem}" convention, then a loose
    // substring. A bare substring alone picks the wrong file when the track
    // itself is named after a stem ("guitar_take_bass.wav" matching guitar).
    juce::File suffixMatch;
    juce::File looseMatch;

    for (const auto& candidate : candidates)
    {
        const auto name = candidate.getFileNameWithoutExtension();

        if (name.equalsIgnoreCase(stem))
            return candidate;

        if (suffixMatch == juce::File() &&
            (name.endsWithIgnoreCase("_" + stem) || name.endsWithIgnoreCase("-" + stem)))
        {
            suffixMatch = candidate;
        }
        else if (looseMatch == juce::File() && name.containsIgnoreCase(stem))
        {
            looseMatch = candidate;
        }
    }

    return suffixMatch != juce::File() ? suffixMatch : looseMatch;
}

void StemLabAudioProcessor::stopStandalonePlayback()
{
    previewTransport.stop();
    stemMixTransport.stop();
}

juce::AudioTransportSource& StemLabAudioProcessor::activeTransport() noexcept
{
    return audioMonitorIsMix.load() ? stemMixTransport : previewTransport;
}

const juce::AudioTransportSource& StemLabAudioProcessor::activeTransport() const noexcept
{
    return audioMonitorIsMix.load() ? stemMixTransport : previewTransport;
}

void StemLabAudioProcessor::unloadStemMix()
{
    stemMixTransport.stop();
    stemMixTransport.setSource(nullptr);
    stemMixSource.reset();
    stemMixJobDirectory = juce::File();
    stemMixTreeGeneration = -1;
    audioMonitorIsMix.store(false);
    monitorMode.store(monitorOriginal);
}

bool StemLabAudioProcessor::ensureStemMixLoaded()
{
    if (!hasSuccessfulJob())
        return false;

    const auto job = getLastJobDirectory();

    int generation = 0;

    {
        const juce::ScopedLock lock(recursiveLock);
        generation = recursiveTreeGeneration;
    }

    // An adaptive split changes which files the mix has to play, so the
    // tree generation is part of the identity of a loaded mix. Decided
    // before any disk call: this runs on every UI tick through
    // refreshStemMixIfNeeded, and the isDirectory() below is a stat. A job
    // tree deleted externally under a loaded mix still surfaces through
    // the job / generation change that reloads or unloads the mix - the
    // same invalidation the stem-file cache relies on.
    if (stemMixSource != nullptr && job == stemMixJobDirectory &&
        generation == stemMixTreeGeneration)
    {
        return true;
    }

    if (!job.isDirectory())
        return false;

    const auto tree = getRecursiveStemItems();

    /*
     * The mix plays the leaves of the stem tree. A root stem that was split
     * further hands its slot to its children - summing both would play the
     * same audio twice - and every leaf, root or child, brings its own
     * lane's solo/mute along.
     */
    struct Lane
    {
        juce::File file;
        std::vector<std::shared_ptr<MonitorFlags>> chain;
    };

    std::vector<Lane> lanes;
    lanes.reserve(static_cast<size_t>(stemCount) + tree.size());

    const auto rootIndexFor = [](const juce::String& name)
    {
        for (int i = 0; i < stemCount; ++i)
            if (getStemName(i).equalsIgnoreCase(name))
                return i;

        return -1;
    };

    /*
     * A leaf answers to its own lane and to every lane above it. Walking the
     * chain here is what keeps a split group's Solo and Mute working: the
     * group itself is no longer in the mix, so its flags would otherwise go
     * nowhere.
     */
    const auto chainFor = [this, &tree, &rootIndexFor](const StemLabRecursiveStemInfo& leaf)
    {
        std::vector<std::shared_ptr<MonitorFlags>> chain;

        chain.push_back(monitorFlagsForRecursive(leaf.id));

        auto parentId = leaf.parentId;

        // Bounded by the tree's depth, and guarded anyway: a manifest that
        // pointed a node at its own ancestor would otherwise spin here.
        for (int depth = 0; depth < 32 && parentId.isNotEmpty(); ++depth)
        {
            const auto parent =
                std::find_if(tree.begin(), tree.end(), [&parentId](const auto& item)
                             { return item.id == parentId; });

            if (parent == tree.end())
                break;

            chain.push_back(monitorFlagsForRecursive(parent->id));
            parentId = parent->parentId;
        }

        if (const auto root = rootIndexFor(leaf.rootStem); root >= 0)
            chain.push_back(monitorFlagsForStem(root));

        return chain;
    };

    for (int i = 0; i < stemCount; ++i)
    {
        const auto rootName = getStemName(i);

        const bool splitFurther =
            std::any_of(tree.begin(), tree.end(), [&rootName](const auto& item)
                        { return item.rootStem.equalsIgnoreCase(rootName); });

        if (splitFurther)
            continue;

        lanes.push_back({getCompletedStemFile(i), {monitorFlagsForStem(i)}});
    }

    for (const auto& item : tree)
        if (!item.hasChildren)
            lanes.push_back({item.file, chainFor(item)});

    std::vector<StemLabStemMixSource::Entry> entries;
    double mixRate = 0.0;

    for (const auto& lane : lanes)
    {
        if (lane.chain.empty() || !lane.file.existsAsFile())
            continue;

        if (std::any_of(lane.chain.begin(), lane.chain.end(),
                        [](const auto& flags) { return flags == nullptr; }))
        {
            continue;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(
            previewFormats.createReaderFor(lane.file));

        if (reader == nullptr || reader->sampleRate <= 0.0)
            continue;

        // All stems of one job share the job's sample rate; a stray
        // mismatch would drift against the shared clock, so skip it rather
        // than sum it out of time.
        if (mixRate > 0.0 && !juce::approximatelyEqual(reader->sampleRate, mixRate))
            continue;

        mixRate = reader->sampleRate;

        StemLabStemMixSource::Entry entry;
        entry.source = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
        entry.chain = lane.chain;
        entries.push_back(std::move(entry));
    }

    if (entries.empty())
        return false;

    const bool wasMixActive = audioMonitorIsMix.load();
    const bool wasPlaying = wasMixActive && stemMixTransport.isPlaying();
    const auto previousPosition = stemMixTransport.getCurrentPosition();

    stemMixTransport.stop();
    stemMixTransport.setSource(nullptr);

    stemMixSource =
        std::make_unique<StemLabStemMixSource>(std::move(entries), previewReadThread);

    // The mix source buffers each stem internally, so the transport itself
    // needs no read-ahead: solo and mute stay live at block rate.
    stemMixTransport.setSource(stemMixSource.get(), 0, nullptr, mixRate);
    stemMixTransport.setPosition(wasMixActive ? previousPosition : 0.0);

    // A split finishing mid-playback rebuilds the mix underneath the user;
    // the clock keeps running rather than stopping on them.
    if (wasPlaying)
    {
        stemMixTransport.start();
        startLoopTimerIfRegions();
    }

    stemMixJobDirectory = job;
    stemMixTreeGeneration = generation;
    return true;
}

void StemLabAudioProcessor::refreshStemMixIfNeeded()
{
    if (hasSuccessfulJob() && !isEngineRunning())
        ensureStemMixLoaded();
}

void StemLabAudioProcessor::switchAudioMonitor(bool useMix)
{
    if (audioMonitorIsMix.load() == useMix)
        return;

    auto& from = useMix ? previewTransport : stemMixTransport;
    auto& to = useMix ? stemMixTransport : previewTransport;

    const auto position = from.getCurrentPosition();
    const bool wasPlaying = from.isPlaying();

    from.stop();

    if (to.getLengthInSeconds() > 0.0)
        to.setPosition(juce::jlimit(0.0, to.getLengthInSeconds(), position));

    audioMonitorIsMix.store(useMix);

    if (wasPlaying)
    {
        to.start();
        startLoopTimerIfRegions();
    }
}

bool StemLabAudioProcessor::isStemMonitorAvailable() { return ensureStemMixLoaded(); }

void StemLabAudioProcessor::setMonitorMode(int mode)
{
    const auto clamped = mode == monitorStems ? monitorStems : monitorOriginal;

    if (clamped == monitorStems && !ensureStemMixLoaded())
        return;

    monitorMode.store(clamped);

    if (clamped == monitorOriginal)
    {
        // The single-file transport may currently hold a completed stem;
        // put the source back before it becomes the monitor.
        const auto source = getCaptureFile();

        if (previewStemIndex.load() != -1 && source.existsAsFile())
        {
            const auto position = activeTransport().getCurrentPosition();
            const bool wasPlaying = activeTransport().isPlaying();

            if (loadPreviewFile(source, -1))
            {
                previewTransport.setPosition(
                    juce::jlimit(0.0, previewTransport.getLengthInSeconds(), position));

                if (wasPlaying && !audioMonitorIsMix.load())
                {
                    previewTransport.start();
                    startLoopTimerIfRegions();
                }
            }
        }
    }

    switchAudioMonitor(clamped == monitorStems);
}

void StemLabAudioProcessor::transportTogglePlay()
{
    if (capturing.load())
        return;

    if (audioMonitorIsMix.load())
    {
        if (!ensureStemMixLoaded())
            return;

        if (stemMixTransport.isPlaying())
        {
            stemMixTransport.stop();
            setActionStatus("Paused");
            return;
        }

        if (transportIsAtEnd(stemMixTransport))
            stemMixTransport.setPosition(0.0);

        stemMixTransport.start();
        startLoopTimerIfRegions();
        setActionStatus("Playing stems");
        return;
    }

    // Original path: the single-file transport. When it holds a completed
    // stem from the legacy per-stem API, fall back to the source so the
    // transport button always means Original.
    toggleStandalonePlayback();
}

void StemLabAudioProcessor::transportSeekNormalised(double normalisedPosition)
{
    const auto clamped = juce::jlimit(0.0, 1.0, normalisedPosition);

    auto& transport = activeTransport();

    const auto length = transport.getLengthInSeconds();

    if (length <= 0.0)
        return;

    transport.setPosition(clamped * length);

    // Keep the inactive monitor aligned so A/B switches stay seamless.
    auto& other = audioMonitorIsMix.load() ? previewTransport : stemMixTransport;

    if (other.getLengthInSeconds() > 0.0)
        other.setPosition(juce::jlimit(0.0, other.getLengthInSeconds(), clamped * length));
}

bool StemLabAudioProcessor::isTransportPlaying() const noexcept
{
    return activeTransport().isPlaying();
}

double StemLabAudioProcessor::getTransportPositionSeconds() const noexcept
{
    return activeTransport().getCurrentPosition();
}

double StemLabAudioProcessor::getTransportLengthSeconds() const noexcept
{
    return activeTransport().getLengthInSeconds();
}

std::shared_ptr<StemLabAudioProcessor::MonitorFlags>
StemLabAudioProcessor::monitorFlagsForStem(int index) const
{
    if (!juce::isPositiveAndBelow(index, stemCount))
        return {};

    return rootMonitorFlags[static_cast<size_t>(index)];
}

std::shared_ptr<StemLabAudioProcessor::MonitorFlags>
StemLabAudioProcessor::monitorFlagsForRecursive(const juce::String& itemId) const
{
    if (itemId.isEmpty())
        return {};

    const juce::ScopedLock lock(recursiveLock);

    // Created on first touch: a lane's flags have to exist before the mix
    // that reads them is built, and before the UI first asks for them.
    auto& flags = recursiveMonitorFlags[itemId];

    if (flags == nullptr)
        flags = std::make_shared<MonitorFlags>();

    return flags;
}

void StemLabAudioProcessor::clearAllMonitorFlags()
{
    for (const auto& flags : rootMonitorFlags)
    {
        flags->solo.store(false);
        flags->mute.store(false);
    }

    const juce::ScopedLock lock(recursiveLock);
    recursiveMonitorFlags.clear();
}

void StemLabAudioProcessor::followSoloIntoStemMix()
{
    /*
     * Solo only means anything inside the stem mix, so pressing it while
     * the monitor is on Original would otherwise change nothing audible.
     * This is also what replaced the old child-audition path: soloing a
     * child lane now plays that child through the same shared clock as
     * every other lane instead of hijacking the single-file transport.
     */
    if (monitorMode.load() != monitorStems)
        setMonitorMode(monitorStems);
}

void StemLabAudioProcessor::setStemSolo(int index, bool solo)
{
    const auto flags = monitorFlagsForStem(index);

    if (flags == nullptr)
        return;

    flags->solo.store(solo);

    if (solo)
        followSoloIntoStemMix();
}

bool StemLabAudioProcessor::isStemSoloed(int index) const
{
    const auto flags = monitorFlagsForStem(index);
    return flags != nullptr && flags->solo.load();
}

void StemLabAudioProcessor::setStemMute(int index, bool mute)
{
    if (const auto flags = monitorFlagsForStem(index))
        flags->mute.store(mute);
}

bool StemLabAudioProcessor::isStemMuted(int index) const
{
    const auto flags = monitorFlagsForStem(index);
    return flags != nullptr && flags->mute.load();
}

void StemLabAudioProcessor::setRecursiveStemSolo(const juce::String& itemId, bool solo)
{
    const auto flags = monitorFlagsForRecursive(itemId);

    if (flags == nullptr)
        return;

    flags->solo.store(solo);

    if (solo)
        followSoloIntoStemMix();
}

bool StemLabAudioProcessor::isRecursiveStemSoloed(const juce::String& itemId) const
{
    const auto flags = monitorFlagsForRecursive(itemId);
    return flags != nullptr && flags->solo.load();
}

void StemLabAudioProcessor::setRecursiveStemMute(const juce::String& itemId, bool mute)
{
    if (const auto flags = monitorFlagsForRecursive(itemId))
        flags->mute.store(mute);
}

bool StemLabAudioProcessor::isRecursiveStemMuted(const juce::String& itemId) const
{
    const auto flags = monitorFlagsForRecursive(itemId);
    return flags != nullptr && flags->mute.load();
}

bool StemLabAudioProcessor::isAnySoloActive() const
{
    return stemMixSource != nullptr && stemMixSource->anySoloActive();
}

bool StemLabAudioProcessor::isLaneAudible(const StemLabLaneMonitorFlags* flags) const
{
    // No mix loaded means nothing is being silenced by anything.
    if (stemMixSource == nullptr || flags == nullptr)
        return true;

    bool named = false;
    const bool audible = stemMixSource->isAudibleThrough(flags, named);

    return named ? audible : true;
}

bool StemLabAudioProcessor::isStemAudible(int index) const
{
    return isLaneAudible(monitorFlagsForStem(index).get());
}

bool StemLabAudioProcessor::isRecursiveStemAudible(const juce::String& itemId) const
{
    return isLaneAudible(monitorFlagsForRecursive(itemId).get());
}

bool StemLabAudioProcessor::startStandaloneRecording()
{
    if (!isStandaloneApp() || capturing.load() || isEngineRunning())
    {
        return false;
    }

    stopStandalonePlayback();

    if (standaloneDeviceManager == nullptr)
    {
        setActionStatus("Audio device is not ready");
        return false;
    }

    auto* device = standaloneDeviceManager->getCurrentAudioDevice();

    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
    {
        setActionStatus("Choose a microphone/interface input in Settings");
        return false;
    }

    const auto sampleRate = device->getCurrentSampleRate();

    if (sampleRate <= 0.0)
    {
        setActionStatus("Audio input sample rate is not ready");
        return false;
    }

    const int activeInputs = device->getActiveInputChannels().countNumberOfSetBits();

    return startThreadedInputCapture("input", sampleRate, juce::jlimit(1, 2, activeInputs), 0.0,
                                     recordingInput, "Recording input...");
}

/*
 * The shared tail of physical-input and host-audio capture (upstream's
 * generic-VST work): everything from the WAV writer to the armed flags is
 * identical, only the source of the samples reaching processBlock differs.
 */
bool StemLabAudioProcessor::startThreadedInputCapture(const juce::String& prefix,
                                                      double sampleRate, int channels,
                                                      double startPpq, int recordingMode,
                                                      const juce::String& recordingStatus)
{
    if (capturing.load() || isEngineRunning() || sampleRate <= 0.0 || channels <= 0)
        return false;

    if (isSystemCaptureStopPending())
    {
        setActionStatus("Still finishing the previous recording");
        return false;
    }

    currentSampleRate = sampleRate;
    currentInputChannels = juce::jlimit(1, 2, channels);

    const auto recordingFile = createRecordingFile(prefix);

    auto fileStream = std::make_unique<juce::FileOutputStream>(recordingFile);

    if (!fileStream->openedOk())
    {
        setStatus("Could not create recording WAV");
        return false;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    const auto options = juce::AudioFormatWriter::Options{}
                             .withSampleRate(currentSampleRate)
                             .withNumChannels(currentInputChannels)
                             .withBitsPerSample(24);
    auto formatWriter = wav.createWriterFor(stream, options);

    if (formatWriter == nullptr)
    {
        setStatus("Could not create recording writer");
        return false;
    }

    // The FIFO is the whole margin against disk jitter: once it is full,
    // write() discards blocks rather than waiting. 32768 samples was 0.68 s
    // at 48 kHz, inside the range of an ordinary flush stall, so budget two
    // seconds at whatever rate the device is actually running.
    const int captureFifoSamples =
        juce::jmax(32768, static_cast<int>(currentSampleRate * 2.0));

    threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
        formatWriter.release(), diskWriterThread, captureFifoSamples);

    {
        const juce::ScopedLock lock(stateLock);
        captureFile = recordingFile;
        lastJobDirectory = juce::File();
        engineLogChunks.clear();
        engineLogBytes = 0;
    }

    capturedSamples.store(0);
    droppedCaptureSamples.store(0);
    inputDurationSeconds.store(0.0);
    captureStartPpq.store(juce::jmax(0.0, startPpq));
    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);

    standaloneRecordingMode.store(recordingMode);

    activeWriter.store(threadedWriter.get(), std::memory_order_release);

    capturing.store(true);
    setStatus(recordingStatus);
    return true;
}

void StemLabAudioProcessor::stopStandaloneRecording()
{
    if (!isStandaloneApp() || standaloneRecordingMode.load() != recordingInput ||
        !capturing.exchange(false))
    {
        return;
    }

    {
        // The audio thread loads activeWriter and then writes through it.
        // Publishing null does not close that window - it may already hold
        // the old pointer - so wait here until any in-flight write is done
        // before the writer is destroyed under it.
        const juce::ScopedLock lock(writerLock);
        activeWriter.store(nullptr, std::memory_order_release);
    }

    threadedWriter.reset();
    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();
    const auto dropped = droppedCaptureSamples.load();

    if (recordingFile.existsAsFile() && setStandaloneInputFile(recordingFile))
    {
        // A short recording that reads as clean is worse than a loud
        // failure: the missing audio is gone either way, but only one of
        // the two tells the user to separate a take that is not the one
        // they played.
        if (dropped > 0)
            setStatus("Input recording ready - "
                      + describeDroppedCapture(dropped, currentSampleRate)
                      + " was lost, the recording disk could not keep up");
        else
            setStatus("Input recording ready");
    }
    else
    {
        setStatus("Input recording stopped");
    }
}

bool StemLabAudioProcessor::startHostAudioCapture()
{
    /*
     * Explicit capture of the audio arriving at the plugin, for hosts with
     * no richer path: not the Standalone (it records its own device), not
     * Ableton (clip bridge), not REAPER (Use Selected Item reads the file).
     */
    if (getHostIntegration() != hostIntegrationNone ||
        !stemlab::host::canStartHostAudioCapture(getHostUiMode(), capturing.load(),
                                                 isEngineRunning()))
    {
        return false;
    }

    stopStandalonePlayback();

    const auto sampleRate = currentSampleRate;
    const auto channels = juce::jlimit(1, 2, getTotalNumInputChannels());

    if (sampleRate <= 0.0 || channels <= 0)
    {
        setActionStatus("Host audio input is not ready");
        return false;
    }

    return startThreadedInputCapture("host", sampleRate, channels,
                                     juce::jmax(0.0, lastKnownHostPpq.load()), recordingHost,
                                     "Capturing host audio...");
}

void StemLabAudioProcessor::stopHostAudioCapture()
{
    if (standaloneRecordingMode.load() != recordingHost || !capturing.exchange(false))
        return;

    {
        // Same window as stopStandaloneRecording: the audio thread may hold
        // the old writer pointer, so wait out any in-flight write first.
        const juce::ScopedLock lock(writerLock);
        activeWriter.store(nullptr, std::memory_order_release);
    }

    threadedWriter.reset();
    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();

    const auto dropped = droppedCaptureSamples.load();

    if (recordingFile.existsAsFile() && recordingFile.getSize() > 44 &&
        setInputAudioFile(recordingFile, captureStartPpq.load(), "Host audio capture"))
    {
        if (dropped > 0)
            setStatus("Host audio capture ready - "
                      + describeDroppedCapture(dropped, currentSampleRate)
                      + " was lost, the recording disk could not keep up");
        else
            setStatus("Host audio capture ready");
    }
    else
    {
        setStatus("Host audio capture stopped - no audio was received");
    }
}

void StemLabAudioProcessor::beginSystemCaptureSource(const juce::File& recordingFile)
{
    {
        const juce::ScopedLock lock(stateLock);
        captureFile = recordingFile;
        lastJobDirectory = juce::File();
        engineLogChunks.clear();
        engineLogBytes = 0;
    }

    inputDurationSeconds.store(0.0);

    // The recording's first sample is the one the writer has just been
    // handed, not the one the click asked for. The beat it belongs on is
    // therefore the host's position now, after the device open the capture
    // thread has been sitting in since the click.
    captureStartPpq.store(isStandaloneApp() ? 0.0 : juce::jmax(0.0, lastKnownHostPpq.load()));

    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);
}

bool StemLabAudioProcessor::startSystemAudioRecording()
{
    if (capturing.load() || isEngineRunning())
    {
        return false;
    }

    // The previous take is still being flushed and its file has not been
    // handed over yet; starting now would repoint captureFile out from
    // under the pending handoff.
    if (isSystemCaptureStopPending())
    {
        setActionStatus("Still finishing the previous recording");
        return false;
    }

    stopStandalonePlayback();

#if JUCE_WINDOWS || JUCE_LINUX
    if (systemLoopbackThread != nullptr)
    {
        systemLoopbackThread->signalThreadShouldExit();
        systemLoopbackThread.reset();
    }

    const auto recordingFile = createRecordingFile("system");

    /*
     * The click gives up nothing that describes the loaded source.
     *
     * Reaching the output monitor is the capture thread's first job, and it
     * fails for reasons only that thread learns about - no PulseAudio or
     * PipeWire server, no monitor source, an endpoint that refuses to
     * start. Its fail() can only append an explanation; it has no way to
     * put back a source, a finished job or a diagnostics log that this
     * click had already discarded. So the handover waits for
     * beginSystemCaptureSource(), which the thread calls once its writer is
     * open - the same order Record In commits in, where the file and the
     * writer are created before startThreadedInputCapture() replaces
     * anything.
     *
     * The two sample counters are the exception, and safe: they only feed
     * the recording-time readout that getCapturedSeconds() consults while
     * `capturing` is set and the drop report the stop path writes, and a
     * capture that never starts now falls back to inputDurationSeconds,
     * which still describes the source. Both threads zero them again when
     * their writer opens.
     */
    capturedSamples.store(0);
    droppedCaptureSamples.store(0);

    standaloneRecordingMode.store(recordingSystem);
    capturing.store(true);

    systemLoopbackThread = std::make_unique<StemLabSystemLoopbackThread>(*this, recordingFile);

#if JUCE_WINDOWS
    setStatus("Recording system audio - Windows default output");
#else
    setStatus("Recording system audio - default output monitor");
#endif

    systemLoopbackThread->startThread();
    return true;
#else
    setStatus("System audio recording is not available on this platform - "
              "record into the host instead");
    return false;
#endif
}

bool StemLabAudioProcessor::isSystemCaptureStopPending() const noexcept
{
    return captureStopTimer.isTimerRunning();
}

void StemLabAudioProcessor::stopSystemAudioRecording()
{
#if JUCE_WINDOWS || JUCE_LINUX
    if (standaloneRecordingMode.load() != recordingSystem && systemLoopbackThread == nullptr)
    {
        return;
    }

    capturing.store(false);

    if (systemLoopbackThread != nullptr)
    {
        systemLoopbackThread->signalThreadShouldExit();
        systemLoopbackThread->notify();

        /*
         * What is left to do is a queue drain plus the ThreadedWriter's
         * destructor flushing its FIFO and rewriting the WAV header - disk
         * work of no fixed duration. Joining it here (stopThread(5000)) is
         * what froze the UI, and its timeout would have escalated to thread
         * cancellation, which this thread's contract forbids because the
         * forced unwind runs through that same writer destructor.
         *
         * So wait only long enough to cover the ordinary case, where the
         * thread is already parked in its 25 ms sleep and exits at once,
         * and hand anything slower to captureStopTimer. Nothing here reads
         * the file, so the tail keeps flushing either way; only the handoff
         * below waits for it.
         */
        if (!systemLoopbackThread->waitForThreadToExit(25))
        {
            // A thread that already failed has said something more useful.
            if (!getStatus().startsWithIgnoreCase("System audio recording failed"))
                setStatus("Finishing system audio recording...");

            captureStopTimer.startTimer(20);
            return;
        }
    }

    finishSystemAudioRecordingStop();
#endif
}

void StemLabAudioProcessor::finishSystemAudioRecordingStop()
{
#if JUCE_WINDOWS || JUCE_LINUX
    // The WAV header is only correct once run() has returned; until then
    // there is nothing to hand to the rest of the plugin.
    if (systemLoopbackThread != nullptr && systemLoopbackThread->isThreadRunning())
        return;

    captureStopTimer.stopTimer();

    bool successful = false;

    if (systemLoopbackThread != nullptr)
    {
        successful = systemLoopbackThread->wasSuccessful();

        // The thread has already left run(), so the destructor's generous
        // wait returns immediately - it stays the backstop, not the path.
        systemLoopbackThread.reset();
    }

    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();
    const auto dropped = droppedCaptureSamples.load();

    if (successful && recordingFile.existsAsFile() && recordingFile.getSize() > 44 &&
        setInputAudioFile(recordingFile, captureStartPpq.load(), "System audio recording"))
    {
        if (dropped > 0)
            setStatus("System audio recording ready - "
                      + describeDroppedCapture(dropped, systemCaptureSampleRate.load())
                      + " was lost, the recording disk could not keep up");
        else
            setStatus("System audio recording ready");
    }
    else if (!getStatus().startsWithIgnoreCase("System audio recording failed"))
    {
        setStatus("System audio recording stopped");
    }
#endif
}

juce::File StemLabAudioProcessor::createRecordingFile(const juce::String& prefix) const
{
    auto folder = stemlab::paths::recordingsDirectory();

    folder.createDirectory();

    return folder.getNonexistentChildFile(prefix + "_" + timestampForFilename(), ".wav", false);
}

juce::File StemLabAudioProcessor::createJobDirectory() const
{
    juce::File root;

    {
        const juce::ScopedLock lock(stateLock);
        root = jobRootDirectory;
    }

    if (!root.isDirectory())
        root = stemlab::paths::jobsDirectory();

    root.createDirectory();

    auto folder = root.getChildFile("job_" + timestampForFilename());

    folder.createDirectory();
    return folder;
}

void StemLabAudioProcessor::setJobRootDirectory(const juce::File& directory)
{
    if (!directory.isDirectory())
        return;

    {
        const juce::ScopedLock lock(stateLock);
        jobRootDirectory = directory;
    }

    // The footer's path readout carries the full path; the feedback line
    // only needs to confirm the change.
    setActionStatus("File location set: " + directory.getFileName());
}

juce::File StemLabAudioProcessor::getJobRootDirectory() const
{
    const juce::ScopedLock lock(stateLock);

    if (jobRootDirectory.isDirectory())
        return jobRootDirectory;

    return stemlab::paths::jobsDirectory();
}

void StemLabAudioProcessor::stopCapture()
{
    const auto mode = standaloneRecordingMode.load();
    if (mode == recordingSystem)
        stopSystemAudioRecording();
    else if (mode == recordingInput)
        stopStandaloneRecording();
    else if (mode == recordingHost)
        stopHostAudioCapture();
}

double StemLabAudioProcessor::getCapturedSeconds() const noexcept
{
    if (!capturing.load())
    {
        const auto duration = inputDurationSeconds.load();

        if (duration > 0.0)
            return duration;
    }

    if (standaloneRecordingMode.load() == recordingSystem)
    {
        const auto captureRate = systemCaptureSampleRate.load();

        if (captureRate > 0.0)
            return static_cast<double>(capturedSamples.load()) / captureRate;
    }

    if (currentSampleRate <= 0.0)
        return 0.0;

    return static_cast<double>(capturedSamples.load()) / currentSampleRate;
}

juce::File StemLabAudioProcessor::getCaptureFile() const
{
    const juce::ScopedLock lock(stateLock);
    return captureFile;
}

bool StemLabAudioProcessor::sendAbletonControlMessage(const juce::String& message)
{
    juce::DatagramSocket socket(false);

    const auto utf8 = message.toRawUTF8();

    const auto length = static_cast<int>(std::strlen(utf8));

    return socket.write("127.0.0.1", 39277, utf8, length) == length;
}

bool StemLabAudioProcessor::requestAbletonSourceClip()
{
    if (getHostIntegration() != hostIntegrationAbletonLive)
        return false;

    if (isStandaloneApp() || capturing.load() || isEngineRunning())
    {
        return false;
    }

    stopStandalonePlayback();

    const auto requestId = juce::Uuid().toString();

    auto replyFolder = stemlab::paths::bridgeTempDirectory();

    replyFolder.createDirectory();

    const auto replyFile = replyFolder.getChildFile("clip_" + requestId + ".json");

    auto legacyFolder = stemlab::paths::legacyBridgeDirectory();

    legacyFolder.createDirectory();

    const auto legacyReply = legacyFolder.getChildFile("stemlab_clip_reply.json");

    if (replyFile.existsAsFile())
        replyFile.deleteFile();

    if (legacyReply.existsAsFile())
        legacyReply.deleteFile();

    {
        const juce::ScopedLock lock(stateLock);
        abletonClipRequestId = requestId;
        abletonClipReplyFile = replyFile;
        abletonLegacyClipReplyFile = legacyReply;
    }

    abletonClipRequestPending.store(true);
    abletonClipRequestStartMs.store(nowMs());

    // 0.9.4+ protocol: tell StemLabRemote exactly where to write the one-shot
    // reply. This avoids Documents/OneDrive latency.
    const auto modernPayload =
        "stemlab_get_clip " + requestId + " " + utf8ToHex(replyFile.getFullPathName());

    const bool modernSent = sendAbletonControlMessage(modernPayload);

    // Compatibility fallback: 0.9.3 and earlier StemLabRemote versions only
    // understand the request-id form and write to
    // Documents/StemLab/Ableton/stemlab_clip_reply.json.
    //
    // Sending both is intentional. New Remote Scripts understand both forms;
    // old scripts will mishandle the first message but immediately receive the
    // second compatible one. The VST accepts whichever valid reply arrives.
    const bool legacySent = sendAbletonControlMessage("stemlab_get_clip " + requestId);

    if (!modernSent && !legacySent)
    {
        abletonClipRequestPending.store(false);
        abletonClipRequestStartMs.store(0.0);
        setStatus("Could not contact StemLabRemote");
        return false;
    }

    setStatus("Getting selected Live clip...");
    return true;
}

void StemLabAudioProcessor::refreshAbletonSourceClipFromDisk()
{
    if (getHostIntegration() != hostIntegrationAbletonLive)
        return;

    if (isStandaloneApp() || !abletonClipRequestPending.load())
    {
        return;
    }

    juce::File modernReply;
    juce::File legacyReply;
    juce::String requestId;

    {
        const juce::ScopedLock lock(stateLock);
        modernReply = abletonClipReplyFile;
        legacyReply = abletonLegacyClipReplyFile;
        requestId = abletonClipRequestId;
    }

    juce::File reply;

    // Prefer the low-latency Temp reply from 0.9.4+ Remote Scripts.
    if (modernReply.existsAsFile())
        reply = modernReply;
    else if (legacyReply.existsAsFile())
        reply = legacyReply;

    if (!reply.existsAsFile())
    {
        const auto started = abletonClipRequestStartMs.load();

        if (started > 0.0 && nowMs() - started > 5000.0)
        {
            abletonClipRequestPending.store(false);
            abletonClipRequestStartMs.store(0.0);

            setStatus("StemLabRemote did not return the clip. Re-select StemLabRemote in Live "
                      "Settings, select the Arrangement audio clip, and try again.");
        }

        return;
    }

    const auto parsed = juce::JSON::parse(reply.loadFileAsString());

    auto* object = parsed.getDynamicObject();

    if (object == nullptr || object->getProperty("protocol").toString() != "stemlab-clip-source")
    {
        // A partially-written/older reply should not permanently poison the
        // request. Leave the request pending and let the next timer tick retry.
        return;
    }

    if (object->getProperty("request_id").toString() != requestId)
    {
        // With an old Remote Script, the first modern-format request can be
        // interpreted as one long request id. The second compatibility request
        // overwrites this legacy file with the correct id shortly afterward.
        return;
    }

    abletonClipRequestPending.store(false);
    abletonClipRequestStartMs.store(0.0);

    const bool success = static_cast<bool>(object->getProperty("success"));

    const auto message = object->getProperty("message").toString();

    const auto path = object->getProperty("path").toString();

    const auto startBeat = static_cast<double>(object->getProperty("start_beat"));

    const auto trackName = object->getProperty("source_track").toString();

    const auto clipName = object->getProperty("clip_name").toString();

    // Clean both possible reply locations regardless of which one won.
    if (modernReply.existsAsFile())
        modernReply.deleteFile();

    if (legacyReply.existsAsFile())
        legacyReply.deleteFile();

    if (!success)
    {
        setStatus(message.isNotEmpty() ? "Live clip: " + message
                                       : "Could not get the selected Live clip");

        return;
    }

    juce::String label;

    if (trackName.isNotEmpty())
        label += trackName;

    if (clipName.isNotEmpty())
    {
        if (label.isNotEmpty())
            label += " / ";

        label += clipName;
    }

    if (label.isEmpty())
        label = juce::File(path).getFileName();

    if (setInputAudioFile(juce::File(path), startBeat, label))
    {
        setActionStatus("Live clip ready");

        {
            const juce::ScopedLock lock(abletonBridgeLock);

            abletonBridgeStatus = "Source ready";
        }
    }
}

bool StemLabAudioProcessor::launchSeparationAndExport()
{
    stopStandalonePlayback();

    if (capturing.load())
    {
        setActionStatus("Finish the capture before separating");
        return false;
    }

    if (isEngineRunning())
        return false;

    const auto source = getCaptureFile();

    if (!source.existsAsFile())
    {
        switch (isStandaloneApp() ? hostIntegrationNone : getHostIntegration())
        {
        case hostIntegrationAbletonLive:
            setActionStatus("Use Live Clip or Record PC first");
            break;
        case hostIntegrationReaper:
            setActionStatus("Select an item in REAPER, then Use Selected Item");
            break;
        case hostIntegrationNone:
        default:
            setActionStatus(isStandaloneApp() ? "Select or record audio first"
                                              : "Capture Host or Record PC first");
            break;
        }

        return false;
    }

    const auto commandName = getEngineCommand().trim();

    if (commandName.isEmpty())
    {
        setActionStatus("Choose the StemLab engine in Settings");
        return false;
    }

    juce::StringArray command;
    command.add(commandName);

    // Portable releases ship a relocatable embedded Python runtime under
    // Engine/ rather than requiring a system Python/venv. When auto-discovery
    // resolves that interpreter, launch StemLab's worker as a module. The old
    // stemlab-plugin-job development path still works.
    {
        const juce::File commandFile(commandName);

        if (looksLikePythonInterpreter(commandFile))
        {
            // For the self-contained Engine, -s keeps the user's ~/.local
            // site-packages from shadowing the Engine's own dependencies. A
            // system or venv interpreter must NOT get it: a user-site
            // "pip install --user -e ." setup depends on user site.
            if (isPortableEngineRuntime(commandFile))
                command.add("-s");

            command.add("-m");
            command.add("stemlab.plugin_job");
        }
    }

    command.add("--input");
    command.add(source.getFullPathName());

    clearRecursiveResults();

    const auto job = createJobDirectory();

    {
        const juce::ScopedLock lock(stateLock);
        lastJobDirectory = job;
        engineLogChunks.clear();
        engineLogBytes = 0;
    }

    if (getHostIntegration() == hostIntegrationAbletonLive)
    {
        const auto ack = job.getChildFile("stemlab_ableton_ack.json");

        if (ack.existsAsFile())
            ack.deleteFile();

        {
            const juce::ScopedLock lock(abletonBridgeLock);
            abletonBridgeStatus = "Separating all six stems...";
        }

        abletonBridgeWaitStartMs.store(0.0);
        abletonAckExpected.store(false);
    }

    command.add("--output");
    command.add(job.getFullPathName());

    command.add("--start-ppq");
    command.add(juce::String(juce::jmax(0.0, captureStartPpq.load()), 8));

    // No --device: the backend's default ("auto" from this release on)
    // resolves the best backend at run time - CUDA, which is also how ROCm
    // answers, then Intel XPU, then CPU. Older engines default to "cuda"
    // and would reject a literal "auto", so omitting the flag is also what
    // keeps a new plugin working against a released 0.9.9 Engine.

    command.add("--engine");
    command.add(getSeparatorEngineId());

    if (!refinementEnabled.load())
        command.add("--no-refine");

    // Separation always produces every stem first. Ableton selection is
    // intentionally deferred until after the user can audition the results.
    command.add("--no-notify");
    command.add("--stems");

    for (int i = 0; i < stemCount; ++i)
        command.add(getStemName(i));

    setStatus("Separating with " + getSeparatorEngineDisplayName() + "...");

    engineCompletedSuccessfully.store(false);

    // Snapshot beside the --no-refine decision above, so the summary this
    // job eventually shows quotes the setting it actually ran with rather
    // than whatever the toggle happens to say when the summary is drawn.
    lastJobRefinement.store(refinementEnabled.load());
    engineProgress.store(0.01);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);
    resetEngineEta();
    engineProgressRate.store(0.0);
    engineCancelRequested.store(false);

    // A leftover sentinel must not cancel the new job the moment its
    // watchdog starts; the watchdog honors any sentinel it ever sees.
    const auto cancelFile = job.getChildFile("stemlab_cancel.txt");
    cancelFile.deleteFile();

    // Likewise a progress file from an earlier run in this directory would
    // be read as this job's progress until the engine overwrites it.
    job.getChildFile("stemlab_progress.txt").deleteFile();

    sawEngineProgressProtocol.store(false);

    // The lanes must not carry a single frame of the previous job's stems
    // into this one; the new job's own announcements refill these.
    resetReadyStemFiles();

    {
        const juce::ScopedLock lock(stateLock);
        activeCancelFile = cancelFile;
    }

    // Bound the waveform-profile cache before the new job's stems fill it:
    // everything but the loaded source is evicted (keyed by file path -
    // see retainOnly), since the completed-state reset above already
    // stopped the lanes drawing the previous job's stems.
    waveformProfiles.retainOnly({source});

    // Exit code 0 alone does not prove success: a child killed by a signal
    // is reaped without a status and reports 0, so the job's own manifest
    // is what the completion handler checks.
    engineThread = std::make_unique<StemLabEngineThread>(
        *this, command, cancelFile, job.getChildFile("stemlab_ableton_manifest.json"));

    engineThread->startThread();
    return true;
}

juce::StringArray
StemLabAudioProcessor::makePythonModuleCommand(const juce::String& moduleName) const
{
    const auto commandName = getEngineCommand().trim();

    if (commandName.isEmpty())
        return {};

    const juce::File commandFile(commandName);
    const auto fileName = commandFile.getFileName();

    juce::StringArray command;

    if (looksLikePythonInterpreter(commandFile))
    {
        command.add(commandName);

        if (isPortableEngineRuntime(commandFile))
            command.add("-s");

        command.add("-m");
        command.add(moduleName);
        return command;
    }

    if (fileName.containsIgnoreCase("stemlab-plugin-job"))
    {
        // Development installs place both workers in the same environment.
        auto recursiveExecutable = commandFile.getSiblingFile(
            fileName.replace("stemlab-plugin-job", "stemlab-recursive-job"));

        if (recursiveExecutable.existsAsFile())
        {
            command.add(recursiveExecutable.getFullPathName());
            return command;
        }
    }

    if (commandName.equalsIgnoreCase("stemlab-plugin-job"))
    {
        command.add("stemlab-recursive-job");
        return command;
    }

    return {};
}

void StemLabAudioProcessor::clearRecursiveResults()
{
    {
        const juce::ScopedLock lock(recursiveLock);
        recursiveItems.clear();
        recursiveMonitorFlags.clear();
        ++recursiveTreeGeneration;
    }

    // Recursive children replace their parent in the default selection for a
    // completed job. Clearing the recursive tree restores the normal six
    // top-level stems for the next source/separation.
    for (auto& value : stemEnabled)
        value.store(true);

    sendChangeMessage();
}

std::vector<StemLabRecursiveStemInfo> StemLabAudioProcessor::getRecursiveStemItems() const
{
    std::vector<StemLabRecursiveStemInfo> snapshot;

    {
        const juce::ScopedLock lock(recursiveLock);
        snapshot = recursiveItems;
    }

    std::vector<StemLabRecursiveStemInfo> ordered;
    ordered.reserve(snapshot.size());

    std::function<void(const juce::String&, int)> appendChildren;
    appendChildren = [&](const juce::String& parentId, int depth)
    {
        for (const auto& item : snapshot)
        {
            if (item.parentId != parentId)
                continue;

            auto copy = item;
            copy.depth = depth;
            ordered.push_back(copy);
            appendChildren(item.id, depth + 1);
        }
    };

    for (int i = 0; i < stemCount; ++i)
        appendChildren(getStemName(i), 1);

    // Keep any future/experimental node visible even if a malformed parent
    // relationship somehow slips into a manifest.
    for (const auto& item : snapshot)
    {
        const auto alreadyPresent =
            std::any_of(ordered.begin(), ordered.end(),
                        [&item](const auto& existing) { return existing.id == item.id; });

        if (!alreadyPresent)
            ordered.push_back(item);
    }

    for (auto& item : ordered)
    {
        const auto prefix = item.id + "/";
        item.hasChildren =
            std::any_of(snapshot.begin(), snapshot.end(), [&prefix](const auto& candidate)
                        { return candidate.id.startsWith(prefix); });
    }

    return ordered;
}

juce::File StemLabAudioProcessor::getRecursiveStemFile(const juce::String& itemId) const
{
    const juce::ScopedLock lock(recursiveLock);

    for (const auto& item : recursiveItems)
        if (item.id == itemId)
            return item.file;

    return {};
}

void StemLabAudioProcessor::setRecursiveStemEnabled(const juce::String& itemId, bool enabled)
{
    {
        const juce::ScopedLock lock(recursiveLock);
        for (auto& item : recursiveItems)
        {
            if (item.id == itemId)
            {
                item.selected = enabled;
                break;
            }
        }
    }

    sendChangeMessage();
}

bool StemLabAudioProcessor::isRecursiveStemEnabled(const juce::String& itemId) const
{
    const juce::ScopedLock lock(recursiveLock);

    for (const auto& item : recursiveItems)
        if (item.id == itemId)
            return item.selected;

    return false;
}

bool StemLabAudioProcessor::finishRecursiveJob(const juce::File& manifestFile)
{
    // The Python side owns separation details. The plugin only consumes the
    // schema-2 tree contract and turns child nodes into selectable UI rows.
    const auto parsed = juce::JSON::parse(manifestFile.loadFileAsString());
    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
    {
        setStatus("Recursive result manifest is invalid", statusFailure);
        return false;
    }

    const auto parentId = object->getProperty("parent_id").toString();
    const auto rootStem = object->getProperty("root_stem").toString();
    auto* children = object->getProperty("children").getArray();

    if (parentId.isEmpty() || rootStem.isEmpty() || children == nullptr)
    {
        setStatus("Recursive result manifest is incomplete", statusFailure);
        return false;
    }

    std::vector<StemLabRecursiveStemInfo> newItems;

    for (const auto& entry : *children)
    {
        auto* child = entry.getDynamicObject();
        if (child == nullptr)
            continue;

        StemLabRecursiveStemInfo item;
        item.id = child->getProperty("id").toString();
        item.label = child->getProperty("label").toString();
        item.parentId = parentId;
        item.rootStem = rootStem;
        item.category = child->getProperty("category").toString();
        item.file = juce::File(child->getProperty("path").toString());
        item.selected = !item.label.containsIgnoreCase("Removed Reverb");
        item.estimatedSourceCount =
            juce::jmax(1, static_cast<int>(child->getProperty("estimated_source_count")));
        item.confidence =
            juce::jlimit(0.0, 1.0, static_cast<double>(child->getProperty("confidence")));

        if (auto* actions = child->getProperty("actions").getArray())
        {
            for (const auto& action : *actions)
                item.actions.addIfNotAlreadyThere(action.toString());
        }

        if (item.id.isNotEmpty() && item.file.existsAsFile())
            newItems.push_back(std::move(item));
    }

    if (newItems.empty())
    {
        setStatus("Recursive split finished without usable audio files", statusFailure);
        return false;
    }

    {
        const juce::ScopedLock lock(recursiveLock);
        const auto prefix = parentId + "/";

        recursiveItems.erase(std::remove_if(recursiveItems.begin(), recursiveItems.end(),
                                            [&](const auto& item)
                                            { return item.id.startsWith(prefix); }),
                             recursiveItems.end());

        // Once a node is split further, default to its children rather than
        // sending/saving both the parent and every child at the same time.
        if (parentId != rootStem)
        {
            for (auto& item : recursiveItems)
                if (item.id == parentId)
                    item.selected = false;
        }

        recursiveItems.insert(recursiveItems.end(), newItems.begin(), newItems.end());

        // New leaves for the monitor mix to play in place of their parent.
        ++recursiveTreeGeneration;
    }

    // The new child lanes ask for these on their next paint; queue the
    // analyses now so the profiles are ready by then.
    for (const auto& item : newItems)
        waveformProfiles.warm(item.file);

    if (parentId == rootStem)
    {
        for (int i = 0; i < stemCount; ++i)
            if (getStemName(i).equalsIgnoreCase(rootStem))
                setStemEnabled(i, false);
    }

    sendChangeMessage();

    return true;
}

bool StemLabAudioProcessor::launchRecursiveStemSplit(int rootStemIndex)
{
    if (!hasSuccessfulJob() || isEngineRunning() ||
        !juce::isPositiveAndBelow(rootStemIndex, stemCount))
    {
        return false;
    }

    const auto rootStem = getStemName(rootStemIndex);
    const bool isVocals = rootStem.equalsIgnoreCase("vocals");
    const bool isDrums = rootStem.equalsIgnoreCase("drums");
    const bool isLeadCandidate = rootStem.equalsIgnoreCase("guitar") ||
                                 rootStem.equalsIgnoreCase("piano") ||
                                 rootStem.equalsIgnoreCase("other");

    if (!isVocals && !isDrums && !isLeadCandidate)
    {
        setStatus("Adaptive splitting is not available for this stem yet");
        return false;
    }

    const auto source = getCompletedStemFile(rootStemIndex);
    if (!source.existsAsFile())
    {
        setStatus("Stem file was not found for adaptive splitting");
        return false;
    }

    auto command = makePythonModuleCommand("stemlab.recursive_job");
    if (command.isEmpty())
    {
        setStatus("Adaptive stem engine could not be located");
        return false;
    }

    const auto output = getLastJobDirectory().getChildFile("recursive").getChildFile(rootStem);

    if (output.isDirectory())
        output.deleteRecursively();
    output.createDirectory();

    const auto operation = isVocals ? juce::String("vocals")
                                    : (isDrums ? juce::String("drums") : juce::String("lead"));

    const auto category =
        isVocals ? juce::String("vocal.group")
                 : (isDrums ? juce::String("drum.group") : juce::String("instrument.") + rootStem);

    command.add("--operation");
    command.add(operation);
    command.add("--input");
    command.add(source.getFullPathName());
    command.add("--output");
    command.add(output.getFullPathName());
    command.add("--parent-id");
    command.add(rootStem);
    command.add("--root-stem");
    command.add(rootStem);
    command.add("--category");
    command.add(category);
    command.add("--depth");
    command.add("1");

    recursiveThread.reset();
    engineProgress.store(0.01);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);
    resetEngineEta();
    engineProgressRate.store(0.0);
    engineCancelRequested.store(false);

    const auto cancelFile = output.getChildFile("stemlab_cancel.txt");

    {
        const juce::ScopedLock lock(stateLock);
        activeCancelFile = cancelFile;
    }

    if (isVocals)
        setStatus("Adaptive vocals: separating lead and backing groups...");
    else if (isDrums)
        setStatus("Adaptive drums: splitting drum components...");
    else
        setStatus("Adaptive lead: detecting foreground and backing layers...");

    recursiveThread = std::make_unique<StemLabRecursiveThread>(
        *this, command, output.getChildFile("recursive_manifest.json"), cancelFile);

    recursiveThread->startThread();
    return true;
}

bool StemLabAudioProcessor::launchRecursiveAction(const juce::String& itemId,
                                                  const juce::String& action)
{
    if (!hasSuccessfulJob() || isEngineRunning())
        return false;

    StemLabRecursiveStemInfo target;
    bool found = false;

    for (const auto& item : getRecursiveStemItems())
    {
        if (item.id == itemId)
        {
            target = item;
            found = true;
            break;
        }
    }

    if (!found || !target.file.existsAsFile())
    {
        setStatus("Adaptive source was not found");
        return false;
    }

    if (!target.actions.contains(action))
    {
        setStatus("That adaptive action is not available for this stem");
        return false;
    }

    const bool isDeverb = action.equalsIgnoreCase("deverb");
    const bool isAdaptiveSplit = action.equalsIgnoreCase("split");

    if (!isDeverb && !isAdaptiveSplit)
    {
        setStatus("Adaptive action is not implemented yet");
        return false;
    }

    auto command = makePythonModuleCommand("stemlab.recursive_job");
    if (command.isEmpty())
    {
        setStatus("Adaptive stem engine could not be located");
        return false;
    }

    auto safeFolder = itemId.replace("/", "_").replace("\\", "_");
    const auto operation = isDeverb ? juce::String("deverb") : juce::String("adaptive");
    const auto output = getLastJobDirectory()
                            .getChildFile("recursive")
                            .getChildFile("actions")
                            .getChildFile(safeFolder + "_" + operation);

    if (output.isDirectory())
        output.deleteRecursively();
    output.createDirectory();

    command.add("--operation");
    command.add(operation);
    command.add("--input");
    command.add(target.file.getFullPathName());
    command.add("--output");
    command.add(output.getFullPathName());
    command.add("--parent-id");
    command.add(target.id);
    command.add("--root-stem");
    command.add(target.rootStem);
    command.add("--category");
    command.add(target.category.isNotEmpty() ? target.category : "unknown");
    command.add("--depth");
    command.add(juce::String(target.depth + 1));

    recursiveThread.reset();
    engineProgress.store(0.01);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);
    resetEngineEta();
    engineProgressRate.store(0.0);
    engineCancelRequested.store(false);

    const auto cancelFile = output.getChildFile("stemlab_cancel.txt");

    {
        const juce::ScopedLock lock(stateLock);
        activeCancelFile = cancelFile;
    }

    setStatus(isDeverb ? "De-reverb: processing isolated lead vocal..."
                       : "Adaptive split: analysing how many useful layers remain...");

    recursiveThread = std::make_unique<StemLabRecursiveThread>(
        *this, command, output.getChildFile("recursive_manifest.json"), cancelFile);

    recursiveThread->startThread();
    return true;
}

bool StemLabAudioProcessor::isRecursiveEngineRunning() const noexcept
{
    return recursiveThread != nullptr && recursiveThread->isThreadRunning();
}

bool StemLabAudioProcessor::isEngineRunning() const noexcept
{
    return (engineThread != nullptr && engineThread->isThreadRunning()) ||
           isRecursiveEngineRunning();
}

double StemLabAudioProcessor::getEngineElapsedSeconds() const noexcept
{
    const auto start = engineStartMs.load();

    if (start <= 0.0)
        return 0.0;

    if (isEngineRunning())
        return juce::jmax(0.0, (nowMs() - start) / 1000.0);

    return lastEngineDurationSeconds.load();
}

double StemLabAudioProcessor::getEngineEstimatedRemainingSeconds() const noexcept
{
    if (!isEngineRunning())
        return 0.0;

    // No finish time can be promised once a stop has been asked for: the
    // engine is winding down, not working towards the number it last
    // reported. -1 rather than 0 so the readout drops the segment entirely
    // instead of pinning "ETA 00:00", which would be the worse lie.
    if (engineCancelRequested.load())
        return -1.0;

    const auto now = nowMs();

    // Prefer the engine's own estimate - a job-level number since the
    // pipeline learned to extend stage estimates across the stages still
    // to come - and count it down locally between reports so the display
    // keeps moving even when the engine is quiet for a while.
    double reportedEta = -1.0;
    double reportedAtMs = 0.0;

    {
        const juce::ScopedLock lock(stateLock);
        reportedEta = engineEtaSeconds;
        reportedAtMs = engineEtaUpdateMs;
    }

    if (reportedEta >= 0.0 && reportedAtMs > 0.0 && now - reportedAtMs < 300000.0)
    {
        const auto countdown = reportedEta - (now - reportedAtMs) / 1000.0;

        // A countdown that ran out means that stage finished and a later
        // stage is running without reports - fall through to the rate model.
        if (countdown > 0.0)
            return juce::jlimit(0.0, 24.0 * 60.0 * 60.0, countdown);
    }

    // Project from the smoothed progress rate. CPU jobs can go minutes
    // between updates, so this also counts down from the last update
    // instead of going blank a few seconds after each one.
    const auto progress = engineProgress.load();
    const auto rate = engineProgressRate.load();
    const auto updated = engineProgressUpdateMs.load();

    if (progress <= 0.02 || progress >= 0.995 || rate <= 1.0e-6 || updated <= 0.0 ||
        now - updated > 300000.0)
    {
        return -1.0;
    }

    const auto estimateAtUpdate = (1.0 - progress) / rate;
    const auto estimate = estimateAtUpdate - (now - updated) / 1000.0;

    // A projection that has counted below zero is stale, not "almost
    // done": clamping it used to pin the display at 00:00 for minutes.
    if (estimate <= 0.0)
        return -1.0;

    return juce::jlimit(0.0, 24.0 * 60.0 * 60.0, estimate);
}

void StemLabAudioProcessor::cancelSeparation()
{
    if (!isEngineRunning())
        return;

    juce::File cancelFile;

    {
        const juce::ScopedLock lock(stateLock);
        cancelFile = activeCancelFile;
    }

    engineCancelRequested.store(true);

    // The engine's watchdog shuts the job down from the inside, taking its
    // model subprocesses with it. A direct kill would only reach the
    // interpreter and orphan the torch worker.
    if (cancelFile != juce::File())
        cancelFile.replaceWithText("cancel\n");

    setStatus("Cancelling...");

    // An engine without the watchdog never sees the sentinel, and its
    // reader thread is parked inside readProcessOutput where no flag can
    // reach it. Enforce the grace period from here instead: this timer
    // callback lands on the message thread, which is free to kill the
    // child and thereby release the reader.
    std::weak_ptr<int> lifetime = lifetimeToken;

    juce::Timer::callAfterDelay(4000,
                                [this, lifetime]
                                {
                                    if (lifetime.expired())
                                        return;

                                    if (!engineCancelRequested.load())
                                        return;

                                    if (engineThread != nullptr && engineThread->isThreadRunning())
                                        engineThread->stopChildProcess(0);

                                    if (recursiveThread != nullptr &&
                                        recursiveThread->isThreadRunning())
                                    {
                                        recursiveThread->stopChildProcess(0);
                                    }
                                });
}

void StemLabAudioProcessor::refreshEngineProgressFromDisk()
{
    // Recursive jobs stream their progress directly through stdout. The main
    // job's progress file may still contain 100% from the six-stem pass, so
    // do not let that stale file overwrite recursive progress/status.
    if (isRecursiveEngineRunning())
        return;

    if (!isEngineRunning())
        return;

    const auto job = getLastJobDirectory();

    if (!job.isDirectory())
        return;

    const auto progressFile = job.getChildFile("stemlab_progress.txt");

    if (!progressFile.existsAsFile())
        return;

    const auto text = progressFile.loadFileAsString().trim();

    // The engine writes "percent\nstage". A '|' separator is also accepted
    // so an older engine paired with this plugin keeps working; looking
    // only for '|' made this whole fallback channel silently inert, because
    // no engine has ever written one.
    auto separator = text.indexOfChar('\n');

    if (separator <= 0)
        separator = text.indexOfChar('|');

    if (separator <= 0)
        return;

    const auto percent = text.substring(0, separator).getDoubleValue();

    const auto stage = text.substring(separator + 1).trim();

    setEngineProgress(juce::jlimit(0.0, 1.0, percent / 100.0));

    /*
        The stage reclaims the status line whenever it shows something
        else. Forwarding it only when the file moved on let anything that
        narrates concurrently - an analysis restarted from a menu, cache
        maintenance - hold the line until the engine's next stage change,
        which mid-Demucs can be minutes away. The two engine statuses that
        must outlive the stage stay safe: "Cancelling..." is behind the
        cancel-flag guard here, and a failure reason is published after
        the process exits, when this poll no longer runs.
    */
    lastPolledFileStage = stage;

    if (stage.isNotEmpty() && !engineCancelRequested.load() && stage != getStatus())
        setStatus(stage);
}

juce::String StemLabAudioProcessor::getStatus() const
{
    const juce::ScopedLock lock(stateLock);
    return status;
}

juce::String StemLabAudioProcessor::getActionStatus() const
{
    const juce::ScopedLock lock(stateLock);
    return actionStatus;
}

int StemLabAudioProcessor::getActionStatusRevision() const
{
    const juce::ScopedLock lock(stateLock);
    return actionStatusRevision;
}

void StemLabAudioProcessor::postUiStatus(const juce::String& message)
{
    setActionStatus(message);
}

juce::String StemLabAudioProcessor::getEngineLog() const
{
    const juce::ScopedLock lock(stateLock);

    // Joined through a stream rather than String::operator+=, which
    // re-measures the accumulated text on every append.
    juce::MemoryOutputStream joined(engineLogBytes + 1);

    for (const auto& chunk : engineLogChunks)
        joined << chunk;

    // Decoded explicitly rather than through toString(), which sniffs the
    // first bytes for a byte-order mark the engine's own output never has.
    return juce::String::fromUTF8(static_cast<const char*>(joined.getData()),
                                  static_cast<int>(joined.getDataSize()));
}

bool StemLabAudioProcessor::hasEngineLog() const
{
    // appendEngineLog never stores an empty chunk, so emptiness here is the
    // same answer getEngineLog().isNotEmpty() gives - without the join.
    const juce::ScopedLock lock(stateLock);
    return !engineLogChunks.empty();
}

juce::File StemLabAudioProcessor::getLastJobDirectory() const
{
    const juce::ScopedLock lock(stateLock);
    return lastJobDirectory;
}

void StemLabAudioProcessor::setStatus(const juce::String& newStatus, StatusSeverity severity)
{
    {
        const juce::ScopedLock lock(stateLock);
        status = newStatus;
        statusSeverity = severity;
    }

    sendChangeMessage();
}

StemLabAudioProcessor::StatusSeverity StemLabAudioProcessor::getStatusSeverity() const
{
    const juce::ScopedLock lock(stateLock);
    return statusSeverity;
}

void StemLabAudioProcessor::setActionStatus(const juce::String& newStatus)
{
    {
        const juce::ScopedLock lock(stateLock);
        actionStatus = newStatus;
        ++actionStatusRevision;
    }

    sendChangeMessage();
}

void StemLabAudioProcessor::setEngineProgress(double progress)
{
    // A cancel is pending: the job is coming down, so nothing it reports on
    // the way out is a claim about finishing. Freezing here rather than in
    // the editor covers all three producers at once - the stdout reader,
    // the adaptive reader, and the message thread's progress-file poll.
    // The two writes that must still land during a cancel (the reader
    // threads' own cancel arms) store engineProgress directly and so are
    // deliberately not routed through here.
    if (engineCancelRequested.load())
        return;

    const auto next = juce::jlimit(0.0, 1.0, progress);

    // Two threads report progress: the engine reader parsing stdout and the
    // message thread polling the progress file. A load/compare/store would
    // let them interleave and publish the older of two values, undoing the
    // monotonic guarantee this clamp exists to provide - and feeding the
    // ETA a rate computed over the wrong interval.
    auto current = engineProgress.load();

    while (next > current && !engineProgress.compare_exchange_weak(current, next))
    {
    }

    if (next > current)
    {
        const auto now = nowMs();
        const auto previousUpdate = engineProgressUpdateMs.exchange(now);

        // Smoothed progress-per-second for the fallback ETA. Only the
        // thread that won the exchange above gets here for this step, so
        // the delta and the interval always belong together.
        //
        // Stage boundaries land as one large instantaneous step (a model
        // finishing hands its whole remaining band over at once). Folding
        // that step into the rate would briefly claim the job runs orders
        // of magnitude faster than it does, so big jumps leave the rate
        // alone - the bar still moves, only the estimator ignores it.
        const auto delta = next - current;
        const auto intervalSeconds = (now - previousUpdate) / 1000.0;

        // The launch sequence bumps the bar a point or two within
        // milliseconds; folding those hops in seeded the estimator with a
        // rate hundreds of times too fast, and the ETA read 00:00 for the
        // rest of the job while the EMA slowly recovered. Real reports are
        // never that close together, so a minimum interval filters the
        // synthetic ones.
        if (previousUpdate > 0.0 && intervalSeconds >= 0.5 && delta < 0.08)
        {
            const auto instantRate = delta / intervalSeconds;
            const auto smoothed = engineProgressRate.load();

            engineProgressRate.store(smoothed <= 0.0 ? instantRate
                                                     : 0.3 * instantRate + 0.7 * smoothed);
        }
    }

    sendChangeMessage();
}

void StemLabAudioProcessor::storeEngineEta(double seconds) noexcept
{
    const juce::ScopedLock lock(stateLock);
    engineEtaSeconds = juce::jmax(0.0, seconds);
    engineEtaUpdateMs = nowMs();
}

void StemLabAudioProcessor::resetEngineEta() noexcept
{
    const juce::ScopedLock lock(stateLock);
    engineEtaSeconds = -1.0;
    engineEtaUpdateMs = 0.0;
}

void StemLabAudioProcessor::handleEngineOutputLine(const juce::String& line)
{
    if (line.startsWithIgnoreCase("STEMLAB_ETA "))
    {
        // One report per model chunk - useful for the display, noise in the
        // diagnostics log, so it is consumed before the log append.
        const auto seconds =
            line.fromFirstOccurrenceOf("STEMLAB_ETA ", false, false).trim().getDoubleValue();

        storeEngineEta(seconds);
        sendChangeMessage();
        return;
    }

    appendEngineLog(line + "\n");

    if (line.startsWithIgnoreCase("STEMLAB_ERROR "))
    {
        const auto message = line.fromFirstOccurrenceOf("STEMLAB_ERROR ", false, false).trim();

        if (message.isNotEmpty())
            setStatus("Failed - " + message, statusFailure);

        return;
    }

    if (line.startsWithIgnoreCase("STEMLAB_PROGRESS "))
    {
        // "STEMLAB_PROGRESS <percent> <stage text...>". The stage is
        // everything after the number - searching for the number's text
        // inside the line would find it again inside a stage like
        // "Refining Drums (2/6)" whenever the digits happened to match.
        const auto payload =
            line.fromFirstOccurrenceOf("STEMLAB_PROGRESS ", false, true).trimStart();

        const auto percent = juce::jlimit(0.0, 100.0, payload.getDoubleValue());

        sawEngineProgressProtocol.store(true);
        setEngineProgress(percent / 100.0);

        const auto stage = payload.fromFirstOccurrenceOf(" ", false, false).trim();

        // The same guard the progress-file poll already applies (see
        // refreshEngineProgressFromDisk): once the user has asked to stop,
        // "Cancelling..." outranks whatever stage the engine is still
        // narrating on its way down. Progress lines now arrive within
        // milliseconds of being printed, so without this the footer flips
        // back to a stage name moments after the click.
        if (stage.isNotEmpty() && !engineCancelRequested.load())
            setStatus(stage);

        return;
    }

    // Before the scraper below, and returning rather than falling through:
    // a stem path with a digit in front of a '%' is an ordinary file name,
    // and letting it reach the scraper would drive the progress bar from it.
    if (line.startsWithIgnoreCase("STEMLAB_STEM_READY "))
    {
        handleStemReadyLine(line.fromFirstOccurrenceOf("STEMLAB_STEM_READY ", false, true));
        return;
    }

    // Legacy fallback: pick a bare "NN%" out of raw model output. It is
    // only for engines too old to emit STEMLAB_PROGRESS at all - once this
    // job has spoken the protocol, raw percentages are noise. A first-run
    // model download prints its own 0-100% and used to drag the bar to 78%
    // before separation had even started.
    if (sawEngineProgressProtocol.load())
        return;

    const auto percentPos = line.indexOfChar('%');

    if (percentPos > 0)
    {
        int start = percentPos - 1;

        while (start >= 0 &&
               juce::CharacterFunctions::isDigit(static_cast<juce::juce_wchar>(line[start])))
        {
            --start;
        }

        const auto number = line.substring(start + 1, percentPos).getIntValue();

        if (number >= 0 && number <= 100)
            setEngineProgress(0.10 + 0.68 * (number / 100.0));
    }
}

void StemLabAudioProcessor::handleStemReadyLine(const juce::String& payload)
{
    // "<stem> <absolute final path>". The stem name is lowercase and free of
    // spaces; the path is the whole remainder, unquoted and unescaped, so it
    // is taken verbatim - stem paths routinely contain spaces, and the line
    // reader has already taken the newline and any carriage return off the
    // end of it.
    const auto trimmed = payload.trimStart();
    const auto separator = trimmed.indexOfChar(' ');

    if (separator <= 0)
        return;

    const auto stem = trimmed.substring(0, separator);
    const auto path = trimmed.substring(separator + 1);

    // The protocol promises an absolute path, and juce::File asserts on
    // anything else rather than returning a null file.
    if (!juce::File::isAbsolutePath(path))
        return;

    int index = -1;

    for (int i = 0; i < stemCount; ++i)
    {
        if (getStemName(i).equalsIgnoreCase(stem))
        {
            index = i;
            break;
        }
    }

    // An engine that learns a seventh stem announces a name this build has
    // no lane for. There is nothing to show it in, so the line is dropped
    // rather than guessed at.
    if (index < 0)
        return;

    // This handler is shared by the adaptive-split and source-analysis
    // readers, and neither of those owns the six root lanes.
    if (!mainEngineRunning.load())
        return;

    // Read before the lock below: getLastJobDirectory takes stateLock, and
    // taking stateLock under stemFileCacheLock would plant a lock ordering
    // that does not exist anywhere else in this class.
    const auto job = getLastJobDirectory();

    if (job == juce::File() || !job.isDirectory())
        return;

    const juce::File announced(path);

    /*
     * The two folders the main job writes its stems into. Anything else -
     * a recursive split's own output, a temp file, a path from a job that
     * has already been replaced - belongs to something the lanes do not
     * draw.
     *
     * Compared through the resolved job directory because the engine
     * announces a resolved path: with a symlink anywhere above the job
     * root - a linked cache or home directory - the two spellings differ
     * and every announcement would be dropped silently.
     */
    const auto resolvedJob = job.getLinkedTarget();

    const auto insideJobOutput = [&announced](const juce::File& root)
    {
        return announced.isAChildOf(root.getChildFile("baseline")) ||
               announced.isAChildOf(root.getChildFile("refined"));
    };

    if (!insideJobOutput(job) && !insideJobOutput(resolvedJob))
        return;

    {
        const juce::ScopedLock lock(stemFileCacheLock);

        // Belt to the explicit resets: one job's slots must never stand
        // next to another's, whatever order the launch and the last lines
        // of the previous reader arrive in.
        if (readyStemJob != job)
        {
            readyStemFile.fill(juce::File());
            readyStemJob = job;
        }

        readyStemFile[static_cast<size_t>(index)] = announced;

        // Bumped inside the lock: a revision published after releasing it
        // could name a snapshot that a reader had already moved past.
        ++readyStemRevision;
    }

    // Analysed while the rest of the job runs, so the lane has a picture the
    // moment it starts drawing this file.
    waveformProfiles.warm(announced);

    // No setStatus here. Ready lines interleave with the engine's
    // STEMLAB_PROGRESS stage reports, and the footer would flip between the
    // two at engine line rate; the 20 Hz refresh shows the new lane anyway.
    sendChangeMessage();
}

void StemLabAudioProcessor::resetReadyStemFiles()
{
    const juce::ScopedLock lock(stemFileCacheLock);

    readyStemFile.fill(juce::File());
    readyStemJob = juce::File();
    ++readyStemRevision;
}

juce::File StemLabAudioProcessor::getReadyStemFile(int index) const
{
    if (!juce::isPositiveAndBelow(index, stemCount))
        return {};

    // Copied under the lock, not merely referenced from under it: juce::File
    // holds a reference-counted String, and an announcement overwriting the
    // slot mid-copy races that refcount.
    const juce::ScopedLock lock(stemFileCacheLock);
    return readyStemFile[static_cast<size_t>(index)];
}

int StemLabAudioProcessor::getReadyStemRevision() const
{
    const juce::ScopedLock lock(stemFileCacheLock);
    return readyStemRevision;
}

void StemLabAudioProcessor::appendEngineLog(const juce::String& text)
{
    if (text.isNotEmpty())
    {
        const juce::ScopedLock lock(stateLock);

        // Appending to one juce::String cost a strlen, an exact-fit realloc
        // and a full copy per line, and the character-count trim then walked
        // every code point of the 50 KB cap on every line after that. Whole
        // chunks leave the front instead, so the cost of a line no longer
        // depends on how much has been logged.
        engineLogBytes += static_cast<size_t>(text.getNumBytesAsUTF8());
        engineLogChunks.push_back(text);

        constexpr size_t maxLogBytes = 50000;

        while (engineLogBytes > maxLogBytes && engineLogChunks.size() > 1)
        {
            engineLogBytes -= static_cast<size_t>(engineLogChunks.front().getNumBytesAsUTF8());
            engineLogChunks.pop_front();
        }
    }

    sendChangeMessage();
}

int StemLabAudioProcessor::saveSelectedStemsTo(const juce::File& destination)
{
    if (getHostIntegration() == hostIntegrationAbletonLive || !hasSuccessfulJob())
        return 0;

    if (!destination.isDirectory())
    {
        if (!destination.createDirectory())
            return 0;
    }

    const auto baseName = getCaptureFile().getFileNameWithoutExtension();

    int saved = 0;

    // Saving goes through the same loop-aware render as dragging: with the
    // loop on, what lands in the folder is the looped section, not the whole
    // stem. The extension is left to the renderer - a plain copy keeps the
    // source's, a looped render is always WAV.
    for (int i = 0; i < stemCount; ++i)
    {
        if (!isStemEnabled(i))
            continue;

        const auto source = getCompletedStemFile(i);

        if (!source.existsAsFile())
            continue;

        const auto target = destination.getChildFile(baseName + "_" + getStemName(i));

        if (exportLoopedRegions(source, target).existsAsFile())
            ++saved;
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (!item.selected || !item.file.existsAsFile())
            continue;

        auto safeName = item.id.replace("/", "_").replace("\\", "_");
        const auto target = destination.getChildFile(baseName + "_" + safeName);

        if (exportLoopedRegions(item.file, target).existsAsFile())
            ++saved;
    }

    setActionStatus("Saved " + juce::String(saved) + (saved == 1 ? " stem" : " stems"));

    return saved;
}

std::vector<stemlab::loops::Region> StemLabAudioProcessor::loopRegionsSnapshot() const
{
    const juce::ScopedLock lock(selectionLock);
    return loopRegionsNormalised;
}

/*
 * A stem leaves as its whole file unless the playback loop is on, in which
 * case the loop's regions are rendered back to back into one WAV - the same
 * merged multi-lane regions the transport plays, so what leaves is what was
 * heard. With no loop the destination is a plain copy, so callers can always
 * hand the returned file away without touching the job's output.
 */
juce::File StemLabAudioProcessor::exportLoopedRegions(const juce::File& source,
                                                      const juce::File& destination)
{
    if (!source.existsAsFile())
        return {};

    const auto regions = loopRegionsSnapshot();

    if (regions.empty())
    {
        auto target = destination;

        if (target.getFileExtension().isEmpty())
            target = target.withFileExtension(source.getFileExtension());

        if (target.existsAsFile())
            target.deleteFile();

        return source.copyFileTo(target) ? target : juce::File{};
    }

    std::unique_ptr<juce::AudioFormatReader> reader(previewFormats.createReaderFor(source));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};

    auto target = destination.withFileExtension("wav");

    if (target.existsAsFile())
        target.deleteFile();

    auto fileStream = std::make_unique<juce::FileOutputStream>(target);

    if (!fileStream->openedOk())
        return {};

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    const auto bits = juce::jlimit(16, 32, static_cast<int>(reader->bitsPerSample));
    const auto options = juce::AudioFormatWriter::Options{}
                             .withSampleRate(reader->sampleRate)
                             .withNumChannels(static_cast<int>(reader->numChannels))
                             .withBitsPerSample(bits > 0 ? bits : 24);
    auto writer = wav.createWriterFor(stream, options);

    if (writer == nullptr)
        return {};

    juce::int64 written = 0;

    for (const auto& region : regions)
    {
        const auto startSample = juce::jlimit<juce::int64>(
            0, reader->lengthInSamples,
            static_cast<juce::int64>(std::floor(region.start * reader->lengthInSamples)));
        const auto endSample = juce::jlimit<juce::int64>(
            startSample, reader->lengthInSamples,
            static_cast<juce::int64>(std::ceil(region.end * reader->lengthInSamples)));

        if (endSample <= startSample)
            continue;

        if (!writer->writeFromAudioReader(*reader, startSample, endSample - startSample))
        {
            writer.reset();
            target.deleteFile();
            return {};
        }

        written += endSample - startSample;
    }

    writer.reset();

    if (written <= 0)
    {
        target.deleteFile();
        return {};
    }

    return target;
}

juce::StringArray StemLabAudioProcessor::getSelectedStemFilesForDrag()
{
    juce::StringArray files;

    if (isEngineRunning() || !hasSuccessfulJob())
        return files;

    for (int i = 0; i < stemCount; ++i)
    {
        if (!isStemEnabled(i))
            continue;

        const auto source = getCompletedStemFile(i);

        if (!source.existsAsFile())
            continue;

        const auto dragFile = getStemDragFile(source, getStemName(i));

        if (!dragFile.existsAsFile())
            return {};

        files.addIfNotAlreadyThere(dragFile.getFullPathName());
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (!item.selected || !item.file.existsAsFile())
            continue;

        const auto dragFile = getStemDragFile(item.file, item.id);

        if (!dragFile.existsAsFile())
            return {};

        files.addIfNotAlreadyThere(dragFile.getFullPathName());
    }

    if (files.isEmpty())
        setActionStatus("Choose at least one stem to drag");

    return files;
}

juce::String StemLabAudioProcessor::getAbletonBridgeStatus() const
{
    const juce::ScopedLock lock(abletonBridgeLock);
    return abletonBridgeStatus;
}

void StemLabAudioProcessor::refreshAbletonBridgeStatusFromDisk()
{
    if (getHostIntegration() != hostIntegrationAbletonLive)
        return;

    if (isStandaloneApp())
        return;

    // The invisible Remote Script writes a small heartbeat/status file when
    // Live loads it. This lets the VST distinguish "integration installed"
    // from "no background script is active" without any visible Live device.
    // The heartbeat can never override a job-specific wait/import status
    // once a separation exists, so it is not worth reading at all then.
    if (!hasSuccessfulJob())
    {
        const auto globalStatusFile =
            stemlab::paths::legacyBridgeDirectory().getChildFile("stemlab_remote_status.json");

        // A heartbeat unchanged since the last poll cannot change the
        // answer either; mtime + size stand in for the content so the poll
        // does not re-read and re-parse the JSON forever. Size 0 also
        // covers a missing file without a separate existence stat.
        const auto heartbeatMtime = globalStatusFile.getLastModificationTime().toMilliseconds();
        const auto heartbeatSize = globalStatusFile.getSize();

        if (heartbeatSize > 0 &&
            (heartbeatMtime != bridgeHeartbeatMtime || heartbeatSize != bridgeHeartbeatSize))
        {
            bridgeHeartbeatMtime = heartbeatMtime;
            bridgeHeartbeatSize = heartbeatSize;

            const auto remoteStatus = juce::JSON::parse(globalStatusFile.loadFileAsString());

            if (auto* statusObject = remoteStatus.getDynamicObject())
            {
                if (statusObject->getProperty("protocol").toString() == "stemlab-remote-status")
                {
                    const bool active = static_cast<bool>(statusObject->getProperty("active"));

                    const double timestamp =
                        static_cast<double>(statusObject->getProperty("timestamp"));

                    const auto nowUnix = juce::Time::getCurrentTime().toMilliseconds() / 1000.0;

                    // The init heartbeat persists on disk. Treat it as a
                    // useful "installed/loaded recently" indication.
                    if (active && nowUnix - timestamp < 24.0 * 60.0 * 60.0)
                    {
                        const juce::ScopedLock lock(abletonBridgeLock);

                        abletonBridgeStatus =
                            "StemLabRemote active - background Ableton integration ready";
                    }
                }
            }
        }
    }

    /*
     * Progress and ack files only appear after Send Selected / Retry armed
     * a wait (both delete any leftovers before arming), so outside one
     * there is nothing to poll for - and a stale ack surviving from an
     * earlier session must not replay as a fresh import result. This is
     * also what keeps the steady state free of per-poll file I/O. The ack
     * alone stays polled past the timeout: Live finishing a long import at
     * 15s must replace "timed out", not leave the user retrying a set that
     * actually landed.
     */
    const auto waitStart = abletonBridgeWaitStartMs.load();

    if (waitStart <= 0.0 && !abletonAckExpected.load())
        return;

    const auto job = getLastJobDirectory();

    if (!job.isDirectory())
        return;

    const auto importProgress = job.getChildFile("stemlab_ableton_import_progress.json");

    // Progress narration only matters while the wait is live; after a
    // timeout only the ack below is still of interest.
    if (waitStart > 0.0 && importProgress.existsAsFile())
    {
        const auto progressParsed = juce::JSON::parse(importProgress.loadFileAsString());

        if (auto* progressObject = progressParsed.getDynamicObject())
        {
            if (progressObject->getProperty("protocol").toString() ==
                "stemlab-ableton-import-progress")
            {
                const auto progressMessage = progressObject->getProperty("message").toString();

                const int imported = static_cast<int>(progressObject->getProperty("imported"));

                const int total = static_cast<int>(progressObject->getProperty("total"));

                if (progressMessage.isNotEmpty())
                {
                    juce::String visible = progressMessage;

                    if (total > 0)
                    {
                        visible += " (" + juce::String(juce::jmin(imported + 1, total)) + "/" +
                                   juce::String(total) + ")";
                    }

                    setStatus(visible);
                }
            }
        }
    }

    const auto ack = job.getChildFile("stemlab_ableton_ack.json");

    if (ack.existsAsFile())
    {
        const auto parsed = juce::JSON::parse(ack.loadFileAsString());

        if (auto* object = parsed.getDynamicObject())
        {
            const auto protocol = object->getProperty("protocol").toString();

            if (protocol == "stemlab-ableton-ack")
            {
                const bool success = static_cast<bool>(object->getProperty("success"));

                const int imported = static_cast<int>(object->getProperty("imported"));

                const auto message = object->getProperty("message").toString();

                {
                    const juce::ScopedLock lock(abletonBridgeLock);

                    abletonBridgeStatus =
                        success ? "Ableton imported " + juce::String(imported) +
                                      (imported == 1 ? " stem" : " stems")
                                : "Ableton import failed" +
                                      (message.isNotEmpty() ? ": " + message : juce::String{});
                }

                setStatus(success ? "Imported into Ableton"
                                  : "Ableton import failed - select source track if needed, then "
                                    "Retry Import");

                abletonBridgeWaitStartMs.store(0.0);
                abletonAckExpected.store(false);
                return;
            }
        }
    }

    // abletonAckExpected deliberately stays true here: the poll keeps
    // watching for a late ack that supersedes this message.
    if (waitStart > 0.0 && nowMs() - waitStart > 12000.0)
    {
        {
            const juce::ScopedLock lock(abletonBridgeLock);

            abletonBridgeStatus = "Ableton import timed out - click Retry";
        }

        setStatus("Ableton import timed out - click Retry");

        abletonBridgeWaitStartMs.store(0.0);
    }
}

bool StemLabAudioProcessor::sendAbletonBridgeNotification(const juce::File& manifestFile)
{
    if (!manifestFile.existsAsFile())
        return false;

    const auto payload = "stemlab_ready " + utf8ToHex(manifestFile.getFullPathName());

    juce::DatagramSocket socket(false);

    const auto bytes = payload.toRawUTF8();

    const auto written =
        socket.write("127.0.0.1", 39277, bytes, static_cast<int>(std::strlen(bytes)));

    return written > 0;
}

bool StemLabAudioProcessor::sendSelectedStemsToAbleton()
{
    if (getHostIntegration() != hostIntegrationAbletonLive)
        return false;

    if (isStandaloneApp() || isEngineRunning() || !hasSuccessfulJob())
    {
        return false;
    }

    const auto job = getLastJobDirectory();

    const auto masterManifest = job.getChildFile("stemlab_ableton_manifest.json");

    if (!masterManifest.existsAsFile())
    {
        setStatus("Completed stem manifest was not found");
        return false;
    }

    auto manifest = juce::JSON::parse(masterManifest.loadFileAsString());

    auto* object = manifest.getDynamicObject();

    if (object == nullptr)
    {
        setStatus("Completed stem manifest is invalid");
        return false;
    }

    auto* allStems = object->getProperty("stems").getArray();

    if (allStems == nullptr)
    {
        setStatus("Completed stem list is invalid");
        return false;
    }

    juce::Array<juce::var> selected;

    for (const auto& entry : *allStems)
    {
        auto* stemObject = entry.getDynamicObject();

        if (stemObject == nullptr)
            continue;

        const auto name = stemObject->getProperty("name").toString();

        for (int i = 0; i < stemCount; ++i)
        {
            if (name.equalsIgnoreCase(getStemName(i)) && isStemEnabled(i))
            {
                selected.add(entry);
                break;
            }
        }
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (!item.selected || !item.file.existsAsFile())
            continue;

        auto* recursiveObject = new juce::DynamicObject();
        recursiveObject->setProperty("name", item.label);
        recursiveObject->setProperty("label", "StemLab - " + item.label);
        recursiveObject->setProperty("path", item.file.getFullPathName().replace("\\", "/"));
        recursiveObject->setProperty("recursive", true);
        recursiveObject->setProperty("root_stem", item.rootStem);
        selected.add(juce::var(recursiveObject));
    }

    if (selected.isEmpty())
    {
        setActionStatus("Choose at least one stem to send");
        return false;
    }

    object->setProperty("stems", juce::var(selected));

    object->setProperty("selection_mode", "post-audition");

    const auto selectedManifest = job.getChildFile("stemlab_ableton_selected_manifest.json");

    if (!selectedManifest.replaceWithText(juce::JSON::toString(manifest, true)))
    {
        setStatus("Could not write selected-stem manifest");
        return false;
    }

    const auto ack = job.getChildFile("stemlab_ableton_ack.json");

    if (ack.existsAsFile())
        ack.deleteFile();

    const auto importProgress = job.getChildFile("stemlab_ableton_import_progress.json");

    if (importProgress.existsAsFile())
        importProgress.deleteFile();

    if (!sendAbletonBridgeNotification(selectedManifest))
    {
        setStatus("Could not contact StemLabRemote");
        return false;
    }

    {
        const juce::ScopedLock lock(abletonBridgeLock);

        abletonBridgeStatus = "Sent " + juce::String(selected.size()) +
                              (selected.size() == 1 ? " stem to Ableton" : " stems to Ableton") +
                              " - waiting for import confirmation";
    }

    abletonBridgeWaitStartMs.store(nowMs());
    abletonAckExpected.store(true);

    setStatus("Sending selected stems to Ableton...");
    return true;
}

bool StemLabAudioProcessor::retryAbletonImport()
{
    if (getHostIntegration() != hostIntegrationAbletonLive)
        return false;

    if (isStandaloneApp() || isEngineRunning())
    {
        return false;
    }

    const auto job = getLastJobDirectory();

    auto manifest = job.getChildFile("stemlab_ableton_selected_manifest.json");

    if (!manifest.existsAsFile())
    {
        manifest = job.getChildFile("stemlab_ableton_manifest.json");
    }

    if (!manifest.existsAsFile())
    {
        setActionStatus("No completed Ableton manifest to import");
        return false;
    }

    const auto ack = job.getChildFile("stemlab_ableton_ack.json");

    if (ack.existsAsFile())
        ack.deleteFile();

    const auto importProgress = job.getChildFile("stemlab_ableton_import_progress.json");

    if (importProgress.existsAsFile())
        importProgress.deleteFile();

    {
        const juce::ScopedLock lock(abletonBridgeLock);

        abletonBridgeStatus = "Retry sent - waiting for StemLabRemote...";
    }

    abletonBridgeWaitStartMs.store(nowMs());
    abletonAckExpected.store(true);

    if (!sendAbletonBridgeNotification(manifest))
    {
        setStatus("Could not send Retry Import message");
        return false;
    }

    setStatus("Retry Import sent to Ableton");
    return true;
}

StemLabAudioProcessor::HostIntegration StemLabAudioProcessor::getHostIntegration() const noexcept
{
    if (isStandaloneApp())
        return hostIntegrationNone;

    if (reaperApi != nullptr && reaperApi->isValid())
        return hostIntegrationReaper;

    // Live offers no in-process API, so its bridge cannot self-identify the
    // way REAPER's does; the wrapper's host type stands in. Any other host
    // gets the generic local-file + host-capture workflow instead of a
    // bridge it does not have.
    return isAbletonHost() ? hostIntegrationAbletonLive : hostIntegrationNone;
}

void StemLabAudioProcessor::setIHostApplication(Steinberg::FUnknown* host)
{
    if (reaperApi != nullptr)
        return;

    reaperApi = stemlab::reaper::Api::tryCreate(host);

    if (reaperApi != nullptr)
    {
        const auto version = reaperApi->getAppVersion();

        appendEngineLog(
            "REAPER host detected" +
            (version.isNotEmpty() ? " (v" + version + ")" : juce::String()) +
            (reaperApi->isValid()
                 ? juce::String(" - API bridge ready\n")
                 : " - missing API functions: " +
                       reaperApi->getMissingFunctionNames().joinIntoString(", ") + "\n"));
    }

    runReaperSelfTestIfRequested();
}

bool StemLabAudioProcessor::isFileFromCurrentJob(const juce::File& file) const
{
    const juce::ScopedLock lock(stateLock);

    return lastJobDirectory != juce::File() && file.isAChildOf(lastJobDirectory);
}

bool StemLabAudioProcessor::requestReaperSourceItem()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (getHostIntegration() != hostIntegrationReaper || capturing.load() || isEngineRunning())
        return false;

    stopStandalonePlayback();

    const auto item = stemlab::reaper::querySelectedItem(*reaperApi);

    if (!item.ok)
    {
        setActionStatus(item.message);
        return false;
    }

    if (!setInputAudioFile(item.file, juce::jmax(0.0, item.startQN), item.label))
        return false;

    {
        const juce::ScopedLock lock(stateLock);

        reaperSourceInfo.valid = true;
        reaperSourceInfo.item = item.item;
        reaperSourceInfo.startSeconds = item.startSeconds;
        reaperSourceInfo.lengthSeconds = item.lengthSeconds;
        reaperSourceInfo.startOffsetSeconds = item.startOffsetSeconds;
        reaperSourceInfo.playRate = item.playRate;
        reaperSourceInfo.preservePitch = item.preservePitch;
        reaperSourceInfo.trackNumber = item.trackNumber;
    }

    setActionStatus("REAPER item ready: " + item.label);
    return true;
}

bool StemLabAudioProcessor::insertSelectedStemsIntoReaper()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (getHostIntegration() != hostIntegrationReaper || isEngineRunning() || !hasSuccessfulJob())
        return false;

    const auto job = getLastJobDirectory();

    const auto manifestFile = job.getChildFile("stemlab_ableton_manifest.json");

    if (!manifestFile.existsAsFile())
    {
        setStatus("Completed stem manifest was not found");
        return false;
    }

    const auto manifest = juce::JSON::parse(manifestFile.loadFileAsString());

    const auto* object = manifest.getDynamicObject();

    const auto* allStems = object != nullptr ? object->getProperty("stems").getArray() : nullptr;

    if (allStems == nullptr)
    {
        setStatus("Completed stem manifest is invalid");
        return false;
    }

    juce::String sourceLabel;

    {
        const juce::ScopedLock lock(stateLock);
        sourceLabel = inputSourceLabel;
    }

    const auto baseName = sourceLabel.isNotEmpty() ? sourceLabel : juce::String("StemLab");

    /*
     * The project mirrors the stem tree: one track per stem the user kept,
     * and a REAPER folder wherever a stem was split further. A group's own
     * audio still goes in - muted - so the user can unmute it and A/B the
     * whole stem against the parts it was split into.
     */
    struct Node
    {
        juce::String name;
        juce::String colourStem;
        juce::File file;
        bool selected = false;
        std::vector<int> children;
    };

    std::vector<Node> nodes;
    std::map<juce::String, int> indexById;

    nodes.reserve(static_cast<size_t>(stemCount));

    for (int i = 0; i < stemCount; ++i)
    {
        const auto stemName = getStemName(i);

        Node node;
        node.name = baseName + " - " + stemName.substring(0, 1).toUpperCase() +
                    stemName.substring(1).toLowerCase();
        node.colourStem = stemName;
        node.selected = isStemEnabled(i);

        for (const auto& entry : *allStems)
        {
            const auto* stemObject = entry.getDynamicObject();

            if (stemObject == nullptr)
                continue;

            if (stemObject->getProperty("name").toString().equalsIgnoreCase(stemName))
            {
                node.file = juce::File(stemObject->getProperty("path").toString());
                break;
            }
        }

        indexById[stemName] = static_cast<int>(nodes.size());
        nodes.push_back(std::move(node));
    }

    // The tree arrives parent-before-child, which is what the two passes
    // below rely on.
    for (const auto& item : getRecursiveStemItems())
    {
        Node node;
        node.name = item.label.trim().isNotEmpty() ? item.label.trim() : juce::String("Stem");
        node.colourStem = item.rootStem;
        node.file = item.file;
        node.selected = item.selected;

        const auto parent = indexById.find(item.parentId);

        if (parent == indexById.end())
            continue; // a node whose parent never made it into the tree

        const auto index = static_cast<int>(nodes.size());
        nodes[static_cast<size_t>(parent->second)].children.push_back(index);
        indexById[item.id] = index;
        nodes.push_back(std::move(node));
    }

    // A node is inserted when the user kept it, or when it has to exist to
    // hold something below it that the user kept.
    std::vector<bool> wanted(nodes.size(), false);

    for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i)
    {
        const auto& node = nodes[static_cast<size_t>(i)];

        bool keep = node.selected && node.file.existsAsFile();

        for (const auto child : node.children)
            keep = keep || wanted[static_cast<size_t>(child)];

        wanted[static_cast<size_t>(i)] = keep;
    }

    juce::Array<stemlab::reaper::StemToInsert> ordered;

    std::function<void(int)> flatten = [&](int index)
    {
        const auto& node = nodes[static_cast<size_t>(index)];

        std::vector<int> keptChildren;

        for (const auto child : node.children)
            if (wanted[static_cast<size_t>(child)])
                keptChildren.push_back(child);

        stemlab::reaper::StemToInsert entry;
        entry.name = node.name;
        entry.colourStem = node.colourStem;
        entry.file = node.file.existsAsFile() ? node.file : juce::File();
        entry.muted = !keptChildren.empty();
        entry.folderDepth = keptChildren.empty() ? 0 : 1;

        ordered.add(entry);

        for (const auto child : keptChildren)
            flatten(child);

        // The last track inside a folder is the one that closes it.
        if (!keptChildren.empty())
            --ordered.getReference(ordered.size() - 1).folderDepth;
    };

    for (int i = 0; i < stemCount; ++i)
        if (wanted[static_cast<size_t>(i)])
            flatten(i);

    if (ordered.isEmpty())
    {
        setActionStatus("Choose at least one stem to insert");
        return false;
    }

    stemlab::reaper::InsertAnchor anchor;
    bool hasReaperGeometry = false;

    {
        const juce::ScopedLock lock(stateLock);

        hasReaperGeometry = reaperSourceInfo.valid;

        if (hasReaperGeometry)
        {
            anchor.startSeconds = reaperSourceInfo.startSeconds;
            anchor.lengthSeconds = reaperSourceInfo.lengthSeconds;
            anchor.startOffsetSeconds = reaperSourceInfo.startOffsetSeconds;
            anchor.playRate = reaperSourceInfo.playRate;
            anchor.preservePitch = reaperSourceInfo.preservePitch;
            anchor.afterTrackNumber = reaperSourceInfo.trackNumber;
            anchor.sourceItem = reaperSourceInfo.item;
        }
    }

    if (!hasReaperGeometry)
    {
        // A dropped file has no REAPER geometry; place the stems where the
        // captured start beat lands on the current tempo map.
        anchor.startSeconds =
            reaperApi->TimeMap2_QNToTime(nullptr, juce::jmax(0.0, captureStartPpq.load()));
    }

    const auto result = stemlab::reaper::insertStemTracks(*reaperApi, ordered, anchor);

    if (result.inserted > 0)
    {
        /*
         * REAPER draws an item from its .reapeaks file, and only builds one
         * for media it imported itself - so stems placed through the API
         * arrive as empty lanes. Build them now, sliced over the message
         * thread, and redraw the arrangement when the last one lands.
         */
        reaperPeakBuilder =
            std::make_unique<stemlab::reaper::PeakBuilder>(*reaperApi, result.insertedFiles);
    }

    setStatus(result.message);
    return result.inserted > 0;
}

void StemLabAudioProcessor::runReaperSelfTestIfRequested()
{
    // Test-only instrumentation: when the environment names a report file,
    // write what the REAPER handshake produced, and optionally exercise the
    // real pull/insert paths against the live project. Never triggered in
    // normal use - both variables have to be set explicitly by a harness.
    const auto reportPath =
        juce::SystemStats::getEnvironmentVariable("STEMLAB_REAPER_SELFTEST", {});

    if (reportPath.isEmpty())
        return;

    const juce::File report(reportPath);

    juce::String text;
    text << "protocol: stemlab-reaper-selftest\n";

    if (reaperApi == nullptr)
    {
        text << "reaper: not-detected\n";
        report.replaceWithText(text);
        return;
    }

    text << "reaper: detected\n";
    text << "version: " << reaperApi->getAppVersion() << "\n";
    text << "valid: " << (reaperApi->isValid() ? "yes" : "no") << "\n";

    const auto missing = reaperApi->getMissingFunctionNames();

    if (!missing.isEmpty())
        text << "missing: " << missing.joinIntoString(",") << "\n";

    report.replaceWithText(text);

    const auto action =
        juce::SystemStats::getEnvironmentVariable("STEMLAB_REAPER_SELFTEST_ACTION", {});

    if (action.isEmpty() || !reaperApi->isValid())
        return;

    // The project may still be loading while plugins initialise; give REAPER
    // a moment, then run on the message thread like the real UI.
    juce::WeakReference<StemLabAudioProcessor> weak(this);

    juce::Timer::callAfterDelay(2500,
                                [weak, action, report]
                                {
                                    if (weak != nullptr)
                                        weak->runReaperSelfTestAction(action, report);
                                });
}

void StemLabAudioProcessor::runReaperSelfTestAction(const juce::String& action,
                                                    const juce::File& report)
{
    juce::String text = report.loadFileAsString();

    if (action.contains("pull"))
    {
        const auto item = stemlab::reaper::querySelectedItem(*reaperApi);

        text << "pull: " << (item.ok ? "ok" : "failed") << "\n";

        if (item.ok)
        {
            text << "pull-file: " << item.file.getFullPathName() << "\n";
            text << "pull-start: " << juce::String(item.startSeconds, 6) << "\n";
            text << "pull-length: " << juce::String(item.lengthSeconds, 6) << "\n";
            text << "pull-label: " << item.label << "\n";
        }
        else
        {
            text << "pull-message: " << item.message << "\n";
        }

        if (item.ok && action.contains("insert"))
        {
            /*
             * Insert the selected item's own audio three times, standing in
             * for stems: a group holding one child, plus a plain stem. That
             * exercises folder depth, the muted group item, and the flat
             * path in one pass.
             */
            juce::Array<stemlab::reaper::StemToInsert> stems;
            stems.add({"Selftest - Vocals", "vocals", item.file, true, 1});
            stems.add({"Lead", "vocals", item.file, false, -1});
            stems.add({"Selftest - Drums", "drums", item.file, false, 0});

            stemlab::reaper::InsertAnchor anchor;
            anchor.startSeconds = item.startSeconds;
            anchor.lengthSeconds = item.lengthSeconds;
            anchor.startOffsetSeconds = item.startOffsetSeconds;
            anchor.playRate = item.playRate;
            anchor.preservePitch = item.preservePitch;
            anchor.afterTrackNumber = item.trackNumber;
            anchor.sourceItem = item.item;

            const auto result =
                stemlab::reaper::insertStemTracks(*reaperApi, stems, anchor);

            text << "insert: " << result.inserted << "\n";
            text << "insert-muted-source: " << (result.mutedSourceItem ? "yes" : "no") << "\n";
            text << "insert-message: " << result.message << "\n";
        }
    }

    text << "selftest: done\n";
    report.replaceWithText(text);
}

juce::String StemLabAudioProcessor::discoverEngineCommand() const
{
    const auto env = juce::SystemStats::getEnvironmentVariable("STEMLAB_ENGINE", {});

    if (env.isNotEmpty())
    {
        const juce::File envFile(env);

        if (envFile.existsAsFile())
            return envFile.getFullPathName();
    }

    auto checkRoot = [](juce::File root) -> juce::String
    {
        const juce::StringArray relativeCandidates{
#if JUCE_WINDOWS
            // Portable release: keep the whole runtime beside StemLab.exe or
            // beside a VST3 folder that the host scans directly.
            "Engine/python.exe", "engine/python.exe",

            // Development fallbacks.
            ".venv/Scripts/stemlab-plugin-job.exe", ".venv/Scripts/stemlab-plugin-job",
            "venv/Scripts/stemlab-plugin-job.exe", "venv/Scripts/stemlab-plugin-job"
#else
            // Same portable layout, POSIX interpreter location. The "engine"
            // spelling is kept because ext4 will not forgive the difference.
            "Engine/bin/python3", "engine/bin/python3", "Engine/bin/python", "engine/bin/python",

            // Development fallbacks. Only the console script proves stemlab
            // is actually installed in a venv; a bare venv python would let
            // an unrelated ~/.venv shadow the discovery pointer.
            ".venv/bin/stemlab-plugin-job", "venv/bin/stemlab-plugin-job"
#endif
        };

        for (int depth = 0; depth < 10 && root.exists(); ++depth)
        {
            for (const auto& relative : relativeCandidates)
            {
                const auto candidate = root.getChildFile(relative);

                if (candidate.existsAsFile())
                    return candidate.getFullPathName();
            }

            const auto parent = root.getParentDirectory();

            if (parent == root)
                break;

            root = parent;
        }

        return {};
    };

    // Prefer a sibling Engine directory. This makes an extracted portable
    // build self-contained even when a development venv or an older installed
    // engine also exists on the same machine.
    if (const auto found = checkRoot(
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());
        found.isNotEmpty())
    {
        return found;
    }

    // The Standalone portable app and scripts/linux/install_backend.sh write this
    // pointer. The VST3 can then reuse the Engine from the extracted release
    // or the installed backend instead of requiring a second copy.
    {
        const auto stemLabLocal = stemlab::paths::configDirectory();

        const auto portablePointer = stemLabLocal.getChildFile("portable_engine_path.txt");

        if (portablePointer.existsAsFile())
        {
            const auto portablePath = portablePointer.loadFileAsString().trim();
            const juce::File portableRuntime(portablePath);

            if (portableRuntime.existsAsFile())
                return portableRuntime.getFullPathName();
        }

        // Backward-compatible fallback for older installer builds that copied
        // the runtime under the config directory itself.
        const auto installedRuntime = stemLabLocal.getChildFile("Engine")
#if JUCE_WINDOWS
                                          .getChildFile("python.exe");
#else
                                          .getChildFile("bin")
                                          .getChildFile("python3");
#endif

        if (installedRuntime.existsAsFile())
            return installedRuntime.getFullPathName();
    }

#ifdef STEMLAB_DEV_REPO_ROOT
    {
        const auto devRoot = juce::File(STEMLAB_DEV_REPO_ROOT);

        if (const auto found = checkRoot(devRoot); found.isNotEmpty())
        {
            return found;
        }
    }
#endif

    if (const auto found = checkRoot(juce::File::getCurrentWorkingDirectory()); found.isNotEmpty())
    {
        return found;
    }

#if !JUCE_WINDOWS
    // A pip/pipx install is the normal way to get the backend on Linux, and
    // it leaves the launcher on PATH rather than in a sibling directory.
    // Resolve it to an absolute path here so the host's environment - which
    // may not inherit the user's shell PATH at all - cannot lose it later.
    {
        juce::StringArray searchPath;
        searchPath.addTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), ":", {});

        for (const auto& directory : searchPath)
        {
            if (!juce::File::isAbsolutePath(directory))
                continue;

            const auto candidate = juce::File(directory).getChildFile("stemlab-plugin-job");

            if (candidate.existsAsFile())
                return candidate.getFullPathName();
        }
    }
#endif

    return "stemlab-plugin-job";
}

void StemLabAudioProcessor::setEngineCommand(const juce::String& command)
{
    const juce::ScopedLock lock(stateLock);
    engineCommand = command;
}

juce::String StemLabAudioProcessor::getEngineCommand() const
{
    const juce::ScopedLock lock(stateLock);
    return engineCommand;
}

void StemLabAudioProcessor::resetEngineCommandToAutoDiscover()
{
    setEngineCommand(discoverEngineCommand());
    setActionStatus("Engine path auto-detected");
}

void StemLabAudioProcessor::setStemEnabled(int index, bool enabled)
{
    if (juce::isPositiveAndBelow(index, stemCount))
        stemEnabled[static_cast<size_t>(index)].store(enabled);
}

bool StemLabAudioProcessor::isStemEnabled(int index) const
{
    if (!juce::isPositiveAndBelow(index, stemCount))
        return false;

    return stemEnabled[static_cast<size_t>(index)].load();
}

void StemLabAudioProcessor::setWaveformColourIndex(int index)
{
    waveformColourIndex.store(juce::jlimit(0, waveformColourCount - 1, index));

    sendChangeMessage();
}

void StemLabAudioProcessor::setWaveformZoom(double zoom)
{
    const auto clamped = juce::jlimit(minWaveformZoom, maxWaveformZoom, zoom);

    if (std::abs(clamped - waveformZoom.load()) < 1.0e-6)
        return;

    waveformZoom.store(clamped);

    sendChangeMessage();
}

juce::Range<double> StemLabAudioProcessor::getWaveformViewRange(double totalLengthSeconds) const
{
    if (!(totalLengthSeconds > 0.0))
        return {0.0, 0.0};

    const auto zoom = juce::jlimit(minWaveformZoom, maxWaveformZoom, waveformZoom.load());

    const auto transportLength = getTransportLengthSeconds();

    const auto normalised =
        transportLength > 0.0
            ? juce::jlimit(0.0, 1.0, getTransportPositionSeconds() / transportLength)
            : 0.0;

    // The window itself is plain arithmetic, so it lives in WaveformGrid.h
    // where the test target can reach it without standing up a processor.
    const auto window = stemlab::waveform::visibleWindow(totalLengthSeconds, zoom, normalised);

    return {window.start, window.end};
}

void StemLabAudioProcessor::setEditorScalePercent(int percent)
{
    // Deliberately no change broadcast: this is written from the editor's
    // own resized(), and telling it to refresh from there would be a loop.
    editorScalePercent.store(juce::jlimit(25, 400, percent));
}

juce::String StemLabAudioProcessor::getSeparatorEngineId() const
{
    switch (getSeparatorEngineIndex())
    {
    case separatorDemucs:
        return "demucs";

    case separatorHybrid:
        return "hybrid";

    case separatorRoFormer:
    default:
        return "roformer";
    }
}

juce::String StemLabAudioProcessor::getSeparatorEngineDisplayName() const
{
    return getSeparatorEngineShortName(getSeparatorEngineIndex());
}

juce::String StemLabAudioProcessor::getSeparatorEngineShortName(int index)
{
    switch (index)
    {
    case separatorDemucs:
        return "Demucs";

    case separatorHybrid:
        return "Hybrid";

    case separatorRoFormer:
    default:
        return "RoFormer";
    }
}

juce::String StemLabAudioProcessor::getSeparatorEngineMenuName(int index)
{
    switch (index)
    {
    case separatorDemucs:
        return "Demucs (htdemucs_6s)";

    case separatorHybrid:
        return "Hybrid (RoFormer + Demucs)";

    case separatorRoFormer:
    default:
        return "BS-RoFormer";
    }
}

juce::String StemLabAudioProcessor::getStemName(int index)
{
    static constexpr const char* names[stemCount] = {"vocals", "drums", "bass",
                                                     "guitar", "piano", "other"};

    if (!juce::isPositiveAndBelow(index, stemCount))
        return {};

    return names[index];
}

void StemLabAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto rootObject = std::make_unique<juce::DynamicObject>();
    rootObject->setProperty("engineCommand", getEngineCommand());
    rootObject->setProperty("refinement", refinementEnabled.load());
    rootObject->setProperty("separatorEngine", separatorEngineIndex.load());
    rootObject->setProperty("waveformColour", waveformColourIndex.load());
    rootObject->setProperty("waveformZoom", waveformZoom.load());
    rootObject->setProperty("editorScale", editorScalePercent.load());

    rootObject->setProperty("jobRootDirectory", getJobRootDirectory().getFullPathName());

    juce::Array<juce::var> stems;

    for (int i = 0; i < stemCount; ++i)
        stems.add(isStemEnabled(i));

    rootObject->setProperty("stems", stems);

    const auto json = juce::JSON::toString(juce::var(rootObject.release()), false);

    destData.replaceAll(json.toRawUTF8(), static_cast<size_t>(json.getNumBytesAsUTF8()));
}

void StemLabAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const juce::String json(juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes));

    const auto parsed = juce::JSON::parse(json);
    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
        return;

    if (object->hasProperty("engineCommand"))
    {
        const auto savedEngine = object->getProperty("engineCommand").toString().trim();

        const auto discoveredEngine = discoverEngineCommand();

        const bool savedIsGeneric = savedEngine.isEmpty() || savedEngine == "stemlab-plugin-job";

        const juce::File savedFile(savedEngine);

        const bool savedLooksLikePath =
            savedEngine.containsChar('\\') || savedEngine.containsChar('/');

        const bool savedPathIsStale = savedLooksLikePath && !savedFile.existsAsFile();

        const juce::File discoveredFile(discoveredEngine);
        const bool discoveredIsPortableRuntime =
            discoveredFile.getFileName().equalsIgnoreCase("python.exe") &&
            discoveredFile.getParentDirectory().getFileName().equalsIgnoreCase("Engine");

        // A self-contained release must not silently fall back to a saved
        // development venv merely because that venv still exists on the build
        // machine. Prefer the discovered sibling/installed Engine runtime.
        if ((discoveredIsPortableRuntime || savedIsGeneric || savedPathIsStale) &&
            discoveredEngine.isNotEmpty() && discoveredEngine != "stemlab-plugin-job")
        {
            setEngineCommand(discoveredEngine);
        }
        else
        {
            setEngineCommand(savedEngine);
        }
    }
    else
    {
        resetEngineCommandToAutoDiscover();
    }

    if (object->hasProperty("refinement"))
    {
        refinementEnabled.store(static_cast<bool>(object->getProperty("refinement")));
    }

    if (object->hasProperty("separatorEngine"))
    {
        setSeparatorEngineIndex(static_cast<int>(object->getProperty("separatorEngine")));
    }

    if (object->hasProperty("waveformColour"))
    {
        setWaveformColourIndex(static_cast<int>(object->getProperty("waveformColour")));

    if (object->hasProperty("waveformZoom"))
        setWaveformZoom(static_cast<double>(object->getProperty("waveformZoom")));
    }

    if (object->hasProperty("editorScale"))
    {
        setEditorScalePercent(static_cast<int>(object->getProperty("editorScale")));
    }

    if (object->hasProperty("jobRootDirectory"))
    {
        const juce::File savedJobRoot(object->getProperty("jobRootDirectory").toString());

        if (savedJobRoot.isDirectory())
            setJobRootDirectory(savedJobRoot);
    }

    const auto stems = object->getProperty("stems");

    if (auto* array = stems.getArray())
    {
        for (int i = 0; i < juce::jmin(stemCount, array->size()); ++i)
        {
            setStemEnabled(i, static_cast<bool>(array->getUnchecked(i)));
        }
    }
}

juce::AudioProcessorEditor* StemLabAudioProcessor::createEditor()
{
    return new StemLabAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StemLabAudioProcessor(); }

bool StemLabAudioProcessor::startSourceAnalysis(const juce::File& source,
                                                const juce::StringArray& correctionArguments,
                                                const juce::String& correctionLabel)
{
    analysisThread.reset();
    sourceAnalysisRunning.store(true);
    engineCancelRequested.store(false);
    engineProgress.store(0.0);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);
    resetEngineEta();
    engineProgressRate.store(0.0);
    sourceBpm.store(-1.0);
    sourceDetectedBpm.store(-1.0);
    sourceHalfBpm.store(-1.0);
    sourceDoubleBpm.store(-1.0);
    sourceAnalysisCorrected.store(false);
    {
        const juce::ScopedLock lock(stateLock);
        sourceKey.clear();
        sourceHash.clear();
        sourceAnalysisDevice.clear();
        sourceBeatModel.clear();
        sourceKeyCandidates.clear();
        sourceBeats.clear();
        sourceDownbeats.clear();
    }

    auto command = makePythonModuleCommand("stemlab.source_analysis");
    if (command.isEmpty())
    {
        sourceAnalysisRunning.store(false);
        sendChangeMessage();
        return false;
    }

    const auto output = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("stemlab_source_analysis", ".json", false);
    const auto cancelFile = output.withFileExtension("cancel");

    // A cancelled analysis leaves its sentinel file behind unless it is explicitly
    // removed. Because the JSON result is normally deleted after every run, JUCE can
    // choose the same result basename on the next analysis. A stale .cancel file then
    // makes the Python worker abort immediately after hashing the source. Clear that
    // per-run sentinel before the worker can observe it.
    if (cancelFile.existsAsFile())
        cancelFile.deleteFile();

    command.add("--input");
    command.add(source.getFullPathName());
    command.add("--output");
    command.add(output.getFullPathName());
    command.add("--mode");
    command.add(sourceAnalysisMode.load() == analysisFast ? "fast" : "accurate");
    command.add("--cancel-file");
    command.add(cancelFile.getFullPathName());

    if (hasSuccessfulJob())
    {
        // hasCompletedStemFile answers from the same scan cache the file
        // itself comes out of, so the four existsAsFile() stats this used to
        // pay told it nothing the scan had not already established.
        for (int index : {3, 4, 5})
        {
            if (!hasCompletedStemFile(index))
                continue;

            command.add("--harmony-stem");
            command.add(getCompletedStemFile(index).getFullPathName());
        }

        if (hasCompletedStemFile(2))
        {
            command.add("--bass-stem");
            command.add(getCompletedStemFile(2).getFullPathName());
        }
    }

    // A correction rides along with the analysis rather than in a process of
    // its own: the CLI writes it to the local store before it reads the
    // cancel token, so it survives a cancelled analysis half, and the
    // sourceAnalysis kind is the one that streams stdout here and honors the
    // cancel file at all.
    command.addArray(correctionArguments);

    analysisThread = std::make_unique<StemLabUtilityThread>(
        *this, StemLabUtilityThread::sourceAnalysis, command, source, output, juce::String{},
        juce::String{}, cancelFile);

    if (!analysisThread->startThread())
    {
        analysisThread.reset();
        sourceAnalysisRunning.store(false);
        sendChangeMessage();
        return false;
    }

    // The correction is applied first and silently; naming it here is the
    // only point at which the user sees it happen, and the engine's own
    // stage lines take the footer over as soon as the analysis half starts.
    setStatus(correctionLabel.isEmpty() ? "Analyzing source with Beat This!..."
                                        : correctionLabel + ", then re-analyzing...");
    sendChangeMessage();
    return true;
}

void StemLabAudioProcessor::finishSourceAnalysis(const juce::File& source, const juce::File& result,
                                                 int exitCode)
{
    if (source != getCaptureFile())
    {
        if (result.existsAsFile())
            result.deleteFile();
        return;
    }

    const auto elapsed = juce::jmax(0.0, (nowMs() - engineStartMs.load()) / 1000.0);
    lastEngineDurationSeconds.store(elapsed);

    if (engineCancelRequested.load())
    {
        if (result.existsAsFile())
            result.deleteFile();
        sourceAnalysisRunning.store(false);
        engineCancelRequested.store(false);
        engineProgress.store(0.0);
        setStatus("Source analysis cancelled - source ready");
        return;
    }

    juce::String key;
    double bpm = -1.0;
    double detectedBpm = -1.0;
    double halfBpm = -1.0;
    double doubleBpm = -1.0;
    double barOne = 0.0;
    int numerator = 4;
    int denominator = 4;
    bool corrected = false;
    juce::String hash;
    juce::String analysisDevice;
    juce::String beatModel;
    std::vector<StemLabKeyCandidate> keyCandidates;
    std::vector<double> beats;
    std::vector<double> downbeats;

    if (exitCode == 0 && result.existsAsFile())
    {
        const auto parsed = juce::JSON::parse(result.loadFileAsString());
        if (auto* object = parsed.getDynamicObject())
        {
            const auto keyValue = object->getProperty("key");
            if (!keyValue.isVoid() && !keyValue.isUndefined())
                key = keyValue.toString();
            const auto bpmValue = object->getProperty("bpm");
            if (!bpmValue.isVoid() && !bpmValue.isUndefined())
                bpm = static_cast<double>(bpmValue);

            detectedBpm = static_cast<double>(object->getProperty("detected_bpm"));
            halfBpm = static_cast<double>(object->getProperty("half_time_bpm"));
            doubleBpm = static_cast<double>(object->getProperty("double_time_bpm"));
            barOne = static_cast<double>(object->getProperty("bar_one"));
            numerator = juce::jmax(1, static_cast<int>(object->getProperty("meter_numerator")));
            denominator = juce::jmax(1, static_cast<int>(object->getProperty("meter_denominator")));
            corrected = static_cast<bool>(object->getProperty("corrected"));
            hash = object->getProperty("source_hash").toString();
            analysisDevice = object->getProperty("device").toString();
            beatModel = object->getProperty("beat_model").toString();

            if (auto* array = object->getProperty("beats").getArray())
                for (const auto& item : *array)
                    beats.push_back(static_cast<double>(item));
            if (auto* array = object->getProperty("downbeats").getArray())
                for (const auto& item : *array)
                    downbeats.push_back(static_cast<double>(item));
            if (auto* array = object->getProperty("key_candidates").getArray())
            {
                for (const auto& item : *array)
                {
                    if (auto* candidate = item.getDynamicObject())
                    {
                        keyCandidates.push_back(
                            {candidate->getProperty("key").toString(),
                             static_cast<double>(candidate->getProperty("probability"))});
                    }
                }
            }
        }
    }

    if (result.existsAsFile())
        result.deleteFile();

    {
        const juce::ScopedLock lock(stateLock);
        sourceKey = key;
        sourceHash = hash;
        sourceAnalysisDevice = analysisDevice;
        sourceBeatModel = beatModel;
        sourceKeyCandidates = std::move(keyCandidates);
        sourceBeats = std::move(beats);
        sourceDownbeats = std::move(downbeats);
    }
    sourceDetectedBpm.store(detectedBpm > 0.0 ? detectedBpm : -1.0);
    sourceHalfBpm.store(halfBpm > 0.0 ? halfBpm : -1.0);
    sourceDoubleBpm.store(doubleBpm > 0.0 ? doubleBpm : -1.0);
    sourceBarOne.store(barOne);
    sourceMeterNumerator.store(numerator);
    sourceMeterDenominator.store(denominator);
    sourceAnalysisCorrected.store(corrected);

    if (corrected)
        sourceBpm.store(bpm > 0.0 ? bpm : -1.0);
    else
        setTempoInterpretation(tempoInterpretation.load());

    sourceAnalysisRunning.store(false);
    engineCancelRequested.store(false);
    engineProgress.store(exitCode == 0 ? 1.0 : 0.0);
    if (exitCode == 0 && !hasSuccessfulJob())
        setStatus("Source ready");
    else if (exitCode == 0)
        setStatus("Source analysis updated - stems ready");
    else if (exitCode != 0)
        setStatus("Source analysis unavailable - separation is still available");
    sendChangeMessage();
}

juce::String StemLabAudioProcessor::getSourceAnalysisText() const
{
    if (sourceAnalysisRunning.load())
        return "Analyzing key/BPM...";

    if (!beatThisEnabled.load())
        return "Beat This!: Off";

    juce::String key;
    {
        const juce::ScopedLock lock(stateLock);
        key = sourceKey;
    }

    const auto bpm = sourceBpm.load();
    if (key.isNotEmpty() && bpm > 0.0)
        return key + " - " + juce::String(juce::roundToInt(bpm)) + " BPM";
    if (key.isNotEmpty())
        return key + " - BPM: Unknown";
    if (bpm > 0.0)
        return "Key: Unknown - " + juce::String(juce::roundToInt(bpm)) + " BPM";
    return "Key: Unknown - BPM: Unknown";
}

juce::String StemLabAudioProcessor::getSourceAnalysisDetails() const
{
    juce::String text = getSourceAnalysisText();
    const juce::ScopedLock lock(stateLock);
    if (!beatThisEnabled.load())
        text += "\nBeat This! is disabled for automatic source analysis.";
    else if (sourceAnalysisDevice.isNotEmpty())
        text += "\nBeat This!: " + (sourceBeatModel.isNotEmpty() ? sourceBeatModel : "model") +
                " on " + sourceAnalysisDevice.toUpperCase();
    if (!sourceKeyCandidates.empty())
    {
        text += "\n\nTop key candidates:";
        for (size_t index = 0; index < std::min<size_t>(3, sourceKeyCandidates.size()); ++index)
        {
            const auto& candidate = sourceKeyCandidates[index];
            text += "\n" + juce::String(static_cast<int>(index + 1)) + ". " + candidate.key +
                    " - " + juce::String(juce::roundToInt(candidate.probability * 100.0)) + "%";
        }
    }
    text += "\nMeter: " + juce::String(sourceMeterNumerator.load()) + "/" +
            juce::String(sourceMeterDenominator.load());
    text += "\nBar one: " + juce::String(sourceBarOne.load(), 3) + " seconds";
    text += "\nTempo choices: " + juce::String(sourceHalfBpm.load(), 1) + " / " +
            juce::String(sourceDetectedBpm.load(), 1) + " / " +
            juce::String(sourceDoubleBpm.load(), 1) + " BPM";
    return text;
}

juce::String StemLabAudioProcessor::getSourceKey() const
{
    const juce::ScopedLock lock(stateLock);
    return sourceKey;
}

void StemLabAudioProcessor::setBeatThisEnabled(bool enabled)
{
    beatThisEnabled.store(enabled);

    if (!enabled)
    {
        if (analysisThread != nullptr && analysisThread->isThreadRunning())
        {
            engineCancelRequested.store(true);
            analysisThread->requestCancel();
            setStatus("Stopping Beat This! analysis...");

            // The sentinel is enough for a job that reaches a checkpoint.
            // Enforce the grace from here for one that does not: the
            // thread itself is parked in readProcessOutput and cannot.
            std::weak_ptr<int> lifetime = lifetimeToken;

            juce::Timer::callAfterDelay(2000,
                                        [this, lifetime]
                                        {
                                            if (lifetime.expired())
                                                return;

                                            if (analysisThread != nullptr &&
                                                analysisThread->isThreadRunning())
                                            {
                                                analysisThread->stopChildProcess(0);
                                            }
                                        });
        }
        sendChangeMessage();
        return;
    }

    const auto source = getCaptureFile();
    if (source.existsAsFile() && !sourceAnalysisRunning.load() &&
        !isRecursiveEngineRunning() &&
        !(engineThread != nullptr && engineThread->isThreadRunning()))
    {
        startSourceAnalysis(source);
    }
    sendChangeMessage();
}

void StemLabAudioProcessor::setSourceAnalysisMode(int mode)
{
    sourceAnalysisMode.store(juce::jlimit(static_cast<int>(analysisAccurate),
                                          static_cast<int>(analysisFast), mode));
    const auto source = getCaptureFile();
    if (beatThisEnabled.load() && source.existsAsFile() && !sourceAnalysisRunning.load())
        startSourceAnalysis(source);
}

void StemLabAudioProcessor::setTempoInterpretation(int interpretation)
{
    interpretation = juce::jlimit(static_cast<int>(tempoHalf), static_cast<int>(tempoDouble),
                                   interpretation);
    tempoInterpretation.store(interpretation);
    if (sourceAnalysisCorrected.load())
        return;
    const double choices[] = {sourceHalfBpm.load(), sourceDetectedBpm.load(),
                              sourceDoubleBpm.load()};
    sourceBpm.store(choices[interpretation] > 0.0 ? choices[interpretation] : -1.0);
    sendChangeMessage();
}

bool StemLabAudioProcessor::saveSourceCorrection(double bpm, const juce::String& key,
                                                 int numerator, int denominator, double barOne)
{
    return applySourceCorrection(
        {"--set-correction", "--correct-bpm", juce::String(bpm, 4), "--correct-key", key,
         "--correct-meter-numerator", juce::String(numerator), "--correct-meter-denominator",
         juce::String(denominator), "--correct-bar-one", juce::String(barOne, 6)},
        "Saving local analysis correction");
}

bool StemLabAudioProcessor::forgetSourceCorrection()
{
    return applySourceCorrection({"--forget-correction"}, "Forgetting local analysis correction");
}

bool StemLabAudioProcessor::applySourceCorrection(const juce::StringArray& correctionArguments,
                                                  const juce::String& label)
{
    const auto source = getCaptureFile();

    if (!source.existsAsFile() || sourceAnalysisRunning.load())
        return false;

    // One process for both halves. The two-process form re-entered
    // startSourceAnalysis from the maintenance thread's own run(), where its
    // first statement - analysisThread.reset() - destroyed and joined the
    // thread it was running on.
    if (beatThisEnabled.load())
        return startSourceAnalysis(source, correctionArguments, label);

    // Analysis switched off: there is nothing for the correction to be
    // followed by, and the CLI's correction-only form is the one that
    // returns as soon as the store is written.
    juce::StringArray arguments{"--input", source.getFullPathName()};
    arguments.addArray(correctionArguments);

    return launchAnalysisMaintenance(arguments, label);
}

bool StemLabAudioProcessor::clearAnalysisCache()
{
    return launchAnalysisMaintenance({"--clear-cache"}, "Clearing local analysis cache");
}

bool StemLabAudioProcessor::launchAnalysisMaintenance(const juce::StringArray& arguments,
                                                      const juce::String& label)
{
    if (sourceAnalysisRunning.load())
        return false;
    auto command = makePythonModuleCommand("stemlab.source_analysis");
    if (command.isEmpty())
        return false;
    command.addArray(arguments);
    const auto source = getCaptureFile();
    analysisThread.reset();
    sourceAnalysisRunning.store(true);
    setStatus(label + "...");
    analysisThread = std::make_unique<StemLabUtilityThread>(
        *this, StemLabUtilityThread::analysisMaintenance, command, source, juce::File{}, label);
    if (!analysisThread->startThread())
    {
        analysisThread.reset();
        sourceAnalysisRunning.store(false);
        return false;
    }
    return true;
}

void StemLabAudioProcessor::finishAnalysisMaintenance(const juce::File& source,
                                                      const juce::String& label, int exitCode)
{
    sourceAnalysisRunning.store(false);

    if (exitCode != 0)
    {
        setStatus(label + " failed - see diagnostics");
        return;
    }

    setStatus(label + " complete");

    if (!beatThisEnabled.load() || !source.existsAsFile())
        return;

    // This runs on the maintenance thread itself, and startSourceAnalysis
    // opens by resetting analysisThread - which is this thread, so calling it
    // from here destroys and joins the thread it is running on. The follow-up
    // therefore starts from the message thread, by which point this thread's
    // run() has returned and the reset is an ordinary join. The weak token
    // drops the callback if the processor goes away first.
    std::weak_ptr<int> lifetime = lifetimeToken;

    juce::MessageManager::callAsync(
        [this, lifetime, source]
        {
            if (lifetime.expired())
                return;

            startSourceAnalysis(source);
        });
}

void StemLabAudioProcessor::setWaveformGridMode(int mode) noexcept
{
    waveformGridMode.store(
        juce::jlimit(static_cast<int>(gridHost), static_cast<int>(gridManual), mode));
    sendChangeMessage();
}

void StemLabAudioProcessor::setManualGrid(double bpm, int numerator, int denominator,
                                          double barOne) noexcept
{
    manualGridBpm.store(juce::jlimit(20.0, 400.0, bpm));
    manualGridNumerator.store(juce::jlimit(1, 32, numerator));
    manualGridDenominator.store(juce::jlimit(1, 32, denominator));
    manualGridBarOne.store(juce::jmax(0.0, barOne));
    sendChangeMessage();
}

StemLabGridInfo StemLabAudioProcessor::getWaveformGridScalars() const
{
    // Everything here reads atomics: the per-lane per-tick display path
    // wants only these scalars and must not pay stateLock plus two vector
    // copies per read. Callers that need the beat vectors go through
    // getWaveformGridInfo below.
    StemLabGridInfo info;
    info.mode = waveformGridMode.load();
    info.captureStartPpq = juce::jmax(0.0, captureStartPpq.load());

    if (info.mode == gridHost)
    {
        info.bpm = lastHostBpm.load();
        info.numerator = lastHostNumerator.load();
        info.denominator = lastHostDenominator.load();
        const auto barLength = info.numerator * 4.0 / juce::jmax(1, info.denominator);
        const auto nextBarPpq = std::ceil(info.captureStartPpq / barLength) * barLength;
        info.barOne = (nextBarPpq - info.captureStartPpq) * 60.0 / info.bpm;
    }
    else if (info.mode == gridManual)
    {
        info.bpm = manualGridBpm.load();
        info.numerator = manualGridNumerator.load();
        info.denominator = manualGridDenominator.load();
        info.barOne = manualGridBarOne.load();
    }
    else
    {
        info.bpm = sourceBpm.load() > 0.0 ? sourceBpm.load() : 120.0;
        info.numerator = sourceMeterNumerator.load();
        info.denominator = sourceMeterDenominator.load();
        info.barOne = sourceBarOne.load();
    }
    return info;
}

StemLabGridInfo StemLabAudioProcessor::getWaveformGridInfo() const
{
    auto info = getWaveformGridScalars();

    if (info.mode == gridSource)
    {
        const juce::ScopedLock lock(stateLock);
        info.beats = sourceBeats;
        info.downbeats = sourceDownbeats;
    }

    return info;
}

int StemLabAudioProcessor::getWaveformLaneHeight(const juce::String& id) const
{
    const juce::ScopedLock lock(laneHeightLock);
    const auto found = waveformLaneHeights.find(id.toStdString());
    return found != waveformLaneHeights.end() ? found->second
                                               : stemlab::waveform::defaultLaneHeight;
}

void StemLabAudioProcessor::setWaveformLaneHeight(const juce::String& id, int height)
{
    const juce::ScopedLock lock(laneHeightLock);
    waveformLaneHeights[id.toStdString()] = stemlab::waveform::clampLaneHeight(height);
    sendChangeMessage();
}

StemLabSelectionRange StemLabAudioProcessor::getStemSelectionRange(const juce::String& id) const
{
    const juce::ScopedLock lock(selectionLock);
    const auto found = stemSelections.find(id.toStdString());
    return found != stemSelections.end() ? found->second : StemLabSelectionRange{};
}

void StemLabAudioProcessor::setStemSelectionRange(const juce::String& id, double start, double end)
{
    start = juce::jlimit(0.0, 1.0, start);
    end = juce::jlimit(0.0, 1.0, end);
    if (end < start)
        std::swap(start, end);

    StemLabSelectionRange range;
    range.start = start;
    range.end = end;
    range.active = end - start >= 0.0001;

    {
        const juce::ScopedLock lock(selectionLock);
        if (range.active)
            stemSelections[id.toStdString()] = range;
        else
            stemSelections.erase(id.toStdString());
    }

    /*
     * Every lane's range takes part in the loop. Upstream keyed the loop off
     * whichever lane was being previewed, and an earlier pass here off the
     * last range drawn; both threw ranges away. The transport is shared by
     * every lane, so the loop is the merged set of all of them: overlapping
     * ranges play through as one stretch, gaps are skipped.
     */
    rebuildLoopRegions();

    sendChangeMessage();
}

void StemLabAudioProcessor::clearStemSelectionRange(const juce::String& id)
{
    {
        const juce::ScopedLock lock(selectionLock);
        stemSelections.erase(id.toStdString());
    }

    rebuildLoopRegions();

    sendChangeMessage();
}

void StemLabAudioProcessor::clearAllStemSelectionRanges()
{
    {
        const juce::ScopedLock lock(selectionLock);
        stemSelections.clear();
    }

    rebuildLoopRegions();

    sendChangeMessage();
}

bool StemLabAudioProcessor::launchStemMidiConversion(int stemIndex)
{
    if (!hasSuccessfulJob() || isEngineRunning() || isMidiConversionRunning() ||
        !juce::isPositiveAndBelow(stemIndex, stemCount))
    {
        return false;
    }

    const auto stem = getStemName(stemIndex);
    return launchMidiConversion(getCompletedStemFile(stemIndex), stem, stem,
                                juce::File::createLegalFileName(stem), stem);
}

bool StemLabAudioProcessor::launchRecursiveMidiConversion(const juce::String& itemId)
{
    if (!hasSuccessfulJob() || isEngineRunning() || isMidiConversionRunning())
        return false;

    for (const auto& item : getRecursiveStemItems())
    {
        if (item.id == itemId)
        {
            const auto stemType = item.category.isNotEmpty() ? item.category : item.rootStem;
            return launchMidiConversion(item.file, stemType, item.label,
                                        juce::File::createLegalFileName(item.id.replace("/", "_")),
                                        item.id);
        }
    }
    return false;
}

bool StemLabAudioProcessor::isMidiConversionRunning() const noexcept
{
    return midiThread != nullptr && midiThread->isThreadRunning();
}

bool StemLabAudioProcessor::launchMidiConversion(const juce::File& source,
                                                 const juce::String& stemType,
                                                 const juce::String& label,
                                                 const juce::String& outputName,
                                                 const juce::String& resultId)
{
    if (!source.existsAsFile())
    {
        setStatus("MIDI source stem was not found");
        return false;
    }

    auto command = makePythonModuleCommand("stemlab.midi");
    if (command.isEmpty())
    {
        setStatus("StemLab MIDI worker could not be located");
        return false;
    }

    const auto midiDirectory = getLastJobDirectory().getChildFile("midi");
    if (!midiDirectory.createDirectory())
    {
        setStatus("Could not create the MIDI output folder");
        return false;
    }

    const auto output = midiDirectory.getChildFile(outputName + ".mid");

    command.add("--input");
    command.add(source.getFullPathName());
    command.add("--output");
    command.add(output.getFullPathName());
    command.add("--stem-type");
    command.add(stemType);

    const auto grid = getWaveformGridInfo();
    command.add("--grid-mode");
    command.add(grid.mode == gridHost ? "host" : (grid.mode == gridManual ? "manual" : "source"));
    command.add("--bar-one");
    command.add(juce::String(grid.barOne, 6));

    if (getSourceBpm() > 0.0)
    {
        command.add("--bpm");
        command.add(juce::String(getSourceBpm(), 3));
    }

    midiThread.reset();
    midiThread = std::make_unique<StemLabUtilityThread>(*this, StemLabUtilityThread::midiConversion,
                                                        command, source, output, label, resultId);
    setStatus("Converting " + label + " to MIDI...");

    if (!midiThread->startThread())
    {
        midiThread.reset();
        setStatus("Could not start MIDI conversion");
        return false;
    }
    return true;
}

void StemLabAudioProcessor::finishMidiConversion(const juce::String& label,
                                                 const juce::File& output, int exitCode,
                                                 const juce::String& resultId)
{
    if (exitCode == 0 && output.existsAsFile() && loadMidiInfo(resultId, output))
    {
        setStatus("MIDI saved: " + output.getFullPathName());
    }
    else
    {
        if (output.existsAsFile())
            output.deleteFile();
        setStatus("MIDI conversion failed for " + label + " - see diagnostics");
    }
}

bool StemLabAudioProcessor::loadMidiInfo(const juce::String& id, const juce::File& midiFile)
{
    const auto metadata = midiFile.getSiblingFile(midiFile.getFileNameWithoutExtension() +
                                                   ".stemlab-midi.json");
    if (!metadata.existsAsFile())
        return false;

    const auto parsed = juce::JSON::parse(metadata.loadFileAsString());
    auto* object = parsed.getDynamicObject();
    if (object == nullptr || static_cast<int>(object->getProperty("schema")) < 1)
        return false;

    StemLabMidiInfo info;
    info.id = id;
    info.sourceStem = object->getProperty("source_stem").toString();
    info.midiFile = juce::File(object->getProperty("midi_file").toString());
    info.dragFile = juce::File(object->getProperty("drag_file").toString());
    info.sourceTempo = static_cast<double>(object->getProperty("source_tempo"));
    info.barOne = static_cast<double>(object->getProperty("bar_one"));
    info.drums = static_cast<bool>(object->getProperty("drums"));

    if (!info.midiFile.existsAsFile())
        info.midiFile = midiFile;

    if (auto* notes = object->getProperty("notes").getArray())
    {
        info.notes.reserve(static_cast<size_t>(notes->size()));
        for (const auto& value : *notes)
        {
            if (auto* note = value.getDynamicObject())
            {
                info.notes.push_back({static_cast<double>(note->getProperty("start")),
                                      static_cast<double>(note->getProperty("end")),
                                      static_cast<int>(note->getProperty("pitch")),
                                      static_cast<int>(note->getProperty("velocity")),
                                      static_cast<double>(note->getProperty("confidence"))});
            }
        }
    }

    if (info.notes.empty())
        return false;

    const juce::ScopedLock lock(midiInfoLock);
    midiInfos[id.toStdString()] = std::move(info);
    sendChangeMessage();
    return true;
}

StemLabMidiInfo StemLabAudioProcessor::getMidiInfo(const juce::String& id) const
{
    const juce::ScopedLock lock(midiInfoLock);
    const auto found = midiInfos.find(id.toStdString());
    return found != midiInfos.end() ? found->second : StemLabMidiInfo{};
}

bool StemLabAudioProcessor::hasMidiInfo(const juce::String& id) const
{
    const auto info = getMidiInfo(id);
    return !info.notes.empty() && info.midiFile.existsAsFile();
}

bool StemLabAudioProcessor::auditionMidi(const juce::String& id)
{
    if (isMidiAuditioning(id))
    {
        stopMidiAudition();
        setStatus("MIDI audition stopped");
        return true;
    }

    const auto info = getMidiInfo(id);
    if (info.notes.empty())
    {
        setStatus("Convert this stem to MIDI first");
        return false;
    }

    previewTransport.stop();
    {
        const juce::ScopedLock lock(midiAuditionLock);
        midiAuditionSynth.allNotesOff(0, false);
        midiAuditionNotes = info.notes;
        midiAuditionId = id;
        midiAuditionPosition = 0.0;
        midiAuditionDuration = 0.0;
        for (const auto& note : midiAuditionNotes)
            midiAuditionDuration = juce::jmax(midiAuditionDuration, note.end);

        // Both orders are built once here so the render block only ever
        // walks forward. Nothing outside the audition reads these copies,
        // so reordering them is free.
        std::sort(midiAuditionNotes.begin(), midiAuditionNotes.end(),
                  [](const StemLabMidiNoteInfo& a, const StemLabMidiNoteInfo& b)
                  { return a.start < b.start; });

        midiAuditionNoteOffOrder.resize(midiAuditionNotes.size());

        for (size_t i = 0; i < midiAuditionNoteOffOrder.size(); ++i)
            midiAuditionNoteOffOrder[i] = i;

        std::sort(midiAuditionNoteOffOrder.begin(), midiAuditionNoteOffOrder.end(),
                  [this](size_t a, size_t b)
                  { return midiAuditionNotes[a].end < midiAuditionNotes[b].end; });

        midiAuditionNoteOnCursor = 0;
        midiAuditionNoteOffCursor = 0;

        midiAuditionActive.store(true);
    }
    setStatus("Auditioning MIDI for " + id);
    return true;
}

bool StemLabAudioProcessor::isMidiAuditioning(const juce::String& id) const
{
    const juce::ScopedLock lock(midiAuditionLock);
    return midiAuditionActive.load() && midiAuditionId == id;
}

void StemLabAudioProcessor::stopMidiAudition()
{
    const juce::ScopedLock lock(midiAuditionLock);
    midiAuditionActive.store(false);
    midiAuditionSynth.allNotesOff(0, false);
    midiAuditionNotes.clear();
    midiAuditionNoteOffOrder.clear();
    midiAuditionNoteOnCursor = 0;
    midiAuditionNoteOffCursor = 0;
    midiAuditionId.clear();
    midiAuditionPosition = 0.0;
    midiAuditionDuration = 0.0;
}

bool StemLabAudioProcessor::renderMidiAudition(juce::AudioBuffer<float>& buffer, int startSample,
                                               int numSamples)
{
    const juce::ScopedLock lock(midiAuditionLock);
    if (!midiAuditionActive.load() || currentSampleRate <= 0.0 || numSamples <= 0)
        return false;

    const auto blockStart = midiAuditionPosition;
    const auto blockEnd = blockStart + static_cast<double>(numSamples) / currentSampleRate;

    midiAuditionEvents.clear();

    auto eventSample = [=](double seconds)
    {
        return startSample + juce::jlimit(
                                 0, numSamples - 1,
                                 static_cast<int>(std::round((seconds - blockStart) *
                                                             currentSampleRate)));
    };

    const auto noteCount = midiAuditionNotes.size();

    /*
     * Note-offs before note-ons, which is what the old note-ordered scan
     * produced and what a legato repeat of the same pitch needs: the voice
     * has to be released before the next one takes it.
     *
     * Each cursor only moves forward. Audition blocks are contiguous
     * (midiAuditionPosition is set to blockEnd below and reset to zero only
     * when the note lists are rebuilt), so anything the cursor has passed
     * is behind blockStart and was already emitted.
     */
    while (midiAuditionNoteOffCursor < noteCount)
    {
        const auto& note = midiAuditionNotes[midiAuditionNoteOffOrder[midiAuditionNoteOffCursor]];

        if (note.end >= blockEnd)
            break;

        if (note.end >= blockStart)
            midiAuditionEvents.addEvent(juce::MidiMessage::noteOff(1, note.pitch),
                                        eventSample(note.end));

        ++midiAuditionNoteOffCursor;
    }

    while (midiAuditionNoteOnCursor < noteCount)
    {
        const auto& note = midiAuditionNotes[midiAuditionNoteOnCursor];

        if (note.start >= blockEnd)
            break;

        if (note.start >= blockStart)
            midiAuditionEvents.addEvent(
                juce::MidiMessage::noteOn(
                    1, note.pitch,
                    static_cast<juce::uint8>(juce::jlimit(1, 127, note.velocity))),
                eventSample(note.start));

        ++midiAuditionNoteOnCursor;
    }

    midiAuditionSynth.renderNextBlock(buffer, midiAuditionEvents, startSample, numSamples);
    midiAuditionPosition = blockEnd;
    if (blockStart > midiAuditionDuration + 0.35)
    {
        midiAuditionSynth.allNotesOff(0, false);
        midiAuditionActive.store(false);
        midiAuditionNotes.clear();
        midiAuditionNoteOffOrder.clear();
        midiAuditionNoteOnCursor = 0;
        midiAuditionNoteOffCursor = 0;
        midiAuditionId.clear();
    }
    return true;
}

bool StemLabAudioProcessor::sendMidiToAbleton(const juce::String& id)
{
    if (isStandaloneApp() || !abletonBridgeActive.load())
    {
        setStatus("StemLab Remote must be active to create an Ableton MIDI clip");
        return false;
    }

    const auto info = getMidiInfo(id);
    if (info.notes.empty())
    {
        setStatus("Convert this stem to MIDI first");
        return false;
    }

    const auto job = getLastJobDirectory();
    if (!job.isDirectory())
        return false;

    const auto grid = getWaveformGridInfo();
    const auto bpm = juce::jlimit(20.0, 400.0, grid.bpm);
    const auto beatsPerSecond = bpm / 60.0;

    auto payload = std::make_unique<juce::DynamicObject>();
    payload->setProperty("protocol", "stemlab-ableton-midi");
    payload->setProperty("version", 1);
    payload->setProperty("source_stem", info.sourceStem);
    payload->setProperty("source_tempo", info.sourceTempo);
    payload->setProperty("grid_mode",
                         grid.mode == gridHost ? "host"
                                               : (grid.mode == gridManual ? "manual" : "source"));
    payload->setProperty("grid_bpm", bpm);
    payload->setProperty("bar_one", grid.barOne);
    payload->setProperty("capture_start_ppq", juce::jmax(0.0, captureStartPpq.load()));
    payload->setProperty("target_track", juce::String{});

    juce::Array<juce::var> notes;
    for (const auto& note : info.notes)
    {
        auto value = std::make_unique<juce::DynamicObject>();
        value->setProperty("pitch", note.pitch);
        value->setProperty("start", juce::jmax(0.0, note.start * beatsPerSecond));
        value->setProperty("duration",
                           juce::jmax(0.0001, (note.end - note.start) * beatsPerSecond));
        value->setProperty("velocity", note.velocity);
        value->setProperty("confidence", note.confidence);
        notes.add(juce::var(value.release()));
    }
    payload->setProperty("notes", juce::var(notes));

    const auto manifest = job.getChildFile(
        "stemlab_ableton_midi_" + juce::File::createLegalFileName(id.replace("/", "_")) + ".json");
    if (!manifest.replaceWithText(juce::JSON::toString(juce::var(payload.release()), true)))
        return false;

    const auto ack = job.getChildFile("stemlab_ableton_midi_ack.json");
    if (ack.existsAsFile())
        ack.deleteFile();

    const auto message = "stemlab_midi_ready " + utf8ToHex(manifest.getFullPathName());
    if (!sendAbletonControlMessage(message))
    {
        setStatus("Could not contact StemLab Remote");
        return false;
    }

    setStatus("Creating MIDI clip in Ableton...");
    return true;
}

juce::String StemLabAudioProcessor::getCurrentPreviewSelectionId() const
{
    // Which lane a selection applies to. Upstream keyed this off a single
    // "preview" slot; the Lanes interface has no such slot - what you hear
    // is the monitor mix - so a soloed lane stands in for it, falling back
    // to the directly previewed stem.
    {
        const juce::ScopedLock lock(recursiveLock);

        for (const auto& item : recursiveItems)
        {
            const auto flags = monitorFlagsForRecursive(item.id);

            if (flags != nullptr && flags->solo.load())
                return item.id;
        }
    }

    for (int i = 0; i < stemCount; ++i)
    {
        const auto flags = monitorFlagsForStem(i);

        if (flags != nullptr && flags->solo.load())
            return getStemName(i);
    }

    const auto index = previewStemIndex.load();

    if (juce::isPositiveAndBelow(index, stemCount))
        return getStemName(index);

    return {};
}

void StemLabAudioProcessor::rebuildLoopRegions()
{
    // Message thread only: mouse gestures and the Esc key are the writers,
    // and the timer that enforces the result runs here too.
    std::vector<stemlab::loops::Region> regions;

    {
        const juce::ScopedLock lock(selectionLock);

        for (const auto& [id, range] : stemSelections)
        {
            juce::ignoreUnused(id);

            if (range.active)
                regions.push_back({range.start, range.end});
        }
    }

    auto merged = stemlab::loops::mergeRegions(std::move(regions));

    {
        const juce::ScopedLock lock(selectionLock);
        loopRegionsNormalised = merged;
    }

    if (merged.empty())
    {
        loopTimer.stopTimer();
        previewLoopWasPlaying = false;
        return;
    }

    // Armed here and re-armed from every transport start: the enforcer
    // stops itself once playback does, so the timer's lifetime follows
    // playback rather than the existence of a range.
    loopTimer.startTimer(30);

    // Sweeping a range pulls the playhead into the loop right away, playing
    // or paused, so pressing play always starts inside what was chosen.
    auto& transport = activeTransport();
    const auto length = transport.getLengthInSeconds();

    if (length > 0.0)
        if (const auto target =
                stemlab::loops::repositionFor(merged, transport.getCurrentPosition() / length))
            transport.setPosition(*target * length);
}

void StemLabAudioProcessor::startLoopTimerIfRegions()
{
    if (!loopRegionsSnapshot().empty())
        loopTimer.startTimer(30);
}

void StemLabAudioProcessor::applyPreviewLoopTick()
{
    auto& transport = activeTransport();
    const bool playing = transport.isPlaying();

    bool haveRegions = false;

    {
        const juce::ScopedLock lock(selectionLock);
        haveRegions = !loopRegionsNormalised.empty();
    }

    /*
     * Ahead of the copy, and ahead of everything else. This runs 33 times a
     * second, and it used to keep running - allocating a fresh region vector
     * on every one of them - for the life of the process after a single
     * swept range, whether or not anything was playing and whether or not
     * the editor was even open.
     *
     * previewLoopWasPlaying is load-bearing in this gate: the transport
     * stops itself at end of file, and the branch below that restarts it
     * from the first region needs a tick after that has happened. One more
     * tick is exactly what carrying the previous playing state buys.
     */
    if (!haveRegions || (!playing && !previewLoopWasPlaying))
    {
        previewLoopWasPlaying = playing;
        loopTimer.stopTimer();
        return;
    }

    std::vector<stemlab::loops::Region> merged;

    {
        const juce::ScopedLock lock(selectionLock);
        merged = loopRegionsNormalised;
    }

    const auto length = transport.getLengthInSeconds();

    // Both early-outs leave nothing this tick can enforce, so they carry the
    // same bookkeeping the tail does. Without it a transport that stopped
    // under a source with no length - or under regions too short to loop -
    // would leave previewLoopWasPlaying stuck true, and the gate above would
    // keep the timer awake on a tick that can never do anything.
    if (merged.empty() || length <= 0.0)
    {
        previewLoopWasPlaying = playing;
        return;
    }

    // A region too small to hold even one tick would pin the transport.
    merged.erase(std::remove_if(merged.begin(), merged.end(),
                                [length](const stemlab::loops::Region& region)
                                { return (region.end - region.start) * length < 0.02; }),
                 merged.end());

    if (merged.empty())
    {
        previewLoopWasPlaying = playing;
        return;
    }

    if (playing)
    {
        if (const auto target =
                stemlab::loops::repositionFor(merged, transport.getCurrentPosition() / length))
            transport.setPosition(*target * length);
    }
    else if (previewLoopWasPlaying && transport.hasStreamFinished())
    {
        // The last region reaches the end of the file: the transport stops
        // itself there, so looping means starting again from the first one.
        transport.setPosition(merged.front().start * length);
        transport.start();
        previewLoopWasPlaying = true;
        return;
    }

    previewLoopWasPlaying = playing;
}

juce::File StemLabAudioProcessor::getStemDragFile(const juce::File& source,
                                                  const juce::String& selectionId)
{
    if (loopRegionsSnapshot().empty())
        return source;

    const auto directory = getLastJobDirectory().getChildFile("selected_regions");
    directory.createDirectory();

    const auto safeName = selectionId.replace("/", "_").replace("\\", "_");

    const auto trimmed =
        exportLoopedRegions(source, directory.getChildFile(safeName + "_selection.wav"));

    // A render that failed must not cancel the gesture: a drag that carries
    // the whole stem is a worse answer than the loop, but silently dropping
    // nothing at all reads as broken drag-and-drop.
    if (!trimmed.existsAsFile())
    {
        setStatus("Could not render the looped " + selectionId +
                  " range - dragging the whole stem instead");
        return source;
    }

    return trimmed;
}
