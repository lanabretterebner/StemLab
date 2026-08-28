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

    Registering those faces is expensive enough to be worth doing once per
    process (see the constructor), so this is shared through
    juce::SharedResourcePointer rather than owned per editor. It must
    outlive every component pointing at it, which is what holding that
    pointer as a member declared before those components guarantees.
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

    /** Deliberately draws nothing. A menu with a parent component paints this
        over itself (MenuWindow::paintOverChildren), and the stock version is a
        black double ring that would sit on top of the outline
        drawPopupMenuBackgroundWithOptions already draws. Nothing else in the
        app uses a ResizableBorderComponent - the window resizes by its corner,
        which is drawCornerResizer above. */
    void drawResizableFrame(juce::Graphics&, int, int, const juce::BorderSize<int>&) override {}

    void drawPopupMenuBackgroundWithOptions(juce::Graphics&, int width, int height,
                                            const juce::PopupMenu::Options&) override;

    void drawPopupMenuItemWithOptions(juce::Graphics&, const juce::Rectangle<int>& area,
                                      bool isHighlighted, const juce::PopupMenu::Item&,
                                      const juce::PopupMenu::Options&) override;

    void drawPopupMenuSectionHeaderWithOptions(juce::Graphics&, const juce::Rectangle<int>& area,
                                               const juce::String& sectionName,
                                               const juce::PopupMenu::Options&) override;

    void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator,
                                              int standardMenuItemHeight, int& idealWidth,
                                              int& idealHeight,
                                              const juce::PopupMenu::Options&) override;

    void getIdealPopupMenuSectionHeaderSizeWithOptions(const juce::String& text,
                                                       int standardMenuItemHeight,
                                                       int& idealWidth, int& idealHeight,
                                                       const juce::PopupMenu::Options&) override;

    int getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&) override;

    juce::Font getPopupMenuFont() override;

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

    /** Failure: a stroked X, drawn at the same weight as check() so the
        two footer glyphs read as one family. */
    juce::Path alert(juce::Rectangle<float> bounds);

    /** Which way a chevron points: down discloses, left/right step. */
    enum class ChevronDirection
    {
        down,
        left,
        right
    };

    juce::Path chevron(juce::Rectangle<float> bounds, ChevronDirection direction);

    /** Separation model: a four-point spark. */
    juce::Path sparkle(juce::Rectangle<float> bounds);

    /** Waveform colour: an artist's palette. */
    juce::Path palette(juce::Rectangle<float> bounds);

    /** Waveform zoom: a stroked magnifier, lens and handle. */
    juce::Path magnifier(juce::Rectangle<float> bounds);

    /** Per-stem split: stacked layers (diamond top + two arcs below). */
    juce::Path layers(juce::Rectangle<float> bounds);

    /** Per-lane actions: three dots, the usual "more actions" affordance. */
    juce::Path kebab(juce::Rectangle<float> bounds);

    /** Drag this stem out: a square, a diagonal arrow, a target corner. */
    juce::Path dragOut(juce::Rectangle<float> bounds);
}
