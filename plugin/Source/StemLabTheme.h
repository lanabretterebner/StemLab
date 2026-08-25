#pragma once

#include <JuceHeader.h>

#include <optional>

/*
    Every visual decision StemLab's interface makes, in one place.

    The editor deliberately contains no colour or font literals, and its
    layout keeps only hairline 0-1 px trims inline: everything else it asks
    this header for. A redesign therefore starts - and mostly ends - here
    and in StemLabLookAndFeel, without touching control wiring, processor
    state, or layout logic.

        colours   Surfaces, text, accents, and per-widget fills.
        palette   Waveform display palettes plus the cross-DAW stem identity
                  colours shared with the Ableton Remote Script.
        fonts     Every font the interface uses.
        metrics   Layout dimensions. Entries that adapt to window size take
                  the editor's responsive flags (narrow: width < compactWidth,
                  shallow: height < compactHeight) and return the compact or
                  regular value.

    The values below transcribe the shipping 0.9.9 look exactly. Renaming or
    regrouping tokens is refactoring; changing values is the redesign.
*/

namespace stemlab::theme
{
    namespace colours
    {
        // Application surfaces.
        inline juce::Colour windowBackground() { return juce::Colour::fromRGB(14, 17, 22); }
        inline juce::Colour panel() { return juce::Colour::fromRGB(22, 27, 34); }
        inline juce::Colour panelOutline() { return juce::Colour::fromRGB(43, 50, 61); }

        // Brand accent: primary action buttons, progress fill, and the
        // drag-and-drop highlight all share this hue.
        inline juce::Colour accent() { return juce::Colour::fromRGB(113, 93, 255); }

        // Secondary text: subtitle, capture status, timing line.
        inline juce::Colour textMuted() { return juce::Colour::fromRGB(145, 154, 168); }

        // Record buttons. System/PC capture reads as "hot"; input capture is
        // a calmer slate so the two arm states scan differently.
        inline juce::Colour recordSystem() { return juce::Colour::fromRGB(194, 66, 94); }
        inline juce::Colour recordInput() { return juce::Colour::fromRGB(87, 102, 126); }

        inline juce::Colour progressTrack() { return juce::Colour::fromRGB(35, 42, 52); }

        // Waveform tiles.
        inline juce::Colour waveformBackground() { return juce::Colour::fromRGB(12, 15, 20); }
        inline juce::Colour waveformGrid() { return juce::Colour::fromRGB(44, 51, 62); }
        inline juce::Colour waveformCentreLine() { return juce::Colour::fromRGB(56, 63, 74); }
        inline juce::Colour waveformOutline() { return juce::Colour::fromRGB(54, 61, 73); }
        inline juce::Colour waveformPlayhead() { return juce::Colours::white.withAlpha(0.95f); }

        inline juce::Colour waveformPlaceholderText() { return textMuted().withAlpha(0.65f); }

        // The elapsed/total time badge drawn over the previewing waveform.
        inline juce::Colour badgeFill()
        {
            return juce::Colour::fromRGB(9, 11, 16).withAlpha(0.78f);
        }

        inline juce::Colour badgeText() { return juce::Colours::white.withAlpha(0.9f); }

        // Whole-window file-drag state.
        inline juce::Colour dragOverlay() { return accent().withAlpha(0.08f); }
        inline juce::Colour dragBorder() { return accent(); }
        inline juce::Colour dragPromptText() { return juce::Colours::white; }
    }

    namespace palette
    {
        /*
            User-selectable waveform palettes. Index 0 is the volume-mapped
            spectrum ramp; 1..6 are the solid hues below. The count and the
            user's choice live in StemLabAudioProcessor (waveformColourCount /
            getWaveformColourIndex); the menu names live in the editor's
            settings menu ("Spectrum (Volume)", "Violet", "Cyan", "Emerald",
            "Amber", "Pink", "Ice"). Keep all three in step.
        */
        inline juce::Colour solidWaveformColour(int index)
        {
            switch (index)
            {
            case 1:
                return juce::Colour::fromRGB(132, 102, 255); // Violet

            case 2:
                return juce::Colour::fromRGB(52, 210, 255); // Cyan

            case 3:
                return juce::Colour::fromRGB(66, 225, 154); // Emerald

            case 4:
                return juce::Colour::fromRGB(255, 179, 66); // Amber

            case 5:
                return juce::Colour::fromRGB(255, 91, 176); // Pink

            case 6:
                return juce::Colour::fromRGB(224, 234, 244); // Ice

            default:
                return juce::Colour::fromRGB(132, 102, 255);
            }
        }

        inline juce::Colour interpolateRamp(const juce::Colour& first, const juce::Colour& second,
                                            float amount)
        {
            return first.interpolatedWith(second, juce::jlimit(0.0f, 1.0f, amount));
        }

        inline juce::Colour spectrumColourForLevel(float level)
        {
            // Level is a perceptual 0..1 value derived from local dBFS.
            // Quiet material starts violet/blue, medium material moves through
            // cyan/green, and strong peaks reach yellow/orange.
            const auto value = juce::jlimit(0.0f, 1.0f, level);

            const juce::Colour violet = juce::Colour::fromRGB(119, 92, 255);

            const juce::Colour blue = juce::Colour::fromRGB(61, 124, 255);

            const juce::Colour cyan = juce::Colour::fromRGB(46, 220, 255);

            const juce::Colour green = juce::Colour::fromRGB(70, 231, 151);

            const juce::Colour yellow = juce::Colour::fromRGB(245, 235, 89);

            const juce::Colour orange = juce::Colour::fromRGB(255, 154, 66);

            if (value < 0.18f)
                return interpolateRamp(violet, blue, value / 0.18f);

            if (value < 0.38f)
                return interpolateRamp(blue, cyan, (value - 0.18f) / 0.20f);

            if (value < 0.62f)
                return interpolateRamp(cyan, green, (value - 0.38f) / 0.24f);

            if (value < 0.84f)
                return interpolateRamp(green, yellow, (value - 0.62f) / 0.22f);

            return interpolateRamp(yellow, orange, (value - 0.84f) / 0.16f);
        }

        inline juce::Colour waveformColourForLevel(int paletteIndex, float level)
        {
            const auto value = juce::jlimit(0.0f, 1.0f, level);

            if (paletteIndex == 0)
                return spectrumColourForLevel(value).withAlpha(0.96f);

            // Solid palettes remain the selected hue, but still react to volume:
            // quieter sections are darker/desaturated and peaks become brighter.
            auto base = solidWaveformColour(paletteIndex);

            const auto muted =
                base.withSaturation(juce::jlimit(0.20f, 1.0f, base.getSaturation() * 0.58f))
                    .withMultipliedBrightness(0.50f);

            const auto hot =
                base.withSaturation(juce::jlimit(0.0f, 1.0f, base.getSaturation() * 1.10f))
                    .withMultipliedBrightness(1.18f);

            return muted.interpolatedWith(hot, value).withAlpha(0.94f);
        }

        // The dB window behind every waveform colour: peaks below the floor
        // count as silence, and the map range decides where the spectrum
        // ramp's breakpoints land in real musical dynamics.
        constexpr float silenceFloorDecibels = -54.0f;
        constexpr float levelMapMinDecibels = -48.0f;
        constexpr float levelMapMaxDecibels = 0.0f;

        inline float perceptualWaveformLevel(float peak)
        {
            const auto safePeak = juce::jlimit(0.0f, 1.0f, peak);

            // dB mapping makes the colour changes useful across real musical
            // dynamics instead of bunching almost everything near "quiet".
            const auto decibels = juce::Decibels::gainToDecibels(safePeak, silenceFloorDecibels);

            return juce::jlimit(0.0f, 1.0f,
                                juce::jmap(decibels, levelMapMinDecibels, levelMapMaxDecibels,
                                           0.0f, 1.0f));
        }

        /*
            Cross-DAW stem identity colours: the track colours REAPER and
            Ableton create for inserted stems, so a user moving between hosts
            sees the same stem identity.

            Must stay byte-identical with _stem_color in
            integrations/ableton/StemLabRemote/__init__.py - the Remote
            Script cannot include this header.

            Note these are similar to, but NOT the same as, the solid
            waveform hues above (e.g. drums 0xFF9A42 here vs Amber 0xFFB342
            there). Unifying them is a redesign decision, not a refactor:
            changing either side alters what users already see.
        */
        inline std::optional<juce::Colour> stemIdentityColour(const juce::String& stemName)
        {
            struct Entry
            {
                const char* name;
                juce::uint32 rgb;
            };

            static constexpr Entry entries[] = {
                {"vocals", 0xF15BAA}, {"drums", 0xFF9A42}, {"bass", 0x34D2FF},
                {"guitar", 0x46E797}, {"piano", 0x8466FF}, {"other", 0xDCEAF4},
            };

            for (const auto& entry : entries)
            {
                if (stemName.equalsIgnoreCase(entry.name))
                    return juce::Colour(0xff000000u | entry.rgb);
            }

            return std::nullopt;
        }
    }

    namespace fonts
    {
        inline juce::FontOptions title() { return juce::FontOptions(24.0f, juce::Font::bold); }

        inline juce::FontOptions status() { return juce::FontOptions(14.0f, juce::Font::bold); }

        inline juce::FontOptions sectionHeading()
        {
            return juce::FontOptions(15.0f, juce::Font::bold);
        }

        inline juce::FontOptions dragPrompt() { return juce::FontOptions(18.0f, juce::Font::bold); }

        inline juce::FontOptions waveformPlaceholder() { return juce::FontOptions(11.0f); }

        inline juce::FontOptions badge() { return juce::FontOptions(10.5f); }
    }

    namespace metrics
    {
        // Below these the editor switches to its compact spacing. Width and
        // height act independently: narrow windows shrink button widths,
        // shallow windows shrink row heights.
        constexpr int compactWidth = 620;
        constexpr int compactHeight = 620;

        constexpr int pick(bool compact, int compactValue, int regularValue)
        {
            return compact ? compactValue : regularValue;
        }

        namespace window
        {
            constexpr int defaultWidth = 680;
            constexpr int defaultHeight = 680;

            constexpr int minWidth = 540;
            constexpr int minHeight = 540;
            constexpr int maxWidth = 1400;
            constexpr int maxHeight = 1200;

            constexpr int outerPadding(bool compact) { return pick(compact, 12, 18); }
        }

        namespace panel
        {
            constexpr float cornerRadius = 12.0f;

            // paint() carves the panel out of a fixed 18px margin and a fixed
            // 78px header reserve; resized() adapts its padding separately.
            // Kept as-is to preserve the shipped look - at compact sizes the
            // painted panel sits a few pixels wider than the laid-out
            // controls. Worth unifying during the redesign.
            constexpr float paintMargin = 18.0f;
            constexpr float headerReserve = 78.0f;

            constexpr float outlineThickness = 1.0f;
            constexpr float dragOutlineThickness = 2.5f;
            constexpr int dragPromptInset = 60;

            constexpr int insetX(bool narrow) { return pick(narrow, 7, 12); }
            constexpr int insetY(bool shallow) { return pick(shallow, 5, 8); }
        }

        namespace header
        {
            constexpr int height(bool shallow) { return pick(shallow, 46, 56); }
            constexpr int titleRowHeight(bool shallow) { return pick(shallow, 27, 32); }
            constexpr int settingsButtonWidth(bool narrow) { return pick(narrow, 74, 82); }
            constexpr int settingsButtonGap = 6;
            constexpr int gapBelow(bool shallow) { return pick(shallow, 4, 8); }
        }

        namespace controls
        {
            // Vertical rhythm of the stacked control rows.
            constexpr int rowGap(bool shallow) { return pick(shallow, 3, 5); }
            constexpr int tightRowGap(bool shallow) { return pick(shallow, 2, 4); }

            // Horizontal gaps between buttons inside a row.
            constexpr int buttonGap = 5;
            constexpr int buttonGapWide = 6;

            constexpr int inputRowHeight(bool shallow) { return pick(shallow, 30, 34); }

            constexpr int captureButtonWidthStandalone(bool narrow)
            {
                return pick(narrow, 92, 102);
            }

            constexpr int captureButtonWidthHosted(bool narrow) { return pick(narrow, 98, 108); }

            constexpr int playButtonWidth(bool narrow) { return pick(narrow, 52, 58); }

            constexpr int recordSystemWidthHosted(bool narrow) { return pick(narrow, 82, 92); }

            // The standalone app moves recording onto its own second row.
            constexpr int recordingRowHeight(bool shallow) { return pick(shallow, 28, 32); }
            constexpr int recordSystemWidthStandalone(bool narrow)
            {
                return pick(narrow, 104, 116);
            }
            constexpr int recordInputWidth(bool narrow) { return pick(narrow, 96, 108); }

            constexpr int refinementHeight(bool shallow) { return pick(shallow, 22, 25); }
            constexpr int separateButtonHeight(bool shallow) { return pick(shallow, 31, 36); }
            constexpr int statusHeight(bool shallow) { return pick(shallow, 18, 20); }
            constexpr int progressHeight(bool shallow) { return pick(shallow, 15, 18); }
            constexpr int timingHeight(bool shallow) { return pick(shallow, 17, 20); }
            constexpr int stemsHeadingHeight(bool shallow) { return pick(shallow, 19, 22); }
        }

        namespace actionRow
        {
            constexpr int height(bool shallow) { return pick(shallow, 30, 34); }
            constexpr int gapAbove(bool shallow) { return pick(shallow, 3, 5); }

            constexpr int sendWidthAbleton(bool narrow) { return pick(narrow, 108, 126); }
            constexpr int retryWidth(bool narrow) { return pick(narrow, 60, 70); }
            constexpr int sendWidthReaper(bool narrow) { return pick(narrow, 104, 118); }
            constexpr int saveWidth(bool narrow) { return pick(narrow, 112, 128); }
            constexpr int locationWidth(bool narrow) { return pick(narrow, 132, 150); }
        }

        namespace stemTree
        {
            constexpr int scrollbarThickness = 10;

            // Row height flexes with the window between these bounds; rows
            // tighter than the threshold drop to the smaller padding.
            constexpr int minRowHeight(bool shallow) { return pick(shallow, 36, 42); }
            constexpr int maxRowHeight = 72;
            constexpr int rowPadThreshold = 42;
            constexpr int rowPad(bool tight) { return pick(tight, 2, 4); }

            constexpr int contentMinWidth = 320;
            constexpr int viewportWidthInset = 12;
            constexpr int treeInsetX = 2;

            // Root stem rows.
            constexpr int expandWidth = 24;
            constexpr int checkboxWidth(bool narrow) { return pick(narrow, 88, 116); }
            constexpr int checkboxGap = 4;
            constexpr int playWidth(bool narrow) { return pick(narrow, 48, 55); }
            constexpr int playGap = 5;
            constexpr int menuWidth(bool narrow) { return pick(narrow, 28, 32); }
            constexpr int menuGap = 3;
        }

        namespace adaptiveRow
        {
            // Child rows of the adaptive stem tree indent by depth.
            constexpr int indentPerDepth = 14;
            constexpr int indentMin = 12;
            constexpr int indentMax = 54;

            constexpr int expandWidth = 22;
            constexpr int expandGap = 2;
            constexpr int noExpandIndent = 24;
            constexpr int expandPadX = 1;
            constexpr int expandPadY = 3;

            constexpr int menuWidth = 30;
            constexpr int menuGap = 3;

            constexpr int playWidth = 50;
            constexpr int playGap = 4;

            // Vertical trim on the play/menu buttons and on label/waveform.
            constexpr int buttonPadX = 1;
            constexpr int buttonPadY = 2;
            constexpr int contentPadY = 1;

            constexpr int labelMinWidth = 96;
            constexpr int labelMaxWidth = 160;
            constexpr int labelWidthDivisor = 4;
            constexpr int labelGap = 4;
        }

        namespace waveform
        {
            constexpr float cornerRadius = 6.0f;
            constexpr int inset = 4;

            constexpr int gridDivisions = 8;

            // A tile draws at most this many stacked channel lanes; files
            // with more channels collapse into two.
            constexpr int maxChannelLanes = 2;

            // Two-pixel slices retain plenty of visual detail while keeping the
            // six simultaneous waveform previews cheap to repaint at 20 Hz.
            constexpr int sliceWidth = 2;

            constexpr float channelHalfHeightRatio = 0.46f;

            // Keep extremely quiet material visible without pretending it is
            // loud: the trace never collapses below this half-height.
            constexpr float minVisibleHalfHeight = 0.55f;

            constexpr float traceThickness = 1.45f;
            constexpr float playheadThickness = 1.5f;

            constexpr int badgeRowHeight = 17;
            constexpr int badgeWidth = 82;
            constexpr float badgeCornerRadius = 4.0f;
            constexpr int badgeTextInsetX = 4;

            constexpr float outlineInset = 0.5f;
            constexpr float outlineThickness = 1.0f;

            // Below this many pixels of travel a gesture is a seek click;
            // beyond it, an external file drag.
            constexpr int clickVersusDragThreshold = 8;

            constexpr int thumbnailResolution = 512;
            constexpr int thumbnailCacheSize = 24;
        }

        // The editor repaints waveforms and re-polls processor state at this
        // rate; StemWaveformComponent's repaint cost is budgeted around it.
        constexpr int uiRefreshHz = 20;
    }
}
