#pragma once

#include <string_view>

namespace stemlab::host
{
enum class UiMode
{
    standalone,
    ableton,
    genericVst
};

constexpr std::string_view captureActionText(UiMode mode) noexcept
{
    switch (mode)
    {
    case UiMode::standalone:
        return "Select File";
    case UiMode::ableton:
        return "Use Live Clip";
    case UiMode::genericVst:
        return "Capture Host";
    }

    return {};
}

constexpr std::string_view completedStemActionText(UiMode mode) noexcept
{
    switch (mode)
    {
    case UiMode::standalone:
        return "Save Selected...";
    case UiMode::ableton:
        return "Send Selected";
    case UiMode::genericVst:
        return "Drag Selected";
    }

    return {};
}

constexpr bool showsImportFromPc(UiMode mode) noexcept
{
    return mode == UiMode::genericVst;
}

constexpr bool canStartHostAudioCapture(UiMode mode, bool captureActive,
                                        bool separationRunning) noexcept
{
    return mode == UiMode::genericVst && !captureActive && !separationRunning;
}
} // namespace stemlab::host
