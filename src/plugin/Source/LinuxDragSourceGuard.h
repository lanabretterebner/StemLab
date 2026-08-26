#pragma once

/*
 * While one of our own external file drags is in flight, this window must
 * not look like a drop target. JUCE's X11 drag source and drop target share
 * one state machine per window, and a drag that starts over its own window
 * XdndEnters itself: the target half then unconditionally accepts, and its
 * stale XdndStatus replies race the real target's, wedging the position
 * throttle so the first drop can go nowhere.
 *
 * Removing the XdndAware property for the drag's duration takes this window
 * out of the target search entirely, so the protocol conversation only ever
 * happens with the window the user is actually dropping on. Inbound drags
 * cannot arrive meanwhile - the drag source holds the pointer grab.
 *
 * Lives in its own translation unit because Xlib's macros (None, Window,
 * Font...) cannot share a file with JUCE headers.
 */
namespace stemlab::linuxdnd
{

/** Hides the window from drop-target searches; returns a token, or null. */
void* suppressDropTarget(void* nativeWindowHandle);

/** Restores the window's drop-target advert and frees the token. */
void restoreDropTarget(void* token);

} // namespace stemlab::linuxdnd
