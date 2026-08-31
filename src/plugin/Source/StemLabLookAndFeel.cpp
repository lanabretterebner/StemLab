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
     * out of its color scheme, which is JUCE's dark slate by default. That
     * is what makes the standalone Audio/MIDI dialog the one foreign surface
     * in the app, so restate the scheme in Nocturne tokens.
     *
     * This must run BEFORE the setColour block below: setColourScheme calls
     * initialiseColours, which rewrites every id that block sets. The nine
     * values are positional - see ColourScheme::UIColour.
     */
    setColourScheme(juce::LookAndFeel_V4::ColourScheme{
        theme::colors::ground().getARGB(),      // windowBackground
        theme::colors::surface().getARGB(),     // widgetBackground
        theme::colors::surface().getARGB(),     // menuBackground
        theme::colors::outline().getARGB(),     // outline
        theme::colors::text().getARGB(),        // defaultText
        theme::colors::accent().getARGB(),      // defaultFill
        theme::colors::primaryText().getARGB(), // highlightedText
        theme::colors::primaryFill().getARGB(), // highlightedFill
        theme::colors::text().getARGB()});      // menuText

    setColour(juce::ResizableWindow::backgroundColourId, theme::colors::ground());

    setColour(juce::Label::textColourId, theme::colors::text());

    setColour(juce::PopupMenu::backgroundColourId, theme::colors::surface());
    setColour(juce::PopupMenu::textColourId, theme::colors::text());
    setColour(juce::PopupMenu::headerTextColourId, theme::colors::sectionHeader());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::colors::hoverFill());
    setColour(juce::PopupMenu::highlightedTextColourId, theme::colors::text());

    setColour(juce::TooltipWindow::backgroundColourId, theme::colors::surface());
    setColour(juce::TooltipWindow::textColourId, theme::colors::text());
    setColour(juce::TooltipWindow::outlineColourId, theme::colors::outline());

    setColour(juce::AlertWindow::backgroundColourId, theme::colors::surface());
    setColour(juce::AlertWindow::textColourId, theme::colors::text());
    setColour(juce::AlertWindow::outlineColourId, theme::colors::outline());

    setColour(juce::TextButton::textColourOffId, theme::colors::text());
    setColour(juce::TextButton::textColourOnId, theme::colors::text());
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
    { return theme::colors::dimIfDisabled(c, enabled); };

    if (variant == "primary")
    {
        // The accent glow behind primary actions is painted by the editor
        // (a shadow drawn inside the component would be clipped away).
        g.setColour(dimmed(hover ? theme::colors::primaryFillHover()
                                 : theme::colors::primaryFill()));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(dimmed(theme::colors::primaryEdge()));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        return;
    }

    if (variant == "accent-outline")
    {
        g.setColour(dimmed(hover ? theme::colors::accentTint13()
                                 : theme::colors::accentTint10()));
        g.fillRoundedRectangle(bounds, radius);

        g.setColour(dimmed(theme::colors::accent()));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        return;
    }

    if (variant == "ghost")
    {
        if (hover)
        {
            g.setColour(theme::colors::accentTint10());
            g.fillRoundedRectangle(bounds, radius);
        }
        return;
    }

    // "neutral" and toggled S/M states.
    const bool active = button.getToggleState();

    if (active)
    {
        const bool solo = variant == "solo";

        g.setColour(dimmed(solo ? theme::colors::soloActiveFill()
                                : theme::colors::muteActiveFill()));
        g.fillRoundedRectangle(bounds, radius);
        return;
    }

    if (hover)
    {
        g.setColour(theme::colors::hoverFill());
        g.fillRoundedRectangle(bounds, radius);
    }

    g.setColour(dimmed(theme::colors::outline()));
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

    juce::Colour color = theme::colors::text();

    if (variant == "primary")
        color = theme::colors::primaryText();
    else if (variant == "accent-outline" || variant == "ghost")
        color = theme::colors::accent();
    else if (variant == "solo" || variant == "mute")
    {
        if (button.getToggleState())
            color = variant == "solo" ? theme::colors::soloActiveText()
                                       : theme::colors::muteActiveText();
        else
            // S and M are single letters at 10px, where antialiasing eats
            // most of a translucent stem. text45 measured 3.72:1 on the
            // panel before rendering and less after; text75 is 7.70:1, so
            // an untoggled Solo or Mute stays a readable letter rather
            // than a smudge.
            color = theme::colors::text75();
    }

    if (!button.isEnabled())
        color = theme::colors::dimDisabled(color);

    g.setColour(color);
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

    g.setColour(theme::colors::progressTrack());
    g.fillRoundedRectangle(track, 1.5f);

    const auto clamped = juce::jlimit(0.0, 1.0, progress);

    if (clamped > 0.0)
    {
        auto fill = track.withWidth(track.getWidth() * static_cast<float>(clamped));

        juce::DropShadow(theme::colors::accentGlow(), 4, {})
            .drawForRectangle(g, fill.getSmallestIntegerContainer());

        g.setColour(theme::colors::progressFill());
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

    g.setColour(mouseOver || mouseDown ? theme::colors::neutral600()
                                       : theme::colors::neutral700());
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

    g.fillAll(theme::colors::surface());

    g.setColour(theme::colors::outline());
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

        g.setColour(theme::colors::divider());
        g.fillRect(line);
        return;
    }

    const auto dimmed = [enabled = item.isEnabled](juce::Colour c)
    { return theme::colors::dimIfDisabled(c, enabled); };

    if (isHighlighted && item.isEnabled)
    {
        g.setColour(theme::colors::accentTint10());
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
        g.setColour(dimmed(theme::colors::accent()));

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
        g.setColour(dimmed(theme::colors::text45()));

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
        g.setColour(dimmed(theme::colors::text45()));
        g.setFont(theme::fonts::meta());
        g.drawText(item.shortcutKeyDescription, trailing, juce::Justification::centredRight,
                   false);
    }

    // A caller-supplied color wins; the tick otherwise reads as the accent
    // and the label stays plain text.
    // item.colour keeps JUCE's spelling: it is PopupMenu::Item's member.
    auto textColor = item.colour != juce::Colour() ? item.colour
                                                   : theme::colors::text();

    g.setColour(dimmed(textColor));
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

    g.setColour(theme::colors::sectionHeader());
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
    const auto color = (mouseOver || mouseDown) ? theme::colors::accent()
                                                 : theme::colors::text45();

    g.setColour(color.withMultipliedAlpha(mouseDown ? 1.0f : 0.75f));

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

    g.setColour(theme::colors::surface());
    g.fillRoundedRectangle(bounds, 6.0f);

    g.setColour(theme::colors::outline());
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    if (height <= tooltipLineHeight)
    {
        // A pixel inside the box's own padding, so a string measured to the
        // pixel cannot pick up an ellipsis from a rounding difference.
        g.setColour(theme::colors::text());
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

    g.setColour(theme::colors::text());
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

    juce::Path alert(juce::Rectangle<float> b)
    {
        // A plain X rather than a circled warning glyph: at the 16px box
        // the footer gives this, an ellipse plus stem and dot muddies into
        // a blob, while two strokes stay legible.
        juce::Path p;
        p.startNewSubPath(b.getX(), b.getY());
        p.lineTo(b.getRight(), b.getBottom());
        p.startNewSubPath(b.getRight(), b.getY());
        p.lineTo(b.getX(), b.getBottom());
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

    juce::Path midiDragOut(juce::Rectangle<float> b)
    {
        /*
         * The MIDI twin of dragOut. Same box, same diagonal arrow, same
         * numbers for it - the arrow is the half that says "drag", so the
         * two handles read as a pair and only their subject differs.
         *
         * An eighth note replaces the square. It is the shape people
         * already read as "notes" at any size, which a 5-pin DIN is not: at
         * 14px five dots in a ring close into a smudge, and a DIN says
         * "hardware port" rather than "the notes in this stem" even when it
         * does resolve. Piano keys were the other candidate and lose to the
         * same size limit - a black-and-white key pattern needs more pixels
         * than this box has to stop reading as a plain striped rectangle.
         *
         * Outlined, not filled: IconButton strokes or fills a whole path,
         * never both, and dragOut is stroked. A filled head would have to
         * make the arrow solid too.
         */
        const auto size = juce::jmin(b.getWidth(), b.getHeight());

        juce::Path p;

        // The head sits low-left, where the square's bottom-left corner was,
        // so the note's mass balances the arrow across the diagonal exactly
        // as the square's did.
        const auto headW = size * 0.30f;
        const auto headH = size * 0.22f;
        const auto headX = b.getX() + size * 0.04f;
        const auto headY = b.getY() + size * 0.40f;

        p.addEllipse(headX, headY, headW, headH);

        // Stem up the head's right edge. It stops at 0.06 rather than the
        // box top: a stem running to the very edge put half its 1.4px pen
        // outside the icon area, the same way dragOut's first square did.
        const auto stemX = headX + headW;
        const auto stemTop = b.getY() + size * 0.06f;

        p.startNewSubPath(stemX, headY + headH * 0.5f);
        p.lineTo(stemX, stemTop);

        /*
         * One flag, curving down and right off the stem's top. Without it
         * the glyph is a lollipop; with two it is a comb at this size. The
         * control point sits outside the end point so the curve bellies out
         * rather than cutting the corner, which is what makes it read as a
         * flag rather than a serif.
         */
        p.quadraticTo(stemX + size * 0.20f, stemTop + size * 0.06f,
                      stemX + size * 0.13f, stemTop + size * 0.20f);

        // The arrow, identical to dragOut's. Deliberately duplicated rather
        // than shared: they are the same today because the pair should look
        // alike, not because one is defined in terms of the other.
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
