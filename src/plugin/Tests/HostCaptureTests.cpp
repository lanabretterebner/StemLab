#include "PluginProcessor.h"
#include "SelfFileDragGuard.h"

#include <cmath>
#include <cstdlib>

namespace
{
void check(bool condition)
{
    if (!condition)
        std::abort();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    juce::File capturedFile;

    {
        StemLabSelfFileDragGuard selfDrag;
        const auto originalSource = juce::File::getCurrentWorkingDirectory()
                                        .getChildFile("original-source.wav")
                                        .getFullPathName();
        const auto generatedStem = juce::File::getCurrentWorkingDirectory()
                                       .getChildFile("vocals.wav")
                                       .getFullPathName();
        juce::String currentSource = originalSource;

        selfDrag.begin({generatedStem});
        if (!selfDrag.shouldIgnore(generatedStem))
            currentSource = generatedStem;

        check(currentSource == originalSource);
        check(!selfDrag.shouldIgnore(originalSource));

        selfDrag.clear();
        check(!selfDrag.shouldIgnore(generatedStem));
    }

    {
        StemLabAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);

        check(processor.getHostUiMode() == stemlab::host::UiMode::genericVst);
        check(processor.startHostAudioCapture());
        check(processor.isHostAudioCapturing());
        check(!processor.startHostAudioCapture());

        juce::AudioBuffer<float> buffer(2, 512);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample,
                                 static_cast<float>((channel + 1) * (sample + 1)) / 2048.0f);

        juce::AudioBuffer<float> expected;
        expected.makeCopyOf(buffer);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                check(std::abs(buffer.getSample(channel, sample) -
                               expected.getSample(channel, sample)) < 1.0e-7f);

        processor.stopHostAudioCapture();
        check(!processor.isHostAudioCapturing());

        capturedFile = processor.getCaptureFile();
        check(capturedFile.existsAsFile());
        check(processor.getCaptureFile() == capturedFile);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(capturedFile));
        check(reader != nullptr);
        check(reader->numChannels == 2);
        check(std::abs(reader->sampleRate - 48000.0) < 1.0e-9);
        check(reader->lengthInSamples == 512);
    }

    {
        StemLabAudioProcessor processor;
        check(processor.getHostUiMode() == stemlab::host::UiMode::genericVst);
        check(processor.setInputAudioFile(capturedFile, 0.0, capturedFile.getFileName()));
        check(processor.getCaptureFile() == capturedFile);
        check(processor.getInputSourceLabel() == capturedFile.getFileName());
        check(processor.getCapturedSeconds() > 0.0);

        const auto sourceBeforeCancelledSelection = processor.getCaptureFile();
        const juce::File cancelledSelection;
        if (cancelledSelection.existsAsFile())
            processor.setInputAudioFile(cancelledSelection, 0.0,
                                        cancelledSelection.getFileName());

        check(processor.getCaptureFile() == sourceBeforeCancelledSelection);
    }

    check(capturedFile.deleteFile());
    return 0;
}
