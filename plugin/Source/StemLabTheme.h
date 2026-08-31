#pragma once

#include <JuceHeader.h>

/*
    Every visual decision the interface makes, in one place.

    The editor holds no colour, font or radius literals: it asks for a role
    ("the primary fill", "the lane playhead") and this file decides what that
    looks like. Restyling happens here and in StemLabLookAndFeel's drawing
    code; only re-arrangements of the interface itself touch the editor.

        colours   Ground and surface, the accent and neutral ramps, and the
                  per-component colour roles built on them.
        fonts     The type scale.
        metrics   Radii and dimensions the look-and-feel draws to.
*/

namespace stemlab::theme
{
    namespace colours
    {
        // Ground and surface.
        inline juce::Colour ground() { return juce::Colour(0xff161826); }
        inline juce::Colour surface() { return juce::Colour(0xff232532); }

        inline juce::Colour text() { return juce::Colour(0xffe9e9ed); }
        inline juce::Colour divider() { return text().withAlpha(0.16f); }

        /*
            The accent is the amber the product's own icon is drawn in -
            #FCB901, which is 63% of that image's opaque pixels and the only
            colour in it besides black. It reaches 10.1:1 on ground(), where
            the violet it replaces reached 4.0:1.

            Amber does not behave like a violet down the ramp. The light end
            carries the brand; below accent500 the chroma is pulled hard,
            because holding it would turn the shadows olive rather than
            making them a deeper amber. Those steps are warm neutrals, and
            are meant to be.
        */
        // Accent ramp. Lightness steps are even in OKLCH, and accent400 is
        // the icon's colour exactly rather than something near it.
        inline juce::Colour accent100() { return juce::Colour(0xfffbf5ea); }
        inline juce::Colour accent200() { return juce::Colour(0xfffae6c0); }
        inline juce::Colour accent300() { return juce::Colour(0xfffccc73); }
        inline juce::Colour accent400() { return juce::Colour(0xfffcb901); }
        inline juce::Colour accent500() { return juce::Colour(0xffd8a020); }
        inline juce::Colour accent600() { return juce::Colour(0xff857963); }
        inline juce::Colour accent700() { return juce::Colour(0xff655d4d); }
        inline juce::Colour accent800() { return juce::Colour(0xff474237); }
        inline juce::Colour accent900() { return juce::Colour(0xff2f2b24); }

        // The brand colour itself, named for the role rather than the step.
        inline juce::Colour accent() { return accent400(); }

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

        // Muted text is the body colour at a per-component opacity.
        inline juce::Colour text75() { return text().withAlpha(0.75f); }
        inline juce::Colour text50() { return text().withAlpha(0.50f); }
        inline juce::Colour text45() { return text().withAlpha(0.45f); }
        inline juce::Colour textMuted() { return text().withAlpha(0.62f); }

        // Menu section headings, deliberately above the disabled-item colour
        // so a heading never reads as an unavailable command.
        inline juce::Colour sectionHeader() { return text().withAlpha(0.60f); }

        // Shared interactive roles.
        inline juce::Colour outline() { return text().withAlpha(0.16f); }
        inline juce::Colour hoverFill() { return text().withAlpha(0.07f); }
        inline juce::Colour rowHoverFill() { return text().withAlpha(0.03f); }
        inline juce::Colour accentTint10() { return accent().withAlpha(0.10f); }
        inline juce::Colour accentTint13() { return accent().withAlpha(0.13f); }
        inline juce::Colour accentHover18() { return accent300().withAlpha(0.18f); }
        inline juce::Colour accentGlow() { return accent().withAlpha(0.40f); }

        /*
            Primary (filled) action.

            Light-on-dark, which every other filled role here uses, is the
            one arrangement this accent cannot take: white on #FCB901 is
            1.7:1, which is unreadable. So the primary action inverts - the
            fill is the brand amber at full strength and the label is the
            ground it sits on, which measures 10.1:1. It is also the more
            honest button: the primary action is the one place the product's
            own colour should be unmissable.
        */
        inline juce::Colour primaryFill() { return accent400(); }
        inline juce::Colour primaryFillHover() { return accent300(); }
        inline juce::Colour primaryEdge() { return accent200(); }
        inline juce::Colour primaryText() { return ground(); }

        // Secondary / toggle segment: dark fill, amber label.
        inline juce::Colour refineFill() { return accent900(); }
        inline juce::Colour refineFillHover() { return accent800(); }
        inline juce::Colour refineText() { return accent300(); }
        inline juce::Colour refineDivider() { return accent200().withAlpha(0.30f); }

        inline juce::Colour pillTrack() { return accent500(); }
        inline juce::Colour pillTrackOff() { return neutral800(); }
        inline juce::Colour pillKnob() { return accent100(); }

        // Destructive action. Kept off the accent ramp: cancel and error
        // must not read as the brand.
        inline juce::Colour dangerFill() { return juce::Colour(0xffa8364a); }
        inline juce::Colour dangerText() { return juce::Colour(0xfffde7ea); }

        // The accent doubles as the arm/recording indicator.
        inline juce::Colour recordDot() { return accent(); }

        // Stem lanes.
        inline juce::Colour laneWell() { return ground(); }
        inline juce::Colour wavePlayed() { return accent(); }
        inline juce::Colour waveMuted() { return neutral800(); }
        inline juce::Colour playhead() { return accent(); }
        inline juce::Colour playheadGlow() { return accent().withAlpha(0.35f); }
        inline juce::Colour selectionFill() { return accent().withAlpha(0.20f); }
        inline juce::Colour selectionEdge() { return accent().withAlpha(0.90f); }

        // A tick drawn in the ground colour, over an accent fill.
        inline juce::Colour checkboxFill() { return accent(); }
        inline juce::Colour checkboxCheck() { return ground(); }
        inline juce::Colour checkboxBorder() { return text().withAlpha(0.30f); }

        inline juce::Colour muteActiveFill() { return neutral800(); }
        inline juce::Colour muteActiveText() { return neutral200(); }
        inline juce::Colour soloActiveFill() { return accent800(); }
        inline juce::Colour soloActiveText() { return accent100(); }

        // Transport and footer.
        inline juce::Colour scrubTrack() { return neutral800(); }
        inline juce::Colour scrubFill() { return accent(); }
        inline juce::Colour progressTrack() { return neutral800(); }
        inline juce::Colour progressFill() { return accent(); }
        inline juce::Colour statusCheck() { return accent(); }
        inline juce::Colour statusError() { return juce::Colour(0xffff8a93); }
        inline juce::Colour spinner() { return accent(); }
        inline juce::Colour spinnerTrack() { return accent().withAlpha(0.18f); }
    }

    namespace fonts
    {
        /*
            The bundled Inter faces, registered once by StemLabLookAndFeel's
            constructor. Every token carries the typeface explicitly: JUCE 9's
            font resolution does not consult LookAndFeel::getTypefaceForFont,
            so a FontOptions without one renders in the platform fallback.

            When the faces are not available - a build without the binary
            resources - make() returns a plain FontOptions and the interface
            renders in the host's default face rather than failing.
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

        // Wordmark: 17/500 with -0.015em letter-spacing.
        constexpr float titleKerning = -0.015f;
        inline juce::FontOptions title() { return make(17.0f, true); }

        inline juce::FontOptions body() { return make(13.0f, false); }
        inline juce::FontOptions bodyMedium() { return make(13.0f, true); }
        inline juce::FontOptions laneName() { return make(13.5f, true); }
        inline juce::FontOptions buttonLabel() { return make(13.0f, true); }
        inline juce::FontOptions meta() { return make(11.0f, false); }
        inline juce::FontOptions status() { return make(12.5f, false); }
        inline juce::FontOptions progress() { return make(12.0f, false); }
        inline juce::FontOptions time() { return make(12.0f, false); }
        inline juce::FontOptions smallButton() { return make(10.0f, false); }
        inline juce::FontOptions gridLabel() { return make(9.0f, false); }
        inline juce::FontOptions tooltip() { return make(12.0f, false); }
    }

    namespace metrics
    {
        // A disabled control keeps its colour and loses strength.
        constexpr float disabledOpacity = 0.45f;
        constexpr float disabledAlphaFloor = 0.34f;
        constexpr float disabledAlphaCeiling = 0.85f;

        static_assert(disabledAlphaFloor < disabledOpacity,
                      "the disabled alpha floor must sit below disabledOpacity");

        namespace buttons
        {
            constexpr float radius = 8.0f;
            constexpr int height = 30;
            constexpr int padX = 12;
        }

        namespace lanes
        {
            constexpr float smRadius = 6.0f;
        }

        namespace footer
        {
            constexpr int progressHeight = 6;
        }

        namespace menu
        {
            /*
                Popup menus are drawn here rather than left to JUCE's stock
                look: a square surface card, rows that highlight as inset
                pills, and the same check glyph the include checkboxes use.
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
    }

    /*
        colours reopened after metrics: dimming is a colour role and belongs
        beside the tokens it operates on, but it reads alpha constants that
        metrics does not declare until above.
    */
    namespace colours
    {
        /** The one way to dim a colour for a disabled control. A
            full-strength colour loses 55% exactly as it always has; a colour
            that already carries alpha is floored so it stays legible, and
            capped so it still reads weaker than its live self. */
        inline juce::Colour dimDisabled(juce::Colour colour)
        {
            const auto alpha = colour.getFloatAlpha();

            return colour.withAlpha(
                juce::jmin(alpha * metrics::disabledAlphaCeiling,
                           juce::jmax(alpha * metrics::disabledOpacity,
                                      metrics::disabledAlphaFloor)));
        }

        /** Paint-code sugar, so a widget can route a token through the dim
            without branching at every call. */
        inline juce::Colour dimIfDisabled(juce::Colour colour, bool enabled)
        {
            return enabled ? colour : dimDisabled(colour);
        }
    }
}
