#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

#include "PluginProcessor.h"

namespace stemlab::widgets
{
    /**
     * The Models page: what is on disk, and what to do about it.
     *
     * One page inside SettingsPanel, which owns the card, the scrim, the tab
     * strip and Close. This draws only its own content, into whatever
     * rectangle it is given.
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

        /** The compile switch, and why it cannot help when it cannot. */
        void setCompileState(bool enabled, bool supported, const juce::String& reason);

        std::function<void(juce::StringArray)> onDownload;
        std::function<void(juce::StringArray)> onCompile;
        std::function<void(juce::StringArray, juce::StringArray)> onRemove;
        std::function<void(bool)> onCompileEnabled;
        std::function<void()> onCancel;

        void paint(juce::Graphics&) override;

        /** Fades the list where it scrolls past its edges. */
        void paintOverChildren(juce::Graphics&) override;

        void resized() override;

    private:
        /** One model or one cache, drawn as a row inside the list. */
        class Row;

        /** The house checkbox plus its caption, clickable as one. */
        class CompileSwitch;

        void rebuildRows();
        void layoutRows();

        std::vector<StemLabAudioProcessor::ManagedModel> models;
        std::vector<StemLabAudioProcessor::ManagedCache> caches;

        /*
         * What the rows were last built from. The editor refreshes this panel
         * on every UI tick, and rebuilding rows there would destroy and
         * recreate every button several times a second - wasteful, and able
         * to pull a button out from under a click that had already started.
         */
        juce::String inventoryDigest;

        juce::Label summaryLabel;
        std::unique_ptr<CompileSwitch> compileSwitch;
        juce::Label activityLabel;
        juce::Label activityPercent;
        juce::Label unavailableLabel;

        juce::Viewport listViewport;
        juce::Component listContent;
        std::vector<std::unique_ptr<Row>> rows;

        juce::TextButton downloadAllButton{"Download all"};
        juce::TextButton cancelButton{"Cancel"};
        juce::TextButton closeButton{"Close"};

        juce::String unavailableReason;

        /*
         * Declared before the bar that binds to it: juce::ProgressBar holds a
         * reference, and a member initialised from one declared later would
         * bind to storage that has not been constructed.
         */
        double activityProgress = 0.0;
        juce::ProgressBar activityBar{activityProgress};

        bool jobRunning = false;

        /*
         * Laid out once in resized() and read by paint(). The two used to
         * derive the same geometry independently from the same constants,
         * which is how the progress track ended up a few pixels off the
         * divider it was supposed to sit clear of.
         */
        juce::Rectangle<int> listArea;
        int headerRuleY = 0;
        int footerRuleY = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelManagerPanel)
    };
}
