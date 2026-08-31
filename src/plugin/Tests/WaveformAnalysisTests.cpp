#include "WaveformAnalysis.h"

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

/** Mean brightness over a profile, which is what a lane's color averages to. */
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

    // ------------------------------------------------------ the color map

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

    // ------------------------------------------------------ the band split

    {
        // 100Hz per bin: bin 1 (100Hz) is low, bin 2 (200Hz) sits exactly on
        // the crossover and is mid, bin 20 (2000Hz) likewise opens the highs.
        std::vector<float> magnitudes(32, 0.0f);

        magnitudes[1] = 1.0f;
        auto bands = bandLevelsForSpectrum(magnitudes.data(), 32, 100.0);
        assert(bands.low == 1.0f && bands.mid == 0.0f && bands.high == 0.0f);

        magnitudes[1] = 0.0f;
        magnitudes[2] = 1.0f;
        bands = bandLevelsForSpectrum(magnitudes.data(), 32, 100.0);
        assert(bands.low == 0.0f && bands.mid == 1.0f && bands.high == 0.0f);

        magnitudes[2] = 0.0f;
        magnitudes[20] = 1.0f;
        bands = bandLevelsForSpectrum(magnitudes.data(), 32, 100.0);
        assert(bands.low == 0.0f && bands.mid == 0.0f && bands.high == 1.0f);

        // Shares follow amplitude, not power: a band at half the magnitude
        // (a quarter of the energy) reads as half the dominant one.
        magnitudes[1] = 1.0f;
        magnitudes[20] = 0.5f;
        bands = bandLevelsForSpectrum(magnitudes.data(), 32, 100.0);
        assert(bands.low == 1.0f);
        assert(std::abs(bands.high - 0.5f) < 1.0e-6f);

        // DC is not bass: bin 0 must not count toward the low band.
        std::vector<float> withOffset(32, 0.0f);
        withOffset[0] = 50.0f;
        withOffset[20] = 1.0f;
        bands = bandLevelsForSpectrum(withOffset.data(), 32, 100.0);
        assert(bands.low == 0.0f && bands.high == 1.0f);

        // Silence has no opinion, and nonsense does not read off the end.
        std::vector<float> silent(32, 0.0f);
        bands = bandLevelsForSpectrum(silent.data(), 32, 100.0);
        assert(bands.low == 0.0f && bands.mid == 0.0f && bands.high == 0.0f);

        bands = bandLevelsForSpectrum(nullptr, 32, 100.0);
        assert(bands.low == 0.0f && bands.mid == 0.0f && bands.high == 0.0f);
    }

    // ------------------------------------------------- end-to-end analysis

    {
        constexpr double rate = 44100.0;

        const auto low = analyseMono(sine(200.0, rate, 1.0).data(),
                                     static_cast<std::size_t>(rate), rate);

        const auto high = analyseMono(sine(5000.0, rate, 1.0).data(),
                                      static_cast<std::size_t>(rate), rate);

        assert(!low.isEmpty() && !high.isEmpty());

        // The whole point of the palette: a bass tone colors differently
        // from a bright one, rather than both following their x position.
        assert(meanBrightness(low) < 0.40f);
        assert(meanBrightness(high) > 0.75f);
        assert(meanBrightness(low) < meanBrightness(high));

        // Profile geometry lines up with the audio it came from.
        assert(std::abs(low.lengthSeconds - 1.0) < 0.01);
        assert(low.secondsPerFrame > 0.0);

        // Band shares ride along, one per frame.
        assert(low.bands.size() == low.brightness.size());
    }

    {
        // Pure tones land their band share in the right band, with the
        // dominant band pinned to 1 and the others down at leakage level.
        constexpr double rate = 44100.0;

        const auto bass = analyseMono(sine(80.0, rate, 0.5).data(),
                                      static_cast<std::size_t>(rate * 0.5), rate);
        const auto vocal = analyseMono(sine(800.0, rate, 0.5).data(),
                                       static_cast<std::size_t>(rate * 0.5), rate);
        const auto hat = analyseMono(sine(5000.0, rate, 0.5).data(),
                                     static_cast<std::size_t>(rate * 0.5), rate);

        const auto& bassMid = bass.bands[bass.bands.size() / 2];
        assert(bassMid.low == 1.0f && bassMid.mid < 0.3f && bassMid.high < 0.3f);

        const auto& vocalMid = vocal.bands[vocal.bands.size() / 2];
        assert(vocalMid.mid == 1.0f && vocalMid.low < 0.3f && vocalMid.high < 0.3f);

        const auto& hatMid = hat.bands[hat.bands.size() / 2];
        assert(hatMid.high == 1.0f && hatMid.low < 0.3f && hatMid.mid < 0.3f);
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

        // The band balance holds through the gap the same way.
        assert(profile.bands.back().high == 1.0f);
        assert(profile.bands.back().low < 0.3f);
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
        // Streaming a file in blocks reads the same profile as holding it.
        // The cache feeds windows as read blocks arrive, so a window lands
        // across a block boundary at nearly every hop and has to be
        // assembled from both blocks rather than restarted in the second.
        constexpr double rate = 44100.0;

        const auto samples = sine(1200.0, rate, 1.5);
        const auto whole = analyseMono(samples.data(), samples.size(), rate);

        for (const std::size_t blockSize : {std::size_t{1}, std::size_t{333},
                                            std::size_t{1024}, std::size_t{4096}})
        {
            MonoSpectrumScanner scanner(samples.size(), rate);

            for (std::size_t position = 0; position < samples.size(); position += blockSize)
            {
                scanner.push(position, samples.data() + position,
                             std::min(blockSize, samples.size() - position));
            }

            const auto streamed = scanner.finish();

            assert(streamed.brightness == whole.brightness);
            assert(streamed.bands.size() == whole.bands.size());

            for (std::size_t i = 0; i < whole.bands.size(); ++i)
            {
                assert(streamed.bands[i].low == whole.bands[i].low);
                assert(streamed.bands[i].mid == whole.bands[i].mid);
                assert(streamed.bands[i].high == whole.bands[i].high);
            }
        }

        // Handing over only what the scanner asks for - what a reader doing
        // the mono sum per window does, so that nothing between two windows
        // is ever touched - reads the same profile again.
        MonoSpectrumScanner asked(samples.size(), rate);

        while (asked.samplesWanted() > 0 && asked.nextSample() < samples.size())
        {
            const auto start = asked.nextSample();

            asked.push(start, samples.data() + start,
                       std::min(asked.samplesWanted(), samples.size() - start));
        }

        assert(asked.finish().brightness == whole.brightness);
    }

    {
        // Past spectrumMaxFrames the hop outgrows the window, and what falls
        // between two windows is then skipped rather than buffered - which is
        // what lets a reader hand over the windows alone.
        constexpr double rate = 44100.0;
        constexpr auto windowSize = static_cast<std::size_t>(spectrumFftSize);

        // Long enough for a hop of two windows; the samples are never read,
        // so the length costs nothing here.
        MonoSpectrumScanner scanner(windowSize * 2 * spectrumMaxFrames, rate);

        const auto samples = sine(900.0, rate, 0.1);

        assert(scanner.nextSample() == 0);
        assert(scanner.samplesWanted() == windowSize);

        scanner.push(0, samples.data(), windowSize);

        // One window analysed, and the next one starts a hop along rather
        // than where this one ended.
        assert(scanner.samplesWanted() == windowSize);
        assert(scanner.nextSample() == windowSize * 2);

        // A span lying entirely in the gap leaves the scanner where it was.
        scanner.push(windowSize, samples.data(), windowSize / 2);

        assert(scanner.samplesWanted() == windowSize);
        assert(scanner.nextSample() == windowSize * 2);

        // A block reaching from inside the gap into the window contributes
        // only its overlap with the window.
        scanner.push(windowSize, samples.data(), windowSize + 8);

        assert(scanner.nextSample() == windowSize * 2 + 8);
    }

    {
        // Nothing to analyse is an empty profile, not a crash.
        assert(analyseMono(nullptr, 0, 44100.0).isEmpty());

        const float one = 0.0f;
        assert(analyseMono(&one, 1, 0.0).isEmpty());
    }

    // -------------------------------------------------- the peak envelope

    {
        constexpr double rate = 1000.0;

        // Two channels that differ, so a mix-up between them shows up.
        std::vector<float> left(1000), right(1000);

        for (std::size_t i = 0; i < left.size(); ++i)
        {
            left[i] = i < 500 ? 1.0f : -0.25f;
            right[i] = i < 500 ? 0.5f : -1.0f;
        }

        const float* channels[] = {left.data(), right.data()};

        const auto envelope = analysePeaks(channels, 2, left.size(), rate);

        assert(!envelope.isEmpty());
        assert(envelope.channels == 2);
        assert(envelope.secondsPerFrame > 0.0);

        // Each channel keeps its own extremes over its own half.
        const auto firstLeft = peakBetween(envelope, 0, 0.0, 0.5);
        assert(std::abs(firstLeft.maximum - 1.0f) < 1.0e-6f);

        const auto secondLeft = peakBetween(envelope, 0, 0.5, 1.0);
        assert(std::abs(secondLeft.minimum + 0.25f) < 1.0e-6f);

        const auto firstRight = peakBetween(envelope, 1, 0.0, 0.5);
        assert(std::abs(firstRight.maximum - 0.5f) < 1.0e-6f);

        const auto secondRight = peakBetween(envelope, 1, 0.5, 1.0);
        assert(std::abs(secondRight.minimum + 1.0f) < 1.0e-6f);

        // The whole file spans both halves of both channels.
        const auto whole = peakBetween(envelope, 0, 0.0, 1.0);
        assert(std::abs(whole.maximum - 1.0f) < 1.0e-6f);
        assert(std::abs(whole.minimum + 0.25f) < 1.0e-6f);

        // A span shorter than a frame still reads the frame it lands in,
        // so a fully zoomed-in column draws audio rather than nothing.
        const auto sliver = peakBetween(envelope, 0, 0.2, 0.2);
        assert(std::abs(sliver.maximum - 1.0f) < 1.0e-6f);

        // Out of range clamps rather than reading off the end.
        const auto before = peakBetween(envelope, 0, -5.0, -4.0);
        assert(std::abs(before.maximum - 1.0f) < 1.0e-6f);

        const auto after = peakBetween(envelope, 0, 50.0, 60.0);
        assert(std::abs(after.minimum + 0.25f) < 1.0e-6f);

        // A channel that does not exist is silence, not a crash.
        const auto missing = peakBetween(envelope, 5, 0.0, 1.0);
        assert(missing.minimum == 0.0f && missing.maximum == 0.0f);
    }

    {
        // Mono stays mono; a third channel is not stored.
        std::vector<float> mono(500, 0.75f);
        const float* one[] = {mono.data()};
        assert(analysePeaks(one, 1, mono.size(), 1000.0).channels == 1);

        std::vector<float> a3(500, 0.1f), b3(500, 0.2f), c3(500, 0.3f);
        const float* three[] = {a3.data(), b3.data(), c3.data()};
        assert(analysePeaks(three, 3, a3.size(), 1000.0).channels == peakMaxChannels);
    }

    {
        // Nothing to analyse is an empty envelope, not a crash.
        assert(analysePeaks(nullptr, 2, 100, 1000.0).isEmpty());

        std::vector<float> some(10, 0.0f);
        const float* one[] = {some.data()};
        assert(analysePeaks(one, 1, 0, 1000.0).isEmpty());
        assert(analysePeaks(one, 1, some.size(), 0.0).isEmpty());
        assert(analysePeaks(one, 0, some.size(), 1000.0).isEmpty());

        // An empty envelope reads as silence rather than indexing into it.
        const auto none = peakBetween(PeakEnvelope{}, 0, 0.0, 1.0);
        assert(none.minimum == 0.0f && none.maximum == 0.0f);
    }

    {
        // A long file coarsens rather than growing without bound.
        std::vector<float> long_(2000000, 0.5f);
        const float* one[] = {long_.data()};
        const auto envelope = analysePeaks(one, 1, long_.size(), 1000000.0);
        assert(envelope.frames() <= peakMaxFrames + 1);
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

        profile.bands = {{1.0f, 0.1f, 0.2f}, {0.3f, 1.0f, 0.4f}, {0.5f, 0.6f, 1.0f}};

        assert(bandsAt(profile, 0.0).low == 1.0f);
        assert(bandsAt(profile, 1.5).mid == 1.0f);
        assert(bandsAt(profile, 2.7).high == 1.0f);

        // Past either end clamps to the nearest frame.
        assert(bandsAt(profile, -10.0).low == 1.0f);
        assert(bandsAt(profile, 900.0).high == 1.0f);

        // No profile is an even balance - white, not a flashing primary.
        const auto neutral = bandsAt(SpectralProfile{}, 1.0);
        assert(neutral.low == 1.0f && neutral.mid == 1.0f && neutral.high == 1.0f);
    }

    return 0;
}
