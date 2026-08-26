#pragma once

#include <JuceHeader.h>

/** Tracks files in FI-STEM's currently active external stem drag. */
class StemLabSelfFileDragGuard final
{
public:
    void begin(const juce::StringArray& files)
    {
        activePaths.clear();

        for (const auto& path : files)
            activePaths.addIfNotAlreadyThere(normalise(path), pathsIgnoreCase);
    }

    void clear() { activePaths.clear(); }

    bool shouldIgnore(const juce::String& path) const
    {
        return activePaths.contains(normalise(path), pathsIgnoreCase);
    }

private:
   #if JUCE_WINDOWS
    static constexpr bool pathsIgnoreCase = true;
   #else
    static constexpr bool pathsIgnoreCase = false;
   #endif

    static juce::String normalise(const juce::String& path)
    {
        return juce::File(path).getFullPathName();
    }

    juce::StringArray activePaths;
};
