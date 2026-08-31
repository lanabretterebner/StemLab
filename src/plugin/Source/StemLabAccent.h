#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <atomic>
#include <cmath>

/*
    The accent, and the eight hues it can be.

    StemLabTheme's accent ramp is ten values - the accent itself and a
    100..900 lightness ramp - and every accent-derived token in the theme
    (fills, glows, tints, the playhead, the record dot, progress) resolves to
    one of them. So making the accent settable is exactly this: make those ten
    values depend on a choice, and leave the rest of the token sheet alone.

    HOW THE OTHER HUES ARE MADE. The shipped ramp was generated in OKLCH, and
    measuring it back confirms it: hue is 289.3 degrees at every step to
    within a degree, lightness steps evenly from 0.971 down to 0.290, and
    chroma arcs up to 0.125 in the middle. So a second accent is that same
    ramp with the hue turned, keeping every L and C - which is the whole point
    of doing it in OKLCH rather than HSB. Lightness in OKLab is perceptual, so
    the contrast ratios the theme documents against ground() and surface()
    hold whatever hue you pick. Turning an HSB hue instead would keep the
    numbers and lose the meaning: a yellow and a blue at one HSB brightness
    are nowhere near equally light, and the 4.5:1 the 11px labels need would
    quietly become 2:1 on half the choices.

    WHY THE DEFAULT IS NOT GENERATED. Index 0 returns the literal bytes from
    the token sheet, not a round-trip through this file. Some of those steps
    sit a hair outside what the conversion reproduces exactly - the lightest
    is 1% over the gamut boundary at its own hue - so regenerating it would
    change the shipped look by a rounding error nobody asked for. The default
    must be the default.

    GAMUT. A hue turn can ask for chroma sRGB cannot show; teal at the middle
    of the ramp wants about 20% more than exists. Chroma is reduced until the
    color fits, and lightness is never touched, because lightness is what
    carries the contrast.
*/

namespace stemlab::theme::accents
{
    /** The ten values, in the order the theme asks for them. */
    enum class Step
    {
        base = 0,
        s100,
        s200,
        s300,
        s400,
        s500,
        s600,
        s700,
        s800,
        s900,
        count
    };

    inline constexpr int stepCount = static_cast<int>(Step::count);

    struct Preset
    {
        const char* name;

        /*  OKLCH hue in degrees. The default's own measured hue is here
            rather than 0, so "rotate to this hue" is an absolute statement
            and the table reads as a list of colors instead of a list of
            offsets from one.
        */
        float hue;
    };

    /*  Eight, because that is what fits the settings row as swatches without
        them shrinking below a comfortable target, and because past about
        eight the choices stop being distinguishable at 14px.
    */
    inline constexpr std::array<Preset, 8> presets{{
        {"Blurple", 289.3f},
        {"Blue", 255.0f},
        {"Teal", 190.0f},
        {"Green", 145.0f},
        {"Amber", 85.0f},
        {"Orange", 50.0f},
        {"Rose", 15.0f},
        {"Magenta", 330.0f},
    }};

    inline constexpr int count() { return static_cast<int>(presets.size()); }

    /** The shipped ramp, and the one index 0 hands back untouched. */
    inline constexpr std::array<juce::uint32, stepCount> defaultRamp{
        0xff9184d9, 0xfff5f4ff, 0xffe7e5fe, 0xffd2cefd, 0xffb5abfc,
        0xff968ae0, 0xff796cbf, 0xff5d5294, 0xff423a6a, 0xff2b2741,
    };

    namespace detail
    {
        struct Lab
        {
            float l, a, b;
        };

        inline float toLinear(float c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        inline float toSrgb(float c)
        {
            return c <= 0.0031308f ? c * 12.92f
                                   : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        inline Lab toOklab(juce::Colour color)
        {
            const auto r = toLinear(color.getFloatRed());
            const auto g = toLinear(color.getFloatGreen());
            const auto b = toLinear(color.getFloatBlue());

            const auto l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
            const auto m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
            const auto s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

            return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
                    1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
                    0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
        }

        /** The linear-light triple, which may fall outside [0, 1]. */
        inline std::array<float, 3> toLinearRgb(Lab lab)
        {
            const auto l = std::pow(lab.l + 0.3963377774f * lab.a + 0.2158037573f * lab.b, 3.0f);
            const auto m = std::pow(lab.l - 0.1055613458f * lab.a - 0.0638541728f * lab.b, 3.0f);
            const auto s = std::pow(lab.l - 0.0894841775f * lab.a - 1.2914855480f * lab.b, 3.0f);

            return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                    -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                    -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
        }

        inline bool inGamut(Lab lab)
        {
            constexpr auto slack = 1.0e-4f;

            for (auto channel : toLinearRgb(lab))
                if (channel < -slack || channel > 1.0f + slack)
                    return false;

            return true;
        }

        inline juce::Colour toColor(Lab lab)
        {
            const auto linear = toLinearRgb(lab);

            const auto channel = [&](int index)
            {
                return static_cast<juce::uint8>(
                    juce::jlimit(0.0f, 255.0f,
                                 std::round(toSrgb(juce::jlimit(0.0f, 1.0f, linear[(size_t) index]))
                                            * 255.0f)));
            };

            return juce::Colour(channel(0), channel(1), channel(2));
        }

        /** The same lightness at a new hue, at as much chroma as sRGB has. */
        inline juce::Colour rotate(juce::Colour source, float hueDegrees)
        {
            const auto lab = toOklab(source);
            const auto chroma = std::hypot(lab.a, lab.b);
            const auto radians = juce::degreesToRadians(hueDegrees);

            const auto at = [&](float c)
            { return Lab{lab.l, c * std::cos(radians), c * std::sin(radians)}; };

            auto wanted = chroma;

            if (!inGamut(at(wanted)))
            {
                // Bisection on chroma alone. Twenty rounds resolves finer than
                // one 8-bit step, and lightness never moves, so the contrast
                // this ramp was checked for survives the clip.
                auto low = 0.0f;
                auto high = chroma;

                for (int round = 0; round < 20; ++round)
                {
                    const auto mid = 0.5f * (low + high);
                    (inGamut(at(mid)) ? low : high) = mid;
                }

                wanted = low;
            }

            return toColor(at(wanted)).withAlpha(source.getFloatAlpha());
        }

        /** The live ramp. Written on the message thread, read from paint. */
        inline std::array<std::atomic<juce::uint32>, stepCount>& liveRamp()
        {
            // Filled in place rather than returned from a lambda: an array of
            // atomics is neither copyable nor movable, so there is nothing to
            // return. The bool exists only to make the fill run once.
            static std::array<std::atomic<juce::uint32>, stepCount> ramp;

            [[maybe_unused]] static const bool filled = []
            {
                for (int step = 0; step < stepCount; ++step)
                    ramp[(size_t) step].store(defaultRamp[(size_t) step],
                                              std::memory_order_relaxed);

                return true;
            }();

            return ramp;
        }

        inline std::atomic<int>& liveIndex()
        {
            static std::atomic<int> index{0};
            return index;
        }
    }

    /** The ramp a preset resolves to, without making it the current one. */
    inline std::array<juce::Colour, stepCount> ramp(int presetIndex)
    {
        std::array<juce::Colour, stepCount> result{};

        for (int step = 0; step < stepCount; ++step)
        {
            const juce::Colour shipped(defaultRamp[(size_t) step]);

            result[(size_t) step] =
                presetIndex == 0
                    ? shipped
                    : detail::rotate(shipped,
                                     presets[(size_t) juce::jlimit(0, count() - 1, presetIndex)].hue);
        }

        return result;
    }

    inline int index() { return detail::liveIndex().load(std::memory_order_relaxed); }

    inline juce::String name(int presetIndex)
    {
        return presets[(size_t) juce::jlimit(0, count() - 1, presetIndex)].name;
    }

    /** Regenerates the live ramp. Cheap, and only ever on a user's click. */
    inline void setIndex(int presetIndex)
    {
        const auto wanted = juce::jlimit(0, count() - 1, presetIndex);
        const auto generated = ramp(wanted);

        for (int step = 0; step < stepCount; ++step)
            detail::liveRamp()[(size_t) step].store(
                generated[(size_t) step].getARGB(), std::memory_order_relaxed);

        detail::liveIndex().store(wanted, std::memory_order_relaxed);
    }

    inline juce::Colour step(Step which)
    {
        return juce::Colour(
            detail::liveRamp()[(size_t) which].load(std::memory_order_relaxed));
    }

    /** What the settings row draws for a choice: that accent's own base. */
    inline juce::Colour swatch(int presetIndex)
    {
        return ramp(juce::jlimit(0, count() - 1, presetIndex))[0];
    }
}
