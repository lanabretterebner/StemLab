#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemlab::widgets
{
// Desktop focus notifications reach every plugin instance. Only reclaim
// focus from our editor or the standalone window itself, never another
// editor, a host control, or a separate non-modal window.
inline bool ownsSettingsFocusTarget(const juce::Component& editor,
                                   const juce::Component* focused,
                                   const juce::Component* standaloneWindow)
{
    return focused != nullptr
           && (focused == &editor || editor.isParentOf(focused)
               || (standaloneWindow != nullptr && focused == standaloneWindow));
}
}
