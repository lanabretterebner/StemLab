#include "StemLabWidgets.h"

namespace theme = stemlab::theme;

namespace stemlab::widgets
{
    // ------------------------------------------------------------- checkbox

    IncludeCheckbox::IncludeCheckbox() : juce::Button("include")
    {
        setClickingTogglesState(true);
    }

    void IncludeCheckbox::paintButton(juce::Graphics& g, bool highlighted, bool)
    {
        namespace lanes = theme::metrics::lanes;

        const auto box = getLocalBounds()
                             .withSizeKeepingCentre(lanes::checkbox, lanes::checkbox)
                             .toFloat();

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        if (getToggleState())
        {
            g.setColour(dimmed(theme::colours::checkboxFill()));
            g.fillRoundedRectangle(box, lanes::checkboxRadius);

            g.setColour(dimmed(theme::colours::checkboxCheck()));
            g.strokePath(stemlab::icons::check(box.reduced(3.5f)),
                         juce::PathStrokeType(1.8f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
        }
        else
        {
            if (highlighted && isEnabled())
            {
                g.setColour(theme::colours::hoverFill());
                g.fillRoundedRectangle(box, lanes::checkboxRadius);
            }

            g.setColour(dimmed(theme::colours::checkboxBorder()));
            g.drawRoundedRectangle(box.reduced(lanes::checkboxBorder * 0.5f),
                                   lanes::checkboxRadius, lanes::checkboxBorder);
        }
    }

    // ---------------------------------------------------------- icon button

    IconButton::IconButton(const juce::String& name, PathFactory factory, float iconSizeIn,
                           bool strokedIcon, float cornerRadius, bool outlinedIn,
                           bool textColouredIn)
        : juce::Button(name), makePath(std::move(factory)), iconSize(iconSizeIn),
          stroked(strokedIcon), radius(cornerRadius), outlined(outlinedIn),
          textColoured(textColouredIn)
    {
    }

    void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

        const bool hover = (highlighted || down) && isEnabled();

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        if (hover)
        {
            g.setColour(!textColoured && (getToggleState() || getName() == "layers")
                            ? theme::colours::accentTint10()
                            : theme::colours::hoverFill());
            g.fillRoundedRectangle(bounds, radius);
        }

        if (outlined)
        {
            g.setColour(dimmed(theme::colours::outline()));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }

        auto iconColour = textColoured
                              ? theme::colours::text()
                              : (hover ? theme::colours::accent() : theme::colours::text45());

        iconColour = dimmed(iconColour);

        g.setColour(iconColour);

        const auto iconArea = getLocalBounds()
                                  .toFloat()
                                  .withSizeKeepingCentre(iconSize, iconSize);

        const auto path = makePath(iconArea);

        if (stroked)
            g.strokePath(path, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        else
            g.fillPath(path);
    }

    // ------------------------------------------------------------ selector

    SelectorButton::SelectorButton(const juce::String& name, PathFactory leadingIcon)
        : juce::Button(name), makeIcon(std::move(leadingIcon))
    {
    }

    void SelectorButton::setLabel(const juce::String& newLabel)
    {
        if (label != newLabel)
        {
            label = newLabel;
            repaint();
        }
    }

    int SelectorButton::getPreferredWidth() const
    {
        namespace header = theme::metrics::header;

        const juce::Font font{theme::fonts::bodyMedium()};

        const int textWidth =
            juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, label)) + 2;

        const int width = header::selectorPadX + header::selectorIcon + header::selectorGap +
                          textWidth + header::selectorGap + header::selectorCaret +
                          header::selectorPadX;

        return juce::jmax(header::selectorMinWidth, width);
    }

    void SelectorButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
    {
        namespace header = theme::metrics::header;

        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

        const bool hover = (highlighted || down) && isEnabled();

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        if (hover)
        {
            g.setColour(theme::colours::accentTint10());
            g.fillRoundedRectangle(bounds, header::selectorRadius);
        }

        g.setColour(dimmed(theme::colours::outline()));
        g.drawRoundedRectangle(bounds, header::selectorRadius, 1.0f);

        auto content = getLocalBounds().reduced(header::selectorPadX, 0);

        const auto iconArea = content.removeFromLeft(header::selectorIcon)
                                  .toFloat()
                                  .withSizeKeepingCentre(static_cast<float>(header::selectorIcon),
                                                         static_cast<float>(header::selectorIcon));

        g.setColour(dimmed(hover ? theme::colours::accent() : theme::colours::text75()));

        if (makeIcon)
            g.fillPath(makeIcon(iconArea));

        content.removeFromLeft(header::selectorGap);

        const auto caretArea =
            content.removeFromRight(header::selectorCaret)
                .toFloat()
                .withSizeKeepingCentre(static_cast<float>(header::selectorCaret),
                                       static_cast<float>(header::selectorCaret) * 0.55f);

        g.setColour(dimmed(theme::colours::text45()));
        g.strokePath(stemlab::icons::chevron(caretArea, stemlab::icons::ChevronDirection::down),
                     juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));

        content.removeFromRight(header::selectorGap);

        g.setColour(dimmed(theme::colours::text()));
        g.setFont(theme::fonts::bodyMedium());
        g.drawText(label, content, juce::Justification::centredLeft, false);
    }

    // ----------------------------------------------------------- disclosure

    DisclosureButton::DisclosureButton() : juce::Button("disclosure")
    {
        setTooltip("Show or hide this stem's sub-stems");
    }

    void DisclosureButton::setExpanded(bool shouldBeExpanded)
    {
        if (expanded != shouldBeExpanded)
        {
            expanded = shouldBeExpanded;
            repaint();
        }
    }

    void DisclosureButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
    {
        namespace lanes = theme::metrics::lanes;

        const bool hover = (highlighted || down) && isEnabled();

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        auto colour = dimmed(hover ? theme::colours::accent() : theme::colours::text45());

        g.setColour(colour);

        // The chevron keeps the same proportions in both orientations: a
        // fixed box would squash the collapsed one against the expanded one.
        const auto span = static_cast<float>(lanes::twistyIcon);
        const auto depth = span * 0.62f;

        const auto icon = getLocalBounds().toFloat().withSizeKeepingCentre(
            expanded ? span : depth, expanded ? depth : span);

        g.strokePath(stemlab::icons::chevron(icon, expanded
                                                ? stemlab::icons::ChevronDirection::down
                                                : stemlab::icons::ChevronDirection::right),
                     juce::PathStrokeType(1.5f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
    }

    // ----------------------------------------------------------- play circle

    PlayCircleButton::PlayCircleButton() : juce::Button("play") {}

    void PlayCircleButton::setShowPause(bool shouldShowPause)
    {
        if (showPause != shouldShowPause)
        {
            showPause = shouldShowPause;
            repaint();
        }
    }

    void PlayCircleButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
    {
        auto circle = getLocalBounds().toFloat().reduced(0.5f);
        circle = circle.withSizeKeepingCentre(juce::jmin(circle.getWidth(), circle.getHeight()),
                                              juce::jmin(circle.getWidth(), circle.getHeight()));

        const bool hover = (highlighted || down) && isEnabled();

        if (hover)
        {
            g.setColour(theme::colours::accentTint13());
            g.fillEllipse(circle);
        }

        auto accent = theme::colours::accent();

        if (!isEnabled())
            accent = theme::colours::dimDisabled(accent);

        g.setColour(accent);
        g.drawEllipse(circle, 1.0f);

        auto glyphArea = circle.withSizeKeepingCentre(circle.getWidth() * 0.34f,
                                                      circle.getHeight() * 0.36f);

        if (!showPause)
            glyphArea.translate(circle.getWidth() * 0.03f, 0.0f);

        g.fillPath(showPause ? stemlab::icons::pause(glyphArea)
                             : stemlab::icons::play(glyphArea));
    }

    // -------------------------------------------------------------- record

    RecordButton::RecordButton(const juce::String& label) : juce::Button(label) {}

    void RecordButton::setRecordingActive(bool active)
    {
        if (recording != active)
        {
            recording = active;
            repaint();
        }
    }

    void RecordButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
    {
        namespace source = theme::metrics::source;

        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

        const bool hover = (highlighted || down) && isEnabled();

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        if (hover)
        {
            g.setColour(theme::colours::hoverFill());
            g.fillRoundedRectangle(bounds, theme::metrics::buttons::radius);
        }

        g.setColour(dimmed(theme::colours::outline()));
        g.drawRoundedRectangle(bounds, theme::metrics::buttons::radius, 1.0f);

        // The accent dot doubles as the recording indicator: solid when
        // idle, pulsing while armed/recording.
        auto dotColour = theme::colours::recordDot();

        if (recording)
        {
            const auto phase = static_cast<double>(juce::Time::getMillisecondCounter() % 1200u) /
                               1200.0;

            dotColour = dotColour.withAlpha(
                0.45f + 0.55f * static_cast<float>(0.5 + 0.5 * std::sin(phase * 2.0 *
                                                                        juce::MathConstants<double>::pi)));
        }

        dotColour = dimmed(dotColour);

        auto content = getLocalBounds().reduced(theme::metrics::buttons::padX, 0);

        const auto dotArea = content.removeFromLeft(source::recordDot);

        g.setColour(dotColour);
        g.fillEllipse(dotArea.toFloat()
                          .withSizeKeepingCentre(static_cast<float>(source::recordDot),
                                                 static_cast<float>(source::recordDot)));

        content.removeFromLeft(6);

        g.setColour(dimmed(theme::colours::text()));
        g.setFont(theme::fonts::body());
        g.drawText(getButtonText(), content, juce::Justification::centredLeft, false);
    }

    // ---------------------------------------------------- separate control

    SeparateSplitControl::SeparateSplitControl()
    {
        setRepaintsOnMouseActivity(true);
    }

    void SeparateSplitControl::setRefineOn(bool on)
    {
        if (refineOn != on)
        {
            refineOn = on;
            repaint();
        }
    }

    void SeparateSplitControl::setSeparateEnabled(bool enabled)
    {
        if (separateEnabled != enabled)
        {
            separateEnabled = enabled;
            repaint();
        }
    }

    void SeparateSplitControl::setRefineInteractive(bool interactive)
    {
        if (refineInteractive != interactive)
        {
            refineInteractive = interactive;
            repaint();
        }
    }

    void SeparateSplitControl::setActionText(const juce::String& text)
    {
        if (actionText != text)
        {
            actionText = text;
            repaint();
        }
    }

    juce::Rectangle<int> SeparateSplitControl::refineArea() const
    {
        namespace source = theme::metrics::source;

        // Refine segment: label + pill plus padding. The label renders with
        // +0.02em kerning, so measure with a little slack.
        const juce::Font font{theme::fonts::refineLabel()};

        const int labelWidth =
            juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, "Refine")) + 4;

        const int width = source::refinePadLeft + labelWidth + source::pillGap +
                          source::pillWidth + source::refinePadRight;

        return getLocalBounds().removeFromLeft(width);
    }

    void SeparateSplitControl::paint(juce::Graphics& g)
    {
        namespace source = theme::metrics::source;

        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        const auto radius = source::separateRadius;

        // The outer glow is painted by the editor behind this control - a
        // shadow drawn in here would be clipped to the component bounds.
        //
        // Only the action half of this fill survives: the Refine segment
        // overpaints it below, so dimming the whole shell dims the action
        // segment alone, which is the half the flag speaks for.
        g.setColour(theme::colours::dimIfDisabled(theme::colours::primaryFill(),
                                                  separateEnabled));
        g.fillRoundedRectangle(bounds, radius);

        const auto refine = refineArea();

        // Refine segment (accent-900, rounded on the left only).
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(refine);

            g.setColour(hoverRefine && refineInteractive ? theme::colours::refineFillHover()
                                                         : theme::colours::refineFill());
            g.fillRoundedRectangle(bounds, radius);
            g.fillRect(bounds.withTrimmedLeft(radius)
                           .withWidth(static_cast<float>(refine.getWidth()) - radius));
        }

        // Divider between the segments.
        g.setColour(theme::colours::refineDivider());
        g.drawVerticalLine(refine.getRight(), bounds.getY() + 1.0f, bounds.getBottom() - 1.0f);

        // Separate hover wash.
        if (hoverSeparate && separateEnabled)
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(getLocalBounds().withTrimmedLeft(refine.getRight() + 1));
            g.setColour(theme::colours::accentHover18());
            g.fillRoundedRectangle(bounds, radius);
        }

        // Refine label + pill switch.
        {
            auto content = refine.reduced(0, 0);
            content.removeFromLeft(source::refinePadLeft);

            // A locked segment dims exactly the way the action label
            // already does, so the two halves of the control read as one
            // disabled thing rather than one greyed and one live.
            const auto dim = [this](juce::Colour c)
            { return theme::colours::dimIfDisabled(c, refineInteractive); };

            g.setColour(dim(theme::colours::refineText()));
            juce::Font refineFont{theme::fonts::refineLabel()};
            g.setFont(refineFont.withExtraKerningFactor(0.02f));

            const juce::Font font{theme::fonts::refineLabel()};
            const int labelWidth =
                juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, "Refine")) + 4;

            g.drawText("Refine", content.removeFromLeft(labelWidth),
                       juce::Justification::centredLeft, false);

            content.removeFromLeft(source::pillGap);

            auto pill = content.removeFromLeft(source::pillWidth)
                            .withSizeKeepingCentre(source::pillWidth, source::pillHeight)
                            .toFloat();

            g.setColour(
                dim(refineOn ? theme::colours::pillTrack() : theme::colours::pillTrackOff()));
            g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);

            const float knob = static_cast<float>(source::pillKnob);
            const float knobY = pill.getCentreY() - knob * 0.5f;
            const float knobX = refineOn ? pill.getRight() - knob - 2.0f : pill.getX() + 2.0f;

            g.setColour(dim(theme::colours::pillKnob()));
            g.fillEllipse(knobX, knobY, knob, knob);
        }

        // Action label ("Separate", or "Cancel" / "Cancelling..." mid-job).
        {
            auto separate = getLocalBounds().withTrimmedLeft(refine.getRight() + 1);

            g.setColour(theme::colours::dimIfDisabled(theme::colours::primaryText(),
                                                      separateEnabled));
            g.setFont(theme::fonts::separateLabel());
            g.drawText(actionText, separate, juce::Justification::centred, false);
        }

        /*
         * The edge is drawn twice, clipped, rather than once for both
         * halves: Refine and the action segment lock independently - Refine
         * keeps toggling before a source is loaded, and stops while a job
         * runs - so a single outline would have to lie about one of them.
         *
         * No +1 on the action half's trim, unlike the hover wash and the
         * label above: the two clips have to meet exactly, or a pixel of
         * the outline goes unpainted at the seam.
         */
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(refine);
            g.setColour(theme::colours::dimIfDisabled(theme::colours::primaryEdge(),
                                                      refineInteractive));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }

        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(getLocalBounds().withTrimmedLeft(refine.getRight()));
            g.setColour(theme::colours::dimIfDisabled(theme::colours::primaryEdge(),
                                                      separateEnabled));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }
    }

    void SeparateSplitControl::mouseMove(const juce::MouseEvent& event)
    {
        const bool inRefine = refineArea().contains(event.getPosition());

        hoverRefine = inRefine;
        hoverSeparate = !inRefine;
        repaint();
    }

    void SeparateSplitControl::mouseExit(const juce::MouseEvent&)
    {
        hoverRefine = hoverSeparate = false;
        repaint();
    }

    void SeparateSplitControl::mouseUp(const juce::MouseEvent& event)
    {
        if (!getLocalBounds().contains(event.getPosition()))
            return;

        if (refineArea().contains(event.getPosition()))
        {
            // Return rather than falling through: a click on a locked
            // Refine segment must do nothing at all, not reach the action
            // segment sitting next to it.
            if (!refineInteractive)
                return;

            refineOn = !refineOn;
            repaint();

            if (onRefineChanged)
                onRefineChanged(refineOn);
        }
        else if (separateEnabled)
        {
            if (onSeparate)
                onSeparate();
        }
    }

    // ------------------------------------------------------------ scrubber

    Scrubber::Scrubber() { setRepaintsOnMouseActivity(true); }

    void Scrubber::setPosition(double normalised)
    {
        const auto clamped = juce::jlimit(0.0, 1.0, normalised);

        if (std::abs(clamped - position) > 0.0005)
        {
            position = clamped;
            repaint();
        }
    }

    void Scrubber::paint(juce::Graphics& g)
    {
        namespace transport = theme::metrics::transport;

        // The track has to dim as well as the fill: with nothing loaded the
        // editor forces the position to 0, so the fill has no width and the
        // track is the only thing on screen to say the bar is inert.
        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        auto track = getLocalBounds()
                         .toFloat()
                         .withSizeKeepingCentre(static_cast<float>(getWidth()),
                                                static_cast<float>(transport::scrubHeight));

        g.setColour(dimmed(theme::colours::scrubTrack()));
        g.fillRoundedRectangle(track, transport::scrubRadius);

        auto fill = track.withWidth(track.getWidth() * static_cast<float>(position));

        g.setColour(dimmed(theme::colours::scrubFill()));
        g.fillRoundedRectangle(fill, transport::scrubRadius);
    }

    void Scrubber::applySeek(const juce::MouseEvent& event)
    {
        // Plain Components still receive mouse events while disabled.
        if (!isEnabled() || getWidth() <= 0)
            return;

        const auto width = static_cast<double>(getWidth());
        const auto x = static_cast<double>(event.position.x);

        auto normalised = juce::jlimit(0.0, 1.0, x / width);

        /*
            x / width can never reach 1.0, so the end of the track has to be
            claimed by the last pixel of the bar - and that pixel is further
            from width than it looks.

            JUCE hit-tests a component by rounding the local point to whole
            units, so no click is ever delivered past width - 0.5. The panel
            is drawn through a scale transform as well, which puts adjacent
            screen pixels 1 / scale apart in local units and leaves the bar's
            right edge wherever the scale lands it, usually mid-pixel. The
            rightmost pixel the pointer can actually reach therefore sits
            somewhere in [width - 0.5 - 1/scale, width - 0.5) - 516.7 of 518
            at the default window size, which is short of the width - 1 this
            replaces. That is why the snap never fired, and why clicking the
            end of a twenty-minute source used to land three seconds inside
            it and take two presses of Play to restart.

            That interval is exactly one screen pixel wide, so snapping all
            of it makes the final pixel mean the end at every window size
            while leaving the pixel before it a position of its own rather
            than a second helping of the end.

            The start needs no such help: the clamp above already pulls the
            bar's leftmost pixel, whose local x is fractionally negative, up
            to 0.0.
        */
        const auto scale =
            static_cast<double>(juce::Component::getApproximateScaleFactorForComponent(this));
        const auto screenPixel = scale > 0.0 ? 1.0 / scale : 1.0;

        if (x >= width - 0.5 - screenPixel)
            normalised = 1.0;

        position = normalised;
        repaint();

        if (onSeek)
            onSeek(normalised);
    }

    void Scrubber::mouseDown(const juce::MouseEvent& event) { applySeek(event); }
    void Scrubber::mouseDrag(const juce::MouseEvent& event) { applySeek(event); }

    // ---------------------------------------------------------- zoom slider

    ZoomSlider::ZoomSlider() { setRepaintsOnMouseActivity(true); }

    void ZoomSlider::setValue(double normalised)
    {
        const auto clamped = juce::jlimit(0.0, 1.0, normalised);

        if (std::abs(clamped - value) > 0.0005)
        {
            value = clamped;
            repaint();
        }
    }

    juce::Range<float> ZoomSlider::knobTravel() const
    {
        namespace header = theme::metrics::header;

        const auto half = static_cast<float>(header::zoomKnob) * 0.5f;

        // The knob is inset by its own radius at each end, so it stays
        // inside the component at both extremes instead of half hanging off.
        return {half, juce::jmax(half, static_cast<float>(getWidth()) - half)};
    }

    void ZoomSlider::paint(juce::Graphics& g)
    {
        namespace header = theme::metrics::header;

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        const auto travel = knobTravel();
        const auto knobX = travel.getStart() +
                           travel.getLength() * static_cast<float>(value);

        auto track = getLocalBounds().toFloat().withSizeKeepingCentre(
            static_cast<float>(getWidth()), static_cast<float>(header::zoomTrackHeight));

        const auto radius = static_cast<float>(header::zoomTrackHeight) * 0.5f;

        g.setColour(dimmed(theme::colours::pillTrackOff()));
        g.fillRoundedRectangle(track, radius);

        // Fill up to the knob's centre rather than to the pointer, so the
        // bar and the knob agree at both ends of the travel.
        g.setColour(dimmed(theme::colours::accent()));
        g.fillRoundedRectangle(track.withWidth(juce::jmax(radius * 2.0f, knobX)), radius);

        const auto knob = juce::Rectangle<float>(static_cast<float>(header::zoomKnob),
                                                 static_cast<float>(header::zoomKnob))
                              .withCentre({knobX, getLocalBounds().toFloat().getCentreY()});

        g.setColour(dimmed(theme::colours::accent300()));
        g.fillEllipse(knob);
    }

    void ZoomSlider::applyDrag(const juce::MouseEvent& event)
    {
        // Plain Components still receive mouse events while disabled.
        if (!isEnabled() || getWidth() <= 0)
            return;

        const auto travel = knobTravel();

        if (travel.getLength() <= 0.0f)
            return;

        const auto normalised =
            juce::jlimit(0.0, 1.0, static_cast<double>((event.position.x - travel.getStart()) /
                                                       travel.getLength()));

        if (std::abs(normalised - value) < 0.0005)
            return;

        value = normalised;
        repaint();

        if (onValueChanged)
            onValueChanged(value);
    }

    void ZoomSlider::mouseDown(const juce::MouseEvent& event) { applyDrag(event); }
    void ZoomSlider::mouseDrag(const juce::MouseEvent& event) { applyDrag(event); }

    void ZoomSlider::mouseWheelMove(const juce::MouseEvent&,
                                    const juce::MouseWheelDetails& wheel)
    {
        if (!isEnabled())
            return;

        const auto stepped = juce::jlimit(0.0, 1.0, value + wheel.deltaY * 0.5);

        if (std::abs(stepped - value) < 0.0005)
            return;

        value = stepped;
        repaint();

        if (onValueChanged)
            onValueChanged(value);
    }

    // ----------------------------------------------------------- segmented

    SegmentedControl::SegmentedControl(const juce::String& first, const juce::String& second)
    {
        labels[0] = first;
        labels[1] = second;
        setRepaintsOnMouseActivity(true);
    }

    void SegmentedControl::setSelectedIndex(int index)
    {
        const auto clamped = juce::jlimit(0, 1, index);

        if (selected != clamped)
        {
            selected = clamped;
            repaint();
        }
    }

    void SegmentedControl::paint(juce::Graphics& g)
    {
        namespace transport = theme::metrics::transport;

        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

        const auto dimmed = [enabled = isEnabled()](juce::Colour c)
        { return theme::colours::dimIfDisabled(c, enabled); };

        g.setColour(dimmed(theme::colours::outline()));
        g.drawRoundedRectangle(bounds, transport::abRadius, 1.0f);

        const int half = getWidth() / 2;

        for (int i = 0; i < 2; ++i)
        {
            auto segment = getLocalBounds();
            segment = i == 0 ? segment.removeFromLeft(half) : segment.withTrimmedLeft(half);

            /*
             * Each state is its own closed pill inset inside the shell.
             * The ring used to be the shell's own rounded rectangle clipped
             * to one half, so it lost its inner edge and its inner corners
             * and read as a broken box against the divider.
             */
            const auto pill = segment.toFloat().reduced(transport::abInset);

            const float pillRadius =
                juce::jmax(2.0f, transport::abRadius - transport::abInset);

            const bool isActive = selected == i;

            if (isActive)
            {
                g.setColour(dimmed(theme::colours::accentTint10()));
                g.fillRoundedRectangle(pill, pillRadius);

                g.setColour(dimmed(theme::colours::accent()));
                g.drawRoundedRectangle(pill, pillRadius, 1.0f);
            }
            else if (hovered == i && isEnabled())
            {
                g.setColour(theme::colours::hoverFill());
                g.fillRoundedRectangle(pill, pillRadius);
            }

            g.setColour(dimmed(isActive ? theme::colours::accent()
                                        : theme::colours::text50()));
            g.setFont(theme::fonts::time());
            g.drawText(labels[i], segment, juce::Justification::centred, false);
        }
    }

    void SegmentedControl::mouseMove(const juce::MouseEvent& event)
    {
        const int next = event.position.x < static_cast<float>(getWidth()) * 0.5f ? 0 : 1;

        if (hovered != next)
        {
            hovered = next;
            repaint();
        }
    }

    void SegmentedControl::mouseExit(const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    void SegmentedControl::mouseUp(const juce::MouseEvent& event)
    {
        if (!isEnabled() || !getLocalBounds().contains(event.getPosition()))
            return;

        const int index = event.position.x < static_cast<float>(getWidth()) * 0.5f ? 0 : 1;

        setSelectedIndex(index);

        if (onSelected)
            onSelected(index);
    }

    // ------------------------------------------------------------ status

    void StatusIndicator::paint(juce::Graphics& g)
    {
        if (state == State::idle)
            return;

        const auto bounds = getLocalBounds().toFloat();
        const auto square =
            bounds.withSizeKeepingCentre(juce::jmin(bounds.getWidth(), bounds.getHeight()),
                                         juce::jmin(bounds.getWidth(), bounds.getHeight()));

        if (state == State::error)
        {
            // reduced(2.5f) rather than the check's 1.5f: a corner-to-corner
            // X reads visually larger than a tick in the same box.
            g.setColour(theme::colours::statusError());
            g.strokePath(stemlab::icons::alert(square.reduced(2.5f)),
                         juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
            return;
        }

        if (state == State::done)
        {
            g.setColour(theme::colours::statusCheck());
            g.strokePath(stemlab::icons::check(square.reduced(1.5f)),
                         juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
            return;
        }

        /*
         * The runner: a 270-degree arc chasing its own tail once a second,
         * over a faint full ring so the shape stays a circle rather than a
         * flying comma. Driven by wall clock, so the repaint cadence only
         * affects smoothness, never speed.
         */
        const auto ring = square.reduced(1.5f);
        const auto radius = ring.getWidth() * 0.5f;

        const auto turn =
            static_cast<float>(juce::Time::getMillisecondCounter() % 1000u) / 1000.0f;

        const auto start = turn * juce::MathConstants<float>::twoPi;

        g.setColour(theme::colours::spinnerTrack());
        g.drawEllipse(ring, 1.6f);

        juce::Path arc;
        arc.addCentredArc(ring.getCentreX(), ring.getCentreY(), radius, radius, 0.0f, start,
                          start + juce::MathConstants<float>::twoPi * 0.75f, true);

        g.setColour(theme::colours::spinner());
        g.strokePath(arc, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // ------------------------------------------------------------- divider

    void FadingDivider::paint(juce::Graphics& g)
    {
        namespace footer = theme::metrics::footer;

        const auto y = static_cast<float>(getHeight()) * 0.5f;
        const auto width = static_cast<float>(getWidth());
        const auto fade = static_cast<float>(footer::dividerFade);

        const auto colour = theme::colours::divider();

        juce::ColourGradient gradient(colour.withAlpha(0.0f), 0.0f, y, colour.withAlpha(0.0f),
                                      width, y, false);

        gradient.addColour(juce::jlimit(0.0, 1.0, static_cast<double>(fade / width)), colour);
        gradient.addColour(juce::jlimit(0.0, 1.0, static_cast<double>(1.0f - fade / width)),
                           colour);

        g.setGradientFill(gradient);
        g.fillRect(0.0f, y - 0.5f, width, 1.0f);
    }
}
