#include "WaveformGrid.h"

#include <cassert>
#include <cmath>

using namespace stemlab::waveform;

int main()
{
    const auto pixel = timeToPixel(12.5, 10.0, 20.0, 1000);
    assert(std::abs(pixel - 250.0) < 1.0e-9);
    assert(std::abs(pixelToTime(pixel, 10.0, 20.0, 1000) - 12.5) < 1.0e-9);

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
    return 0;
}
