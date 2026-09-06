#include "SourceLengthReader.h"

#include <cassert>
#include <cmath>

using stemlab::source::frameCeilingForBytes;
using stemlab::source::framesActuallyPresent;
using stemlab::source::storesFixedSizeFrames;

namespace
{
constexpr int sampleRate = 44100;
constexpr int sampleCount = 10 * sampleRate;

juce::MemoryBlock encode(juce::AudioFormat& format)
{
    juce::MemoryBlock bytes;
    {
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::MemoryOutputStream>(bytes, false);
        const auto options = juce::AudioFormatWriter::Options{}
                                 .withSampleRate(sampleRate)
                                 .withNumChannels(2)
                                 .withBitsPerSample(16);
        auto writer = format.createWriterFor(stream, options);
        assert(writer != nullptr);
        juce::AudioBuffer<float> audio(2, sampleCount);
        for (int i = 0; i < sampleCount; ++i)
            for (int channel = 0; channel < 2; ++channel)
                audio.setSample(channel, i, 0.25f * std::sin(
                    static_cast<float>(i) * 440.0f * juce::MathConstants<float>::twoPi / sampleRate));
        assert(writer->writeFromAudioSampleBuffer(audio, 0, sampleCount));
    }
    return bytes;
}

void checkDecodedSources()
{
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("stemlab-source-length-" + juce::Uuid().toString());
    assert(directory.createDirectory().wasOk());
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto write = [&](const char* name, const juce::MemoryBlock& bytes)
    {
        const auto file = directory.getChildFile(name);
        assert(file.replaceWithData(bytes.getData(), bytes.getSize()));
        return file;
    };
    const auto read = [&](const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        assert(reader != nullptr);
        assert(reader->lengthInSamples == sampleCount);
        return reader;
    };

    juce::WavAudioFormat wav;
    const auto pcm = encode(wav);
    const auto intactFile = write("intact.wav", pcm);
    auto intact = read(intactFile);
    assert(!stemlab::source::clampReaderToFileContents(*intact, intactFile));
    assert(intact->lengthInSamples == sampleCount);
    intact.reset();

    const auto truncatedFile = write("truncated.wav", juce::MemoryBlock(pcm.getData(), 400000));
    auto truncated = read(truncatedFile);
    assert(stemlab::source::clampReaderToFileContents(*truncated, truncatedFile));
    assert(truncated->lengthInSamples == 100000);
    truncated.reset();

    juce::AiffAudioFormat aiff;
    const auto aiffFile = write("intact.aiff", encode(aiff));
    auto aiffReader = read(aiffFile);
    assert(!stemlab::source::clampReaderToFileContents(*aiffReader, aiffFile));
    aiffReader.reset();

    juce::OggVorbisAudioFormat vorbis;
    const auto ogg = encode(vorbis);
    const auto oggFile = write("source.ogg", ogg);
    auto oggReader = read(oggFile);
    assert(!stemlab::source::clampReaderToFileContents(*oggReader, oggFile));
    oggReader.reset();

    // A supported Vorbis-in-WAV subformat: JUCE's WAV reader delegates to
    // OggVorbisAudioFormat. The compressed bytes do not bound decoded frames.
    juce::MemoryOutputStream wrapped;
    wrapped.write("RIFF", 4);
    wrapped.writeInt(static_cast<int>(36 + ogg.getSize() + (ogg.getSize() & 1)));
    wrapped.write("WAVEfmt ", 8);
    wrapped.writeInt(16);
    wrapped.writeShort(0x674f);
    wrapped.writeShort(2);
    wrapped.writeInt(sampleRate);
    wrapped.writeInt(16000);
    wrapped.writeShort(1);
    wrapped.writeShort(16);
    wrapped.write("data", 4);
    wrapped.writeInt(static_cast<int>(ogg.getSize()));
    wrapped.write(ogg.getData(), ogg.getSize());
    if ((ogg.getSize() & 1) != 0)
        wrapped.writeByte(0);
    const auto compressedFile = write("compressed.wav", wrapped.getMemoryBlock());
    auto compressed = read(compressedFile);
    assert(compressed->getFormatName() == vorbis.getFormatName());
    assert(frameCeilingForBytes(compressedFile.getSize(), 2, 16) < sampleCount / 10);
    assert(!stemlab::source::clampReaderToFileContents(*compressed, compressedFile));
    assert(compressed->lengthInSamples == sampleCount);

    // Read late audio through the clamped reader, rather than checking only
    // a duration field: the original bug removed almost the whole preview.
    juce::AudioBuffer<float> tail(2, 512);
    assert(compressed->read(&tail, 0, 512, 8 * sampleRate, true, true));
    assert(tail.getMagnitude(0, 512) > 0.1f);
    compressed.reset();
    assert(directory.deleteRecursively());
}
} // namespace

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

    // The gate names the actual decoder, never a filename extension.
    static_assert(storesFixedSizeFrames("WAV file"));
    static_assert(storesFixedSizeFrames("AIFF file"));
    static_assert(!storesFixedSizeFrames("Ogg-Vorbis file"));
    static_assert(!storesFixedSizeFrames("FLAC file"));
    static_assert(!storesFixedSizeFrames(".wav"));
    static_assert(!storesFixedSizeFrames(""));

    checkDecodedSources();
    return 0;
}
