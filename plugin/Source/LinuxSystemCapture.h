#pragma once

#include "PluginProcessor.h"

#if JUCE_LINUX

/*
    Linux system-audio capture: records whatever the desktop is playing by
    reading the default output's monitor source through PulseAudio's simple
    API. PipeWire ships a complete PulseAudio server implementation, so the
    same client path covers both audio stacks - which together are what
    desktop Linux actually runs.

    libpulse-simple is dlopen'd at runtime rather than linked, so the plugin
    still loads on a machine without the PulseAudio client libraries; the
    Record System button then fails with an instruction instead of taking the
    whole plugin down with an unresolved library.

    Blocking libpulse calls run on a detached, self-owned reader thread (see
    the .cpp); this juce::Thread only drains its queue with bounded waits,
    so stopping capture can never block the UI or hit thread cancellation,
    even against a wedged audio server.

    The class carries the same name and owner contract as the Windows WASAPI
    loopback thread in PluginProcessor.cpp - one implementation exists per
    platform.
*/
class StemLabSystemLoopbackThread final : public juce::Thread
{
public:
    StemLabSystemLoopbackThread (
        StemLabAudioProcessor& ownerIn,
        juce::File outputFileIn);

    ~StemLabSystemLoopbackThread() override;

    bool wasSuccessful() const noexcept
    {
        return successful.load();
    }

    void run() override;

private:
    bool openWriter (
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>& writer);

    void fail (const juce::String& message);

    StemLabAudioProcessor& owner;
    juce::File outputFile;
    std::atomic<bool> successful { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (
        StemLabSystemLoopbackThread)
};

#endif
