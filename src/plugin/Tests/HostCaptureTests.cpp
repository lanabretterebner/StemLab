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

        // No mix loaded: a Solo cannot silence anything, so no lane may dim.
        processor.setStemSolo(0, true);
        check(!processor.isAnySoloActive());
        for (int i = 0; i < StemLabAudioProcessor::stemCount; ++i)
            check(processor.isStemAudible(i));
        processor.setStemSolo(0, false);

        // The beat grid's tempo, which the painter reads as "no grid" at zero.
        {
            // Nothing has been analysed here, so the default source mode must
            // report no tempo. It used to answer 120, which drew a full grid
            // over audio nobody had measured.
            check(processor.getWaveformGridMode() == StemLabAudioProcessor::gridSource);
            check(processor.getSourceBpm() <= 0.0);
            check(processor.getWaveformGridScalars().bpm == 0.0);

            // Off is a real mode, not a value the clamp folds into Manual.
            processor.setWaveformGridMode(StemLabAudioProcessor::gridOff);
            check(processor.getWaveformGridMode() == StemLabAudioProcessor::gridOff);
            check(processor.getWaveformGridScalars().bpm == 0.0);

            // Manual carries whatever tempo was set, and survives a round trip
            // through the setter's range check.
            processor.setManualGrid(174.0, 4, 4, 0.0);
            processor.setWaveformGridMode(StemLabAudioProcessor::gridManual);
            check(processor.getWaveformGridScalars().bpm == 174.0);
            check(processor.getManualGridBpm() == 174.0);

            // Out-of-range tempos are clamped rather than stored, so no state
            // can put the grid somewhere the prompt would refuse.
            processor.setManualGrid(5000.0, 4, 4, 0.0);
            check(processor.getManualGridBpm() == 400.0);

            processor.setWaveformGridMode(StemLabAudioProcessor::gridSource);
        }

        processor.setStemSelectionRange("vocals", 0.2, 0.5);
        check(processor.getStemSelectionRange("vocals").active);

        // Loading a source invalidates every lane's loop range: it is
        // normalised against a file that is gone, and it would keep steering
        // the transport and trimming every drag and save.
        check(processor.setInputAudioFile(capturedFile, 0.0, capturedFile.getFileName()));
        check(!processor.getStemSelectionRange("vocals").active);

        const auto sourceBeforeCancelledSelection = processor.getCaptureFile();
        const juce::File cancelledSelection;
        if (cancelledSelection.existsAsFile())
            processor.setInputAudioFile(cancelledSelection, 0.0,
                                        cancelledSelection.getFileName());

        check(processor.getCaptureFile() == sourceBeforeCancelledSelection);

        // A file is a source only if something can read audio out of it.
        // Size cannot answer that: 64 bytes of text named .wav is bigger
        // than a WAV header and is not audio at all, and it used to load
        // and report a ready source with Separate live.
        const auto textNamedWav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                      .getChildFile("stemlab-not-audio.wav");
        textNamedWav.deleteFile();
        check(textNamedWav.replaceWithText("this is not audio at all, just text\n"));
        check(!processor.setInputAudioFile(textNamedWav, 0.0, textNamedWav.getFileName()));
        check(processor.getCaptureFile() == sourceBeforeCancelledSelection);
        check(textNamedWav.deleteFile());

        const auto emptyWav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("stemlab-empty.wav");
        emptyWav.deleteFile();
        check(emptyWav.create());
        check(!processor.setInputAudioFile(emptyWav, 0.0, emptyWav.getFileName()));
        check(processor.getCaptureFile() == sourceBeforeCancelledSelection);
        check(emptyWav.deleteFile());

        // A real WAV with no samples in it: readable, and still not a
        // source anything can be separated out of.
        const auto headerOnlyWav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("stemlab-header-only.wav");
        headerOnlyWav.deleteFile();

        {
            auto fileStream = std::make_unique<juce::FileOutputStream>(headerOnlyWav);
            check(fileStream->openedOk());

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
            const auto options = juce::AudioFormatWriter::Options{}
                                     .withSampleRate(48000.0)
                                     .withNumChannels(2)
                                     .withBitsPerSample(16);
            auto writer = wav.createWriterFor(stream, options);
            check(writer != nullptr);
        }

        check(headerOnlyWav.getSize() > 0);
        check(!processor.setInputAudioFile(headerOnlyWav, 0.0, headerOnlyWav.getFileName()));
        check(processor.getCaptureFile() == sourceBeforeCancelledSelection);
        check(headerOnlyWav.deleteFile());

        // The other half of the guard: an extension no bundled decoder
        // claims is still accepted, because the engine normalizes those
        // with FFmpeg long before anything here would have to read them.
        juce::AudioFormatManager bundledFormats;
        bundledFormats.registerBasicFormats();

        if (bundledFormats.findFormatForFileExtension(".m4a") == nullptr)
        {
            const auto engineOnlyFormat =
                juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("stemlab-engine-only.m4a");
            engineOnlyFormat.deleteFile();
            check(engineOnlyFormat.replaceWithText("stand-in for a container JUCE cannot read\n"));
            check(processor.setInputAudioFile(engineOnlyFormat, 0.0,
                                              engineOnlyFormat.getFileName()));
            check(processor.getCaptureFile() == engineOnlyFormat);
            check(engineOnlyFormat.deleteFile());
        }
    }

    /*  Refinement is off on a project that has never been told otherwise, and
        a project that was told stays told.

        The default is a product decision - refinement is a second pass over
        stems that are already usable, and it costs real time on every
        separation - and nothing else in the build would notice it being
        flipped back by a refactor. The round trip is here for the same
        reason: the state key is what keeps a user who wants refinement from
        having to turn it on again every session, and it is one line away
        from being nested inside another property's branch and silently lost.
    */
    {
        StemLabAudioProcessor fresh;
        check(!fresh.isRefinementEnabled());

        fresh.setRefinementEnabled(true);

        juce::MemoryBlock saved;
        fresh.getStateInformation(saved);

        StemLabAudioProcessor restored;
        check(!restored.isRefinementEnabled());

        restored.setStateInformation(saved.getData(), (int) saved.getSize());
        check(restored.isRefinementEnabled());
    }

    check(capturedFile.deleteFile());
    return 0;
}
