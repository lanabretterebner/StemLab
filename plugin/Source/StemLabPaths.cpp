#include "StemLabPaths.h"

namespace stemlab::paths
{
namespace
{
   #if ! JUCE_WINDOWS
    juce::File homeDirectory()
    {
        return juce::File::getSpecialLocation (
            juce::File::userHomeDirectory);
    }

    /*  An XDG variable is only honoured when it holds an absolute path.
        The spec says relative values must be ignored, and juce::File would
        assert on one anyway.
    */
    juce::File xdgDirectory (
        const char* variable,
        const juce::String& fallbackRelativeToHome)
    {
        const auto value =
            juce::SystemStats::getEnvironmentVariable (
                variable,
                {}).trim();

        if (value.isNotEmpty()
            && juce::File::isAbsolutePath (value))
        {
            return juce::File (value);
        }

        return homeDirectory()
            .getChildFile (fallbackRelativeToHome);
    }
   #endif

   #if ! JUCE_WINDOWS
    /*  The user's music folder, resolved without ever inventing an English
        one.

        juce::File::userMusicDirectory cannot be used for this. It reads
        ~/.config/user-dirs.dirs and returns what it finds there only when
        that directory already exists - otherwise it falls back to a literal
        "~/Music". On a German desktop whose XDG_MUSIC_DIR is $HOME/Musik but
        which has not created it yet, and on any minimal install without
        xdg-user-dirs at all, that fallback makes StemLab create an English
        ~/Music next to the user's real music folder. It also never consults
        the XDG_MUSIC_DIR environment variable.

        So: the environment variable first, then the config file - honoured
        whether or not the directory exists yet, because a configured
        location is an answer and we can create it - and if neither says
        anything, no guess at all. The data directory is language-neutral
        and correct by the spec; a wrong folder in the right language is
        worse than a right folder in a dull place.
    */
    juce::File userMusicRoot()
    {
        const auto fromEnvironment =
            juce::SystemStats::getEnvironmentVariable ("XDG_MUSIC_DIR", {}).trim();

        if (fromEnvironment.isNotEmpty()
            && juce::File::isAbsolutePath (fromEnvironment))
        {
            return juce::File (fromEnvironment);
        }

        juce::StringArray lines;
        homeDirectory().getChildFile (".config/user-dirs.dirs").readLines (lines);

        for (const auto& raw : lines)
        {
            const auto line = raw.trimStart();

            if (! line.startsWith ("XDG_MUSIC_DIR"))
                continue;

            const auto value = line.fromFirstOccurrenceOf ("=", false, false)
                                   .trim()
                                   .unquoted()
                                   .replace ("$HOME", homeDirectory().getFullPathName());

            if (juce::File::isAbsolutePath (value))
                return juce::File (value);
        }

        return {};
    }
   #endif

   #if JUCE_WINDOWS
    /*  %LOCALAPPDATA%, or the user's home if the variable is missing or
        relative - which it is not on any supported Windows, but a path
        built from an empty string would silently become a relative one.
    */
    juce::File windowsLocalAppData()
    {
        const auto value =
            juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}).trim();

        if (value.isNotEmpty() && juce::File::isAbsolutePath (value))
            return juce::File (value);

        return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    }
   #endif
}

juce::File userDataDirectory()
{
   #if JUCE_WINDOWS
    return windowsLocalAppData().getChildFile ("StemLab");
   #elif JUCE_MAC
    return homeDirectory()
        .getChildFile ("Library")
        .getChildFile ("Application Support")
        .getChildFile ("StemLab");
   #else
    return xdgDirectory ("XDG_DATA_HOME", ".local/share")
        .getChildFile ("StemLab");
   #endif
}

juce::File userMediaDirectory()
{
   #if JUCE_WINDOWS
    // The shell resolves this to the real localised path, so unlike the
    // Linux side there is nothing to second-guess here.
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
        .getChildFile ("StemLab");
   #elif JUCE_MAC
    return userDataDirectory();
   #else
    /*  <music>/StemLab where the desktop says where that is, and
        $XDG_DATA_HOME/StemLab where it does not. Audio the user made is
        theirs and belongs with the rest of their audio - but only if we can
        find out where that actually is, in their language. See
        userMusicRoot.
    */
    const auto music = userMusicRoot();

    if (music != juce::File{})
        return music.getChildFile ("StemLab");

    return userDataDirectory();
   #endif
}

juce::File jobsDirectory()
{
   #if JUCE_MAC
    return homeDirectory()
        .getChildFile ("Library")
        .getChildFile ("Caches")
        .getChildFile ("StemLab")
        .getChildFile ("jobs");
   #else
    return userMediaDirectory()
        .getChildFile ("Jobs");
   #endif
}

juce::File configDirectory()
{
   #if JUCE_WINDOWS
    return windowsLocalAppData().getChildFile ("StemLab");
   #elif JUCE_MAC
    return userDataDirectory();
   #else
    return xdgDirectory ("XDG_CONFIG_HOME", ".config")
        .getChildFile ("StemLab");
   #endif
}

juce::File capturesDirectory()
{
    return userMediaDirectory()
        .getChildFile ("Captures");
}

juce::File recordingsDirectory()
{
    return userMediaDirectory()
        .getChildFile ("Recordings");
}

juce::File bridgeTempDirectory()
{
    return juce::File::getSpecialLocation (
               juce::File::tempDirectory)
        .getChildFile ("StemLab")
        .getChildFile ("Ableton");
}

juce::File remoteStatusDirectory()
{
   #if JUCE_WINDOWS
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("StemLab")
        .getChildFile ("Ableton");
   #else
    // Ableton integration is Windows-only; this exists so the path resolves
    // to something harmless rather than being conditionally compiled away.
    return userDataDirectory().getChildFile ("Ableton");
   #endif
}

juce::File engineExecutable()
{
    const auto override_ =
        juce::SystemStats::getEnvironmentVariable ("STEMLAB_ENGINE", {}).trim();

    if (override_.isNotEmpty() && juce::File::isAbsolutePath (override_))
        return juce::File (override_);

   #if JUCE_WINDOWS
    return userDataDirectory()
        .getChildFile ("Engine")
        .getChildFile ("python.exe");
   #else
    // Exactly where scripts/linux/install_backend.sh puts it.
    return userDataDirectory()
        .getChildFile ("Engine")
        .getChildFile ("bin")
        .getChildFile ("python3");
   #endif
}
}
