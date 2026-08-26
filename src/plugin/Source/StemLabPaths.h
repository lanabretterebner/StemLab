#pragma once

#include <JuceHeader.h>

/*
    Every user-writable location StemLab uses, in one place.

    Windows keeps the 0.9.9 layout byte-for-byte: everything under
    Documents\StemLab, with the engine pointer in %LOCALAPPDATA%\StemLab.
    Existing installs must keep finding their old captures and jobs, so the
    Windows branches here are a straight transcription of what the processor
    used to build inline.

    Linux follows the XDG Base Directory spec instead of inventing a
    Documents-shaped layout that does not exist there:

        data    $XDG_DATA_HOME/StemLab      (~/.local/share/StemLab)
        cache   $XDG_CACHE_HOME/StemLab     (~/.cache/StemLab)
        config  $XDG_CONFIG_HOME/StemLab    (~/.config/StemLab)

    Separation jobs are large and fully regenerable from the source audio, so
    on Linux they live in the cache directory rather than beside the user's
    documents.
*/
namespace stemlab::paths
{
    /** Captures, recordings, and anything else the user is expected to keep. */
    juce::File userDataDirectory();

    /** Default parent for job_* directories. Overridable from the UI. */
    juce::File jobsDirectory();

    /** Small machine-local settings, e.g. portable_engine_path.txt. */
    juce::File configDirectory();

    juce::File capturesDirectory();
    juce::File recordingsDirectory();

    /** One-shot host-bridge reply files. Temp, because they are throwaway. */
    juce::File bridgeTempDirectory();

    /** Fixed reply location understood by StemLabRemote 0.9.3 and earlier. */
    juce::File legacyBridgeDirectory();
}
