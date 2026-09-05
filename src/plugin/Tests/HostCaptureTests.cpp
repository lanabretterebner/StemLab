#include "MixAudibility.h"
#include "PluginProcessor.h"
#include "SelfFileDragGuard.h"
#include "AudioProcessingChecks.h"

#include <cmath>
#include <cstdlib>

namespace
{
void check(bool condition)
{
    if (!condition)
        std::abort();
}

/** setenv is POSIX; MSVC spells it differently and has no compatibility shim. */
void setEnvironment(const char* name, const juce::String& value)
{
   #if JUCE_WINDOWS
    _putenv_s(name, value.toRawUTF8());
   #else
    setenv(name, value.toRawUTF8(), 1);
   #endif
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    /*  The one rule the mixer and the lane drawing share.

        Muting a soloed lane used to light the button and change nothing,
        because "anything soloed" meant only solo counted - so the lane kept
        playing while its own button claimed otherwise.
    */
    {
        using stemlab::mix::isAudible;

        // Nothing soloed: mute is the whole answer.
        static_assert(isAudible(false, false, false));
        static_assert(!isAudible(true, false, false));

        // Something is soloed: this lane is heard only if it is one of them.
        static_assert(isAudible(false, true, true));
        static_assert(!isAudible(false, false, true));

        // And a soloed lane that is also muted stays silent.
        static_assert(!isAudible(true, true, true));
    }

    /*  Every setting the plugin remembers is written to a file under the
        config directory, and this suite constructs processors and changes
        settings. Point that directory at a temporary one first, or running
        the tests would rewrite the settings of whoever ran them.
    */
    const auto configSandbox =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("stemlab-test-config-"
                          + juce::String(juce::Time::getHighResolutionTicks()));

    configSandbox.createDirectory();

    // One per platform: StemLabPaths reads XDG_CONFIG_HOME on Linux,
    // LOCALAPPDATA on Windows, and the home directory on macOS.
    for (const auto* variable : {"XDG_CONFIG_HOME", "LOCALAPPDATA", "HOME"})
        setEnvironment(variable, configSandbox.getFullPathName());

    check(StemLabAudioProcessor::settingsPreferenceFile()
              .getParentDirectory()
              .isAChildOf(configSandbox)
          || StemLabAudioProcessor::settingsPreferenceFile().isAChildOf(configSandbox));

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

    /*  Nothing at all goes into the host's project, and every setting comes
        back from the preference file instead.

        The old JSON blob is what made a setting a per-project answer: open
        the same session on another machine and it overrode what that machine
        had been told, and every new project started from defaults however
        many times you had already chosen. These check the two halves - that
        the project chunk is empty, and that the file carries what the chunk
        used to.
    */
    {
        StemLabAudioProcessor::settingsPreferenceFile().deleteFile();

        StemLabAudioProcessor first;

        // Off unless asked for: refinement is a second pass over stems that
        // are already usable, and it costs real time on every separation.
        check(!first.isRefinementEnabled());

        juce::MemoryBlock chunk;
        first.getStateInformation(chunk);
        check(chunk.getSize() == 0);

        first.setRefinementEnabled(true);
        first.setSeparatorEngineIndex(StemLabAudioProcessor::separatorHybrid);
        first.setWaveformZoom(8.0);
        first.setEditorScalePercent(150);
        first.setStemEnabled(3, false);

        // Still nothing for the host, however much has been changed.
        first.getStateInformation(chunk);
        check(chunk.getSize() == 0);
    }

    // Written when the processor went away, and read by the next one.
    {
        check(StemLabAudioProcessor::settingsPreferenceFile().existsAsFile());

        StemLabAudioProcessor second;

        check(second.isRefinementEnabled());
        check(second.getSeparatorEngineIndex() == StemLabAudioProcessor::separatorHybrid);
        check(std::abs(second.getWaveformZoom() - 8.0) < 1.0e-9);
        check(second.getEditorScalePercent() == 150);
        check(!second.isStemEnabled(3));
        check(second.isStemEnabled(0));
    }

    /*  A file somebody edited by hand, or one written by a build whose
        ranges were wider, goes through the same setters the UI does. A
        remembered zoom of 4096 must come back clamped, not put the lanes
        somewhere no control could.
    */
    {
        StemLabAudioProcessor::settingsPreferenceFile().replaceWithText(
            R"({"waveformZoom": 4096.0, "editorScale": 9000, "separatorEngine": 77})");

        StemLabAudioProcessor clamped;

        check(clamped.getWaveformZoom() <= StemLabAudioProcessor::maxWaveformZoom);
        check(clamped.getEditorScalePercent() <= 400);
        check(clamped.getSeparatorEngineIndex()
              < StemLabAudioProcessor::separatorEngineCount);
    }

    /*  Upgrading must not silently reset everyone who already had settings.

        Every project saved by an older build still carries the blob this no
        longer writes, so the first project opened on a machine with no
        preference file donates its settings to that file - once. After that
        a file exists and project state is ignored, which is the point of it
        being a preference: one project decides, rather than whichever was
        opened last quietly redeciding for all the others.
    */
    const juce::String legacyState =
        R"({"refinement": true, "editorScale": 175, "separatorEngine": 1})";

    {
        StemLabAudioProcessor::settingsPreferenceFile().deleteFile();

        StemLabAudioProcessor upgrading;
        check(!upgrading.isRefinementEnabled());

        upgrading.setStateInformation(legacyState.toRawUTF8(),
                                      (int) legacyState.getNumBytesAsUTF8());

        check(upgrading.isRefinementEnabled());
        check(upgrading.getEditorScalePercent() == 175);

        // Adopted means written, so the next project finds a file.
        check(StemLabAudioProcessor::settingsPreferenceFile().existsAsFile());
    }

    {
        // A second project, with the preference file now in place: its own
        // saved settings are not allowed to speak for the user again.
        StemLabAudioProcessor settled;

        const juce::String otherState =
            R"({"refinement": false, "editorScale": 300, "separatorEngine": 0})";

        settled.setStateInformation(otherState.toRawUTF8(),
                                    (int) otherState.getNumBytesAsUTF8());

        check(settled.isRefinementEnabled());
        check(settled.getEditorScalePercent() == 175);
    }

    // Nothing at all, which is what an instance the host never restored gets.
    {
        StemLabAudioProcessor empty;
        empty.setStateInformation(nullptr, 0);

        check(empty.getEditorScalePercent() == 175);
    }

    // A host can construct all instances before restoring their chunks.
    // Only the first valid chunk may donate, even when both constructors
    // observed a missing settings file.
    {
        StemLabAudioProcessor::settingsPreferenceFile().deleteFile();
        StemLabAudioProcessor first;
        StemLabAudioProcessor second;
        first.setStateInformation(legacyState.toRawUTF8(),
                                  static_cast<int>(legacyState.getNumBytesAsUTF8()));
        const juce::String otherState = R"({"editorScale": 300})";
        second.setStateInformation(otherState.toRawUTF8(),
                                   static_cast<int>(otherState.getNumBytesAsUTF8()));
        check(second.getEditorScalePercent() == 175);
        StemLabAudioProcessor reloaded;
        check(reloaded.getEditorScalePercent() == 175);
    }

    /*  A tempo is not a preference, and neither is the mode that needs one.

        Everything else remembered here says how somebody works. A manual
        tempo, its meter and where bar one falls say what is in one piece of
        audio - and no instance restores the audio either, so a remembered
        174 would be a grid drawn confidently over whatever is loaded next.
        The mode goes with it: manual restored without its tempo opens at the
        default 120, which is the same confident grid over nothing.
    */
    {
        StemLabAudioProcessor::settingsPreferenceFile().deleteFile();

        {
            StemLabAudioProcessor tempo;
            tempo.setManualGrid(174.0, 7, 8, 1.5);
            tempo.setWaveformGridMode(StemLabAudioProcessor::gridManual);

            // Something that is remembered, so the file is written at all.
            tempo.setEditorScalePercent(133);
        }

        const auto stored =
            StemLabAudioProcessor::settingsPreferenceFile().loadFileAsString();

        check(stored.contains("editorScale"));
        check(!stored.contains("manualGridBpm"));
        check(!stored.contains("manualGridNumerator"));
        check(!stored.contains("manualGridDenominator"));
        check(!stored.contains("manualGridBarOne"));

        StemLabAudioProcessor next;

        check(next.getEditorScalePercent() == 133);
        check(next.getWaveformGridMode() != StemLabAudioProcessor::gridManual);
    }

    StemLabAudioProcessingTestAccess::run();
    StemLabAudioProcessingTestAccess::checkSourceLifecycle(capturedFile);
    StemLabAudioProcessingTestAccess::checkMidiResultRetirement(capturedFile);
    StemLabAudioProcessingTestAccess::checkMidiGridArguments();

    configSandbox.deleteRecursively();

    check(capturedFile.deleteFile());
    return 0;
}
