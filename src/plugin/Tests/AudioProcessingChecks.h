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
};
