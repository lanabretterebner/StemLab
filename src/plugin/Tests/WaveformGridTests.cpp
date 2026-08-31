#include "WaveformGrid.h"
#include "HostIntegrationPolicy.h"

#include <cassert>
#include <algorithm>
#include <cmath>

using namespace stemlab::waveform;

int main()
{
    const auto pixel = timeToPixel(12.5, 10.0, 20.0, 1000);
    assert(std::abs(pixel - 250.0) < 1.0e-9);
    assert(std::abs(pixelToTime(pixel, 10.0, 20.0, 1000) - 12.5) < 1.0e-9);

    // A tempo of zero means "no grid", and is how an unanalysed source and a
    // grid switched off both reach the painter. Drawing anything for it was
    // the bug: a source nothing had measured still showed a confident grid.
    GridRequest none;
    none.visibleEnd = 120.0;
    none.pixelWidth = 600;
    none.bpm = 0.0;
    assert(makeGridLines(none).empty());

    none.bpm = -1.0;
    assert(makeGridLines(none).empty());

    // Detected beats do not resurrect it: a caller that has switched the grid
    // off keeps it off whatever else it is carrying.
    none.bpm = 0.0;
    none.useDetectedBeats = true;
    none.beats = {0.5, 1.0, 1.5, 2.0};
    assert(makeGridLines(none).empty());

    GridRequest far;
    far.visibleEnd = 120.0;
    far.pixelWidth = 600;
    const auto farLines = makeGridLines(far);
    assert(!farLines.empty());
    for (const auto& line : farLines)
        assert(line.kind == GridLineKind::bar);

    auto medium = far;
    medium.visibleEnd = 20.0;
    const auto mediumLines = makeGridLines(medium);
    assert(std::any_of(mediumLines.begin(), mediumLines.end(),
                       [](const auto& line) { return line.kind == GridLineKind::beat; }));

    auto close = far;
    close.visibleEnd = 3.0;
    const auto closeLines = makeGridLines(close);
    assert(std::any_of(closeLines.begin(), closeLines.end(),
                       [](const auto& line) { return line.kind == GridLineKind::subdivision; }));

    GridRequest threeFour;
    threeFour.visibleEnd = 6.1;
    threeFour.numerator = 3;
    const auto threeFourLines = makeGridLines(threeFour);
    assert(std::any_of(threeFourLines.begin(), threeFourLines.end(), [](const auto& line)
                       { return line.kind == GridLineKind::bar && line.barNumber == 2 &&
                                std::abs(line.seconds - 1.5) < 1.0e-9; }));

    GridRequest source;
    source.visibleEnd = 3.0;
    source.pixelWidth = 600;
    source.useDetectedBeats = true;
    source.beats = {0.1, 0.6, 1.1, 1.6, 2.1, 2.6};
    source.downbeats = {0.1, 1.6};
    const auto sourceLines = makeGridLines(source);
    assert(std::any_of(sourceLines.begin(), sourceLines.end(), [](const auto& line)
                       { return line.kind == GridLineKind::bar && line.barNumber == 2 &&
                                std::abs(line.seconds - 1.6) < 1.0e-9; }));

    // ------------------------------------------------------- zoom windows

    const auto sameTime = [](double a, double b) { return std::abs(a - b) < 1.0e-9; };

    // Zoom 1 (and anything below it) is the whole file, wherever the
    // playhead is.
    for (const double zoom : {1.0, 0.5, 0.0})
    {
        const auto whole = visibleWindow(200.0, zoom, 0.75);
        assert(sameTime(whole.start, 0.0) && sameTime(whole.end, 200.0));
    }

    // Centred on the playhead in the middle of the file.
    const auto centred = visibleWindow(200.0, 4.0, 0.5);
    assert(sameTime(centred.start, 75.0) && sameTime(centred.end, 125.0));
    assert(sameTime(centred.length(), 50.0));

    // Clamped at the head rather than starting before the file.
    const auto atStart = visibleWindow(200.0, 4.0, 0.0);
    assert(sameTime(atStart.start, 0.0) && sameTime(atStart.end, 50.0));

    // Clamped at the tail rather than running past the end.
    const auto atEnd = visibleWindow(200.0, 4.0, 1.0);
    assert(sameTime(atEnd.start, 150.0) && sameTime(atEnd.end, 200.0));

    // A window is always the same length wherever it is clamped, so the
    // lanes do not stretch as the playhead reaches either end.
    for (const double where : {0.0, 0.01, 0.5, 0.99, 1.0})
        assert(sameTime(visibleWindow(200.0, 8.0, where).length(), 25.0));

    // Out-of-range playheads clamp rather than escaping the file.
    assert(sameTime(visibleWindow(200.0, 4.0, -3.0).start, 0.0));
    assert(sameTime(visibleWindow(200.0, 4.0, 9.0).end, 200.0));

    // An empty or nonsense file is an empty window, not a divide by zero.
    assert(sameTime(visibleWindow(0.0, 4.0, 0.5).length(), 0.0));
    assert(sameTime(visibleWindow(-5.0, 4.0, 0.5).length(), 0.0));

    assert(clampLaneHeight(1) == 42);
    assert(clampLaneHeight(defaultLaneHeight) == defaultLaneHeight);
    assert(clampLaneHeight(999) == 180);

    /*  The playhead must never move backwards while the transport moves
        forwards.

        A zoomed view is centred on the playhead, so mid-file the playhead
        belongs at one fixed point in the well. Measured from the snapped
        origin it did not stay there: the floor lags the true start by a
        fraction of a column that grows each frame and resets when the snap
        catches up, so the playhead crawled backwards and jumped forward
        about a pixel - invisible at 1x, where the window never scrolls, and
        plainly jittery zoomed in.

        This walks a transport forward at the UI's own rate and asserts the
        two origins behave the way the header says they do.
    */
    {
        constexpr double total = 200.0, columns = 486.0, tick = 0.05;

        for (const double zoom : {8.0, 32.0})
        {
            const auto span = total / zoom;

            auto backwardsFrom = [&](bool snapped)
            {
                auto worst = 0.0;
                auto previous = -1.0e18;

                for (int step = 0; step < 200; ++step)
                {
                    const auto position = 40.0 + tick * step;
                    const auto window =
                        stemlab::waveform::visibleWindow(total, zoom, position / total);

                    const auto origin =
                        snapped ? stemlab::waveform::snappedViewStart(window.start, span, columns)
                                : window.start;

                    const auto x = (position - origin) / span * columns;

                    worst = std::min(worst, x - previous);
                    previous = x;
                }

                return worst;
            };

            // The true origin holds the playhead still, to well inside a
            // pixel. Nothing about it may drift backwards.
            if (!(backwardsFrom(false) > -1.0e-9))
                return 1;

            // And the snapped origin is why this test exists: it goes
            // backwards by a visible fraction of a pixel, every frame.
            if (!(backwardsFrom(true) < -0.01))
                return 1;
        }
    }

    using stemlab::host::UiMode;
    if (stemlab::host::captureActionText(UiMode::ableton) != "Use Live Clip" ||
        stemlab::host::completedStemActionText(UiMode::ableton) != "Send Selected" ||
        stemlab::host::captureActionText(UiMode::genericVst) != "Capture Host" ||
        stemlab::host::completedStemActionText(UiMode::genericVst) != "Drag Selected" ||
        stemlab::host::showsImportFromPc(UiMode::standalone) ||
        stemlab::host::showsImportFromPc(UiMode::ableton) ||
        !stemlab::host::showsImportFromPc(UiMode::genericVst) ||
        !stemlab::host::canStartHostAudioCapture(UiMode::genericVst, false, false) ||
        stemlab::host::canStartHostAudioCapture(UiMode::genericVst, true, false) ||
        stemlab::host::canStartHostAudioCapture(UiMode::genericVst, false, true) ||
        stemlab::host::canStartHostAudioCapture(UiMode::ableton, false, false))
        return 1;
    return 0;
}
