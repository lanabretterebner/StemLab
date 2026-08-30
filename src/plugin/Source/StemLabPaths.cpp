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

   #if JUCE_WINDOWS
    juce::File documentsStemLab()
    {
        return juce::File::getSpecialLocation (
                   juce::File::userDocumentsDirectory)
            .getChildFile ("StemLab");
    }
   #endif
}

juce::File userDataDirectory()
{
   #if JUCE_WINDOWS
    return documentsStemLab();
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
    // Documents\StemLab, unchanged: the 0.9.9 layout is a compatibility
    // promise, and moving it would strand every existing install's captures.
    return documentsStemLab();
   #elif JUCE_MAC
    return userDataDirectory();
   #else
    // ~/Music/StemLab, through JUCE's XDG user-dirs resolver so a localised
    // or relocated music folder is honoured. Audio the user made is theirs,
    // and belongs with the rest of their audio rather than inside the
    // directory the application unpacks itself into - which on Linux is the
    // same ~/.local/share/StemLab the bundle installs to.
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
        .getChildFile ("StemLab");
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
    const auto localAppData =
        juce::SystemStats::getEnvironmentVariable (
            "LOCALAPPDATA",
            {});

    if (localAppData.isNotEmpty()
        && juce::File::isAbsolutePath (localAppData))
    {
        return juce::File (localAppData)
            .getChildFile ("StemLab");
    }

    return documentsStemLab();
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

juce::File legacyBridgeDirectory()
{
    return userDataDirectory()
        .getChildFile ("Ableton");
}
}
