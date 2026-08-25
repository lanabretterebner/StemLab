#pragma once

#include <JuceHeader.h>

#include "StemLabTheme.h"

/*
    StemLab's LookAndFeel: JUCE's stock widgets drawn in the Nocturne
    system. Buttons pick their variant from the component ID:

        "primary"        filled accent (Insert Stems)
        "accent-outline" accent text + border on an accent tint (Use ...)
        "neutral"        1px outline, plain text (Record, Save, S/M)
        "ghost"          accent text, no border (Change)

    Anything without an ID draws as "neutral". Small buttons (<= 22px tall)
    use the 6px radius and the 10px label size; everything else 8px/13px.

    Type: the bundled Inter faces load from BinaryData here.
    juce::Font::bold is mapped to Inter Medium (weight 500) - per Nocturne,
    nothing renders bolder than 500.
*/
class StemLabLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    StemLabLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;

    void drawProgressBar(juce::Graphics&, juce::ProgressBar&, int width, int height,
                         double progress, const juce::String& textToShow) override;

    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                       bool isVertical, int thumbStart, int thumbSize, bool mouseOver,
                       bool mouseDown) override;

    void drawCornerResizer(juce::Graphics&, int width, int height, bool mouseOver,
                           bool mouseDown) override;

    juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

private:
    juce::Typeface::Ptr interRegular, interMedium;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemLabLookAndFeel)
};

/** Vector icons drawn in-toolkit (Phosphor-style strokes per the spec). */
namespace stemlab::icons
{
    /** Brand glyph: five vertical rounded waveform bars. */
    juce::Path waveformBars(juce::Rectangle<float> bounds);

    /** Settings: three horizontal slider rails with offset knobs. Filled,
        not stroked - a rail this thin outlines into a smudge. */
    juce::Path sliders(juce::Rectangle<float> bounds);

    juce::Path play(juce::Rectangle<float> bounds);
    juce::Path pause(juce::Rectangle<float> bounds);
    juce::Path folder(juce::Rectangle<float> bounds);
    juce::Path check(juce::Rectangle<float> bounds);

    /** Disclosure chevron: down when a lane's children are expanded. */
    juce::Path chevron(juce::Rectangle<float> bounds, bool pointingDown);

    /** Per-stem split: stacked layers (diamond top + two arcs below). */
    juce::Path layers(juce::Rectangle<float> bounds);
}
