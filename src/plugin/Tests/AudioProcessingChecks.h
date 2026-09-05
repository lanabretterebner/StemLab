#pragma once

#include "PluginProcessor.h"

#include <cstdio>
#include <cstdlib>

struct StemLabAudioProcessingTestAccess
{
    struct RampSource final : juce::PositionableAudioSource
    {
        void prepareToPlay(int, double) override {}
        void releaseResources() override {}
        void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
        {
            largestRead = juce::jmax(largestRead, info.numSamples);
            for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
                for (int sample = 0; sample < info.numSamples; ++sample)
                    info.buffer->setSample(channel, info.startSample + sample,
                                           static_cast<float>((position + sample) % 1000) *
                                               0.0001f * (channel + 1));
            position += info.numSamples;
        }
        void setNextReadPosition(juce::int64 value) override { position = value; }
        juce::int64 getNextReadPosition() const override { return position; }
        juce::int64 getTotalLength() const override { return 1000000; }
        bool isLooping() const override { return false; }
        juce::int64 position = 0;
        int largestRead = 0;
    };

    static void require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "Audio regression: %s\n", message);
            std::abort();
        }
    }

    static void run()
    {
        checkSourceBlockSizes();
        checkMixFade();
        checkCaptureReconfiguration();
        // Both framework entry points must schedule MIDI at the rate they
        // gave the voices, including when the device changes rate.
        for (const auto rate : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0})
        {
            for (const bool sourcePreparation : {false, true})
            {
                StemLabAudioProcessor processor;
                processor.prepareToPlay(44100.0, 256);
                if (sourcePreparation)
                    processor.prepareToPlay(256, rate);
                else
                    processor.prepareToPlay(rate, 256);

                processor.midiInfos["test"].notes.push_back({0.0, 0.1, 69, 100, 1.0});
                require(processor.auditionMidi("test"), "MIDI audition starts");

                constexpr int offset = 13;
                const int noteOffSample = static_cast<int>(rate / 10.0);
                juce::AudioBuffer<float> audio(2, offset + noteOffSample + 2);
                for (int channel = 0; channel < 2; ++channel)
                    juce::FloatVectorOperations::fill(audio.getWritePointer(channel), 0.25f,
                                                      audio.getNumSamples());
                processor.renderMidiAudition(audio, offset, noteOffSample + 1);

                bool sawNoteOff = false;
                for (const auto event : processor.midiAuditionEvents)
                    if (event.getMessage().isNoteOff())
                    {
                        require(event.samplePosition == offset + noteOffSample,
                                "note-off follows the prepared sample rate");
                        sawNoteOff = true;
                    }
                require(sawNoteOff, "note-off is present in its block");
                for (int channel = 0; channel < 2; ++channel)
                {
                    require(juce::exactlyEqual(audio.getSample(channel, offset - 1), 0.25f),
                            "MIDI render preserves the prefix");
                    require(juce::exactlyEqual(audio.getSample(channel, audio.getNumSamples() - 1),
                                               0.25f),
                            "MIDI render preserves the suffix");
                }
            }
        }

        {
            StemLabAudioProcessor processor;
            processor.prepareToPlay(48000.0, 256);
            // An offline block may contain an entire dense transcription.
            for (int i = 0; i < 600; ++i)
                processor.midiInfos["dense"].notes.push_back({0.0, 0.01, 60, 90, 1.0});
            require(processor.auditionMidi("dense"), "dense MIDI audition starts");
            const auto* storage = processor.midiAuditionEvents.data.begin();
            juce::AudioBuffer<float> audio(2, 1024);
            audio.clear();
            processor.renderMidiAudition(audio, 0, audio.getNumSamples());
            require(processor.midiAuditionEvents.getNumEvents() == 1200,
                    "all dense MIDI events are rendered");
            require(storage == processor.midiAuditionEvents.data.begin(),
                    "dense MIDI rendering does not grow event storage");
        }

        for (const int channels : {1, 2})
        {
            StemLabAudioProcessor processor;
            auto layout = processor.getBusesLayout();
            layout.inputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(channels));
            layout.outputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(channels));
            require(processor.setBusesLayout(layout), "mono/stereo layout is accepted");
            processor.prepareToPlay(48000.0, 256);
            const auto scratchSize = processor.previewScratch.getNumSamples();
            for (const int samples : {0, 1, 17, 256, 8193})
            {
                juce::AudioBuffer<float> audio(channels, samples);
                for (int channel = 0; channel < channels; ++channel)
                    juce::FloatVectorOperations::fill(audio.getWritePointer(channel),
                                                      0.125f * (channel + 1), samples);
                juce::MidiBuffer midi;
                processor.processBlock(audio, midi);
                for (int channel = 0; channel < channels; ++channel)
                    for (int sample = 0; sample < samples; ++sample)
                        require(juce::exactlyEqual(audio.getSample(channel, sample),
                                                   0.125f * (channel + 1)),
                                "idle processing preserves host audio");
                require(processor.previewScratch.getNumSamples() == scratchSize,
                        "oversized blocks do not grow preview scratch");
            }

            // Exercise the active transport and its JUCE resampler as well
            // as the idle path. The source stays alive until detached.
            RampSource source;
            processor.previewTransport.setSource(&source, 0, nullptr, 48000.0);
            processor.previewTransport.start();
            juce::AudioBuffer<float> preview(channels, 8193);
            preview.clear();
            juce::MidiBuffer midi;
            processor.processBlock(preview, midi);
            for (int channel = 0; channel < channels; ++channel)
                for (int sample = 0; sample < preview.getNumSamples(); ++sample)
                    require(std::abs(preview.getSample(channel, sample) -
                                     static_cast<float>(sample % 1000) * 0.0001f * (channel + 1)) <
                                1.0e-6f,
                            "oversized preview preserves every sample and channel");
            require(processor.previewScratch.getNumSamples() == scratchSize,
                    "active preview keeps prepared scratch storage");
            processor.previewTransport.setSource(nullptr);
            processor.releaseResources();
            processor.prepareToPlay(96000.0, 17);
        }
    }

    static void checkSourceBlockSizes()
    {
        StemLabAudioProcessor processor;
        processor.prepareToPlay(256, 48000.0);
        RampSource source;
        processor.previewTransport.setSource(&source, 0, nullptr, 48000.0);
        processor.previewTransport.start();
        juce::AudioBuffer<float> audio(2, 8219);
        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill(audio.getWritePointer(channel), 0.25f,
                                              audio.getNumSamples());
        processor.getNextAudioBlock({&audio, 13, 8193});
        require(source.largestRead <= 256 + 32,
                "AudioSource oversized blocks keep resampler reads within prepared capacity");
        for (int channel = 0; channel < 2; ++channel)
        {
            require(juce::exactlyEqual(audio.getSample(channel, 12), 0.25f),
                    "AudioSource preserves prefix");
            require(juce::exactlyEqual(audio.getSample(channel, 8206), 0.25f),
                    "AudioSource preserves suffix");
            for (int sample = 0; sample < 8193; ++sample)
                require(std::abs(audio.getSample(channel, 13 + sample) -
                                 static_cast<float>(sample % 1000) * 0.0001f * (channel + 1)) <
                            1.0e-6f,
                        "AudioSource renders every oversized-block sample");
        }
        processor.previewTransport.setSource(nullptr);
    }

    static void checkMixFade()
    {
        const auto job = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("stemlab-mix-regression", "", false);
        const auto wavFile = job.getChildFile("baseline/vocals.wav");
        require(wavFile.getParentDirectory().createDirectory().wasOk(), "create mix fixture");
        {
            std::unique_ptr<juce::OutputStream> stream = wavFile.createOutputStream();
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriter::Options{}
                                                        .withSampleRate(48000.0)
                                                        .withNumChannels(2)
                                                        .withBitsPerSample(24));
            require(writer != nullptr, "create mix WAV writer");
            juce::AudioBuffer<float> audio(2, 48000);
            for (int channel = 0; channel < 2; ++channel)
                juce::FloatVectorOperations::fill(audio.getWritePointer(channel), 0.25f, 48000);
            require(writer->writeFromAudioSampleBuffer(audio, 0, 48000), "write mix fixture");
        }
        {
            StemLabAudioProcessor processor;
            processor.prepareToPlay(48000.0, 1024);
            processor.lastJobDirectory = job;
            processor.engineCompletedSuccessfully.store(true);
            require(processor.ensureStemMixLoaded(), "load mix fixture");
            processor.audioMonitorIsMix.store(true);
            processor.stemMixTransport.start();
            juce::AudioBuffer<float> audio(2, 1024);
            juce::MidiBuffer midi;
            bool ready = false;
            for (int attempt = 0; attempt < 200 && !ready; ++attempt)
            {
                juce::Thread::sleep(5); // wait for asynchronous read-ahead off the audio thread
                audio.clear();
                processor.processBlock(audio, midi);
                ready = std::abs(audio.getSample(0, 1000) - 0.25f) < 1.0e-5f;
            }
            require(ready, "mix read-ahead becomes ready");
            processor.setStemMute(0, true);
            processor.processBlock(audio, midi);
            require(std::abs(audio.getSample(0, 700)) < 1.0e-6f,
                    "mute reaches silence after 10 ms even inside a large block");
            processor.stemMixTransport.setSource(nullptr);
        }
        require(job.deleteRecursively(), "remove mix fixture");
    }

    static void checkCaptureReconfiguration()
    {
        for (const bool changeChannels : {false, true})
        {
            StemLabAudioProcessor processor;
            processor.prepareToPlay(48000.0, 256);
            require(processor.startHostAudioCapture(), "start capture before reconfiguration");
            const auto file = processor.getCaptureFile();
            juce::AudioBuffer<float> audio(2, 256);
            audio.clear();
            juce::MidiBuffer midi;
            processor.processBlock(audio, midi);
            processor.prepareToPlay(48000.0, 128);
            require(processor.isHostAudioCapturing(), "block-size-only change preserves capture");
            if (changeChannels)
            {
                auto layout = processor.getBusesLayout();
                layout.inputBuses.set(0, juce::AudioChannelSet::mono());
                layout.outputBuses.set(0, juce::AudioChannelSet::mono());
                require(processor.setBusesLayout(layout), "change capture layout to mono");
            }
            processor.prepareToPlay(changeChannels ? 48000.0 : 96000.0, 128);
            require(!processor.isHostAudioCapturing(),
                    "format change finalises capture before incompatible samples arrive");
            audio.setSize(changeChannels ? 1 : 2, 128);
            audio.clear();
            processor.processBlock(audio, midi);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            {
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
                require(reader != nullptr && juce::exactlyEqual(reader->sampleRate, 48000.0) &&
                            reader->numChannels == 2 && reader->lengthInSamples == 256,
                        "reconfiguration preserves the original capture format and length");
            }
            processor.previewTransport.setSource(nullptr);
            processor.previewReaderSource.reset();
            require(file.deleteFile(), "remove reconfigured capture");
        }
    }
};
