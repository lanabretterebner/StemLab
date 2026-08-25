#pragma once

#include <JuceHeader.h>

#include "StemLabTheme.h"

/*
    StemLab's LookAndFeel: the single hook where the redesign restyles JUCE's
    stock widgets (buttons, toggles, progress bar, scrollbars, popup menus).

    Today it intentionally overrides nothing. The 0.9.9 interface is JUCE's
    stock LookAndFeel_V4 dark scheme plus a handful of per-widget colours the
    editor sets from stemlab::theme (accent action buttons, record buttons,
    progress bar). Subclassing without overrides keeps that rendering
    byte-identical while giving redesign work a home that is already wired
    into the component tree - the editor installs an instance with
    setLookAndFeel(), so every child widget resolves its drawing through this
    class.

    When restyling:

      - Prefer overriding draw methods / colour IDs here over adding
        setColour calls in the editor; per-widget colours that vary between
        widgets of the same class (e.g. the two record buttons) are the
        exception and stay at the widget.
      - Take every colour, font, and dimension from stemlab::theme rather
        than inlining literals, so the token header stays the one source of
        truth.
*/
class StemLabLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    StemLabLookAndFeel() = default;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabLookAndFeel)
};
