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
#include "SettingsFocusPolicy.h"

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

    // Even editors sharing a host window must not reclaim each other's
    // keyboard. The standalone window itself remains a valid return target
    // after a dialog closes, but its other controls and windows do not.
    {
        juce::Component window, firstEditor, secondEditor, firstControl,
            secondControl, hostControl, otherWindow;
        window.addChildComponent(firstEditor);
        window.addChildComponent(secondEditor);
        window.addChildComponent(hostControl);
        firstEditor.addChildComponent(firstControl);
        secondEditor.addChildComponent(secondControl);

        const auto owns = [&](const juce::Component* focused,
                              const juce::Component* standaloneWindow = nullptr)
        {
            return widgets::ownsSettingsFocusTarget(firstEditor, focused, standaloneWindow);
        };

        check(owns(&firstEditor));
        check(owns(&firstControl));
        check(!owns(&secondEditor));
        check(!owns(&secondControl));
        check(!owns(&hostControl));
        check(!owns(&window));
        check(!owns(nullptr));
        check(owns(&window, &window));
        check(!owns(&hostControl, &window));
        check(!owns(&otherWindow, &window));
    }

    for (int round = 0; round < 2; ++round)
    {
        StemLabAudioProcessor processor;

        const auto savedWidth = round == 0 ? 2200 : 700;
        const auto savedHeight = round == 0 ? 500 : 1200;
        processor.setEditorWindowSize(savedWidth, savedHeight);

        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());

        check(editor != nullptr);
        check(editor->getWidth() == savedWidth);
        check(editor->getHeight() == savedHeight);
        check(processor.getEditorWindowWidth() == savedWidth);
        check(processor.getEditorWindowHeight() == savedHeight);

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

    // Legacy preferences still size a hosted editor from the saved scale.
    {
        StemLabAudioProcessor processor;
        processor.setEditorWindowSize(0, 0);
        processor.setEditorScalePercent(175);
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        check(editor->getWidth() == juce::roundToInt(theme::metrics::window::width * 1.75));
        check(editor->getHeight() == juce::roundToInt(theme::metrics::window::height * 1.75));
    }

    configSandbox.deleteRecursively();

    return 0;
}
