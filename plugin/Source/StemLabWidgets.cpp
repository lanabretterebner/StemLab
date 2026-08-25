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

        if (getToggleState())
        {
            g.setColour(theme::colours::checkboxFill());
            g.fillRoundedRectangle(box, lanes::checkboxRadius);

            g.setColour(theme::colours::checkboxCheck());
            g.strokePath(stemlab::icons::check(box.reduced(3.5f)),
                         juce::PathStrokeType(1.8f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
        }
        else
        {
            if (highlighted)
            {
                g.setColour(theme::colours::hoverFill());
                g.fillRoundedRectangle(box, lanes::checkboxRadius);
            }

            g.setColour(theme::colours::checkboxBorder());
            g.drawRoundedRectangle(box.reduced(lanes::checkboxBorder * 0.5f),
                                   lanes::checkboxRadius, lanes::checkboxBorder);
        }
    }

    // ---------------------------------------------------------- icon button

    IconButton::IconButton(const juce::String& name, PathFactory factory, float iconSizeIn,
                           bool strokedIcon, float cornerRadius, bool outlinedIn)
        : juce::Button(name), makePath(std::move(factory)), iconSize(iconSizeIn),
          stroked(strokedIcon), radius(cornerRadius), outlined(outlinedIn)
    {
    }

    void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

        const bool hover = (highlighted || down) && isEnabled();

        if (hover)
        {
            g.setColour(getToggleState() || getName() == "layers"
                            ? theme::colours::accentTint10()
                            : theme::colours::hoverFill());
            g.fillRoundedRectangle(bounds, radius);
        }

        if (outlined)
        {
            g.setColour(theme::colours::outline());
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }

        auto iconColour = hover ? theme::colours::accent() : theme::colours::text45();

        if (!isEnabled())
            iconColour = iconColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

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

        if (hover)
        {
            g.setColour(theme::colours::hoverFill());
            g.fillRoundedRectangle(bounds, theme::metrics::buttons::radius);
        }

        g.setColour(theme::colours::outline());
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

        // Outer glow + accent-700 shell + accent-400 border.
        if (separateEnabled)
            juce::DropShadow(theme::colours::accentGlow(), 11, {}).drawForRectangle(
                g, getLocalBounds());

        g.setColour(theme::colours::primaryFill());
        g.fillRoundedRectangle(bounds, radius);

        const auto refine = refineArea();

        // Refine segment (accent-900, rounded on the left only).
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(refine);

            g.setColour(hoverRefine ? theme::colours::refineFillHover()
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

            g.setColour(theme::colours::refineText());
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

            g.setColour(refineOn ? theme::colours::pillTrack() : theme::colours::pillTrackOff());
            g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);

            const float knob = static_cast<float>(source::pillKnob);
            const float knobY = pill.getCentreY() - knob * 0.5f;
            const float knobX = refineOn ? pill.getRight() - knob - 2.0f : pill.getX() + 2.0f;

            g.setColour(theme::colours::pillKnob());
            g.fillEllipse(knobX, knobY, knob, knob);
        }

        // Separate label.
        {
            auto separate = getLocalBounds().withTrimmedLeft(refine.getRight() + 1);

            auto textColour = theme::colours::primaryText();

            if (!separateEnabled)
                textColour = textColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

            g.setColour(textColour);
            g.setFont(theme::fonts::separateLabel());
            g.drawText("Separate", separate, juce::Justification::centred, false);
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
        if (getWidth() <= 0)
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

        g.setColour(theme::colours::outline());
        g.drawRoundedRectangle(bounds, transport::abRadius, 1.0f);

        const int half = getWidth() / 2;

        for (int i = 0; i < 2; ++i)
        {
            auto segment = getLocalBounds();
            segment = i == 0 ? segment.removeFromLeft(half) : segment.withTrimmedLeft(half);

            const bool isActive = selected == i;

            if (isActive)
            {
                // Inset accent ring on the active option.
                juce::Graphics::ScopedSaveState save(g);
                g.reduceClipRegion(segment.reduced(1));

                g.setColour(theme::colours::accent());
                g.drawRoundedRectangle(bounds.reduced(1.5f), transport::abRadius - 1.5f, 1.0f);
            }
            else if (hovered == i && isEnabled())
            {
                juce::Graphics::ScopedSaveState save(g);
                g.reduceClipRegion(segment.reduced(1));

                g.setColour(theme::colours::hoverFill());
                g.fillRoundedRectangle(bounds.reduced(1.0f), transport::abRadius - 1.0f);
            }

            auto textColour = isActive ? theme::colours::accent() : theme::colours::text50();

            if (!isEnabled())
                textColour = textColour.withMultipliedAlpha(theme::metrics::disabledOpacity);

            g.setColour(textColour);
            g.setFont(theme::fonts::time());
            g.drawText(labels[i], segment, juce::Justification::centred, false);
        }

        // Divider between segments.
        g.setColour(theme::colours::outline());
        g.drawVerticalLine(half, bounds.getY() + 3.0f, bounds.getBottom() - 3.0f);
    }

    void SegmentedControl::mouseMove(const juce::MouseEvent& event)
    {
        hovered = event.position.x < static_cast<float>(getWidth()) * 0.5f ? 0 : 1;
    }

    void SegmentedControl::mouseExit(const juce::MouseEvent&) { hovered = -1; }

    void SegmentedControl::mouseUp(const juce::MouseEvent& event)
    {
        if (!isEnabled() || !getLocalBounds().contains(event.getPosition()))
            return;

        const int index = event.position.x < static_cast<float>(getWidth()) * 0.5f ? 0 : 1;

        setSelectedIndex(index);

        if (onSelected)
            onSelected(index);
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
