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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        if (getToggleState())
        {
            g.setColour(theme::colours::checkboxFill().withMultipliedAlpha(dim));
            g.fillRoundedRectangle(box, lanes::checkboxRadius);

            g.setColour(theme::colours::checkboxCheck().withMultipliedAlpha(dim));
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

            g.setColour(theme::colours::checkboxBorder().withMultipliedAlpha(dim));
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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        if (hover)
        {
            g.setColour(!textColoured && (getToggleState() || getName() == "layers")
                            ? theme::colours::accentTint10()
                            : theme::colours::hoverFill());
            g.fillRoundedRectangle(bounds, radius);
        }

        if (outlined)
        {
            g.setColour(theme::colours::outline().withMultipliedAlpha(dim));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }

        auto iconColour = textColoured
                              ? theme::colours::text()
                              : (hover ? theme::colours::accent() : theme::colours::text45());

        iconColour = iconColour.withMultipliedAlpha(dim);

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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        if (hover)
        {
            g.setColour(theme::colours::accentTint10());
            g.fillRoundedRectangle(bounds, header::selectorRadius);
        }

        g.setColour(theme::colours::outline().withMultipliedAlpha(dim));
        g.drawRoundedRectangle(bounds, header::selectorRadius, 1.0f);

        auto content = getLocalBounds().reduced(header::selectorPadX, 0);

        const auto iconArea = content.removeFromLeft(header::selectorIcon)
                                  .toFloat()
                                  .withSizeKeepingCentre(static_cast<float>(header::selectorIcon),
                                                         static_cast<float>(header::selectorIcon));

        g.setColour((hover ? theme::colours::accent() : theme::colours::text75())
                        .withMultipliedAlpha(dim));

        if (makeIcon)
            g.fillPath(makeIcon(iconArea));

        content.removeFromLeft(header::selectorGap);

        const auto caretArea =
            content.removeFromRight(header::selectorCaret)
                .toFloat()
                .withSizeKeepingCentre(static_cast<float>(header::selectorCaret),
                                       static_cast<float>(header::selectorCaret) * 0.55f);

        g.setColour(theme::colours::text45().withMultipliedAlpha(dim));
        g.strokePath(stemlab::icons::chevron(caretArea, stemlab::icons::ChevronDirection::down),
                     juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));

        content.removeFromRight(header::selectorGap);

        g.setColour(theme::colours::text().withMultipliedAlpha(dim));
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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        auto colour = (hover ? theme::colours::accent() : theme::colours::text45())
                          .withMultipliedAlpha(dim);

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
            accent = accent.withMultipliedAlpha(theme::metrics::disabledOpacity);

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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        if (hover)
        {
            g.setColour(theme::colours::hoverFill());
            g.fillRoundedRectangle(bounds, theme::metrics::buttons::radius);
        }

        g.setColour(theme::colours::outline().withMultipliedAlpha(dim));
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

        if (!isEnabled())
            dotColour = dotColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

        auto content = getLocalBounds().reduced(theme::metrics::buttons::padX, 0);

        const auto dotArea = content.removeFromLeft(source::recordDot);

        g.setColour(dotColour);
        g.fillEllipse(dotArea.toFloat()
                          .withSizeKeepingCentre(static_cast<float>(source::recordDot),
                                                 static_cast<float>(source::recordDot)));

        content.removeFromLeft(6);

        auto textColour = theme::colours::text();

        if (!isEnabled())
            textColour = textColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

        g.setColour(textColour);
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
        g.setColour(theme::colours::primaryFill());
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
            const auto dim = [this](juce::Colour c) {
                return refineInteractive
                           ? c
                           : c.withMultipliedAlpha(theme::metrics::disabledOpacity);
            };

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

            auto textColour = theme::colours::primaryText();

            if (!separateEnabled)
                textColour = textColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

            g.setColour(textColour);
            g.setFont(theme::fonts::separateLabel());
            g.drawText(actionText, separate, juce::Justification::centred, false);
        }

        g.setColour(theme::colours::primaryEdge());
        g.drawRoundedRectangle(bounds, radius, 1.0f);
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

        auto track = getLocalBounds()
                         .toFloat()
                         .withSizeKeepingCentre(static_cast<float>(getWidth()),
                                                static_cast<float>(transport::scrubHeight));

        g.setColour(theme::colours::scrubTrack());
        g.fillRoundedRectangle(track, transport::scrubRadius);

        auto fill = track.withWidth(track.getWidth() * static_cast<float>(position));

        g.setColour(theme::colours::scrubFill());
        g.fillRoundedRectangle(fill, transport::scrubRadius);
    }

    void Scrubber::applySeek(const juce::MouseEvent& event)
    {
        // Plain Components still receive mouse events while disabled.
        if (!isEnabled() || getWidth() <= 0)
            return;

        const auto normalised = juce::jlimit(
            0.0, 1.0, static_cast<double>(event.position.x) / static_cast<double>(getWidth()));

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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        const auto travel = knobTravel();
        const auto knobX = travel.getStart() +
                           travel.getLength() * static_cast<float>(value);

        auto track = getLocalBounds().toFloat().withSizeKeepingCentre(
            static_cast<float>(getWidth()), static_cast<float>(header::zoomTrackHeight));

        const auto radius = static_cast<float>(header::zoomTrackHeight) * 0.5f;

        g.setColour(theme::colours::pillTrackOff().withMultipliedAlpha(dim));
        g.fillRoundedRectangle(track, radius);

        // Fill up to the knob's centre rather than to the pointer, so the
        // bar and the knob agree at both ends of the travel.
        g.setColour(theme::colours::accent().withMultipliedAlpha(dim));
        g.fillRoundedRectangle(track.withWidth(juce::jmax(radius * 2.0f, knobX)), radius);

        const auto knob = juce::Rectangle<float>(static_cast<float>(header::zoomKnob),
                                                 static_cast<float>(header::zoomKnob))
                              .withCentre({knobX, getLocalBounds().toFloat().getCentreY()});

        g.setColour(theme::colours::accent300().withMultipliedAlpha(dim));
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

        const float dim = isEnabled() ? 1.0f : theme::metrics::disabledOpacity;

        g.setColour(theme::colours::outline().withMultipliedAlpha(dim));
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
                g.setColour(theme::colours::accentTint10().withMultipliedAlpha(dim));
                g.fillRoundedRectangle(pill, pillRadius);

                g.setColour(theme::colours::accent().withMultipliedAlpha(dim));
                g.drawRoundedRectangle(pill, pillRadius, 1.0f);
            }
            else if (hovered == i && isEnabled())
            {
                g.setColour(theme::colours::hoverFill());
                g.fillRoundedRectangle(pill, pillRadius);
            }

            auto textColour = isActive ? theme::colours::accent() : theme::colours::text50();

            if (!isEnabled())
                textColour = textColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

            g.setColour(textColour);
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
