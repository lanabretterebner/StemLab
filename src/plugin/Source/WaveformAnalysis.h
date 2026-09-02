#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

/*
    Spectral coloring for the lane waveforms.

    The "Spectrum" palette used to be a fixed violet-to-amber sweep across
    the lane's width: pretty, and completely unrelated to the audio - a sine
    tone and a drum loop came out identically colored. This computes what
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

/** Frames are half-overlapped, so color follows a transient rather than
    smearing a window's worth of neighbouring material over it. */
constexpr int spectrumHop = spectrumFftSize / 2;

/** A long file decimates rather than growing without bound: past this the
    hop stretches, which costs time resolution nobody can see anyway. */
constexpr std::size_t spectrumMaxFrames = 30000;

/** The band the color ramp spans. Below and above this everything would
    pile up at one end of the hue sweep. */
constexpr double spectrumLowHz = 60.0;
constexpr double spectrumHighHz = 8000.0;

/** Crossovers for the three-band split behind the RGB and 3-Band palettes:
    kick and bass below, vocals and most instruments in the middle, cymbals
    and air on top - the classic DJ-waveform carve. */
constexpr double bandLowCrossoverHz = 200.0;
constexpr double bandHighCrossoverHz = 2000.0;

/** One frame's low/mid/high balance, each 0..1 with the dominant band at 1
    - shares of the loudest band, not absolute levels, because color only
    needs the ratio and painting then never has to renormalise. */
struct BandLevels
{
    float low = 1.0f;
    float mid = 1.0f;
    float high = 1.0f;
};

/** Per-frame spectral brightness and band balance for one audio file. */
struct SpectralProfile
{
    double lengthSeconds = 0.0;
    double secondsPerFrame = 0.0;

    /** 0 (bass-heavy) to 1 (bright), one entry per analysis frame. */
    std::vector<float> brightness;

    /** Band shares, one entry per analysis frame, aligned with brightness. */
    std::vector<BandLevels> bands;

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
 * all would otherwise drag every centroid toward zero. Skipping it is not by
 * itself enough on a windowed transform - part of the offset lands in bin 1
 * as well - so MonoSpectrumScanner cancels that leak before calling here.
 * Returns 0 for a frame with no energy, which the caller treats as "no
 * opinion" rather than "bass".
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

/**
 * Low/mid/high balance of one magnitude spectrum.
 *
 * Energy is summed per band (bin 0 skipped, as for the centroid) and scaled
 * so the loudest band reads 1. The ratios come back through a square root:
 * color tracks amplitude, not power, so a band 6dB down reads half as
 * strong rather than a quarter. A frame with no energy returns all zeros,
 * which the caller treats as "no opinion" rather than as a color.
 */
inline BandLevels bandLevelsForSpectrum(const float* magnitudes, int binCount,
                                        double binWidthHz)
{
    if (magnitudes == nullptr || binCount < 2 || !(binWidthHz > 0.0))
        return {0.0f, 0.0f, 0.0f};

    double energy[3] = {0.0, 0.0, 0.0};

    for (int bin = 1; bin < binCount; ++bin)
    {
        const double hz = static_cast<double>(bin) * binWidthHz;
        const double magnitude = static_cast<double>(magnitudes[bin]);

        const int band = hz < bandLowCrossoverHz ? 0 : hz < bandHighCrossoverHz ? 1 : 2;

        energy[band] += magnitude * magnitude;
    }

    const auto top = std::max({energy[0], energy[1], energy[2]});

    if (top <= 1.0e-12)
        return {0.0f, 0.0f, 0.0f};

    return {static_cast<float>(std::sqrt(energy[0] / top)),
            static_cast<float>(std::sqrt(energy[1] / top)),
            static_cast<float>(std::sqrt(energy[2] / top))};
}

/** Place a centroid on the 0..1 color ramp, logarithmically - an octave is
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
 * Spectrum analysis over samples arriving a piece at a time.
 *
 * The windows are the only samples any of this ever looks at: the hop
 * stretches so that no more than spectrumMaxFrames of them exist, and nothing
 * outside one is read. A caller reading a file in blocks can therefore feed
 * the windows as the blocks arrive, and never has to hold the file - or a
 * mono copy of it - in memory, which for an hour-long capture is the
 * difference between a kilobyte and half a gigabyte.
 *
 * The sample count and rate are wanted up front because the hop is derived
 * from them; both are known before a byte is read.
 *
 * Silent frames inherit the previous frame's brightness rather than
 * collapsing to an arbitrary value: a gap between two hi-hat hits should not
 * flash violet in the middle of an otherwise bright lane. A frame that is
 * nothing but a DC offset is one of those gaps, and is read as one.
 */
class MonoSpectrumScanner
{
public:
    MonoSpectrumScanner(std::size_t sampleCount, double sampleRate)
    {
        if (sampleCount == 0 || !(sampleRate > 0.0))
            return;

        profile.lengthSeconds = static_cast<double>(sampleCount) / sampleRate;

        // Stretch the hop rather than the frame: the window stays 1024 wide,
        // so the frequency resolution of a long file matches a short one.
        if (sampleCount / hop > spectrumMaxFrames)
            hop = std::max<std::size_t>(spectrumHop, sampleCount / spectrumMaxFrames);

        profile.secondsPerFrame = static_cast<double>(hop) / sampleRate;

        frameCount = (sampleCount + hop - 1) / hop;
        binWidth = sampleRate / static_cast<double>(spectrumFftSize);

        // Periodic Hann, computed once and reused across every frame.
        window.resize(windowSize);

        for (std::size_t i = 0; i < windowSize; ++i)
        {
            const double phase = 2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                                 static_cast<double>(windowSize);

            window[i] = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
        }

        frame.resize(windowSize);
        magnitudes.resize(windowSize / 2 + 1);
        pending.reserve(windowSize);
    }

    /** Index in the file of the first sample the next window still wants.
        Anything before it has either been analysed or falls in the gap
        between two windows, which a stretched hop never reads. */
    std::size_t nextSample() const { return framesDone * hop + pending.size(); }

    /** How many samples from nextSample() the next window still wants; zero
        once every frame has been analysed. */
    std::size_t samplesWanted() const
    {
        return framesDone < frameCount ? windowSize - pending.size() : 0;
    }

    /**
     * Feed samples in file order, position being the file index of samples[0].
     *
     * The span may be a whole read block or exactly the samplesWanted() from
     * nextSample(): what lies between windows is dropped either way. A window
     * spanning two blocks is held across the calls rather than restarted, so
     * pushes have to run forward and may not skip what a window wants - a
     * span starting past nextSample() is refused rather than misassembled.
     */
    void push(std::size_t position, const float* samples, std::size_t count)
    {
        while (samples != nullptr && count > 0 && framesDone < frameCount)
        {
            const auto start = nextSample();

            if (position + count <= start || position > start)
                return;

            const auto skipped = start - position;

            samples += skipped;
            position = start;
            count -= skipped;

            const auto taken = std::min(count, windowSize - pending.size());

            pending.insert(pending.end(), samples, samples + taken);

            samples += taken;
            position += taken;
            count -= taken;

            if (pending.size() == windowSize)
                analyseWindow();
        }
    }

    /** The finished profile. A window running past the end of the file is
        zero-padded rather than dropped, so the tail of a file is colored
        like the rest of it. */
    SpectralProfile finish()
    {
        while (framesDone < frameCount)
        {
            pending.resize(windowSize, 0.0f);
            analyseWindow();
        }

        return std::move(profile);
    }

private:
    static constexpr std::size_t windowSize = static_cast<std::size_t>(spectrumFftSize);

    /** Peak-to-peak below this - about -100dBFS - is a gap rather than
        audio, and carries no color anyone could see. */
    static constexpr float silenceAmplitude = 1.0e-5f;

    void analyseWindow()
    {
        auto lowest = pending[0];
        auto highest = pending[0];

        for (std::size_t i = 0; i < windowSize; ++i)
        {
            lowest = std::min(lowest, pending[i]);
            highest = std::max(highest, pending[i]);

            frame[i] = {pending[i] * window[i], 0.0f};
        }

        forwardFft(frame);

        /*  Take the offset out of the spectrum, not just out of bin 0.

            A periodic Hann window spreads a constant across exactly three
            bins: half its weight into bin 0, and a quarter of it, negated,
            into each of bins +-1. Skipping bin 0 therefore removes only half
            of a DC offset - the other half sits in bin 1 at 43Hz, where it
            reads as deep bass and drags the frame's centroid to the bottom
            of the ramp. Folding half of bin 0 back into bin 1 cancels that
            leak exactly, and costs a frame with real content nothing beyond
            the offset it does carry.
        */
        frame[1] += 0.5f * frame[0];
        frame[0] = {0.0f, 0.0f};

        for (std::size_t bin = 0; bin < magnitudes.size(); ++bin)
            magnitudes[bin] = std::abs(frame[bin]);

        /*  A window whose samples never move is a gap whatever offset it
            sits at, and after the fold above all that is left of it is the
            transform's own round-off. Asking that for a color would answer
            with noise, so silence is settled here rather than by the energy
            test inside the two helpers, which at this scale only ever
            catches an exactly zero frame.
        */
        const auto silent = (highest - lowest) <= silenceAmplitude;

        const auto centroid =
            silent ? 0.0
                   : spectralCentroid(magnitudes.data(),
                                      static_cast<int>(magnitudes.size()), binWidth);

        if (centroid > 0.0)
            previousBrightness = brightnessForCentroid(centroid);

        profile.brightness.push_back(previousBrightness);

        const auto bands =
            silent ? BandLevels{0.0f, 0.0f, 0.0f}
                   : bandLevelsForSpectrum(magnitudes.data(),
                                           static_cast<int>(magnitudes.size()), binWidth);

        if (bands.low > 0.0f || bands.mid > 0.0f || bands.high > 0.0f)
            previousBands = bands;

        profile.bands.push_back(previousBands);

        ++framesDone;

        // Half-overlapped windows share samples: the tail of this one opens
        // the next, and only a hop longer than the window leaves a gap.
        if (hop < windowSize)
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(hop));
        else
            pending.clear();
    }

    std::size_t hop = static_cast<std::size_t>(spectrumHop);
    std::size_t frameCount = 0;
    std::size_t framesDone = 0;
    double binWidth = 0.0;

    std::vector<float> window;
    std::vector<float> pending;
    std::vector<std::complex<float>> frame;
    std::vector<float> magnitudes;

    SpectralProfile profile;

    // Silent frames inherit both measures, same reasoning for both: a gap
    // between two hi-hat hits must not flash a wrong color.
    float previousBrightness = 0.5f;
    BandLevels previousBands;
};

/**
 * Analyse mono samples into a profile.
 *
 * The whole-buffer form of MonoSpectrumScanner, for callers that already hold
 * the audio; one that reads a file in blocks should drive the scanner instead
 * of assembling a buffer to pass here.
 */
inline SpectralProfile analyseMono(const float* samples, std::size_t sampleCount,
                                   double sampleRate)
{
    if (samples == nullptr || sampleCount == 0 || !(sampleRate > 0.0))
        return {};

    MonoSpectrumScanner scanner(sampleCount, sampleRate);

    scanner.push(0, samples, sampleCount);

    return scanner.finish();
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

/** Everything a lane needs to draw one file: shape and color. */
struct WaveformProfile
{
    double lengthSeconds = 0.0;
    SpectralProfile spectrum;
    PeakEnvelope peaks;

    bool isEmpty() const { return peaks.isEmpty() && spectrum.isEmpty(); }
};

/** Brightness at a time in the file; a neutral 0.5 when there is no profile
    yet, so an unanalysed lane draws in one calm color rather than flashing. */
inline float brightnessAt(const SpectralProfile& profile, double seconds)
{
    if (profile.isEmpty() || !(profile.secondsPerFrame > 0.0))
        return 0.5f;

    const auto frame = static_cast<long long>(seconds / profile.secondsPerFrame);

    const auto clamped = std::clamp<long long>(
        frame, 0, static_cast<long long>(profile.brightness.size()) - 1);

    return profile.brightness[static_cast<std::size_t>(clamped)];
}

/** Band balance at a time in the file; the neutral default (every band at 1)
    when there is no profile yet, so an unanalysed lane draws in one calm
    color rather than flashing. */
inline BandLevels bandsAt(const SpectralProfile& profile, double seconds)
{
    if (profile.bands.empty() || !(profile.secondsPerFrame > 0.0))
        return {};

    const auto frame = static_cast<long long>(seconds / profile.secondsPerFrame);

    const auto clamped =
        std::clamp<long long>(frame, 0, static_cast<long long>(profile.bands.size()) - 1);

    return profile.bands[static_cast<std::size_t>(clamped)];
}
}
