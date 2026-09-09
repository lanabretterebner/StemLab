/*  The accent ramps, which are generated rather than written down.

    A hue that comes out wrong here is not a crash - it is an interface that
    is subtly harder to read than the one that was designed, on a setting
    nobody will think to blame. So the properties the design depends on are
    asserted rather than eyeballed: the default is untouched, lightness is
    preserved across a hue turn, the 100-to-900 ramp keeps separating, and
    the contrast the theme documents survives on all eight.
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

namespace
{
namespace window = stemlab::theme::metrics::window;

/*  The scale a restored window opens at, against the screen it opens on.

    A saved 250% opened 2208x1474 on a 1024x768 display - Separate, the
    transport and the whole footer outside it, the resizer grip with them.
    Shrinking is the only direction this may move, and minScale is the floor.
*/
constexpr int chromeW = 8;
constexpr int chromeH = 60;

// A screen big enough is not allowed to change anything.
static_assert(window::openingScale(1.0, 2400, 1600) == 1.0);
static_assert(window::openingScale(2.5, 2400, 1600) == 2.5);
static_assert(window::openingScale(0.7, 2400, 1600) == 0.7);

// The saved value is still held to the app's own range first.
static_assert(window::openingScale(4.0, 4000, 4000) == window::maxScale);
static_assert(window::openingScale(0.1, 4000, 4000) == window::minScale);

// The measured case: 250% wanted, 1024x768 available, so it comes down to
// whatever fits rather than opening 2.15x the screen.
static_assert(window::openingScale(2.5, 1024, 768) < 2.5);
static_assert(window::openingScale(2.5, 1024, 768) * window::width <= 1024.0);
static_assert(window::openingScale(2.5, 1024, 768) * window::height <= 768.0);

// The same at the far more ordinary 150%, which was 296 px too wide.
static_assert(window::openingScale(1.5, 1024 - chromeW, 768 - chromeH) * window::width
              <= 1024.0 - chromeW);
static_assert(window::openingScale(1.5, 1024 - chromeW, 768 - chromeH) * window::height
              <= 768.0 - chromeH);

// Height can be the binding limit as easily as width.
static_assert(window::openingScale(2.0, 4000, 600) * window::height <= 600.0);

// A screen too small for the app's own minimum gets the minimum, not
// something below it: the layout cannot serve that screen either way, and a
// window at the documented floor is the better of two bad answers.
static_assert(window::openingScale(2.0, 320, 240) == window::minScale);

// No display to ask means no reduction - an unknown answer must not shrink
// a window the user chose.
static_assert(window::openingScale(2.0, 0, 0) == 2.0);
static_assert(window::openingScale(2.0, -1, -1) == 2.0);

/*
    openingSize: the window comes back the shape it was left.

    A scale is the smaller of the two ratios, so reopening from it alone gave
    back the largest 880x564-shaped rectangle inside the window the user left.
    Measured across two trials: 2200x500 came back 730x498, 880x1400 came back
    880x594, 700x1200 came back 704x481. The first three assertions are those
    three measurements, now answered with the size that went in.
*/
static_assert(window::openingSize(2200, 500, 0.83, 2560, 1440).width == 2200);
static_assert(window::openingSize(2200, 500, 0.83, 2560, 1440).height == 500);
static_assert(window::openingSize(880, 1400, 1.0, 2560, 1440).height == 1400);
static_assert(window::openingSize(700, 1200, 0.80, 2560, 1440).width == 700);
static_assert(window::openingSize(700, 1200, 0.80, 2560, 1440).height == 1200);

// The app's own resize limits still bind, in both directions and per axis.
static_assert(window::openingSize(9000, 9000, 1.0, 0, 0).width
              == window::rounded(window::width * window::maxScale));
static_assert(window::openingSize(9000, 9000, 1.0, 0, 0).height
              == window::rounded(window::height * window::maxScale));
static_assert(window::openingSize(10, 10, 1.0, 0, 0).width
              == window::rounded(window::width * window::minScale));
static_assert(window::openingSize(10, 10, 1.0, 0, 0).height
              == window::rounded(window::height * window::minScale));

/*
    Then the screen, per axis rather than in proportion. A window too wide for
    the display wants its width cut; cutting its height to match would be the
    same shape loss the scale caused, arriving by another route.
*/
static_assert(window::openingSize(2200, 500, 1.0, 1024, 768).width == 1024);
static_assert(window::openingSize(2200, 500, 1.0, 1024, 768).height == 500);
static_assert(window::openingSize(800, 1400, 1.0, 1024, 768).height == 768);
static_assert(window::openingSize(800, 1400, 1.0, 1024, 768).width == 800);

// A screen below the app's own minimum gets the minimum, as the scale does.
static_assert(window::openingSize(2000, 1200, 1.0, 320, 240).width
              == window::rounded(window::width * window::minScale));
static_assert(window::openingSize(2000, 1200, 1.0, 320, 240).height
              == window::rounded(window::height * window::minScale));

/*
    Nothing recorded - a first launch, or preferences written before the size
    was - falls back to the scale, and to exactly what the scale used to do.
*/
static_assert(window::openingSize(0, 0, 1.5, 2560, 1440).width
              == window::rounded(window::width * window::openingScale(1.5, 2560, 1440)));
static_assert(window::openingSize(0, 0, 1.5, 2560, 1440).height
              == window::rounded(window::height * window::openingScale(1.5, 2560, 1440)));
static_assert(window::openingSize(1200, 0, 2.5, 1024, 768).width
              == window::rounded(window::width * window::openingScale(2.5, 1024, 768)));
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

    /*  The 100-to-900 ramp still separates on every preset. This is the
        symptom of a step falling outside sRGB rather than a gamut test of
        its own: a step that had to be clamped back into the cube stops
        moving away from its neighbour, and the ramp shows a flat patch
        where a reader expects a gradient. Index 0 is the base color, which
        sits inside the ramp's range rather than on it, so the walk starts
        at 100.
    */
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
