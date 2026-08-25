#include "StemLabLookAndFeel.h"
#include "BinaryData.h"

namespace theme = stemlab::theme;

namespace
{
bool isSmallButton(const juce::Button& button) { return button.getHeight() <= 22; }

juce::String variantOf(const juce::Button& button)
{
    const auto id = button.getComponentID();
    return id.isEmpty() ? "neutral" : id;
}
} // namespace

StemLabLookAndFeel::StemLabLookAndFeel()
{
    interRegular = juce::Typeface::createSystemTypefaceFor(BinaryData::InterRegular_ttf,
                                                           BinaryData::InterRegular_ttfSize);

    interMedium = juce::Typeface::createSystemTypefaceFor(BinaryData::InterMedium_ttf,
                                                          BinaryData::InterMedium_ttfSize);

    // Publish the faces to the theme's font tokens: JUCE 9 resolves fonts
    // without consulting getTypefaceForFont, so every FontOptions must
    // carry its typeface explicitly (see theme::fonts::make).
    theme::fonts::regularTypeface() = interRegular;
    theme::fonts::mediumTypeface() = interMedium;

    if (interRegular != nullptr)
        setDefaultSansSerifTypeface(interRegular);

    setColour(juce::ResizableWindow::backgroundColourId, theme::colours::ground());

    setColour(juce::Label::textColourId, theme::colours::text());

    setColour(juce::PopupMenu::backgroundColourId, theme::colours::surface());
    setColour(juce::PopupMenu::textColourId, theme::colours::text());
    setColour(juce::PopupMenu::headerTextColourId, theme::colours::text50());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::colours::hoverFill());
    setColour(juce::PopupMenu::highlightedTextColourId, theme::colours::text());

    setColour(juce::TooltipWindow::backgroundColourId, theme::colours::surface());
    setColour(juce::TooltipWindow::textColourId, theme::colours::text());
    setColour(juce::TooltipWindow::outlineColourId, theme::colours::outline());

    setColour(juce::AlertWindow::backgroundColourId, theme::colours::surface());
    setColour(juce::AlertWindow::textColourId, theme::colours::text());
    setColour(juce::AlertWindow::outlineColourId, theme::colours::outline());

    setColour(juce::TextButton::textColourOffId, theme::colours::text());
    setColour(juce::TextButton::textColourOnId, theme::colours::text());
}

juce::Typeface::Ptr StemLabLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    // Nocturne renders nothing bolder than 500: "bold" means Inter Medium.
    if (interRegular != nullptr && interMedium != nullptr)
        return font.isBold() ? interMedium : interRegular;

    return juce::LookAndFeel_V4::getTypefaceForFont(font);
}

void StemLabLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                              const juce::Colour&, bool highlighted, bool down)
{
    const auto variant = variantOf(button);

    const auto radius = isSmallButton(button) ? theme::metrics::lanes::smRadius
                                              : theme::metrics::buttons::radius;

    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);

    const bool hover = (highlighted || down) && button.isEnabled();

    // Disabled controls drop to 45% as a whole - fill and border included,
    // matching the text dim in drawButtonText.
    const float dim = button.isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

    if (variant == "primary")
    {
        // The accent glow behind primary actions is painted by the editor
        // (a shadow drawn inside the component would be clipped away).
        g.setColour((hover ? theme::colours::primaryFillHover() : theme::colours::primaryFill())
                        .withMultipliedAlpha(dim));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(theme::colours::primaryEdge().withMultipliedAlpha(dim));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        return;
    }

    if (variant == "accent-outline")
    {
        g.setColour((hover ? theme::colours::accentTint13() : theme::colours::accentTint10())
                        .withMultipliedAlpha(dim));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(theme::colours::accent().withMultipliedAlpha(dim));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        return;
    }

    if (variant == "ghost")
    {
        if (hover)
        {
            g.setColour(theme::colours::accentTint10());
            g.fillRoundedRectangle(bounds, radius);
        }
        return;
    }

    // "neutral" and toggled S/M states.
    const bool active = button.getToggleState();

    if (active)
    {
        const bool solo = variant == "solo";

        g.setColour((solo ? theme::colours::soloActiveFill() : theme::colours::muteActiveFill())
                        .withMultipliedAlpha(dim));
        g.fillRoundedRectangle(bounds, radius);
        return;
    }

    if (hover)
    {
        g.setColour(theme::colours::hoverFill());
        g.fillRoundedRectangle(bounds, radius);
    }

    g.setColour(theme::colours::outline().withMultipliedAlpha(dim));
    g.drawRoundedRectangle(bounds, radius, 1.0f);
}

juce::Font StemLabLookAndFeel::getTextButtonFont(juce::TextButton& button, int)
{
    return juce::Font(isSmallButton(button) ? theme::fonts::smallButton() : theme::fonts::body());
}

void StemLabLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                        bool /*highlighted*/, bool /*down*/)
{
    const auto variant = variantOf(button);

    juce::Colour colour = theme::colours::text();

    if (variant == "primary")
        colour = theme::colours::primaryText();
    else if (variant == "accent-outline" || variant == "ghost")
        colour = theme::colours::accent();
    else if (variant == "solo" || variant == "mute")
    {
        if (button.getToggleState())
            colour = variant == "solo" ? theme::colours::soloActiveText()
                                       : theme::colours::muteActiveText();
        else
            colour = theme::colours::text45();
    }

    if (!button.isEnabled())
        colour = colour.withMultipliedAlpha(theme::metrics::disabledOpacity);

    g.setColour(colour);
    g.setFont(getTextButtonFont(button, button.getHeight()));

    g.drawText(button.getButtonText(), button.getLocalBounds().reduced(2, 0),
               juce::Justification::centred, false);
}

void StemLabLookAndFeel::drawProgressBar(juce::Graphics& g, juce::ProgressBar& bar, int width,
                                         int height, double progress, const juce::String&)
{
    // Slim Nocturne bar: 3px track with an accent fill and a soft glow.
    // The percentage/ETA label is a separate component in the footer.
    const auto barHeight = theme::metrics::footer::progressHeight;

    auto track =
        juce::Rectangle<float>(0.0f, (static_cast<float>(height) - barHeight) * 0.5f,
                               static_cast<float>(width), barHeight);

    juce::ignoreUnused(bar);

    g.setColour(theme::colours::progressTrack());
    g.fillRoundedRectangle(track, 1.5f);

    const auto clamped = juce::jlimit(0.0, 1.0, progress);

    if (clamped > 0.0)
    {
        auto fill = track.withWidth(track.getWidth() * static_cast<float>(clamped));

        juce::DropShadow(theme::colours::accentGlow(), 4, {})
            .drawForRectangle(g, fill.getSmallestIntegerContainer());

        g.setColour(theme::colours::progressFill());
        g.fillRoundedRectangle(fill, 1.5f);
    }
}

void StemLabLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y,
                                       int width, int height, bool isVertical, int thumbStart,
                                       int thumbSize, bool mouseOver, bool mouseDown)
{
    auto thumb = isVertical
                     ? juce::Rectangle<int>(x + 2, thumbStart, width - 4, thumbSize)
                     : juce::Rectangle<int>(thumbStart, y + 2, thumbSize, height - 4);

    g.setColour(mouseOver || mouseDown ? theme::colours::neutral600()
                                       : theme::colours::neutral700());
    g.fillRoundedRectangle(thumb.toFloat(), 3.0f);
}

juce::Rectangle<int> StemLabLookAndFeel::getTooltipBounds(const juce::String& text,
                                                          juce::Point<int> screenPos,
                                                          juce::Rectangle<int> parentArea)
{
    const juce::Font font{theme::fonts::tooltip()};

    const int width =
        juce::jmin(260, 16 + juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)));
    const int height = 22;

    return juce::Rectangle<int>(screenPos.x, screenPos.y + 18, width, height)
        .constrainedWithin(parentArea);
}

void StemLabLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width,
                                     int height)
{
    const auto bounds = juce::Rectangle<float>(0, 0, static_cast<float>(width),
                                               static_cast<float>(height))
                            .reduced(0.5f);

    g.setColour(theme::colours::surface());
    g.fillRoundedRectangle(bounds, 6.0f);

    g.setColour(theme::colours::outline());
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    g.setColour(theme::colours::text());
    g.setFont(theme::fonts::tooltip());
    g.drawText(text, bounds.reduced(7.0f, 0.0f), juce::Justification::centredLeft);
}

namespace stemlab::icons
{
    juce::Path waveformBars(juce::Rectangle<float> b)
    {
        // Five vertical rounded bars at mixed heights, like a tiny waveform.
        juce::Path p;

        const float heights[] = {0.45f, 0.80f, 1.00f, 0.62f, 0.34f};
        const float barW = b.getWidth() / 5.0f * 0.55f;
        const float pitch = b.getWidth() / 5.0f;

        for (int i = 0; i < 5; ++i)
        {
            const float h = b.getHeight() * heights[i];
            const float x = b.getX() + pitch * static_cast<float>(i) + (pitch - barW) * 0.5f;
            const float y = b.getCentreY() - h * 0.5f;

            p.addRoundedRectangle(x, y, barW, h, barW * 0.5f);
        }

        return p;
    }

    juce::Path sliders(juce::Rectangle<float> b)
    {
        // Three horizontal lines with a knob on each, offset left/right.
        juce::Path p;

        const float knob = b.getHeight() * 0.22f;
        const float lineH = juce::jmax(1.2f, b.getHeight() * 0.08f);
        const float knobXs[] = {0.68f, 0.30f, 0.55f};

        for (int i = 0; i < 3; ++i)
        {
            const float y = b.getY() + b.getHeight() * (0.18f + 0.32f * static_cast<float>(i));

            p.addRoundedRectangle(b.getX(), y - lineH * 0.5f, b.getWidth(), lineH, lineH * 0.5f);

            const float cx = b.getX() + b.getWidth() * knobXs[i];
            p.addEllipse(cx - knob, y - knob, knob * 2.0f, knob * 2.0f);
        }

        return p;
    }

    juce::Path play(juce::Rectangle<float> b)
    {
        juce::Path p;
        p.addTriangle(b.getX(), b.getY(), b.getX(), b.getBottom(), b.getRight(), b.getCentreY());
        p = p.createPathWithRoundedCorners(1.5f);
        return p;
    }

    juce::Path pause(juce::Rectangle<float> b)
    {
        juce::Path p;
        const float barW = b.getWidth() * 0.32f;
        p.addRoundedRectangle(b.getX(), b.getY(), barW, b.getHeight(), 1.0f);
        p.addRoundedRectangle(b.getRight() - barW, b.getY(), barW, b.getHeight(), 1.0f);
        return p;
    }

    juce::Path folder(juce::Rectangle<float> b)
    {
        juce::Path p;
        const float tabW = b.getWidth() * 0.4f;
        const float tabH = b.getHeight() * 0.22f;

        p.startNewSubPath(b.getX(), b.getY() + tabH);
        p.lineTo(b.getX(), b.getBottom());
        p.lineTo(b.getRight(), b.getBottom());
        p.lineTo(b.getRight(), b.getY() + tabH * 1.6f);
        p.lineTo(b.getX() + tabW + tabH, b.getY() + tabH * 1.6f);
        p.lineTo(b.getX() + tabW, b.getY());
        p.lineTo(b.getX() + tabH, b.getY());
        p.closeSubPath();
        p = p.createPathWithRoundedCorners(1.5f);
        return p;
    }

    juce::Path check(juce::Rectangle<float> b)
    {
        juce::Path p;
        p.startNewSubPath(b.getX(), b.getY() + b.getHeight() * 0.55f);
        p.lineTo(b.getX() + b.getWidth() * 0.38f, b.getBottom() - b.getHeight() * 0.08f);
        p.lineTo(b.getRight(), b.getY() + b.getHeight() * 0.12f);
        return p;
    }

    juce::Path layers(juce::Rectangle<float> b)
    {
        // Diamond on top, two arcs (shallow chevrons) stacked below.
        juce::Path p;

        const float cx = b.getCentreX();
        const float topH = b.getHeight() * 0.42f;

        p.startNewSubPath(cx, b.getY());
        p.lineTo(b.getRight(), b.getY() + topH * 0.5f);
        p.lineTo(cx, b.getY() + topH);
        p.lineTo(b.getX(), b.getY() + topH * 0.5f);
        p.closeSubPath();

        for (int i = 0; i < 2; ++i)
        {
            const float y = b.getY() + b.getHeight() * (0.62f + 0.20f * static_cast<float>(i));
            p.startNewSubPath(b.getX(), y);
            p.lineTo(cx, y + b.getHeight() * 0.16f);
            p.lineTo(b.getRight(), y);
        }

        return p;
    }
}
