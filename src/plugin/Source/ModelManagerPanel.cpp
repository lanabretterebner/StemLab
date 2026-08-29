#include "ModelManagerPanel.h"

#include "StemLabTheme.h"

namespace stemlab::widgets
{
    namespace
    {
        namespace card
        {
            constexpr int width = 700;
            constexpr int height = 470;
            constexpr float radius = 12.0f;
            constexpr int padX = 22;
            constexpr int padY = 18;

            constexpr int titleHeight = 24;
            constexpr int summaryHeight = 16;
            constexpr int compileHeight = 16;
            constexpr int toggleHeight = 22;
            constexpr int footerHeight = 30;
            constexpr int activityHeight = 18;
            constexpr int gap = 12;
        }

        namespace row
        {
            constexpr int modelHeight = 42;
            constexpr int cacheHeight = 30;
            constexpr int headerHeight = 24;
            constexpr int actionWidth = 74;
            constexpr int actionHeight = 22;
            constexpr int gap = 8;
            constexpr float radius = 8.0f;
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

            // Shown after the purpose rather than instead of it: why a model
            // cannot be compiled matters less than what the model is for, and
            // the absent Compile button has already said the first part.
            note = model.compilable || !model.present ? juce::String{} : model.compileReason;

            setTooltip(model.present ? model.path : juce::String{});
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

            auto second = detail;

            if (note.isNotEmpty())
                second = second.isEmpty() ? note : second + "  -  " + note;

            if (second.isEmpty())
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
                g.drawText(second, block, juce::Justification::centredLeft, true);
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
        juce::String note;
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

        compileLabel.setFont(juce::Font(theme::fonts::make(11.0f, false)));
        compileLabel.setColour(juce::Label::textColourId, colours::text45());
        addChildComponent(compileLabel);

        compileToggle.setColour(juce::ToggleButton::textColourId, colours::text75());
        compileToggle.setColour(juce::ToggleButton::tickColourId, colours::accent());
        compileToggle.setColour(juce::ToggleButton::tickDisabledColourId, colours::outline());

        compileToggle.onClick = [this]
        {
            if (onCompileEnabled)
                onCompileEnabled(compileToggle.getToggleState());
        };

        addAndMakeVisible(compileToggle);

        activityLabel.setFont(juce::Font(theme::fonts::make(11.5f, false)));
        activityLabel.setColour(juce::Label::textColourId, colours::text50());
        addAndMakeVisible(activityLabel);

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
                   << (model.compilable ? '1' : '0') << model.bytes << model.compileReason << ';';

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
            summary << "  -  " << describeBytes(onDisk) << " on disk";

        if (toFetch > 0)
            summary << "  -  " << describeBytes(toFetch) << " to fetch";

        summaryLabel.setText(summary, juce::dontSendNotification);

        downloadAllButton.setVisible(installed < static_cast<int>(models.size()));

        rebuildRows();
        resized();
    }

    void ModelManagerPanel::setCompileState(bool enabled, bool supported,
                                            const juce::String& reason)
    {
        // dontSendNotification: this is the processor telling the panel what
        // is true, and firing onClick from here would send it straight back.
        if (compileToggle.getToggleState() != enabled)
            compileToggle.setToggleState(enabled, juce::dontSendNotification);

        // The note explains a switch that is on but cannot work - no toolchain,
        // no Triton. With it off, the unticked box already says everything, and
        // a line repeating it would be noise.
        const auto note = enabled && !supported ? reason : juce::String{};

        compileLabel.setText(note, juce::dontSendNotification);

        if (compileLabel.isVisible() == note.isNotEmpty())
            return;

        compileLabel.setVisible(note.isNotEmpty());
        resized();
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

        if (busy != jobRunning)
        {
            jobRunning = busy;

            // Every action is refused while one runs: the engine takes one
            // job at a time, and a second click would only be rejected deeper
            // down where the user cannot see why.
            downloadAllButton.setEnabled(!busy);
            cancelButton.setVisible(busy);

            for (auto& entry : rows)
                entry->setBusy(busy);

            resized();
        }

        repaint();
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

        listContent.setSize(listViewport.getWidth() - 10, juce::jmax(y, listViewport.getHeight()));
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

        // A divider under the header and above the footer, so the list reads
        // as its own region rather than as text floating in a box.
        const auto inner = cardBounds().reduced(card::padX, card::padY);
        const auto listTop = inner.getY() + card::titleHeight + card::summaryHeight
                             + card::toggleHeight
                             + (compileLabel.isVisible() ? card::compileHeight : 0) + card::gap;
        const auto listBottom =
            inner.getBottom() - card::footerHeight - card::activityHeight - card::gap;

        g.setColour(colours::divider());
        g.fillRect(inner.getX(), listTop - card::gap / 2, inner.getWidth(), 1);
        g.fillRect(inner.getX(), listBottom + card::gap / 2, inner.getWidth(), 1);

        if (!jobRunning || activityProgress <= 0.0)
            return;

        // The progress track sits under the activity line, only while a job
        // runs: a permanent empty track would suggest something is pending.
        auto track = juce::Rectangle<int>(inner.getX(), listBottom + card::gap + 2,
                                          inner.getWidth(), 3)
                         .toFloat();

        g.setColour(colours::outline());
        g.fillRoundedRectangle(track, 1.5f);

        g.setColour(colours::accent());
        g.fillRoundedRectangle(
            track.withWidth(static_cast<float>(track.getWidth() * activityProgress)), 1.5f);
    }

    void ModelManagerPanel::resized()
    {
        auto inner = cardBounds().reduced(card::padX, card::padY);

        titleLabel.setBounds(inner.removeFromTop(card::titleHeight));
        summaryLabel.setBounds(inner.removeFromTop(card::summaryHeight));

        compileToggle.setBounds(inner.removeFromTop(card::toggleHeight));

        if (compileLabel.isVisible())
            compileLabel.setBounds(inner.removeFromTop(card::compileHeight));

        inner.removeFromTop(card::gap);

        auto footer = inner.removeFromBottom(card::footerHeight);
        auto activity = inner.removeFromBottom(card::activityHeight);

        activityLabel.setBounds(activity);
        inner.removeFromBottom(card::gap);

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
