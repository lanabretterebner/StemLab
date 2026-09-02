#pragma once

namespace stemlab::host
{
enum class UiMode
{
    standalone,
    ableton,
    genericVst
};

/*
 * This header used to carry three more helpers naming the capture button,
 * the completed-stem button and whether "Import from PC" shows. Nothing
 * called them: the editor builds that copy inline and had already drifted
 * away from the strings here, so the header was a second, quietly wrong
 * answer to the same question. Put UI text back here only alongside the
 * editor call that reads it.
 */

constexpr bool canStartHostAudioCapture(UiMode mode, bool captureActive,
                                        bool separationRunning) noexcept
{
    return mode == UiMode::genericVst && !captureActive && !separationRunning;
}
} // namespace stemlab::host
