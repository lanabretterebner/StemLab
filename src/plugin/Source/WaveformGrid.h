#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
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

/**
 * A view start floored to a whole column of time.
 *
 * A scrolling view has to re-bucket the audio into the same columns every
 * frame or the whole waveform crawls, so the cached column image is drawn
 * from this rather than from the true start.
 *
 * It is only ever an origin for the picture. Anything placed by absolute
 * time - the playhead above all - must use the true start: the floor lags
 * it by a fraction of a column that grows every frame and resets when the
 * snap catches up, so a playhead measured from here creeps backwards and
 * jumps forward instead of moving. See the test beside this.
 */
inline double snappedViewStart(double viewStart, double viewLength, double columns)
{
    if (!(viewLength > 0.0) || !(columns > 0.0))
        return viewStart;

    const auto secondsPerColumn = viewLength / columns;

    return std::floor(viewStart / secondsPerColumn) * secondsPerColumn;
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

    /** 1-based, and counting backwards before bar one rather than wrapping.
        Filled for bars and beats; 0 on a subdivision. */
    int barNumber = 0;

    /** 0-based position of this beat inside its bar, so a label can read
        "bar.beat". 0 on a bar line (which is beat one) and on a
        subdivision. */
    int beatInBar = 0;
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

    /*  Spans, not vectors: a lane builds one of these on every paint and a
        long track carries thousands of beats. The caller owns the storage
        and must outlive the call - in the editor that is the snapshot the
        lane is already holding a pointer to.
    */
    std::span<const double> beats;
    std::span<const double> downbeats;
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

inline bool nearAny(std::span<const double> sortedValues, double target, double tolerance)
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
        /*
         * Ruled from the beats themselves, which is what makes a track whose
         * tempo moves rule correctly: the spacing between lines is whatever
         * the analysis measured there, so nothing here has to know that the
         * tempo changed, or where.
         *
         * beatDuration above is still used, but only as a zoom threshold -
         * how much room a beat has on screen - never as a position.
         */
        const auto haveDownbeats = !request.downbeats.empty();

        /*  Which bar a beat belongs to, and where it sits inside it.

            With downbeats, the bar is the last one at or before the beat and
            the position is how many beats have passed since it - so a bar of
            an odd length, or a meter the analysis read differently from the
            setting, still numbers its own beats. Without them the meter is
            all there is to go on.
        */
        const auto numbering = [&](size_t beatIndex) -> std::pair<int, int>
        {
            if (!haveDownbeats)
            {
                const auto index = static_cast<long long>(beatIndex);
                const auto bar = index / numerator;
                return {static_cast<int>(bar) + 1, static_cast<int>(index % numerator)};
            }

            const auto seconds = request.beats[beatIndex];

            const auto after = std::upper_bound(request.downbeats.begin(),
                                                request.downbeats.end(), seconds + 1.0e-6);

            if (after == request.downbeats.begin())
            {
                // Before the first downbeat: count backwards from it, so the
                // pickup beats belong to bar 0, -1 ... rather than to bar 1.
                const auto firstDownbeatIndex = static_cast<long long>(std::distance(
                    request.beats.begin(),
                    std::lower_bound(request.beats.begin(), request.beats.end(),
                                     request.downbeats.front() - 1.0e-6)));

                const auto offset = firstDownbeatIndex - static_cast<long long>(beatIndex);
                const auto barsBack = (offset + numerator - 1) / numerator;

                return {static_cast<int>(1 - barsBack),
                        static_cast<int>(((numerator - offset % numerator) % numerator))};
            }

            const auto barIndex = std::distance(request.downbeats.begin(), after) - 1;

            const auto barStart = static_cast<long long>(std::distance(
                request.beats.begin(),
                std::lower_bound(request.beats.begin(), request.beats.end(),
                                 request.downbeats[static_cast<size_t>(barIndex)] - 1.0e-6)));

            return {static_cast<int>(barIndex) + 1,
                    static_cast<int>(static_cast<long long>(beatIndex) - barStart)};
        };

        auto first = std::lower_bound(request.beats.begin(), request.beats.end(),
                                      request.visibleStart - beatDuration);

        for (auto beat = first; beat != request.beats.end() && *beat <= request.visibleEnd; ++beat)
        {
            const auto index = static_cast<size_t>(std::distance(request.beats.begin(), beat));
            const auto current = *beat;

            const auto [barNumber, beatInBar] = numbering(index);

            const bool isBar = haveDownbeats ? nearAny(request.downbeats, current, 0.04)
                                             : beatInBar == 0;

            if (current >= request.visibleStart && current <= request.visibleEnd)
            {
                if (isBar)
                    lines.push_back({current, GridLineKind::bar, barNumber, 0});
                else if (pixelsPerBeat >= 9.0)
                    lines.push_back({current, GridLineKind::beat, barNumber, beatInBar});
            }

            const auto next = std::next(beat);

            if (subdivisions > 1 && next != request.beats.end())
            {
                // Split the real interval to the next beat, not a nominal
                // one: on a track that drifts the two are not the same.
                const auto interval = *next - current;

                for (int division = 1; division < subdivisions; ++division)
                {
                    const auto value = current + interval * division / subdivisions;
                    if (value >= request.visibleStart && value <= request.visibleEnd)
                        lines.push_back({value, GridLineKind::subdivision, 0, 0});
                }
            }
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
            const int withinBar = static_cast<int>(((beatIndex % numerator) + numerator)
                                                    % numerator);

            const auto barIndex = static_cast<int>(std::floor(
                static_cast<double>(beatIndex) / static_cast<double>(numerator)));

            if (withinBar == 0)
                lines.push_back({seconds, GridLineKind::bar, barIndex + 1, 0});
            else if (pixelsPerBeat >= 9.0)
                lines.push_back({seconds, GridLineKind::beat, barIndex + 1, withinBar});

            if (subdivisions > 1)
            {
                for (int division = 1; division < subdivisions; ++division)
                {
                    const auto value = seconds + beatDuration * division / subdivisions;
                    if (value >= request.visibleStart && value <= request.visibleEnd)
                        lines.push_back({value, GridLineKind::subdivision, 0, 0});
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
