/*
 * Opening and closing the editor must leave nothing behind that JUCE has to
 * tear down after main.
 *
 * The bundled Inter faces are published through function-local statics in
 * StemLabTheme, and a Typeface made from memory keeps an entry in a JUCE
 * singleton that is created after those statics and therefore destroyed
 * before them. Holding the last reference in a static means releasing it
 * during static destruction, into a cache that no longer exists - which
 * segfaulted the standalone on every clean exit, after the window was closed
 * and the user thought they were done.
 *
 * So this test's real assertion is its own exit status: it opens an editor,
 * closes it, opens another, closes that, and returns. A build that keeps the
 * faces past main does not reach the end of the process alive.
 */

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "StemLabTheme.h"

using namespace stemlab;

#include <cstdlib>

namespace
{
void check(bool condition)
{
    if (!condition)
        std::abort();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    // The editor writes preferences; keep them out of whoever ran the tests.
    const auto configSandbox =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("stemlab-editor-life-"
                          + juce::String(juce::Time::getHighResolutionTicks()));

    configSandbox.createDirectory();

    for (const auto* variable : {"XDG_CONFIG_HOME", "LOCALAPPDATA", "HOME"})
       #if JUCE_WINDOWS
        _putenv_s(variable, configSandbox.getFullPathName().toRawUTF8());
       #else
        setenv(variable, configSandbox.getFullPathName().toRawUTF8(), 1);
       #endif

    // No faces before an editor has ever existed.
    check(theme::fonts::regularTypeface() == nullptr);

    for (int round = 0; round < 2; ++round)
    {
        StemLabAudioProcessor processor;

        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());

        check(editor != nullptr);

        // The look and feel registers the bundled faces on construction, and
        // every font token resolves through them.
        check(theme::fonts::regularTypeface() != nullptr);
        check(theme::fonts::mediumTypeface() != nullptr);

        // A real size, so resized() runs the whole layout at least once.
        editor->setSize(880, 564);

        check(editor->getWidth() == 880);

        editor.reset();

        /*  And the last editor closing takes them back down again: the
            SharedResourcePointer destroys the look and feel here, inside the
            application's lifetime, which is the whole point.
        */
        check(theme::fonts::regularTypeface() == nullptr);
        check(theme::fonts::mediumTypeface() == nullptr);
    }

    configSandbox.deleteRecursively();

    return 0;
}
