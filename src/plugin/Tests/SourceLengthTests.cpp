#include "SourceLength.h"

#include <cassert>

using stemlab::source::frameCeilingForBytes;
using stemlab::source::framesActuallyPresent;
using stemlab::source::storesFixedSizeFrames;

int main()
{
    // The bug: head -c 400000 of a 30-second stereo 16-bit WAV still declares
    // 1 323 000 frames in its data chunk, and the app printed 00:30 for the
    // 2.27 seconds that survived - in the strip, the transport and the grid.
    constexpr long long declared = 5292000 / 4;
    constexpr long long ceiling = frameCeilingForBytes(400000, 2, 16);

    static_assert(ceiling == 100000);
    static_assert(framesActuallyPresent(declared, ceiling) == 100000);

    // The ceiling counts the headers as if they were audio, so it is a little
    // longer than the audio really there (399 956 bytes, 99 989 frames). That
    // direction is deliberate: over-reporting by 11 frames is a rounding
    // error, under-reporting would cut audio off a file nothing is wrong with.
    static_assert(ceiling > (400000 - 44) / 4);
    static_assert(ceiling - (400000 - 44) / 4 < 20);

    // A whole file is never called short. The same 30-second WAV at full
    // length has more bytes than its audio needs, headers included.
    static_assert(framesActuallyPresent(declared, frameCeilingForBytes(5292044, 2, 16)) == declared);

    // Nothing to go on: the declared count stands. An unknown answer must not
    // shorten anything.
    static_assert(frameCeilingForBytes(0, 2, 16) == 0);
    static_assert(frameCeilingForBytes(400000, 0, 16) == 0);
    static_assert(frameCeilingForBytes(400000, 2, 0) == 0);
    static_assert(framesActuallyPresent(declared, 0) == declared);
    static_assert(framesActuallyPresent(declared, -1) == declared);

    // A sample depth that is not a whole number of bytes is not frame
    // arithmetic this can do, so it declines rather than rounds.
    static_assert(frameCeilingForBytes(400000, 2, 12) == 0);
    static_assert(frameCeilingForBytes(400000, 2, 20) == 0);

    // Depths and channel counts that are: 8-bit mono, 24-bit stereo,
    // 32-bit float 5.1.
    static_assert(frameCeilingForBytes(400000, 1, 8) == 400000);
    static_assert(frameCeilingForBytes(400000, 2, 24) == 66666);
    static_assert(frameCeilingForBytes(400000, 6, 32) == 16666);

    // Only the formats whose frames really are a fixed size on disk. FLAC and
    // the rest decode to more audio than they occupy, so the same arithmetic
    // would call every one of them truncated - the reason this gate exists.
    assert(storesFixedSizeFrames(".wav"));
    assert(storesFixedSizeFrames(".bwf"));
    assert(storesFixedSizeFrames(".aiff"));
    assert(storesFixedSizeFrames(".aif"));
    assert(storesFixedSizeFrames(".WAV"));
    assert(storesFixedSizeFrames(".Aiff"));

    assert(!storesFixedSizeFrames(".flac"));
    assert(!storesFixedSizeFrames(".mp3"));
    assert(!storesFixedSizeFrames(".ogg"));
    assert(!storesFixedSizeFrames(".m4a"));
    assert(!storesFixedSizeFrames(".opus"));
    assert(!storesFixedSizeFrames(".aac"));
    assert(!storesFixedSizeFrames(".wv"));
    assert(!storesFixedSizeFrames("wav"));
    assert(!storesFixedSizeFrames(""));

    return 0;
}
