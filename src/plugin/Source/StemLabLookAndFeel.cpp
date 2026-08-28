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
    /*
     * Publish the faces to the theme's font tokens: JUCE 9 resolves fonts
     * without consulting getTypefaceForFont, so every FontOptions must
     * carry its typeface explicitly (see theme::fonts::make).
     *
     * The tokens are also where the faces live for the process. Even the
     * from-memory registration below goes through the FreeType backend's
     * typeface list, whose first use scans the system font directories on
     * the calling thread - which is the message thread here. Published
     * tokens are therefore adopted rather than rebuilt, so the tokens are
     * written exactly once however many instances exist.
     */
    if (theme::fonts::regularTypeface() == nullptr)
        theme::fonts::regularTypeface() = juce::Typeface::createSystemTypefaceFor(
            BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize);

    if (theme::fonts::mediumTypeface() == nullptr)
        theme::fonts::mediumTypeface() = juce::Typeface::createSystemTypefaceFor(
            BinaryData::InterMedium_ttf, BinaryData::InterMedium_ttfSize);

    interRegular = theme::fonts::regularTypeface();
    interMedium = theme::fonts::mediumTypeface();

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

// ============================================================== popup menus

void StemLabLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width,
                                                            int height,
                                                            const juce::PopupMenu::Options&)
{
    // Square on purpose: rounded corners would need a non-opaque menu
    // window, which depends on desktop compositing - a look that changes
    // with the user's window manager is worse than one honest shape.
    const auto bounds =
        juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height))
            .reduced(0.5f);

    g.fillAll(theme::colours::surface());

    g.setColour(theme::colours::outline());
    g.drawRect(bounds, 1.0f);
}

void StemLabLookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& g,
                                                      const juce::Rectangle<int>& area,
                                                      bool isHighlighted,
                                                      const juce::PopupMenu::Item& item,
                                                      const juce::PopupMenu::Options&)
{
    namespace menu = theme::metrics::menu;

    if (item.isSeparator)
    {
        auto line = area.withSizeKeepingCentre(area.getWidth() - 2 * menu::padX, 1);

        g.setColour(theme::colours::divider());
        g.fillRect(line);
        return;
    }

    const float dim = item.isEnabled ? 1.0f : theme::metrics::disabledOpacity;

    if (isHighlighted && item.isEnabled)
    {
        g.setColour(theme::colours::accentTint10());
        g.fillRoundedRectangle(area.reduced(menu::rowInsetX, menu::rowInsetY).toFloat(),
                               menu::rowRadius);
    }

    auto row = area.reduced(menu::rowInsetX + menu::padX, 0);

    // The tick gutter is reserved whether or not this row is ticked, so
    // labels line up down the whole menu.
    const auto tickArea = row.removeFromLeft(menu::tickColumn);

    row.removeFromLeft(menu::tickGap);

    if (item.isTicked)
    {
        g.setColour(theme::colours::accent().withMultipliedAlpha(dim));

        g.strokePath(stemlab::icons::check(tickArea.toFloat()
                                               .withSizeKeepingCentre(
                                                   static_cast<float>(menu::tickIcon),
                                                   static_cast<float>(menu::tickIcon) * 0.72f)),
                     juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
    }

    const bool hasSubMenu =
        item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);

    auto trailing = row.removeFromRight(menu::trailingColumn);

    if (hasSubMenu)
    {
        g.setColour(theme::colours::text45().withMultipliedAlpha(dim));

        g.strokePath(
            stemlab::icons::chevron(trailing.toFloat().withSizeKeepingCentre(
                                        static_cast<float>(menu::submenuArrow) * 0.6f,
                                        static_cast<float>(menu::submenuArrow)),
                                    stemlab::icons::ChevronDirection::right),
            juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                 juce::PathStrokeType::rounded));
    }
    else if (item.shortcutKeyDescription.isNotEmpty())
    {
        g.setColour(theme::colours::text45().withMultipliedAlpha(dim));
        g.setFont(theme::fonts::meta());
        g.drawText(item.shortcutKeyDescription, trailing, juce::Justification::centredRight,
                   false);
    }

    // A caller-supplied colour wins; the tick otherwise reads as the accent
    // and the label stays plain text.
    auto textColour = item.colour != juce::Colour() ? item.colour : theme::colours::text();

    g.setColour(textColour.withMultipliedAlpha(dim));
    g.setFont(getPopupMenuFont());
    g.drawText(item.text, row, juce::Justification::centredLeft, true);
}

void StemLabLookAndFeel::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g,
                                                               const juce::Rectangle<int>& area,
                                                               const juce::String& sectionName,
                                                               const juce::PopupMenu::Options&)
{
    namespace menu = theme::metrics::menu;

    g.setColour(theme::colours::text45());
    g.setFont(juce::Font(theme::fonts::meta()).withExtraKerningFactor(0.04f));

    g.drawText(sectionName,
               area.reduced(menu::rowInsetX + menu::padX, 0)
                   .withTrimmedLeft(menu::tickColumn + menu::tickGap)
                   .withTrimmedTop(4),
               juce::Justification::bottomLeft, false);
}

void StemLabLookAndFeel::getIdealPopupMenuItemSizeWithOptions(const juce::String& text,
                                                              bool isSeparator,
                                                              int standardMenuItemHeight,
                                                              int& idealWidth, int& idealHeight,
                                                              const juce::PopupMenu::Options&)
{
    namespace menu = theme::metrics::menu;

    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = menu::separatorHeight;
        return;
    }

    const juce::Font font{getPopupMenuFont()};

    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : menu::rowHeight;

    idealWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)) +
                 menu::rowInsetX * 2 + menu::padX * 2 + menu::tickColumn + menu::tickGap +
                 menu::trailingColumn;
}

void StemLabLookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions(
    const juce::String& text, int, int& idealWidth, int& idealHeight,
    const juce::PopupMenu::Options&)
{
    namespace menu = theme::metrics::menu;

    const juce::Font font{theme::fonts::meta()};

    idealHeight = menu::sectionHeaderHeight;

    idealWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)) +
                 menu::rowInsetX * 2 + menu::padX * 2 + menu::tickColumn + menu::tickGap;
}

int StemLabLookAndFeel::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&)
{
    return theme::metrics::menu::borderSize;
}

juce::Font StemLabLookAndFeel::getPopupMenuFont() { return juce::Font(theme::fonts::body()); }

void StemLabLookAndFeel::drawCornerResizer(juce::Graphics& g, int width, int height,
                                           bool mouseOver, bool mouseDown)
{
    // Three short diagonal ticks in the corner: enough to say the window
    // resizes, quiet enough to sit on top of the footer without shouting.
    const auto colour = (mouseOver || mouseDown) ? theme::colours::accent()
                                                 : theme::colours::text45();

    g.setColour(colour.withMultipliedAlpha(mouseDown ? 1.0f : 0.75f));

    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);

    for (int tick = 1; tick <= 3; ++tick)
    {
        const auto inset = static_cast<float>(tick) * juce::jmin(w, h) * 0.22f;

        g.drawLine(w - inset, h - 2.0f, w - 2.0f, h - inset, 1.2f);
    }
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
        /*
         * Three horizontal rails with a knob on each, offset left/right.
         *
         * This path is filled, never stroked: a rail is barely more than a
         * pixel tall, so stroking its outline drew two touching edges plus
         * a ring around every knob - at 16px that collapsed into a smudge.
         */
        juce::Path p;

        const float railHeight = juce::jmax(1.3f, b.getHeight() * 0.10f);
        const float knobRadius = juce::jmax(1.7f, b.getHeight() * 0.15f);

        const float railY[] = {0.16f, 0.50f, 0.84f};
        const float knobX[] = {0.70f, 0.30f, 0.56f};

        for (int i = 0; i < 3; ++i)
        {
            const float y = b.getY() + b.getHeight() * railY[i];

            p.addRoundedRectangle(b.getX(), y - railHeight * 0.5f, b.getWidth(), railHeight,
                                  railHeight * 0.5f);

            const float cx = juce::jlimit(b.getX() + knobRadius, b.getRight() - knobRadius,
                                          b.getX() + b.getWidth() * knobX[i]);

            p.addEllipse(cx - knobRadius, y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
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
        /*
         * A tab on the top left rising out of one straight left edge, the
         * way every folder glyph is drawn. The previous outline chamfered
         * its top-left corner and set the tab a third of the way down the
         * body, which read as a lopsided pentagon rather than a folder.
         */
        juce::Path p;

        const float tabTop = b.getY() + b.getHeight() * 0.08f;
        const float bodyTop = b.getY() + b.getHeight() * 0.28f;

        p.startNewSubPath(b.getX(), tabTop);
        p.lineTo(b.getX() + b.getWidth() * 0.36f, tabTop);
        p.lineTo(b.getX() + b.getWidth() * 0.50f, bodyTop);
        p.lineTo(b.getRight(), bodyTop);
        p.lineTo(b.getRight(), b.getBottom());
        p.lineTo(b.getX(), b.getBottom());
        p.closeSubPath();

        return p.createPathWithRoundedCorners(1.5f);
    }

    juce::Path check(juce::Rectangle<float> b)
    {
        juce::Path p;
        p.startNewSubPath(b.getX(), b.getY() + b.getHeight() * 0.55f);
        p.lineTo(b.getX() + b.getWidth() * 0.38f, b.getBottom() - b.getHeight() * 0.08f);
        p.lineTo(b.getRight(), b.getY() + b.getHeight() * 0.12f);
        return p;
    }

    juce::Path chevron(juce::Rectangle<float> b, ChevronDirection direction)
    {
        juce::Path p;

        if (direction == ChevronDirection::down)
        {
            const float inset = b.getHeight() * 0.20f;

            p.startNewSubPath(b.getX(), b.getY() + inset);
            p.lineTo(b.getCentreX(), b.getBottom() - inset);
            p.lineTo(b.getRight(), b.getY() + inset);

            return p;
        }

        const float inset = b.getWidth() * 0.20f;
        const bool right = direction == ChevronDirection::right;

        const float tip = right ? b.getRight() - inset : b.getX() + inset;
        const float tail = right ? b.getX() + inset : b.getRight() - inset;

        p.startNewSubPath(tail, b.getY());
        p.lineTo(tip, b.getCentreY());
        p.lineTo(tail, b.getBottom());

        return p;
    }

    juce::Path sparkle(juce::Rectangle<float> b)
    {
        // A four-point star with concave sides: the arms meet the centre
        // through control points rather than a straight diamond edge.
        juce::Path p;

        const auto cx = b.getCentreX();
        const auto cy = b.getCentreY();

        const auto armX = b.getWidth() * 0.5f;
        const auto armY = b.getHeight() * 0.5f;
        const auto waist = juce::jmin(armX, armY) * 0.16f;

        p.startNewSubPath(cx, cy - armY);
        p.quadraticTo(cx + waist, cy - waist, cx + armX, cy);
        p.quadraticTo(cx + waist, cy + waist, cx, cy + armY);
        p.quadraticTo(cx - waist, cy + waist, cx - armX, cy);
        p.quadraticTo(cx - waist, cy - waist, cx, cy - armY);
        p.closeSubPath();

        return p;
    }

    juce::Path palette(juce::Rectangle<float> b)
    {
        /*
         * Material Symbols "palette" (Apache 2.0; see docs/third-party.md):
         * the familiar painter's palette with a thumb cup and four paint
         * wells, scaled from its 24x24 viewBox into the requested bounds.
         * Even-odd winding knocks the wells out of the body regardless of
         * subpath direction.
         */
        static const juce::Path glyph = []
        {
            auto p = juce::Drawable::parseSVGPath(
                "M12 22C6.49 22 2 17.51 2 12S6.49 2 12 2s10 4.04 10 9c0 "
                "3.31-2.69 6-6 6h-1.77c-.28 0-.5.22-.5.5 0 "
                ".12.05.23.13.33.41.47.64 1.06.64 1.67A2.5 2.5 0 0 1 12 "
                "22zm-5.5-9c.83 0 1.5-.67 1.5-1.5S7.33 10 6.5 10 5 10.67 5 "
                "11.5 5.67 13 6.5 13zm3-4C10.33 9 11 8.33 11 7.5S10.33 6 9.5 "
                "6 8 6.67 8 7.5 8.67 9 9.5 9zm5 0c.83 0 1.5-.67 "
                "1.5-1.5S15.33 6 14.5 6 13 6.67 13 7.5 13.67 9 14.5 9zm3 "
                "4c.83 0 1.5-.67 1.5-1.5S18.33 10 17.5 10 16 10.67 16 "
                "11.5s.67 1.5 1.5 1.5z");

            p.setUsingNonZeroWinding(false);
            return p;
        }();

        auto p = glyph;
        p.applyTransform(glyph.getTransformToScaleToFit(b, true));
        return p;
    }

    juce::Path dragOut(juce::Rectangle<float> b)
    {
        /*
         * Drag this stem out: a rounded square, an arrow leaving it
         * diagonally, and a corner bracket standing in for wherever it is
         * going.
         *
         * The reference art dashes that second square. At 14px dashes close
         * up into a grey smear, so it is reduced to the two edges that carry
         * the meaning.
         */
        const auto size = juce::jmin(b.getWidth(), b.getHeight());

        juce::Path p;

        // Source: rounded square across the top-left.
        const auto square = size * 0.52f;
        p.addRoundedRectangle(b.getX(), b.getY(), square, square, size * 0.12f);

        // Target: the far corner of a box, opposite the source.
        const auto bracket = size * 0.30f;
        const auto right = b.getX() + size;
        const auto bottom = b.getY() + size;

        p.startNewSubPath(right, bottom - bracket);
        p.lineTo(right, bottom);
        p.lineTo(right - bracket, bottom);

        // The arrow between them, on the diagonal.
        const auto from = juce::Point<float>(b.getX() + size * 0.34f, b.getY() + size * 0.34f);
        const auto to = juce::Point<float>(b.getX() + size * 0.74f, b.getY() + size * 0.74f);

        p.startNewSubPath(from);
        p.lineTo(to);

        const auto head = size * 0.20f;

        p.startNewSubPath(to.x - head, to.y);
        p.lineTo(to.x, to.y);
        p.lineTo(to.x, to.y - head);

        return p;
    }

    juce::Path kebab(juce::Rectangle<float> b)
    {
        // Three dots up the centre: the usual "more actions" affordance.
        // Filled, because a 2px ring at this size closes up into a smudge.
        const auto size = juce::jmin(b.getWidth(), b.getHeight());
        const auto dot = size * 0.19f;
        const auto centreX = b.getCentreX();

        juce::Path p;

        for (int i = 0; i < 3; ++i)
        {
            const auto centreY = b.getY() + size * (0.18f + 0.32f * static_cast<float>(i));

            p.addEllipse(centreX - dot * 0.5f, centreY - dot * 0.5f, dot, dot);
        }

        return p;
    }

    juce::Path magnifier(juce::Rectangle<float> b)
    {
        /*
         * Stroked, unlike the palette beside it: a magnifier is a lens and a
         * handle, and filling it would turn the lens into a solid dot.
         * Drawn as an outline path so the caller can stroke it at any size.
         */
        const auto size = juce::jmin(b.getWidth(), b.getHeight());
        const auto lens = size * 0.62f;

        juce::Path p;

        p.addEllipse(b.getX(), b.getY(), lens, lens);

        // The handle leaves the lens at 45 degrees, from just outside its
        // lower-right edge to the bottom-right corner of the bounds.
        const auto centre = juce::Point<float>(b.getX() + lens * 0.5f, b.getY() + lens * 0.5f);
        const auto radius = lens * 0.5f;
        const auto diagonal = 0.70710678f;

        p.startNewSubPath(centre.x + radius * diagonal, centre.y + radius * diagonal);
        p.lineTo(b.getX() + size * 0.96f, b.getY() + size * 0.96f);

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
