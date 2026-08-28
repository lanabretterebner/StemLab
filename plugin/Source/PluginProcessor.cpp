#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "WaveformGrid.h"

#include <algorithm>
#include <cmath>
#include <functional>

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

namespace
{
juce::String timestampForFilename()
{
    return juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

/*  How much audio the disk writer refused, in the terms the user cares about.
    Reported at stop rather than logged when it happens: the count is raised on
    the audio thread, where nothing may allocate or format text.
*/
juce::String describeDroppedCapture(juce::int64 samples, double sampleRate)
{
    if (sampleRate > 0.0)
        return juce::String(static_cast<double>(samples) / sampleRate, 2) + " s";

    return juce::String(samples) + " samples";
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

struct MidiAuditionSound final : juce::SynthesiserSound
{
    bool appliesToNote(int) override { return true; }
    bool appliesToChannel(int) override { return true; }
};

class MidiAuditionVoice final : public juce::SynthesiserVoice
{
public:
    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<MidiAuditionSound*>(sound) != nullptr;
    }

    void startNote(int note, float velocity, juce::SynthesiserSound*, int) override
    {
        phase = 0.0;
        level = 0.16 * velocity;
        phaseDelta = juce::MathConstants<double>::twoPi *
                     juce::MidiMessage::getMidiNoteInHertz(note) / getSampleRate();
        release = 0.0;
    }

    void stopNote(float, bool allowTailOff) override
    {
        if (allowTailOff)
            release = 1.0;
        else
            clearCurrentNote();
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void renderNextBlock(juce::AudioBuffer<float>& output, int start, int samples) override
    {
        if (phaseDelta == 0.0)
            return;

        while (--samples >= 0)
        {
            const auto value = static_cast<float>(std::sin(phase) * level);
            for (int channel = 0; channel < output.getNumChannels(); ++channel)
                output.addSample(channel, start, value);

            phase += phaseDelta;
            ++start;

            if (release > 0.0 && (release *= 0.992) < 0.002)
            {
                clearCurrentNote();
                phaseDelta = 0.0;
                break;
            }
        }
    }

private:
    double phase = 0.0;
    double phaseDelta = 0.0;
    double level = 0.0;
    double release = 0.0;
};

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
    StemLabEngineThread(StemLabAudioProcessor& ownerIn, juce::StringArray commandIn,
                        juce::File cancelFileIn, juce::File cleanupDirectoryIn)
        : juce::Thread("FI-STEM engine"), owner(ownerIn), command(std::move(commandIn)),
          cancelFile(std::move(cancelFileIn)), cleanupDirectory(std::move(cleanupDirectoryIn))
    {
    }

    ~StemLabEngineThread() override
    {
        if (isThreadRunning())
            requestCancel();
        stopThread(3500);

        if (process != nullptr && process->isRunning())
            process->kill();
    }

    bool requestCancel()
    {
        const juce::ScopedLock lock(cancelLock);
        if (finished)
            return false;

        if (cancelRequested.exchange(true))
            return true;

        cancelStartedMs.store(nowMs());
        cancelFile.replaceWithText("cancel\n");
        return true;
    }

    void run() override
    {
        owner.setStatus("Starting...");
        owner.setEngineProgress(0.02);

        process = std::make_unique<juce::ChildProcess>();

        if (!process->start(command,
                            juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            process.reset();
            if (finishWasCancelled())
                owner.finishCancelledJob(cleanupDirectory, true);
            else
            {
                owner.setStatus("Could not start FI-STEM engine");
                owner.appendEngineLog("Failed to launch engine process.\n");
            }
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

        while (process->isRunning())
        {
            if (threadShouldExit())
                requestCancel();

            if (cancelRequested.load() && nowMs() - cancelStartedMs.load() > 2500.0)
                process->kill();

            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));

            if (bytes > 0)
            {
                buffer[static_cast<size_t>(bytes)] = '\0';
                pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
                consumeLines();
            }

            wait(35);
        }

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

        if (finishWasCancelled())
        {
            owner.finishCancelledJob(cleanupDirectory, true);
            return;
        }

        if (cancelFile.existsAsFile())
            cancelFile.deleteFile();

        if (exitCode == 0)
        {
            owner.engineCompletedSuccessfully.store(true);
            owner.setEngineProgress(1.0);

            if (owner.isStandaloneApp())
            {
                owner.setStatus("Done - audition stems, then choose what to save");
            }
            else if (owner.isAbletonHost())
            {
                {
                    const juce::ScopedLock lock(owner.abletonBridgeLock);

                    owner.abletonBridgeStatus =
                        "Stems ready - audition them, choose what you want, then Send Selected";
                }

                owner.abletonBridgeWaitStartMs.store(0.0);

                owner.setStatus("Done - audition stems, then Send Selected");
            }
            else
            {
                owner.setStatus("Done - audition stems, then Drag Selected");
            }
        }
        else
        {
            owner.engineCompletedSuccessfully.store(false);

            if (!owner.getStatus().startsWithIgnoreCase("Failed - "))
                owner.setStatus("FI-STEM engine failed - see Settings > Copy diagnostics");

            owner.appendEngineLog("Engine exit code: " + juce::String(exitCode) + "\n");
        }
    }

private:
    bool finishWasCancelled()
    {
        const juce::ScopedLock lock(cancelLock);
        finished = true;
        return cancelRequested.load();
    }

    StemLabAudioProcessor& owner;
    juce::StringArray command;
    juce::File cancelFile;
    juce::File cleanupDirectory;
    juce::CriticalSection cancelLock;
    bool finished = false;
    std::atomic<bool> cancelRequested{false};
    std::atomic<double> cancelStartedMs{0.0};
    std::unique_ptr<juce::ChildProcess> process;
};

class StemLabRecursiveThread final : public juce::Thread
{
public:
    StemLabRecursiveThread(StemLabAudioProcessor& ownerIn, juce::StringArray commandIn,
                           juce::File manifestFileIn, juce::File cancelFileIn,
                           juce::File cleanupDirectoryIn)
        : juce::Thread("FI-STEM recursive engine"), owner(ownerIn), command(std::move(commandIn)),
          manifestFile(std::move(manifestFileIn)), cancelFile(std::move(cancelFileIn)),
          cleanupDirectory(std::move(cleanupDirectoryIn))
    {
    }

    ~StemLabRecursiveThread() override
    {
        if (isThreadRunning())
            requestCancel();
        stopThread(3500);

        if (process != nullptr && process->isRunning())
            process->kill();
    }

    bool requestCancel()
    {
        const juce::ScopedLock lock(cancelLock);
        if (finished)
            return false;

        if (cancelRequested.exchange(true))
            return true;

        cancelStartedMs.store(nowMs());
        cancelFile.replaceWithText("cancel\n");
        return true;
    }

    void run() override
    {
        owner.setEngineProgress(0.01);
        process = std::make_unique<juce::ChildProcess>();

        if (!process->start(command,
                            juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            process.reset();
            if (finishWasCancelled())
                owner.finishCancelledJob(cleanupDirectory, false);
            else
            {
                owner.setStatus("Could not start Recursive Stem Splitting");
                owner.appendEngineLog("Failed to launch recursive engine process.\n");
            }
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

        while (process->isRunning())
        {
            if (threadShouldExit())
                requestCancel();

            if (cancelRequested.load() && nowMs() - cancelStartedMs.load() > 2500.0)
                process->kill();

            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));

            if (bytes > 0)
            {
                buffer[static_cast<size_t>(bytes)] = '\0';
                pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
                consumeLines();
            }

            wait(35);
        }

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

        if (finishWasCancelled())
        {
            owner.finishCancelledJob(cleanupDirectory, false);
            return;
        }

        if (cancelFile.existsAsFile())
            cancelFile.deleteFile();

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
    bool finishWasCancelled()
    {
        const juce::ScopedLock lock(cancelLock);
        finished = true;
        return cancelRequested.load();
    }

    StemLabAudioProcessor& owner;
    juce::StringArray command;
    juce::File manifestFile;
    juce::File cancelFile;
    juce::File cleanupDirectory;
    juce::CriticalSection cancelLock;
    bool finished = false;
    std::atomic<bool> cancelRequested{false};
    std::atomic<double> cancelStartedMs{0.0};
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
                           ? "FI-STEM source analysis"
                           : (kindIn == analysisMaintenance ? "FI-STEM analysis maintenance"
                                                            : "FI-STEM MIDI")),
          owner(ownerIn), kind(kindIn), command(std::move(commandIn)), source(std::move(sourceIn)),
          output(std::move(outputIn)), label(std::move(labelIn)), context(std::move(contextIn)),
          cancelFile(std::move(cancelFileIn))
    {
    }

    ~StemLabUtilityThread() override
    {
        requestCancel();
        signalThreadShouldExit();
        stopThread(2500);
        if (process != nullptr && process->isRunning())
            process->kill();
    }

    bool requestCancel()
    {
        if (!isThreadRunning() || cancelRequested.exchange(true))
            return false;
        cancelStartedMs.store(nowMs());
        if (cancelFile.getFullPathName().isNotEmpty())
            cancelFile.replaceWithText("cancel\n");
        return true;
    }

    void run() override
    {
        process = std::make_unique<juce::ChildProcess>();
        if (!process->start(command,
                            juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            finish(-1);
            process.reset();
            return;
        }

        std::array<char, 4096> buffer{};
        juce::String processOutput;
        juce::String pendingOutput;

        auto forwardAnalysisLines = [&]
        {
            while (true)
            {
                const auto newline = pendingOutput.indexOfChar('\n');
                if (newline < 0)
                    break;
                const auto line = pendingOutput.substring(0, newline).trimEnd();
                pendingOutput = pendingOutput.substring(newline + 1);
                if (line.isNotEmpty())
                    owner.handleEngineOutputLine(line);
            }
        };

        while (!threadShouldExit() && process->isRunning())
        {
            if (cancelRequested.load() && nowMs() - cancelStartedMs.load() > 2000.0)
                process->kill();

            const auto bytes =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size() - 1));
            if (bytes > 0)
            {
                buffer[static_cast<size_t>(bytes)] = '\0';
                processOutput += juce::String::fromUTF8(buffer.data(), bytes);
                if (kind == sourceAnalysis)
                {
                    pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
                    forwardAnalysisLines();
                }
            }
            wait(50);
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
            processOutput += juce::String::fromUTF8(buffer.data(), bytes);
            if (kind == sourceAnalysis)
            {
                pendingOutput += juce::String::fromUTF8(buffer.data(), bytes);
                forwardAnalysisLines();
            }
        }

        if (kind == sourceAnalysis && pendingOutput.trim().isNotEmpty())
            owner.handleEngineOutputLine(pendingOutput.trim());

        const auto exitCode = static_cast<int>(process->getExitCode());
        process.reset();

        if (processOutput.isNotEmpty() && kind != sourceAnalysis)
            owner.appendEngineLog(processOutput.endsWithChar('\n') ? processOutput
                                                                   : processOutput + "\n");

        if (!threadShouldExit() || cancelRequested.load())
            finish(exitCode);
    }

private:
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
    std::unique_ptr<juce::ChildProcess> process;
};

#if JUCE_WINDOWS
class StemLabSystemLoopbackThread final : public juce::Thread
{
public:
    StemLabSystemLoopbackThread(StemLabAudioProcessor& ownerIn, juce::File outputFileIn)
        : juce::Thread("FI-STEM WASAPI loopback"), owner(ownerIn),
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

        // Two seconds of margin at whatever rate the endpoint runs: once the
        // FIFO is full, write() discards rather than waits.
        auto writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            formatWriter.release(), owner.diskWriterThread,
            juce::jmax(65536, static_cast<int>(sampleRate * 2.0)));

        owner.currentSampleRate = sampleRate;
        owner.currentInputChannels = outputChannels;
        owner.capturedSamples.store(0);
        owner.droppedCaptureSamples.store(0);

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

    previewFormats.registerBasicFormats();
    for (int voice = 0; voice < 16; ++voice)
        midiAuditionSynth.addVoice(new MidiAuditionVoice());
    midiAuditionSynth.addSound(new MidiAuditionSound());
    diskWriterThread.startThread();

    const auto discoveredEngine = discoverEngineCommand();

    if (discoveredEngine.isNotEmpty())
        engineCommand = discoveredEngine;

    if (isStandaloneApp())
    {
        // When the portable Standalone app is launched, remember the exact
        // sibling Engine path for the separately installed VST3. This keeps
        // the multi-gigabyte ML runtime portable and avoids copying it into
        // LocalAppData just to make Ableton integration work.
        const juce::File discoveredFile(discoveredEngine);
        const bool isPortableEngine =
            discoveredFile.getFileName().equalsIgnoreCase("python.exe") &&
            discoveredFile.getParentDirectory().getFileName().equalsIgnoreCase("Engine");

        if (isPortableEngine)
        {
            const auto localAppData = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});

            if (localAppData.isNotEmpty())
            {
                auto settingsDirectory = juce::File(localAppData).getChildFile("FI-STEM");

                if (settingsDirectory.createDirectory())
                {
                    settingsDirectory.getChildFile("portable_engine_path.txt")
                        .replaceWithText(discoveredFile.getFullPathName());
                }
            }
        }

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

    analysisThread.reset();
    midiThread.reset();

    if (standaloneDeviceManager != nullptr)
        standaloneDeviceManager->removeAudioCallback(&previewPlayer);

    standaloneDeviceManager = nullptr;

    previewPlayer.setSource(nullptr);
    previewTransport.setSource(nullptr);
    previewReaderSource.reset();

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

    systemLoopbackThread.reset();

    diskWriterThread.stopThread(2000);
}

void StemLabAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    midiAuditionSynth.setCurrentPlaybackSampleRate(sampleRate);
    currentInputChannels = juce::jmax(1, getTotalNumInputChannels());

    if (!isStandaloneApp())
    {
        previewTransport.prepareToPlay(samplesPerBlock, sampleRate);

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
        previewScratch.setSize(0, 0);
    }
}

void StemLabAudioProcessor::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    midiAuditionSynth.setCurrentPlaybackSampleRate(sampleRate);
    previewTransport.prepareToPlay(samplesPerBlockExpected, sampleRate);
}

void StemLabAudioProcessor::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (midiAuditionActive.load())
    {
        bufferToFill.clearActiveBufferRegion();
        renderMidiAudition(*bufferToFill.buffer, bufferToFill.startSample,
                           bufferToFill.numSamples);
    }
    else
    {
        renderPreviewAudioBlock(bufferToFill);
    }
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
                lastHostPlaying.store(position->getIsPlaying());

                if (auto value = position->getPpqPosition())
                    lastKnownHostPpq.store(juce::jmax(0.0, *value));

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
    if (standaloneRecordingMode.load() != recordingSystem)
    {
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

    // In the VST, completed-stem/source preview replaces the source-track
    // audio while previewTransport is playing. That makes a stem Play button
    // behave like an actual audition/solo rather than layering the stem on top
    // of the original song.
    if (!isStandaloneApp() && midiAuditionActive.load())
    {
        buffer.clear();
        renderMidiAudition(buffer, 0, buffer.getNumSamples());
    }
    else if (!isStandaloneApp() && previewTransport.isPlaying() &&
             previewScratch.getNumChannels() > 0)
    {
        const auto requiredSamples = buffer.getNumSamples();

        if (previewScratch.getNumSamples() < requiredSamples)
        {
            previewScratch.setSize(juce::jmax(1, buffer.getNumChannels()), requiredSamples, false,
                                   false, true);
        }

        previewScratch.clear();

        juce::AudioSourceChannelInfo info(&previewScratch, 0, requiredSamples);

        renderPreviewAudioBlock(info);

        buffer.clear();

        const auto channels = juce::jmin(buffer.getNumChannels(), previewScratch.getNumChannels());

        for (int channel = 0; channel < channels; ++channel)
        {
            buffer.addFrom(channel, 0, previewScratch, channel, 0, requiredSamples);
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

    return isAbletonHost() ? stemlab::host::UiMode::ableton
                           : stemlab::host::UiMode::genericVst;
}

juce::String StemLabAudioProcessor::getCurrentPreviewSelectionId() const
{
    const auto index = previewStemIndex.load();
    if (juce::isPositiveAndBelow(index, stemCount))
        return getStemName(index);
    if (index == -3)
        return getPreviewRecursiveId();
    return {};
}

void StemLabAudioProcessor::updatePreviewLoopForId(const juce::String& id)
{
    const auto range = getStemSelectionRange(id);
    const auto length = previewTransport.getLengthInSeconds();
    const bool enabled = range.active && length > 0.0 && range.length() * length >= 0.02;

    previewLoopEnabled.store(enabled);
    previewLoopStart.store(enabled ? range.start * length : 0.0);
    previewLoopEnd.store(enabled ? range.end * length : 0.0);

    if (enabled)
    {
        const auto position = previewTransport.getCurrentPosition();
        const auto start = previewLoopStart.load();
        const auto end = previewLoopEnd.load();
        if (position < start || position >= end)
            previewTransport.setPosition(start);
    }
}

void StemLabAudioProcessor::renderPreviewAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    if (!previewLoopEnabled.load() || info.buffer == nullptr || info.numSamples <= 0)
    {
        previewTransport.getNextAudioBlock(info);
        return;
    }

    const auto loopStart = previewLoopStart.load();
    const auto loopEnd = previewLoopEnd.load();
    const auto sampleRate = juce::jmax(1.0, currentSampleRate);
    if (loopEnd <= loopStart + 1.0 / sampleRate)
    {
        previewTransport.getNextAudioBlock(info);
        return;
    }

    int filled = 0;
    while (filled < info.numSamples)
    {
        auto position = previewTransport.getCurrentPosition();
        if (position < loopStart || position >= loopEnd - 0.5 / sampleRate)
        {
            previewTransport.setPosition(loopStart);
            position = loopStart;
        }

        const auto remainingSeconds = juce::jmax(0.0, loopEnd - position);
        const auto remainingSamples = juce::jmax(
            1, static_cast<int>(std::floor(remainingSeconds * sampleRate + 0.5)));
        const auto chunk = juce::jmin(info.numSamples - filled, remainingSamples);
        juce::AudioSourceChannelInfo slice(info.buffer, info.startSample + filled, chunk);
        previewTransport.getNextAudioBlock(slice);
        filled += chunk;

        if (filled < info.numSamples)
            previewTransport.setPosition(loopStart);
    }
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

    stopMidiAudition();

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

    if (juce::isPositiveAndBelow(previewStem, stemCount))
        updatePreviewLoopForId(getStemName(previewStem));
    else
        previewLoopEnabled.store(false);

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
    droppedCaptureSamples.store(0);
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
        lastJobDirectory = {};
        engineLog.clear();

        inputSourceLabel = sourceLabel.isNotEmpty() ? sourceLabel : file.getFileName();
    }

    captureStartPpq.store(juce::jmax(0.0, startPpq));

    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);
    clearRecursiveResults();
    clearAllStemSelectionRanges();

    if (isAbletonHost())
    {
        const juce::ScopedLock lock(abletonBridgeLock);
        abletonBridgeStatus = "Source ready - Separate All Stems";
    }

    // Source analysis is optional. Always clear results from the previous source
    // so disabling Beat This! never leaves stale key/BPM metadata on a new file.
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

    setStatus(previewAvailable ? "Source ready"
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

void StemLabAudioProcessor::startSourceAnalysis(const juce::File& source)
{
    analysisThread.reset();
    sourceAnalysisRunning.store(true);
    engineCancelRequested.store(false);
    engineProgress.store(0.0);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);
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
        return;
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
        for (int index : {3, 4, 5})
        {
            const auto stem = getCompletedStemFile(index);
            if (stem.existsAsFile())
            {
                command.add("--harmony-stem");
                command.add(stem.getFullPathName());
            }
        }
        const auto bass = getCompletedStemFile(2);
        if (bass.existsAsFile())
        {
            command.add("--bass-stem");
            command.add(bass.getFullPathName());
        }
    }

    analysisThread = std::make_unique<StemLabUtilityThread>(
        *this, StemLabUtilityThread::sourceAnalysis, command, source, output, juce::String{},
        juce::String{}, cancelFile);

    if (!analysisThread->startThread())
    {
        analysisThread.reset();
        sourceAnalysisRunning.store(false);
    }
    setStatus("Analyzing source with Beat This!...");
    sendChangeMessage();
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

juce::String StemLabAudioProcessor::getSourceKey() const
{
    const juce::ScopedLock lock(stateLock);
    return sourceKey;
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
    if (exitCode == 0)
    {
        setStatus(label + " complete");
        if (beatThisEnabled.load() && source.existsAsFile())
            startSourceAnalysis(source);
    }
    else
        setStatus(label + " failed - see diagnostics");
}

bool StemLabAudioProcessor::saveSourceCorrection(double bpm, const juce::String& key,
                                                 int numerator, int denominator, double barOne)
{
    const auto source = getCaptureFile();
    if (!source.existsAsFile())
        return false;
    juce::StringArray arguments{"--input", source.getFullPathName(), "--set-correction",
                                "--correct-bpm", juce::String(bpm, 4), "--correct-key", key,
                                "--correct-meter-numerator", juce::String(numerator),
                                "--correct-meter-denominator", juce::String(denominator),
                                "--correct-bar-one", juce::String(barOne, 6)};
    return launchAnalysisMaintenance(arguments, "Saving local analysis correction");
}

bool StemLabAudioProcessor::forgetSourceCorrection()
{
    const auto source = getCaptureFile();
    if (!source.existsAsFile())
        return false;
    return launchAnalysisMaintenance(
        {"--input", source.getFullPathName(), "--forget-correction"},
        "Forgetting local analysis correction");
}

bool StemLabAudioProcessor::clearAnalysisCache()
{
    return launchAnalysisMaintenance({"--clear-cache"}, "Clearing local analysis cache");
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

StemLabGridInfo StemLabAudioProcessor::getWaveformGridInfo() const
{
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

    if (getCurrentPreviewSelectionId() == id)
        updatePreviewLoopForId(id);

    sendChangeMessage();
}

void StemLabAudioProcessor::clearStemSelectionRange(const juce::String& id)
{
    {
        const juce::ScopedLock lock(selectionLock);
        stemSelections.erase(id.toStdString());
    }

    if (getCurrentPreviewSelectionId() == id)
        updatePreviewLoopForId(id);

    sendChangeMessage();
}

void StemLabAudioProcessor::clearAllStemSelectionRanges()
{
    {
        const juce::ScopedLock lock(selectionLock);
        stemSelections.clear();
    }
    previewLoopEnabled.store(false);
    previewLoopStart.store(0.0);
    previewLoopEnd.store(0.0);
    sendChangeMessage();
}

void StemLabAudioProcessor::soloStemForExport(int index)
{
    if (!juce::isPositiveAndBelow(index, stemCount))
        return;

    const juce::ScopedLock soloLock(exportSoloLock);

    // Right-clicking the currently soloed stem again exits solo export and
    // restores exactly what the user had selected before entering solo mode.
    if (exportSoloActive && !exportSoloRecursive && exportSoloStemIndex == index)
    {
        for (int i = 0; i < stemCount; ++i)
            stemEnabled[static_cast<size_t>(i)].store(exportSoloStemSnapshot[static_cast<size_t>(i)]);

        {
            const juce::ScopedLock lock(recursiveLock);
            for (auto& item : recursiveItems)
            {
                const auto found = exportSoloRecursiveSnapshot.find(item.id.toStdString());
                item.selected = found != exportSoloRecursiveSnapshot.end() ? found->second : false;
            }
        }

        exportSoloActive = false;
        exportSoloRecursive = false;
        exportSoloStemIndex = -1;
        exportSoloRecursiveId.clear();
        exportSoloRecursiveSnapshot.clear();
        setStatus("Solo export off - restored previous export selection");
        return;
    }

    // Snapshot only when entering solo mode. If the user right-clicks another
    // stem while already soloing, keep the original snapshot for clean restore.
    if (!exportSoloActive)
    {
        for (int i = 0; i < stemCount; ++i)
            exportSoloStemSnapshot[static_cast<size_t>(i)] =
                stemEnabled[static_cast<size_t>(i)].load();

        exportSoloRecursiveSnapshot.clear();
        {
            const juce::ScopedLock lock(recursiveLock);
            for (const auto& item : recursiveItems)
                exportSoloRecursiveSnapshot[item.id.toStdString()] = item.selected;
        }
    }

    exportSoloActive = true;
    exportSoloRecursive = false;
    exportSoloStemIndex = index;
    exportSoloRecursiveId.clear();

    for (int i = 0; i < stemCount; ++i)
        stemEnabled[static_cast<size_t>(i)].store(i == index);

    {
        const juce::ScopedLock lock(recursiveLock);
        for (auto& item : recursiveItems)
            item.selected = false;
    }

    setStatus("Solo export: " + getStemName(index) +
              " (right-click again to restore previous selection)");
}

void StemLabAudioProcessor::soloRecursiveStemForExport(const juce::String& itemId)
{
    const juce::ScopedLock soloLock(exportSoloLock);

    if (exportSoloActive && exportSoloRecursive && exportSoloRecursiveId == itemId)
    {
        for (int i = 0; i < stemCount; ++i)
            stemEnabled[static_cast<size_t>(i)].store(exportSoloStemSnapshot[static_cast<size_t>(i)]);

        {
            const juce::ScopedLock lock(recursiveLock);
            for (auto& item : recursiveItems)
            {
                const auto found = exportSoloRecursiveSnapshot.find(item.id.toStdString());
                item.selected = found != exportSoloRecursiveSnapshot.end() ? found->second : false;
            }
        }

        exportSoloActive = false;
        exportSoloRecursive = false;
        exportSoloStemIndex = -1;
        exportSoloRecursiveId.clear();
        exportSoloRecursiveSnapshot.clear();
        setStatus("Solo export off - restored previous export selection");
        return;
    }

    if (!exportSoloActive)
    {
        for (int i = 0; i < stemCount; ++i)
            exportSoloStemSnapshot[static_cast<size_t>(i)] =
                stemEnabled[static_cast<size_t>(i)].load();

        exportSoloRecursiveSnapshot.clear();
        {
            const juce::ScopedLock lock(recursiveLock);
            for (const auto& item : recursiveItems)
                exportSoloRecursiveSnapshot[item.id.toStdString()] = item.selected;
        }
    }

    exportSoloActive = true;
    exportSoloRecursive = true;
    exportSoloStemIndex = -1;
    exportSoloRecursiveId = itemId;

    for (auto& enabled : stemEnabled)
        enabled.store(false);

    juce::String label = itemId;
    {
        const juce::ScopedLock lock(recursiveLock);
        for (auto& item : recursiveItems)
        {
            item.selected = item.id == itemId;
            if (item.selected)
                label = item.label;
        }
    }

    setStatus("Solo export: " + label +
              " (right-click again to restore previous selection)");
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
        setStatus("FI-STEM MIDI worker could not be located");
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
                                                   ".fi-stem-midi.json");
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
    juce::MidiBuffer events;

    auto eventSample = [=](double seconds)
    {
        return startSample + juce::jlimit(
                                 0, numSamples - 1,
                                 static_cast<int>(std::round((seconds - blockStart) *
                                                             currentSampleRate)));
    };

    for (const auto& note : midiAuditionNotes)
    {
        if (note.start >= blockStart && note.start < blockEnd)
            events.addEvent(juce::MidiMessage::noteOn(
                                1, note.pitch,
                                static_cast<juce::uint8>(juce::jlimit(1, 127, note.velocity))),
                            eventSample(note.start));
        if (note.end >= blockStart && note.end < blockEnd)
            events.addEvent(juce::MidiMessage::noteOff(1, note.pitch), eventSample(note.end));
    }

    midiAuditionSynth.renderNextBlock(buffer, events, startSample, numSamples);
    midiAuditionPosition = blockEnd;
    if (blockStart > midiAuditionDuration + 0.35)
    {
        midiAuditionSynth.allNotesOff(0, false);
        midiAuditionActive.store(false);
        midiAuditionNotes.clear();
        midiAuditionId.clear();
    }
    return true;
}

bool StemLabAudioProcessor::sendMidiToAbleton(const juce::String& id)
{
    if (isStandaloneApp() || !abletonBridgeActive.load())
    {
        setStatus("FI-STEM Remote must be active to create an Ableton MIDI clip");
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
        setStatus("Could not contact FI-STEM Remote");
        return false;
    }

    setStatus("Creating MIDI clip in Ableton...");
    return true;
}

bool StemLabAudioProcessor::isMidiConversionRunning() const noexcept
{
    return midiThread != nullptr && midiThread->isThreadRunning();
}

bool StemLabAudioProcessor::playCompletedStem(int index)
{
    if (capturing.load() || !hasSuccessfulJob() || !juce::isPositiveAndBelow(index, stemCount))
    {
        return false;
    }

    const auto stemFile = getCompletedStemFile(index);

    if (!stemFile.existsAsFile())
    {
        setStatus("Stem preview file was not found");
        return false;
    }

    if (previewStemIndex.load() == index)
    {
        if (previewTransport.isPlaying())
        {
            previewTransport.stop();
            setStatus(getStemName(index).toUpperCase() + " paused");
            return true;
        }
    }
    else
    {
        if (!loadPreviewFile(stemFile, index))
        {
            setStatus("Could not load stem preview");
            return false;
        }
    }

    updatePreviewLoopForId(getStemName(index));
    if (previewLoopEnabled.load())
    {
        const auto position = previewTransport.getCurrentPosition();
        if (position < previewLoopStart.load() || position >= previewLoopEnd.load())
            previewTransport.setPosition(previewLoopStart.load());
    }
    else if (previewTransport.getCurrentPosition() >=
             previewTransport.getLengthInSeconds() - 0.01)
    {
        previewTransport.setPosition(0.0);
    }

    previewTransport.start();
    setStatus((previewLoopEnabled.load() ? "Looping " : "Playing ") + getStemName(index));
    return true;
}

bool StemLabAudioProcessor::seekCompletedStem(int index, double normalisedPosition)
{
    if (capturing.load() || !hasSuccessfulJob() || !juce::isPositiveAndBelow(index, stemCount))
    {
        return false;
    }

    const auto stemFile = getCompletedStemFile(index);

    if (!stemFile.existsAsFile())
        return false;

    const bool keepPlaying = previewStemIndex.load() == index && previewTransport.isPlaying();

    if (previewStemIndex.load() != index)
    {
        if (!loadPreviewFile(stemFile, index))
            return false;
    }

    const auto length = previewTransport.getLengthInSeconds();

    if (length <= 0.0)
        return false;

    previewTransport.setPosition(juce::jlimit(0.0, 1.0, normalisedPosition) * length);

    if (keepPlaying)
        previewTransport.start();

    return true;
}

void StemLabAudioProcessor::stopStandalonePlayback()
{
    previewTransport.stop();
    stopMidiAudition();
}

bool StemLabAudioProcessor::isStandalonePlaying() const noexcept
{
    return previewTransport.isPlaying();
}

double StemLabAudioProcessor::getPreviewPositionSeconds() const noexcept
{
    return previewTransport.getCurrentPosition();
}

double StemLabAudioProcessor::getPreviewLengthSeconds() const noexcept
{
    return previewTransport.getLengthInSeconds();
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

    const int activeInputs = device->getActiveInputChannels().countNumberOfSetBits();

    return startThreadedInputCapture("input", sampleRate, juce::jlimit(1, 2, activeInputs), 0.0,
                                     recordingInput, "Recording input...");
}

bool StemLabAudioProcessor::startThreadedInputCapture(const juce::String& prefix,
                                                       double sampleRate, int channels,
                                                       double startPpq, int recordingMode,
                                                       const juce::String& recordingStatus)
{
    if (capturing.load() || isEngineRunning() || sampleRate <= 0.0 || channels <= 0)
        return false;

    currentSampleRate = sampleRate;
    currentInputChannels = juce::jlimit(1, 2, channels);

    const auto recordingFile = createRecordingFile(prefix);

    analysisThread.reset();
    sourceAnalysisRunning.store(false);
    sourceBpm.store(-1.0);
    {
        const juce::ScopedLock lock(stateLock);
        sourceKey.clear();
    }

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
    // write() discards blocks rather than waiting. 32768 samples was 0.68 s at
    // 48 kHz, inside the range of an ordinary flush stall, so budget two
    // seconds at whatever rate the device is actually running.
    const int captureFifoSamples =
        juce::jmax(32768, static_cast<int>(currentSampleRate * 2.0));

    threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
        formatWriter.release(), diskWriterThread, captureFifoSamples);

    {
        const juce::ScopedLock lock(stateLock);
        captureFile = recordingFile;
        lastJobDirectory = {};
        engineLog.clear();
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

    activeWriter.store(nullptr, std::memory_order_release);
    threadedWriter.reset();
    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();

    if (recordingFile.existsAsFile() && setStandaloneInputFile(recordingFile))
    {
        // A short recording that reads as clean is worse than a loud
        // failure: the audio is gone either way, but only one of the
        // two tells the user the take is not the one they played.
        if (const auto dropped = droppedCaptureSamples.load(); dropped > 0)
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
    if (!stemlab::host::canStartHostAudioCapture(getHostUiMode(), capturing.load(),
                                                  isEngineRunning()))
    {
        return false;
    }

    stopStandalonePlayback();

    const auto sampleRate = currentSampleRate;
    const auto channels = juce::jlimit(1, 2, getTotalNumInputChannels());
    if (sampleRate <= 0.0 || channels <= 0)
    {
        setStatus("Host audio input is not ready");
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

    activeWriter.store(nullptr, std::memory_order_release);
    threadedWriter.reset();
    standaloneRecordingMode.store(recordingNone);

    const auto recordingFile = getCaptureFile();
    if (recordingFile.existsAsFile() && recordingFile.getSize() > 44 &&
        setInputAudioFile(recordingFile, captureStartPpq.load(), "Host audio capture"))
    {
        // A short recording that reads as clean is worse than a loud
        // failure: the audio is gone either way, but only one of the
        // two tells the user the take is not the one they played.
        if (const auto dropped = droppedCaptureSamples.load(); dropped > 0)
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

bool StemLabAudioProcessor::startSystemAudioRecording()
{
    if (capturing.load() || isEngineRunning())
    {
        return false;
    }

    stopStandalonePlayback();

#if JUCE_WINDOWS
    if (systemLoopbackThread != nullptr)
    {
        systemLoopbackThread->signalThreadShouldExit();
        systemLoopbackThread.reset();
    }

    const auto recordingFile = createRecordingFile("system");

    analysisThread.reset();
    sourceAnalysisRunning.store(false);
    sourceBpm.store(-1.0);
    {
        const juce::ScopedLock lock(stateLock);
        sourceKey.clear();
    }

    {
        const juce::ScopedLock lock(stateLock);
        captureFile = recordingFile;
        lastJobDirectory = {};
        engineLog.clear();
    }

    capturedSamples.store(0);
    droppedCaptureSamples.store(0);
    inputDurationSeconds.store(0.0);

    captureStartPpq.store(isStandaloneApp() ? 0.0 : juce::jmax(0.0, lastKnownHostPpq.load()));

    engineCompletedSuccessfully.store(false);
    engineProgress.store(0.0);

    standaloneRecordingMode.store(recordingSystem);
    capturing.store(true);

    systemLoopbackThread = std::make_unique<StemLabSystemLoopbackThread>(*this, recordingFile);

    setStatus("Recording system audio - Windows default output");
    systemLoopbackThread->startThread();
    return true;
#else
    setStatus("System audio recording is currently Windows-only");
    return false;
#endif
}

void StemLabAudioProcessor::stopSystemAudioRecording()
{
#if JUCE_WINDOWS
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
        // A short recording that reads as clean is worse than a loud
        // failure: the audio is gone either way, but only one of the
        // two tells the user the take is not the one they played.
        if (const auto dropped = droppedCaptureSamples.load(); dropped > 0)
            setStatus("System audio recording ready - "
                      + describeDroppedCapture(dropped, currentSampleRate)
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
    auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                      .getChildFile("FI-STEM")
                      .getChildFile("Recordings");

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
    {
        root = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                   .getChildFile("FI-STEM")
                   .getChildFile("Jobs");
    }

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

    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("FI-STEM")
        .getChildFile("Jobs");
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

bool StemLabAudioProcessor::toggleHostTransport()
{
    if (isStandaloneApp())
        return false;

    if (auto* hostPlayHead = getPlayHead();
        hostPlayHead != nullptr && hostPlayHead->canControlTransport())
    {
        hostPlayHead->transportPlay(!lastHostPlaying.load());
        return true;
    }

    // Ableton's VST3 playhead is normally read-only. FI-STEM Remote is the
    // existing host-supported bridge and performs this on Live's main thread.
    return sendAbletonControlMessage("stemlab_toggle_transport");
}

bool StemLabAudioProcessor::requestAbletonSourceClip()
{
    if (isStandaloneApp() || capturing.load() || isEngineRunning())
    {
        return false;
    }

    stopStandalonePlayback();

    const auto requestId = juce::Uuid().toString();

    auto replyFolder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("FI-STEM")
                           .getChildFile("Ableton");

    replyFolder.createDirectory();

    const auto replyFile = replyFolder.getChildFile("clip_" + requestId + ".json");

    auto legacyFolder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                            .getChildFile("FI-STEM")
                            .getChildFile("Ableton");

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

    // 0.9.4+ protocol: tell FI-STEM Remote exactly where to write the one-shot
    // reply. This avoids Documents/OneDrive latency.
    const auto modernPayload =
        "stemlab_get_clip " + requestId + " " + utf8ToHex(replyFile.getFullPathName());

    const bool modernSent = sendAbletonControlMessage(modernPayload);

    // Compatibility fallback: 0.9.3 and earlier FI-STEM Remote versions only
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
        setStatus("Could not contact FI-STEM Remote");
        return false;
    }

    setStatus("Getting selected Live clip...");
    return true;
}

void StemLabAudioProcessor::refreshAbletonSourceClipFromDisk()
{
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

            setStatus("FI-STEM Remote did not return the clip. Re-select FIStemRemote in Live "
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

    if (isEngineRunning() || isMidiConversionRunning())
        return false;

    const auto source = getCaptureFile();

    if (!source.existsAsFile())
    {
        setStatus(isStandaloneApp()   ? "Select or record audio first"
                  : isAbletonHost()   ? "Use Live Clip or Record PC first"
                                      : "Capture Host or Record PC first");
        return false;
    }

    const auto commandName = getEngineCommand().trim();

    if (commandName.isEmpty())
    {
        setStatus("Choose the FI-STEM engine in Settings");
        return false;
    }

    juce::StringArray command;
    command.add(commandName);

    // Portable releases ship a relocatable embedded Python runtime under
    // Engine\python.exe rather than requiring a system Python/venv. When
    // auto-discovery resolves that interpreter, launch FI-STEM's worker as a
    // module. The old stemlab-plugin-job.exe development path still works.
    {
        const juce::File commandFile(commandName);
        const auto fileName = commandFile.getFileName();

        if (fileName.equalsIgnoreCase("python.exe") || fileName.equalsIgnoreCase("pythonw.exe") ||
            fileName.equalsIgnoreCase("python"))
        {
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

    if (isAbletonHost())
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

    const auto cancelFile = job.getChildFile("stemlab_cancel.request");
    if (cancelFile.existsAsFile())
        cancelFile.deleteFile();
    command.add("--cancel-file");
    command.add(cancelFile.getFullPathName());

    command.add("--start-ppq");
    command.add(juce::String(juce::jmax(0.0, captureStartPpq.load()), 8));

    command.add("--device");
    command.add("cuda");

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
    engineCancelRequested.store(false);
    engineProgress.store(0.01);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);

    engineThread = std::make_unique<StemLabEngineThread>(*this, command, cancelFile, job);

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

    if (fileName.equalsIgnoreCase("python.exe") || fileName.equalsIgnoreCase("pythonw.exe") ||
        fileName.equalsIgnoreCase("python"))
    {
        command.add(commandName);
        command.add("-m");
        command.add(moduleName);
        return command;
    }

    juce::String workerExecutable;
    if (moduleName == "stemlab.recursive_job")
        workerExecutable = "stemlab-recursive-job";
    else if (moduleName == "stemlab.source_analysis")
        workerExecutable = "stemlab-source-analysis";
    else if (moduleName == "stemlab.midi")
        workerExecutable = "stemlab-midi-job";

    if (workerExecutable.isEmpty())
        return {};

    if (fileName.containsIgnoreCase("stemlab-plugin-job"))
    {
        // Development installs place all workers in the same environment.
        auto siblingExecutable =
            commandFile.getSiblingFile(workerExecutable + commandFile.getFileExtension());

        if (siblingExecutable.existsAsFile())
        {
            command.add(siblingExecutable.getFullPathName());
            return command;
        }
    }

    if (commandName.equalsIgnoreCase("stemlab-plugin-job"))
    {
        command.add(workerExecutable);
        return command;
    }

    return {};
}

void StemLabAudioProcessor::clearRecursiveResults()
{
    {
        const juce::ScopedLock soloLock(exportSoloLock);
        exportSoloActive = false;
        exportSoloRecursive = false;
        exportSoloStemIndex = -1;
        exportSoloRecursiveId.clear();
        exportSoloRecursiveSnapshot.clear();
    }

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

    {
        const juce::ScopedLock lock(midiInfoLock);
        midiInfos.clear();
    }

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
        const juce::ScopedLock soloLock(exportSoloLock);
        exportSoloActive = false;
        exportSoloRecursive = false;
        exportSoloStemIndex = -1;
        exportSoloRecursiveId.clear();
        exportSoloRecursiveSnapshot.clear();
    }

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

bool StemLabAudioProcessor::playRecursiveStem(const juce::String& itemId)
{
    if (capturing.load() || isEngineRunning() || !hasSuccessfulJob())
        return false;

    const auto stemFile = getRecursiveStemFile(itemId);

    if (!stemFile.existsAsFile())
    {
        setStatus("Recursive stem preview file was not found");
        return false;
    }

    const auto currentId = getPreviewRecursiveId();

    if (currentId == itemId && previewTransport.isPlaying())
    {
        previewTransport.stop();
        setStatus("Recursive stem paused");
        return true;
    }

    if (currentId != itemId)
    {
        if (!loadPreviewFile(stemFile, -3))
        {
            setStatus("Could not load recursive stem preview");
            return false;
        }

        const juce::ScopedLock lock(recursiveLock);
        previewRecursiveId = itemId;
    }

    updatePreviewLoopForId(itemId);
    if (previewLoopEnabled.load())
    {
        const auto position = previewTransport.getCurrentPosition();
        if (position < previewLoopStart.load() || position >= previewLoopEnd.load())
            previewTransport.setPosition(previewLoopStart.load());
    }
    else if (previewTransport.getCurrentPosition() >=
             previewTransport.getLengthInSeconds() - 0.01)
    {
        previewTransport.setPosition(0.0);
    }

    previewTransport.start();

    for (const auto& item : getRecursiveStemItems())
    {
        if (item.id == itemId)
        {
            setStatus((previewLoopEnabled.load() ? "Looping " : "Playing ") + item.label);
            break;
        }
    }

    return true;
}

bool StemLabAudioProcessor::seekRecursiveStem(const juce::String& itemId, double normalisedPosition)
{
    if (capturing.load() || isEngineRunning() || !hasSuccessfulJob())
        return false;

    const auto stemFile = getRecursiveStemFile(itemId);
    if (!stemFile.existsAsFile())
        return false;

    const auto currentId = getPreviewRecursiveId();
    const bool keepPlaying = currentId == itemId && previewTransport.isPlaying();

    if (currentId != itemId)
    {
        if (!loadPreviewFile(stemFile, -3))
            return false;

        const juce::ScopedLock lock(recursiveLock);
        previewRecursiveId = itemId;
    }

    updatePreviewLoopForId(itemId);
    const auto length = previewTransport.getLengthInSeconds();
    if (length <= 0.0)
        return false;

    previewTransport.setPosition(juce::jlimit(0.0, 1.0, normalisedPosition) * length);

    if (keepPlaying)
        previewTransport.start();

    sendChangeMessage();
    return true;
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
    if (!hasSuccessfulJob() || isEngineRunning() || isMidiConversionRunning() ||
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

    const auto cancelFile = output.getChildFile("stemlab_cancel.request");
    command.add("--cancel-file");
    command.add(cancelFile.getFullPathName());

    recursiveThread.reset();
    engineCancelRequested.store(false);
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
        *this, command, output.getChildFile("recursive_manifest.json"), cancelFile, output);

    recursiveThread->startThread();
    return true;
}

bool StemLabAudioProcessor::launchRecursiveAction(const juce::String& itemId,
                                                  const juce::String& action)
{
    if (!hasSuccessfulJob() || isEngineRunning() || isMidiConversionRunning())
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

    const auto cancelFile = output.getChildFile("stemlab_cancel.request");
    command.add("--cancel-file");
    command.add(cancelFile.getFullPathName());

    recursiveThread.reset();
    engineCancelRequested.store(false);
    engineProgress.store(0.01);
    engineStartMs.store(nowMs());
    engineProgressUpdateMs.store(engineStartMs.load());
    lastEngineDurationSeconds.store(0.0);

    setStatus(isDeverb ? "De-reverb: processing isolated lead vocal..."
                       : "Adaptive split: analysing how many useful layers remain...");

    recursiveThread = std::make_unique<StemLabRecursiveThread>(
        *this, command, output.getChildFile("recursive_manifest.json"), cancelFile, output);

    recursiveThread->startThread();
    return true;
}

bool StemLabAudioProcessor::isRecursiveEngineRunning() const noexcept
{
    return recursiveThread != nullptr && recursiveThread->isThreadRunning();
}

bool StemLabAudioProcessor::cancelRunningJob()
{
    if (engineCancelRequested.exchange(true))
        return false;

    bool requested = false;
    if (engineThread != nullptr && engineThread->isThreadRunning())
    {
        requested = engineThread->requestCancel();
    }
    else if (recursiveThread != nullptr && recursiveThread->isThreadRunning())
    {
        requested = recursiveThread->requestCancel();
    }
    else if (analysisThread != nullptr && analysisThread->isThreadRunning())
    {
        requested = analysisThread->requestCancel();
    }

    if (!requested)
    {
        engineCancelRequested.store(false);
        return false;
    }

    setStatus("Cancel requested...");
    return true;
}

void StemLabAudioProcessor::finishCancelledJob(const juce::File& cleanupDirectory, bool mainJob)
{
    if (cleanupDirectory.isDirectory())
        cleanupDirectory.deleteRecursively();

    if (mainJob)
    {
        engineCompletedSuccessfully.store(false);
        const juce::ScopedLock lock(stateLock);
        if (lastJobDirectory == cleanupDirectory)
            lastJobDirectory = {};
    }

    engineProgress.store(0.0);
    engineCancelRequested.store(false);
    appendEngineLog("FI-STEM job cancelled by user.\n");
    setStatus("Cancelled - ready");
}

bool StemLabAudioProcessor::isEngineRunning() const noexcept
{
    return (engineThread != nullptr && engineThread->isThreadRunning()) ||
           isRecursiveEngineRunning() ||
           (analysisThread != nullptr && analysisThread->isThreadRunning());
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
    if (isRecursiveEngineRunning() || sourceAnalysisRunning.load())
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

    if (engineCancelRequested.load())
        return;

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

juce::File StemLabAudioProcessor::exportSelectedRegion(
    const juce::File& source, const juce::File& destination, const juce::String& selectionId,
    double* startSeconds, double* endSeconds)
{
    if (startSeconds != nullptr)
        *startSeconds = 0.0;
    if (endSeconds != nullptr)
        *endSeconds = 0.0;

    if (!source.existsAsFile())
        return {};

    const auto range = getStemSelectionRange(selectionId);
    if (!range.active)
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

    const auto startSample = juce::jlimit<juce::int64>(
        0, reader->lengthInSamples - 1,
        static_cast<juce::int64>(std::floor(range.start * reader->lengthInSamples)));
    const auto endSample = juce::jlimit<juce::int64>(
        startSample + 1, reader->lengthInSamples,
        static_cast<juce::int64>(std::ceil(range.end * reader->lengthInSamples)));
    const auto samples = endSample - startSample;

    if (startSeconds != nullptr)
        *startSeconds = static_cast<double>(startSample) / reader->sampleRate;
    if (endSeconds != nullptr)
        *endSeconds = static_cast<double>(endSample) / reader->sampleRate;

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
    if (writer == nullptr || !writer->writeFromAudioReader(*reader, startSample, samples))
    {
        writer.reset();
        target.deleteFile();
        return {};
    }

    writer.reset();
    return target;
}

int StemLabAudioProcessor::saveSelectedStemsTo(const juce::File& destination)
{
    if (!isStandaloneApp() || !hasSuccessfulJob())
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

        const auto id = getStemName(i);
        const auto range = getStemSelectionRange(id);
        const auto outputName = baseName + "_" + id +
                                (range.active ? "_selection.wav" : source.getFileExtension());
        const auto target = destination.getChildFile(outputName);

        if (exportSelectedRegion(source, target, id).existsAsFile())
            ++saved;
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (!item.selected || !item.file.existsAsFile())
            continue;

        auto safeName = item.id.replace("/", "_").replace("\\", "_");
        const auto range = getStemSelectionRange(item.id);
        const auto outputName = baseName + "_" + safeName +
                                (range.active ? "_selection.wav"
                                              : item.file.getFileExtension());
        const auto target = destination.getChildFile(outputName);

        if (exportSelectedRegion(item.file, target, item.id).existsAsFile())
            ++saved;
    }

    setStatus("Saved " + juce::String(saved) + (saved == 1 ? " stem" : " stems"));

    return saved;
}

juce::StringArray StemLabAudioProcessor::getSelectedStemFilesForDrag()
{
    juce::StringArray files;
    if (isEngineRunning() || !hasSuccessfulJob())
        return files;

    const auto selectedRegionDirectory = getLastJobDirectory().getChildFile("selected_regions");

    for (int i = 0; i < stemCount; ++i)
    {
        if (!isStemEnabled(i))
            continue;

        const auto source = getCompletedStemFile(i);
        if (!source.existsAsFile())
            continue;

        const auto id = getStemName(i);
        auto dragFile = source;
        if (getStemSelectionRange(id).active)
        {
            selectedRegionDirectory.createDirectory();
            dragFile = exportSelectedRegion(
                source, selectedRegionDirectory.getChildFile(id + "_selection.wav"), id);
        }

        if (!dragFile.existsAsFile())
        {
            setStatus("Could not export selected " + id + " range");
            return {};
        }

        files.addIfNotAlreadyThere(dragFile.getFullPathName());
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (!item.selected || !item.file.existsAsFile())
            continue;

        auto dragFile = item.file;
        if (getStemSelectionRange(item.id).active)
        {
            selectedRegionDirectory.createDirectory();
            const auto safeName = item.id.replace("/", "_").replace("\\", "_");
            dragFile = exportSelectedRegion(
                item.file, selectedRegionDirectory.getChildFile(safeName + "_selection.wav"),
                item.id);
        }

        if (!dragFile.existsAsFile())
        {
            setStatus("Could not export selected " + item.label + " range");
            return {};
        }

        files.addIfNotAlreadyThere(dragFile.getFullPathName());
    }

    if (files.isEmpty())
        setStatus("Choose at least one stem to drag");

    return files;
}

juce::String StemLabAudioProcessor::getAbletonBridgeStatus() const
{
    const juce::ScopedLock lock(abletonBridgeLock);
    return abletonBridgeStatus;
}

void StemLabAudioProcessor::refreshAbletonBridgeStatusFromDisk()
{
    if (isStandaloneApp())
        return;

    abletonBridgeActive.store(false);

    // The invisible Remote Script writes a small heartbeat/status file when
    // Live loads it. This lets the VST distinguish "integration installed"
    // from "no background script is active" without any visible Live device.
    const auto globalStatusFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                      .getChildFile("FI-STEM")
                                      .getChildFile("Ableton")
                                      .getChildFile("stemlab_remote_status.json");

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

                abletonBridgeActive.store(active && nowUnix - timestamp < 24.0 * 60.0 * 60.0);

                // The init heartbeat persists on disk. Treat it as a useful
                // "installed/loaded recently" indication but never override a
                // job-specific wait/import status once a separation exists.
                if (active && nowUnix - timestamp < 24.0 * 60.0 * 60.0 && !hasSuccessfulJob())
                {
                    const juce::ScopedLock lock(abletonBridgeLock);

                    abletonBridgeStatus =
                        "FI-STEM Remote active - background Ableton integration ready";
                }
            }
        }
    }

    const auto job = getLastJobDirectory();

    if (!job.isDirectory())
        return;

    const auto midiAck = job.getChildFile("stemlab_ableton_midi_ack.json");
    if (midiAck.existsAsFile())
    {
        const auto parsed = juce::JSON::parse(midiAck.loadFileAsString());
        if (auto* object = parsed.getDynamicObject())
        {
            if (object->getProperty("protocol").toString() == "stemlab-ableton-midi-ack")
            {
                const bool success = static_cast<bool>(object->getProperty("success"));
                const auto message = object->getProperty("message").toString();
                setStatus(success ? message : "Ableton MIDI import failed: " + message);
                midiAck.deleteFile();
            }
        }
    }

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
    const auto selectedRegionDirectory = job.getChildFile("selected_regions");
    selectedRegionDirectory.createDirectory();
    const auto grid = getWaveformGridInfo();
    const auto captureStartBeat = juce::jmax(0.0, captureStartPpq.load());

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
                const auto id = getStemName(i);
                const auto range = getStemSelectionRange(id);
                if (range.active)
                {
                    double startSeconds = 0.0;
                    double endSeconds = 0.0;
                    const auto target = selectedRegionDirectory.getChildFile(id + "_selection.wav");
                    const auto exported = exportSelectedRegion(
                        getCompletedStemFile(i), target, id, &startSeconds, &endSeconds);
                    if (!exported.existsAsFile())
                    {
                        setStatus("Could not export selected " + id + " range");
                        return false;
                    }
                    stemObject->setProperty("path",
                                            exported.getFullPathName().replace("\\", "/"));
                    stemObject->setProperty("selection_start_seconds", startSeconds);
                    stemObject->setProperty("selection_end_seconds", endSeconds);
                    stemObject->setProperty(
                        "start_beat",
                        captureStartBeat + startSeconds * juce::jmax(20.0, grid.bpm) / 60.0);
                }
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
        recursiveObject->setProperty("label", "FI-STEM - " + item.label);
        auto exportFile = item.file;
        const auto range = getStemSelectionRange(item.id);
        if (range.active)
        {
            double startSeconds = 0.0;
            double endSeconds = 0.0;
            auto safeName = item.id.replace("/", "_").replace("\\", "_");
            exportFile = exportSelectedRegion(
                item.file, selectedRegionDirectory.getChildFile(safeName + "_selection.wav"),
                item.id, &startSeconds, &endSeconds);
            if (!exportFile.existsAsFile())
            {
                setStatus("Could not export selected " + item.label + " range");
                return false;
            }
            recursiveObject->setProperty("selection_start_seconds", startSeconds);
            recursiveObject->setProperty("selection_end_seconds", endSeconds);
            recursiveObject->setProperty(
                "start_beat",
                captureStartBeat + startSeconds * juce::jmax(20.0, grid.bpm) / 60.0);
        }
        recursiveObject->setProperty("path", exportFile.getFullPathName().replace("\\", "/"));
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

    object->setProperty("selection_mode", "post-audition-ranges");

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
        setStatus("Could not contact FI-STEM Remote");
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

        abletonBridgeStatus = "Retry sent - waiting for FI-STEM Remote...";
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
            // Portable release: keep the whole runtime beside FI-STEM.exe or
            // beside a VST3 folder that Ableton scans directly.
            "Engine/python.exe", "engine/python.exe",

            // Development fallbacks.
            ".venv/Scripts/stemlab-plugin-job.exe", ".venv/Scripts/stemlab-plugin-job",
            "venv/Scripts/stemlab-plugin-job.exe", "venv/Scripts/stemlab-plugin-job"};

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

    // The Standalone portable app writes this pointer on launch. Ableton's
    // VST3 can then reuse the Engine directory from the extracted release
    // instead of requiring a second multi-gigabyte copy.
    const auto localAppData = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});

    if (localAppData.isNotEmpty())
    {
        const auto stemLabLocal = juce::File(localAppData).getChildFile("FI-STEM");

        const auto portablePointer = stemLabLocal.getChildFile("portable_engine_path.txt");

        if (portablePointer.existsAsFile())
        {
            const auto portablePath = portablePointer.loadFileAsString().trim();
            const juce::File portableRuntime(portablePath);

            if (portableRuntime.existsAsFile())
                return portableRuntime.getFullPathName();
        }

        // Backward-compatible fallback for older installer builds that copied
        // the runtime under LocalAppData\FI-STEM\Engine.
        const auto installedRuntime =
            stemLabLocal.getChildFile("Engine").getChildFile("python.exe");

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
    if (!juce::isPositiveAndBelow(index, stemCount))
        return;

    {
        const juce::ScopedLock soloLock(exportSoloLock);
        exportSoloActive = false;
        exportSoloRecursive = false;
        exportSoloStemIndex = -1;
        exportSoloRecursiveId.clear();
        exportSoloRecursiveSnapshot.clear();
    }

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
    rootObject->setProperty("beatThisEnabled", beatThisEnabled.load());
    rootObject->setProperty("analysisMode", sourceAnalysisMode.load());
    rootObject->setProperty("tempoInterpretation", tempoInterpretation.load());
    rootObject->setProperty("waveformGridMode", waveformGridMode.load());
    rootObject->setProperty("manualGridBpm", manualGridBpm.load());
    rootObject->setProperty("manualGridNumerator", manualGridNumerator.load());
    rootObject->setProperty("manualGridDenominator", manualGridDenominator.load());
    rootObject->setProperty("manualGridBarOne", manualGridBarOne.load());

    rootObject->setProperty("jobRootDirectory", getJobRootDirectory().getFullPathName());

    juce::Array<juce::var> stems;

    for (int i = 0; i < stemCount; ++i)
        stems.add(isStemEnabled(i));

    rootObject->setProperty("stems", stems);

    juce::Array<juce::var> laneHeights;
    {
        const juce::ScopedLock lock(laneHeightLock);
        for (const auto& [id, height] : waveformLaneHeights)
        {
            auto lane = std::make_unique<juce::DynamicObject>();
            lane->setProperty("id", juce::String(id));
            lane->setProperty("height", height);
            laneHeights.add(juce::var(lane.release()));
        }
    }
    rootObject->setProperty("waveformLaneHeights", juce::var(laneHeights));

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

    // Beat This! became opt-in in the FI-STEM rebrand. Older 0.9.9 state blobs
    // have an analysisMode property but no toggle; migrate those projects to
    // disabled + Fast so the first explicit enable never unexpectedly starts
    // the heavier final0 model.
    const bool hasBeatThisState = object->hasProperty("beatThisEnabled");
    beatThisEnabled.store(hasBeatThisState
                              ? static_cast<bool>(object->getProperty("beatThisEnabled"))
                              : false);

    if (hasBeatThisState && object->hasProperty("analysisMode"))
        sourceAnalysisMode.store(juce::jlimit(
            static_cast<int>(analysisAccurate), static_cast<int>(analysisFast),
            static_cast<int>(object->getProperty("analysisMode"))));
    else
        sourceAnalysisMode.store(analysisFast);

    if (object->hasProperty("tempoInterpretation"))
        tempoInterpretation.store(juce::jlimit(
            static_cast<int>(tempoHalf), static_cast<int>(tempoDouble),
            static_cast<int>(object->getProperty("tempoInterpretation"))));

    if (object->hasProperty("waveformGridMode"))
        waveformGridMode.store(juce::jlimit(
            static_cast<int>(gridHost), static_cast<int>(gridManual),
            static_cast<int>(object->getProperty("waveformGridMode"))));

    if (object->hasProperty("manualGridBpm"))
        manualGridBpm.store(
            juce::jlimit(20.0, 400.0, static_cast<double>(object->getProperty("manualGridBpm"))));
    if (object->hasProperty("manualGridNumerator"))
        manualGridNumerator.store(juce::jlimit(
            1, 32, static_cast<int>(object->getProperty("manualGridNumerator"))));
    if (object->hasProperty("manualGridDenominator"))
        manualGridDenominator.store(juce::jlimit(
            1, 32, static_cast<int>(object->getProperty("manualGridDenominator"))));
    if (object->hasProperty("manualGridBarOne"))
        manualGridBarOne.store(
            juce::jmax(0.0, static_cast<double>(object->getProperty("manualGridBarOne"))));

    if (auto* lanes = object->getProperty("waveformLaneHeights").getArray())
    {
        const juce::ScopedLock lock(laneHeightLock);
        waveformLaneHeights.clear();
        for (const auto& value : *lanes)
        {
            if (auto* lane = value.getDynamicObject())
            {
                const auto id = lane->getProperty("id").toString().toStdString();
                const auto height = stemlab::waveform::clampLaneHeight(
                    static_cast<int>(lane->getProperty("height")));
                if (!id.empty())
                    waveformLaneHeights[id] = height;
            }
        }
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
