#include "SourceLabel.h"

#include <cassert>
#include <string>

using stemlab::source::joinSourceLabel;

int main()
{
    // The bug: a DAW that imported "Song.wav" names the track after the file
    // and the take after the file without its extension, so the label read
    // "Song.wav / Song" - in the window and on every exported stem.
    assert(joinSourceLabel("Song.wav", "Song") == "Song");
    assert(joinSourceLabel("Song", "Song.wav") == "Song");
    assert(joinSourceLabel("Song.wav", "Song.wav") == "Song");
    assert(joinSourceLabel("Song.flac", "Song") == "Song");
    // When the two differ only in case, the track's spelling wins: it is the
    // one that matched the file on disk, and this seeds filenames.
    assert(joinSourceLabel("Song.WAV", "song") == "Song");

    // Two names that genuinely say different things both survive: that is
    // what the join is for.
    assert(joinSourceLabel("Drums", "Kick 03") == "Drums / Kick 03");
    assert(joinSourceLabel("Bass.wav", "Verse.wav") == "Bass.wav / Verse.wav");

    // One side missing leaves the other standing alone, still tidied.
    assert(joinSourceLabel("", "Song.wav") == "Song");
    assert(joinSourceLabel("Song.wav", "") == "Song");
    assert(joinSourceLabel("", "") == "");
    assert(joinSourceLabel("   ", "Song") == "Song");

    // Surrounding whitespace must not stop two identical names matching.
    assert(joinSourceLabel("  Song.wav  ", "Song") == "Song");

    // A dot is not always an extension. Stripping every suffix would eat the
    // version number off a track the user deliberately named this way.
    assert(joinSourceLabel("Mix v1.2", "Take") == "Mix v1.2 / Take");
    assert(joinSourceLabel("Mix v1.2", "Mix v1") == "Mix v1.2 / Mix v1");
    assert(joinSourceLabel("Song.stems", "Song") == "Song.stems / Song");

    // A name that is nothing but a suffix keeps it, rather than vanishing.
    assert(joinSourceLabel(".wav", "Song") == ".wav / Song");
    assert(joinSourceLabel("Song.", "Song") == "Song. / Song");

    return 0;
}
