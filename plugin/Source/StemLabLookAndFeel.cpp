#include "StemLabLookAndFeel.h"

#include <cmath>
#include <optional>
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

/*
    A fill the caller asked for by name rather than by variant.

    The variants above are this look-and-feel's own vocabulary, selected by
    component ID. Code that predates them says what it wants the ordinary
    JUCE way - setColour(buttonColourId, ...) - and silently got a neutral
    button here, because the variant lookup never consulted the colour. That
    turned every accented button in the interface grey.

    So an explicitly set button colour is honoured when no variant was asked
    for. isColourSpecified is what separates "the caller chose this" from
    "this is the default nobody set".
*/
std::optional<juce::Colour> explicitFill(const juce::Button& button)
{
    if (button.getComponentID().isNotEmpty())
        return std::nullopt;

    if (!button.isColourSpecified(juce::TextButton::buttonColourId))
        return std::nullopt;

    return button.findColour(juce::TextButton::buttonColourId);
}

/** A label that stays readable on whatever fill it lands on.

    The interface accent is a bright amber, and light-on-light is the way a
    caller-chosen fill fails: white on it measures 1.7:1. Rather than trust
    each call site to have thought about that, the label is chosen by
    measuring both candidates against the fill and keeping the better one. */
juce::Colour labelOn(juce::Colour fill)
{
    const auto luminance = [](juce::Colour c)
    {
        const auto channel = [](double v)
        {
            v /= 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };

        return 0.2126 * channel(c.getRed()) + 0.7152 * channel(c.getGreen()) +
               0.0722 * channel(c.getBlue());
    };

    const auto ratio = [&luminance](juce::Colour a, juce::Colour b)
    {
        const auto la = luminance(a);
        const auto lb = luminance(b);

        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    };

    const auto ink = theme::colours::ground();
    const auto light = theme::colours::text();

    return ratio(ink, fill) >= ratio(light, fill) ? ink : light;
}

/*
    Tooltip geometry, shared so getTooltipBounds and drawTooltip cannot
    disagree about where the text goes. A tooltip is what the UI falls back
    on when something did not fit - a long source path most of all - so text
    too wide for one line wraps instead of being truncated a second time.
    Anything that fits keeps the single 22px line it always had.

    A word wrapper is no help here: a path is one unbroken token as far as it
    is concerned, so it drops everything past the first line's worth. The
    wrap below therefore breaks where a path actually has seams - after a
    separator, a space, an underscore, a hyphen, a dot - and splits between
    characters when even that leaves a run too wide. Nothing is discarded,
    which is the whole point of the tooltip: the name the strip could not
    show appears in full nowhere else.
*/
constexpr int tooltipMaxWidth = 420;
constexpr int tooltipPadX = 8;
constexpr int tooltipPadY = 5;
constexpr int tooltipLineHeight = 22;
constexpr int tooltipLineSpacing = 16;

float tooltipTextWidth(const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth(juce::Font(theme::fonts::tooltip()), text);
}

// Break opportunities: the seams a path or a long filename is assembled
// from. The separator stays at the end of its line, the way a hyphenated
// word breaks, so the eye can tell a wrap from a character the name has.
bool breaksAfter(juce::juce_wchar c)
{
    return c == '/' || c == '\\' || c == ' ' || c == '_' || c == '-' || c == '.';
}

juce::StringArray splitIntoChunks(const juce::String& text)
{
    juce::StringArray chunks;
    juce::String current;

    for (auto c = text.getCharPointer(); ! c.isEmpty(); ++c)
    {
        const auto ch = *c;

        if (ch == '\n')
        {
            chunks.add(current);
            chunks.add("\n");
            current.clear();
            continue;
        }

        current += juce::String::charToString(ch);

        if (breaksAfter(ch))
        {
            chunks.add(current);
            current.clear();
        }
    }

    if (current.isNotEmpty())
        chunks.add(current);

    return chunks;
}

juce::StringArray wrapTooltip(const juce::String& text, float maxWidth)
{
    juce::StringArray lines;
    juce::String line;

    // A line's trailing separator is allowed to sit in the padding rather
    // than push the next chunk down, so measure without it.
    const auto fits = [maxWidth](const juce::String& s)
    { return tooltipTextWidth(s.trimEnd()) <= maxWidth; };

    for (auto chunk : splitIntoChunks(text))
    {
        if (chunk == "\n")
        {
            lines.add(line.trimEnd());
            line.clear();
            continue;
        }

        if (line.isNotEmpty() && ! fits(line + chunk))
        {
            lines.add(line.trimEnd());
            line.clear();
        }

        // A chunk with no seam in it can still outrun the column - a
        // 120-character filename is exactly that - so give it one.
        while (! fits(line + chunk))
        {
            int fit = 0;

            while (fit < chunk.length() && fits(line + chunk.substring(0, fit + 1)))
                ++fit;

            if (fit == 0)
                fit = 1; // a column too narrow for one glyph still has to advance

            line += chunk.substring(0, fit);
            chunk = chunk.substring(fit);

            lines.add(line.trimEnd());
            line.clear();
        }

        line += chunk;
    }

    if (line.isNotEmpty() || lines.isEmpty())
        lines.add(line.trimEnd());

    return lines;
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

    /*
     * The stock widgets this class does not draw itself - combo boxes, list
     * boxes, toggles, sliders, table headers - are painted by LookAndFeel_V4
     * out of its colour scheme, which is JUCE's dark slate by default. That
     * is what makes the standalone Audio/MIDI dialog the one foreign surface
     * in the app, so restate the scheme in Nocturne tokens.
     *
     * This must run BEFORE the setColour block below: setColourScheme calls
     * initialiseColours, which rewrites every id that block sets. The nine
     * values are positional - see ColourScheme::UIColour.
     */
    setColourScheme(juce::LookAndFeel_V4::ColourScheme{
        theme::colours::ground().getARGB(),      // windowBackground
        theme::colours::surface().getARGB(),     // widgetBackground
        theme::colours::surface().getARGB(),     // menuBackground
        theme::colours::outline().getARGB(),     // outline
        theme::colours::text().getARGB(),        // defaultText
        theme::colours::accent().getARGB(),      // defaultFill
        theme::colours::primaryText().getARGB(), // highlightedText
        theme::colours::primaryFill().getARGB(), // highlightedFill
        theme::colours::text().getARGB()});      // menuText

    setColour(juce::ResizableWindow::backgroundColourId, theme::colours::ground());

    setColour(juce::Label::textColourId, theme::colours::text());

    setColour(juce::PopupMenu::backgroundColourId, theme::colours::surface());
    setColour(juce::PopupMenu::textColourId, theme::colours::text());
    setColour(juce::PopupMenu::headerTextColourId, theme::colours::sectionHeader());
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

    // Disabled controls drop as a whole - fill and border included, matching
    // the text dim in drawButtonText.
    const auto dimmed = [enabled = button.isEnabled()](juce::Colour c)
    { return theme::colours::dimIfDisabled(c, enabled); };

    /*
        A transport control is a circle, not a rounded rectangle: it is the
        one control that is pressed rather than read, and the shape says so
        before the glyph does.
    */
    if (variant == "transport")
    {
        const auto circle = bounds.withSizeKeepingCentre(
            std::min(bounds.getWidth(), bounds.getHeight()),
            std::min(bounds.getWidth(), bounds.getHeight()));

        g.setColour(dimmed(hover ? theme::colours::primaryFillHover()
                                 : theme::colours::primaryFill()));
        g.fillEllipse(circle);
        return;
    }

    if (const auto fill = explicitFill(button))
    {
        g.setColour(dimmed(hover ? fill->brighter(0.12f) : *fill));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(dimmed(fill->brighter(0.30f)));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        return;
    }

    if (variant == "primary")
    {
        // The accent glow behind primary actions is painted by the editor
        // (a shadow drawn inside the component would be clipped away).
        g.setColour(dimmed(hover ? theme::colours::primaryFillHover()
                                 : theme::colours::primaryFill()));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(dimmed(theme::colours::primaryEdge()));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        return;
    }

    if (variant == "accent-outline")
    {
        g.setColour(dimmed(hover ? theme::colours::accentTint13()
                                 : theme::colours::accentTint10()));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(dimmed(theme::colours::accent()));
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

        g.setColour(dimmed(solo ? theme::colours::soloActiveFill()
                                : theme::colours::muteActiveFill()));
        g.fillRoundedRectangle(bounds, radius);
        return;
    }

    if (hover)
    {
        g.setColour(theme::colours::hoverFill());
        g.fillRoundedRectangle(bounds, radius);
    }

    g.setColour(dimmed(theme::colours::outline()));
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

    if (const auto fill = explicitFill(button))
        colour = labelOn(*fill);
    else if (variant == "primary")
        colour = theme::colours::primaryText();
    else if (variant == "accent-outline" || variant == "ghost")
        colour = theme::colours::accent();
    else if (variant == "solo" || variant == "mute")
    {
        if (button.getToggleState())
            colour = variant == "solo" ? theme::colours::soloActiveText()
                                       : theme::colours::muteActiveText();
        else
            // S and M are single letters at 10px, where antialiasing eats
            // most of a translucent stem. text45 measured 3.72:1 on the
            // panel before rendering and less after; text75 is 7.70:1, so
            // an untoggled Solo or Mute stays a readable letter rather
            // than a smudge.
            colour = theme::colours::text75();
    }

    if (!button.isEnabled())
        colour = theme::colours::dimDisabled(colour);

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

    const auto dimmed = [enabled = item.isEnabled](juce::Colour c)
    { return theme::colours::dimIfDisabled(c, enabled); };

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
        g.setColour(dimmed(theme::colours::accent()));

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
        g.setColour(dimmed(theme::colours::text45()));

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
        g.setColour(dimmed(theme::colours::text45()));
        g.setFont(theme::fonts::meta());
        g.drawText(item.shortcutKeyDescription, trailing, juce::Justification::centredRight,
                   false);
    }

    // A caller-supplied colour wins; the tick otherwise reads as the accent
    // and the label stays plain text.
    auto textColour = item.colour != juce::Colour() ? item.colour : theme::colours::text();

    g.setColour(dimmed(textColour));
    g.setFont(getPopupMenuFont());
    g.drawText(item.text, row, juce::Justification::centredLeft, true);
}

void StemLabLookAndFeel::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g,
                                                               const juce::Rectangle<int>& area,
                                                               const juce::String& sectionName,
                                                               const juce::PopupMenu::Options&)
{
    namespace menu = theme::metrics::menu;

    /*
     * JUCE gives a section header a child component inset by the menu border
     * on both sides (ItemComponent::resized), while an ordinary row is painted
     * across the item's whole width. Undo that first, so a heading and the
     * items under it share a left edge instead of sitting borderSize apart.
     */
    const auto row = area.expanded(menu::borderSize, 0);

    g.setColour(theme::colours::sectionHeader());
    g.setFont(juce::Font(theme::fonts::meta()).withExtraKerningFactor(0.04f));

    g.drawText(sectionName,
               row.reduced(menu::rowInsetX + menu::padX, 0)
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
    const int natural = 2 * tooltipPadX + juce::roundToInt(tooltipTextWidth(text));

    // The tooltip lives inside the editor, so the area it has to fit in can
    // be narrower than the cap. Wrap to the width the box will really get,
    // or the height would be counted for lines that never appear.
    const int cap = juce::jmin(tooltipMaxWidth, parentArea.getWidth());

    const int width = juce::jmin(cap, natural);

    // Only text that hit the cap can wrap, so the wrap is worth running only
    // then; everything else is one line and its height is known.
    const int lines = (natural <= cap && ! text.containsChar('\n'))
                          ? 1
                          : wrapTooltip(text, static_cast<float>(width - 2 * tooltipPadX)).size();

    const int height = lines > 1 ? lines * tooltipLineSpacing + 2 * tooltipPadY
                                 : tooltipLineHeight;

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

    if (height <= tooltipLineHeight)
    {
        // A pixel inside the box's own padding, so a string measured to the
        // pixel cannot pick up an ellipsis from a rounding difference.
        g.setColour(theme::colours::text());
        g.setFont(theme::fonts::tooltip());
        g.drawText(text, bounds.reduced(tooltipPadX - 1.0f, 0.0f),
                   juce::Justification::centredLeft);
        return;
    }

    // The wrapped case. The wrap is recomputed from the width actually
    // handed down rather than remembered, so a box the tooltip window
    // trimmed to fit the screen still draws the text that width holds.
    const auto textArea = bounds.reduced(static_cast<float>(tooltipPadX), 0.0f);
    const auto lines = wrapTooltip(text, textArea.getWidth());

    const auto block = static_cast<float>(lines.size() * tooltipLineSpacing);
    auto row = textArea.withSizeKeepingCentre(textArea.getWidth(), block)
                   .removeFromTop(static_cast<float>(tooltipLineSpacing));

    g.setColour(theme::colours::text());
    g.setFont(theme::fonts::tooltip());

    for (const auto& line : lines)
    {
        g.drawText(line, row, juce::Justification::centredLeft, false);
        row.translate(0.0f, static_cast<float>(tooltipLineSpacing));
    }
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

    juce::Path dragOut(juce::Rectangle<float> b)
    {
        /*
         * Drag this stem out: a rounded square, and an arrow leaving it
         * diagonally. Two elements, not three.
         *
         * The reference art puts a dashed destination square opposite the
         * source. At 14px dashes close up into a grey smear, so it was once
         * reduced to a corner bracket - but a bracket is the same L shape as
         * the arrowhead, and at this size the two sat five pixels apart and
         * read as one arrow with a stray duplicate of its own corner. There
         * is no room to separate them: moving the bracket far enough to stop
         * reading as detached puts it on top of the head. A square with an
         * arrow leaving it already says "drag out" without a third element.
         */
        const auto size = juce::jmin(b.getWidth(), b.getHeight());

        juce::Path p;

        // Source: rounded square in the top-left, inset so its 1.4px stroke
        // stays in the box. The old square ran to the very edge, which put
        // half the pen outside it and made this glyph read heavier than the
        // kebab sitting next to it.
        const auto inset = size * 0.03f;
        const auto square = size * 0.42f;
        p.addRoundedRectangle(b.getX() + inset, b.getY() + inset, square, square, size * 0.10f);

        /*
         * The arrow, on the diagonal. It starts clear of the square rather
         * than inside it: the square's corner arc meets the diagonal at
         * 0.42 of the box and the shaft's round cap reaches back to 0.52, so
         * roughly a pixel of ground separates them at 14px. The shaft used
         * to begin at 0.34, well inside the fill, and ate the square's
         * bottom-right corner.
         */
        const auto from = juce::Point<float>(b.getX() + size * 0.56f, b.getY() + size * 0.56f);
        const auto to = juce::Point<float>(b.getX() + size * 0.93f, b.getY() + size * 0.93f);

        p.startNewSubPath(from);
        p.lineTo(to);

        const auto head = size * 0.24f;

        p.startNewSubPath(to.x - head, to.y);
        p.lineTo(to.x, to.y);
        p.lineTo(to.x, to.y - head);

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
}
