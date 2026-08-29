/*
 * The lane listens deeply so its hover highlight covers its children
 * (StemLaneComponent's constructor), which makes JUCE replay every
 * descendant's mouse event on the lane as well. For the wheel that mattered:
 * the waveform well zooms and consumes the event, and the replay then
 * forwarded the same wheel to the viewport, so one gesture zoomed and
 * scrolled at once.
 *
 * The lane tells a replay from a real forward by asking the event which
 * component it landed on. That is a property of JUCE's dispatch rather than
 * of StemLab, so it is pinned here: a JUCE upgrade that changed it would
 * otherwise bring the double-handling back silently.
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <cassert>

namespace
{
/** The lane's rule, on its own so the test exercises the real decision. */
bool wheelShouldBeForwarded(const juce::MouseEvent& event, const juce::Component* lane)
{
    return event.eventComponent == lane;
}
}  // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::Component lane, well;
    lane.setSize(100, 40);
    well.setSize(96, 36);
    lane.addAndMakeVisible(well);

    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();

    // What MouseListenerList::sendMouseEvent hands a deep listener: an event
    // still naming the component the wheel landed on, which here is the well.
    const juce::MouseEvent replayed{source,      {10.0f, 10.0f}, juce::ModifierKeys(),
                                    1.0f,        0.0f,           0.0f,
                                    0.0f,        0.0f,           &well,
                                    &well,       now,            {10.0f, 10.0f},
                                    now,         1,              false};

    assert(replayed.eventComponent == &well);
    assert(!wheelShouldBeForwarded(replayed, &lane));

    // What Component::mouseWheelMove hands the parent when a child does not
    // consume the wheel: getEventRelativeTo re-points eventComponent at the
    // parent, so this one has to keep travelling or the list stops scrolling.
    const auto forwarded = replayed.getEventRelativeTo(&lane);

    assert(forwarded.eventComponent == &lane);
    assert(forwarded.originalComponent == &well);
    assert(wheelShouldBeForwarded(forwarded, &lane));

    // A wheel that lands on the lane itself is forwarded too.
    const juce::MouseEvent direct{source,      {10.0f, 10.0f}, juce::ModifierKeys(),
                                  1.0f,        0.0f,           0.0f,
                                  0.0f,        0.0f,           &lane,
                                  &lane,       now,            {10.0f, 10.0f},
                                  now,         1,              false};

    assert(wheelShouldBeForwarded(direct, &lane));

    return 0;
}
