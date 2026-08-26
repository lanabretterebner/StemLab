#include "WaveformGrid.h"
#include "HostIntegrationPolicy.h"

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

    assert(clampLaneHeight(1) == 42);
    assert(clampLaneHeight(defaultLaneHeight) == defaultLaneHeight);
    assert(clampLaneHeight(999) == 180);

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
