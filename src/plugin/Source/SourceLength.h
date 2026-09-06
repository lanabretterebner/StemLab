#pragma once

/*
 * How long the audio in a source file really is, when its own header
 * disagrees with the bytes on disk.
 *
 * An uncompressed container states the size of its audio up front - the WAV
 * `data` chunk, the AIFF `SSND` chunk - and JUCE takes the sample count
 * straight from that number (juce_WavAudioFormat.cpp: `lengthInSamples =
 * dataLength / bytesPerFrame`). A download that stopped early, a render that
 * was interrupted, a file copied off a full disk: all of them leave the
 * declared size standing over audio that is no longer there, and the reader
 * repeats it. A six-minute file cut short after five seconds still reports
 * six minutes, to the source strip, the transport clock, the grid and every
 * duration the engine is handed.
 *
 * The file itself settles it. Frames of a fixed size cannot outnumber the
 * bytes holding them, headers included - so the size on disk is a ceiling
 * that no well-formed file can exceed and a truncated one always does.
 *
 * That arithmetic only holds where a frame really is a fixed number of bytes.
 * FLAC, MP3, Ogg and the rest decode to far more audio than they occupy, so
 * the same sum would call every one of them truncated; storesFixedSizeFrames
 * is what keeps them out of it.
 */

#include <string>
#include <string_view>

namespace stemlab::source
{

/**
 * Whether a file with this extension stores audio as fixed-size frames, so
 * that its size on disk bounds how much audio it can hold.
 *
 * Deliberately the extensions JUCE's own uncompressed readers claim - WAV
 * takes ".wav .bwf", AIFF takes ".aiff .aif" - and nothing else. A format
 * this does not name is left alone rather than guessed at, which costs only
 * the check: the alternative, guessing wrong about a compressed format,
 * would shorten a file that was never damaged.
 */
inline bool storesFixedSizeFrames(std::string_view extension)
{
    std::string lower;
    lower.reserve(extension.size());

    for (const char c : extension)
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));

    return lower == ".wav" || lower == ".bwf" || lower == ".aiff" || lower == ".aif";
}

/**
 * The most frames a file this many bytes long could hold, if every byte of it
 * were audio. Generous on purpose: it counts the headers as audio too, so it
 * never accuses a sound file of being short.
 *
 * Returns 0 for "cannot say" - no size, no channels, or a sample depth that
 * is not a whole number of bytes - and callers must conclude nothing from it.
 */
constexpr long long frameCeilingForBytes(long long fileBytes, int numChannels, int bitsPerSample)
{
    if (fileBytes <= 0 || numChannels <= 0 || bitsPerSample <= 0 || bitsPerSample % 8 != 0)
        return 0;

    return fileBytes / (static_cast<long long>(numChannels) * (bitsPerSample / 8));
}

/**
 * The frame count to believe: the header's, unless the file is too small to
 * contain it.
 *
 * A ceiling of 0 means the question could not be asked, and the declared
 * count stands - an unknown answer must not shorten anything.
 */
constexpr long long framesActuallyPresent(long long declaredFrames, long long frameCeiling)
{
    if (frameCeiling <= 0 || declaredFrames <= frameCeiling)
        return declaredFrames;

    return frameCeiling;
}

}
