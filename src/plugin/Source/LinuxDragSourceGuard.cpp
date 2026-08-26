#include "LinuxDragSourceGuard.h"

#if defined(__linux__)

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdint>

namespace stemlab::linuxdnd
{

namespace
{
    struct Token
    {
        Display* display = nullptr;
        ::Window window = 0;
    };
} // namespace

void* suppressDropTarget(void* nativeWindowHandle)
{
    if (nativeWindowHandle == nullptr)
        return nullptr;

    // A private connection: property changes are server-side state, so they
    // are visible to JUCE's own connection immediately after the flush.
    auto* display = XOpenDisplay(nullptr);

    if (display == nullptr)
        return nullptr;

    const auto window = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(nativeWindowHandle));
    const auto aware = XInternAtom(display, "XdndAware", False);

    XDeleteProperty(display, window, aware);
    XFlush(display);

    return new Token{display, window};
}

void restoreDropTarget(void* token)
{
    auto* state = static_cast<Token*>(token);

    if (state == nullptr)
        return;

    const auto aware = XInternAtom(state->display, "XdndAware", False);

    // The value JUCE advertises: XWindowSystemUtilities::Atoms::DndVersion.
    unsigned long version = 3;
    XChangeProperty(state->display, state->window, aware, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&version), 1);
    XFlush(state->display);
    XCloseDisplay(state->display);

    delete state;
}

} // namespace stemlab::linuxdnd

#else

namespace stemlab::linuxdnd
{
void* suppressDropTarget(void*) { return nullptr; }
void restoreDropTarget(void*) {}
} // namespace stemlab::linuxdnd

#endif
