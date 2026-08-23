#include "PluginEditor.h"
#include "BinaryData.h"

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace
{
    juce::Colour background()
    {
        return juce::Colour::fromRGB (14, 17, 22);
    }

    juce::Colour panel()
    {
        return juce::Colour::fromRGB (22, 27, 34);
    }

    juce::Colour accent()
    {
        return juce::Colour::fromRGB (113, 93, 255);
    }

    juce::Colour textMuted()
    {
        return juce::Colour::fromRGB (145, 154, 168);
    }

    juce::File stemLabSettingsDirectory()
    {
        return juce::File::getSpecialLocation (
                   juce::File::userApplicationDataDirectory)
            .getChildFile ("StemLab");
    }

    juce::File firstRunMarkerFile()
    {
        return stemLabSettingsDirectory()
            .getChildFile ("portable-first-run-0.9.9.txt");
    }

    juce::File portableRootDirectory()
    {
        return juce::File::getSpecialLocation (
                   juce::File::currentExecutableFile)
            .getParentDirectory();
    }

    juce::File portableAbletonSetupScript()
    {
        auto root = portableRootDirectory();

        for (int depth = 0; depth < 6 && root.exists(); ++depth)
        {
            const auto candidate =
                root.getChildFile ("install_ableton_integration.ps1");

            if (candidate.existsAsFile())
                return candidate;

            const auto parent = root.getParentDirectory();
            if (parent == root)
                break;

            root = parent;
        }

        return {};
    }

    juce::String formatSeconds (double seconds)
    {
        if (seconds < 0.0)
            return "--:--";

        const int total =
            juce::jmax (
                0,
                static_cast<int> (seconds + 0.5));

        const int minutes = total / 60;
        const int secs = total % 60;

        return juce::String::formatted (
            "%02d:%02d",
            minutes,
            secs);
    }

    juce::Colour solidWaveformColour (int index)
    {
        switch (index)
        {
            case 1:
                return juce::Colour::fromRGB (132, 102, 255);

            case 2:
                return juce::Colour::fromRGB (52, 210, 255);

            case 3:
                return juce::Colour::fromRGB (66, 225, 154);

            case 4:
                return juce::Colour::fromRGB (255, 179, 66);

            case 5:
                return juce::Colour::fromRGB (255, 91, 176);

            case 6:
                return juce::Colour::fromRGB (224, 234, 244);

            default:
                return juce::Colour::fromRGB (132, 102, 255);
        }
    }

    juce::Colour interpolateRamp (
        const juce::Colour& first,
        const juce::Colour& second,
        float amount)
    {
        return first.interpolatedWith (
            second,
            juce::jlimit (0.0f, 1.0f, amount));
    }

    juce::Colour spectrumColourForLevel (float level)
    {
        // Level is a perceptual 0..1 value derived from local dBFS.
        // Quiet material starts violet/blue, medium material moves through
        // cyan/green, and strong peaks reach yellow/orange.
        const auto value =
            juce::jlimit (0.0f, 1.0f, level);

        const juce::Colour violet =
            juce::Colour::fromRGB (119, 92, 255);

        const juce::Colour blue =
            juce::Colour::fromRGB (61, 124, 255);

        const juce::Colour cyan =
            juce::Colour::fromRGB (46, 220, 255);

        const juce::Colour green =
            juce::Colour::fromRGB (70, 231, 151);

        const juce::Colour yellow =
            juce::Colour::fromRGB (245, 235, 89);

        const juce::Colour orange =
            juce::Colour::fromRGB (255, 154, 66);

        if (value < 0.18f)
            return interpolateRamp (violet, blue, value / 0.18f);

        if (value < 0.38f)
            return interpolateRamp (
                blue,
                cyan,
                (value - 0.18f) / 0.20f);

        if (value < 0.62f)
            return interpolateRamp (
                cyan,
                green,
                (value - 0.38f) / 0.24f);

        if (value < 0.84f)
            return interpolateRamp (
                green,
                yellow,
                (value - 0.62f) / 0.22f);

        return interpolateRamp (
            yellow,
            orange,
            (value - 0.84f) / 0.16f);
    }

    juce::Colour waveformColourForLevel (
        int paletteIndex,
        float level)
    {
        const auto value =
            juce::jlimit (0.0f, 1.0f, level);

        if (paletteIndex == 0)
            return spectrumColourForLevel (value).withAlpha (0.96f);

        // Solid palettes remain the selected hue, but still react to volume:
        // quieter sections are darker/desaturated and peaks become brighter.
        auto base = solidWaveformColour (paletteIndex);

        const auto muted =
            base
                .withSaturation (
                    juce::jlimit (
                        0.20f,
                        1.0f,
                        base.getSaturation() * 0.58f))
                .withMultipliedBrightness (0.50f);

        const auto hot =
            base
                .withSaturation (
                    juce::jlimit (
                        0.0f,
                        1.0f,
                        base.getSaturation() * 1.10f))
                .withMultipliedBrightness (1.18f);

        return muted
            .interpolatedWith (hot, value)
            .withAlpha (0.94f);
    }

    float perceptualWaveformLevel (float peak)
    {
        const auto safePeak =
            juce::jlimit (0.0f, 1.0f, peak);

        // dB mapping makes the colour changes useful across real musical
        // dynamics instead of bunching almost everything near "quiet".
        const auto decibels =
            juce::Decibels::gainToDecibels (
                safePeak,
                -54.0f);

        return juce::jlimit (
            0.0f,
            1.0f,
            juce::jmap (
                decibels,
                -48.0f,
                0.0f,
                0.0f,
                1.0f));
    }

}

StemWaveformComponent::StemWaveformComponent (
    StemLabAudioProcessor& processorIn,
    int stemIndexIn,
    juce::AudioFormatManager& formatManager,
    juce::AudioThumbnailCache& thumbnailCache)
    : processor (processorIn),
      stemIndex (stemIndexIn),
      thumbnail (512, formatManager, thumbnailCache)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks (true, false);
}

void StemWaveformComponent::setFile (const juce::File& file)
{
    if (file == currentFile)
        return;

    currentFile = file;
    thumbnail.clear();

    if (currentFile.existsAsFile())
    {
        thumbnail.setSource (
            new juce::FileInputSource (currentFile));
    }

    repaint();
}

void StemWaveformComponent::paint (juce::Graphics& g)
{
    const auto full = getLocalBounds().toFloat();

    g.setColour (juce::Colour::fromRGB (12, 15, 20));
    g.fillRoundedRectangle (full, 6.0f);

    const auto bounds =
        getLocalBounds().reduced (4);

    if (bounds.isEmpty())
        return;

    // Mini-meter-style faint timing grid.
    g.setColour (juce::Colour::fromRGB (44, 51, 62));

    for (int division = 1; division < 8; ++division)
    {
        const auto x =
            bounds.getX()
            + bounds.getWidth() * division / 8;

        g.drawVerticalLine (
            x,
            static_cast<float> (bounds.getY()),
            static_cast<float> (bounds.getBottom()));
    }

    g.setColour (juce::Colour::fromRGB (56, 63, 74));
    g.drawHorizontalLine (
        bounds.getCentreY(),
        static_cast<float> (bounds.getX()),
        static_cast<float> (bounds.getRight()));

    const auto length = thumbnail.getTotalLength();

    if (length > 0.0 && thumbnail.getNumChannels() > 0)
    {
        const int channelCount =
            juce::jlimit (
                1,
                2,
                thumbnail.getNumChannels());

        // Two-pixel slices retain plenty of visual detail while keeping the
        // six simultaneous waveform previews cheap to repaint at 20 Hz.
        constexpr int sliceWidth = 2;

        for (int channel = 0;
             channel < channelCount;
             ++channel)
        {
            const auto channelTop =
                bounds.getY()
                + bounds.getHeight() * channel / channelCount;

            const auto channelBottom =
                bounds.getY()
                + bounds.getHeight() * (channel + 1) / channelCount;

            const auto channelHeight =
                juce::jmax (
                    1,
                    channelBottom - channelTop);

            const auto centreY =
                static_cast<float> (channelTop)
                + static_cast<float> (channelHeight) * 0.5f;

            const auto halfHeight =
                static_cast<float> (channelHeight) * 0.46f;

            // The waveform colour is calculated per horizontal time slice,
            // so local volume determines the colour at that point in time.
            for (int x = bounds.getX();
                 x < bounds.getRight();
                 x += sliceWidth)
            {
                const auto normalisedStart =
                    static_cast<double> (x - bounds.getX())
                    / static_cast<double> (
                        juce::jmax (1, bounds.getWidth()));

                const auto normalisedEnd =
                    static_cast<double> (
                        juce::jmin (
                            bounds.getRight(),
                            x + sliceWidth)
                        - bounds.getX())
                    / static_cast<double> (
                        juce::jmax (1, bounds.getWidth()));

                const auto startTime =
                    normalisedStart * length;

                const auto endTime =
                    juce::jmax (
                        startTime + 0.000001,
                        normalisedEnd * length);

                float minimum = 0.0f;
                float maximum = 0.0f;

                thumbnail.getApproximateMinMax (
                    startTime,
                    endTime,
                    channel,
                    minimum,
                    maximum);

                const auto localPeak =
                    juce::jmax (
                        std::abs (minimum),
                        std::abs (maximum));

                const auto level =
                    perceptualWaveformLevel (localPeak);

                const auto colour =
                    waveformColourForLevel (
                        processor.getWaveformColourIndex(),
                        level);

                const auto yTop =
                    centreY
                    - juce::jlimit (
                        0.0f,
                        1.0f,
                        maximum)
                        * halfHeight;

                const auto yBottom =
                    centreY
                    - juce::jlimit (
                        -1.0f,
                        0.0f,
                        minimum)
                        * halfHeight;

                // Keep extremely quiet material visible without pretending it
                // is loud. This matches the dense, thin low-level trace style
                // used by meter-oriented waveform displays.
                const auto visibleTop =
                    juce::jmin (
                        yTop,
                        centreY - 0.55f);

                const auto visibleBottom =
                    juce::jmax (
                        yBottom,
                        centreY + 0.55f);

                g.setColour (colour);

                g.drawLine (
                    static_cast<float> (x),
                    visibleTop,
                    static_cast<float> (x),
                    visibleBottom,
                    1.45f);
            }
        }
    }
    else
    {
        g.setColour (textMuted().withAlpha (0.65f));
        g.setFont (juce::FontOptions (11.0f));

        g.drawText (
            "waveform",
            bounds,
            juce::Justification::centred);
    }

    const auto previewIndex =
        processor.getPreviewStemIndex();

    if (previewIndex == stemIndex)
    {
        const auto previewLength =
            processor.getPreviewLengthSeconds();

        const auto previewPosition =
            processor.getPreviewPositionSeconds();

        if (previewLength > 0.0)
        {
            const auto normalised =
                juce::jlimit (
                    0.0,
                    1.0,
                    previewPosition / previewLength);

            const auto x =
                static_cast<float> (bounds.getX())
                + static_cast<float> (normalised)
                    * static_cast<float> (bounds.getWidth());

            g.setColour (juce::Colours::white.withAlpha (0.95f));
            g.drawLine (
                x,
                static_cast<float> (bounds.getY()),
                x,
                static_cast<float> (bounds.getBottom()),
                1.5f);

            const auto timeText =
                formatSeconds (previewPosition)
                + " / "
                + formatSeconds (previewLength);

            auto badgeArea = bounds;
            auto badgeRow =
                badgeArea.removeFromTop (17);

            auto badge =
                badgeRow.removeFromRight (82);

            g.setColour (
                juce::Colour::fromRGB (9, 11, 16)
                    .withAlpha (0.78f));

            g.fillRoundedRectangle (
                badge.toFloat(),
                4.0f);

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::FontOptions (10.5f));

            g.drawText (
                timeText,
                badge.reduced (4, 0),
                juce::Justification::centredRight);
        }
    }

    g.setColour (
        juce::Colour::fromRGB (54, 61, 73));

    g.drawRoundedRectangle (
        full.reduced (0.5f),
        6.0f,
        1.0f);
}

void StemWaveformComponent::mouseDown (
    const juce::MouseEvent& event)
{
    if (! currentFile.existsAsFile() || getWidth() <= 0)
        return;

    const auto normalised =
        juce::jlimit (
            0.0,
            1.0,
            static_cast<double> (event.position.x)
                / static_cast<double> (getWidth()));

    processor.seekCompletedStem (
        stemIndex,
        normalised);

    repaint();
}

StemLabAudioProcessorEditor::StemLabAudioProcessorEditor (
    StemLabAudioProcessor& processorIn)
    : AudioProcessorEditor (&processorIn),
      processor (processorIn)
{
    setSize (680, 680);
    setResizable (true, true);

    // The UI is intentionally fluid. At the minimum size the waveform rows
    // collapse to compact strips; extra vertical space is given directly to
    // the six waveform rows instead of becoming dead space.
    setResizeLimits (
        540,
        540,
        1400,
        1200);

    if (processor.isStandaloneApp())
    {
        auto safeThis =
            juce::Component::SafePointer<StemLabAudioProcessorEditor> (this);

        juce::MessageManager::callAsync (
            [safeThis]
            {
                if (safeThis == nullptr)
                    return;

                if (auto* window =
                        safeThis->findParentComponentOfClass<
                            juce::DocumentWindow>())
                {
                    window->setUsingNativeTitleBar (true);
                    window->setName ("StemLab");

                    const auto appIcon =
                        juce::ImageFileFormat::loadFrom (
                            BinaryData::StemLabIcon_png,
                            BinaryData::StemLabIcon_pngSize);

                    if (appIcon.isValid())
                        window->setIcon (appIcon);
                }
            });
    }

    titleLabel.setText (
        "StemLab",
        juce::dontSendNotification);

    titleLabel.setFont (
        juce::FontOptions (
            24.0f,
            juce::Font::bold));

    addAndMakeVisible (titleLabel);

    subtitleLabel.setColour (
        juce::Label::textColourId,
        textMuted());

    subtitleLabel.setText (
        processor.isStandaloneApp()
            ? "Load or record audio, split it, audition stems, then save"
            : "Use a Live clip or record PC audio, split it, audition stems, then send",
        juce::dontSendNotification);

    addAndMakeVisible (subtitleLabel);

    settingsButton.onClick = [this]
    {
        showSettingsMenu();
    };
    addAndMakeVisible (settingsButton);

    captureButton.setColour (
        juce::TextButton::buttonColourId,
        accent());

    if (processor.isStandaloneApp())
    {
        captureButton.setButtonText ("Select File");
        captureButton.onClick = [this]
        {
            chooseStandaloneAudioFile();
        };

        stopButton.setVisible (false);

        playButton.setButtonText ("Play");
        playButton.onClick = [this]
        {
            processor.toggleStandalonePlayback();
            refreshFromProcessor();
        };
        addAndMakeVisible (playButton);

        recordSystemButton.setButtonText ("Record System");
        recordSystemButton.setColour (
            juce::TextButton::buttonColourId,
            juce::Colour::fromRGB (194, 66, 94));

        recordSystemButton.onClick = [this]
        {
            if (processor.getStandaloneRecordingMode()
                == StemLabAudioProcessor::recordingSystem)
            {
                processor.stopSystemAudioRecording();
            }
            else
            {
                processor.startSystemAudioRecording();
            }

            refreshFromProcessor();
        };
        addAndMakeVisible (recordSystemButton);

        recordInputButton.setButtonText ("Record Input");
        recordInputButton.setColour (
            juce::TextButton::buttonColourId,
            juce::Colour::fromRGB (87, 102, 126));

        recordInputButton.onClick = [this]
        {
            if (processor.getStandaloneRecordingMode()
                == StemLabAudioProcessor::recordingInput)
            {
                processor.stopStandaloneRecording();
            }
            else
            {
                processor.startStandaloneRecording();
            }

            refreshFromProcessor();
        };
        addAndMakeVisible (recordInputButton);

    }
    else
    {
        captureButton.setButtonText ("Use Live Clip");
        captureButton.onClick = [this]
        {
            processor.requestAbletonSourceClip();
            refreshFromProcessor();
        };

        stopButton.setVisible (false);

        playButton.setButtonText ("Play");
        playButton.onClick = [this]
        {
            processor.toggleStandalonePlayback();
            refreshFromProcessor();
        };
        addAndMakeVisible (playButton);

        recordSystemButton.setButtonText ("Record PC");
        recordSystemButton.setColour (
            juce::TextButton::buttonColourId,
            juce::Colour::fromRGB (194, 66, 94));

        recordSystemButton.onClick = [this]
        {
            if (processor.getStandaloneRecordingMode()
                == StemLabAudioProcessor::recordingSystem)
            {
                processor.stopSystemAudioRecording();
            }
            else
            {
                processor.startSystemAudioRecording();
            }

            refreshFromProcessor();
        };
        addAndMakeVisible (recordSystemButton);

        recordInputButton.setVisible (false);

    }

    addAndMakeVisible (captureButton);
    addAndMakeVisible (stopButton);

    captureTimeLabel.setJustificationType (
        juce::Justification::centredLeft);

    captureTimeLabel.setColour (
        juce::Label::textColourId,
        textMuted());

    addAndMakeVisible (captureTimeLabel);

    refinementButton.setToggleState (
        processor.isRefinementEnabled(),
        juce::dontSendNotification);

    refinementButton.setTooltip (
        "Runs after "
        + processor.getSeparatorEngineDisplayName()
        + " separation");

    refinementButton.onClick = [this]
    {
        processor.setRefinementEnabled (
            refinementButton.getToggleState());
    };

    addAndMakeVisible (refinementButton);

    separateButton.setColour (
        juce::TextButton::buttonColourId,
        accent());

    separateButton.setButtonText (
        "Separate All Stems");

    separateButton.onClick = [this]
    {
        processor.launchSeparationAndExport();
        refreshFromProcessor();
    };

    addAndMakeVisible (separateButton);

    progressBar.setColour (
        juce::ProgressBar::foregroundColourId,
        accent());

    progressBar.setColour (
        juce::ProgressBar::backgroundColourId,
        juce::Colour::fromRGB (35, 42, 52));

    progressBar.setPercentageDisplay (true);
    addAndMakeVisible (progressBar);

    statusLabel.setFont (
        juce::FontOptions (
            14.0f,
            juce::Font::bold));

    addAndMakeVisible (statusLabel);

    timingLabel.setColour (
        juce::Label::textColourId,
        textMuted());

    addAndMakeVisible (timingLabel);

    stemsLabel.setFont (
        juce::FontOptions (
            15.0f,
            juce::Font::bold));

    stemsLabel.setText (
        processor.isStandaloneApp()
            ? "Audition stems, then choose what to save"
            : "Audition stems, then choose what to send to Ableton",
        juce::dontSendNotification);

    addAndMakeVisible (stemsLabel);

    waveformFormats.registerBasicFormats();

    for (int i = 0;
         i < StemLabAudioProcessor::stemCount;
         ++i)
    {
        auto& button =
            stemButtons[static_cast<size_t> (i)];

        auto& preview =
            stemPlayButtons[static_cast<size_t> (i)];

        const auto name =
            StemLabAudioProcessor::getStemName (i);

        button.setButtonText (
            name.substring (0, 1).toUpperCase()
            + name.substring (1));

        button.setToggleState (
            processor.isStemEnabled (i),
            juce::dontSendNotification);

        button.onClick = [this, i]
        {
            processor.setStemEnabled (
                i,
                stemButtons[
                    static_cast<size_t> (i)]
                    .getToggleState());
        };

        preview.setButtonText ("Play");
        preview.onClick = [this, i]
        {
            processor.playCompletedStem (i);
            refreshFromProcessor();
        };

        addAndMakeVisible (button);
        addAndMakeVisible (preview);

        waveformComponents[
            static_cast<size_t> (i)]
            =
            std::make_unique<
                StemWaveformComponent> (
                    processor,
                    i,
                    waveformFormats,
                    waveformCache);

        addAndMakeVisible (
            *waveformComponents[
                static_cast<size_t> (i)]);
    }

    saveSelectedButton.setVisible (
        processor.isStandaloneApp());

    saveSelectedButton.onClick = [this]
    {
        chooseSaveFolder();
    };

    addAndMakeVisible (saveSelectedButton);

    sendSelectedButton.setVisible (
        ! processor.isStandaloneApp());

    sendSelectedButton.setColour (
        juce::TextButton::buttonColourId,
        accent());

    sendSelectedButton.onClick = [this]
    {
        processor.sendSelectedStemsToAbleton();
        refreshFromProcessor();
    };

    addAndMakeVisible (sendSelectedButton);

    retryImportButton.setVisible (
        ! processor.isStandaloneApp());

    retryImportButton.onClick = [this]
    {
        processor.retryAbletonImport();
        refreshFromProcessor();
    };

    addAndMakeVisible (retryImportButton);

    openJobButton.onClick = [this]
    {
        chooseJobRootFolder();
    };

    addAndMakeVisible (openJobButton);

    bridgeLabel.setVisible (false);

    processor.addChangeListener (this);

    startTimerHz (20);
    refreshFromProcessor();

    if (processor.isStandaloneApp())
    {
        auto safeThis =
            juce::Component::SafePointer<StemLabAudioProcessorEditor> (this);

        juce::MessageManager::callAsync (
            [safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->showFirstRunWelcome();
            });
    }
}

StemLabAudioProcessorEditor::~StemLabAudioProcessorEditor()
{
    processor.removeChangeListener (this);
    stopTimer();
}

bool StemLabAudioProcessorEditor::isSupportedAudioFile (
    const juce::File& file)
{
    const auto ext =
        file.getFileExtension().toLowerCase();

    return ext == ".wav"
        || ext == ".flac"
        || ext == ".mp3"
        || ext == ".aiff"
        || ext == ".aif"
        || ext == ".ogg";
}

bool StemLabAudioProcessorEditor::isInterestedInFileDrag (
    const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        if (isSupportedAudioFile (
                juce::File (path)))
        {
            return true;
        }
    }

    return false;
}

void StemLabAudioProcessorEditor::filesDropped (
    const juce::StringArray& files,
    int,
    int)
{
    dragActive = false;
    repaint();

    if (processor.isCapturing())
        return;

    for (const auto& path : files)
    {
        const juce::File file (path);

        if (isSupportedAudioFile (file)
            && (processor.isStandaloneApp()
                    ? processor.setStandaloneInputFile (file)
                    : processor.setInputAudioFile (
                        file,
                        processor.getCaptureStartPpq() >= 0.0
                            ? processor.getCaptureStartPpq()
                            : 0.0,
                        file.getFileName())))
        {
            refreshFromProcessor();
            return;
        }
    }
}

void StemLabAudioProcessorEditor::fileDragEnter (
    const juce::StringArray&,
    int,
    int)
{
    dragActive = true;
    repaint();
}

void StemLabAudioProcessorEditor::fileDragExit (
    const juce::StringArray&)
{
    dragActive = false;
    repaint();
}

void StemLabAudioProcessorEditor::paint (
    juce::Graphics& g)
{
    g.fillAll (background());

    auto area =
        getLocalBounds()
            .toFloat()
            .reduced (18.0f);

    auto panelArea =
        area.withTrimmedTop (78.0f);

    g.setColour (panel());
    g.fillRoundedRectangle (
        panelArea,
        12.0f);

    g.setColour (
        dragActive
            ? accent()
            : juce::Colour::fromRGB (
                43,
                50,
                61));

    g.drawRoundedRectangle (
        panelArea,
        12.0f,
        dragActive ? 2.5f : 1.0f);

    if (dragActive)
    {
        g.setColour (
            accent().withAlpha (0.08f));

        g.fillRoundedRectangle (
            panelArea,
            12.0f);

        g.setColour (juce::Colours::white);

        g.setFont (
            juce::FontOptions (
                18.0f,
                juce::Font::bold));

        g.drawFittedText (
            "Drop audio to load",
            getLocalBounds().reduced (60),
            juce::Justification::centred,
            1);
    }
}

void StemLabAudioProcessorEditor::resized()
{
    const int width =
        getWidth();

    const int height =
        getHeight();

    // Slightly smaller outside padding at compact sizes.
    const int outerPadding =
        width < 620 || height < 620
            ? 12
            : 18;

    auto area =
        getLocalBounds().reduced (
            outerPadding);

    const int headerHeight =
        height < 620
            ? 46
            : 56;

    auto header =
        area.removeFromTop (
            headerHeight);

    auto titleRow =
        header.removeFromTop (
            height < 620
                ? 27
                : 32);

    settingsButton.setBounds (
        titleRow.removeFromRight (
            width < 620
                ? 74
                : 82));

    titleRow.removeFromRight (6);
    titleLabel.setBounds (titleRow);
    subtitleLabel.setBounds (header);

    area.removeFromTop (
        height < 620 ? 4 : 8);

    const int panelInset =
        width < 620
            ? 7
            : 12;

    area.reduce (
        panelInset,
        height < 620 ? 5 : 8);

    const int compact =
        height < 620 ? 1 : 0;

    const int inputHeight =
        compact ? 30 : 34;

    auto inputRow =
        area.removeFromTop (
            inputHeight);

    const int useClipWidth =
        processor.isStandaloneApp()
            ? (width < 620 ? 92 : 102)
            : (width < 620 ? 98 : 108);

    captureButton.setBounds (
        inputRow.removeFromLeft (
            useClipWidth));

    inputRow.removeFromLeft (5);

    playButton.setBounds (
        inputRow.removeFromLeft (
            width < 620 ? 52 : 58));

    inputRow.removeFromLeft (5);

    if (! processor.isStandaloneApp())
    {
        recordSystemButton.setBounds (
            inputRow.removeFromLeft (
                width < 620 ? 82 : 92));

        inputRow.removeFromLeft (6);
    }

    captureTimeLabel.setBounds (
        inputRow);

    if (processor.isStandaloneApp())
    {
        area.removeFromTop (
            compact ? 2 : 4);

        auto recordingRow =
            area.removeFromTop (
                compact ? 28 : 32);

        recordSystemButton.setBounds (
            recordingRow.removeFromLeft (
                width < 620 ? 104 : 116));

        recordingRow.removeFromLeft (5);

        recordInputButton.setBounds (
            recordingRow.removeFromLeft (
                width < 620 ? 96 : 108));
    }

    area.removeFromTop (
        compact ? 3 : 5);

    refinementButton.setBounds (
        area.removeFromTop (
            compact ? 22 : 25));

    area.removeFromTop (
        compact ? 3 : 5);

    separateButton.setBounds (
        area.removeFromTop (
            compact ? 31 : 36));

    area.removeFromTop (
        compact ? 3 : 5);

    statusLabel.setBounds (
        area.removeFromTop (
            compact ? 18 : 20));

    progressBar.setBounds (
        area.removeFromTop (
            compact ? 15 : 18));

    timingLabel.setBounds (
        area.removeFromTop (
            compact ? 17 : 20));

    area.removeFromTop (
        compact ? 2 : 4);

    stemsLabel.setBounds (
        area.removeFromTop (
            compact ? 19 : 22));

    // Reserve the bottom action row first. Everything between the stem label
    // and that row becomes waveform space.
    const int bottomGap =
        compact ? 3 : 5;

    const int actionHeight =
        compact ? 30 : 34;

    auto actionRow =
        area.removeFromBottom (
            actionHeight);

    area.removeFromBottom (
        bottomGap);

    // Grow waveform rows with the window. This is the core scaling behavior:
    // 540px-tall window -> compact strips; 900px+ -> large waveform views.
    const int availableStemHeight =
        juce::jmax (
            6 * 28,
            area.getHeight());

    const int stemRowHeight =
        juce::jlimit (
            28,
            105,
            availableStemHeight
                / StemLabAudioProcessor::stemCount);

    // Centre the stem rows if rounding leaves a little unused vertical room.
    const int rowsHeight =
        stemRowHeight
        * StemLabAudioProcessor::stemCount;

    if (area.getHeight() > rowsHeight)
    {
        area.removeFromTop (
            (area.getHeight() - rowsHeight)
            / 2);
    }

    const int checkboxWidth =
        width < 620 ? 72 : 86;

    const int playWidth =
        width < 620 ? 48 : 55;

    for (int i = 0;
         i < StemLabAudioProcessor::stemCount;
         ++i)
    {
        auto row =
            area.removeFromTop (
                stemRowHeight);

        const int rowPad =
            stemRowHeight < 38
                ? 2
                : 4;

        auto checkboxArea =
            row.removeFromLeft (
                checkboxWidth)
                .reduced (0, rowPad);

        stemButtons[
            static_cast<size_t> (i)]
            .setBounds (
                checkboxArea);

        row.removeFromLeft (
            width < 620 ? 2 : 4);

        auto playArea =
            row.removeFromRight (
                playWidth)
                .reduced (
                    0,
                    juce::jmax (
                        2,
                        rowPad));

        stemPlayButtons[
            static_cast<size_t> (i)]
            .setBounds (
                playArea);

        row.removeFromRight (
            width < 620 ? 3 : 5);

        if (auto* waveform =
                waveformComponents[
                    static_cast<size_t> (i)]
                    .get())
        {
            waveform->setBounds (
                row.reduced (
                    0,
                    juce::jmax (
                        1,
                        rowPad - 1)));
        }
    }

    if (processor.isStandaloneApp())
    {
        saveSelectedButton.setBounds (
            actionRow.removeFromLeft (
                width < 620 ? 112 : 128));

        actionRow.removeFromLeft (5);
    }
    else
    {
        sendSelectedButton.setBounds (
            actionRow.removeFromLeft (
                width < 620 ? 108 : 126));

        actionRow.removeFromLeft (5);

        retryImportButton.setBounds (
            actionRow.removeFromLeft (
                width < 620 ? 60 : 70));

        actionRow.removeFromLeft (5);
    }

    const int locationWidth =
        juce::jmin (
            width < 620 ? 132 : 150,
            actionRow.getWidth());

    openJobButton.setBounds (
        actionRow.removeFromLeft (
            juce::jmax (
                0,
                locationWidth)));
}

void StemLabAudioProcessorEditor::timerCallback()
{
    processor.refreshEngineProgressFromDisk();

    if (! processor.isStandaloneApp())
    {
        processor.refreshAbletonSourceClipFromDisk();
        processor.refreshAbletonBridgeStatusFromDisk();
    }

    refreshFromProcessor();

    for (auto& waveform : waveformComponents)
    {
        if (waveform != nullptr)
            waveform->repaint();
    }
}

void StemLabAudioProcessorEditor::changeListenerCallback (
    juce::ChangeBroadcaster*)
{
    refreshFromProcessor();
}

void StemLabAudioProcessorEditor::refreshFromProcessor()
{
    const auto capturing =
        processor.isCapturing();

    const auto captureArmed =
        processor.isCaptureArmed();

    const auto captureFinalizing =
        processor.isCaptureFinalizing();

    const auto recordingMode =
        processor.getStandaloneRecordingMode();

    const auto engineRunning =
        processor.isEngineRunning();

    const auto captureFile =
        processor.getCaptureFile();

    const auto captureExists =
        captureFile.existsAsFile();

    const auto jobDone =
        processor.hasSuccessfulJob();

    captureButton.setEnabled (
        ! capturing
        && ! captureArmed
        && ! captureFinalizing
        && ! engineRunning);

    stopButton.setEnabled (false);

    separateButton.setEnabled (
        ! capturing
        && ! captureArmed
        && ! captureFinalizing
        && ! processor.isAwaitingAbletonSourceClip()
        && ! engineRunning
        && captureExists);

    openJobButton.setEnabled (
        ! engineRunning);

    if (processor.isStandaloneApp())
    {
        juce::String captureText;

        if (capturing)
        {
            captureText =
                recordingMode
                    == StemLabAudioProcessor::recordingSystem
                    ? "System recording - "
                    : "Input recording - ";

            captureText +=
                formatSeconds (
                    processor.getCapturedSeconds());
        }
        else if (captureExists)
        {
            captureText =
                captureFile.getFileName()
                + " - "
                + formatSeconds (
                    processor.getCapturedSeconds());
        }
        else
        {
            captureText = "No file selected";
        }

        captureTimeLabel.setText (
            captureText,
            juce::dontSendNotification);

        captureButton.setEnabled (
            ! capturing
            && ! engineRunning);

        recordSystemButton.setEnabled (
            ! engineRunning
            && (recordingMode
                    == StemLabAudioProcessor::recordingNone
                || recordingMode
                    == StemLabAudioProcessor::recordingSystem));

        recordInputButton.setEnabled (
            ! engineRunning
            && (recordingMode
                    == StemLabAudioProcessor::recordingNone
                || recordingMode
                    == StemLabAudioProcessor::recordingInput));

        recordSystemButton.setButtonText (
            recordingMode
                    == StemLabAudioProcessor::recordingSystem
                ? "Stop System"
                : "Record System");

        recordInputButton.setButtonText (
            recordingMode
                    == StemLabAudioProcessor::recordingInput
                ? "Stop Input"
                : "Record Input");

        playButton.setEnabled (
            captureExists
            && ! engineRunning
            && ! capturing);

        const auto previewIndex =
            processor.getPreviewStemIndex();

        const auto previewPlaying =
            processor.isStandalonePlaying();

        playButton.setButtonText (
            previewPlaying
                && previewIndex == -1
                    ? "Pause"
                    : "Play");

        for (int i = 0;
             i < StemLabAudioProcessor::stemCount;
             ++i)
        {
            const auto stemFile =
                jobDone
                    ? processor.getCompletedStemFile (i)
                    : juce::File {};

            stemButtons[
                static_cast<size_t> (i)]
                .setEnabled (
                    jobDone
                    && ! engineRunning
                    && ! capturing);

            auto& preview =
                stemPlayButtons[
                    static_cast<size_t> (i)];

            preview.setEnabled (
                jobDone
                && ! engineRunning
                && ! capturing
                && stemFile.existsAsFile());

            preview.setButtonText (
                previewPlaying
                    && previewIndex == i
                        ? "Pause"
                        : "Play");

            if (auto* waveform =
                    waveformComponents[
                        static_cast<size_t> (i)]
                        .get())
            {
                waveform->setFile (stemFile);
                waveform->setEnabled (
                    jobDone
                    && ! engineRunning
                    && ! capturing
                    && stemFile.existsAsFile());
            }
        }

        saveSelectedButton.setEnabled (
            jobDone
            && ! engineRunning
            && ! capturing);
    }
    else
    {
        juce::String captureText;

        if (processor.isAwaitingAbletonSourceClip())
        {
            captureText =
                "Reading Live clip...";
        }
        else if (
            recordingMode
                == StemLabAudioProcessor::recordingSystem
            && capturing)
        {
            captureText =
                "Recording PC - "
                + formatSeconds (
                    processor.getCapturedSeconds());
        }
        else if (captureExists)
        {
            const auto label =
                processor.getInputSourceLabel();

            captureText =
                (label.isNotEmpty()
                    ? label
                    : captureFile.getFileName());

            const auto duration =
                processor.getCapturedSeconds();

            if (duration > 0.0)
            {
                captureText +=
                    " - "
                    + formatSeconds (duration);
            }

            captureText +=
                " - beat "
                + juce::String (
                    processor.getCaptureStartPpq(),
                    3);
        }
        else
        {
            captureText =
                "Select a Live audio clip, then Use Live Clip";
        }

        captureTimeLabel.setText (
            captureText,
            juce::dontSendNotification);

        captureButton.setEnabled (
            ! capturing
            && ! engineRunning
            && ! processor.isAwaitingAbletonSourceClip());

        recordSystemButton.setEnabled (
            ! engineRunning
            && ! processor.isAwaitingAbletonSourceClip()
            && (recordingMode
                    == StemLabAudioProcessor::recordingNone
                || recordingMode
                    == StemLabAudioProcessor::recordingSystem));

        recordSystemButton.setButtonText (
            recordingMode
                    == StemLabAudioProcessor::recordingSystem
                ? "Stop PC"
                : "Record PC");

        playButton.setEnabled (
            captureExists
            && ! engineRunning
            && ! capturing);

        const auto previewIndex =
            processor.getPreviewStemIndex();

        const auto previewPlaying =
            processor.isStandalonePlaying();

        playButton.setButtonText (
            previewPlaying
                && previewIndex == -1
                    ? "Pause"
                    : "Play");

        int selectedCount = 0;

        for (int i = 0;
             i < StemLabAudioProcessor::stemCount;
             ++i)
        {
            const auto stemFile =
                jobDone
                    ? processor.getCompletedStemFile (i)
                    : juce::File {};

            if (processor.isStemEnabled (i))
                ++selectedCount;

            stemButtons[
                static_cast<size_t> (i)]
                .setEnabled (
                    jobDone
                    && ! engineRunning
                    && ! capturing);

            auto& preview =
                stemPlayButtons[
                    static_cast<size_t> (i)];

            preview.setVisible (true);

            preview.setEnabled (
                jobDone
                && ! engineRunning
                && ! capturing
                && stemFile.existsAsFile());

            preview.setButtonText (
                previewPlaying
                    && previewIndex == i
                        ? "Pause"
                        : "Play");

            if (auto* waveform =
                    waveformComponents[
                        static_cast<size_t> (i)]
                        .get())
            {
                waveform->setFile (stemFile);

                waveform->setEnabled (
                    jobDone
                    && ! engineRunning
                    && ! capturing
                    && stemFile.existsAsFile());
            }
        }

        sendSelectedButton.setEnabled (
            jobDone
            && ! engineRunning
            && ! capturing
            && selectedCount > 0);

        retryImportButton.setEnabled (
            jobDone
            && ! engineRunning);

        bridgeLabel.setText (
            processor.getAbletonBridgeStatus(),
            juce::dontSendNotification);
    }

    progressValue =
        processor.getEngineProgress();

    statusLabel.setText (
        processor.getStatus(),
        juce::dontSendNotification);

    if (engineRunning)
    {
        const auto elapsed =
            processor.getEngineElapsedSeconds();

        const auto eta =
            processor.getEngineEstimatedRemainingSeconds();

        timingLabel.setText (
            "Elapsed "
                + formatSeconds (elapsed)
                + "   |   ETA "
                + (eta >= 0.0
                    ? formatSeconds (eta)
                    : "estimating..."),
            juce::dontSendNotification);
    }
    else if (jobDone)
    {
        timingLabel.setText (
            "Completed in "
                + formatSeconds (
                    processor.getEngineElapsedSeconds()),
            juce::dontSendNotification);
    }
    else
    {
        timingLabel.setText (
            "Elapsed 00:00   |   ETA --:--",
            juce::dontSendNotification);
    }
}

void StemLabAudioProcessorEditor::chooseStandaloneAudioFile()
{
    if (! processor.isStandaloneApp()
        || processor.isCapturing())
    {
        return;
    }

    audioFileChooser =
        std::make_unique<juce::FileChooser> (
            "Choose audio file",
            juce::File::getSpecialLocation (
                juce::File::userHomeDirectory),
            "*.wav;*.flac;*.mp3;*.aiff;*.aif;*.ogg");

    audioFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& chooser)
        {
            const auto result =
                chooser.getResult();

            if (result.existsAsFile())
            {
                processor.setStandaloneInputFile (
                    result);

                refreshFromProcessor();
            }
        });
}

void StemLabAudioProcessorEditor::chooseSaveFolder()
{
    if (! processor.isStandaloneApp()
        || ! processor.hasSuccessfulJob())
    {
        return;
    }

    outputFolderChooser =
        std::make_unique<juce::FileChooser> (
            "Choose where to save selected stems",
            juce::File::getSpecialLocation (
                juce::File::userMusicDirectory));

    outputFolderChooser->launchAsync (
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [this] (const juce::FileChooser& chooser)
        {
            const auto folder =
                chooser.getResult();

            if (folder.isDirectory())
                processor.saveSelectedStemsTo (folder);
        });
}

void StemLabAudioProcessorEditor::chooseJobRootFolder()
{
    auto start =
        processor.getJobRootDirectory();

    if (! start.isDirectory())
    {
        start =
            juce::File::getSpecialLocation (
                juce::File::userMusicDirectory);
    }

    jobFolderChooser =
        std::make_unique<juce::FileChooser> (
            "Choose StemLab file location",
            start);

    jobFolderChooser->launchAsync (
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [this] (const juce::FileChooser& chooser)
        {
            const auto folder =
                chooser.getResult();

            if (folder.isDirectory())
            {
                processor.setJobRootDirectory (
                    folder);

                refreshFromProcessor();
            }
        });
}

void StemLabAudioProcessorEditor::showSettingsMenu()
{
    juce::PopupMenu menu;

    if (processor.isStandaloneApp())
    {
        menu.addSectionHeader ("Audio");
        menu.addItem (
            1,
            "Audio/MIDI Settings...");
        menu.addSeparator();
    }

    menu.addSectionHeader ("Display");

    juce::PopupMenu waveformMenu;

    const juce::StringArray colourNames
    {
        "Spectrum (Volume)",
        "Violet",
        "Cyan",
        "Emerald",
        "Amber",
        "Pink",
        "Ice"
    };

    for (int i = 0;
         i < colourNames.size();
         ++i)
    {
        waveformMenu.addItem (
            100 + i,
            colourNames[i],
            true,
            processor.getWaveformColourIndex() == i);
    }

    menu.addSubMenu (
        "Waveform Color",
        waveformMenu);

    menu.addSeparator();

    menu.addSectionHeader ("Separator");

    juce::PopupMenu separatorMenu;

    const juce::StringArray separatorNames
    {
        "BS-RoFormer",
        "Demucs (htdemucs_6s)",
        "Hybrid (RoFormer + Demucs)"
    };

    for (int i = 0;
         i < separatorNames.size();
         ++i)
    {
        separatorMenu.addItem (
            200 + i,
            separatorNames[i],
            ! processor.isEngineRunning(),
            processor.getSeparatorEngineIndex() == i);
    }

    menu.addSubMenu (
        "Separation Engine",
        separatorMenu);

    menu.addSeparator();

    menu.addSectionHeader ("StemLab engine");

    menu.addItem (
        2,
        "Choose engine executable...");

    menu.addItem (
        3,
        "Auto-detect engine");

    menu.addSeparator();

    menu.addItem (
        4,
        "Copy diagnostics to clipboard",
        processor.getEngineLog().isNotEmpty());

    if (processor.isStandaloneApp())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Ableton Live");
        menu.addItem (
            5,
            "Install / Repair Ableton Integration...");
    }

    auto safeThis =
        juce::Component::SafePointer<
            StemLabAudioProcessorEditor> (this);

    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetComponent (&settingsButton),
        [safeThis] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)
            {
                safeThis->showStandaloneAudioSettings();
            }
            else if (result == 2)
            {
                safeThis->chooseEngineExecutable();
            }
            else if (result == 3)
            {
                safeThis->processor
                    .resetEngineCommandToAutoDiscover();
            }
            else if (result == 4)
            {
                juce::SystemClipboard
                    ::copyTextToClipboard (
                        safeThis->processor
                            .getEngineLog());

                safeThis->processor.postUiStatus (
                    "Diagnostics copied to clipboard");
            }
            else if (result == 5)
            {
                safeThis->launchAbletonSetup();
            }
            else if (result >= 100
                     && result
                        < 100
                            + StemLabAudioProcessor
                                ::waveformColourCount)
            {
                safeThis->processor
                    .setWaveformColourIndex (
                        result - 100);
            }
            else if (result >= 200
                     && result
                        < 200
                            + StemLabAudioProcessor
                                ::separatorEngineCount)
            {
                safeThis->processor
                    .setSeparatorEngineIndex (
                        result - 200);

                safeThis->processor.postUiStatus (
                    "Separator: "
                    + safeThis->processor
                        .getSeparatorEngineDisplayName());
            }

            safeThis->refreshFromProcessor();
        });
}

void StemLabAudioProcessorEditor::showFirstRunWelcome()
{
    if (! processor.isStandaloneApp())
        return;

    const auto root = portableRootDirectory();
    const auto portableEngine =
        root.getChildFile ("Engine")
            .getChildFile ("python.exe");
    const auto setupScript = portableAbletonSetupScript();

    // Only show onboarding for an actual extracted portable release. Normal
    // source/development builds should open directly without nagging.
    if (! portableEngine.existsAsFile()
        || ! setupScript.existsAsFile()
        || firstRunMarkerFile().existsAsFile())
    {
        return;
    }

    auto options = juce::MessageBoxOptions()
        .withIconType (juce::MessageBoxIconType::InfoIcon)
        .withTitle ("Welcome to StemLab")
        .withMessage (
            "StemLab is ready to use as a standalone app.\n\n"
            "If you use Ableton Live, StemLab can set up its VST3 and "
            "Remote Script now. This does not copy the large ML engine a "
            "second time.")
        .withButton ("Set Up Ableton")
        .withButton ("Use Standalone")
        .withAssociatedComponent (this);

    auto safeThis =
        juce::Component::SafePointer<StemLabAudioProcessorEditor> (this);

    juce::AlertWindow::showAsync (
        options,
        [safeThis] (int result)
        {
            auto settings = stemLabSettingsDirectory();
            settings.createDirectory();
            firstRunMarkerFile().replaceWithText (
                "StemLab portable onboarding completed.\n");

            if (safeThis != nullptr && result == 1)
                safeThis->launchAbletonSetup();
        });
}

void StemLabAudioProcessorEditor::launchAbletonSetup()
{
    if (! processor.isStandaloneApp())
        return;

    const auto script = portableAbletonSetupScript();

    if (! script.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Ableton setup not found",
            "install_ableton_integration.ps1 was not found beside this "
            "StemLab portable release.",
            "OK",
            this);
        return;
    }

    auto systemRoot = juce::SystemStats::getEnvironmentVariable (
        "SystemRoot",
        "C:\\Windows");

    const auto powershell =
        juce::File (systemRoot)
            .getChildFile ("System32")
            .getChildFile ("WindowsPowerShell")
            .getChildFile ("v1.0")
            .getChildFile ("powershell.exe");

    if (! powershell.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "PowerShell not found",
            "StemLab could not start the Ableton setup helper.",
            "OK",
            this);
        return;
    }

    const auto arguments =
        "-NoProfile -ExecutionPolicy Bypass -File \""
        + script.getFullPathName()
        + "\"";

    if (powershell.startAsProcess (arguments))
    {
        processor.postUiStatus (
            "Ableton setup opened - follow the Windows prompt");
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Could not start Ableton setup",
            "Run install_ableton_integration.ps1 from the extracted StemLab "
            "folder instead.",
            "OK",
            this);
    }
}

void StemLabAudioProcessorEditor::showStandaloneAudioSettings()
{
    if (! processor.isStandaloneApp())
        return;

   #if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
    if (auto* holder =
            juce::StandalonePluginHolder::getInstance())
    {
        holder->showAudioSettingsDialog();
        return;
    }
   #endif

    processor.postUiStatus (
        "Standalone audio settings are unavailable");
}

void StemLabAudioProcessorEditor::chooseEngineExecutable()
{
    auto start =
        juce::File (
            processor.getEngineCommand());

    if (! start.exists())
    {
        start =
            juce::File::getSpecialLocation (
                juce::File::userHomeDirectory);
    }

    fileChooser =
        std::make_unique<juce::FileChooser> (
            "Choose stemlab-plugin-job executable",
            start,
            "*.exe");

    fileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& chooser)
        {
            const auto result =
                chooser.getResult();

            if (result.existsAsFile())
            {
                processor.setEngineCommand (
                    result.getFullPathName());

                processor.postUiStatus (
                    "Engine path updated");

                refreshFromProcessor();
            }
        });
}
