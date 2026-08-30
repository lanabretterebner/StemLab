#include "ModelManagerPanel.h"

#include "StemLabTheme.h"
#include "StemLabWidgets.h"

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
            constexpr int summaryHeight = 18;
            constexpr int compileHeight = 22;
            constexpr int footerHeight = 30;
            constexpr int activityHeight = 16;
            constexpr int barHeight = 6;
            constexpr int gap = 12;

            /*
             * The bottom strip, measured rather than guessed. Laid out with
             * equal gaps, the rule and the progress track read as one double
             * line 7px apart; the track has to sit closer to the caption it
             * belongs with than to the rule it is separated by.
             */
            constexpr int ruleGap = 8;    // list edge -> rule
            constexpr int barGap = 14;    // rule -> track
            constexpr int barTextGap = 4; // track -> caption
            constexpr int footerGap = 8;  // caption -> buttons

            /** How far the list fades out at an edge it can scroll past. */
            constexpr int fade = 14;
        }

        namespace row
        {
            constexpr int modelHeight = 42;
            constexpr int cacheHeight = 32;
            constexpr int headerHeight = 24;
            constexpr int actionWidth = 74;
            constexpr int actionHeight = 22;
            constexpr int gap = 8;
            constexpr float radius = 8.0f;
        }

        /**
         * The separator this app uses between facts on one line.
         *
         * Through fromUTF8 deliberately: JUCE's char* String constructor
         * mangles a non-ASCII literal into mojibake.
         */
        juce::String dot()
        {
            return juce::String::fromUTF8(" \xc2\xb7 ");
        }

        /** Sizes for people, not for machines: no 0.0 B, no 1024 MB. */
        juce::String describeBytes(juce::int64 bytes)
        {
            if (bytes <= 0)
                return {};

            static constexpr const char* units[] = {"B", "KB", "MB", "GB"};

            auto value = static_cast<double>(bytes);
            int unit = 0;

            while (value >= 1024.0 && unit < 3)
            {
                value /= 1024.0;
                ++unit;
            }

            return juce::String(value, unit == 0 ? 0 : 1) + " " + units[unit];
        }
    }

    // -------------------------------------------------------- compile switch

    /**
     * The compile opt-in: the house checkbox with its caption beside it.
     *
     * A component rather than a juce::ToggleButton because the stock one
     * draws its own pale tick box, which is the one control in this card that
     * looked like it came from a different program. Clicking the caption
     * toggles too - a 15px target for a switch nobody uses often is mean.
     *
     * Why it cannot help, when it cannot, lives in the tooltip. It used to be
     * a line of grey text underneath, which pushed the entire list down by a
     * row whenever it appeared.
     */
    class ModelManagerPanel::CompileSwitch final : public juce::Component,
                                                  public juce::SettableTooltipClient
    {
    public:
        CompileSwitch()
        {
            box.setClickingTogglesState(true);
            box.onClick = [this]
            {
                if (onToggle)
                    onToggle(box.getToggleState());
            };

            addAndMakeVisible(box);
        }

        void setState(bool enabled, bool supported)
        {
            // dontSendNotification: this is the processor telling the switch
            // what is true, and firing onClick would send it straight back.
            if (box.getToggleState() != enabled)
                box.setToggleState(enabled, juce::dontSendNotification);

            if (usable != supported)
            {
                usable = supported;
                repaint();
            }
        }

        bool isOn() const { return box.getToggleState(); }

        void paint(juce::Graphics& g) override
        {
            auto text = getLocalBounds();
            text.removeFromLeft(boxWidth + 8);

            // Dimmed rather than disabled: a machine that cannot compile
            // today may be able to after installing a compiler, and a
            // control you cannot touch gives nowhere to hang the reason.
            g.setColour(usable ? theme::colours::text75() : theme::colours::text45());
            g.setFont(juce::Font(theme::fonts::make(12.0f, false)));
            g.drawText("Compile separations", text, juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            box.setBounds(getLocalBounds().removeFromLeft(boxWidth));
        }

        void mouseUp(const juce::MouseEvent& event) override
        {
            if (getLocalBounds().contains(event.getPosition()))
                box.triggerClick();
        }

        std::function<void(bool)> onToggle;

    private:
        static constexpr int boxWidth = 18;

        IncludeCheckbox box;
        bool usable = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompileSwitch)
    };

    // ------------------------------------------------------------------ row

    /**
     * One line of the list: a model, a cache, or a section heading.
     *
     * The three share a class because they share a shape - a label, a
     * secondary line, a size on the right and at most one button - and
     * splitting them would mean three near-identical paint methods.
     */
    class ModelManagerPanel::Row final : public juce::Component,
                                        public juce::SettableTooltipClient
    {
    public:
        enum class Kind
        {
            heading,
            model,
            cache
        };

        Row(Kind kindIn, juce::String idIn) : kind(kindIn), id(std::move(idIn))
        {
            if (kind == Kind::heading)
            {
                setInterceptsMouseClicks(false, false);
                return;
            }

            addAndMakeVisible(action);
            action.onClick = [this]
            {
                if (onAction)
                    onAction(id);
            };

            addChildComponent(secondary);
            secondary.onClick = [this]
            {
                if (onSecondary)
                    onSecondary(id);
            };
        }

        void configureModel(const StemLabAudioProcessor::ManagedModel& model)
        {
            title = model.label;
            detail = model.purpose;
            present = model.present;

            size = model.present ? describeBytes(model.bytes) : describeBytes(model.approxBytes);

            // A missing model shows what it will cost, which reads very
            // differently from what an installed one occupies, so the size is
            // prefixed rather than left to look like it is already there.
            if (!model.present && size.isNotEmpty())
                size = "needs " + size;

            action.setButtonText(model.present ? "Remove" : "Get");
            action.setEnabled(true);

            const auto canCompile = model.compilable && model.present;

            secondary.setButtonText(model.compiled ? "Compiled" : "Compile");
            secondary.setVisible(canCompile);
            secondary.setEnabled(canCompile && !model.compiled);

            // Why a model is not compilable is engine trivia - "Beat This! is
            // not among the patched models" told a user nothing they wanted,
            // in the same breath as what the model is for. The absent Compile
            // button already says everything the row needs to.
            setTooltip(model.present ? model.path : model.compileReason);
        }

        void configureCache(const StemLabAudioProcessor::ManagedCache& cache)
        {
            title = cache.label;
            detail = cache.warning;
            size = describeBytes(cache.bytes);
            present = cache.bytes > 0;

            action.setButtonText("Clear");
            action.setEnabled(cache.bytes > 0);
            secondary.setVisible(false);

            setTooltip(cache.path);
        }

        void configureHeading(juce::String text) { title = std::move(text); }

        void setBusy(bool busy)
        {
            action.setEnabled(!busy && (kind != Kind::cache || present));
            secondary.setEnabled(!busy && secondary.isVisible()
                                 && secondary.getButtonText() != "Compiled");
        }

        void paint(juce::Graphics& g) override
        {
            namespace colours = theme::colours;

            auto bounds = getLocalBounds();

            if (kind == Kind::heading)
            {
                g.setColour(colours::sectionHeader());
                g.setFont(juce::Font(theme::fonts::make(11.0f, true)));
                g.drawText(title, bounds.removeFromBottom(16), juce::Justification::centredLeft);
                return;
            }

            g.setColour(colours::rowHoverFill());
            g.fillRoundedRectangle(bounds.toFloat(), row::radius);

            auto text = bounds.reduced(10, 0);
            text.removeFromRight(row::actionWidth + row::gap);

            if (secondary.isVisible())
                text.removeFromRight(row::actionWidth + row::gap);

            const auto sizeWidth = size.isNotEmpty() ? 78 : 0;
            auto sizeArea = text.removeFromRight(sizeWidth);

            g.setColour(present ? colours::text75() : colours::text45());
            g.setFont(juce::Font(theme::fonts::make(12.5f, true)));

            if (detail.isEmpty())
            {
                g.drawText(title, text, juce::Justification::centredLeft, true);
            }
            else
            {
                // The pair is centred as one block rather than each line in
                // half the row, which pushed them to opposite edges and made
                // a caption look like an unrelated line.
                constexpr int titleLine = 16;
                constexpr int detailLine = 14;

                auto block = text.withSizeKeepingCentre(text.getWidth(), titleLine + detailLine);

                g.drawText(title, block.removeFromTop(titleLine),
                           juce::Justification::centredLeft, true);

                g.setColour(colours::text45());
                g.setFont(juce::Font(theme::fonts::make(10.5f, false)));
                g.drawText(detail, block, juce::Justification::centredLeft, true);
            }

            if (size.isNotEmpty())
            {
                g.setColour(colours::text45());
                g.setFont(juce::Font(theme::fonts::make(10.5f, false)));
                g.drawText(size, sizeArea, juce::Justification::centredRight, true);
            }
        }

        void resized() override
        {
            if (kind == Kind::heading)
                return;

            auto bounds = getLocalBounds().reduced(10, 0);

            auto place = [&bounds](juce::Component& target)
            {
                auto slot = bounds.removeFromRight(row::actionWidth);
                target.setBounds(
                    slot.withSizeKeepingCentre(row::actionWidth, row::actionHeight));
                bounds.removeFromRight(row::gap);
            };

            place(action);

            if (secondary.isVisible())
                place(secondary);
        }

        std::function<void(const juce::String&)> onAction;
        std::function<void(const juce::String&)> onSecondary;

        Kind kind;
        juce::String id;

    private:
        juce::TextButton action;
        juce::TextButton secondary;

        juce::String title;
        juce::String detail;
        juce::String size;
        bool present = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Row)
    };

    // ---------------------------------------------------------------- panel

    ModelManagerPanel::ModelManagerPanel()
    {
        namespace colours = theme::colours;

        setWantsKeyboardFocus(true);

        // Opaque would be wrong: the scrim is deliberately translucent so the
        // panel stays visible behind it and the overlay reads as covering the
        // interface rather than replacing it.
        setInterceptsMouseClicks(true, true);

        titleLabel.setText("Model Manager", juce::dontSendNotification);
        titleLabel.setFont(juce::Font(theme::fonts::make(17.0f, true)));
        titleLabel.setColour(juce::Label::textColourId, colours::text());
        addAndMakeVisible(titleLabel);

        summaryLabel.setFont(juce::Font(theme::fonts::make(11.5f, false)));
        summaryLabel.setColour(juce::Label::textColourId, colours::text50());
        addAndMakeVisible(summaryLabel);

        compileSwitch = std::make_unique<CompileSwitch>();
        compileSwitch->onToggle = [this](bool on)
        {
            if (onCompileEnabled)
                onCompileEnabled(on);
        };
        addAndMakeVisible(*compileSwitch);

        activityLabel.setFont(juce::Font(theme::fonts::make(11.5f, false)));
        activityLabel.setColour(juce::Label::textColourId, colours::text50());
        addAndMakeVisible(activityLabel);

        // The percentage the bar cannot say. Right-aligned against the same
        // edge as the track so the two read as one readout.
        activityPercent.setFont(juce::Font(theme::fonts::make(11.5f, false)));
        activityPercent.setColour(juce::Label::textColourId, colours::text50());
        activityPercent.setJustificationType(juce::Justification::centredRight);
        addChildComponent(activityPercent);

        // The house progress bar, through the LookAndFeel every other bar in
        // the app goes through. This card used to draw its own track, which
        // is how it ended up a few pixels off the divider above it.
        activityBar.setPercentageDisplay(false);
        activityBar.setColour(juce::ProgressBar::backgroundColourId,
                              juce::Colours::transparentBlack);
        addChildComponent(activityBar);

        unavailableLabel.setFont(juce::Font(theme::fonts::make(12.0f, false)));
        unavailableLabel.setColour(juce::Label::textColourId, colours::text50());
        unavailableLabel.setJustificationType(juce::Justification::centred);
        addChildComponent(unavailableLabel);

        listViewport.setViewedComponent(&listContent, false);
        listViewport.setScrollBarsShown(true, false);
        addAndMakeVisible(listViewport);

        downloadAllButton.onClick = [this]
        {
            juce::StringArray missing;

            for (const auto& model : models)
                if (!model.present)
                    missing.add(model.id);

            if (onDownload && !missing.isEmpty())
                onDownload(missing);
        };

        cancelButton.onClick = [this]
        {
            if (onCancel)
                onCancel();
        };

        closeButton.onClick = [this]
        {
            if (onClose)
                onClose();
        };

        addAndMakeVisible(downloadAllButton);
        addChildComponent(cancelButton);
        addAndMakeVisible(closeButton);
    }

    ModelManagerPanel::~ModelManagerPanel() = default;

    juce::Rectangle<int> ModelManagerPanel::cardBounds() const
    {
        return getLocalBounds().withSizeKeepingCentre(
            juce::jmin(card::width, getWidth() - 40),
            juce::jmin(card::height, getHeight() - 40));
    }

    void ModelManagerPanel::setInventory(
        const std::vector<StemLabAudioProcessor::ManagedModel>& modelsIn,
        const std::vector<StemLabAudioProcessor::ManagedCache>& cachesIn)
    {
        // Everything a row draws goes into the digest, so a change the user
        // could see rebuilds and a change they could not does nothing.
        juce::String digest;

        for (const auto& model : modelsIn)
            digest << model.id << (model.present ? '1' : '0') << (model.compiled ? '1' : '0')
                   << (model.compilable ? '1' : '0') << model.bytes << ';';

        for (const auto& cache : cachesIn)
            digest << cache.id << cache.bytes << ';';

        const auto changed = digest != inventoryDigest || !unavailableReason.isEmpty();

        inventoryDigest = digest;
        models = modelsIn;
        caches = cachesIn;
        unavailableReason.clear();

        if (!changed)
        {
            unavailableLabel.setVisible(false);
            listViewport.setVisible(true);
            return;
        }

        int installed = 0;
        juce::int64 onDisk = 0;
        juce::int64 toFetch = 0;

        for (const auto& model : models)
        {
            if (model.present)
            {
                ++installed;
                onDisk += model.bytes;
            }
            else
            {
                toFetch += model.approxBytes;
            }
        }

        for (const auto& cache : caches)
            onDisk += cache.bytes;

        juce::String summary;
        summary << installed << " of " << static_cast<int>(models.size()) << " models installed";

        if (onDisk > 0)
            summary << dot() << describeBytes(onDisk) << " on disk";

        if (toFetch > 0)
            summary << dot() << describeBytes(toFetch) << " to fetch";

        summaryLabel.setText(summary, juce::dontSendNotification);

        downloadAllButton.setVisible(installed < static_cast<int>(models.size()));

        rebuildRows();
        resized();
    }

    void ModelManagerPanel::setCompileState(bool enabled, bool supported,
                                            const juce::String& reason)
    {
        compileSwitch->setState(enabled, supported);

        // The reason lives here rather than on a line of its own. It is worth
        // having - an unset opt-in and a missing compiler need opposite
        // advice - but not worth a paragraph under a checkbox that moved the
        // whole list down whenever the answer changed.
        compileSwitch->setTooltip(supported
                                      ? juce::String("Compile the separation models on this "
                                                     "machine. The first run is slower; every "
                                                     "run after it is faster.")
                                      : reason);
    }

    void ModelManagerPanel::setUnavailable(const juce::String& reason)
    {
        unavailableReason = reason;

        models.clear();
        caches.clear();
        rows.clear();
        inventoryDigest.clear();

        summaryLabel.setText({}, juce::dontSendNotification);
        unavailableLabel.setText(reason, juce::dontSendNotification);
        downloadAllButton.setVisible(false);

        resized();
    }

    void ModelManagerPanel::setActivity(const juce::String& message, double progress, bool busy)
    {
        activityLabel.setText(message, juce::dontSendNotification);
        activityProgress = juce::jlimit(0.0, 1.0, progress);

        activityPercent.setText(juce::String(juce::roundToInt(activityProgress * 100.0)) + "%",
                                juce::dontSendNotification);

        if (busy != jobRunning)
        {
            jobRunning = busy;

            // Every action is refused while one runs: the engine takes one
            // job at a time, and a second click would only be rejected deeper
            // down where the user cannot see why.
            downloadAllButton.setEnabled(!busy);
            cancelButton.setVisible(busy);
            activityBar.setVisible(busy);
            activityPercent.setVisible(busy);

            for (auto& entry : rows)
                entry->setBusy(busy);

            resized();
        }

        activityBar.repaint();
    }

    void ModelManagerPanel::rebuildRows()
    {
        rows.clear();

        auto add = [this](std::unique_ptr<Row> entry)
        {
            listContent.addAndMakeVisible(*entry);
            rows.push_back(std::move(entry));
        };

        auto heading = std::make_unique<Row>(Row::Kind::heading, juce::String{});
        heading->configureHeading("Models");
        add(std::move(heading));

        for (const auto& model : models)
        {
            auto entry = std::make_unique<Row>(Row::Kind::model, model.id);
            entry->configureModel(model);

            entry->onAction = [this](const juce::String& id)
            {
                const auto found = std::find_if(models.begin(), models.end(),
                                                [&id](const auto& m) { return m.id == id; });

                if (found == models.end())
                    return;

                if (found->present)
                {
                    if (onRemove)
                        onRemove({id}, {});
                }
                else if (onDownload)
                {
                    onDownload({id});
                }
            };

            entry->onSecondary = [this](const juce::String& id)
            {
                if (onCompile)
                    onCompile({id});
            };

            add(std::move(entry));
        }

        auto cacheHeading = std::make_unique<Row>(Row::Kind::heading, juce::String{});
        cacheHeading->configureHeading("Caches");
        add(std::move(cacheHeading));

        for (const auto& cache : caches)
        {
            auto entry = std::make_unique<Row>(Row::Kind::cache, cache.id);
            entry->configureCache(cache);

            entry->onAction = [this](const juce::String& id)
            {
                if (onRemove)
                    onRemove({}, {id});
            };

            add(std::move(entry));
        }

        for (auto& entry : rows)
            entry->setBusy(jobRunning);
    }

    void ModelManagerPanel::layoutRows()
    {
        int y = 0;

        for (auto& entry : rows)
        {
            const auto height = entry->kind == Row::Kind::heading  ? row::headerHeight
                                : entry->kind == Row::Kind::model ? row::modelHeight
                                                                  : row::cacheHeight;

            entry->setBounds(0, y, listViewport.getWidth() - 10, height);
            y += height;
        }

        // A little air past the last row, so scrolled to the bottom the list
        // ends on a gap rather than on a row cut off against the divider.
        listContent.setSize(listViewport.getWidth() - 10,
                            juce::jmax(y + 6, listViewport.getHeight()));
    }

    void ModelManagerPanel::paint(juce::Graphics& g)
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

        // Both rules come from what resized() actually laid out, rather than
        // from a second reading of the same constants.
        g.setColour(colours::divider());
        g.fillRect(listArea.getX(), headerRuleY, listArea.getWidth(), 1);
        g.fillRect(listArea.getX(), footerRuleY, listArea.getWidth(), 1);
    }

    void ModelManagerPanel::paintOverChildren(juce::Graphics& g)
    {
        if (listArea.isEmpty() || !listViewport.isVisible())
            return;

        // A row cut in half against a rule looks like a bug. Fading the last
        // few pixels into the card says "there is more below" in the one
        // place the list can be scrolled past, and costs nothing when it
        // cannot: both edges are drawn only where there is content beyond.
        const auto surface = theme::colours::surface();

        auto wash = [&g, &surface](juce::Rectangle<int> area, bool downwards)
        {
            juce::ColourGradient gradient(surface, 0.0f,
                                          static_cast<float>(downwards ? area.getBottom()
                                                                       : area.getY()),
                                          surface.withAlpha(0.0f), 0.0f,
                                          static_cast<float>(downwards ? area.getY()
                                                                       : area.getBottom()),
                                          false);

            g.setGradientFill(gradient);
            g.fillRect(area);
        };

        const auto scroll = listViewport.getViewPositionY();
        const auto hidden = listContent.getHeight() - listViewport.getHeight();

        if (scroll > 0)
            wash(listArea.withHeight(card::fade), false);

        if (scroll < hidden)
            wash(listArea.withTop(listArea.getBottom() - card::fade), true);
    }

    void ModelManagerPanel::resized()
    {
        auto inner = cardBounds().reduced(card::padX, card::padY);

        titleLabel.setBounds(inner.removeFromTop(card::titleHeight));
        summaryLabel.setBounds(inner.removeFromTop(card::summaryHeight));

        inner.removeFromTop(card::gap / 2);
        compileSwitch->setBounds(inner.removeFromTop(card::compileHeight));

        inner.removeFromTop(card::gap);
        headerRuleY = inner.getY() - card::gap / 2;

        auto footer = inner.removeFromBottom(card::footerHeight);

        // Reserved whether or not a job is running: a strip that appeared
        // only while working would shift the list under the pointer at the
        // moment the user clicked something in it.
        inner.removeFromBottom(card::footerGap);
        auto activity = inner.removeFromBottom(card::activityHeight);
        inner.removeFromBottom(card::barTextGap);
        auto bar = inner.removeFromBottom(card::barHeight);

        activityBar.setBounds(bar);

        // The percentage takes the right end of the same line as the message.
        auto percentArea = activity.removeFromRight(48);
        activityPercent.setBounds(percentArea);
        activityLabel.setBounds(activity);

        inner.removeFromBottom(card::barGap + card::ruleGap);
        footerRuleY = inner.getBottom() + card::ruleGap;

        listArea = inner;
        listViewport.setBounds(inner);
        unavailableLabel.setBounds(inner);
        unavailableLabel.setVisible(unavailableReason.isNotEmpty());
        listViewport.setVisible(unavailableReason.isEmpty());

        // Right to left, so Close keeps the corner whatever else is showing.
        auto place = [&footer](juce::Component& target, int width)
        {
            if (!target.isVisible())
                return;

            target.setBounds(footer.removeFromRight(width).reduced(0, 2));
            footer.removeFromRight(row::gap);
        };

        place(closeButton, 82);
        place(cancelButton, 82);
        place(downloadAllButton, 110);

        layoutRows();
    }

    bool ModelManagerPanel::keyPressed(const juce::KeyPress& key)
    {
        if (key != juce::KeyPress::escapeKey)
            return false;

        if (onClose)
            onClose();

        return true;
    }
}
