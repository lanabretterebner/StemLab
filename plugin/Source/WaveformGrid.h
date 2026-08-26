#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace stemlab::waveform
{
constexpr int defaultLaneHeight = 58;

inline int clampLaneHeight(int height) { return std::clamp(height, 42, 180); }

/** The span of a file a lane draws, in seconds. */
struct ViewWindow
{
    double start = 0.0;
    double end = 0.0;

    double length() const { return end - start; }
};

/**
 * Which seconds of a file a lane shows at a given zoom.
 *
 * zoom 1 is the whole file. Above that the window is centred on the
 * playhead and clamped to the file, so it stops following at either end
 * rather than scrolling past into empty space. playheadNormalised is a
 * fraction of the file, not seconds: lanes and the transport can disagree
 * on length by a block or two, and every lane has to land on one window.
 */
inline ViewWindow visibleWindow(double totalLengthSeconds, double zoom,
                                double playheadNormalised)
{
    if (!(totalLengthSeconds > 0.0))
        return {0.0, 0.0};

    const auto span = totalLengthSeconds / std::max(1.0, zoom);

    if (span >= totalLengthSeconds)
        return {0.0, totalLengthSeconds};

    const auto centre = std::clamp(playheadNormalised, 0.0, 1.0) * totalLengthSeconds;

    const auto start = std::clamp(centre - span * 0.5, 0.0, totalLengthSeconds - span);

    return {start, start + span};
}

enum class GridLineKind
{
    subdivision,
    beat,
    bar
};

struct GridLine
{
    double seconds = 0.0;
    GridLineKind kind = GridLineKind::beat;
    int barNumber = 0;
};

struct GridRequest
{
    double visibleStart = 0.0;
    double visibleEnd = 1.0;
    int pixelWidth = 1;
    double bpm = 120.0;
    int numerator = 4;
    int denominator = 4;
    double barOne = 0.0;
    bool useDetectedBeats = false;
    std::vector<double> beats;
    std::vector<double> downbeats;
};

inline double timeToPixel(double seconds, double visibleStart, double visibleEnd, int pixelWidth)
{
    const auto span = std::max(1.0e-9, visibleEnd - visibleStart);
    return (seconds - visibleStart) / span * std::max(1, pixelWidth);
}

inline double pixelToTime(double pixel, double visibleStart, double visibleEnd, int pixelWidth)
{
    const auto width = std::max(1, pixelWidth);
    return visibleStart + pixel / width * (visibleEnd - visibleStart);
}

inline bool nearAny(const std::vector<double>& sortedValues, double target, double tolerance)
{
    const auto found = std::lower_bound(sortedValues.begin(), sortedValues.end(), target - tolerance);
    return found != sortedValues.end() && std::abs(*found - target) <= tolerance;
}

inline std::vector<GridLine> makeGridLines(const GridRequest& request)
{
    std::vector<GridLine> lines;
    const auto span = request.visibleEnd - request.visibleStart;
    if (span <= 0.0 || request.pixelWidth <= 0 || request.bpm <= 0.0)
        return lines;

    const auto numerator = std::max(1, request.numerator);
    const auto denominator = std::max(1, request.denominator);
    const auto beatDuration = 60.0 / request.bpm * 4.0 / denominator;
    const auto pixelsPerBeat = beatDuration / span * request.pixelWidth;
    const int subdivisions = pixelsPerBeat >= 120.0 ? 4 : (pixelsPerBeat >= 60.0 ? 2 : 1);

    if (request.useDetectedBeats && !request.beats.empty())
    {
        if (request.downbeats.empty())
        {
            for (size_t index = 0; index < request.beats.size(); index += numerator)
            {
                const auto seconds = request.beats[index];
                if (seconds >= request.visibleStart && seconds <= request.visibleEnd)
                    lines.push_back(
                        {seconds, GridLineKind::bar, static_cast<int>(index / numerator) + 1});
            }
        }
        else
        {
            auto downbeat = std::lower_bound(request.downbeats.begin(), request.downbeats.end(),
                                             request.visibleStart);
            while (downbeat != request.downbeats.end() && *downbeat <= request.visibleEnd)
            {
                lines.push_back(
                    {*downbeat, GridLineKind::bar,
                     static_cast<int>(std::distance(request.downbeats.begin(), downbeat)) + 1});
                ++downbeat;
            }
        }

        auto beat = std::lower_bound(request.beats.begin(), request.beats.end(),
                                     request.visibleStart - beatDuration);
        while (beat != request.beats.end() && *beat <= request.visibleEnd)
        {
            const auto current = *beat;
            const auto next = std::next(beat);
            if (pixelsPerBeat >= 9.0 && !nearAny(request.downbeats, current, 0.04))
                lines.push_back({current, GridLineKind::beat, 0});
            if (subdivisions > 1 && next != request.beats.end())
            {
                const auto interval = *next - current;
                for (int division = 1; division < subdivisions; ++division)
                {
                    const auto value = current + interval * division / subdivisions;
                    if (value >= request.visibleStart && value <= request.visibleEnd)
                        lines.push_back({value, GridLineKind::subdivision, 0});
                }
            }
            ++beat;
        }
    }
    else
    {
        const auto first = static_cast<std::int64_t>(
                               std::floor((request.visibleStart - request.barOne) / beatDuration)) -
                           1;
        const auto last = static_cast<std::int64_t>(
                              std::ceil((request.visibleEnd - request.barOne) / beatDuration)) +
                          1;
        for (auto beatIndex = first; beatIndex <= last; ++beatIndex)
        {
            const auto seconds = request.barOne + static_cast<double>(beatIndex) * beatDuration;
            if (seconds < request.visibleStart || seconds > request.visibleEnd)
                continue;
            const bool isBar = ((beatIndex % numerator) + numerator) % numerator == 0;
            if (isBar)
            {
                const auto barIndex = static_cast<int>(std::floor(
                    static_cast<double>(beatIndex) / static_cast<double>(numerator)));
                lines.push_back({seconds, GridLineKind::bar, barIndex >= 0 ? barIndex + 1 : 0});
            }
            else if (pixelsPerBeat >= 9.0)
            {
                lines.push_back({seconds, GridLineKind::beat, 0});
            }

            if (subdivisions > 1)
            {
                for (int division = 1; division < subdivisions; ++division)
                {
                    const auto value = seconds + beatDuration * division / subdivisions;
                    if (value >= request.visibleStart && value <= request.visibleEnd)
                        lines.push_back({value, GridLineKind::subdivision, 0});
                }
            }
        }
    }

    std::sort(lines.begin(), lines.end(), [](const auto& left, const auto& right)
              {
                  if (left.seconds != right.seconds)
                      return left.seconds < right.seconds;
                  return static_cast<int>(left.kind) > static_cast<int>(right.kind);
              });
    lines.erase(std::unique(lines.begin(), lines.end(), [](const auto& left, const auto& right)
                            { return std::abs(left.seconds - right.seconds) < 1.0e-6; }),
                lines.end());
    return lines;
}
} // namespace stemlab::waveform
