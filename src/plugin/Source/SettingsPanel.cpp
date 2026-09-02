#include "SettingsPanel.h"

#include "StemLabTheme.h"

#include <array>

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
        constexpr int creditsTab = 2;

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

        /*
         * Every row's caption is drawn the same way: the label fills what
         * the control column leaves, and greys when the row cannot be
         * operated. Written once because three copies of the same four
         * lines is how one row ends up a shade or a pixel away from the
         * other two.
         */
        void paintCaption(juce::Graphics& g, const juce::Component& row,
                          const juce::String& caption)
        {
            auto bounds = row.getLocalBounds();
            bounds.removeFromRight(rows::controlWidth);

            g.setColour(row.isEnabled() ? theme::colors::text75()
                                        : theme::colors::text45());
            g.setFont(juce::Font(theme::fonts::make(12.0f, false)));
            g.drawText(caption, bounds, juce::Justification::centredLeft, true);
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

            void setCaption(juce::String text)
            {
                if (caption == text)
                    return;

                caption = std::move(text);
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
                namespace colors = theme::colors;

                const auto live = isEnabled();

                paintCaption(g, *this, caption);

                for (int index = 0; index < options.size(); ++index)
                {
                    auto cell = pillBounds(index)
                                    .withSizeKeepingCentre(pillWidth() - 4, rows::buttonHeight)
                                    .toFloat();

                    const auto chosen = index == selected;

                    if (chosen)
                    {
                        g.setColour(live ? colors::primaryFill()
                                         : colors::primaryFill().withAlpha(0.4f));
                        g.fillRoundedRectangle(cell, 6.0f);
                    }
                    else if (index == hovered && live)
                    {
                        g.setColour(colors::hoverFill());
                        g.fillRoundedRectangle(cell, 6.0f);
                    }

                    g.setColour(colors::outline());
                    g.drawRoundedRectangle(cell.reduced(0.5f), 6.0f, 1.0f);

                    g.setColour(!live      ? colors::text45()
                                : chosen   ? colors::primaryText()
                                           : colors::text75());

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

        /**
         * A label and one filled circle per accent, the chosen one ringed.
         *
         * Not a ChoiceRow of named pills: eight names do not fit the control
         * column without shrinking past the point of being readable, and a
         * color is the one setting where the swatch says more than its name
         * anyway. The name is still there as the tooltip, for anyone reading
         * the interface rather than looking at it.
         */
        class SwatchRow final : public juce::Component,
                                public juce::TooltipClient
        {
        public:
            explicit SwatchRow(juce::String captionIn) : caption(std::move(captionIn))
            {
                setMouseCursor(juce::MouseCursor::PointingHandCursor);
            }

            void setSelectedIndex(int index)
            {
                if (selected == index)
                    return;

                selected = index;
                repaint();
            }

            std::function<void(int)> onSelected;

            void paint(juce::Graphics& g) override
            {
                namespace colors = theme::colors;

                paintCaption(g, *this, caption);

                for (int index = 0; index < theme::accents::count(); ++index)
                {
                    const auto cell = swatchBounds(index).toFloat();
                    const auto dot = cell.withSizeKeepingCentre(diameter, diameter);

                    g.setColour(theme::accents::swatch(index));
                    g.fillEllipse(dot);

                    /*  The ring is drawn outside the fill rather than over it,
                        so the swatch a person is judging is the whole circle
                        and not a circle with a line through its edge.
                    */
                    if (index == selected)
                    {
                        g.setColour(colors::text());
                        g.drawEllipse(dot.expanded(3.0f), 1.5f);
                    }
                    else if (index == hovered)
                    {
                        g.setColour(colors::text50());
                        g.drawEllipse(dot.expanded(3.0f), 1.0f);
                    }
                }
            }

            void mouseMove(const juce::MouseEvent& event) override
            {
                const auto index = indexAt(event.getPosition());

                if (index == hovered)
                    return;

                hovered = index;
                repaint();
            }

            /** The name of whichever swatch the pointer is over. */
            juce::String getTooltip() override
            {
                return hovered >= 0 ? theme::accents::name(hovered) : juce::String();
            }

            void mouseExit(const juce::MouseEvent&) override
            {
                hovered = -1;
                repaint();
            }

            void mouseUp(const juce::MouseEvent& event) override
            {
                const auto index = indexAt(event.getPosition());

                if (index >= 0 && onSelected)
                    onSelected(index);
            }

        private:
            static constexpr float diameter = 16.0f;

            int swatchWidth() const
            {
                return rows::controlWidth / juce::jmax(1, theme::accents::count());
            }

            /** Right-aligned, so this row ends where every other row does. */
            juce::Rectangle<int> swatchBounds(int index) const
            {
                const auto width = swatchWidth();
                const auto left = getWidth() - width * theme::accents::count();

                return getLocalBounds().withX(left + index * width).withWidth(width);
            }

            int indexAt(juce::Point<int> point) const
            {
                for (int index = 0; index < theme::accents::count(); ++index)
                    if (swatchBounds(index).contains(point))
                        return index;

                return -1;
            }

            juce::String caption;
            int selected = 0;
            int hovered = -1;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwatchRow)
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

            void paint(juce::Graphics& g) override { paintCaption(g, *this, caption); }

            /*  The caption greys with the row, so it has to be redrawn when
                the row is enabled or disabled. Without this a row switched
                off mid-session - Copy while there is no engine log, Check
                while an update check runs - kept a live-looking label above
                a dead button until something else forced a repaint.
            */
            void enablementChanged() override { repaint(); }

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
                g.setColour(theme::colors::sectionHeader());
                g.setFont(juce::Font(theme::fonts::make(11.0f, true)));
                g.drawText(text, getLocalBounds().removeFromBottom(16),
                           juce::Justification::centredLeft);
            }

        private:
            juce::String text;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeadingRow)
        };
    }

    // -------------------------------------------------------- credits page

    /**
     * Who else's work is in here.
     *
     * The same list docs/third-party.md carries, in the app, because that is
     * where the obligations actually land: the OFL asks that its licence
     * travel with the font, and a file in a source repository is not
     * something a person who installed a bundle will ever open. It is one
     * scrolling column of headings and prose rather than rows of controls,
     * because there is nothing here to operate.
     *
     * Deliberately not generated from the markdown. The doc is the legal
     * record and says more than a panel should - build configuration
     * caveats, redistribution obligations - and rendering markdown to fit a
     * 700px card would be a parser nobody asked for. These are the same
     * facts, said shorter, and the doc is named at the end for the rest.
     */
    class SettingsPanel::Credits final : public juce::Component
    {
    public:
        Credits()
        {
            listViewport.setViewedComponent(&content, false);
            listViewport.setScrollBarsShown(true, false);
            addAndMakeVisible(listViewport);
        }

        /*  Guarded like every other setter on this page: the editor pushes
            the whole settings snapshot on every UI tick, and an
            unconditional repaint re-measured all seven paragraphs twenty
            times a second for as long as the page was open.
        */
        void setVersion(const juce::String& text)
        {
            if (version == text)
                return;

            version = text;
            repaint();
        }

        void resized() override
        {
            listViewport.setBounds(getLocalBounds());

            /*  The height is measured at the width the column is about to
                get, not at the width it still has. Reading the width off
                the column itself measured the prose against a component
                that had not been sized yet - zero on the first pass, so
                bodyWidth's 120px floor - which wrapped every paragraph far
                narrower than it draws and left a tail of empty scroll
                below the last line for the lifetime of the panel.
            */
            const auto width = juce::jmax(0, listViewport.getWidth() - 12);

            content.setSize(width, contentHeight(width));
        }

        void paint(juce::Graphics&) override {}

    private:
        struct Entry
        {
            const char* heading;
            const char* body;
        };

        /*  Kept in step with docs/third-party.md by hand. There are seven of
            them and they change when a dependency does, which is not often
            and never quietly.
        */
        static constexpr std::array<Entry, 7> entries{{
            {"JUCE",
             "The Standalone app and the VST3 are built with JUCE 9, under its "
             "own dual licence (AGPLv3 or commercial). The VST3 SDK it includes "
             "carries separate terms."},
            {"BS-RoFormer",
             "Separation can run bs-roformer-infer - OpenMIRLab, lucidrains' "
             "BS-RoFormer, ZFTurbo's Music-Source-Separation-Training. "
             "Checkpoints are downloaded, not shipped, and carry their own "
             "terms."},
            {"Demucs",
             "Separation can run the upstream demucs package and the "
             "htdemucs_6s model, which keep their upstream licensing."},
            {"Beat This!",
             "Beat and downbeat analysis uses Beat This! 1.1.0, MIT licensed. "
             "Its small0 and final0 checkpoints are downloaded on first use "
             "and checked against a recorded SHA-256."},
            {"Inter",
             "The interface is set in Inter, copyright The Inter Project "
             "Authors, under the SIL Open Font License 1.1. The full licence "
             "ships beside the font files as OFL.txt."},
            {"Python and the ML runtime",
             "CPython, PyTorch, NumPy, SciPy, SoundFile, librosa, Mido, "
             "PyYAML, tqdm, audio-separator and their dependencies, each under "
             "its own upstream licence."},
            {"FFmpeg",
             "Used for MP3, OGG and AIFF input. Its licensing depends on how "
             "the binary on your machine was built."},
        }};

        static constexpr int headingHeight = 22;
        static constexpr int paragraphGap = 14;

        /*  The wrapping width, taken from the column rather than from the
            component that owns it: measuring and drawing have to agree, and
            only the column knows which width it was given.
        */
        static int bodyWidth(int columnWidth) { return juce::jmax(120, columnWidth - 4); }

        /** The wrapped height of one entry's prose, measured not guessed. */
        static int bodyHeight(const char* text, int columnWidth)
        {
            juce::AttributedString wrapped;
            wrapped.append(text, juce::Font(theme::fonts::make(12.0f, false)));

            juce::TextLayout layout;
            layout.createLayout(wrapped, static_cast<float>(bodyWidth(columnWidth)));

            return juce::roundToInt(layout.getHeight());
        }

        static int contentHeight(int columnWidth)
        {
            auto total = 0;

            for (const auto& entry : entries)
                total += headingHeight + bodyHeight(entry.body, columnWidth) + paragraphGap;

            return total + 40;
        }

        /** The scrolled column. A child so the viewport has something to
            move; everything is drawn here rather than as sub-components,
            since none of it is interactive. */
        class Column final : public juce::Component
        {
        public:
            explicit Column(Credits& ownerIn) : owner(ownerIn) {}

            void paint(juce::Graphics& g) override { owner.paintColumn(g, *this); }

        private:
            Credits& owner;
        };

        void paintColumn(juce::Graphics& g, juce::Component& column)
        {
            auto area = column.getLocalBounds();

            for (const auto& entry : entries)
            {
                g.setColour(theme::colors::sectionHeader());
                g.setFont(juce::Font(theme::fonts::make(11.0f, true)));
                g.drawText(entry.heading, area.removeFromTop(headingHeight),
                           juce::Justification::centredLeft, false);

                juce::AttributedString wrapped;
                wrapped.append(entry.body, juce::Font(theme::fonts::make(12.0f, false)),
                               theme::colors::text75());

                const auto height = bodyHeight(entry.body, column.getWidth());

                wrapped.draw(g, area.removeFromTop(height).toFloat());
                area.removeFromTop(paragraphGap);
            }

            g.setColour(theme::colors::text45());
            g.setFont(juce::Font(theme::fonts::make(11.0f, false)));
            g.drawText("StemLab " + version + " - full terms in docs/third-party.md",
                       area.removeFromTop(20), juce::Justification::centredLeft, false);
        }

        juce::Viewport listViewport;
        Column content{*this};
        juce::String version;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Credits)
    };

    // ------------------------------------------------------- settings page

    class SettingsPanel::Preferences final : public juce::Component
    {
    public:
        /*  The page calls the panel's own callbacks rather than declaring a
            second set for the shell to relay into. The two sets had to be
            kept in step by hand - fourteen declarations, fourteen forwarding
            lambdas - and a page callback the shell forgot to forward is a
            control that silently does nothing.
        */
        explicit Preferences(SettingsPanel& ownerIn) : owner(ownerIn)
        {
            listViewport.setViewedComponent(&content, false);
            listViewport.setScrollBarsShown(true, false);
            addAndMakeVisible(listViewport);

            auto add = [this](juce::Component& component)
            {
                content.addAndMakeVisible(component);
                order.push_back(&component);
            };

            add(appearanceHeading);
            add(accentRow);
            add(waveformPalette);

            add(audioHeading);
            add(audioSettings);

            add(gridHeading);
            add(gridMode);
            add(manualTempo);
            add(loopQuantize);

            add(analysisHeading);
            add(analysisQuality);
            add(tempoMode);
            add(tempoReading);
            add(clearCache);

            add(engineHeading);
            add(fusedNormalise);

            add(updatesHeading);
            add(checkUpdates);

            add(diagnosticsHeading);
            add(copyDiagnostics);
            add(abletonIntegration);

            accentRow.onSelected = [this](int index)
            {
                if (owner.onAccent)
                    owner.onAccent(index);
            };

            waveformPalette.onSelected = [this](int index)
            {
                if (owner.onWaveformPalette)
                    owner.onWaveformPalette(index);
            };

            gridMode.onSelected = [this](int index)
            {
                if (owner.onGridMode)
                    owner.onGridMode(index);
            };

            loopQuantize.onSelected = [this](int index)
            {
                if (owner.onLoopQuantize)
                    owner.onLoopQuantize(index);
            };

            analysisQuality.onSelected = [this](int index)
            {
                if (owner.onAnalysisQuality)
                    owner.onAnalysisQuality(index);
            };

            tempoMode.onSelected = [this](int index)
            {
                if (owner.onTempoMode)
                    owner.onTempoMode(index);
            };

            tempoReading.onSelected = [this](int index)
            {
                if (owner.onTempoInterpretation)
                    owner.onTempoInterpretation(index);
            };

            fusedNormalise.onSelected = [this](int index)
            {
                if (owner.onFusedNormalise)
                    owner.onFusedNormalise(index == 1);
            };

            auto wire = [](ActionRow& row, std::function<void()>& callback)
            {
                row.action().onClick = [&callback]
                {
                    if (callback)
                        callback();
                };
            };

            wire(audioSettings, owner.onAudioSettings);
            wire(manualTempo, owner.onSetManualTempo);
            wire(clearCache, owner.onClearAnalysisCache);
            wire(checkUpdates, owner.onCheckUpdates);
            wire(copyDiagnostics, owner.onCopyDiagnostics);
            wire(abletonIntegration, owner.onAbletonIntegration);

            versionLabel.setFont(juce::Font(theme::fonts::make(11.0f, false)));
            versionLabel.setColour(juce::Label::textColourId, theme::colors::text45());
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

            accentRow.setSelectedIndex(settings.accent);
            waveformPalette.setSelectedIndex(settings.waveformPalette);

            gridMode.setSelectedIndex(settings.gridMode);
            manualTempo.setCaption("Manual tempo (" + bpm(settings.manualBpm) + " BPM)");

            loopQuantize.setSelectedIndex(settings.loopQuantize);

            /*  Greyed when there is no rule to snap to - the grid switched
                off, or a source with no tempo behind it. Left visible, and
                left showing what it is set to: the setting is remembered,
                and hiding it would make a loop that suddenly stops snapping
                look like a bug rather than a missing grid.
            */
            loopQuantize.setEnabled(settings.loopQuantizeAvailable);

            analysisQuality.setSelectedIndex(settings.analysisQuality);

            // Static reads one tempo for the whole track, which is what a
            // host's tempo field takes. Dynamic names each stretch that holds
            // its own - the same analysis either way, so switching costs
            // nothing and never re-runs it.
            tempoMode.setSelectedIndex(settings.tempoMode);

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

            // A reading is only worth a host's tempo field if one tempo holds
            // the whole track. When it does not, the number is still the best
            // single answer and saying so is the useful thing - the grid will
            // walk away from a played or drifting track however it is set.
            tempoReading.setCaption(
                settings.tempoSections.isNotEmpty()
                    ? "Tempo reading (" + settings.tempoSections + ")"
                    : (settings.tempoAvailable && !settings.tempoSteady
                           ? "Tempo reading (varies across the track)"
                           : "Tempo reading"));


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

    private:
        SettingsPanel& owner;

        juce::Viewport listViewport;
        juce::Component content;
        std::vector<juce::Component*> order;

        HeadingRow appearanceHeading{"Appearance"};
        SwatchRow accentRow{"Accent color"};

        /*  The order the palette menu in the header used, kept: Spectrum
            first because it is the default, and the two audio-driven ones
            beside it. The editor maps these positions onto the theme's
            indices, which are a different order.
        */
        ChoiceRow waveformPalette{"Waveform",
                                  {"Spectrum", "RGB", "3-Band", "Stem", "Accent"}};

        HeadingRow audioHeading{"Audio"};
        ActionRow audioSettings{"Audio and MIDI devices", "Open..."};

        HeadingRow gridHeading{"Beat grid"};
        ChoiceRow gridMode{"Grid follows", {"Host", "Source", "Manual", "Off"}};
        ActionRow manualTempo{"Manual tempo", "Set..."};

        /*  Beats, then a bar - the order they grow in, so the row reads as a
            scale rather than a list. Off leads because it is the default and
            because it is what the other four are measured against.
        */
        ChoiceRow loopQuantize{"Loop quantise", {"Off", "1/4", "1/2", "1", "Bar"}};

        HeadingRow analysisHeading{"Source analysis"};
        ChoiceRow analysisQuality{"Analysis quality", {"Fast", "Accurate"}};
        ChoiceRow tempoMode{"Tempo analysis", {"Static", "Dynamic"}};
        ChoiceRow tempoReading{"Tempo reading", {"Half", "As detected", "Double"}};
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

        /*  The scrim needs no mouse setup of its own: a component covering
            the panel already hit-tests every click inside it, and a click
            that lands on the scrim stops at a mouseUp that does nothing.
            Both of those are what juce::Component does by default, so
            saying them again here only invites the reader to believe they
            were switched on for some reason.
        */

        titleLabel.setText("StemLab", juce::dontSendNotification);
        titleLabel.setFont(juce::Font(theme::fonts::make(17.0f, true)));
        titleLabel.setColour(juce::Label::textColourId, theme::colors::text());
        addAndMakeVisible(titleLabel);

        tabs.onSelected = [this](int index)
        {
            showPage(index == modelsTab    ? Page::models
                     : index == creditsTab ? Page::credits
                                           : Page::settings);
        };

        addAndMakeVisible(tabs);

        closeButton.onClick = [this]
        {
            if (onClose)
                onClose();
        };

        addAndMakeVisible(closeButton);

        settingsPage = std::make_unique<Preferences>(*this);
        creditsPage = std::make_unique<Credits>();

        addChildComponent(modelsPage);
        addChildComponent(*settingsPage);
        addChildComponent(*creditsPage);

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
        creditsPage->setVersion(settings.version);
    }

    void SettingsPanel::showPage(Page requested)
    {
        page = requested;

        tabs.setSelectedIndex(page == Page::models    ? modelsTab
                              : page == Page::credits ? creditsTab
                                                      : settingsTab);

        modelsPage.setVisible(page == Page::models);
        settingsPage->setVisible(page == Page::settings);
        creditsPage->setVisible(page == Page::credits);

        resized();
    }

    void SettingsPanel::paint(juce::Graphics& g)
    {
        namespace colors = theme::colors;

        // The scrim: dark enough that the card is unmistakably in front, not
        // so dark that the interface behind it disappears and the overlay
        // reads as a different screen.
        g.setColour(colors::ground().withAlpha(0.82f));
        g.fillAll();

        const auto bounds = cardBounds().toFloat();

        g.setColour(colors::surface());
        g.fillRoundedRectangle(bounds, card::radius);

        g.setColour(colors::outline());
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

        if (creditsPage != nullptr)
            creditsPage->setBounds(inner);
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
