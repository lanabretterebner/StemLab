#pragma once

#include <JuceHeader.h>
#include <functional>

#include "StemLabTheme.h"
#include "StemLabLookAndFeel.h"

/*
    Nocturne widgets that have no stock JUCE equivalent. All of them draw
    exclusively from stemlab::theme and stemlab::icons; none of them talk to
    the processor - the editor wires callbacks.
*/
namespace stemlab::widgets
{
    /** 15px rounded include checkbox: accent fill + dark check when on. */
    class IncludeCheckbox final : public juce::Button
    {
    public:
        IncludeCheckbox();

        void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IncludeCheckbox)
    };

    /**
     * Square icon button. Default: 45% icon turning accent on hover (lane
     * controls). textColoured: full text-colour icon that stays
     * text-coloured on hover with only the hover fill (header settings).
     */
    class IconButton final : public juce::Button
    {
    public:
        using PathFactory = std::function<juce::Path(juce::Rectangle<float>)>;

        IconButton(const juce::String& name, PathFactory factory, float iconSize,
                   bool strokedIcon, float cornerRadius, bool outlined,
                   bool textColoured = false);

        void paintButton(juce::Graphics&, bool highlighted, bool down) override;

        /** Swap the glyph in place, for a control whose shape reports state -
            a transport button showing play or pause. */
        void setIcon(PathFactory factory)
        {
            makePath = std::move(factory);
            repaint();
        }

    private:
        PathFactory makePath;
        float iconSize;
        bool stroked;
        float radius;
        bool outlined;
        bool textColoured;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconButton)
    };

    /**
     * Header dropdown: an outlined pill carrying a leading icon, a label,
     * and a trailing caret. Clicking it opens the caller's menu; the arrows
     * beside it step the same choice without one.
     */
    class SelectorButton final : public juce::Button
    {
    public:
        using PathFactory = std::function<juce::Path(juce::Rectangle<float>)>;

        SelectorButton(const juce::String& name, PathFactory leadingIcon);

        void setLabel(const juce::String& newLabel);

        /** What the label needs, so the header can lay the pill out to fit. */
        int getPreferredWidth() const;

        void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    private:
        PathFactory makeIcon;
        juce::String label;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SelectorButton)
    };

    /**
     * Lane disclosure twisty: a chevron that points down while a lane's
     * children are shown and right while they are collapsed. Lanes with no
     * children keep the column but hide the button, so every checkbox and
     * name stays on the same grid.
     */
    class DisclosureButton final : public juce::Button
    {
    public:
        DisclosureButton();

        void setExpanded(bool shouldBeExpanded);
        bool isExpanded() const noexcept { return expanded; }

        void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    private:
        bool expanded = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DisclosureButton)
    };

    /** 34px circular play/pause with accent border and glyph. */
    class PlayCircleButton final : public juce::Button
    {
    public:
        PlayCircleButton();

        void setShowPause(bool shouldShowPause);
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    private:
        bool showPause = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayCircleButton)
    };

    /** Neutral-outline record button with the accent dot; pulses while armed. */
    class RecordButton final : public juce::Button
    {
    public:
        explicit RecordButton(const juce::String& label);

        void setRecordingActive(bool active);
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    private:
        bool recording = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordButton)
    };

    /**
     * The Separate split control: a Refine toggle segment and the primary
     * Separate action in one accent-filled shell with an outer glow.
     */
    class SeparateSplitControl final : public juce::Component,
                                       public juce::SettableTooltipClient
    {
    public:
        SeparateSplitControl();

        std::function<void()> onSeparate;
        std::function<void(bool)> onRefineChanged;

        void setRefineOn(bool on);
        bool isRefineOn() const noexcept { return refineOn; }

        void setSeparateEnabled(bool enabled);
        bool isSeparateActionEnabled() const noexcept { return separateEnabled; }

        /** Whether the Refine segment accepts clicks. False while a job
            runs: the flag is read once at launch to build the command
            line, so a mid-run flip would silently apply to the next job
            while appearing to change this one. */
        void setRefineInteractive(bool interactive);
        bool isRefineInteractive() const noexcept { return refineInteractive; }

        /** The action segment's label: "Separate" normally, "Cancel" /
            "Cancelling..." while a job runs (the editor decides). */
        void setActionText(const juce::String& text);

        /** The tip shown over the Refine segment.

            This is one component holding two segments, so the single
            tooltip a SettableTooltipClient carries described whichever
            segment was set last - Refine's text appeared over Separate.
            setTooltip() still describes the action segment; this describes
            Refine, and getTooltip() picks between them by what is hovered. */
        void setRefineTooltip(const juce::String& text) { refineTooltip = text; }

        juce::String getTooltip() override
        {
            return hoverRefine ? refineTooltip : juce::SettableTooltipClient::getTooltip();
        }

        void paint(juce::Graphics&) override;
        void resized() override {}
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        juce::Rectangle<int> refineArea() const;

        bool refineOn = true;
        bool separateEnabled = false;
        bool refineInteractive = true;
        bool hoverRefine = false, hoverSeparate = false;
        juce::String actionText{"Separate"};
        juce::String refineTooltip;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SeparateSplitControl)
    };

    /** 3px seek bar; reports normalised position on click/drag. */
    class Scrubber final : public juce::Component
    {
    public:
        Scrubber();

        std::function<void(double)> onSeek;

        void setPosition(double normalised);

        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;

        /** juce::Button repaints itself when its enablement flips; a plain
            Component does not, and paint() reads isEnabled(). Without this
            the bar keeps whichever look it had when it was last drawn for
            some other reason - a hover, or a position that moved. */
        void enablementChanged() override { repaint(); }

    private:
        void applySeek(const juce::MouseEvent&);

        double position = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Scrubber)
    };

    /**
     * Header zoom slider: a thin track filled up to the knob, dragged or
     * clicked anywhere along its length.
     *
     * The position is normalised; the caller maps it to a zoom factor. That
     * mapping is exponential rather than linear (see the editor), so the
     * useful low end of the range is not crammed into the first few pixels.
     */
    class ZoomSlider final : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        ZoomSlider();

        std::function<void(double)> onValueChanged;

        void setValue(double normalised);
        double getValue() const noexcept { return value; }

        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        /** paint() reads isEnabled(); see Scrubber above. */
        void enablementChanged() override { repaint(); }

    private:
        void applyDrag(const juce::MouseEvent&);

        /** Where the knob's centre may travel, in local x. */
        juce::Range<float> knobTravel() const;

        double value = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZoomSlider)
    };

    /** Two-option segmented control (Original | Stems). */
    class SegmentedControl final : public juce::Component
    {
    public:
        SegmentedControl(const juce::String& first, const juce::String& second);

        std::function<void(int)> onSelected;

        void setSelectedIndex(int index);
        int getSelectedIndex() const noexcept { return selected; }

        void paint(juce::Graphics&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;

        /** paint() reads isEnabled(); see Scrubber above. */
        void enablementChanged() override { repaint(); }

    private:
        juce::String labels[2];
        int selected = 1;
        int hovered = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SegmentedControl)
    };

    /**
     * The footer status indicator: a spinning arc while a job runs, the
     * check glyph once one has finished, a red X when the last thing the
     * status line said was a failure, nothing before the first job.
     * The editor advances the spin from its UI timer via animate().
     */
    class StatusIndicator final : public juce::Component
    {
    public:
        enum class State
        {
            idle,
            running,
            done,
            error
        };

        StatusIndicator() { setInterceptsMouseClicks(false, false); }

        void setState(State newState)
        {
            if (state != newState)
            {
                state = newState;
                repaint();
            }
        }

        /** One UI-timer tick: keeps the arc turning while running. */
        void animate()
        {
            if (state == State::running)
                repaint();
        }

        void paint(juce::Graphics&) override;

    private:
        State state = State::idle;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusIndicator)
    };

    /** 1px divider that fades to transparent over 48px at each end. */
    class FadingDivider final : public juce::Component
    {
    public:
        FadingDivider() { setInterceptsMouseClicks(false, false); }

        void paint(juce::Graphics&) override;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FadingDivider)
    };
}
