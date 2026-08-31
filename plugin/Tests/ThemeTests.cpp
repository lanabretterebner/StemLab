/*
    The theme's contrast guarantees, pinned.

    The accent is the amber the product's icon is drawn in. That colour is
    bright, and bright accents fail in the opposite direction to dark ones:
    the danger is not a label that vanishes into the background but a label
    that vanishes into its own button. White on #FCB901 is 1.7:1. So every
    pair of tokens the interface actually stacks - a text role over the fill
    role it is drawn on - is measured here, and a future change to the ramp
    that breaks one of them fails the build rather than shipping.
*/

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "StemLabTheme.h"

namespace
{
int failures = 0;

/** WCAG 2.1 relative luminance. */
double relativeLuminance(juce::Colour colour)
{
    const auto channel = [](double v)
    {
        v /= 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };

    return 0.2126 * channel(colour.getRed()) + 0.7152 * channel(colour.getGreen()) +
           0.0722 * channel(colour.getBlue());
}

/** WCAG 2.1 contrast ratio, 1.0 (identical) to 21.0 (black on white). */
double contrastRatio(juce::Colour a, juce::Colour b)
{
    const auto la = relativeLuminance(a);
    const auto lb = relativeLuminance(b);

    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void expectContrast(const char* what, juce::Colour foreground, juce::Colour background,
                    double minimum)
{
    /*
        Flatten first. Several text roles are the body colour at a reduced
        opacity, and measuring those at full strength reports a contrast the
        user never sees - which is the wrong way for a guard to be wrong.
        What reaches the eye is the foreground composited over whatever it
        is drawn on, so that is what gets measured.
    */
    const auto ratio = contrastRatio(background.overlaidWith(foreground), background);

    if (ratio + 1.0e-9 < minimum)
    {
        std::printf("FAIL %-46s %5.2f:1 (needs %.1f:1)\n", what, ratio, minimum);
        ++failures;
        return;
    }

    std::printf("ok   %-46s %5.2f:1\n", what, ratio);
}

void expectColour(const char* what, juce::Colour actual, juce::uint32 expected)
{
    if (actual.getARGB() == expected)
    {
        std::printf("ok   %-46s #%06X\n", what, expected & 0xffffffu);
        return;
    }

    std::printf("FAIL %-46s got #%06X, wanted #%06X\n", what, actual.getARGB() & 0xffffffu,
                expected & 0xffffffu);
    ++failures;
}
}  // namespace

int main()
{
    using namespace stemlab::theme::colours;

    // WCAG AA: 4.5:1 for body text, 3.0:1 for large text and for the
    // non-text parts of a control that has to be locatable.
    constexpr double bodyText = 4.5;
    constexpr double largeTextOrUi = 3.0;

    std::printf("-- text on the fill it is drawn on --\n");
    expectContrast("primaryText on primaryFill", primaryText(), primaryFill(), bodyText);
    expectContrast("primaryText on primaryFillHover", primaryText(), primaryFillHover(), bodyText);
    expectContrast("refineText on refineFill", refineText(), refineFill(), bodyText);
    expectContrast("refineText on refineFillHover", refineText(), refineFillHover(), bodyText);
    expectContrast("soloActiveText on soloActiveFill", soloActiveText(), soloActiveFill(),
                   bodyText);
    expectContrast("muteActiveText on muteActiveFill", muteActiveText(), muteActiveFill(),
                   bodyText);
    expectContrast("dangerText on dangerFill", dangerText(), dangerFill(), bodyText);
    expectContrast("checkboxCheck on checkboxFill", checkboxCheck(), checkboxFill(), largeTextOrUi);

    std::printf("\n-- text on the surfaces it sits on --\n");
    expectContrast("text on ground", text(), ground(), bodyText);
    expectContrast("text on surface", text(), surface(), bodyText);
    expectContrast("sectionHeader on surface", sectionHeader(), surface(), largeTextOrUi);
    expectContrast("textMuted on ground", textMuted(), ground(), largeTextOrUi);

    std::printf("\n-- marks that have to be findable on dark --\n");
    expectContrast("accent on ground", accent(), ground(), largeTextOrUi);
    expectContrast("accent on surface", accent(), surface(), largeTextOrUi);
    expectContrast("playhead on laneWell", playhead(), laneWell(), largeTextOrUi);
    expectContrast("progressFill on progressTrack", progressFill(), progressTrack(),
                   largeTextOrUi);

    std::printf("\n-- the brand colour is the icon's, exactly --\n");
    // Resources/FIStemIcon.png is two colours: black, and this amber across
    // 63% of its opaque pixels. If the ramp is ever regenerated, accent400
    // must still land on it rather than merely near it.
    expectColour("accent400 is the icon amber", accent400(), 0xfffcb901u);
    expectColour("accent() is accent400", accent(), accent400().getARGB());

    std::printf("\n-- a disabled control weakens but stays legible --\n");
    /*
        Two guarantees, and the order between them matters. The ceiling is
        absolute: a dimmed colour is always weaker than its live self, so a
        disabled control can never look more present than an enabled one.
        The floor is conditional on that ceiling - it stops an ordinary
        token being dimmed into invisibility, but it cannot raise a colour
        that was already fainter than the floor, because that would make
        disabling it *brighten* it.
    */
    using stemlab::theme::metrics::disabledAlphaCeiling;
    using stemlab::theme::metrics::disabledAlphaFloor;

    // juce::Colour keeps alpha as a byte, so both bounds hold only to the
    // nearest 1/255 - comparing tighter than that tests the storage, not
    // the rule.
    constexpr auto alphaStep = 1.0f / 255.0f;

    for (const auto alpha : {1.0f, 0.75f, 0.50f, 0.16f, 0.10f, 0.03f})
    {
        const auto live = text().withAlpha(alpha);
        const auto dimmed = dimDisabled(live);
        const auto got = dimmed.getFloatAlpha();
        const auto ceiling = alpha * disabledAlphaCeiling;
        const auto lowest = std::min(disabledAlphaFloor, ceiling);

        if (got > ceiling + alphaStep)
        {
            std::printf("FAIL alpha %.2f dimmed to %.3f, not weaker than %.3f\n", alpha, got,
                        ceiling);
            ++failures;
        }
        else if (got + alphaStep < lowest)
        {
            std::printf("FAIL alpha %.2f dimmed to %.3f, under its floor %.3f\n", alpha, got,
                        lowest);
            ++failures;
        }
        else
        {
            std::printf("ok   alpha %.2f dims to %.3f (weaker, at or above %.3f)\n", alpha, got,
                        lowest);
        }
    }

    std::printf("\n%s\n", failures == 0 ? "all theme checks passed" : "theme checks FAILED");

    return failures == 0 ? 0 : 1;
}
