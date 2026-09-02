/*
 * The lane listens deeply so its hover highlight covers its children
 * (StemLaneComponent's constructor), which makes JUCE replay every
 * descendant's mouse event on the lane as well. For the wheel that mattered:
 * the lane used to register ITSELF as that deep listener, and juce::Component
 * IS a juce::MouseListener whose mouseWheelMove forwards to the nearest
 * enabled ancestor - so the listener leg of the dispatch forwarded a second
 * time and one notch scrolled the lane list two rows.
 *
 * The fix is a separate DescendantMouseRelay: a plain MouseListener carrying
 * the four events the row wants from its children and deliberately not
 * mouseWheelMove, which leaves the wheel to the single delivery
 * Component::mouseWheelMove already makes.
 *
 * Both halves of that are properties of JUCE rather than of StemLab, so they
 * are pinned here: a JUCE upgrade that gave MouseListener::mouseWheelMove a
 * forwarding default, or that stopped Component::mouseWheelMove walking up
 * the parents, would otherwise bring the double-handling (or a lane list that
 * no longer scrolls at all) back silently.
 *
 * Component::internalMouseWheel runs the two legs this test calls by hand:
 * target->mouseWheelMove(...) first, then MouseListenerList::sendMouseEvent
 * with &MouseListener::mouseWheelMove for every deep listener up the tree.
 * Neither entry point is public, so the legs are exercised directly.
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <cassert>

namespace
{
/** The lane list's viewport in miniature: it counts the wheels that reach it
    and stops them there, so a second forward shows up as a second count. */
struct WheelCounter final : public juce::Component
{
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override
    {
        ++wheels;
    }

    int wheels = 0;
};

/** The shape of StemLaneComponent's DescendantMouseRelay: the four events the
    row wants from its children, and no mouseWheelMove. */
struct FourEventRelay final : public juce::MouseListener
{
    void mouseEnter(const juce::MouseEvent&) override { ++carried; }
    void mouseExit(const juce::MouseEvent&) override { ++carried; }
    void mouseDrag(const juce::MouseEvent&) override { ++carried; }
    void mouseUp(const juce::MouseEvent&) override { ++carried; }

    int carried = 0;
};
}  // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    WheelCounter viewport;
    juce::Component lane, well;

    viewport.setSize(100, 200);
    lane.setSize(100, 40);
    well.setSize(96, 36);

    viewport.addAndMakeVisible(lane);
    lane.addAndMakeVisible(well);

    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();

    // One detent of a physical wheel, landing on the well.
    const juce::MouseWheelDetails wheel{0.0f, 1.0f, false, false, false};

    const juce::MouseEvent onWell{source,      {10.0f, 10.0f}, juce::ModifierKeys(),
                                  1.0f,        0.0f,           0.0f,
                                  0.0f,        0.0f,           &well,
                                  &well,       now,            {10.0f, 10.0f},
                                  now,         1,              false};

    FourEventRelay relay;
    juce::MouseListener& relayLeg = relay;

    // Leg one, the virtual: the well does not consume the wheel, so
    // Component::mouseWheelMove walks it up through the lane to the viewport.
    // This is the delivery the lane list scrolls on, and there must be one.
    well.mouseWheelMove(onWell, wheel);
    assert(viewport.wheels == 1);

    // Leg two as it runs today: the deep listener is a plain MouseListener
    // that does not override mouseWheelMove, and MouseListener's default body
    // is empty - so the listener leg adds nothing to the count.
    relayLeg.mouseWheelMove(onWell.getEventRelativeTo(&lane), wheel);
    assert(viewport.wheels == 1);

    // Why the relay may not be the lane itself: a juce::Component registered
    // as its own deep listener answers leg two through
    // Component::mouseWheelMove, which forwards. That second count is the
    // two-rows-per-notch scroll this test exists to keep from coming back.
    juce::MouseListener& laneAsItsOwnListener = lane;
    laneAsItsOwnListener.mouseWheelMove(onWell.getEventRelativeTo(&lane), wheel);
    assert(viewport.wheels == 2);

    // The other half of the bargain: the four events the relay does override
    // still reach it through the MouseListener interface the dispatch calls,
    // which is what keeps the row's hover highlight covering its children.
    relayLeg.mouseEnter(onWell);
    relayLeg.mouseExit(onWell);
    relayLeg.mouseDrag(onWell);
    relayLeg.mouseUp(onWell);
    assert(relay.carried == 4);

    return 0;
}
