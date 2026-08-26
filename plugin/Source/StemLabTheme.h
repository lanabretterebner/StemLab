#pragma once

#include <JuceHeader.h>

#include <optional>

/*
    Every visual decision StemLab's interface makes, in one place: the
    "Nocturne" design system, draft 1a "Lanes". The design handoff this was
    built from is no longer in the repository, so these values are the only
    record of the tokens - treat this file as the spec, not as a copy of one.

    The editor deliberately contains no colour or font literals; layout
    values are tokens apart from a few small one-off trims at their call
    sites. Restyle by changing values here and in the drawing code of
    StemLabLookAndFeel / StemLabWidgets; only re-arrangements of the
    interface itself touch the editor.

        colours   Ground/surface/text, the accent and neutral ramps, and
                  per-component colour roles.
        palette   The cross-DAW stem identity colours shared with the
                  Ableton Remote Script.
        fonts     Inter-based type scale (weight 500 is expressed as
                  juce::Font::bold and resolved to Inter Medium by
                  StemLabLookAndFeel; nothing renders bolder than 500).
        metrics   Layout dimensions of the Lanes panel, at the design
                  size the whole panel is scaled from.
*/

namespace stemlab::theme
{
    namespace colours
    {
        // Nocturne ground and surface.
        inline juce::Colour ground() { return juce::Colour(0xff161826); }
        inline juce::Colour surface() { return juce::Colour(0xff232532); }

        inline juce::Colour text() { return juce::Colour(0xffe9e9ed); }
        inline juce::Colour divider() { return text().withAlpha(0.16f); }

        // The one blurple accent, used as lines, tints, and glows.
        inline juce::Colour accent() { return juce::Colour(0xff9184d9); }

        // Accent ramp (OKLCH-generated in the token sheet).
        inline juce::Colour accent100() { return juce::Colour(0xfff5f4ff); }
        inline juce::Colour accent200() { return juce::Colour(0xffe7e5fe); }
        inline juce::Colour accent300() { return juce::Colour(0xffd2cefd); }
        inline juce::Colour accent400() { return juce::Colour(0xffb5abfc); }
        inline juce::Colour accent500() { return juce::Colour(0xff968ae0); }
        inline juce::Colour accent600() { return juce::Colour(0xff796cbf); }
        inline juce::Colour accent700() { return juce::Colour(0xff5d5294); }
        inline juce::Colour accent800() { return juce::Colour(0xff423a6a); }
        inline juce::Colour accent900() { return juce::Colour(0xff2b2741); }

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
        inline juce::Colour waveUnplayed() { return neutral700(); }
        inline juce::Colour wavePlayed() { return accent(); }
        inline juce::Colour waveMuted() { return neutral800(); }
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

        // Footer.
        inline juce::Colour progressTrack() { return neutral800(); }
        inline juce::Colour progressFill() { return accent(); }
        inline juce::Colour statusCheck() { return accent(); }
    }

    namespace palette
    {
        /*
            Cross-DAW stem identity colours: the track colours REAPER and
            Ableton create for inserted stems, so a user moving between hosts
            sees the same stem identity.

            Must stay byte-identical with _stem_color in
            integrations/ableton/StemLabRemote/__init__.py - the Remote
            Script cannot include this header. Deliberately independent from
            the interface accent: these belong to the user's DAW project,
            not to StemLab's theme.
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

    namespace waveform
    {
        /*
            User-selectable lane waveform colours.

            The redesign shipped with a single accent waveform, which lost
            the one thing colour was carrying: which lane you are looking at
            in a tall adaptive tree. Index 0 is that accent look, unchanged;
            the rest colour the whole lane, dimming the unplayed portion of
            their own hue instead of falling back to neutral.

            The index persists in plugin state, so the order of these must
            stay stable.
        */
        constexpr int paletteCount = 7;

        inline juce::String paletteName(int index)
        {
            static const char* const names[paletteCount] = {
                "Nocturne Accent", "Stem Colours", "Spectrum",
                "Solid Blue",      "Solid Green",  "Solid Amber",
                "Solid Magenta"};

            return names[juce::jlimit(0, paletteCount - 1, index)];
        }

        /**
         * The played-portion colour of one waveform bar.
         *
         * @param index          selected palette
         * @param stemName       identity key ("vocals"...) for Stem Colours
         * @param positionAcross 0..1 across the lane, for the spectrum sweep
         */
        inline juce::Colour playedColour(int index, const juce::String& stemName,
                                         float positionAcross)
        {
            switch (juce::jlimit(0, paletteCount - 1, index))
            {
            case 1:
                return palette::stemIdentityColour(stemName).value_or(colours::accent());

            case 2:
                // One sweep from violet to amber, so a lane reads left to
                // right without any bar dropping to an unreadable value.
                return juce::Colour::fromHSV(
                    0.72f - 0.62f * juce::jlimit(0.0f, 1.0f, positionAcross), 0.55f, 0.98f,
                    1.0f);

            case 3:
                return juce::Colour(0xff4ea8ff);
            case 4:
                return juce::Colour(0xff46e797);
            case 5:
                return juce::Colour(0xffffb454);
            case 6:
                return juce::Colour(0xffef6bb4);

            case 0:
            default:
                return colours::wavePlayed();
            }
        }

        inline juce::Colour unplayedColour(int index, const juce::String& stemName,
                                           float positionAcross)
        {
            if (juce::jlimit(0, paletteCount - 1, index) == 0)
                return colours::waveUnplayed();

            return playedColour(index, stemName, positionAcross)
                .withMultipliedSaturation(0.55f)
                .withMultipliedBrightness(0.45f);
        }
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
        inline juce::FontOptions status() { return make(11.5f, false); }
        inline juce::FontOptions time() { return make(12.0f, false); }
        inline juce::FontOptions footerPath() { return make(12.0f, false); }
        inline juce::FontOptions smallButton() { return make(10.0f, false); }
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
                footer. Adaptive child lanes scroll within the lanes region
                rather than growing the window.
            */
            constexpr int groundMargin = 0;
            constexpr int width = 880 + 2 * groundMargin;
            constexpr int height = 564 + 2 * groundMargin;

            /*
                The window resizes, but the layout does not reflow: the whole
                panel is drawn at the size above and scaled by one transform,
                between these bounds. The aspect ratio is fixed, so a lane's
                proportions are identical at every size.
            */
            constexpr double minScale = 0.70;
            constexpr double maxScale = 2.50;
        }

        namespace panel
        {
            constexpr int padX = 22;
            constexpr int padY = 20;
            constexpr int stackGap = 14;
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
            constexpr int paletteButton = 30;
            constexpr int paletteIcon = 15;

            /*
                Which stems the job carries forward is a per-lane checkbox,
                which is fine for one change and tedious for six. The two
                pills act on every lane at once and the readout beside them
                says where that left things:

                    [Select all] [Deselect all]  5 of 6 selected
            */
            constexpr int selectButtonHeight = 22;
            constexpr int selectButtonPadX = 12;
            constexpr int selectButtonGap = 6;
            constexpr int selectCountGap = 10;

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
            constexpr int padY = 10;
            constexpr int gap = 12;
            constexpr int height = 56;

            constexpr int captureButtonWidth = 132;
            constexpr int recordButtonWidth = 108;
            constexpr int recordDot = 10;

            // The Separate split control.
            constexpr int separateMinWidth = 260;
            constexpr int separateHeight = 36;
            constexpr int separateExtraLeftGap = 8;
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
            constexpr int twistyColumn = 14;
            constexpr int twistyIcon = 9;
            constexpr int includeColumn = 18;
            constexpr int nameColumn = 92;
            constexpr int controlsColumn = 78;
            constexpr int columnGap = 12;

            constexpr int rowPadY = 5;
            constexpr float rowRadius = 6.0f;
            constexpr int wellHeight = 40;
            constexpr float wellRadius = 6.0f;

            constexpr int checkbox = 15;
            constexpr float checkboxRadius = 4.0f;
            constexpr float checkboxBorder = 1.5f;

            constexpr int smButton = 22;
            constexpr float smRadius = 6.0f;
            constexpr int smGap = 6;
            constexpr int layersIcon = 14;

            // Waveform bars: 2px rounded bars on a ~4px pitch (approx. 150
            // bars across the reference 612px well).
            constexpr float barWidth = 2.0f;
            constexpr float barPitch = 4.0f;
            constexpr float barMinHeight = 2.0f;
            constexpr float playheadWidth = 1.0f;
            constexpr float playheadGlowWidth = 8.0f;

            constexpr float excludedOpacity = 0.45f;

            // Adaptive child lanes indent under their root.
            constexpr int childIndent = 26;

            constexpr int scrollbarThickness = 8;
        }

        namespace transport
        {
            constexpr int height = 34;
            constexpr int playButton = 34;
            constexpr int gap = 14;
            constexpr int timeWidth = 92;
            constexpr int scrubHeight = 3;
            constexpr float scrubRadius = 2.0f;
            constexpr int abWidth = 150;
            constexpr int abHeight = 28;
            constexpr float abRadius = 8.0f;

            // The selected option is a closed pill inset inside the shell.
            constexpr float abInset = 3.0f;
        }

        namespace footer
        {
            constexpr int dividerFade = 48;
            constexpr int dividerGap = 12;
            constexpr int height = 34;
            constexpr int gap = 10;
            constexpr int statusLineHeight = 14;
            constexpr int statusLineGap = 6;
            constexpr int statusRightMargin = 28;
            constexpr float progressHeight = 3.0f;
            constexpr int progressRowHeight = 12;
            constexpr int progressLabelWidth = 110;
            constexpr int progressLabelGap = 8;
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
                to JUCE's stock look: a rounded surface card, rows that
                highlight as inset pills, and the same check glyph the
                include checkboxes use.
            */
            constexpr float radius = 10.0f;
            constexpr int borderSize = 6;

            constexpr int rowHeight = 26;
            constexpr int rowInsetX = 4;
            constexpr int rowInsetY = 1;
            constexpr float rowRadius = 6.0f;

            // Left gutter for the tick, right gutter for a submenu arrow or
            // a shortcut. Both are reserved on every row so labels line up.
            constexpr int tickColumn = 20;
            constexpr int tickIcon = 11;
            constexpr int tickGap = 8;
            constexpr int trailingColumn = 24;
            constexpr int submenuArrow = 8;

            constexpr int padX = 10;
            constexpr int separatorHeight = 9;
            constexpr int sectionHeaderHeight = 26;
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

        // The editor repaints lanes and re-polls processor state at this rate.
        constexpr int uiRefreshHz = 20;
    }
}
