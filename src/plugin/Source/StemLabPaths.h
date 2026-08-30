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
        media   ~/Music/StemLab             (XDG_MUSIC_DIR when set)

    The split that matters is the app against the user's own audio. On Linux
    the bundle installs into $XDG_DATA_HOME/StemLab, so captures, recordings
    and finished stems written there would sit inside the application's own
    directory - indistinguishable from it to anyone tidying up, and deleted
    by anything that removes the app wholesale. They go to ~/Music/StemLab
    instead, where the rest of a person's audio already is.

    Jobs moved there with them. They are regenerable, which is why they used
    to live in the cache, but they are also the output someone asked for, and
    a cache directory is not where anyone looks for stems they just made.
*/
namespace stemlab::paths
{
    /** The application's own directory: the Engine, the bridge, nothing of
        the user's. On Linux this is where the bundle unpacks itself. */
    juce::File userDataDirectory();

    /** Audio the user made or asked for: captures, recordings, stems. */
    juce::File userMediaDirectory();

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
