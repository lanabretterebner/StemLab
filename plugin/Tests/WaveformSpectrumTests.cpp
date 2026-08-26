#include "WaveformSpectrum.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <vector>

using namespace stemlab::waveform;

namespace
{
constexpr double pi = 3.14159265358979323846;

/** Naive O(n^2) DFT, to check the fast one against. */
std::vector<std::complex<float>> naiveDft(const std::vector<std::complex<float>>& input)
{
    const auto n = input.size();
    std::vector<std::complex<float>> output(n);

    for (std::size_t k = 0; k < n; ++k)
    {
        std::complex<double> sum(0.0, 0.0);

        for (std::size_t t = 0; t < n; ++t)
        {
            const double angle = -2.0 * pi * static_cast<double>(k) * static_cast<double>(t) /
                                 static_cast<double>(n);

            sum += std::complex<double>(input[t].real(), input[t].imag()) *
                   std::complex<double>(std::cos(angle), std::sin(angle));
        }

        output[k] = {static_cast<float>(sum.real()), static_cast<float>(sum.imag())};
    }

    return output;
}

std::vector<float> sine(double frequencyHz, double sampleRate, double seconds)
{
    const auto count = static_cast<std::size_t>(sampleRate * seconds);
    std::vector<float> samples(count);

    for (std::size_t i = 0; i < count; ++i)
    {
        samples[i] = static_cast<float>(
            std::sin(2.0 * pi * frequencyHz * static_cast<double>(i) / sampleRate));
    }

    return samples;
}

/** Mean brightness over a profile, which is what a lane's colour averages to. */
float meanBrightness(const SpectralProfile& profile)
{
    if (profile.isEmpty())
        return 0.0f;

    double total = 0.0;

    for (const auto value : profile.brightness)
        total += value;

    return static_cast<float>(total / static_cast<double>(profile.brightness.size()));
}
}

int main()
{
    // ------------------------------------------------------------ the FFT

    // Against a naive DFT on a non-trivial signal: same answer, every bin.
    {
        std::vector<std::complex<float>> input;

        for (int i = 0; i < 16; ++i)
        {
            input.push_back({static_cast<float>(std::sin(0.7 * i) + 0.3 * std::cos(2.1 * i)),
                             0.0f});
        }

        const auto expected = naiveDft(input);

        auto actual = input;
        forwardFft(actual);

        for (std::size_t k = 0; k < expected.size(); ++k)
            assert(std::abs(actual[k] - expected[k]) < 1.0e-3f);
    }

    // A pure tone sitting exactly on a bin puts its energy in that bin.
    {
        constexpr int n = 64;
        constexpr int tone = 7;

        std::vector<std::complex<float>> input(n);

        for (int i = 0; i < n; ++i)
            input[static_cast<std::size_t>(i)] = {
                static_cast<float>(std::sin(2.0 * pi * tone * i / n)), 0.0f};

        forwardFft(input);

        int peak = 1;

        for (int k = 2; k < n / 2; ++k)
            if (std::abs(input[static_cast<std::size_t>(k)]) >
                std::abs(input[static_cast<std::size_t>(peak)]))
                peak = k;

        assert(peak == tone);
    }

    // Degenerate sizes are a no-op rather than a crash.
    {
        std::vector<std::complex<float>> empty;
        forwardFft(empty);

        std::vector<std::complex<float>> single{{1.0f, 0.0f}};
        forwardFft(single);
        assert(single.size() == 1);
    }

    // ------------------------------------------------------- the centroid

    {
        // All the energy in one bin: the centroid is that bin's frequency.
        std::vector<float> magnitudes(9, 0.0f);
        magnitudes[4] = 1.0f;

        assert(std::abs(spectralCentroid(magnitudes.data(), 9, 100.0) - 400.0) < 1.0e-6);

        // Split evenly between two bins: halfway between them.
        magnitudes[4] = 1.0f;
        magnitudes[6] = 1.0f;
        assert(std::abs(spectralCentroid(magnitudes.data(), 9, 100.0) - 500.0) < 1.0e-6);

        // DC is not a pitch: bin 0 must not drag the answer down.
        std::vector<float> withOffset(9, 0.0f);
        withOffset[0] = 50.0f;
        withOffset[4] = 1.0f;
        assert(std::abs(spectralCentroid(withOffset.data(), 9, 100.0) - 400.0) < 1.0e-6);

        // Silence has no opinion.
        std::vector<float> silent(9, 0.0f);
        assert(spectralCentroid(silent.data(), 9, 100.0) == 0.0);

        // Nonsense input does not read off the end.
        assert(spectralCentroid(nullptr, 9, 100.0) == 0.0);
        assert(spectralCentroid(magnitudes.data(), 1, 100.0) == 0.0);
    }

    // ------------------------------------------------------ the colour map

    {
        // Monotonic across the band, and clamped outside it.
        assert(brightnessForCentroid(spectrumLowHz) < 0.001f);
        assert(brightnessForCentroid(spectrumHighHz) > 0.999f);
        assert(brightnessForCentroid(10.0) == 0.0f);
        assert(brightnessForCentroid(40000.0) == 1.0f);

        assert(brightnessForCentroid(200.0) < brightnessForCentroid(2000.0));
        assert(brightnessForCentroid(2000.0) < brightnessForCentroid(6000.0));

        // Logarithmic: equal octaves are equal distances.
        const auto lower = brightnessForCentroid(250.0) - brightnessForCentroid(125.0);
        const auto upper = brightnessForCentroid(4000.0) - brightnessForCentroid(2000.0);
        assert(std::abs(lower - upper) < 1.0e-5f);

        // A frame with no energy is neutral, not bass.
        assert(brightnessForCentroid(0.0) == 0.5f);
    }

    // ------------------------------------------------- end-to-end analysis

    {
        constexpr double rate = 44100.0;

        const auto low = analyseMono(sine(200.0, rate, 1.0).data(),
                                     static_cast<std::size_t>(rate), rate);

        const auto high = analyseMono(sine(5000.0, rate, 1.0).data(),
                                      static_cast<std::size_t>(rate), rate);

        assert(!low.isEmpty() && !high.isEmpty());

        // The whole point of the palette: a bass tone colours differently
        // from a bright one, rather than both following their x position.
        assert(meanBrightness(low) < 0.40f);
        assert(meanBrightness(high) > 0.75f);
        assert(meanBrightness(low) < meanBrightness(high));

        // Profile geometry lines up with the audio it came from.
        assert(std::abs(low.lengthSeconds - 1.0) < 0.01);
        assert(low.secondsPerFrame > 0.0);
    }

    {
        // Silence inherits rather than flashing: a gap between two bright
        // hits stays bright.
        constexpr double rate = 44100.0;

        auto samples = sine(6000.0, rate, 0.5);
        samples.resize(static_cast<std::size_t>(rate), 0.0f);

        const auto profile = analyseMono(samples.data(), samples.size(), rate);

        assert(!profile.isEmpty());
        assert(profile.brightness.back() > 0.75f);
    }

    {
        // A long file decimates instead of growing without bound.
        constexpr double rate = 8000.0;

        const auto samples = sine(400.0, rate, 400.0);

        const auto profile = analyseMono(samples.data(), samples.size(), rate);

        assert(profile.brightness.size() <= spectrumMaxFrames + 1);
        assert(std::abs(profile.lengthSeconds - 400.0) < 0.01);
    }

    {
        // Nothing to analyse is an empty profile, not a crash.
        assert(analyseMono(nullptr, 0, 44100.0).isEmpty());

        const float one = 0.0f;
        assert(analyseMono(&one, 1, 0.0).isEmpty());
    }

    // --------------------------------------------------------- the lookup

    {
        SpectralProfile profile;
        profile.lengthSeconds = 3.0;
        profile.secondsPerFrame = 1.0;
        profile.brightness = {0.1f, 0.5f, 0.9f};

        assert(std::abs(brightnessAt(profile, 0.0) - 0.1f) < 1.0e-6f);
        assert(std::abs(brightnessAt(profile, 1.5) - 0.5f) < 1.0e-6f);
        assert(std::abs(brightnessAt(profile, 2.7) - 0.9f) < 1.0e-6f);

        // Past either end clamps to the nearest frame.
        assert(std::abs(brightnessAt(profile, -10.0) - 0.1f) < 1.0e-6f);
        assert(std::abs(brightnessAt(profile, 900.0) - 0.9f) < 1.0e-6f);

        // No profile is a neutral mid-hue, so an unanalysed lane is calm.
        assert(brightnessAt(SpectralProfile{}, 1.0) == 0.5f);
    }

    return 0;
}
