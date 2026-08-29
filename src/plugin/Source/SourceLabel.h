#pragma once

/**
 * How a captured source is named for the user, and for the files that come
 * out of it.
 *
 * Both DAW bridges build the label the same way: a track name joined to the
 * name of the take or clip inside it. That reads well when the two say
 * different things ("Drums / Kick 03"), but a DAW that just imported
 * "Song.wav" typically names the track after the file and the take after the
 * file with its extension dropped. The honest join then reads
 * "Song.wav / Song" - the same name twice - and because the label also seeds
 * exported stem names, it turns up on every stem as well as in the window.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace stemlab::source
{
namespace detail
{
inline std::string trim(std::string_view text)
{
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };

    const auto begin = std::find_if(text.begin(), text.end(), notSpace);
    const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();

    return begin < end ? std::string(begin, end) : std::string{};
}

inline bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

/** Drop a trailing extension, but only one this application would have
    opened. A track legitimately called "Mix v1.2" or "Take 2.0" must keep
    everything after its dot; only a real audio suffix is noise here. */
inline std::string withoutAudioExtension(std::string_view name)
{
    static constexpr std::array<std::string_view, 12> audioExtensions{
        "wav", "wave", "flac", "aif", "aiff", "aifc", "mp3", "m4a", "ogg", "oga", "opus", "aac"};

    const auto trimmed = trim(name);
    const auto dot = trimmed.find_last_of('.');

    // A leading dot is the whole name, not a suffix: ".wav" keeps its name.
    if (dot == std::string::npos || dot == 0 || dot + 1 >= trimmed.size())
        return trimmed;

    const std::string_view suffix{trimmed.data() + dot + 1, trimmed.size() - dot - 1};

    const bool isAudio =
        std::any_of(audioExtensions.begin(), audioExtensions.end(),
                    [suffix](std::string_view known) { return equalsIgnoreCase(suffix, known); });

    return isAudio ? trimmed.substr(0, dot) : trimmed;
}
}  // namespace detail

/**
 * Join a track name and a take/clip name into one source label.
 *
 * When the two name the same thing - identical, or identical once an audio
 * extension is dropped - the result is that name once, without the
 * extension, because the label is also used to build filenames. Otherwise
 * both survive, since they are then telling the user two different things.
 * Where the two differ only in case, the track name's spelling is the one
 * kept, being the spelling that matched the file on disk.
 * Either side may be empty; the other is returned on its own.
 */
inline std::string joinSourceLabel(std::string_view trackName, std::string_view takeName)
{
    const auto track = detail::trim(trackName);
    const auto take = detail::trim(takeName);

    if (track.empty())
        return detail::withoutAudioExtension(take);

    if (take.empty())
        return detail::withoutAudioExtension(track);

    const auto trackBase = detail::withoutAudioExtension(track);
    const auto takeBase = detail::withoutAudioExtension(take);

    if (detail::equalsIgnoreCase(trackBase, takeBase))
        return trackBase;

    return track + " / " + take;
}
}  // namespace stemlab::source
