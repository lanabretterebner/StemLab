#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

#include "PluginProcessor.h"

namespace stemlab::widgets
{
    /**
     * The Model Manager: what is on disk, and what to do about it.
     *
     * A child of the panel rather than a window of its own. The panel is laid
     * out at a fixed design size and scaled as a whole, so sitting inside it
     * means this inherits that transform and needs no sizing rules; it is also
     * the only shape that behaves the same in the Standalone and inside a
     * host, where a real modal window would be an unwelcome guest.
     *
     * Modal is enforced by covering the panel and swallowing clicks, not by a
     * JUCE modal loop: a loop would block the message thread that the running
     * job's progress arrives on.
     */
    class ModelManagerPanel final : public juce::Component
    {
    public:
        ModelManagerPanel();
        ~ModelManagerPanel() override;

        /** Replace what is shown. Cheap enough to call on every refresh. */
        void setInventory(const std::vector<StemLabAudioProcessor::ManagedModel>& models,
                          const std::vector<StemLabAudioProcessor::ManagedCache>& caches);

        /** The line above the buttons, and whether a job is running. */
        void setActivity(const juce::String& message, double progress, bool busy);

        /** Shown in place of the list when the engine could not be asked. */
        void setUnavailable(const juce::String& reason);

        /** The compile switch and a note about it when it is not usable. */
        void setCompileState(bool enabled, bool supported, const juce::String& reason);

        std::function<void(juce::StringArray)> onDownload;
        std::function<void(juce::StringArray)> onCompile;
        std::function<void(juce::StringArray, juce::StringArray)> onRemove;
        std::function<void(bool)> onCompileEnabled;
        std::function<void()> onCancel;
        std::function<void()> onClose;

        void paint(juce::Graphics&) override;
        void resized() override;

        /** Escape closes, matching every other dismissable surface. */
        bool keyPressed(const juce::KeyPress&) override;

        /** The scrim eats clicks so the panel behind cannot be operated. */
        void mouseUp(const juce::MouseEvent&) override {}

    private:
        /** One model or one cache, drawn as a row inside the list. */
        class Row;

        void rebuildRows();
        void layoutRows();
        juce::Rectangle<int> cardBounds() const;

        std::vector<StemLabAudioProcessor::ManagedModel> models;
        std::vector<StemLabAudioProcessor::ManagedCache> caches;

        /*
         * What the rows were last built from. The editor refreshes this panel
         * on every UI tick, and rebuilding rows there would destroy and
         * recreate every button several times a second - wasteful, and able
         * to pull a button out from under a click that had already started.
         */
        juce::String inventoryDigest;

        juce::Label titleLabel;
        juce::Label summaryLabel;
        juce::Label compileLabel;
        juce::ToggleButton compileToggle{"Compile separations"};
        juce::Label activityLabel;
        juce::Label unavailableLabel;

        juce::Viewport listViewport;
        juce::Component listContent;
        std::vector<std::unique_ptr<Row>> rows;

        juce::TextButton downloadAllButton{"Download all"};
        juce::TextButton cancelButton{"Cancel"};
        juce::TextButton closeButton{"Close"};

        juce::String unavailableReason;
        double activityProgress = 0.0;
        bool jobRunning = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelManagerPanel)
    };
}
