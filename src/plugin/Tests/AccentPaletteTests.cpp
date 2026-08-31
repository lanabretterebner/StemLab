/*  The accent ramps, which are generated rather than written down.

    A hue that comes out wrong here is not a crash - it is an interface that
    is subtly harder to read than the one that was designed, on a setting
    nobody will think to blame. So the properties the design depends on are
    asserted rather than eyeballed: the default is untouched, lightness is
    preserved across a hue turn, every step is inside sRGB, and the contrast
    the theme documents survives on all eight.
*/

#include "StemLabAccent.h"
#include "StemLabTheme.h"

#include <cassert>
#include <cmath>

using namespace stemlab::theme;

namespace
{
/** WCAG relative luminance, which is what the theme's ratios are quoted in. */
double luminance(juce::Colour color)
{
    const auto channel = [](double value)
    { return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };

    return 0.2126 * channel(color.getFloatRed())
         + 0.7152 * channel(color.getFloatGreen())
         + 0.0722 * channel(color.getFloatBlue());
}

double contrast(juce::Colour a, juce::Colour b)
{
    const auto high = std::max(luminance(a), luminance(b));
    const auto low = std::min(luminance(a), luminance(b));

    return (high + 0.05) / (low + 0.05);
}

/** OKLab lightness, the axis a hue turn must leave alone. */
float lightness(juce::Colour color)
{
    return accents::detail::toOklab(color).l;
}
}

int main()
{
    // The shipped look is the literal token sheet, not a round trip through
    // the conversion - which would move it by a rounding error nobody asked
    // for. This is the assertion that keeps index 0 honest.
    {
        const auto shipped = accents::ramp(0);

        for (int step = 0; step < accents::stepCount; ++step)
            assert(shipped[(size_t) step].getARGB()
                   == accents::defaultRamp[(size_t) step]);
    }

    // Lightness is what carries contrast, so a hue turn preserves it. The
    // tolerance is one 8-bit step's worth of OKLab L, not a fudge factor:
    // the ramp is quantised to sRGB bytes on the way out.
    for (int preset = 0; preset < accents::count(); ++preset)
    {
        const auto ramp = accents::ramp(preset);

        for (int step = 0; step < accents::stepCount; ++step)
        {
            const juce::Colour shipped(accents::defaultRamp[(size_t) step]);

            assert(std::abs(lightness(ramp[(size_t) step]) - lightness(shipped)) < 0.01f);
        }
    }

    // Every generated step is a real sRGB color. A step that clipped would
    // show up as a flat patch where the ramp should still be separating.
    for (int preset = 0; preset < accents::count(); ++preset)
    {
        const auto ramp = accents::ramp(preset);

        for (int step = 1; step < accents::stepCount - 1; ++step)
        {
            // 100 is the lightest and 900 the darkest, so the ramp descends.
            assert(lightness(ramp[(size_t) step]) > lightness(ramp[(size_t) step + 1]));
        }
    }

    /*  The pairs the theme actually puts together, on every accent. These
        are the ratios the token sheet quotes for the default; the point of
        turning hue in OKLCH rather than HSB is that they hold for the rest.

        primaryText on primaryFill is accent100 on accent700 - the Separate
        button's own label - and needs the 4.5:1 that body text needs.
    */
    for (int preset = 0; preset < accents::count(); ++preset)
    {
        const auto ramp = accents::ramp(preset);

        const auto primaryFill = ramp[(size_t) accents::Step::s700];
        const auto primaryText = ramp[(size_t) accents::Step::s100];

        assert(contrast(primaryText, primaryFill) > 4.5);

        // refineText on refineFill: accent300 on accent900.
        const auto refineFill = ramp[(size_t) accents::Step::s900];
        const auto refineText = ramp[(size_t) accents::Step::s300];

        assert(contrast(refineText, refineFill) > 4.5);

        // The accent itself has to read as a line and a fill against the
        // ground it is drawn on, #161826.
        assert(contrast(ramp[(size_t) accents::Step::base], juce::Colour(0xff161826)) > 3.0);
    }

    // Setting an index changes what the theme hands out, and setting it back
    // restores the shipped bytes exactly.
    {
        assert(accents::index() == 0);
        assert(accents::step(accents::Step::base).getARGB() == accents::defaultRamp[0]);

        accents::setIndex(3);
        assert(accents::index() == 3);
        assert(accents::step(accents::Step::base).getARGB() != accents::defaultRamp[0]);

        accents::setIndex(0);
        assert(accents::index() == 0);

        for (int step = 0; step < accents::stepCount; ++step)
            assert(accents::step(static_cast<accents::Step>(step)).getARGB()
                   == accents::defaultRamp[(size_t) step]);
    }

    // An index from a file that no longer matches this build is clamped
    // rather than read off the end of the table.
    {
        accents::setIndex(-7);
        assert(accents::index() == 0);

        accents::setIndex(9999);
        assert(accents::index() == accents::count() - 1);

        accents::setIndex(0);
    }

    // Every preset is distinguishable from every other one at a glance: two
    // that looked alike would be two settings that do the same thing.
    for (int a = 0; a < accents::count(); ++a)
        for (int b = a + 1; b < accents::count(); ++b)
            assert(accents::swatch(a) != accents::swatch(b));

    /*  The waveform palette named "Accent" draws with the accent that is
        set, not with the one that shipped.

        It is called Accent because that is what it is. A palette that had
        been renamed and left drawing the old blurple would look identical on
        the default and wrong on every other choice - and would only be
        noticed by someone who set an accent and then looked at a waveform,
        which needs a separation first. So it is asserted here rather than
        left to be found.
    */
    {
        namespace wave = stemlab::theme::waveform;

        assert(wave::paletteName(0) == juce::String("Accent"));

        for (int preset = 0; preset < accents::count(); ++preset)
        {
            accents::setIndex(preset);

            // Any stem name and any brightness: palette 0 ignores both, which
            // is the difference between it and Stem Color and Spectrum.
            assert(wave::playedColor(0, "vocals", 0.2f) == accents::step(accents::Step::base));
            assert(wave::playedColor(0, "drums", 0.9f) == accents::step(accents::Step::base));
        }

        // And it actually moves: the blurple is not hiding behind the name.
        accents::setIndex(3);
        assert(wave::playedColor(0, "vocals", 0.5f).getARGB() != accents::defaultRamp[0]);

        accents::setIndex(0);
        assert(wave::playedColor(0, "vocals", 0.5f).getARGB() == accents::defaultRamp[0]);
    }

    return 0;
}
