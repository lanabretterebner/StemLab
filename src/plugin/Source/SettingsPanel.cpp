#include "SettingsPanel.h"

#include "StemLabTheme.h"

namespace stemlab::widgets
{
    namespace
    {
        namespace card
        {
            constexpr int width = 700;
            constexpr int height = 496;
            constexpr float radius = 12.0f;
            constexpr int padX = 22;
            constexpr int padY = 18;

            constexpr int titleHeight = 24;
            constexpr int tabsHeight = 26;
            constexpr int tabsWidth = 200;
            constexpr int gap = 12;
        }

        /*
         * The tab strip's order, written down once and read by both
         * directions. Settings sits first because it is the page the gear
         * opens; Models is somewhere you go, or somewhere a missing model
         * sends you. Two mappings of the same order kept by hand is how an
         * index quietly inverts.
         */
        constexpr int settingsTab = 0;
        constexpr int modelsTab = 1;

        namespace rows
        {
            constexpr int height = 34;
            constexpr int headingHeight = 26;
            constexpr int gap = 2;
            constexpr int controlWidth = 300;
            constexpr int buttonWidth = 150;
            constexpr int buttonHeight = 22;

            /*
             * Every control ends at the same right edge, and no pill grows
             * past this. Left to fill the column, a two-option row drew pills
             * twice the width of a four-option row and the buttons sat 150px
             * short of both, which read as three different columns.
             */
            constexpr int maxPillWidth = 92;
        }

        juce::String bpm(double value)
        {
            return value > 0.0 ? juce::String(value, 1) : juce::String();
        }
    }

    // ------------------------------------------------------------- choice row

    namespace
    {
        /**
         * A label and N mutually exclusive pills on one line.
         *
         * SegmentedControl covers exactly two options and is used for the page
         * switcher above; the beat grid has four and the tempo reading three.
         * Deliberately not a popup: the point of this window is that the
         * settings are visible rather than hidden one click deeper, and a menu
         * inside a page that replaced a menu would be a joke at the reader's
         * expense.
         */
        class ChoiceRow final : public juce::Component
        {
        public:
            ChoiceRow(juce::String captionIn, juce::StringArray optionsIn)
                : caption(std::move(captionIn)), options(std::move(optionsIn))
            {
            }

            void setSelectedIndex(int index)
            {
                if (selected == index)
                    return;

                selected = index;
                repaint();
            }

            void setOptionLabels(const juce::StringArray& labels)
            {
                if (options == labels)
                    return;

                options = labels;
                repaint();
            }

            std::function<void(int)> onSelected;

            void paint(juce::Graphics& g) override
            {
                namespace colours = theme::colours;

                const auto live = isEnabled();

                auto bounds = getLocalBounds();
                bounds.removeFromRight(rows::controlWidth);

                g.setColour(live ? colours::text75() : colours::text45());
                g.setFont(juce::Font(theme::fonts::make(12.0f, false)));
                g.drawText(caption, bounds, juce::Justification::centredLeft, true);

                for (int index = 0; index < options.size(); ++index)
                {
                    auto cell = pillBounds(index)
                                    .withSizeKeepingCentre(pillWidth() - 4, rows::buttonHeight)
                                    .toFloat();

                    const auto chosen = index == selected;

                    if (chosen)
                    {
                        g.setColour(live ? colours::primaryFill()
                                         : colours::primaryFill().withAlpha(0.4f));
                        g.fillRoundedRectangle(cell, 6.0f);
                    }
                    else if (index == hovered && live)
                    {
                        g.setColour(colours::hoverFill());
                        g.fillRoundedRectangle(cell, 6.0f);
                    }

                    g.setColour(colours::outline());
                    g.drawRoundedRectangle(cell.reduced(0.5f), 6.0f, 1.0f);

                    g.setColour(!live      ? colours::text45()
                                : chosen   ? colours::primaryText()
                                           : colours::text75());

                    g.setFont(juce::Font(theme::fonts::make(11.0f, chosen)));
                    g.drawText(options[index], cell.toNearestInt(),
                               juce::Justification::centred, true);
                }
            }

            void mouseMove(const juce::MouseEvent& event) override
            {
                const auto index = indexAt(event.getPosition());

                if (index != hovered)
                {
                    hovered = index;
                    repaint();
                }
            }

            void mouseExit(const juce::MouseEvent&) override
            {
                hovered = -1;
                repaint();
            }

            void mouseUp(const juce::MouseEvent& event) override
            {
                if (!isEnabled())
                    return;

                const auto index = indexAt(event.getPosition());

                if (index >= 0 && onSelected)
                    onSelected(index);
            }

            void enablementChanged() override { repaint(); }

        private:
            int pillWidth() const
            {
                const auto count = juce::jmax(1, options.size());

                return juce::jmin(rows::controlWidth / count, rows::maxPillWidth);
            }

            /** The group is right-aligned, so every row ends in one line. */
            juce::Rectangle<int> pillBounds(int index) const
            {
                const auto width = pillWidth();
                const auto right = getWidth();
                const auto left = right - width * juce::jmax(1, options.size());

                return getLocalBounds().withX(left + index * width).withWidth(width);
            }

            int indexAt(juce::Point<int> point) const
            {
                for (int index = 0; index < options.size(); ++index)
                    if (pillBounds(index).contains(point))
                        return index;

                return -1;
            }

            juce::String caption;
            juce::StringArray options;
            int selected = -1;
            int hovered = -1;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChoiceRow)
        };

        /** A label and one button on the right, for a row that does something. */
        class ActionRow final : public juce::Component
        {
        public:
            ActionRow(juce::String captionIn, const juce::String& buttonText)
                : caption(std::move(captionIn)), button(buttonText)
            {
                addAndMakeVisible(button);
            }

            juce::TextButton& action() { return button; }

            void setCaption(juce::String text)
            {
                if (caption == text)
                    return;

                caption = std::move(text);
                repaint();
            }

            void paint(juce::Graphics& g) override
            {
                auto bounds = getLocalBounds();
                bounds.removeFromRight(rows::controlWidth);

                g.setColour(isEnabled() ? theme::colours::text75() : theme::colours::text45());
                g.setFont(juce::Font(theme::fonts::make(12.0f, false)));
                g.drawText(caption, bounds, juce::Justification::centredLeft, true);
            }

            void resized() override
            {
                button.setBounds(getLocalBounds()
                                     .removeFromRight(rows::buttonWidth)
                                     .withSizeKeepingCentre(rows::buttonWidth,
                                                            rows::buttonHeight));
            }

        private:
            juce::String caption;
            juce::TextButton button;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActionRow)
        };

        /** A section heading inside the scrolled page. */
        class HeadingRow final : public juce::Component
        {
        public:
            explicit HeadingRow(juce::String textIn) : text(std::move(textIn))
            {
                setInterceptsMouseClicks(false, false);
            }

            void paint(juce::Graphics& g) override
            {
                g.setColour(theme::colours::sectionHeader());
                g.setFont(juce::Font(theme::fonts::make(11.0f, true)));
                g.drawText(text, getLocalBounds().removeFromBottom(16),
                           juce::Justification::centredLeft);
            }

        private:
            juce::String text;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeadingRow)
        };
    }

    // ------------------------------------------------------- settings page

    class SettingsPanel::Preferences final : public juce::Component
    {
    public:
        Preferences()
        {
            listViewport.setViewedComponent(&content, false);
            listViewport.setScrollBarsShown(true, false);
            addAndMakeVisible(listViewport);

            auto add = [this](juce::Component& component)
            {
                content.addAndMakeVisible(component);
                order.push_back(&component);
            };

            add(audioHeading);
            add(audioSettings);

            add(gridHeading);
            add(gridMode);
            add(manualTempo);

            add(analysisHeading);
            add(analysisToggle);
            add(analysisQuality);
            add(tempoReading);
            add(forgetCorrection);
            add(clearCache);

            add(engineHeading);
            add(fusedNormalise);

            add(updatesHeading);
            add(checkUpdates);

            add(diagnosticsHeading);
            add(copyDiagnostics);
            add(abletonIntegration);

            gridMode.onSelected = [this](int index)
            {
                if (onGridMode)
                    onGridMode(index);
            };

            analysisQuality.onSelected = [this](int index)
            {
                if (onAnalysisQuality)
                    onAnalysisQuality(index);
            };

            tempoReading.onSelected = [this](int index)
            {
                if (onTempoInterpretation)
                    onTempoInterpretation(index);
            };

            fusedNormalise.onSelected = [this](int index)
            {
                if (onFusedNormalise)
                    onFusedNormalise(index == 1);
            };

            auto wire = [](ActionRow& row, std::function<void()>& callback)
            {
                row.action().onClick = [&callback]
                {
                    if (callback)
                        callback();
                };
            };

            wire(audioSettings, onAudioSettings);
            wire(manualTempo, onSetManualTempo);
            wire(analysisToggle, onAnalysisToggle);
            wire(forgetCorrection, onForgetCorrection);
            wire(clearCache, onClearAnalysisCache);
            wire(checkUpdates, onCheckUpdates);
            wire(copyDiagnostics, onCopyDiagnostics);
            wire(abletonIntegration, onAbletonIntegration);

            versionLabel.setFont(juce::Font(theme::fonts::make(11.0f, false)));
            versionLabel.setColour(juce::Label::textColourId, theme::colours::text45());
            versionLabel.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(versionLabel);
        }

        void apply(const Settings& settings)
        {
            // Audio settings belong to the Standalone; inside a host the DAW
            // owns the device, so the row would open a window that cannot
            // change anything.
            audioHeading.setVisible(settings.standalone);
            audioSettings.setVisible(settings.standalone);

            gridMode.setSelectedIndex(settings.gridMode);
            manualTempo.setCaption("Manual tempo (" + bpm(settings.manualBpm) + " BPM)");

            analysisToggle.action().setButtonText(settings.analysisRunning ? "Stop" : "Analyse");
            analysisToggle.setEnabled(settings.analysisToggleEnabled);
            analysisToggle.action().setEnabled(settings.analysisToggleEnabled);

            analysisQuality.setSelectedIndex(settings.analysisQuality);

            // The three readings carry the tempo each would give, so the
            // choice is made against numbers rather than against words.
            tempoReading.setOptionLabels(
                settings.tempoAvailable
                    ? juce::StringArray{"Half " + bpm(settings.halfBpm),
                                        "Detected " + bpm(settings.detectedBpm),
                                        "Double " + bpm(settings.doubleBpm)}
                    : juce::StringArray{"Half", "Detected", "Double"});

            tempoReading.setSelectedIndex(settings.tempoInterpretation);
            tempoReading.setEnabled(settings.tempoAvailable);

            forgetCorrection.setEnabled(settings.canForgetCorrection);
            forgetCorrection.action().setEnabled(settings.canForgetCorrection);

            // Greyed rather than hidden for the reason the menu gave: only the
            // hybrid engine fuses, but the setting still persists, and hiding
            // it would make it look lost.
            fusedNormalise.setSelectedIndex(settings.fusedNormalise ? 1 : 0);
            fusedNormalise.setEnabled(settings.fusedNormaliseAvailable);

            // Hidden rather than greyed, because there is nothing the user
            // could do to make it work: the updater is a file the bundle's
            // install.sh puts beside the app, so a build run from a checkout
            // has no updater and never will.
            updatesHeading.setVisible(settings.updaterAvailable);
            checkUpdates.setVisible(settings.updaterAvailable);

            checkUpdates.action().setButtonText(settings.updateCheckRunning
                                                    ? "Checking..."
                                                    : "Check...");
            checkUpdates.setEnabled(!settings.updateCheckRunning);
            checkUpdates.action().setEnabled(!settings.updateCheckRunning);

            copyDiagnostics.setEnabled(settings.hasDiagnostics);
            copyDiagnostics.action().setEnabled(settings.hasDiagnostics);

            abletonIntegration.setVisible(settings.abletonAvailable);

            versionLabel.setText("StemLab v" + settings.version, juce::dontSendNotification);

            resized();
        }

        void resized() override
        {
            auto bounds = getLocalBounds();

            versionLabel.setBounds(bounds.removeFromBottom(16));
            bounds.removeFromBottom(card::gap / 2);

            listViewport.setBounds(bounds);

            const auto width = listViewport.getWidth() - 10;

            int y = 0;

            for (auto* component : order)
            {
                if (!component->isVisible())
                    continue;

                const auto height = dynamic_cast<HeadingRow*>(component) != nullptr
                                        ? rows::headingHeight
                                        : rows::height;

                component->setBounds(0, y, width, height);
                y += height + rows::gap;
            }

            content.setSize(width, juce::jmax(y, listViewport.getHeight()));
        }

        std::function<void(int)> onGridMode;
        std::function<void()> onSetManualTempo;
        std::function<void()> onAnalysisToggle;
        std::function<void(int)> onAnalysisQuality;
        std::function<void(int)> onTempoInterpretation;
        std::function<void()> onForgetCorrection;
        std::function<void()> onClearAnalysisCache;
        std::function<void(bool)> onFusedNormalise;
        std::function<void()> onCheckUpdates;
        std::function<void()> onCopyDiagnostics;
        std::function<void()> onAudioSettings;
        std::function<void()> onAbletonIntegration;

    private:
        juce::Viewport listViewport;
        juce::Component content;
        std::vector<juce::Component*> order;

        HeadingRow audioHeading{"Audio"};
        ActionRow audioSettings{"Audio and MIDI devices", "Open..."};

        HeadingRow gridHeading{"Beat grid"};
        ChoiceRow gridMode{"Grid follows", {"Host", "Source", "Manual", "Off"}};
        ActionRow manualTempo{"Manual tempo", "Set..."};

        HeadingRow analysisHeading{"Source analysis"};
        ActionRow analysisToggle{"Key and BPM analysis", "Analyse"};
        ChoiceRow analysisQuality{"Analysis quality", {"Fast", "Accurate"}};
        ChoiceRow tempoReading{"Tempo reading", {"Half", "As detected", "Double"}};
        ActionRow forgetCorrection{"Saved correction for this source", "Forget"};
        ActionRow clearCache{"Analysis cache", "Clear"};

        HeadingRow engineHeading{"Separation"};
        ChoiceRow fusedNormalise{"Normalise fused stems", {"Off", "On"}};

        HeadingRow updatesHeading{"Updates"};
        ActionRow checkUpdates{"Check for a newer release", "Check..."};

        HeadingRow diagnosticsHeading{"Diagnostics"};
        ActionRow copyDiagnostics{"Engine log", "Copy"};
        ActionRow abletonIntegration{"Ableton Live integration", "Install / repair"};

        juce::Label versionLabel;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Preferences)
    };

    // ------------------------------------------------------------------ shell

    SettingsPanel::SettingsPanel()
    {
        setWantsKeyboardFocus(true);

        // Opaque would be wrong: the scrim is deliberately translucent so the
        // panel stays visible behind it and the overlay reads as covering the
        // interface rather than replacing it.
        setInterceptsMouseClicks(true, true);

        titleLabel.setText("StemLab", juce::dontSendNotification);
        titleLabel.setFont(juce::Font(theme::fonts::make(17.0f, true)));
        titleLabel.setColour(juce::Label::textColourId, theme::colours::text());
        addAndMakeVisible(titleLabel);

        tabs.onSelected = [this](int index)
        { showPage(index == modelsTab ? Page::models : Page::settings); };

        addAndMakeVisible(tabs);

        closeButton.onClick = [this]
        {
            if (onClose)
                onClose();
        };

        addAndMakeVisible(closeButton);

        settingsPage = std::make_unique<Preferences>();

        addChildComponent(modelsPage);
        addChildComponent(*settingsPage);

        auto forward = [](std::function<void()>& from, std::function<void()>& to)
        { from = [&to] { if (to) to(); }; };

        forward(settingsPage->onSetManualTempo, onSetManualTempo);
        forward(settingsPage->onAnalysisToggle, onAnalysisToggle);
        forward(settingsPage->onForgetCorrection, onForgetCorrection);
        forward(settingsPage->onClearAnalysisCache, onClearAnalysisCache);
        forward(settingsPage->onCheckUpdates, onCheckUpdates);
        forward(settingsPage->onCopyDiagnostics, onCopyDiagnostics);
        forward(settingsPage->onAudioSettings, onAudioSettings);
        forward(settingsPage->onAbletonIntegration, onAbletonIntegration);

        settingsPage->onGridMode = [this](int index)
        { if (onGridMode) onGridMode(index); };

        settingsPage->onAnalysisQuality = [this](int index)
        { if (onAnalysisQuality) onAnalysisQuality(index); };

        settingsPage->onTempoInterpretation = [this](int index)
        { if (onTempoInterpretation) onTempoInterpretation(index); };

        settingsPage->onFusedNormalise = [this](bool on)
        { if (onFusedNormalise) onFusedNormalise(on); };

        showPage(Page::settings);
    }

    SettingsPanel::~SettingsPanel() = default;

    juce::Rectangle<int> SettingsPanel::cardBounds() const
    {
        return getLocalBounds().withSizeKeepingCentre(
            juce::jmin(card::width, getWidth() - 40),
            juce::jmin(card::height, getHeight() - 40));
    }

    void SettingsPanel::setSettings(const Settings& settings)
    {
        settingsPage->apply(settings);
    }

    void SettingsPanel::showPage(Page requested)
    {
        page = requested;

        tabs.setSelectedIndex(page == Page::models ? modelsTab : settingsTab);

        modelsPage.setVisible(page == Page::models);
        settingsPage->setVisible(page == Page::settings);

        resized();
    }

    void SettingsPanel::paint(juce::Graphics& g)
    {
        namespace colours = theme::colours;

        // The scrim: dark enough that the card is unmistakably in front, not
        // so dark that the interface behind it disappears and the overlay
        // reads as a different screen.
        g.setColour(colours::ground().withAlpha(0.82f));
        g.fillAll();

        const auto bounds = cardBounds().toFloat();

        g.setColour(colours::surface());
        g.fillRoundedRectangle(bounds, card::radius);

        g.setColour(colours::outline());
        g.drawRoundedRectangle(bounds.reduced(0.5f), card::radius, 1.0f);
    }

    void SettingsPanel::resized()
    {
        auto inner = cardBounds().reduced(card::padX, card::padY);

        auto header = inner.removeFromTop(juce::jmax(card::titleHeight, card::tabsHeight));

        titleLabel.setBounds(header.removeFromLeft(160));

        // The switcher sits in the header rather than above the content: it
        // chooses what this window is showing, which is a property of the
        // window and not of either page.
        tabs.setBounds(header.removeFromLeft(card::tabsWidth)
                           .withSizeKeepingCentre(card::tabsWidth, card::tabsHeight));

        closeButton.setBounds(header.removeFromRight(82).withSizeKeepingCentre(82, 26));

        inner.removeFromTop(card::gap);

        modelsPage.setBounds(inner);

        if (settingsPage != nullptr)
            settingsPage->setBounds(inner);
    }

    bool SettingsPanel::keyPressed(const juce::KeyPress& key)
    {
        if (key != juce::KeyPress::escapeKey)
            return false;

        if (onClose)
            onClose();

        return true;
    }
}
