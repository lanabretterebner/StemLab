#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <optional>

#include "StemLabAccent.h"

/*
    Every visual decision StemLab's interface makes, in one place: the
    "Nocturne" design system, draft 1a "Lanes". The design handoff this was
    built from is no longer in the repository, so these values are the only
    record of the tokens - treat this file as the spec, not as a copy of one.

    The editor deliberately contains no color or font literals; layout
    values are tokens apart from a few small one-off trims at their call
    sites. Restyle by changing values here and in the drawing code of
    StemLabLookAndFeel / StemLabWidgets; only re-arrangements of the
    interface itself touch the editor.

        colors   Ground/surface/text, the accent and neutral ramps, and
                  per-component color roles.
        palette   The cross-DAW stem identity colors shared with the
                  Ableton Remote Script.
        fonts     Inter-based type scale (weight 500 is expressed as
                  juce::Font::bold and resolved to Inter Medium by
                  StemLabLookAndFeel; nothing renders bolder than 500).
        metrics   Layout dimensions of the Lanes panel, at the design
                  size the whole panel is scaled from.
*/

namespace stemlab::theme
{
    namespace colors
    {
        // Nocturne ground and surface.
        inline juce::Colour ground() { return juce::Colour(0xff161826); }
        inline juce::Colour surface() { return juce::Colour(0xff232532); }

        inline juce::Colour text() { return juce::Colour(0xffe9e9ed); }
        inline juce::Colour divider() { return text().withAlpha(0.16f); }

        /*  The accent, used as lines, tints and glows, and the ramp under
            it (OKLCH-generated in the token sheet).

            Settable rather than literal: these ten are the only place the
            accent hue enters the design system, so every token below - the
            fills, the playhead, the record dot, the progress bar - follows
            the setting without knowing it exists. The values live in
            StemLabAccent.h, which also says how the other hues are made and
            why the default is not one of them.
        */
        inline juce::Colour accent() { return accents::step(accents::Step::base); }

        inline juce::Colour accent100() { return accents::step(accents::Step::s100); }
        inline juce::Colour accent200() { return accents::step(accents::Step::s200); }
        inline juce::Colour accent300() { return accents::step(accents::Step::s300); }
        inline juce::Colour accent400() { return accents::step(accents::Step::s400); }
        inline juce::Colour accent500() { return accents::step(accents::Step::s500); }
        inline juce::Colour accent600() { return accents::step(accents::Step::s600); }
        inline juce::Colour accent700() { return accents::step(accents::Step::s700); }
        inline juce::Colour accent800() { return accents::step(accents::Step::s800); }
        inline juce::Colour accent900() { return accents::step(accents::Step::s900); }

        // Neutral ramp.
        inline juce::Colour neutral100() { return juce::Colour(0xfff3f5fe); }
        inline juce::Colour neutral200() { return juce::Colour(0xffe4e7f5); }
        inline juce::Colour neutral300() { return juce::Colour(0xffcfd3e5); }
        inline juce::Colour neutral400() { return juce::Colour(0xffb2b6ca); }
        inline juce::Colour neutral500() { return juce::Colour(0xff9397ab); }
        inline juce::Colour neutral600() { return juce::Colour(0xff75798c); }
        inline juce::Colour neutral700() { return juce::Colour(0xff595d6c); }
        inline juce::Colour neutral800() { return juce::Colour(0xff3f424d); }
        inline juce::Colour neutral900() { return juce::Colour(0xff292b31); }

        // Muted text = text at a per-component opacity (spec: 40-75%).
        inline juce::Colour text75() { return text().withAlpha(0.75f); }
        inline juce::Colour text50() { return text().withAlpha(0.50f); }
        inline juce::Colour text45() { return text().withAlpha(0.45f); }

        // Menu section headings. Deliberately above the disabled-item color
        // (text at metrics::disabledOpacity, which is also 45%), so a heading
        // never reads as an unavailable command: 60% is 5.49:1 on surface(),
        // against 3.71:1, and 11px headings need 4.5:1 to clear WCAG AA.
        inline juce::Colour sectionHeader() { return text().withAlpha(0.60f); }

        // Shared interactive roles.
        inline juce::Colour outline() { return text().withAlpha(0.16f); }
        inline juce::Colour hoverFill() { return text().withAlpha(0.07f); }
        inline juce::Colour rowHoverFill() { return text().withAlpha(0.03f); }
        inline juce::Colour accentTint10() { return accent().withAlpha(0.10f); }
        inline juce::Colour accentTint13() { return accent().withAlpha(0.13f); }
        inline juce::Colour accentHover18() { return accent400().withAlpha(0.18f); }

        inline juce::Colour accentGlow() { return accent().withAlpha(0.40f); }

        // Primary (filled) action: Separate control shell, Insert Stems.
        inline juce::Colour primaryFill() { return accent700(); }
        inline juce::Colour primaryFillHover() { return accent600(); }
        inline juce::Colour primaryEdge() { return accent400(); }
        inline juce::Colour primaryText() { return accent100(); }

        // Refine toggle segment.
        inline juce::Colour refineFill() { return accent900(); }
        inline juce::Colour refineFillHover() { return accent800(); }
        inline juce::Colour refineText() { return accent300(); }
        inline juce::Colour refineDivider() { return accent200().withAlpha(0.30f); }
        inline juce::Colour pillTrack() { return accent500(); }
        inline juce::Colour pillTrackOff() { return neutral800(); }
        inline juce::Colour pillKnob() { return accent100(); }

        // Record dot: the accent doubles as the arm/recording indicator.
        inline juce::Colour recordDot() { return accent(); }

        // Stem lanes.
        inline juce::Colour laneWell() { return ground(); }
        inline juce::Colour wavePlayed() { return accent(); }
        inline juce::Colour waveMuted() { return neutral800(); }
        /*  Transcribed notes over a lane's audio. Text rather than accent:
            accent is the playhead and the played waveform, and notes lying
            under both must not be mistaken for either. Translucent so the
            waveform stays readable through them - the notes describe the
            audio, they do not replace it. */
        inline juce::Colour midiOverlay() { return text().withAlpha(0.55f); }

        inline juce::Colour playhead() { return accent(); }
        inline juce::Colour playheadGlow() { return accent().withAlpha(0.35f); }

        inline juce::Colour checkboxFill() { return accent(); }
        inline juce::Colour checkboxCheck() { return ground(); }
        inline juce::Colour checkboxBorder() { return text().withAlpha(0.30f); }

        inline juce::Colour muteActiveFill() { return neutral800(); }
        inline juce::Colour muteActiveText() { return neutral200(); }
        inline juce::Colour soloActiveFill() { return accent800(); }
        inline juce::Colour soloActiveText() { return accent100(); }

        // Transport.
        inline juce::Colour scrubTrack() { return neutral800(); }
        inline juce::Colour scrubFill() { return accent(); }

        // Lighter than the fill it sits on, so the handle reads as a thing on
        // the bar rather than a bulge in it. Same pair as the zoom slider.
        inline juce::Colour scrubKnob() { return accent300(); }

        // Footer.
        inline juce::Colour progressTrack() { return neutral800(); }
        inline juce::Colour progressFill() { return accent(); }
        inline juce::Colour statusCheck() { return accent(); }
        inline juce::Colour statusError() { return juce::Colour(0xffff8a93); }
        inline juce::Colour spinner() { return accent(); }
        inline juce::Colour spinnerTrack() { return accent().withAlpha(0.18f); }
    }

    namespace palette
    {
        /*
            Cross-DAW stem identity colors: the track colors REAPER and
            Ableton create for inserted stems, so a user moving between hosts
            sees the same stem identity.

            Must stay byte-identical with _stem_color in
            integrations/ableton/StemLabRemote/__init__.py - the Remote
            Script cannot include this header. Deliberately independent from
            the interface accent: these belong to the user's DAW project,
            not to StemLab's theme.
        */
        inline std::optional<juce::Colour> stemIdentityColor(const juce::String& stemName)
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

    namespace waveform
    {
        /*
            User-selectable lane waveform colors.

            The redesign shipped with a single accent waveform, which lost
            the one thing color was carrying: which lane you are looking at
            in a tall adaptive tree. Index 0 is that accent look - named
            "Accent" rather than after the design system, because it now
            follows whichever accent is set and calling it Nocturne would
            name it after a colour it may not be. Every palette colors the
            whole lane at full strength - playback position is the
            playhead's job, not a brightness split.

            Beyond the accent and the per-stem identity colors, every
            palette is driven by the audio itself: Spectrum sweeps a hue
            from the spectral centroid, and RGB / 3-Band paint the DJ-
            deck-style low/mid/high balance. The solid single-color fills
            that used to sit here said nothing a lane name did not.

            The chosen index is remembered by name (see the processor's
            waveform-palette preference), so these may be reordered - but
            paletteRgb and paletteThreeBand below are positions the painter
            branches on, and must follow the list if they move.
        */
        constexpr int paletteCount = 5;

        // The two palettes the painter has to treat specially: RGB blends
        // one color per bar from the band balance, 3-Band nests one bar
        // per band.
        constexpr int paletteRgb = 3;
        constexpr int paletteThreeBand = 4;

        inline juce::String paletteName(int index)
        {
            static const char* const names[paletteCount] = {
                "Accent", "Stem Color", "Spectrum", "RGB", "3-Band"};

            return names[juce::jlimit(0, paletteCount - 1, index)];
        }

        /**
         * The played-portion color of one waveform bar.
         *
         * @param index      selected palette
         * @param stemName   identity key ("vocals"...) for Stem Color
         * @param brightness 0..1 spectral brightness of the audio under this
         *                   bar, for Spectrum; 0.5 means "not analysed yet"
         */
        inline juce::Colour playedColor(int index, const juce::String& stemName,
                                         float brightness)
        {
            switch (juce::jlimit(0, paletteCount - 1, index))
            {
            case 1:
                return palette::stemIdentityColor(stemName).value_or(colors::accent());

            case 2:
                /*
                    Violet where the audio's spectral centroid sits low and
                    amber where it sits high, so a bass lane reads violet and
                    a hi-hat lane reads amber.

                    This used to be driven by the bar's position across the
                    lane, which looked like a spectrum and meant nothing: a
                    sine tone and a drum loop came out identically colored.
                    The value now comes from stemlab::waveform::brightnessAt
                    over a real FFT of the file.

                    Saturation and value are fixed so no bar can land on an
                    unreadable color, whatever the audio does.
                */
                return juce::Colour::fromHSV(
                    0.72f - 0.62f * juce::jlimit(0.0f, 1.0f, brightness), 0.55f, 0.98f, 1.0f);

            case 0:
            default:
                return colors::wavePlayed();
            }
        }

        /*
            rekordbox-style RGB: the low band drives blue, the mid green,
            the high red, so a kick reads blue, a vocal reads green into
            yellow, and a full mix washes toward white. The gamma lifts the
            quieter bands - band shares of a real mix rarely pass 0.5, and
            linearly that would leave every bar a saturated primary.
        */
        inline juce::Colour rgbColor(float low, float mid, float high)
        {
            const auto lift = [](float share)
            { return std::pow(juce::jlimit(0.0f, 1.0f, share), 0.6f); };

            return juce::Colour::fromFloatRGBA(lift(high), lift(mid), lift(low), 1.0f);
        }

        // 3-Band's fixed band colors, rekordbox-style: blue lows, amber
        // mids, near-white highs.
        inline juce::Colour bandLowColor() { return juce::Colour(0xff4472ff); }
        inline juce::Colour bandMidColor() { return juce::Colour(0xffffb454); }
        inline juce::Colour bandHighColor() { return juce::Colour(0xfff0f4ff); }
    }

    namespace fonts
    {
        /*
            The bundled Inter faces, registered once by StemLabLookAndFeel's
            constructor. Every token below carries the typeface explicitly:
            JUCE 9's font resolution does not consult
            LookAndFeel::getTypefaceForFont, so a FontOptions without a
            typeface would silently render in the platform fallback.

            Weight 500 ("medium: true") uses Inter Medium - per Nocturne,
            nothing renders bolder than 500.
        */
        inline juce::Typeface::Ptr& regularTypeface()
        {
            static juce::Typeface::Ptr typeface;
            return typeface;
        }

        inline juce::Typeface::Ptr& mediumTypeface()
        {
            static juce::Typeface::Ptr typeface;
            return typeface;
        }

        inline juce::FontOptions make(float size, bool medium)
        {
            auto& typeface = medium ? mediumTypeface() : regularTypeface();

            if (typeface != nullptr)
                return juce::FontOptions(typeface).withHeight(size);

            return juce::FontOptions(size, medium ? juce::Font::bold : juce::Font::plain);
        }

        // "StemLab" wordmark: 17/500 with -0.015em letter-spacing.
        constexpr float titleKerning = -0.015f;
        inline juce::FontOptions title() { return make(17.0f, true); }

        inline juce::FontOptions body() { return make(13.0f, false); }
        inline juce::FontOptions bodyMedium() { return make(13.0f, true); }

        inline juce::FontOptions laneName() { return make(13.5f, true); }

        inline juce::FontOptions separateLabel() { return make(13.5f, true); }

        inline juce::FontOptions refineLabel() { return make(11.0f, false); }

        inline juce::FontOptions meta() { return make(11.0f, false); }
        inline juce::FontOptions status() { return make(12.5f, false); }
        inline juce::FontOptions progress() { return make(12.0f, false); }
        inline juce::FontOptions time() { return make(12.0f, false); }
        inline juce::FontOptions footerPath() { return make(12.0f, false); }
        inline juce::FontOptions smallButton() { return make(10.0f, false); }

        // Bar numbers on the lane grid: small and quiet, since they are a
        // ruler behind the audio rather than something to read.
        inline juce::FontOptions gridLabel() { return make(9.0f, false); }
        inline juce::FontOptions tooltip() { return make(12.0f, false); }
    }

    namespace metrics
    {
        namespace window
        {
            /*
                The surface runs edge to edge: no ground margin, no rounded
                corners, no shadow. Floating the panel on a ground inset made
                sense in a mock; in a host's FX window it is a dead border
                around the only thing on screen.

                880x564 is the content stack exactly - panel::padY either
                side of a header, source strip, six lanes, transport and
                footer. The six root lanes and the lanes viewport are the
                same height BY CONSTRUCTION: whatever the chrome does not
                use, wellHeight absorbs, so there is never a dead band
                between the last lane and the transport. Re-run the sum
                after touching any vertical metric. Adaptive child lanes
                scroll within the lanes region rather than growing the
                window.
            */
            constexpr int groundMargin = 0;
            constexpr int width = 880 + 2 * groundMargin;
            constexpr int height = 564 + 2 * groundMargin;

            /*
                The window resizes, but the layout does not reflow: the whole
                panel is drawn at the size above and scaled by one transform,
                between these bounds. The window is free to take any shape;
                the panel is not, so a lane's proportions are identical at
                every size and an off-shape window letterboxes the panel
                rather than stretching it. These two are the range of that
                scale, not window sizes.
            */
            constexpr double minScale = 0.70;
            constexpr double maxScale = 2.50;
        }

        namespace panel
        {
            constexpr int padX = 22;
            constexpr int padY = 14;
            constexpr int stackGap = 10;
        }

        namespace header
        {
            constexpr int height = 30;
            constexpr int glyphSize = 20;
            constexpr int glyphGap = 10;
            constexpr int settingsButton = 32;
            constexpr float settingsRadius = 8.0f;
            constexpr int settingsIcon = 16;

            /*
                The separation model and the waveform palette sit in the
                header rather than three levels down a settings menu: they
                are choices made while working, and the model in particular
                belongs next to the Separate button that runs it.

                    < [ * Hybrid v ] >   (palette)   (settings)
            */
            constexpr int selectorHeight = 30;
            constexpr float selectorRadius = 8.0f;
            constexpr int selectorPadX = 10;
            constexpr int selectorIcon = 13;
            constexpr int selectorGap = 7;
            constexpr int selectorCaret = 8;
            constexpr int selectorMinWidth = 104;

            // The step arrows either side of the selector.
            constexpr int stepButton = 20;
            constexpr int stepIcon = 9;

            // Between the selector group, the palette, and the settings icon.
            constexpr int groupGap = 10;
            // Square, and the same square as the settings icon beside it:
            // the two sit side by side and their hover backgrounds have to
            // match. The glyph stays a shade smaller, since a filled
            // palette reads heavier than the settings icon's strokes.
            constexpr int paletteButton = settingsButton;
            constexpr int paletteIcon = 15;

            /*
                Which stems the job carries forward is a per-lane checkbox,
                which is fine for one change and tedious for six. The two
                pills act on every lane at once, and the readout to their
                left answers for whatever the user changed last - model,
                palette, transport, a rejected click - before settling back
                on where the selection stands:

                    5 of 6 stems will be saved  [Select all] [Deselect all]

                The bottom status line is the other half of that split: it
                reports only the work the plugin is doing.
            */
            constexpr int selectButtonHeight = 22;
            constexpr int selectButtonPadX = 12;
            constexpr int selectButtonGap = 6;
            constexpr int selectCountGap = 10;
            constexpr int userStatusHeight = 16;

            /*
                Waveform zoom, sitting between that group and the model:

                    (o) [-----O------------]  4x

                The lanes draw a window of the file rather than the whole of
                it, so the readout is a multiplier, not a duration.
            */
            constexpr int zoomIcon = 15;
            constexpr int zoomIconGap = 7;
            constexpr int zoomTrackWidth = 92;
            constexpr int zoomTrackHeight = 4;
            constexpr int zoomKnob = 13;
            constexpr int zoomLabelGap = 8;
            constexpr int zoomLabelWidth = 26;
        }

        namespace source
        {
            constexpr float radius = 8.0f;
            constexpr int padX = 12;
            constexpr int padY = 8;
            constexpr int gap = 12;
            constexpr int height = 52;

            constexpr int captureButtonWidth = 132;
            constexpr int recordButtonWidth = 108;
            constexpr int recordDot = 10;

            /*
                Analyse and Set tempo, which share the left of the strip with
                the source's name and meta line.

                Tighter than the buttons to their right because they are not
                competing with them for attention - they act on the source
                already named beside them - and because that side of the
                strip is what shrinks first as the window narrows.

                metaMinWidth is the floor the text keeps for itself. Without
                it a long enough label would take the whole line and leave
                the source with no name at all.
            */
            constexpr int inlineButtonPadX = 20;
            constexpr int inlineButtonMinWidth = 58;
            constexpr int inlineButtonMaxWidth = 180;
            constexpr int metaMinWidth = 48;

            /*
                groupButtonGap is the gap between two buttons that belong
                together, anywhere in this strip - the Analyse/Set BPM pair
                and the Import/Record pair alike. It is tighter than the gap
                to whatever is outside the group:

                    song.wav  [Analyse][Set BPM] | [Import from DAW][Record PC] | [Refine|Separate]

                The first pair acts on the source named to their left; the
                second picks a different source; the last consumes it.
                Grouping them by spacing says which is which before any of
                the labels are read.
            */
            constexpr int groupButtonGap = 8;

            /*
                A hairline in the gap before the Separate control, splitting
                the strip into the sources you can pick and the action that
                consumes one:

                    song.wav   [Import from DAW] [Record PC] | [Refine|Separate]

                Shorter than the buttons either side, so it reads as a
                separator rather than a fourth control.
            */
            constexpr int dividerWidth = 1;
            constexpr int dividerHeight = 22;

            /*
                Every hairline in the strip is centred in dividerSpan, so the
                space either side of one is the same wherever it appears.
                Wider than the gap between two buttons of the same group,
                which is what makes it read as a boundary rather than as one
                more gap.
            */
            constexpr int dividerSpan = 20;

            /*
                The Separate split control.

                A quarter of the strip rather than a third, and a floor low
                enough to matter at the window's minimum width: the control
                is one word and a toggle, and every pixel it was holding
                beyond that came out of the source's own name - the one part
                of this strip whose width is not the designer's to choose.

                The floor is what "Refine [pill] | Separate" measures plus
                room for the longer of the two labels, so the text inside it
                never has to elide.
            */
            constexpr int separateMinWidth = 216;
            constexpr int separateWidthDivisor = 4;
            constexpr int separateHeight = 36;
            constexpr float separateRadius = 8.0f;
            constexpr int refinePadLeft = 12;
            constexpr int refinePadRight = 14;
            constexpr int pillWidth = 22;
            constexpr int pillHeight = 12;
            constexpr int pillKnob = 8;
            constexpr int pillGap = 8;
        }

        namespace lanes
        {
            // Grid per row: twisty | include | name | waveform | controls.
            constexpr int twistyColumn = 16;
            constexpr int twistyIcon = 11;

            // The twisty used to butt straight up against the checkbox.
            constexpr int twistyGap = 6;

            // A collapsed row whose hidden descendants are soloed or muted
            // carries this dot in the gap beside its twisty, so the state
            // does not vanish with the rows. Fits the 6px gap with a pixel
            // of air on each side.
            constexpr float hiddenActivityDot = 4.0f;

            constexpr int includeColumn = 18;
            constexpr int nameColumn = 92;

            // S, M and the menu. The drag handle sits on the other side of
            // the waveform, next to the name it belongs to.
            constexpr int controlsColumn = 78;

            // Between the drag handle and the waveform it drags.
            constexpr int dragGap = 8;
            constexpr int columnGap = 12;

            // The wells sit close together: this is the whole gap between one
            // lane's waveform and the next, halved.
            constexpr int rowPadY = 1;
            constexpr float rowRadius = 6.0f;
            constexpr int wellHeight = 54;
            constexpr float wellRadius = 6.0f;

            constexpr int checkbox = 15;
            constexpr float checkboxRadius = 4.0f;
            constexpr float checkboxBorder = 1.5f;

            constexpr int smButton = 22;
            constexpr float smRadius = 6.0f;
            constexpr int smGap = 6;
            constexpr int layersIcon = 14;

            // The well's inset: the waveform draws inside this, and clicks
            // are measured against the same rectangle.
            constexpr float wellPadX = 6.0f;
            constexpr float wellPadY = 5.0f;

            // Stereo draws as two half-height waveforms with this between:
            // enough to read as two channels, not enough to waste the well.
            constexpr float channelGap = 1.0f;

            // Silence still draws a hairline, so an empty stem reads as flat
            // rather than as a lane that failed to load.
            constexpr float waveMinHeight = 1.0f;

            // Retained for the reference layout; the lanes now draw one
            // column per pixel from a peak envelope rather than these bars.
            constexpr float barWidth = 2.0f;
            constexpr float barPitch = 4.0f;
            constexpr float barMinHeight = 2.0f;
            constexpr float playheadWidth = 1.0f;
            constexpr float playheadGlowWidth = 8.0f;

            constexpr float excludedOpacity = 0.45f;

            // Adaptive child lanes indent under their root.
            constexpr int childIndent = 26;

            constexpr int scrollbarThickness = 8;

            // Bar-number labels on the beat grid. Below this spacing the
            // numbers would run into each other, so the ruler thins out to
            // every 2nd, 4th, 8th bar instead of crowding.
            constexpr float gridLabelMinSpacing = 42.0f;
            constexpr float gridLabelWidth = 26.0f;
            constexpr float gridLabelHeight = 11.0f;

            // Each number sits on a small plate of the well's own ground so
            // it keeps the contrast it was measured for whatever the audio
            // under it is doing. On a quiet lane the plate is the color
            // already there, so nothing shows; on a loud one it is the only
            // reason the number is still legible.
            constexpr float gridLabelPlateAlpha = 0.90f;
            constexpr float gridLabelPlateRadius = 2.0f;
            constexpr float gridLabelPlatePadding = 2.5f;
        }

        namespace transport
        {
            constexpr int height = 34;
            constexpr int playButton = 34;
            constexpr int gap = 14;
            constexpr int timeWidth = 92;
            constexpr int scrubHeight = 3;
            constexpr float scrubRadius = 2.0f;

            /*  The handle. Wider than the 3px track on purpose: the track is
                a readout and the handle is a target, and 11px is the smallest
                dot that still reads as one and can be grabbed without aiming.
                Two more on hover, so it answers the pointer before the drag.
            */
            constexpr int scrubKnob = 11;
            constexpr int scrubKnobHover = 13;
            constexpr int abWidth = 150;
            constexpr int abHeight = 28;
            constexpr float abRadius = 8.0f;

            // The selected option is a closed pill inset inside the shell.
            constexpr float abInset = 3.0f;
        }

        namespace footer
        {
            constexpr int dividerFade = 48;
            constexpr int dividerGap = 8;
            constexpr int height = 34;
            constexpr int gap = 10;
            /*
                The status block shifts down by statusTopInset while the
                progress row shows: the thin track sits centred in its row,
                so without the shift the text's clearance above is visibly
                smaller than the track's clearance below. The readout's
                descender dips past the footer row into the panel padding,
                which is empty.
            */
            constexpr int statusTopInset = 2;
            constexpr int statusLineHeight = 16;
            constexpr int statusLineGap = 4;
            constexpr int statusTextGap = 6;
            constexpr int statusRightMargin = 28;
            constexpr float progressHeight = 3.0f;
            constexpr int progressRowHeight = 14;
            constexpr int progressBarWidth = 320;
            constexpr int progressLabelGap = 10;
            constexpr int folderIcon = 14;
            constexpr int folderIconGap = 6;
            constexpr int pathWidth = 170;
            constexpr int changeWidth = 56;
            constexpr int retryWidth = 60;
            constexpr int saveWidth = 96;
            constexpr int insertWidth = 108;
            constexpr int buttonHeight = 30;
        }

        namespace menu
        {
            /*
                Popup menus are drawn by StemLabLookAndFeel rather than left
                to JUCE's stock look: a square surface card, rows that
                highlight as inset pills, and the same check glyph the
                include checkboxes use.
            */
            constexpr int borderSize = 4;

            constexpr int rowHeight = 23;
            constexpr int rowInsetX = 4;
            constexpr int rowInsetY = 1;
            constexpr float rowRadius = 6.0f;

            // Left gutter for the tick, right gutter for a submenu arrow.
            // Both are reserved on every row so labels line up.
            constexpr int tickColumn = 16;
            constexpr int tickIcon = 11;
            constexpr int tickGap = 6;
            constexpr int trailingColumn = 12;
            constexpr int submenuArrow = 8;

            constexpr int padX = 8;
            constexpr int separatorHeight = 7;
            constexpr int sectionHeaderHeight = 22;
        }

        namespace buttons
        {
            constexpr float radius = 8.0f;
            constexpr int height = 30;
            constexpr int padX = 12;
        }

        namespace waveform
        {
            // Thumbnail resolution/cache for the lane previews.
            constexpr int thumbnailResolution = 512;
            constexpr int thumbnailCacheSize = 24;

            // Below this many pixels of travel a gesture is a seek click;
            // beyond it, an external file drag.
            constexpr int clickVersusDragThreshold = 8;
        }

        constexpr float disabledOpacity = 0.45f;

        /*
            Disabled foregrounds are floored. Several roles are themselves
            alpha tokens - text45, text50, outline - so a flat 0.45 multiply
            lands them near alpha 0.2, which reads as absent rather than as
            inactive; the magnifier glyph sat at 1.8:1 against the panel.

            The ceiling is what keeps the floor honest. Without it any token
            quieter than the floor - outline, at 0.16 - would be raised to
            0.34 and render BRIGHTER dead than alive. Capping at 85% of the
            live alpha guarantees every role still steps down.
        */
        constexpr float disabledAlphaFloor = 0.34f;
        constexpr float disabledAlphaCeiling = 0.85f;

        // Retune these together: a floor at or above the multiply would make
        // every fully opaque token brighter disabled than enabled.
        static_assert(disabledAlphaFloor < disabledOpacity,
                      "the disabled alpha floor must sit below disabledOpacity");

        // The editor repaints lanes and re-polls processor state at this rate
        // while anything can change on its own: a job narrating, the transport
        // moving, a timed readout still counting down.
        constexpr int uiRefreshHz = 20;

        /*
            ...and at this rate once nothing can. Almost every user action
            already calls refreshFromProcessor() inside its own handler, so
            the idle tick is only catching what the editor is never told
            about; half a second of latency on that is invisible, and it
            removes 90% of the wakeups an open-but-idle window was costing.
        */
        constexpr int uiIdleRefreshHz = 2;

        // Full rate is held this long past the last reason for it, so a
        // stream of events cannot thrash the timer between the two rates
        // and a reason that flickers off for one tick does not demote.
        constexpr int uiIdleHoldMs = 1500;
    }

    /*
        colors reopened after metrics: dimming is a color role and belongs
        beside the tokens it operates on, but it reads alpha constants that
        metrics does not declare until above.
    */
    namespace colors
    {
        /** The one way to dim a color for a disabled control. A
            full-strength color loses 55% exactly as it always has; a color
            that already carries alpha is floored so it stays legible, and
            capped so it still reads weaker than its live self. */
        inline juce::Colour dimDisabled(juce::Colour color)
        {
            const auto alpha = color.getFloatAlpha();

            return color.withAlpha(
                juce::jmin(alpha * metrics::disabledAlphaCeiling,
                           juce::jmax(alpha * metrics::disabledOpacity,
                                      metrics::disabledAlphaFloor)));
        }

        /** Paint-code sugar, so a widget can route a token through the dim
            without branching at every call. */
        inline juce::Colour dimIfDisabled(juce::Colour color, bool enabled)
        {
            return enabled ? color : dimDisabled(color);
        }
    }
}
