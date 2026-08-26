#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

/*
    Spectral colouring for the lane waveforms.

    The "Spectrum" palette used to be a fixed violet-to-amber sweep across
    the lane's width: pretty, and completely unrelated to the audio - a sine
    tone and a drum loop came out identically coloured. This computes what
    the palette was pretending to show, so a bass lane really does read
    violet and a hi-hat lane really does read amber.

    Everything here is plain C++ with no JUCE dependency, including the FFT,
    so the whole analysis is exercised by the test target without standing up
    a plugin - and so the plugin does not have to link juce_dsp for one
    transform.
*/
namespace stemlab::waveform
{
/** Analysis window. 1024 at 44.1k is 23ms and 43Hz per bin - fine enough to
    place a centroid, coarse enough to stay cheap over a whole stem. */
constexpr int spectrumFftOrder = 10;
constexpr int spectrumFftSize = 1 << spectrumFftOrder;

/** Frames are half-overlapped, so colour follows a transient rather than
    smearing a window's worth of neighbouring material over it. */
constexpr int spectrumHop = spectrumFftSize / 2;

/** A long file decimates rather than growing without bound: past this the
    hop stretches, which costs time resolution nobody can see anyway. */
constexpr std::size_t spectrumMaxFrames = 30000;

/** The band the colour ramp spans. Below and above this everything would
    pile up at one end of the hue sweep. */
constexpr double spectrumLowHz = 60.0;
constexpr double spectrumHighHz = 8000.0;

/** Per-frame spectral brightness for one audio file. */
struct SpectralProfile
{
    double lengthSeconds = 0.0;
    double secondsPerFrame = 0.0;

    /** 0 (bass-heavy) to 1 (bright), one entry per analysis frame. */
    std::vector<float> brightness;

    bool isEmpty() const { return brightness.empty(); }
};

/**
 * In-place iterative radix-2 FFT.
 *
 * The input is real, but running it as a complex transform costs a factor of
 * two on a 1024-point window that already takes microseconds, and a
 * real-input specialisation is a great deal more code to get wrong.
 */
inline void forwardFft(std::vector<std::complex<float>>& data)
{
    const std::size_t n = data.size();

    if (n < 2)
        return;

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;

        for (; j & bit; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
            std::swap(data[i], data[j]);
    }

    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * 3.14159265358979323846 / static_cast<double>(len);

        const std::complex<float> step(static_cast<float>(std::cos(angle)),
                                       static_cast<float>(std::sin(angle)));

        for (std::size_t start = 0; start < n; start += len)
        {
            std::complex<float> w(1.0f, 0.0f);

            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto even = data[start + k];
                const auto odd = data[start + k + len / 2] * w;

                data[start + k] = even + odd;
                data[start + k + len / 2] = even - odd;

                w *= step;
            }
        }
    }
}

/**
 * Energy-weighted mean frequency of one magnitude spectrum, in Hz.
 *
 * Bin 0 is skipped: DC offset is not a pitch, and a file with any offset at
 * all would otherwise drag every centroid toward zero. Returns 0 for a frame
 * with no energy, which the caller treats as "no opinion" rather than "bass".
 */
inline double spectralCentroid(const float* magnitudes, int binCount, double binWidthHz)
{
    if (magnitudes == nullptr || binCount < 2)
        return 0.0;

    double weighted = 0.0;
    double total = 0.0;

    for (int bin = 1; bin < binCount; ++bin)
    {
        const double magnitude = static_cast<double>(magnitudes[bin]);

        weighted += magnitude * (static_cast<double>(bin) * binWidthHz);
        total += magnitude;
    }

    if (total <= 1.0e-9)
        return 0.0;

    return weighted / total;
}

/** Place a centroid on the 0..1 colour ramp, logarithmically - an octave is
    an octave whether it sits at 100Hz or at 4kHz. */
inline float brightnessForCentroid(double centroidHz)
{
    if (!(centroidHz > 0.0))
        return 0.5f;

    const double low = std::log(spectrumLowHz);
    const double high = std::log(spectrumHighHz);

    const auto position = (std::log(centroidHz) - low) / (high - low);

    return static_cast<float>(std::clamp(position, 0.0, 1.0));
}

/**
 * Analyse mono samples into a profile.
 *
 * Silent frames inherit the previous frame's brightness rather than
 * collapsing to an arbitrary value: a gap between two hi-hat hits should not
 * flash violet in the middle of an otherwise bright lane.
 */
inline SpectralProfile analyseMono(const float* samples, std::size_t sampleCount,
                                   double sampleRate)
{
    SpectralProfile profile;

    if (samples == nullptr || sampleCount == 0 || !(sampleRate > 0.0))
        return profile;

    profile.lengthSeconds = static_cast<double>(sampleCount) / sampleRate;

    // Stretch the hop rather than the frame: the window stays 1024 wide, so
    // the frequency resolution of a long file matches a short one.
    std::size_t hop = spectrumHop;

    if (sampleCount / hop > spectrumMaxFrames)
        hop = std::max<std::size_t>(spectrumHop, sampleCount / spectrumMaxFrames);

    profile.secondsPerFrame = static_cast<double>(hop) / sampleRate;

    const double binWidth = sampleRate / static_cast<double>(spectrumFftSize);

    // Periodic Hann, computed once and reused across every frame.
    std::vector<float> window(spectrumFftSize);

    for (int i = 0; i < spectrumFftSize; ++i)
    {
        const double phase = 2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                             static_cast<double>(spectrumFftSize);

        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
    }

    std::vector<std::complex<float>> frame(spectrumFftSize);
    std::vector<float> magnitudes(spectrumFftSize / 2 + 1);

    float previousBrightness = 0.5f;

    for (std::size_t start = 0; start < sampleCount; start += hop)
    {
        // A window running past the end is zero-padded rather than dropped,
        // so the tail of a file is coloured like the rest of it.
        for (int i = 0; i < spectrumFftSize; ++i)
        {
            const std::size_t index = start + static_cast<std::size_t>(i);

            const float sample = index < sampleCount ? samples[index] : 0.0f;

            frame[static_cast<std::size_t>(i)] = {sample * window[static_cast<std::size_t>(i)],
                                                  0.0f};
        }

        forwardFft(frame);

        for (std::size_t bin = 0; bin < magnitudes.size(); ++bin)
            magnitudes[bin] = std::abs(frame[bin]);

        const auto centroid =
            spectralCentroid(magnitudes.data(), static_cast<int>(magnitudes.size()), binWidth);

        if (centroid > 0.0)
            previousBrightness = brightnessForCentroid(centroid);

        profile.brightness.push_back(previousBrightness);
    }

    return profile;
}

/*
    Peak envelope.

    The lanes used to sample juce::AudioThumbnail once per drawn bar, per
    repaint - tens of thousands of calls a second across six lanes at the UI
    rate, which is where the sluggishness came from. Reading min and max per
    channel once into a flat array turns painting into indexing.
*/

/** ~2ms per frame: finer than a pixel at the deepest zoom, so columns never
    have to interpolate between frames. */
constexpr double peakSecondsPerFrame = 0.002;

/** Past this a long file coarsens rather than growing without bound. */
constexpr std::size_t peakMaxFrames = 400000;

/** Only the first two channels are drawn, so only those are stored. */
constexpr int peakMaxChannels = 2;

/** Per-frame minimum and maximum sample value, per channel. */
struct PeakEnvelope
{
    double secondsPerFrame = 0.0;
    int channels = 0;

    // Interleaved: frame * channels + channel.
    std::vector<float> minima;
    std::vector<float> maxima;

    std::size_t frames() const
    {
        return channels > 0 ? minima.size() / static_cast<std::size_t>(channels) : 0;
    }

    bool isEmpty() const { return frames() == 0; }
};

struct PeakRange
{
    float minimum = 0.0f;
    float maximum = 0.0f;
};

/**
 * The envelope over a span of time, for one channel.
 *
 * A span shorter than a frame still reads the one frame it lands in, so a
 * fully zoomed-in column draws real audio rather than nothing.
 */
inline PeakRange peakBetween(const PeakEnvelope& envelope, int channel, double startSeconds,
                             double endSeconds)
{
    const auto frameCount = envelope.frames();

    if (frameCount == 0 || !(envelope.secondsPerFrame > 0.0) || channel < 0 ||
        channel >= envelope.channels)
    {
        return {};
    }

    const auto last = static_cast<long long>(frameCount) - 1;

    auto first = static_cast<long long>(std::floor(startSeconds / envelope.secondsPerFrame));
    auto final = static_cast<long long>(std::ceil(endSeconds / envelope.secondsPerFrame)) - 1;

    final = std::max(first, final);

    first = std::clamp<long long>(first, 0, last);
    final = std::clamp<long long>(final, 0, last);

    PeakRange range{envelope.maxima[static_cast<std::size_t>(first) *
                                        static_cast<std::size_t>(envelope.channels) +
                                    static_cast<std::size_t>(channel)],
                    envelope.maxima[static_cast<std::size_t>(first) *
                                        static_cast<std::size_t>(envelope.channels) +
                                    static_cast<std::size_t>(channel)]};

    range.minimum = envelope.minima[static_cast<std::size_t>(first) *
                                        static_cast<std::size_t>(envelope.channels) +
                                    static_cast<std::size_t>(channel)];

    for (auto frame = first + 1; frame <= final; ++frame)
    {
        const auto index = static_cast<std::size_t>(frame) *
                               static_cast<std::size_t>(envelope.channels) +
                           static_cast<std::size_t>(channel);

        range.minimum = std::min(range.minimum, envelope.minima[index]);
        range.maximum = std::max(range.maximum, envelope.maxima[index]);
    }

    return range;
}

/** Reduce de-interleaved channel data to a peak envelope. */
inline PeakEnvelope analysePeaks(const float* const* channelData, int channelCount,
                                 std::size_t sampleCount, double sampleRate)
{
    PeakEnvelope envelope;

    if (channelData == nullptr || channelCount <= 0 || sampleCount == 0 || !(sampleRate > 0.0))
        return envelope;

    envelope.channels = std::min(channelCount, peakMaxChannels);

    auto hop = static_cast<std::size_t>(std::max(1.0, sampleRate * peakSecondsPerFrame));

    if (sampleCount / hop > peakMaxFrames)
        hop = std::max<std::size_t>(hop, sampleCount / peakMaxFrames);

    envelope.secondsPerFrame = static_cast<double>(hop) / sampleRate;

    const auto frames = (sampleCount + hop - 1) / hop;
    const auto slots = frames * static_cast<std::size_t>(envelope.channels);

    envelope.minima.assign(slots, 0.0f);
    envelope.maxima.assign(slots, 0.0f);

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto start = frame * hop;
        const auto stop = std::min(sampleCount, start + hop);

        for (int channel = 0; channel < envelope.channels; ++channel)
        {
            const auto* samples = channelData[channel];

            if (samples == nullptr)
                continue;

            float lowest = samples[start];
            float highest = samples[start];

            for (auto i = start + 1; i < stop; ++i)
            {
                lowest = std::min(lowest, samples[i]);
                highest = std::max(highest, samples[i]);
            }

            const auto index =
                frame * static_cast<std::size_t>(envelope.channels) +
                static_cast<std::size_t>(channel);

            envelope.minima[index] = lowest;
            envelope.maxima[index] = highest;
        }
    }

    return envelope;
}

/** Everything a lane needs to draw one file: shape and colour. */
struct WaveformProfile
{
    double lengthSeconds = 0.0;
    SpectralProfile spectrum;
    PeakEnvelope peaks;

    bool isEmpty() const { return peaks.isEmpty() && spectrum.isEmpty(); }
};

/** Brightness at a time in the file; a neutral 0.5 when there is no profile
    yet, so an unanalysed lane draws in one calm colour rather than flashing. */
inline float brightnessAt(const SpectralProfile& profile, double seconds)
{
    if (profile.isEmpty() || !(profile.secondsPerFrame > 0.0))
        return 0.5f;

    const auto frame = static_cast<long long>(seconds / profile.secondsPerFrame);

    const auto clamped = std::clamp<long long>(
        frame, 0, static_cast<long long>(profile.brightness.size()) - 1);

    return profile.brightness[static_cast<std::size_t>(clamped)];
}
}
