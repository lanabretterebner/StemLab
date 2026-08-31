#pragma once

#include <JuceHeader.h>

/*
    Every user-writable location StemLab uses, in one place.

    Windows follows the platform's own conventions rather than a single
    Documents\StemLab tree: application data in %LOCALAPPDATA%\StemLab, and
    the user's own audio in their Music folder, which the shell resolves to
    the real localised path.

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

    /** Where StemLabRemote writes its heartbeat and its clip replies.

        Pinned to Documents\StemLab\Ableton on Windows because that is
        where the Remote script itself builds the path (see
        integrations/ableton/StemLabRemote). It is installed into Ableton
        and updated separately from the plugin, so this is a protocol
        location shared with another program, not a layout choice - moving
        it here would break every install whose Remote had not been
        updated in the same minute. */
    juce::File remoteStatusDirectory();

    /** The Engine's interpreter. One fixed location per platform.

        There is no discovery. StemLab installs its Engine here and looks
        for it here, and if it is not here it is not installed - which is a
        thing the app can say plainly, rather than searching ten directories
        up from wherever a host happened to load a VST3 from and reporting
        whatever it found. STEMLAB_ENGINE still overrides it, for running
        against a development checkout. */
    juce::File engineExecutable();
}
