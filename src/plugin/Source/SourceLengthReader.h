#pragma once

#include "SourceLength.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace stemlab::source
{
// Shared by preview/source metadata and waveform analysis so they use the
// selected decoder's format, even when the container extension is misleading.
inline bool clampReaderToFileContents(juce::AudioFormatReader& reader, const juce::File& file)
{
    if (!storesFixedSizeFrames(reader.getFormatName().toStdString()))
        return false;

    const auto ceiling = frameCeilingForBytes(file.getSize(),
                                              static_cast<int>(reader.numChannels),
                                              static_cast<int>(reader.bitsPerSample));
    const auto present = static_cast<juce::int64>(
        framesActuallyPresent(reader.lengthInSamples, ceiling));
    const bool wasCutShort = present < reader.lengthInSamples;
    reader.lengthInSamples = present;
    return wasCutShort;
}
} // namespace stemlab::source
