#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ReaperBridge.h"
#include "StemLabPaths.h"

#include <algorithm>
#include <functional>

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
 * Sums one AudioFormatReaderSource per completed stem on a single shared
 * clock, applying per-stem solo/mute gains.
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
        int stemIndex = 0;
    };

    StemLabStemMixSource(std::vector<Entry> entriesIn,
                         const std::array<std::atomic<bool>, StemLabAudioProcessor::stemCount>& soloIn,
                         const std::array<std::atomic<bool>, StemLabAudioProcessor::stemCount>& muteIn)
        : entries(std::move(entriesIn)), solo(soloIn), mute(muteIn)
    {
        currentGains.resize(entries.size(), 0.0f);
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        for (auto& entry : entries)
            entry.source->prepareToPlay(samplesPerBlockExpected, sampleRate);

        // The wrapping ResamplingAudioSource asks for up to its ring-buffer
        // size (our expected block + 32) in one call, and hosts occasionally
        // deliver larger blocks than announced; growing the scratch inside
        // getNextAudioBlock would allocate on the audio thread.
        scratch.setSize(2, juce::jmax(4096, 2 * samplesPerBlockExpected + 64), false, false,
                        true);

        gainStepPerSample =
            sampleRate > 0.0 ? static_cast<float>(1.0 / (0.010 * sampleRate)) : 0.1f;
    }

    void releaseResources() override
    {
        for (auto& entry : entries)
            entry.source->releaseResources();

        scratch.setSize(0, 0);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        if (info.numSamples <= 0)
            return;

        // One consistent read position for every stem this call; a
        // concurrent seek from the message thread wins the final exchange
        // and simply takes effect next call.
        const auto blockStart = position.load(std::memory_order_acquire);

        bool anySolo = false;

        for (const auto& state : solo)
            anySolo = anySolo || state.load(std::memory_order_relaxed);

        if (scratch.getNumSamples() < info.numSamples)
            scratch.setSize(2, info.numSamples, false, false, true); // last-resort fallback

        const float maxDelta = gainStepPerSample * static_cast<float>(info.numSamples);

        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto& entry = entries[i];

            const auto stem = static_cast<size_t>(entry.stemIndex);

            const bool audible = anySolo ? solo[stem].load(std::memory_order_relaxed)
                                         : !mute[stem].load(std::memory_order_relaxed);

            const float target = audible ? 1.0f : 0.0f;
            const float previous = currentGains[i];

            const float next =
                previous + juce::jlimit(-maxDelta, maxDelta, target - previous);

            if (next <= 0.0f && previous <= 0.0f)
                continue; // fully silent; position is re-seeded from blockStart anyway

            juce::AudioSourceChannelInfo scratchInfo(&scratch, 0, info.numSamples);
            entry.source->setNextReadPosition(blockStart);
            entry.source->getNextAudioBlock(scratchInfo);

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
            longest = juce::jmax(longest, entry.source->getTotalLength());

        return longest;
    }

    bool isLooping() const override { return false; }
    void setLooping(bool) override {}

private:
    std::vector<Entry> entries;
    const std::array<std::atomic<bool>, StemLabAudioProcessor::stemCount>& solo;
    const std::array<std::atomic<bool>, StemLabAudioProcessor::stemCount>& mute;
    std::vector<float> currentGains;
    float gainStepPerSample = 0.1f;
    juce::AudioBuffer<float> scratch;
    std::atomic<juce::int64> position{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabStemMixSource)
};

namespace
{
juce::String timestampForFilename()
{
    return juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

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

class StemLabEngineThread final : public juce::Thread
{
public:
    StemLabEngineThread(StemLabAudioProcessor& ownerIn, juce::StringArray commandIn)
        : juce::Thread("StemLab engine"), owner(ownerIn), command(std::move(commandIn))
    {
    }

    ~StemLabEngineThread() override
    {
        signalThreadShouldExit();

        if (process != nullptr && process->isRunning())
            process->kill();

        stopThread(3000);
    }

    void run() override
    {
        owner.setStatus("Starting...");
        owner.setEngineProgress(0.02);

        process = std::make_unique<juce::ChildProcess>();

        if (!process->start(command,
                            juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            owner.setStatus("Could not start StemLab engine");
            owner.appendEngineLog("Failed to launch engine process.\n");
            process.reset();
            return;
        }

        std::array<char, 4096> buffer{};
        juce::String pendingOutput;

        auto consumeLines = [&owner = owner, &pendingOutput]
        {
            while (true)
            {
                const auto newline = pendingOutput.indexOfChar('\n');
                if (newline < 0)
                    break;

                auto line = pendingOutput.substring(0, newline).trimEnd();
                pendingOutput = pendingOutput.substring(newline + 1);

                if (line.isNotEmpty())
                    owner.handleEngineOutputLine(line);
            }
        };

        while (!threadShouldExit())
        {
            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));

            if (bytes > 0)
            {
                buffer[static_cast<size_t>(bytes)] = '\0';
                pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
                consumeLines();
            }

            if (!process->isRunning())
                break;

            wait(35);
        }

        if (threadShouldExit() && process->isRunning())
            process->kill();

        while (true)
        {
            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));

            if (bytes <= 0)
                break;

            buffer[static_cast<size_t>(bytes)] = '\0';
            pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
            consumeLines();
        }

        if (pendingOutput.trim().isNotEmpty())
            owner.handleEngineOutputLine(pendingOutput.trim());

        const auto exitCode = process->getExitCode();
        process.reset();

        const auto elapsed = juce::jmax(0.0, (nowMs() - owner.engineStartMs.load()) / 1000.0);

        owner.lastEngineDurationSeconds.store(elapsed);

        if (exitCode == 0)
        {
            owner.engineCompletedSuccessfully.store(true);
            owner.setEngineProgress(1.0);

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

                owner.setStatus("Done - audition stems, then Send Selected");
                break;
            }

            case StemLabAudioProcessor::hostIntegrationReaper:
                owner.setStatus("Done - audition stems, then Insert Stems");
                break;

            case StemLabAudioProcessor::hostIntegrationNone:
            default:
                owner.setStatus("Done - audition stems, then choose what to save");
                break;
            }
        }
        else
        {
            owner.engineCompletedSuccessfully.store(false);

            if (!owner.getStatus().startsWithIgnoreCase("Failed - "))
                owner.setStatus("StemLab engine failed - see Settings > Copy diagnostics");

            owner.appendEngineLog("Engine exit code: " + juce::String(exitCode) + "\n");
        }
    }

private:
    StemLabAudioProcessor& owner;
    juce::StringArray command;
    std::unique_ptr<juce::ChildProcess> process;
};

class StemLabRecursiveThread final : public juce::Thread
{
public:
    StemLabRecursiveThread(StemLabAudioProcessor& ownerIn, juce::StringArray commandIn,
                           juce::File manifestFileIn)
        : juce::Thread("StemLab recursive engine"), owner(ownerIn), command(std::move(commandIn)),
          manifestFile(std::move(manifestFileIn))
    {
    }

    ~StemLabRecursiveThread() override
    {
        signalThreadShouldExit();

        if (process != nullptr && process->isRunning())
            process->kill();

        stopThread(3000);
    }

    void run() override
    {
        owner.setEngineProgress(0.01);
        process = std::make_unique<juce::ChildProcess>();

        if (!process->start(command,
                            juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            owner.setStatus("Could not start Recursive Stem Splitting");
            owner.appendEngineLog("Failed to launch recursive engine process.\n");
            process.reset();
            return;
        }

        std::array<char, 4096> buffer{};
        juce::String pendingOutput;

        auto consumeLines = [&owner = owner, &pendingOutput]
        {
            while (true)
            {
                const auto newline = pendingOutput.indexOfChar('\n');
                if (newline < 0)
                    break;

                auto line = pendingOutput.substring(0, newline).trimEnd();
                pendingOutput = pendingOutput.substring(newline + 1);

                if (line.isNotEmpty())
                    owner.handleEngineOutputLine(line);
            }
        };

        while (!threadShouldExit())
        {
            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));

            if (bytes > 0)
            {
                buffer[static_cast<size_t>(bytes)] = '\0';
                pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
                consumeLines();
            }

            if (!process->isRunning())
                break;

            wait(35);
        }

        if (threadShouldExit() && process->isRunning())
            process->kill();

        while (true)
        {
            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));

            if (bytes <= 0)
                break;

            buffer[static_cast<size_t>(bytes)] = '\0';
            pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
            consumeLines();
        }

        if (pendingOutput.trim().isNotEmpty())
            owner.handleEngineOutputLine(pendingOutput.trim());

        const auto exitCode = process->getExitCode();
        process.reset();

        const auto elapsed = juce::jmax(0.0, (nowMs() - owner.engineStartMs.load()) / 1000.0);

        owner.lastEngineDurationSeconds.store(elapsed);

        if (exitCode == 0 && manifestFile.existsAsFile())
        {
            owner.finishRecursiveJob(manifestFile);
            owner.setEngineProgress(1.0);
            owner.setStatus("Recursive Stem Splitting complete");
        }
        else
        {
            if (!owner.getStatus().startsWithIgnoreCase("Failed - "))
                owner.setStatus("Recursive Stem Splitting failed - see diagnostics");

            owner.appendEngineLog("Recursive engine exit code: " + juce::String(exitCode) + "\n");
        }
    }

private:
    StemLabAudioProcessor& owner;
    juce::StringArray command;
    juce::File manifestFile;
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

        auto writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            formatWriter.release(), owner.diskWriterThread, 65536);

        owner.currentSampleRate = sampleRate;
        owner.currentInputChannels = outputChannels;
        owner.systemCaptureSampleRate.store(sampleRate);
        owner.capturedSamples.store(0);

        hr = audioClient->Start();

        if (FAILED(hr))
        {
            fail("Could not start Windows loopback capture: " + hresultText(hr));
            return;
        }

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

                writer->write(converted.getArrayOfReadPointers(), static_cast<int>(frames));

                owner.capturedSamples.fetch_add(static_cast<juce::int64>(frames));

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

    previewFormats.registerBasicFormats();
    diskWriterThread.startThread();

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

#if JUCE_WINDOWS || JUCE_LINUX
    systemLoopbackThread.reset();
#endif

    diskWriterThread.stopThread(2000);
}

void StemLabAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentInputChannels = juce::jmax(1, getTotalNumInputChannels());

    if (!isStandaloneApp())
    {
        previewTransport.prepareToPlay(samplesPerBlock, sampleRate);
        stemMixTransport.prepareToPlay(samplesPerBlock, sampleRate);

        previewScratch.setSize(juce::jmax(1, getTotalNumOutputChannels()),
                               juce::jmax(1, samplesPerBlock), false, false, true);
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
    previewTransport.prepareToPlay(samplesPerBlockExpected, sampleRate);
    stemMixTransport.prepareToPlay(samplesPerBlockExpected, sampleRate);
}

void StemLabAudioProcessor::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
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
            }
        }
    }

    // Standalone input recording uses audio arriving through this processor;
    // system loopback is captured by a separate WASAPI thread.
    if (standaloneRecordingMode.load() != recordingSystem)
    {
        if (auto* writer = activeWriter.load(std::memory_order_acquire))
        {
            writer->write(buffer.getArrayOfReadPointers(), buffer.getNumSamples());

            capturedSamples.fetch_add(buffer.getNumSamples());
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

    previewTransport.setSource(previewReaderSource.get(), 0, nullptr, sourceRate);

    previewTransport.setPosition(0.0);
    previewStemIndex.store(previewStem);

    if (previewStem != -3)
    {
        const juce::ScopedLock lock(recursiveLock);
        previewRecursiveId.clear();
    }

    return true;
}

bool StemLabAudioProcessor::setInputAudioFile(const juce::File& file, double startPpq,
                                              const juce::String& sourceLabel)
{
    if (!file.existsAsFile())
    {
        setStatus("Selected audio file does not exist");
        return false;
    }

    double duration = 0.0;
    bool previewAvailable = false;

    std::unique_ptr<juce::AudioFormatReader> infoReader(previewFormats.createReaderFor(file));

    if (infoReader != nullptr)
    {
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
        engineLog.clear();
        reaperSourceInfo = {};

        inputSourceLabel = sourceLabel.isNotEmpty() ? sourceLabel : file.getFileName();
    }

    captureStartPpq.store(juce::jmax(0.0, startPpq));

    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);
    clearRecursiveResults();

    // The previous job's stems are gone as far as monitoring is concerned.
    unloadStemMix();

    for (auto& state : stemSolo)
        state.store(false);

    for (auto& state : stemMute)
        state.store(false);

    {
        const juce::ScopedLock lock(abletonBridgeLock);
        abletonBridgeStatus =
            isStandaloneApp() ? juce::String{} : "Source ready - Separate All Stems";
    }

    setStatus(previewAvailable ? "Source ready"
                               : "Source ready - preview unavailable until stems are made");

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
        return;

    if (previewStemIndex.load() != -1)
    {
        if (!loadPreviewFile(source, -1))
            return;
    }

    if (previewTransport.isPlaying())
    {
        previewTransport.stop();
        setStatus("Source paused");
        return;
    }

    if (previewTransport.getCurrentPosition() >= previewTransport.getLengthInSeconds() - 0.01)
    {
        previewTransport.setPosition(0.0);
    }

    previewTransport.start();
    setStatus("Playing source");
}

juce::File StemLabAudioProcessor::getCompletedStemFile(int index) const
{
    if (!juce::isPositiveAndBelow(index, stemCount))
        return {};

    const auto job = getLastJobDirectory();

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

    const auto stem = getStemName(index);

    for (const auto& candidate : candidates)
    {
        if (candidate.getFileNameWithoutExtension().containsIgnoreCase(stem))
            return candidate;
    }

    return {};
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
    audioMonitorIsMix.store(false);
    monitorMode.store(monitorOriginal);
}

bool StemLabAudioProcessor::ensureStemMixLoaded()
{
    if (!hasSuccessfulJob())
        return false;

    const auto job = getLastJobDirectory();

    if (!job.isDirectory())
        return false;

    if (stemMixSource != nullptr && job == stemMixJobDirectory)
        return true;

    std::vector<StemLabStemMixSource::Entry> entries;
    double mixRate = 0.0;

    for (int i = 0; i < stemCount; ++i)
    {
        const auto file = getCompletedStemFile(i);

        if (!file.existsAsFile())
            continue;

        std::unique_ptr<juce::AudioFormatReader> reader(previewFormats.createReaderFor(file));

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
        entry.stemIndex = i;
        entries.push_back(std::move(entry));
    }

    if (entries.empty())
        return false;

    const bool wasMixActive = audioMonitorIsMix.load();
    const auto previousPosition = stemMixTransport.getCurrentPosition();

    stemMixTransport.stop();
    stemMixTransport.setSource(nullptr);

    stemMixSource =
        std::make_unique<StemLabStemMixSource>(std::move(entries), stemSolo, stemMute);

    stemMixTransport.setSource(stemMixSource.get(), 0, nullptr, mixRate);
    stemMixTransport.setPosition(wasMixActive ? previousPosition : 0.0);

    stemMixJobDirectory = job;
    return true;
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
        to.start();
}

bool StemLabAudioProcessor::isStemMonitorAvailable() { return ensureStemMixLoaded(); }

void StemLabAudioProcessor::setMonitorMode(int mode)
{
    const auto clamped = mode == monitorStems ? monitorStems : monitorOriginal;

    if (clamped == monitorStems && !ensureStemMixLoaded())
        return;

    monitorMode.store(clamped);

    // Leaving a child audition also ends here: the monitor decides again.
    {
        const juce::ScopedLock lock(recursiveLock);
        previewRecursiveId.clear();
    }

    if (clamped == monitorOriginal)
    {
        // The single-file transport may currently hold a stem or child
        // audition; put the source back before it becomes the monitor.
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
                    previewTransport.start();
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
            setStatus("Paused");
            return;
        }

        if (stemMixTransport.getCurrentPosition() >=
            stemMixTransport.getLengthInSeconds() - 0.01)
        {
            stemMixTransport.setPosition(0.0);
        }

        stemMixTransport.start();
        setStatus("Playing stems");
        return;
    }

    // Original / child-audition path: the single-file transport. When it
    // holds a completed stem from the legacy per-stem API, fall back to the
    // source so the transport button always means Original.
    if (getPreviewRecursiveId().isNotEmpty())
    {
        if (previewTransport.isPlaying())
        {
            previewTransport.stop();
            setStatus("Paused");
            return;
        }

        if (previewTransport.getCurrentPosition() >=
            previewTransport.getLengthInSeconds() - 0.01)
        {
            previewTransport.setPosition(0.0);
        }

        previewTransport.start();
        return;
    }

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

void StemLabAudioProcessor::setStemSolo(int index, bool solo)
{
    if (juce::isPositiveAndBelow(index, stemCount))
        stemSolo[static_cast<size_t>(index)].store(solo);
}

bool StemLabAudioProcessor::isStemSoloed(int index) const
{
    return juce::isPositiveAndBelow(index, stemCount) &&
           stemSolo[static_cast<size_t>(index)].load();
}

void StemLabAudioProcessor::setStemMute(int index, bool mute)
{
    if (juce::isPositiveAndBelow(index, stemCount))
        stemMute[static_cast<size_t>(index)].store(mute);
}

bool StemLabAudioProcessor::isStemMuted(int index) const
{
    return juce::isPositiveAndBelow(index, stemCount) &&
           stemMute[static_cast<size_t>(index)].load();
}

void StemLabAudioProcessor::setAuditionRecursiveStem(const juce::String& itemId, bool on)
{
    if (!on)
    {
        // Return to whatever the monitor mode says.
        setMonitorMode(monitorMode.load());
        return;
    }

    const auto stemFile = getRecursiveStemFile(itemId);

    if (!stemFile.existsAsFile())
        return;

    const auto position = activeTransport().getCurrentPosition();
    const bool wasPlaying = activeTransport().isPlaying();

    stemMixTransport.stop();

    if (!loadPreviewFile(stemFile, -3))
        return;

    {
        const juce::ScopedLock lock(recursiveLock);
        previewRecursiveId = itemId;
    }

    previewTransport.setPosition(
        juce::jlimit(0.0, previewTransport.getLengthInSeconds(), position));

    audioMonitorIsMix.store(false);

    if (wasPlaying)
        previewTransport.start();
}

juce::String StemLabAudioProcessor::getAuditionRecursiveId() const
{
    return getPreviewRecursiveId();
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
        setStatus("Audio device is not ready");
        return false;
    }

    auto* device = standaloneDeviceManager->getCurrentAudioDevice();

    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
    {
        setStatus("Choose a microphone/interface input in Settings");
        return false;
    }

    const auto sampleRate = device->getCurrentSampleRate();

    if (sampleRate <= 0.0)
    {
        setStatus("Audio input sample rate is not ready");
        return false;
    }

    currentSampleRate = sampleRate;

    const int activeInputs = device->getActiveInputChannels().countNumberOfSetBits();

    currentInputChannels = juce::jlimit(1, 2, activeInputs);

    const auto recordingFile = createRecordingFile("input");

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

    threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
        formatWriter.release(), diskWriterThread, 32768);

    {
        const juce::ScopedLock lock(stateLock);
        captureFile = recordingFile;
        lastJobDirectory = juce::File();
        engineLog.clear();
    }

    capturedSamples.store(0);
    inputDurationSeconds.store(0.0);
    captureStartPpq.store(0.0);
    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);

    standaloneRecordingMode.store(recordingInput);

    activeWriter.store(threadedWriter.get(), std::memory_order_release);

    capturing.store(true);
    setStatus("Recording input...");
    return true;
}

void StemLabAudioProcessor::stopStandaloneRecording()
{
    if (!isStandaloneApp() || standaloneRecordingMode.load() != recordingInput ||
        !capturing.exchange(false))
    {
        return;
    }

    activeWriter.store(nullptr, std::memory_order_release);
    threadedWriter.reset();
    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();

    if (recordingFile.existsAsFile() && setStandaloneInputFile(recordingFile))
    {
        setStatus("Input recording ready");
    }
    else
    {
        setStatus("Input recording stopped");
    }
}

bool StemLabAudioProcessor::startSystemAudioRecording()
{
    if (capturing.load() || isEngineRunning())
    {
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

    {
        const juce::ScopedLock lock(stateLock);
        captureFile = recordingFile;
        lastJobDirectory = juce::File();
        engineLog.clear();
    }

    capturedSamples.store(0);
    inputDurationSeconds.store(0.0);

    captureStartPpq.store(isStandaloneApp() ? 0.0 : juce::jmax(0.0, lastKnownHostPpq.load()));

    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);

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

void StemLabAudioProcessor::stopSystemAudioRecording()
{
#if JUCE_WINDOWS || JUCE_LINUX
    if (standaloneRecordingMode.load() != recordingSystem && systemLoopbackThread == nullptr)
    {
        return;
    }

    capturing.store(false);

    bool successful = false;

    if (systemLoopbackThread != nullptr)
    {
        systemLoopbackThread->signalThreadShouldExit();
        systemLoopbackThread->stopThread(5000);
        successful = systemLoopbackThread->wasSuccessful();
        systemLoopbackThread.reset();
    }

    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();

    if (successful && recordingFile.existsAsFile() && recordingFile.getSize() > 44 &&
        setInputAudioFile(recordingFile, captureStartPpq.load(), "System audio recording"))
    {
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

    setStatus("File location set: " + directory.getFullPathName());
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
        setStatus("Live clip ready");

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
        setStatus("Finish the capture before separating");
        return false;
    }

    if (isEngineRunning())
        return false;

    const auto source = getCaptureFile();

    if (!source.existsAsFile())
    {
        setStatus(isStandaloneApp() ? "Select or record audio first"
                                    : "Use Live Clip or Record PC first");
        return false;
    }

    const auto commandName = getEngineCommand().trim();

    if (commandName.isEmpty())
    {
        setStatus("Choose the StemLab engine in Settings");
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
        engineLog.clear();
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
    engineProgress.store(0.01);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);

    engineThread = std::make_unique<StemLabEngineThread>(*this, command);

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
        previewRecursiveId.clear();
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

juce::String StemLabAudioProcessor::getPreviewRecursiveId() const
{
    const juce::ScopedLock lock(recursiveLock);
    return previewRecursiveId;
}

void StemLabAudioProcessor::finishRecursiveJob(const juce::File& manifestFile)
{
    // The Python side owns separation details. The plugin only consumes the
    // schema-2 tree contract and turns child nodes into selectable UI rows.
    const auto parsed = juce::JSON::parse(manifestFile.loadFileAsString());
    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
    {
        setStatus("Recursive result manifest is invalid");
        return;
    }

    const auto parentId = object->getProperty("parent_id").toString();
    const auto rootStem = object->getProperty("root_stem").toString();
    auto* children = object->getProperty("children").getArray();

    if (parentId.isEmpty() || rootStem.isEmpty() || children == nullptr)
    {
        setStatus("Recursive result manifest is incomplete");
        return;
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
        setStatus("Recursive split finished without usable audio files");
        return;
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
    }

    if (parentId == rootStem)
    {
        for (int i = 0; i < stemCount; ++i)
            if (getStemName(i).equalsIgnoreCase(rootStem))
                setStemEnabled(i, false);
    }

    sendChangeMessage();
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

    if (isVocals)
        setStatus("Adaptive vocals: separating lead and backing groups...");
    else if (isDrums)
        setStatus("Adaptive drums: splitting drum components...");
    else
        setStatus("Adaptive lead: detecting foreground and backing layers...");

    recursiveThread = std::make_unique<StemLabRecursiveThread>(
        *this, command, output.getChildFile("recursive_manifest.json"));

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

    setStatus(isDeverb ? "De-reverb: processing isolated lead vocal..."
                       : "Adaptive split: analysing how many useful layers remain...");

    recursiveThread = std::make_unique<StemLabRecursiveThread>(
        *this, command, output.getChildFile("recursive_manifest.json"));

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

    const auto progress = engineProgress.load();

    if (progress < 0.12 || progress >= 0.995)
        return -1.0;

    const auto updated = engineProgressUpdateMs.load();
    const auto start = engineStartMs.load();

    // Model calls can sit on one progress marker for minutes. In that case an
    // elapsed-time projection grows forever, so report an unknown ETA instead.
    if (updated <= start || nowMs() - updated > 5000.0)
        return -1.0;

    const auto elapsedAtUpdate = (updated - start) / 1000.0;
    const auto estimate = elapsedAtUpdate * (1.0 - progress) / progress;
    return juce::jlimit(0.0, 60.0 * 60.0, estimate);
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
    const auto separator = text.indexOfChar('|');

    if (separator <= 0)
        return;

    const auto percent = text.substring(0, separator).getDoubleValue();

    const auto stage = text.substring(separator + 1).trim();

    setEngineProgress(juce::jlimit(0.0, 1.0, percent / 100.0));

    if (stage.isNotEmpty())
    {
        const auto current = getStatus();

        if (current != stage)
            setStatus(stage);
    }
}

juce::String StemLabAudioProcessor::getStatus() const
{
    const juce::ScopedLock lock(stateLock);
    return status;
}

void StemLabAudioProcessor::postUiStatus(const juce::String& message) { setStatus(message); }

juce::String StemLabAudioProcessor::getEngineLog() const
{
    const juce::ScopedLock lock(stateLock);
    return engineLog;
}

juce::File StemLabAudioProcessor::getLastJobDirectory() const
{
    const juce::ScopedLock lock(stateLock);
    return lastJobDirectory;
}

void StemLabAudioProcessor::setStatus(const juce::String& newStatus)
{
    {
        const juce::ScopedLock lock(stateLock);
        status = newStatus;
    }

    sendChangeMessage();
}

void StemLabAudioProcessor::setEngineProgress(double progress)
{
    const auto current = engineProgress.load();
    const auto next = juce::jmax(current, juce::jlimit(0.0, 1.0, progress));

    if (next > current)
        engineProgressUpdateMs.store(nowMs());

    engineProgress.store(next);

    sendChangeMessage();
}

void StemLabAudioProcessor::handleEngineOutputLine(const juce::String& line)
{
    appendEngineLog(line + "\n");

    if (line.startsWithIgnoreCase("STEMLAB_ERROR "))
    {
        const auto message = line.fromFirstOccurrenceOf("STEMLAB_ERROR ", false, false).trim();

        if (message.isNotEmpty())
            setStatus("Failed - " + message);

        return;
    }

    if (line.startsWithIgnoreCase("STEMLAB_PROGRESS "))
    {
        auto tokens = juce::StringArray::fromTokens(line, " ", "\"");

        if (tokens.size() >= 2)
        {
            const auto percent = juce::jlimit(0, 100, tokens[1].getIntValue());

            setEngineProgress(percent / 100.0);

            if (tokens.size() >= 3)
            {
                auto stage = line.fromFirstOccurrenceOf(tokens[1], false, false).trim();

                setStatus(stage);
            }
        }

        return;
    }

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

void StemLabAudioProcessor::appendEngineLog(const juce::String& text)
{
    {
        const juce::ScopedLock lock(stateLock);
        engineLog += text;

        constexpr int maxLogCharacters = 50000;

        if (engineLog.length() > maxLogCharacters)
        {
            engineLog = engineLog.substring(engineLog.length() - maxLogCharacters);
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

    for (int i = 0; i < stemCount; ++i)
    {
        if (!isStemEnabled(i))
            continue;

        const auto source = getCompletedStemFile(i);

        if (!source.existsAsFile())
            continue;

        const auto outputName = baseName + "_" + getStemName(i) + source.getFileExtension();

        auto target = destination.getChildFile(outputName);

        if (target.existsAsFile())
            target.deleteFile();

        if (source.copyFileTo(target))
            ++saved;
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (!item.selected || !item.file.existsAsFile())
            continue;

        auto safeName = item.id.replace("/", "_").replace("\\", "_");
        const auto outputName = baseName + "_" + safeName + item.file.getFileExtension();

        auto target = destination.getChildFile(outputName);

        if (target.existsAsFile())
            target.deleteFile();

        if (item.file.copyFileTo(target))
            ++saved;
    }

    setStatus("Saved " + juce::String(saved) + (saved == 1 ? " stem" : " stems"));

    return saved;
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
    const auto globalStatusFile =
        stemlab::paths::legacyBridgeDirectory().getChildFile("stemlab_remote_status.json");

    if (globalStatusFile.existsAsFile())
    {
        const auto remoteStatus = juce::JSON::parse(globalStatusFile.loadFileAsString());

        if (auto* statusObject = remoteStatus.getDynamicObject())
        {
            if (statusObject->getProperty("protocol").toString() == "stemlab-remote-status")
            {
                const bool active = static_cast<bool>(statusObject->getProperty("active"));

                const double timestamp =
                    static_cast<double>(statusObject->getProperty("timestamp"));

                const auto nowUnix = juce::Time::getCurrentTime().toMilliseconds() / 1000.0;

                // The init heartbeat persists on disk. Treat it as a useful
                // "installed/loaded recently" indication but never override a
                // job-specific wait/import status once a separation exists.
                if (active && nowUnix - timestamp < 24.0 * 60.0 * 60.0 && !hasSuccessfulJob())
                {
                    const juce::ScopedLock lock(abletonBridgeLock);

                    abletonBridgeStatus =
                        "StemLabRemote active - background Ableton integration ready";
                }
            }
        }
    }

    const auto job = getLastJobDirectory();

    if (!job.isDirectory())
        return;

    const auto importProgress = job.getChildFile("stemlab_ableton_import_progress.json");

    if (importProgress.existsAsFile() && abletonBridgeWaitStartMs.load() > 0.0)
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
                return;
            }
        }
    }

    const auto waitStart = abletonBridgeWaitStartMs.load();

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
        setStatus("Choose at least one stem to send");
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
        setStatus("No completed Ableton manifest to import");
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
    // way REAPER's does. On Windows - the only platform Live runs on - the
    // UDP + Remote Script path stays the assumption for any other host.
#if JUCE_WINDOWS
    return hostIntegrationAbletonLive;
#else
    return hostIntegrationNone;
#endif
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
        setStatus(item.message);
        return false;
    }

    if (!setInputAudioFile(item.file, juce::jmax(0.0, item.startQN), item.label))
        return false;

    {
        const juce::ScopedLock lock(stateLock);

        reaperSourceInfo.valid = true;
        reaperSourceInfo.startSeconds = item.startSeconds;
        reaperSourceInfo.lengthSeconds = item.lengthSeconds;
        reaperSourceInfo.startOffsetSeconds = item.startOffsetSeconds;
        reaperSourceInfo.playRate = item.playRate;
        reaperSourceInfo.preservePitch = item.preservePitch;
        reaperSourceInfo.trackNumber = item.trackNumber;
    }

    setStatus("REAPER item ready: " + item.label);
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

    juce::Array<stemlab::reaper::StemToInsert> selected;

    for (const auto& entry : *allStems)
    {
        const auto* stemObject = entry.getDynamicObject();

        if (stemObject == nullptr)
            continue;

        const auto name = stemObject->getProperty("name").toString();

        for (int i = 0; i < stemCount; ++i)
        {
            if (name.equalsIgnoreCase(getStemName(i)) && isStemEnabled(i))
            {
                selected.add({name, juce::File(stemObject->getProperty("path").toString())});
                break;
            }
        }
    }

    if (selected.isEmpty())
    {
        setStatus("Choose at least one stem to insert");
        return false;
    }

    stemlab::reaper::InsertAnchor anchor;
    juce::String sourceLabel;
    bool hasReaperGeometry = false;

    {
        const juce::ScopedLock lock(stateLock);

        sourceLabel = inputSourceLabel;
        hasReaperGeometry = reaperSourceInfo.valid;

        if (hasReaperGeometry)
        {
            anchor.startSeconds = reaperSourceInfo.startSeconds;
            anchor.lengthSeconds = reaperSourceInfo.lengthSeconds;
            anchor.startOffsetSeconds = reaperSourceInfo.startOffsetSeconds;
            anchor.playRate = reaperSourceInfo.playRate;
            anchor.preservePitch = reaperSourceInfo.preservePitch;
            anchor.afterTrackNumber = reaperSourceInfo.trackNumber;
        }
    }

    if (!hasReaperGeometry)
    {
        // A dropped file has no REAPER geometry; place the stems where the
        // captured start beat lands on the current tempo map.
        anchor.startSeconds =
            reaperApi->TimeMap2_QNToTime(nullptr, juce::jmax(0.0, captureStartPpq.load()));
    }

    const auto result = stemlab::reaper::insertStemTracks(*reaperApi, selected, anchor, sourceLabel);

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
            // Insert the selected item's own audio twice, standing in for
            // stems, echoing the item's real geometry.
            juce::Array<stemlab::reaper::StemToInsert> stems;
            stems.add({"vocals", item.file});
            stems.add({"drums", item.file});

            stemlab::reaper::InsertAnchor anchor;
            anchor.startSeconds = item.startSeconds;
            anchor.lengthSeconds = item.lengthSeconds;
            anchor.startOffsetSeconds = item.startOffsetSeconds;
            anchor.playRate = item.playRate;
            anchor.preservePitch = item.preservePitch;
            anchor.afterTrackNumber = item.trackNumber;

            const auto result =
                stemlab::reaper::insertStemTracks(*reaperApi, stems, anchor, "Selftest");

            text << "insert: " << result.inserted << "\n";
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

    // The Standalone portable app and scripts/install_backend.sh write this
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
    setStatus("Engine path auto-detected");
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
    switch (getSeparatorEngineIndex())
    {
    case separatorDemucs:
        return "Demucs";

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
