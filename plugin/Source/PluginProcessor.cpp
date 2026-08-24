#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
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
        return juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    }

    double nowMs()
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    juce::String utf8ToHex (const juce::String& text)
    {
        const auto utf8 = text.toUTF8();
        juce::String hex;

        for (int i = 0; i < utf8.sizeInBytes() - 1; ++i)
        {
            hex += juce::String::toHexString (
                       static_cast<int> (
                           static_cast<unsigned char> (
                               utf8.getAddress()[i])))
                       .paddedLeft ('0', 2)
                       .toUpperCase();
        }

        return hex;
    }

   #if JUCE_WINDOWS
    juce::String hresultText (HRESULT result)
    {
        return "0x"
            + juce::String::toHexString (
                static_cast<juce::int64> (
                    static_cast<unsigned long> (result)));
    }

    struct CoTaskMemWaveFormatDeleter
    {
        void operator() (WAVEFORMATEX* value) const noexcept
        {
            if (value != nullptr)
                CoTaskMemFree (value);
        }
    };

    struct EventHandle
    {
        HANDLE value = nullptr;

        ~EventHandle()
        {
            if (value != nullptr)
                CloseHandle (value);
        }
    };

    struct ComApartment
    {
        explicit ComApartment (HRESULT resultIn)
            : result (resultIn),
              shouldUninitialise (SUCCEEDED (resultIn))
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
}

class StemLabEngineThread final : public juce::Thread
{
public:
    StemLabEngineThread (StemLabAudioProcessor& ownerIn,
                         juce::StringArray commandIn,
                         juce::File jobDirectoryIn)
        : juce::Thread ("StemLab engine"),
          owner (ownerIn),
          command (std::move (commandIn)),
          jobDirectory (std::move (jobDirectoryIn))
    {
    }

    ~StemLabEngineThread() override
    {
        signalThreadShouldExit();

        if (process != nullptr && process->isRunning())
            process->kill();

        stopThread (3000);
    }

    void run() override
    {
        owner.setStatus ("Starting...");
        owner.setEngineProgress (0.02);

        process = std::make_unique<juce::ChildProcess>();

        if (! process->start (command,
                              juce::ChildProcess::wantStdOut
                                | juce::ChildProcess::wantStdErr))
        {
            owner.setStatus ("Could not start StemLab engine");
            owner.appendEngineLog ("Failed to launch engine process.\n");
            process.reset();
            return;
        }

        std::array<char, 4096> buffer {};
        juce::String pendingOutput;

        auto consumeLines = [&owner = owner, &pendingOutput]
        {
            while (true)
            {
                const auto newline = pendingOutput.indexOfChar ('\n');
                if (newline < 0)
                    break;

                auto line = pendingOutput.substring (0, newline).trimEnd();
                pendingOutput = pendingOutput.substring (newline + 1);

                if (line.isNotEmpty())
                    owner.handleEngineOutputLine (line);
            }
        };

        while (! threadShouldExit())
        {
            const auto bytes = process->readProcessOutput (
                buffer.data(),
                static_cast<int> (buffer.size() - 1));

            if (bytes > 0)
            {
                buffer[static_cast<size_t> (bytes)] = '\0';
                pendingOutput += juce::String::fromUTF8 (buffer.data(), bytes);
                consumeLines();
            }

            if (! process->isRunning())
                break;

            wait (35);
        }

        if (threadShouldExit() && process->isRunning())
            process->kill();

        while (true)
        {
            const auto bytes = process->readProcessOutput (
                buffer.data(),
                static_cast<int> (buffer.size() - 1));

            if (bytes <= 0)
                break;

            buffer[static_cast<size_t> (bytes)] = '\0';
            pendingOutput += juce::String::fromUTF8 (buffer.data(), bytes);
            consumeLines();
        }

        if (pendingOutput.trim().isNotEmpty())
            owner.handleEngineOutputLine (pendingOutput.trim());

        const auto exitCode = process->getExitCode();
        process.reset();

        const auto elapsed =
            juce::jmax (0.0, (nowMs() - owner.engineStartMs.load()) / 1000.0);

        owner.lastEngineDurationSeconds.store (elapsed);

        if (exitCode == 0)
        {
            owner.engineCompletedSuccessfully.store (true);
            owner.setEngineProgress (1.0);

            if (owner.isStandaloneApp())
            {
                owner.setStatus (
                    "Done - audition stems, then choose what to save");
            }
            else
            {
                {
                    const juce::ScopedLock lock (
                        owner.abletonBridgeLock);

                    owner.abletonBridgeStatus =
                        "Stems ready - audition them, choose what you want, then Send Selected";
                }

                owner.abletonImportedStemCount.store (0);
                owner.abletonBridgeWaitStartMs.store (0.0);

                owner.setStatus (
                    "Done - audition stems, then Send Selected");
            }
        }
        else
        {
            owner.engineCompletedSuccessfully.store (false);

            if (! owner.getStatus().startsWithIgnoreCase ("Failed - "))
                owner.setStatus ("StemLab engine failed - see Settings > Copy diagnostics");

            owner.appendEngineLog (
                "Engine exit code: " + juce::String (exitCode) + "\n");
        }
    }

private:
    StemLabAudioProcessor& owner;
    juce::StringArray command;
    juce::File jobDirectory;
    std::unique_ptr<juce::ChildProcess> process;
};

class StemLabRecursiveThread final : public juce::Thread
{
public:
    StemLabRecursiveThread (StemLabAudioProcessor& ownerIn,
                            juce::StringArray commandIn,
                            juce::File manifestFileIn)
        : juce::Thread ("StemLab recursive engine"),
          owner (ownerIn),
          command (std::move (commandIn)),
          manifestFile (std::move (manifestFileIn))
    {
    }

    ~StemLabRecursiveThread() override
    {
        signalThreadShouldExit();

        if (process != nullptr && process->isRunning())
            process->kill();

        stopThread (3000);
    }

    void run() override
    {
        owner.setEngineProgress (0.01);
        process = std::make_unique<juce::ChildProcess>();

        if (! process->start (command,
                              juce::ChildProcess::wantStdOut
                                | juce::ChildProcess::wantStdErr))
        {
            owner.setStatus ("Could not start Recursive Stem Splitting");
            owner.appendEngineLog ("Failed to launch recursive engine process.\n");
            process.reset();
            return;
        }

        std::array<char, 4096> buffer {};
        juce::String pendingOutput;

        auto consumeLines = [&owner = owner, &pendingOutput]
        {
            while (true)
            {
                const auto newline = pendingOutput.indexOfChar ('\n');
                if (newline < 0)
                    break;

                auto line = pendingOutput.substring (0, newline).trimEnd();
                pendingOutput = pendingOutput.substring (newline + 1);

                if (line.isNotEmpty())
                    owner.handleEngineOutputLine (line);
            }
        };

        while (! threadShouldExit())
        {
            const auto bytes = process->readProcessOutput (
                buffer.data(),
                static_cast<int> (buffer.size() - 1));

            if (bytes > 0)
            {
                buffer[static_cast<size_t> (bytes)] = '\0';
                pendingOutput += juce::String::fromUTF8 (buffer.data(), bytes);
                consumeLines();
            }

            if (! process->isRunning())
                break;

            wait (35);
        }

        if (threadShouldExit() && process->isRunning())
            process->kill();

        while (true)
        {
            const auto bytes = process->readProcessOutput (
                buffer.data(),
                static_cast<int> (buffer.size() - 1));

            if (bytes <= 0)
                break;

            buffer[static_cast<size_t> (bytes)] = '\0';
            pendingOutput += juce::String::fromUTF8 (buffer.data(), bytes);
            consumeLines();
        }

        if (pendingOutput.trim().isNotEmpty())
            owner.handleEngineOutputLine (pendingOutput.trim());

        const auto exitCode = process->getExitCode();
        process.reset();

        const auto elapsed =
            juce::jmax (0.0, (nowMs() - owner.engineStartMs.load()) / 1000.0);

        owner.lastEngineDurationSeconds.store (elapsed);

        if (exitCode == 0 && manifestFile.existsAsFile())
        {
            owner.finishRecursiveJob (manifestFile);
            owner.setEngineProgress (1.0);
            owner.setStatus ("Recursive Stem Splitting complete");
        }
        else
        {
            if (! owner.getStatus().startsWithIgnoreCase ("Failed - "))
                owner.setStatus ("Recursive Stem Splitting failed - see diagnostics");

            owner.appendEngineLog (
                "Recursive engine exit code: " + juce::String (exitCode) + "\n");
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
    StemLabSystemLoopbackThread (
        StemLabAudioProcessor& ownerIn,
        juce::File outputFileIn)
        : juce::Thread ("StemLab WASAPI loopback"),
          owner (ownerIn),
          outputFile (std::move (outputFileIn))
    {
    }

    ~StemLabSystemLoopbackThread() override
    {
        signalThreadShouldExit();
        stopThread (4000);
    }

    bool wasSuccessful() const noexcept
    {
        return successful.load();
    }

    void run() override
    {
        using Microsoft::WRL::ComPtr;

        const auto comResult =
            CoInitializeEx (nullptr, COINIT_MULTITHREADED);

        ComApartment comApartment (comResult);

        if (FAILED (comResult) && comResult != RPC_E_CHANGED_MODE)
        {
            fail ("Could not initialise Windows audio COM: "
                  + hresultText (comResult));
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;

        auto hr = CoCreateInstance (
            __uuidof (MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS (&enumerator));

        if (FAILED (hr))
        {
            fail ("Could not open Windows audio devices: " + hresultText (hr));
            return;
        }

        ComPtr<IMMDevice> renderDevice;

        // Capture the current Windows default playback endpoint. For the
        // user's current setup, if Windows is playing through the Focusrite,
        // this captures that Focusrite-bound system mix directly.
        hr = enumerator->GetDefaultAudioEndpoint (
            eRender,
            eConsole,
            &renderDevice);

        if (FAILED (hr))
        {
            fail ("Could not find the default Windows output: "
                  + hresultText (hr));
            return;
        }

        ComPtr<IAudioClient> audioClient;

        hr = renderDevice->Activate (
            __uuidof (IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**> (audioClient.GetAddressOf()));

        if (FAILED (hr))
        {
            fail ("Could not open the Windows output for loopback: "
                  + hresultText (hr));
            return;
        }

        WAVEFORMATEX* rawMixFormat = nullptr;
        hr = audioClient->GetMixFormat (&rawMixFormat);

        std::unique_ptr<WAVEFORMATEX, CoTaskMemWaveFormatDeleter>
            mixFormat (rawMixFormat);

        if (FAILED (hr) || mixFormat == nullptr)
        {
            fail ("Could not read the Windows output mix format: "
                  + hresultText (hr));
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

        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && mixFormat->cbSize
                >= static_cast<WORD> (
                    sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX)))
        {
            const auto* extensible =
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (
                    mixFormat.get());

            isFloat = IsEqualGUID (
                extensible->SubFormat,
                KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

            isPcm = IsEqualGUID (
                extensible->SubFormat,
                KSDATAFORMAT_SUBTYPE_PCM);
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
            fail (
                "Unsupported Windows loopback format: "
                + juce::String (mixFormat->wBitsPerSample)
                + "-bit");
            return;
        }

        const int sourceChannels =
            juce::jmax (1, static_cast<int> (mixFormat->nChannels));

        const int outputChannels =
            juce::jlimit (1, 2, sourceChannels);

        const double sampleRate =
            static_cast<double> (mixFormat->nSamplesPerSec);

        if (sampleRate <= 0.0)
        {
            fail ("Windows loopback returned an invalid sample rate");
            return;
        }

        // Use timer/polling loopback instead of EVENTCALLBACK. Some WASAPI
        // endpoints reject LOOPBACK | EVENTCALLBACK with
        // AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021), even though ordinary
        // shared-mode loopback works correctly.
        hr = audioClient->Initialize (
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            0,
            0,
            mixFormat.get(),
            nullptr);

        if (FAILED (hr))
        {
            fail ("Could not start WASAPI loopback mode: "
                  + hresultText (hr));
            return;
        }

        ComPtr<IAudioCaptureClient> captureClient;

        hr = audioClient->GetService (
            __uuidof (IAudioCaptureClient),
            reinterpret_cast<void**> (captureClient.GetAddressOf()));

        if (FAILED (hr))
        {
            fail ("Could not open the WASAPI capture client: "
                  + hresultText (hr));
            return;
        }

        auto stream =
            std::make_unique<juce::FileOutputStream> (outputFile);

        if (! stream->openedOk())
        {
            fail ("Could not create the system-audio recording file");
            return;
        }

        juce::WavAudioFormat wav;

        auto* rawWriter = wav.createWriterFor (
            stream.get(),
            sampleRate,
            static_cast<unsigned int> (outputChannels),
            24,
            {},
            0);

        if (rawWriter == nullptr)
        {
            fail ("Could not create the system-audio WAV writer");
            return;
        }

        stream.release();

        auto writer =
            std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
                rawWriter,
                owner.diskWriterThread,
                65536);

        owner.currentSampleRate = sampleRate;
        owner.currentInputChannels = outputChannels;
        owner.capturedSamples.store (0);

        hr = audioClient->Start();

        if (FAILED (hr))
        {
            fail ("Could not start Windows loopback capture: "
                  + hresultText (hr));
            return;
        }

        bool captureFailed = false;

        while (! threadShouldExit())
        {
            // Timer-driven loopback is intentionally conservative. A 5 ms
            // poll interval is tiny compared with stem-separation workloads
            // and avoids endpoint-specific event-callback incompatibilities.
            wait (5);

            UINT32 nextPacketFrames = 0;

            hr = captureClient->GetNextPacketSize (&nextPacketFrames);

            if (FAILED (hr))
            {
                fail ("Could not read Windows loopback packet size: "
                      + hresultText (hr));
                captureFailed = true;
                break;
            }

            while (nextPacketFrames > 0 && ! threadShouldExit())
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                hr = captureClient->GetBuffer (
                    &data,
                    &frames,
                    &flags,
                    &devicePosition,
                    &qpcPosition);

                if (FAILED (hr))
                {
                    fail ("Could not read Windows loopback audio: "
                          + hresultText (hr));
                    captureFailed = true;
                    break;
                }

                juce::AudioBuffer<float> converted (
                    outputChannels,
                    static_cast<int> (frames));

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0
                    || data == nullptr)
                {
                    converted.clear();
                }
                else
                {
                    const auto bytesPerSample =
                        static_cast<int> (
                            mixFormat->wBitsPerSample / 8);

                    const auto blockAlign =
                        static_cast<int> (mixFormat->nBlockAlign);

                    for (int channel = 0;
                         channel < outputChannels;
                         ++channel)
                    {
                        auto* destination =
                            converted.getWritePointer (channel);

                        for (UINT32 frame = 0; frame < frames; ++frame)
                        {
                            const auto* sample =
                                data
                                + static_cast<size_t> (frame)
                                    * static_cast<size_t> (blockAlign)
                                + static_cast<size_t> (channel)
                                    * static_cast<size_t> (bytesPerSample);

                            float value = 0.0f;

                            switch (sourceFormat)
                            {
                                case SourceFormat::float32:
                                {
                                    float input = 0.0f;
                                    std::memcpy (&input, sample, sizeof (float));
                                    value = input;
                                    break;
                                }

                                case SourceFormat::pcm16:
                                {
                                    int16_t input = 0;
                                    std::memcpy (
                                        &input,
                                        sample,
                                        sizeof (int16_t));

                                    value =
                                        static_cast<float> (input)
                                        / 32768.0f;
                                    break;
                                }

                                case SourceFormat::pcm24:
                                {
                                    int32_t input =
                                        static_cast<int32_t> (sample[0])
                                        | (static_cast<int32_t> (sample[1]) << 8)
                                        | (static_cast<int32_t> (sample[2]) << 16);

                                    if ((input & 0x00800000) != 0)
                                        input |= static_cast<int32_t> (0xff000000);

                                    value =
                                        static_cast<float> (input)
                                        / 8388608.0f;
                                    break;
                                }

                                case SourceFormat::pcm32:
                                {
                                    int32_t input = 0;
                                    std::memcpy (
                                        &input,
                                        sample,
                                        sizeof (int32_t));

                                    value =
                                        static_cast<float> (
                                            static_cast<double> (input)
                                            / 2147483648.0);
                                    break;
                                }

                                case SourceFormat::unsupported:
                                    break;
                            }

                            destination[frame] =
                                juce::jlimit (-1.0f, 1.0f, value);
                        }
                    }
                }

                writer->write (
                    converted.getArrayOfReadPointers(),
                    static_cast<int> (frames));

                owner.capturedSamples.fetch_add (
                    static_cast<juce::int64> (frames));

                captureClient->ReleaseBuffer (frames);

                if (captureFailed)
                    break;

                nextPacketFrames = 0;
                hr = captureClient->GetNextPacketSize (&nextPacketFrames);

                if (FAILED (hr))
                {
                    fail ("Could not continue Windows loopback capture: "
                          + hresultText (hr));
                    captureFailed = true;
                    break;
                }
            }

            if (captureFailed)
                break;
        }

        audioClient->Stop();
        writer.reset();

        if (! captureFailed)
            successful.store (true);
    }

private:
    void fail (const juce::String& message)
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

    StemLabAudioProcessor& owner;
    juce::File outputFile;
    std::atomic<bool> successful { false };
};
#endif

StemLabAudioProcessor::StemLabAudioProcessor()
    : AudioProcessor (
          BusesProperties()
              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& value : stemEnabled)
        value.store (true);

    previewFormats.registerBasicFormats();
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
        const juce::File discoveredFile (discoveredEngine);
        const bool isPortableEngine =
            discoveredFile.getFileName().equalsIgnoreCase ("python.exe")
            && discoveredFile.getParentDirectory()
                .getFileName().equalsIgnoreCase ("Engine");

        if (isPortableEngine)
        {
            const auto localAppData =
                juce::SystemStats::getEnvironmentVariable (
                    "LOCALAPPDATA",
                    {});

            if (localAppData.isNotEmpty())
            {
                auto settingsDirectory =
                    juce::File (localAppData)
                        .getChildFile ("StemLab");

                if (settingsDirectory.createDirectory())
                {
                    settingsDirectory
                        .getChildFile ("portable_engine_path.txt")
                        .replaceWithText (
                            discoveredFile.getFullPathName());
                }
            }
        }

        previewPlayer.setSource (&previewTransport);

       #if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            standaloneDeviceManager = &holder->deviceManager;
            standaloneDeviceManager->addAudioCallback (&previewPlayer);

            // Physical input recording needs JUCE's standalone input enabled.
            // StemLab clears the standalone processor output below, so that
            // live input is never monitored back to the speakers.
            holder->getMuteInputValue().setValue (false);
        }
       #endif
    }
}

StemLabAudioProcessor::~StemLabAudioProcessor()
{
    cancelPendingUpdate();
    stopCapture();
    stopStandalonePlayback();

    if (standaloneDeviceManager != nullptr)
        standaloneDeviceManager->removeAudioCallback (&previewPlayer);

    standaloneDeviceManager = nullptr;

    previewPlayer.setSource (nullptr);
    previewTransport.setSource (nullptr);
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

    diskWriterThread.stopThread (2000);
}

void StemLabAudioProcessor::prepareToPlay (
    double sampleRate,
    int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentInputChannels = juce::jmax (1, getTotalNumInputChannels());

    if (! isStandaloneApp())
    {
        previewTransport.prepareToPlay (
            samplesPerBlock,
            sampleRate);

        previewScratch.setSize (
            juce::jmax (1, getTotalNumOutputChannels()),
            juce::jmax (1, samplesPerBlock),
            false,
            false,
            true);
    }
}

void StemLabAudioProcessor::releaseResources()
{
    // Do not tear down WASAPI loopback just because JUCE/Live reconfigures
    // the normal device. System audio is captured independently.
    if (standaloneRecordingMode.load() != recordingSystem)
        stopCapture();

    if (! isStandaloneApp())
    {
        previewTransport.releaseResources();
        previewScratch.setSize (0, 0);
    }
}

void StemLabAudioProcessor::prepareToPlay (int samplesPerBlockExpected,
                                           double sampleRate)
{
    previewTransport.prepareToPlay (samplesPerBlockExpected, sampleRate);
}

void StemLabAudioProcessor::releaseResourcesForAudioSource()
{
    previewTransport.releaseResources();
}

void StemLabAudioProcessor::getNextAudioBlock (
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    previewTransport.getNextAudioBlock (bufferToFill);
}

bool StemLabAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input  = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    if (input != output)
        return false;

    return input == juce::AudioChannelSet::mono()
        || input == juce::AudioChannelSet::stereo();
}

void StemLabAudioProcessor::processBlock (
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto inputChannels =
        getTotalNumInputChannels();

    const auto outputChannels =
        getTotalNumOutputChannels();

    for (int channel = inputChannels;
         channel < outputChannels;
         ++channel)
    {
        buffer.clear (
            channel,
            0,
            buffer.getNumSamples());
    }

    bool hostPlaying = false;
    bool hasHostPosition = false;
    double hostPpq = 0.0;
    juce::int64 hostTimelineSample = 0;

    if (! isStandaloneApp())
    {
        if (auto* hostPlayHead = getPlayHead())
        {
            if (auto position = hostPlayHead->getPosition())
            {
                hasHostPosition = true;
                hostPlaying = position->getIsPlaying();

                if (auto value = position->getPpqPosition())
                    hostPpq = *value;

                if (auto value = position->getTimeInSamples())
                    hostTimelineSample = *value;

                lastKnownHostPpq.store (
                    juce::jmax (0.0, hostPpq));

                lastKnownHostTimelineSample.store (
                    juce::jmax<juce::int64> (
                        0,
                        hostTimelineSample));
            }
        }

        // Arm Capture means "start on the first playing Live audio block".
        // This avoids recording silence between pressing the button and
        // starting Live's transport, and makes the captured PPQ unambiguous.
        if (captureArmed.load()
            && hasHostPosition
            && hostPlaying
            && threadedWriter != nullptr)
        {
            captureStartPpq.store (
                juce::jmax (0.0, hostPpq));

            captureStartTimelineSample.store (
                hostTimelineSample);

            capturedSamples.store (0);

            activeWriter.store (
                threadedWriter.get(),
                std::memory_order_release);

            captureArmed.store (false);
            capturing.store (true);
            hostWasPlayingDuringCapture.store (true);
        }

        // Once Live stops after capture actually started, close the writer on
        // the message thread. We never destroy the writer from the audio thread.
        if (capturing.load()
            && hostWasPlayingDuringCapture.load()
            && hasHostPosition
            && ! hostPlaying)
        {
            activeWriter.store (
                nullptr,
                std::memory_order_release);

            capturing.store (false);
            captureFinalizeRequested.store (true);
            triggerAsyncUpdate();
        }
    }

    // Physical-input/VST capture uses audio arriving through this processor.
    // System loopback is captured by a separate WASAPI thread.
    if (standaloneRecordingMode.load() != recordingSystem)
    {
        if (auto* writer =
                activeWriter.load (
                    std::memory_order_acquire))
        {
            writer->write (
                buffer.getArrayOfReadPointers(),
                buffer.getNumSamples());

            capturedSamples.fetch_add (
                buffer.getNumSamples());
        }
    }

    // In the VST, completed-stem/source preview replaces the source-track
    // audio while previewTransport is playing. That makes a stem Play button
    // behave like an actual audition/solo rather than layering the stem on top
    // of the original song.
    if (! isStandaloneApp()
        && previewTransport.isPlaying()
        && previewScratch.getNumChannels() > 0)
    {
        const auto requiredSamples =
            buffer.getNumSamples();

        if (previewScratch.getNumSamples() < requiredSamples)
        {
            previewScratch.setSize (
                juce::jmax (
                    1,
                    buffer.getNumChannels()),
                requiredSamples,
                false,
                false,
                true);
        }

        previewScratch.clear();

        juce::AudioSourceChannelInfo info (
            &previewScratch,
            0,
            requiredSamples);

        previewTransport.getNextAudioBlock (info);

        buffer.clear();

        const auto channels =
            juce::jmin (
                buffer.getNumChannels(),
                previewScratch.getNumChannels());

        for (int channel = 0;
             channel < channels;
             ++channel)
        {
            buffer.addFrom (
                channel,
                0,
                previewScratch,
                channel,
                0,
                requiredSamples);
        }
    }

    // Standalone recording is intentionally silent monitoring. Preview audio
    // is supplied by previewPlayer as a separate AudioDeviceManager callback.
    if (isStandaloneApp())
        buffer.clear();
}

void StemLabAudioProcessor::handleAsyncUpdate()
{
    if (captureFinalizeRequested.exchange (false))
        finalizeHostCapture();
}

void StemLabAudioProcessor::finalizeHostCapture()
{
    activeWriter.store (
        nullptr,
        std::memory_order_release);

    threadedWriter.reset();
    captureArmed.store (false);
    capturing.store (false);
    hostWasPlayingDuringCapture.store (false);

    if (capturedSamples.load() > 0)
        setStatus ("Capture ready - choose stems");
    else
        setStatus ("Capture stopped");
}

bool StemLabAudioProcessor::isStandaloneApp() const noexcept
{
    return wrapperType == wrapperType_Standalone;
}

bool StemLabAudioProcessor::loadPreviewFile (
    const juce::File& file,
    int previewStem)
{
    // Previewing is supported by both wrappers:
    //
    // Standalone -> previewPlayer / AudioDeviceManager callback
    // VST3       -> processBlock() pulls previewTransport into plugin output
    //
    // This used to reject every non-Standalone call, which meant the Ableton
    // Play buttons could never load a source/stem even though the VST preview
    // audio path itself was already implemented.
    if (! file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (
        previewFormats.createReaderFor (file));

    if (reader == nullptr)
        return false;

    const auto sourceRate = reader->sampleRate;

    previewTransport.stop();
    previewTransport.setSource (nullptr);
    previewReaderSource.reset();

    auto* readerPtr = reader.release();

    previewReaderSource =
        std::make_unique<juce::AudioFormatReaderSource> (
            readerPtr,
            true);

    previewTransport.setSource (
        previewReaderSource.get(),
        0,
        nullptr,
        sourceRate);

    previewTransport.setPosition (0.0);
    previewStemIndex.store (previewStem);

    if (previewStem != -3)
    {
        const juce::ScopedLock lock (recursiveLock);
        previewRecursiveId.clear();
    }

    return true;
}

bool StemLabAudioProcessor::setInputAudioFile (
    const juce::File& file,
    double startPpq,
    const juce::String& sourceLabel)
{
    if (! file.existsAsFile())
    {
        setStatus ("Selected audio file does not exist");
        return false;
    }

    double duration = 0.0;
    bool previewAvailable = false;

    std::unique_ptr<juce::AudioFormatReader> infoReader (
        previewFormats.createReaderFor (file));

    if (infoReader != nullptr)
    {
        currentInputChannels =
            static_cast<int> (
                infoReader->numChannels);

        if (infoReader->sampleRate > 0.0)
        {
            duration =
                static_cast<double> (
                    infoReader->lengthInSamples)
                / infoReader->sampleRate;
        }

        capturedSamples.store (
            infoReader->lengthInSamples);

        infoReader.reset();

        previewAvailable =
            loadPreviewFile (
                file,
                -1);
    }
    else
    {
        // This is intentionally not fatal. The Python engine normalizes
        // compressed/container audio with FFmpeg before RoFormer, so a file
        // may be perfectly separable even when JUCE has no source-preview
        // decoder for that specific format.
        capturedSamples.store (0);
        inputDurationSeconds.store (0.0);

        previewTransport.stop();
        previewTransport.setSource (nullptr);
        previewReaderSource.reset();
        previewStemIndex.store (-2);
    }

    inputDurationSeconds.store (
        juce::jmax (0.0, duration));

    {
        const juce::ScopedLock lock (stateLock);
        captureFile = file;
        lastJobDirectory = {};
        engineLog.clear();

        inputSourceLabel =
            sourceLabel.isNotEmpty()
                ? sourceLabel
                : file.getFileName();
    }

    captureStartPpq.store (
        juce::jmax (0.0, startPpq));

    captureStartTimelineSample.store (0);

    captureArmed.store (false);
    captureFinalizeRequested.store (false);
    hostWasPlayingDuringCapture.store (false);

    engineCompletedSuccessfully.store (false);
    engineProgress.store (0.0);
    clearRecursiveResults();

    {
        const juce::ScopedLock lock (abletonBridgeLock);
        abletonBridgeStatus =
            isStandaloneApp()
                ? juce::String {}
                : "Source ready - Separate All Stems";
    }

    setStatus (
        previewAvailable
            ? "Source ready"
            : "Source ready - preview unavailable until stems are made");

    return true;
}

bool StemLabAudioProcessor::setStandaloneInputFile (
    const juce::File& file)
{
    if (! isStandaloneApp())
        return false;

    return setInputAudioFile (
        file,
        0.0,
        file.getFileName());
}

juce::String StemLabAudioProcessor::getInputSourceLabel() const
{
    const juce::ScopedLock lock (stateLock);
    return inputSourceLabel;
}

void StemLabAudioProcessor::toggleStandalonePlayback()
{
    if (capturing.load())
        return;

    const auto source = getCaptureFile();

    if (! source.existsAsFile())
        return;

    if (previewStemIndex.load() != -1)
    {
        if (! loadPreviewFile (source, -1))
            return;
    }

    if (previewTransport.isPlaying())
    {
        previewTransport.stop();
        setStatus ("Source paused");
        return;
    }

    if (previewTransport.getCurrentPosition()
        >= previewTransport.getLengthInSeconds() - 0.01)
    {
        previewTransport.setPosition (0.0);
    }

    previewTransport.start();
    setStatus ("Playing source");
}

juce::File StemLabAudioProcessor::getCompletedStemFile (int index) const
{
    if (! juce::isPositiveAndBelow (index, stemCount))
        return {};

    const auto job = getLastJobDirectory();

    if (! job.isDirectory())
        return {};

    auto sourceFolder = job.getChildFile ("refined");

    if (! sourceFolder.isDirectory())
        sourceFolder = job.getChildFile ("baseline");

    if (! sourceFolder.isDirectory())
        return {};

    juce::Array<juce::File> candidates;
    sourceFolder.findChildFiles (
        candidates,
        juce::File::findFiles,
        true,
        "*.wav");

    juce::Array<juce::File> flacs;
    sourceFolder.findChildFiles (
        flacs,
        juce::File::findFiles,
        true,
        "*.flac");

    candidates.addArray (flacs);

    const auto stem = getStemName (index);

    for (const auto& candidate : candidates)
    {
        if (candidate.getFileNameWithoutExtension().containsIgnoreCase (stem))
            return candidate;
    }

    return {};
}

bool StemLabAudioProcessor::playCompletedStem (int index)
{
    if (capturing.load()
        || ! hasSuccessfulJob()
        || ! juce::isPositiveAndBelow (index, stemCount))
    {
        return false;
    }

    const auto stemFile = getCompletedStemFile (index);

    if (! stemFile.existsAsFile())
    {
        setStatus ("Stem preview file was not found");
        return false;
    }

    if (previewStemIndex.load() == index)
    {
        if (previewTransport.isPlaying())
        {
            previewTransport.stop();
            setStatus (getStemName (index).toUpperCase() + " paused");
            return true;
        }
    }
    else
    {
        if (! loadPreviewFile (stemFile, index))
        {
            setStatus ("Could not load stem preview");
            return false;
        }
    }

    if (previewTransport.getCurrentPosition()
        >= previewTransport.getLengthInSeconds() - 0.01)
    {
        previewTransport.setPosition (0.0);
    }

    previewTransport.start();
    setStatus ("Playing " + getStemName (index));
    return true;
}

bool StemLabAudioProcessor::seekCompletedStem (
    int index,
    double normalisedPosition)
{
    if (capturing.load()
        || ! hasSuccessfulJob()
        || ! juce::isPositiveAndBelow (index, stemCount))
    {
        return false;
    }

    const auto stemFile = getCompletedStemFile (index);

    if (! stemFile.existsAsFile())
        return false;

    const bool keepPlaying =
        previewStemIndex.load() == index
        && previewTransport.isPlaying();

    if (previewStemIndex.load() != index)
    {
        if (! loadPreviewFile (stemFile, index))
            return false;
    }

    const auto length =
        previewTransport.getLengthInSeconds();

    if (length <= 0.0)
        return false;

    previewTransport.setPosition (
        juce::jlimit (0.0, 1.0, normalisedPosition)
        * length);

    if (keepPlaying)
        previewTransport.start();

    return true;
}

void StemLabAudioProcessor::stopStandalonePlayback()
{
    previewTransport.stop();
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
    if (! isStandaloneApp()
        || capturing.load()
        || isEngineRunning())
    {
        return false;
    }

    stopStandalonePlayback();

    if (standaloneDeviceManager == nullptr)
    {
        setStatus ("Audio device is not ready");
        return false;
    }

    auto* device = standaloneDeviceManager->getCurrentAudioDevice();

    if (device == nullptr
        || device->getActiveInputChannels().countNumberOfSetBits() == 0)
    {
        setStatus ("Choose a microphone/interface input in Settings");
        return false;
    }

    const auto sampleRate = device->getCurrentSampleRate();

    if (sampleRate <= 0.0)
    {
        setStatus ("Audio input sample rate is not ready");
        return false;
    }

    currentSampleRate = sampleRate;

    const int activeInputs =
        device->getActiveInputChannels().countNumberOfSetBits();

    currentInputChannels = juce::jlimit (1, 2, activeInputs);

    const auto recordingFile = createRecordingFile();

    auto stream =
        std::make_unique<juce::FileOutputStream> (recordingFile);

    if (! stream->openedOk())
    {
        setStatus ("Could not create recording WAV");
        return false;
    }

    juce::WavAudioFormat wav;

    auto* rawWriter = wav.createWriterFor (
        stream.get(),
        currentSampleRate,
        static_cast<unsigned int> (currentInputChannels),
        24,
        {},
        0);

    if (rawWriter == nullptr)
    {
        setStatus ("Could not create recording writer");
        return false;
    }

    stream.release();

    threadedWriter =
        std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            rawWriter,
            diskWriterThread,
            32768);

    {
        const juce::ScopedLock lock (stateLock);
        captureFile = recordingFile;
        lastJobDirectory = {};
        engineLog.clear();
    }

    capturedSamples.store (0);
    inputDurationSeconds.store (0.0);
    captureStartPpq.store (0.0);
    captureStartTimelineSample.store (0);
    engineCompletedSuccessfully.store (false);
    engineProgress.store (0.0);

    standaloneRecordingMode.store (recordingInput);

    activeWriter.store (
        threadedWriter.get(),
        std::memory_order_release);

    capturing.store (true);
    setStatus ("Recording input...");
    return true;
}

void StemLabAudioProcessor::stopStandaloneRecording()
{
    if (! isStandaloneApp()
        || standaloneRecordingMode.load() != recordingInput
        || ! capturing.exchange (false))
    {
        return;
    }

    activeWriter.store (nullptr, std::memory_order_release);
    threadedWriter.reset();
    standaloneRecordingMode.store (recordingNone);

    const auto recordingFile = getCaptureFile();

    if (recordingFile.existsAsFile()
        && setStandaloneInputFile (recordingFile))
    {
        setStatus ("Input recording ready");
    }
    else
    {
        setStatus ("Input recording stopped");
    }
}

bool StemLabAudioProcessor::startSystemAudioRecording()
{
    if (capturing.load()
        || isEngineRunning())
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

    const auto recordingFile = createSystemRecordingFile();

    {
        const juce::ScopedLock lock (stateLock);
        captureFile = recordingFile;
        lastJobDirectory = {};
        engineLog.clear();
    }

    capturedSamples.store (0);
    inputDurationSeconds.store (0.0);

    captureStartPpq.store (
        isStandaloneApp()
            ? 0.0
            : juce::jmax (
                0.0,
                lastKnownHostPpq.load()));

    captureStartTimelineSample.store (
        isStandaloneApp()
            ? 0
            : juce::jmax<juce::int64> (
                0,
                lastKnownHostTimelineSample.load()));

    engineCompletedSuccessfully.store (false);
    engineProgress.store (0.0);

    standaloneRecordingMode.store (recordingSystem);
    capturing.store (true);

    systemLoopbackThread =
        std::make_unique<StemLabSystemLoopbackThread> (
            *this,
            recordingFile);

    setStatus ("Recording system audio - Windows default output");
    systemLoopbackThread->startThread();
    return true;
   #else
    setStatus ("System audio recording is currently Windows-only");
    return false;
   #endif
}

void StemLabAudioProcessor::stopSystemAudioRecording()
{
   #if JUCE_WINDOWS
    if (standaloneRecordingMode.load() != recordingSystem
        && systemLoopbackThread == nullptr)
    {
        return;
    }

    capturing.store (false);

    bool successful = false;

    if (systemLoopbackThread != nullptr)
    {
        systemLoopbackThread->signalThreadShouldExit();
        systemLoopbackThread->stopThread (5000);
        successful = systemLoopbackThread->wasSuccessful();
        systemLoopbackThread.reset();
    }

    standaloneRecordingMode.store (recordingNone);

    const auto recordingFile = getCaptureFile();

    if (successful
        && recordingFile.existsAsFile()
        && recordingFile.getSize() > 44
        && setInputAudioFile (
            recordingFile,
            captureStartPpq.load(),
            "System audio recording"))
    {
        setStatus ("System audio recording ready");
    }
    else if (! getStatus().startsWithIgnoreCase (
                 "System audio recording failed"))
    {
        setStatus ("System audio recording stopped");
    }
   #endif
}

juce::File StemLabAudioProcessor::createCaptureFile() const
{
    auto folder = juce::File::getSpecialLocation (
                      juce::File::userDocumentsDirectory)
                      .getChildFile ("StemLab")
                      .getChildFile ("Captures");

    folder.createDirectory();

    return folder.getNonexistentChildFile (
        "capture_" + timestampForFilename(),
        ".wav",
        false);
}

juce::File StemLabAudioProcessor::createRecordingFile() const
{
    auto folder = juce::File::getSpecialLocation (
                      juce::File::userDocumentsDirectory)
                      .getChildFile ("StemLab")
                      .getChildFile ("Recordings");

    folder.createDirectory();

    return folder.getNonexistentChildFile (
        "input_" + timestampForFilename(),
        ".wav",
        false);
}

juce::File StemLabAudioProcessor::createSystemRecordingFile() const
{
    auto folder = juce::File::getSpecialLocation (
                      juce::File::userDocumentsDirectory)
                      .getChildFile ("StemLab")
                      .getChildFile ("Recordings");

    folder.createDirectory();

    return folder.getNonexistentChildFile (
        "system_" + timestampForFilename(),
        ".wav",
        false);
}

juce::File StemLabAudioProcessor::createJobDirectory() const
{
    juce::File root;

    {
        const juce::ScopedLock lock (stateLock);
        root = jobRootDirectory;
    }

    if (! root.isDirectory())
    {
        root =
            juce::File::getSpecialLocation (
                juce::File::userDocumentsDirectory)
                .getChildFile ("StemLab")
                .getChildFile ("Jobs");
    }

    root.createDirectory();

    auto folder =
        root.getChildFile (
            "job_" + timestampForFilename());

    folder.createDirectory();
    return folder;
}

void StemLabAudioProcessor::setJobRootDirectory (
    const juce::File& directory)
{
    if (! directory.isDirectory())
        return;

    {
        const juce::ScopedLock lock (stateLock);
        jobRootDirectory = directory;
    }

    setStatus (
        "File location set: "
        + directory.getFullPathName());
}

juce::File StemLabAudioProcessor::getJobRootDirectory() const
{
    const juce::ScopedLock lock (stateLock);

    if (jobRootDirectory.isDirectory())
        return jobRootDirectory;

    return juce::File::getSpecialLocation (
               juce::File::userDocumentsDirectory)
        .getChildFile ("StemLab")
        .getChildFile ("Jobs");
}

bool StemLabAudioProcessor::startCapture()
{
    if (isStandaloneApp())
        return startStandaloneRecording();

    if (capturing.load()
        || captureArmed.load()
        || captureFinalizeRequested.load()
        || isEngineRunning())
    {
        return false;
    }

    if (currentSampleRate <= 0.0)
    {
        setStatus ("Audio device is not ready");
        return false;
    }

    const auto newCaptureFile =
        createCaptureFile();

    auto stream =
        std::make_unique<juce::FileOutputStream> (
            newCaptureFile);

    if (! stream->openedOk())
    {
        setStatus ("Could not create capture WAV");
        return false;
    }

    juce::WavAudioFormat wav;

    auto* rawWriter = wav.createWriterFor (
        stream.get(),
        currentSampleRate,
        static_cast<unsigned int> (
            juce::jlimit (
                1,
                2,
                currentInputChannels)),
        24,
        {},
        0);

    if (rawWriter == nullptr)
    {
        setStatus ("Could not create WAV writer");
        return false;
    }

    stream.release();

    threadedWriter =
        std::make_unique<
            juce::AudioFormatWriter::ThreadedWriter> (
                rawWriter,
                diskWriterThread,
                32768);

    {
        const juce::ScopedLock lock (stateLock);
        captureFile = newCaptureFile;
        lastJobDirectory = {};
        engineLog.clear();
    }

    capturedSamples.store (0);
    inputDurationSeconds.store (0.0);
    captureStartPpq.store (-1.0);
    captureStartTimelineSample.store (-1);
    hostWasPlayingDuringCapture.store (false);
    captureFinalizeRequested.store (false);
    captureArmed.store (true);
    capturing.store (false);
    engineCompletedSuccessfully.store (false);
    engineProgress.store (0.0);

    {
        const juce::ScopedLock lock (abletonBridgeLock);
        abletonBridgeStatus =
            "StemLabRemote status will appear after separation";
    }

    abletonImportedStemCount.store (0);

    setStatus ("Armed - press Play in Ableton");
    return true;
}

void StemLabAudioProcessor::stopCapture()
{
    if (isStandaloneApp())
    {
        const auto mode =
            standaloneRecordingMode.load();

        if (mode == recordingSystem)
            stopSystemAudioRecording();
        else if (mode == recordingInput)
            stopStandaloneRecording();

        return;
    }

    const bool wasArmed =
        captureArmed.exchange (false);

    const bool wasCapturing =
        capturing.exchange (false);

    activeWriter.store (
        nullptr,
        std::memory_order_release);

    captureFinalizeRequested.store (false);
    cancelPendingUpdate();

    threadedWriter.reset();
    hostWasPlayingDuringCapture.store (false);

    if (wasArmed
        && ! wasCapturing
        && capturedSamples.load() == 0)
    {
        const auto emptyCapture =
            getCaptureFile();

        if (emptyCapture.existsAsFile())
            emptyCapture.deleteFile();

        {
            const juce::ScopedLock lock (stateLock);
            captureFile = {};
        }

        setStatus ("Capture cancelled");
        return;
    }

    if (capturedSamples.load() > 0)
    {
        if (captureStartPpq.load() < 0.0)
            captureStartPpq.store (0.0);

        setStatus ("Capture ready - choose stems");
    }
    else
    {
        setStatus ("Capture stopped");
    }
}

double StemLabAudioProcessor::getCapturedSeconds() const noexcept
{
    if (! capturing.load())
    {
        const auto duration =
            inputDurationSeconds.load();

        if (duration > 0.0)
            return duration;
    }

    if (currentSampleRate <= 0.0)
        return 0.0;

    return static_cast<double> (
               capturedSamples.load())
        / currentSampleRate;
}

juce::File StemLabAudioProcessor::getCaptureFile() const
{
    const juce::ScopedLock lock (stateLock);
    return captureFile;
}

juce::File StemLabAudioProcessor::getAbletonClipReplyFile() const
{
    const juce::ScopedLock lock (stateLock);
    return abletonClipReplyFile;
}

bool StemLabAudioProcessor::sendAbletonControlMessage (
    const juce::String& message)
{
    juce::DatagramSocket socket (false);

    const auto utf8 =
        message.toRawUTF8();

    const auto length =
        static_cast<int> (
            std::strlen (utf8));

    return socket.write (
        "127.0.0.1",
        39277,
        utf8,
        length) == length;
}

bool StemLabAudioProcessor::requestAbletonSourceClip()
{
    if (isStandaloneApp()
        || capturing.load()
        || isEngineRunning())
    {
        return false;
    }

    stopStandalonePlayback();

    const auto requestId =
        juce::Uuid().toString();

    auto replyFolder =
        juce::File::getSpecialLocation (
            juce::File::tempDirectory)
            .getChildFile ("StemLab")
            .getChildFile ("Ableton");

    replyFolder.createDirectory();

    const auto replyFile =
        replyFolder.getChildFile (
            "clip_" + requestId + ".json");

    auto legacyFolder =
        juce::File::getSpecialLocation (
            juce::File::userDocumentsDirectory)
            .getChildFile ("StemLab")
            .getChildFile ("Ableton");

    legacyFolder.createDirectory();

    const auto legacyReply =
        legacyFolder.getChildFile (
            "stemlab_clip_reply.json");

    if (replyFile.existsAsFile())
        replyFile.deleteFile();

    if (legacyReply.existsAsFile())
        legacyReply.deleteFile();

    {
        const juce::ScopedLock lock (stateLock);
        abletonClipRequestId = requestId;
        abletonClipReplyFile = replyFile;
        abletonLegacyClipReplyFile = legacyReply;
    }

    abletonClipRequestPending.store (true);
    abletonClipRequestStartMs.store (nowMs());

    // 0.9.4+ protocol: tell StemLabRemote exactly where to write the one-shot
    // reply. This avoids Documents/OneDrive latency.
    const auto modernPayload =
        "stemlab_get_clip "
        + requestId
        + " "
        + utf8ToHex (
            replyFile.getFullPathName());

    const bool modernSent =
        sendAbletonControlMessage (
            modernPayload);

    // Compatibility fallback: 0.9.3 and earlier StemLabRemote versions only
    // understand the request-id form and write to
    // Documents/StemLab/Ableton/stemlab_clip_reply.json.
    //
    // Sending both is intentional. New Remote Scripts understand both forms;
    // old scripts will mishandle the first message but immediately receive the
    // second compatible one. The VST accepts whichever valid reply arrives.
    const bool legacySent =
        sendAbletonControlMessage (
            "stemlab_get_clip " + requestId);

    if (! modernSent && ! legacySent)
    {
        abletonClipRequestPending.store (false);
        abletonClipRequestStartMs.store (0.0);
        setStatus ("Could not contact StemLabRemote");
        return false;
    }

    setStatus ("Getting selected Live clip...");
    return true;
}

void StemLabAudioProcessor::refreshAbletonSourceClipFromDisk()
{
    if (isStandaloneApp()
        || ! abletonClipRequestPending.load())
    {
        return;
    }

    juce::File modernReply;
    juce::File legacyReply;
    juce::String requestId;

    {
        const juce::ScopedLock lock (stateLock);
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

    if (! reply.existsAsFile())
    {
        const auto started =
            abletonClipRequestStartMs.load();

        if (started > 0.0
            && nowMs() - started > 5000.0)
        {
            abletonClipRequestPending.store (false);
            abletonClipRequestStartMs.store (0.0);

            setStatus (
                "StemLabRemote did not return the clip. Re-select StemLabRemote in Live Settings, select the Arrangement audio clip, and try again.");
        }

        return;
    }

    const auto parsed =
        juce::JSON::parse (
            reply.loadFileAsString());

    auto* object =
        parsed.getDynamicObject();

    if (object == nullptr
        || object->getProperty (
            "protocol").toString()
            != "stemlab-clip-source")
    {
        // A partially-written/older reply should not permanently poison the
        // request. Leave the request pending and let the next timer tick retry.
        return;
    }

    if (object->getProperty (
            "request_id").toString()
        != requestId)
    {
        // With an old Remote Script, the first modern-format request can be
        // interpreted as one long request id. The second compatibility request
        // overwrites this legacy file with the correct id shortly afterward.
        return;
    }

    abletonClipRequestPending.store (false);
    abletonClipRequestStartMs.store (0.0);

    const bool success =
        static_cast<bool> (
            object->getProperty (
                "success"));

    const auto message =
        object->getProperty (
            "message").toString();

    const auto path =
        object->getProperty (
            "path").toString();

    const auto startBeat =
        static_cast<double> (
            object->getProperty (
                "start_beat"));

    const auto trackName =
        object->getProperty (
            "source_track").toString();

    const auto clipName =
        object->getProperty (
            "clip_name").toString();

    // Clean both possible reply locations regardless of which one won.
    if (modernReply.existsAsFile())
        modernReply.deleteFile();

    if (legacyReply.existsAsFile())
        legacyReply.deleteFile();

    if (! success)
    {
        setStatus (
            message.isNotEmpty()
                ? "Live clip: " + message
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
        label = juce::File (path).getFileName();

    if (setInputAudioFile (
            juce::File (path),
            startBeat,
            label))
    {
        setStatus (
            "Live clip ready");

        {
            const juce::ScopedLock lock (
                abletonBridgeLock);

            abletonBridgeStatus =
                "Source ready";
        }
    }
}

bool StemLabAudioProcessor::launchSeparationAndExport()
{
    stopStandalonePlayback();

    if (capturing.load()
        || captureArmed.load()
        || captureFinalizeRequested.load())
    {
        setStatus ("Finish the capture before separating");
        return false;
    }

    if (isEngineRunning())
        return false;

    const auto source = getCaptureFile();

    if (! source.existsAsFile())
    {
        setStatus (
            isStandaloneApp()
                ? "Select or record audio first"
                : "Use Live Clip or Record PC first");
        return false;
    }

    const auto commandName = getEngineCommand().trim();

    if (commandName.isEmpty())
    {
        setStatus ("Choose the StemLab engine in Settings");
        return false;
    }

    juce::StringArray command;
    command.add (commandName);

    // Portable releases ship a relocatable embedded Python runtime under
    // Engine\python.exe rather than requiring a system Python/venv. When
    // auto-discovery resolves that interpreter, launch StemLab's worker as a
    // module. The old stemlab-plugin-job.exe development path still works.
    {
        const juce::File commandFile (commandName);
        const auto fileName = commandFile.getFileName();

        if (fileName.equalsIgnoreCase ("python.exe")
            || fileName.equalsIgnoreCase ("pythonw.exe")
            || fileName.equalsIgnoreCase ("python"))
        {
            command.add ("-m");
            command.add ("stemlab.plugin_job");
        }
    }

    command.add ("--input");
    command.add (source.getFullPathName());

    clearRecursiveResults();

    const auto job = createJobDirectory();

    {
        const juce::ScopedLock lock (stateLock);
        lastJobDirectory = job;
        engineLog.clear();
    }

    if (! isStandaloneApp())
    {
        const auto ack =
            job.getChildFile ("stemlab_ableton_ack.json");

        if (ack.existsAsFile())
            ack.deleteFile();

        {
            const juce::ScopedLock lock (abletonBridgeLock);
            abletonBridgeStatus =
                "Separating all six stems...";
        }

        abletonImportedStemCount.store (0);
        abletonBridgeWaitStartMs.store (0.0);
    }

    command.add ("--output");
    command.add (job.getFullPathName());

    command.add ("--start-ppq");
    command.add (juce::String (juce::jmax (0.0, captureStartPpq.load()), 8));

    command.add ("--device");
    command.add ("cuda");

    command.add ("--engine");
    command.add (getSeparatorEngineId());

    if (! refinementEnabled.load())
        command.add ("--no-refine");

    // Separation always produces every stem first. Ableton selection is
    // intentionally deferred until after the user can audition the results.
    command.add ("--no-notify");
    command.add ("--stems");

    for (int i = 0; i < stemCount; ++i)
        command.add (getStemName (i));

    setStatus (
        "Separating with "
        + getSeparatorEngineDisplayName()
        + "...");

    engineCompletedSuccessfully.store (false);
    engineProgress.store (0.01);
    engineStartMs.store (nowMs());
    lastEngineDurationSeconds.store (0.0);

    engineThread = std::make_unique<StemLabEngineThread> (
        *this,
        command,
        job);

    engineThread->startThread();
    return true;
}

juce::StringArray StemLabAudioProcessor::makePythonModuleCommand (
    const juce::String& moduleName) const
{
   #ifdef STEMLAB_DEV_REPO_ROOT
    // Development builds may run recursive jobs from a separate environment
    // while the packaged app runs both jobs from the bundled StemLab Engine.
    if (moduleName == "stemlab.recursive_job")
    {
        const auto devRoot = juce::File (STEMLAB_DEV_REPO_ROOT);
        const juce::StringArray recursiveDevCandidates
        {
            ".substem-venv/Scripts/stemlab-recursive-job.exe",
            ".substem-venv/Scripts/stemlab-recursive-job",
            ".venv/Scripts/stemlab-recursive-job.exe",
            ".venv/Scripts/stemlab-recursive-job"
        };

        for (const auto& relative : recursiveDevCandidates)
        {
            const auto candidate = devRoot.getChildFile (relative);
            if (candidate.existsAsFile())
                return { candidate.getFullPathName() };
        }
    }
   #endif

    const auto commandName = getEngineCommand().trim();

    if (commandName.isEmpty())
        return {};

    const juce::File commandFile (commandName);
    const auto fileName = commandFile.getFileName();

    juce::StringArray command;

    if (fileName.equalsIgnoreCase ("python.exe")
        || fileName.equalsIgnoreCase ("pythonw.exe")
        || fileName.equalsIgnoreCase ("python"))
    {
        command.add (commandName);
        command.add ("-m");
        command.add (moduleName);
        return command;
    }

    if (fileName.containsIgnoreCase ("stemlab-plugin-job"))
    {
        auto recursiveExecutable =
            commandFile.getSiblingFile (
                fileName.replace ("stemlab-plugin-job", "stemlab-recursive-job"));

        if (recursiveExecutable.existsAsFile())
        {
            command.add (recursiveExecutable.getFullPathName());
            return command;
        }
    }

    if (commandName.equalsIgnoreCase ("stemlab-plugin-job"))
    {
        command.add ("stemlab-recursive-job");
        return command;
    }

    return {};
}

void StemLabAudioProcessor::clearRecursiveResults()
{
    {
        const juce::ScopedLock lock (recursiveLock);
        recursiveItems.clear();
        previewRecursiveId.clear();
    }

    // Recursive children replace their parent in the default selection for a
    // completed job. Clearing the recursive tree restores the normal six
    // top-level stems for the next source/separation.
    for (auto& value : stemEnabled)
        value.store (true);

    sendChangeMessage();
}

std::vector<StemLabRecursiveStemInfo>
StemLabAudioProcessor::getRecursiveStemItems() const
{
    std::vector<StemLabRecursiveStemInfo> snapshot;

    {
        const juce::ScopedLock lock (recursiveLock);
        snapshot = recursiveItems;
    }

    std::vector<StemLabRecursiveStemInfo> ordered;
    ordered.reserve (snapshot.size());

    std::function<void (const juce::String&, int)> appendChildren;
    appendChildren = [&] (const juce::String& parentId, int depth)
    {
        for (const auto& item : snapshot)
        {
            if (item.parentId != parentId)
                continue;

            auto copy = item;
            copy.depth = depth;
            ordered.push_back (copy);
            appendChildren (item.id, depth + 1);
        }
    };

    for (int i = 0; i < stemCount; ++i)
        appendChildren (getStemName (i), 1);

    // Keep any future/experimental node visible even if a malformed parent
    // relationship somehow slips into a manifest.
    for (const auto& item : snapshot)
    {
        const auto alreadyPresent = std::any_of (
            ordered.begin(),
            ordered.end(),
            [&item] (const auto& existing)
            {
                return existing.id == item.id;
            });

        if (! alreadyPresent)
            ordered.push_back (item);
    }

    for (auto& item : ordered)
    {
        const auto prefix = item.id + "/";
        item.hasChildren = std::any_of (
            snapshot.begin(),
            snapshot.end(),
            [&prefix] (const auto& candidate)
            {
                return candidate.id.startsWith (prefix);
            });
    }

    return ordered;
}

juce::File StemLabAudioProcessor::getRecursiveStemFile (
    const juce::String& itemId) const
{
    const juce::ScopedLock lock (recursiveLock);

    for (const auto& item : recursiveItems)
        if (item.id == itemId)
            return item.file;

    return {};
}

void StemLabAudioProcessor::setRecursiveStemEnabled (
    const juce::String& itemId,
    bool enabled)
{
    {
        const juce::ScopedLock lock (recursiveLock);
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

bool StemLabAudioProcessor::isRecursiveStemEnabled (
    const juce::String& itemId) const
{
    const juce::ScopedLock lock (recursiveLock);

    for (const auto& item : recursiveItems)
        if (item.id == itemId)
            return item.selected;

    return false;
}

juce::String StemLabAudioProcessor::getPreviewRecursiveId() const
{
    const juce::ScopedLock lock (recursiveLock);
    return previewRecursiveId;
}

bool StemLabAudioProcessor::playRecursiveStem (
    const juce::String& itemId)
{
    if (capturing.load() || isEngineRunning() || ! hasSuccessfulJob())
        return false;

    const auto stemFile = getRecursiveStemFile (itemId);

    if (! stemFile.existsAsFile())
    {
        setStatus ("Recursive stem preview file was not found");
        return false;
    }

    const auto currentId = getPreviewRecursiveId();

    if (currentId == itemId && previewTransport.isPlaying())
    {
        previewTransport.stop();
        setStatus ("Recursive stem paused");
        return true;
    }

    if (currentId != itemId)
    {
        if (! loadPreviewFile (stemFile, -3))
        {
            setStatus ("Could not load recursive stem preview");
            return false;
        }

        const juce::ScopedLock lock (recursiveLock);
        previewRecursiveId = itemId;
    }

    if (previewTransport.getCurrentPosition()
        >= previewTransport.getLengthInSeconds() - 0.01)
    {
        previewTransport.setPosition (0.0);
    }

    previewTransport.start();

    for (const auto& item : getRecursiveStemItems())
    {
        if (item.id == itemId)
        {
            setStatus ("Playing " + item.label);
            break;
        }
    }

    return true;
}

bool StemLabAudioProcessor::seekRecursiveStem (
    const juce::String& itemId,
    double normalisedPosition)
{
    if (capturing.load() || isEngineRunning() || ! hasSuccessfulJob())
        return false;

    const auto stemFile = getRecursiveStemFile (itemId);
    if (! stemFile.existsAsFile())
        return false;

    const auto currentId = getPreviewRecursiveId();
    const bool keepPlaying =
        currentId == itemId && previewTransport.isPlaying();

    if (currentId != itemId)
    {
        if (! loadPreviewFile (stemFile, -3))
            return false;

        const juce::ScopedLock lock (recursiveLock);
        previewRecursiveId = itemId;
    }

    const auto length = previewTransport.getLengthInSeconds();
    if (length <= 0.0)
        return false;

    previewTransport.setPosition (
        juce::jlimit (0.0, 1.0, normalisedPosition) * length);

    if (keepPlaying)
        previewTransport.start();

    sendChangeMessage();
    return true;
}

void StemLabAudioProcessor::finishRecursiveJob (
    const juce::File& manifestFile)
{
    // The Python side owns separation details. The plugin only consumes the
    // schema-2 tree contract and turns child nodes into selectable UI rows.
    const auto parsed = juce::JSON::parse (manifestFile.loadFileAsString());
    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
    {
        setStatus ("Recursive result manifest is invalid");
        return;
    }

    const auto parentId = object->getProperty ("parent_id").toString();
    const auto rootStem = object->getProperty ("root_stem").toString();
    auto* children = object->getProperty ("children").getArray();

    if (parentId.isEmpty() || rootStem.isEmpty() || children == nullptr)
    {
        setStatus ("Recursive result manifest is incomplete");
        return;
    }

    std::vector<StemLabRecursiveStemInfo> newItems;

    for (const auto& entry : *children)
    {
        auto* child = entry.getDynamicObject();
        if (child == nullptr)
            continue;

        StemLabRecursiveStemInfo item;
        item.id = child->getProperty ("id").toString();
        item.label = child->getProperty ("label").toString();
        item.parentId = parentId;
        item.rootStem = rootStem;
        item.category = child->getProperty ("category").toString();
        item.file = juce::File (child->getProperty ("path").toString());
        item.selected = ! item.label.containsIgnoreCase ("Removed Reverb");
        item.estimatedSourceCount = juce::jmax (
            1,
            static_cast<int> (child->getProperty ("estimated_source_count")));
        item.confidence = juce::jlimit (
            0.0,
            1.0,
            static_cast<double> (child->getProperty ("confidence")));
        item.complexity = juce::jlimit (
            0.0,
            1.0,
            static_cast<double> (child->getProperty ("complexity")));

        if (auto* actions = child->getProperty ("actions").getArray())
        {
            for (const auto& action : *actions)
                item.actions.addIfNotAlreadyThere (action.toString());
        }

        if (item.id.isNotEmpty() && item.file.existsAsFile())
            newItems.push_back (std::move (item));
    }

    if (newItems.empty())
    {
        setStatus ("Recursive split finished without usable audio files");
        return;
    }

    {
        const juce::ScopedLock lock (recursiveLock);
        const auto prefix = parentId + "/";

        recursiveItems.erase (
            std::remove_if (
                recursiveItems.begin(),
                recursiveItems.end(),
                [&] (const auto& item)
                {
                    return item.id.startsWith (prefix);
                }),
            recursiveItems.end());

        // Once a node is split further, default to its children rather than
        // sending/saving both the parent and every child at the same time.
        if (parentId != rootStem)
        {
            for (auto& item : recursiveItems)
                if (item.id == parentId)
                    item.selected = false;
        }

        recursiveItems.insert (
            recursiveItems.end(),
            newItems.begin(),
            newItems.end());
    }

    if (parentId == rootStem)
    {
        for (int i = 0; i < stemCount; ++i)
            if (getStemName (i).equalsIgnoreCase (rootStem))
                setStemEnabled (i, false);
    }

    sendChangeMessage();
}

bool StemLabAudioProcessor::launchRecursiveStemSplit (int rootStemIndex)
{
    if (! hasSuccessfulJob()
        || isEngineRunning()
        || ! juce::isPositiveAndBelow (rootStemIndex, stemCount))
    {
        return false;
    }

    const auto rootStem = getStemName (rootStemIndex);
    const bool isVocals = rootStem.equalsIgnoreCase ("vocals");
    const bool isDrums = rootStem.equalsIgnoreCase ("drums");
    const bool isLeadCandidate =
        rootStem.equalsIgnoreCase ("guitar")
        || rootStem.equalsIgnoreCase ("piano")
        || rootStem.equalsIgnoreCase ("other");

    if (! isVocals && ! isDrums && ! isLeadCandidate)
    {
        setStatus ("Adaptive splitting is not available for this stem yet");
        return false;
    }

    const auto source = getCompletedStemFile (rootStemIndex);
    if (! source.existsAsFile())
    {
        setStatus ("Stem file was not found for adaptive splitting");
        return false;
    }

    auto command = makePythonModuleCommand ("stemlab.recursive_job");
    if (command.isEmpty())
    {
        setStatus ("Adaptive stem engine could not be located");
        return false;
    }

    const auto output =
        getLastJobDirectory()
            .getChildFile ("recursive")
            .getChildFile (rootStem);

    if (output.isDirectory())
        output.deleteRecursively();
    output.createDirectory();

    const auto operation =
        isVocals ? juce::String ("vocals")
                 : (isDrums ? juce::String ("drums")
                            : juce::String ("lead"));

    const auto category =
        isVocals ? juce::String ("vocal.group")
                 : (isDrums ? juce::String ("drum.group")
                            : juce::String ("instrument.") + rootStem);

    command.add ("--operation");
    command.add (operation);
    command.add ("--input");
    command.add (source.getFullPathName());
    command.add ("--output");
    command.add (output.getFullPathName());
    command.add ("--parent-id");
    command.add (rootStem);
    command.add ("--root-stem");
    command.add (rootStem);
    command.add ("--category");
    command.add (category);
    command.add ("--depth");
    command.add ("1");

    recursiveThread.reset();
    engineProgress.store (0.01);
    engineStartMs.store (nowMs());
    lastEngineDurationSeconds.store (0.0);

    if (isVocals)
        setStatus ("Adaptive vocals: separating lead and backing groups...");
    else if (isDrums)
        setStatus ("Adaptive drums: splitting drum components...");
    else
        setStatus ("Adaptive lead: detecting foreground and backing layers...");

    recursiveThread = std::make_unique<StemLabRecursiveThread> (
        *this,
        command,
        output.getChildFile ("recursive_manifest.json"));

    recursiveThread->startThread();
    return true;
}

bool StemLabAudioProcessor::launchRecursiveAction (
    const juce::String& itemId,
    const juce::String& action)
{
    if (! hasSuccessfulJob() || isEngineRunning())
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

    if (! found || ! target.file.existsAsFile())
    {
        setStatus ("Adaptive source was not found");
        return false;
    }

    if (! target.actions.contains (action))
    {
        setStatus ("That adaptive action is not available for this stem");
        return false;
    }

    const bool isDeverb = action.equalsIgnoreCase ("deverb");
    const bool isAdaptiveSplit = action.equalsIgnoreCase ("split");

    if (! isDeverb && ! isAdaptiveSplit)
    {
        setStatus ("Adaptive action is not implemented yet");
        return false;
    }

    auto command = makePythonModuleCommand ("stemlab.recursive_job");
    if (command.isEmpty())
    {
        setStatus ("Adaptive stem engine could not be located");
        return false;
    }

    auto safeFolder = itemId.replace ("/", "_").replace ("\\", "_");
    const auto operation = isDeverb ? juce::String ("deverb")
                                    : juce::String ("adaptive");
    const auto output =
        getLastJobDirectory()
            .getChildFile ("recursive")
            .getChildFile ("actions")
            .getChildFile (safeFolder + "_" + operation);

    if (output.isDirectory())
        output.deleteRecursively();
    output.createDirectory();

    command.add ("--operation");
    command.add (operation);
    command.add ("--input");
    command.add (target.file.getFullPathName());
    command.add ("--output");
    command.add (output.getFullPathName());
    command.add ("--parent-id");
    command.add (target.id);
    command.add ("--root-stem");
    command.add (target.rootStem);
    command.add ("--category");
    command.add (target.category.isNotEmpty() ? target.category : "unknown");
    command.add ("--depth");
    command.add (juce::String (target.depth + 1));

    recursiveThread.reset();
    engineProgress.store (0.01);
    engineStartMs.store (nowMs());
    lastEngineDurationSeconds.store (0.0);

    setStatus (
        isDeverb
            ? "De-reverb: processing isolated lead vocal..."
            : "Adaptive split: analysing how many useful layers remain...");

    recursiveThread = std::make_unique<StemLabRecursiveThread> (
        *this,
        command,
        output.getChildFile ("recursive_manifest.json"));

    recursiveThread->startThread();
    return true;
}

bool StemLabAudioProcessor::isRecursiveEngineRunning() const noexcept
{
    return recursiveThread != nullptr && recursiveThread->isThreadRunning();
}

bool StemLabAudioProcessor::isEngineRunning() const noexcept
{
    return (engineThread != nullptr && engineThread->isThreadRunning())
        || isRecursiveEngineRunning();
}

double StemLabAudioProcessor::getEngineElapsedSeconds() const noexcept
{
    const auto start = engineStartMs.load();

    if (start <= 0.0)
        return 0.0;

    if (isEngineRunning())
        return juce::jmax (0.0, (nowMs() - start) / 1000.0);

    return lastEngineDurationSeconds.load();
}

double StemLabAudioProcessor::getEngineEstimatedRemainingSeconds() const noexcept
{
    if (! isEngineRunning())
        return 0.0;

    const auto progress = engineProgress.load();

    if (progress < 0.12 || progress >= 0.995)
        return -1.0;

    const auto elapsed = getEngineElapsedSeconds();

    if (elapsed <= 0.1)
        return -1.0;

    const auto estimate = elapsed * (1.0 - progress) / progress;
    return juce::jlimit (0.0, 60.0 * 60.0, estimate);
}

void StemLabAudioProcessor::refreshEngineProgressFromDisk()
{
    // Recursive jobs stream their progress directly through stdout. The main
    // job's progress file may still contain 100% from the six-stem pass, so
    // do not let that stale file overwrite recursive progress/status.
    if (isRecursiveEngineRunning())
        return;

    if (! isEngineRunning())
        return;

    const auto job = getLastJobDirectory();

    if (! job.isDirectory())
        return;

    const auto progressFile =
        job.getChildFile ("stemlab_progress.txt");

    if (! progressFile.existsAsFile())
        return;

    const auto text = progressFile.loadFileAsString().trim();
    const auto separator = text.indexOfChar ('|');

    if (separator <= 0)
        return;

    const auto percent =
        text.substring (0, separator).getDoubleValue();

    const auto stage =
        text.substring (separator + 1).trim();

    setEngineProgress (
        juce::jlimit (0.0, 1.0, percent / 100.0));

    if (stage.isNotEmpty())
    {
        const auto current = getStatus();

        if (current != stage)
            setStatus (stage);
    }
}

juce::String StemLabAudioProcessor::getStatus() const
{
    const juce::ScopedLock lock (stateLock);
    return status;
}

void StemLabAudioProcessor::postUiStatus (const juce::String& message)
{
    setStatus (message);
}

juce::String StemLabAudioProcessor::getEngineLog() const
{
    const juce::ScopedLock lock (stateLock);
    return engineLog;
}

juce::File StemLabAudioProcessor::getLastJobDirectory() const
{
    const juce::ScopedLock lock (stateLock);
    return lastJobDirectory;
}

void StemLabAudioProcessor::setStatus (const juce::String& newStatus)
{
    {
        const juce::ScopedLock lock (stateLock);
        status = newStatus;
    }

    sendChangeMessage();
}

void StemLabAudioProcessor::setEngineProgress (double progress)
{
    const auto current = engineProgress.load();

    engineProgress.store (
        juce::jmax (
            current,
            juce::jlimit (0.0, 1.0, progress)));

    sendChangeMessage();
}

void StemLabAudioProcessor::handleEngineOutputLine (const juce::String& line)
{
    appendEngineLog (line + "\n");

    if (line.startsWithIgnoreCase ("STEMLAB_ERROR "))
    {
        const auto message =
            line.fromFirstOccurrenceOf ("STEMLAB_ERROR ", false, false).trim();

        if (message.isNotEmpty())
            setStatus ("Failed - " + message);

        return;
    }

    if (line.startsWithIgnoreCase ("STEMLAB_PROGRESS "))
    {
        auto tokens = juce::StringArray::fromTokens (line, " ", "\"");

        if (tokens.size() >= 2)
        {
            const auto percent =
                juce::jlimit (0, 100, tokens[1].getIntValue());

            setEngineProgress (percent / 100.0);

            if (tokens.size() >= 3)
            {
                auto stage =
                    line.fromFirstOccurrenceOf (
                        tokens[1],
                        false,
                        false)
                        .trim();

                setStatus (stage);
            }
        }

        return;
    }

    const auto percentPos = line.indexOfChar ('%');

    if (percentPos > 0)
    {
        int start = percentPos - 1;

        while (start >= 0
               && juce::CharacterFunctions::isDigit (
                   static_cast<juce::juce_wchar> (line[start])))
        {
            --start;
        }

        const auto number =
            line.substring (start + 1, percentPos).getIntValue();

        if (number >= 0 && number <= 100)
            setEngineProgress (
                0.10 + 0.68 * (number / 100.0));
    }
}

void StemLabAudioProcessor::appendEngineLog (const juce::String& text)
{
    {
        const juce::ScopedLock lock (stateLock);
        engineLog += text;

        constexpr int maxLogCharacters = 50000;

        if (engineLog.length() > maxLogCharacters)
        {
            engineLog = engineLog.substring (
                engineLog.length() - maxLogCharacters);
        }
    }

    sendChangeMessage();
}

int StemLabAudioProcessor::saveSelectedStemsTo (
    const juce::File& destination)
{
    if (! isStandaloneApp() || ! hasSuccessfulJob())
        return 0;

    if (! destination.isDirectory())
    {
        if (! destination.createDirectory())
            return 0;
    }

    const auto baseName =
        getCaptureFile().getFileNameWithoutExtension();

    int saved = 0;

    for (int i = 0; i < stemCount; ++i)
    {
        if (! isStemEnabled (i))
            continue;

        const auto source = getCompletedStemFile (i);

        if (! source.existsAsFile())
            continue;

        const auto outputName =
            baseName
            + "_"
            + getStemName (i)
            + source.getFileExtension();

        auto target = destination.getChildFile (outputName);

        if (target.existsAsFile())
            target.deleteFile();

        if (source.copyFileTo (target))
            ++saved;
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (! item.selected || ! item.file.existsAsFile())
            continue;

        auto safeName = item.id.replace ("/", "_").replace ("\\", "_");
        const auto outputName =
            baseName
            + "_"
            + safeName
            + item.file.getFileExtension();

        auto target = destination.getChildFile (outputName);

        if (target.existsAsFile())
            target.deleteFile();

        if (item.file.copyFileTo (target))
            ++saved;
    }

    setStatus (
        "Saved "
        + juce::String (saved)
        + (saved == 1 ? " stem" : " stems"));

    return saved;
}

juce::String StemLabAudioProcessor::getAbletonBridgeStatus() const
{
    const juce::ScopedLock lock (abletonBridgeLock);
    return abletonBridgeStatus;
}

void StemLabAudioProcessor::refreshAbletonBridgeStatusFromDisk()
{
    if (isStandaloneApp())
        return;

    // The invisible Remote Script writes a small heartbeat/status file when
    // Live loads it. This lets the VST distinguish "integration installed"
    // from "no background script is active" without any visible Live device.
    const auto globalStatusFile =
        juce::File::getSpecialLocation (
            juce::File::userDocumentsDirectory)
            .getChildFile ("StemLab")
            .getChildFile ("Ableton")
            .getChildFile ("stemlab_remote_status.json");

    if (globalStatusFile.existsAsFile())
    {
        const auto remoteStatus =
            juce::JSON::parse (
                globalStatusFile.loadFileAsString());

        if (auto* statusObject =
                remoteStatus.getDynamicObject())
        {
            if (statusObject->getProperty ("protocol").toString()
                == "stemlab-remote-status")
            {
                const bool active =
                    static_cast<bool> (
                        statusObject->getProperty ("active"));

                const double timestamp =
                    static_cast<double> (
                        statusObject->getProperty ("timestamp"));

                const auto nowUnix =
                    juce::Time::getCurrentTime().toMilliseconds()
                    / 1000.0;

                // The init heartbeat persists on disk. Treat it as a useful
                // "installed/loaded recently" indication but never override a
                // job-specific wait/import status once a separation exists.
                if (active
                    && nowUnix - timestamp < 24.0 * 60.0 * 60.0
                    && ! hasSuccessfulJob())
                {
                    const juce::ScopedLock lock (
                        abletonBridgeLock);

                    abletonBridgeStatus =
                        "StemLabRemote active - background Ableton integration ready";
                }
            }
        }
    }

    const auto job =
        getLastJobDirectory();

    if (! job.isDirectory())
        return;

    const auto importProgress =
        job.getChildFile (
            "stemlab_ableton_import_progress.json");

    if (importProgress.existsAsFile()
        && abletonBridgeWaitStartMs.load() > 0.0)
    {
        const auto progressParsed =
            juce::JSON::parse (
                importProgress.loadFileAsString());

        if (auto* progressObject =
                progressParsed.getDynamicObject())
        {
            if (progressObject->getProperty (
                    "protocol").toString()
                == "stemlab-ableton-import-progress")
            {
                const auto progressMessage =
                    progressObject->getProperty (
                        "message").toString();

                const int imported =
                    static_cast<int> (
                        progressObject->getProperty (
                            "imported"));

                const int total =
                    static_cast<int> (
                        progressObject->getProperty (
                            "total"));

                if (progressMessage.isNotEmpty())
                {
                    juce::String visible =
                        progressMessage;

                    if (total > 0)
                    {
                        visible +=
                            " ("
                            + juce::String (
                                juce::jmin (
                                    imported + 1,
                                    total))
                            + "/"
                            + juce::String (total)
                            + ")";
                    }

                    setStatus (visible);
                }
            }
        }
    }

    const auto ack =
        job.getChildFile (
            "stemlab_ableton_ack.json");

    if (ack.existsAsFile())
    {
        const auto parsed =
            juce::JSON::parse (
                ack.loadFileAsString());

        if (auto* object =
                parsed.getDynamicObject())
        {
            const auto protocol =
                object->getProperty (
                    "protocol").toString();

            if (protocol == "stemlab-ableton-ack")
            {
                const bool success =
                    static_cast<bool> (
                        object->getProperty (
                            "success"));

                const int imported =
                    static_cast<int> (
                        object->getProperty (
                            "imported"));

                const auto message =
                    object->getProperty (
                        "message").toString();

                abletonImportedStemCount.store (
                    juce::jmax (0, imported));

                {
                    const juce::ScopedLock lock (
                        abletonBridgeLock);

                    abletonBridgeStatus =
                        success
                            ? "Ableton imported "
                                + juce::String (imported)
                                + (imported == 1
                                    ? " stem"
                                    : " stems")
                            : "Ableton import failed"
                                + (message.isNotEmpty()
                                    ? ": " + message
                                    : juce::String {});
                }

                setStatus (
                    success
                        ? "Imported into Ableton"
                        : "Ableton import failed - select source track if needed, then Retry Import");

                abletonBridgeWaitStartMs.store (0.0);
                return;
            }
        }
    }

    const auto waitStart =
        abletonBridgeWaitStartMs.load();

    if (waitStart > 0.0
        && nowMs() - waitStart > 12000.0)
    {
        {
            const juce::ScopedLock lock (
                abletonBridgeLock);

            abletonBridgeStatus =
                "Ableton import timed out - click Retry";
        }

        setStatus (
            "Ableton import timed out - click Retry");

        abletonBridgeWaitStartMs.store (0.0);
    }
}

bool StemLabAudioProcessor::sendAbletonBridgeNotification (
    const juce::File& manifestFile)
{
    if (! manifestFile.existsAsFile())
        return false;

    const auto payload =
        "stemlab_ready "
        + utf8ToHex (
            manifestFile.getFullPathName());

    juce::DatagramSocket socket (false);

    const auto bytes =
        payload.toRawUTF8();

    const auto written =
        socket.write (
            "127.0.0.1",
            39277,
            bytes,
            static_cast<int> (
                std::strlen (bytes)));

    return written > 0;
}

bool StemLabAudioProcessor::sendSelectedStemsToAbleton()
{
    if (isStandaloneApp()
        || isEngineRunning()
        || ! hasSuccessfulJob())
    {
        return false;
    }

    const auto job =
        getLastJobDirectory();

    const auto masterManifest =
        job.getChildFile (
            "stemlab_ableton_manifest.json");

    if (! masterManifest.existsAsFile())
    {
        setStatus (
            "Completed stem manifest was not found");
        return false;
    }

    auto manifest =
        juce::JSON::parse (
            masterManifest.loadFileAsString());

    auto* object =
        manifest.getDynamicObject();

    if (object == nullptr)
    {
        setStatus (
            "Completed stem manifest is invalid");
        return false;
    }

    auto* allStems =
        object->getProperty (
            "stems").getArray();

    if (allStems == nullptr)
    {
        setStatus (
            "Completed stem list is invalid");
        return false;
    }

    juce::Array<juce::var> selected;

    for (const auto& entry : *allStems)
    {
        auto* stemObject =
            entry.getDynamicObject();

        if (stemObject == nullptr)
            continue;

        const auto name =
            stemObject->getProperty (
                "name").toString();

        for (int i = 0;
             i < stemCount;
             ++i)
        {
            if (name.equalsIgnoreCase (
                    getStemName (i))
                && isStemEnabled (i))
            {
                selected.add (entry);
                break;
            }
        }
    }

    for (const auto& item : getRecursiveStemItems())
    {
        if (! item.selected || ! item.file.existsAsFile())
            continue;

        auto* recursiveObject = new juce::DynamicObject();
        recursiveObject->setProperty (
            "name",
            item.label);
        recursiveObject->setProperty (
            "label",
            "StemLab - " + item.label);
        recursiveObject->setProperty (
            "path",
            item.file.getFullPathName().replace ("\\", "/"));
        recursiveObject->setProperty ("recursive", true);
        recursiveObject->setProperty ("root_stem", item.rootStem);
        selected.add (juce::var (recursiveObject));
    }

    if (selected.isEmpty())
    {
        setStatus (
            "Choose at least one stem to send");
        return false;
    }

    object->setProperty (
        "stems",
        juce::var (selected));

    object->setProperty (
        "selection_mode",
        "post-audition");

    const auto selectedManifest =
        job.getChildFile (
            "stemlab_ableton_selected_manifest.json");

    if (! selectedManifest.replaceWithText (
            juce::JSON::toString (
                manifest,
                true)))
    {
        setStatus (
            "Could not write selected-stem manifest");
        return false;
    }

    const auto ack =
        job.getChildFile (
            "stemlab_ableton_ack.json");

    if (ack.existsAsFile())
        ack.deleteFile();

    const auto importProgress =
        job.getChildFile (
            "stemlab_ableton_import_progress.json");

    if (importProgress.existsAsFile())
        importProgress.deleteFile();

    if (! sendAbletonBridgeNotification (
            selectedManifest))
    {
        setStatus (
            "Could not contact StemLabRemote");
        return false;
    }

    {
        const juce::ScopedLock lock (
            abletonBridgeLock);

        abletonBridgeStatus =
            "Sent "
            + juce::String (selected.size())
            + (selected.size() == 1
                ? " stem to Ableton"
                : " stems to Ableton")
            + " - waiting for import confirmation";
    }

    abletonImportedStemCount.store (0);
    abletonBridgeWaitStartMs.store (nowMs());

    setStatus (
        "Sending selected stems to Ableton...");
    return true;
}

bool StemLabAudioProcessor::retryAbletonImport()
{
    if (isStandaloneApp()
        || isEngineRunning())
    {
        return false;
    }

    const auto job =
        getLastJobDirectory();

    auto manifest =
        job.getChildFile (
            "stemlab_ableton_selected_manifest.json");

    if (! manifest.existsAsFile())
    {
        manifest =
            job.getChildFile (
                "stemlab_ableton_manifest.json");
    }

    if (! manifest.existsAsFile())
    {
        setStatus ("No completed Ableton manifest to import");
        return false;
    }

    const auto ack =
        job.getChildFile (
            "stemlab_ableton_ack.json");

    if (ack.existsAsFile())
        ack.deleteFile();

    const auto importProgress =
        job.getChildFile (
            "stemlab_ableton_import_progress.json");

    if (importProgress.existsAsFile())
        importProgress.deleteFile();

    {
        const juce::ScopedLock lock (
            abletonBridgeLock);

        abletonBridgeStatus =
            "Retry sent - waiting for StemLabRemote...";
    }

    abletonImportedStemCount.store (0);
    abletonBridgeWaitStartMs.store (nowMs());

    if (! sendAbletonBridgeNotification (manifest))
    {
        setStatus ("Could not send Retry Import message");
        return false;
    }

    setStatus ("Retry Import sent to Ableton");
    return true;
}

juce::String StemLabAudioProcessor::discoverEngineCommand() const
{
    const auto env = juce::SystemStats::getEnvironmentVariable (
        "STEMLAB_ENGINE",
        {});

    if (env.isNotEmpty())
    {
        const juce::File envFile (env);

        if (envFile.existsAsFile())
            return envFile.getFullPathName();
    }

    auto checkRoot = [] (juce::File root) -> juce::String
    {
        const juce::StringArray relativeCandidates
        {
            // Portable release: keep the whole runtime beside StemLab.exe or
            // beside a VST3 folder that Ableton scans directly.
            "Engine/python.exe",
            "engine/python.exe",

            // Development fallbacks.
            ".venv/Scripts/stemlab-plugin-job.exe",
            ".venv/Scripts/stemlab-plugin-job",
            "venv/Scripts/stemlab-plugin-job.exe",
            "venv/Scripts/stemlab-plugin-job"
        };

        for (int depth = 0; depth < 10 && root.exists(); ++depth)
        {
            for (const auto& relative : relativeCandidates)
            {
                const auto candidate = root.getChildFile (relative);

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
    if (const auto found = checkRoot (
            juce::File::getSpecialLocation (
                juce::File::currentExecutableFile)
                .getParentDirectory());
        found.isNotEmpty())
    {
        return found;
    }

    // The Standalone portable app writes this pointer on launch. Ableton's
    // VST3 can then reuse the Engine directory from the extracted release
    // instead of requiring a second multi-gigabyte copy.
    const auto localAppData = juce::SystemStats::getEnvironmentVariable (
        "LOCALAPPDATA",
        {});

    if (localAppData.isNotEmpty())
    {
        const auto stemLabLocal =
            juce::File (localAppData)
                .getChildFile ("StemLab");

        const auto portablePointer =
            stemLabLocal.getChildFile ("portable_engine_path.txt");

        if (portablePointer.existsAsFile())
        {
            const auto portablePath =
                portablePointer.loadFileAsString().trim();
            const juce::File portableRuntime (portablePath);

            if (portableRuntime.existsAsFile())
                return portableRuntime.getFullPathName();
        }

        // Backward-compatible fallback for older installer builds that copied
        // the runtime under LocalAppData\StemLab\Engine.
        const auto installedRuntime =
            stemLabLocal
                .getChildFile ("Engine")
                .getChildFile ("python.exe");

        if (installedRuntime.existsAsFile())
            return installedRuntime.getFullPathName();
    }

   #ifdef STEMLAB_DEV_REPO_ROOT
    {
        const auto devRoot =
            juce::File (STEMLAB_DEV_REPO_ROOT);

        if (const auto found =
                checkRoot (devRoot);
            found.isNotEmpty())
        {
            return found;
        }
    }
   #endif

    if (const auto found = checkRoot (
            juce::File::getCurrentWorkingDirectory());
        found.isNotEmpty())
    {
        return found;
    }

    return "stemlab-plugin-job";
}

void StemLabAudioProcessor::setEngineCommand (const juce::String& command)
{
    const juce::ScopedLock lock (stateLock);
    engineCommand = command;
}

juce::String StemLabAudioProcessor::getEngineCommand() const
{
    const juce::ScopedLock lock (stateLock);
    return engineCommand;
}

void StemLabAudioProcessor::resetEngineCommandToAutoDiscover()
{
    setEngineCommand (discoverEngineCommand());
    setStatus ("Engine path auto-detected");
}

void StemLabAudioProcessor::setStemEnabled (int index, bool enabled)
{
    if (juce::isPositiveAndBelow (index, stemCount))
        stemEnabled[static_cast<size_t> (index)].store (enabled);
}

bool StemLabAudioProcessor::isStemEnabled (int index) const
{
    if (! juce::isPositiveAndBelow (index, stemCount))
        return false;

    return stemEnabled[static_cast<size_t> (index)].load();
}

void StemLabAudioProcessor::setWaveformColourIndex (int index)
{
    waveformColourIndex.store (
        juce::jlimit (
            0,
            waveformColourCount - 1,
            index));

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

juce::String StemLabAudioProcessor::getStemName (int index)
{
    static constexpr const char* names[stemCount] =
    {
        "vocals",
        "drums",
        "bass",
        "guitar",
        "piano",
        "other"
    };

    if (! juce::isPositiveAndBelow (index, stemCount))
        return {};

    return names[index];
}

void StemLabAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto rootObject = std::make_unique<juce::DynamicObject>();
    rootObject->setProperty ("engineCommand", getEngineCommand());
    rootObject->setProperty ("refinement", refinementEnabled.load());
    rootObject->setProperty (
        "separatorEngine",
        separatorEngineIndex.load());
    rootObject->setProperty (
        "waveformColour",
        waveformColourIndex.load());

    rootObject->setProperty (
        "jobRootDirectory",
        getJobRootDirectory().getFullPathName());

    juce::Array<juce::var> stems;

    for (int i = 0; i < stemCount; ++i)
        stems.add (isStemEnabled (i));

    rootObject->setProperty ("stems", stems);

    const auto json = juce::JSON::toString (
        juce::var (rootObject.release()),
        false);

    destData.replaceAll (
        json.toRawUTF8(),
        static_cast<size_t> (json.getNumBytesAsUTF8()));
}

void StemLabAudioProcessor::setStateInformation (
    const void* data,
    int sizeInBytes)
{
    const juce::String json (
        juce::String::fromUTF8 (
            static_cast<const char*> (data),
            sizeInBytes));

    const auto parsed = juce::JSON::parse (json);
    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
        return;

    if (object->hasProperty ("engineCommand"))
    {
        const auto savedEngine =
            object->getProperty ("engineCommand").toString().trim();

        const auto discoveredEngine = discoverEngineCommand();

        const bool savedIsGeneric =
            savedEngine.isEmpty()
            || savedEngine == "stemlab-plugin-job";

        const juce::File savedFile (savedEngine);

        const bool savedLooksLikePath =
            savedEngine.containsChar ('\\')
            || savedEngine.containsChar ('/');

        const bool savedPathIsStale =
            savedLooksLikePath
            && ! savedFile.existsAsFile();

        const juce::File discoveredFile (discoveredEngine);
        const bool discoveredIsPortableRuntime =
            discoveredFile.getFileName().equalsIgnoreCase ("python.exe")
            && discoveredFile.getParentDirectory()
                .getFileName().equalsIgnoreCase ("Engine");

        // A self-contained release must not silently fall back to a saved
        // development venv merely because that venv still exists on the build
        // machine. Prefer the discovered sibling/installed Engine runtime.
        if ((discoveredIsPortableRuntime
                || savedIsGeneric
                || savedPathIsStale)
            && discoveredEngine.isNotEmpty()
            && discoveredEngine != "stemlab-plugin-job")
        {
            setEngineCommand (discoveredEngine);
        }
        else
        {
            setEngineCommand (savedEngine);
        }
    }
    else
    {
        resetEngineCommandToAutoDiscover();
    }

    if (object->hasProperty ("refinement"))
    {
        refinementEnabled.store (
            static_cast<bool> (
                object->getProperty ("refinement")));
    }

    if (object->hasProperty ("separatorEngine"))
    {
        setSeparatorEngineIndex (
            static_cast<int> (
                object->getProperty ("separatorEngine")));
    }

    if (object->hasProperty ("waveformColour"))
    {
        setWaveformColourIndex (
            static_cast<int> (
                object->getProperty ("waveformColour")));
    }

    if (object->hasProperty ("jobRootDirectory"))
    {
        const juce::File savedJobRoot (
            object->getProperty (
                "jobRootDirectory").toString());

        if (savedJobRoot.isDirectory())
            setJobRootDirectory (savedJobRoot);
    }

    const auto stems = object->getProperty ("stems");

    if (auto* array = stems.getArray())
    {
        for (int i = 0;
             i < juce::jmin (stemCount, array->size());
             ++i)
        {
            setStemEnabled (
                i,
                static_cast<bool> (
                    array->getUnchecked (i)));
        }
    }
}

juce::AudioProcessorEditor* StemLabAudioProcessor::createEditor()
{
    return new StemLabAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StemLabAudioProcessor();
}
