#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ModelManagerPanel.h"
#include "StemLabWidgets.h"

namespace stemlab::widgets
{
    /**
     * Everything that used to be behind the gear, as one modal with two pages.
     *
     * A child of the panel rather than a window of its own. The panel is laid
     * out at a fixed design size and scaled as a whole, so sitting inside it
     * means this inherits that transform and needs no sizing rules; it is also
     * the only shape that behaves the same in the Standalone and inside a
     * host, where a real modal window would be an unwelcome guest.
     *
     * Modal is enforced by covering the panel and swallowing clicks, not by a
     * JUCE modal loop: a loop would block the message thread that a running
     * job's progress arrives on.
     *
     * The shell owns the scrim, the card, the page switcher, Close and the
     * Escape key. Each page owns only its own content, and is handed the
     * rectangle left over.
     */
    class SettingsPanel final : public juce::Component
    {
    public:
        enum class Page
        {
            models,
            settings
        };

        /** Everything the settings page draws, filled by the editor.
         *
         * A snapshot rather than a processor reference, for the reason the
         * widgets header gives: nothing in here talks to the processor, so
         * every one of these can be driven from a test or a screenshot rig.
         */
        struct Settings
        {
            bool standalone = false;

            int gridMode = 0;
            bool hostTempoAvailable = false;
            double manualBpm = 120.0;

            bool analysisRunning = false;
            bool analysisToggleEnabled = true;
            int analysisQuality = 0;

            int tempoInterpretation = 0;
            bool tempoAvailable = false;
            double halfBpm = 0.0;
            double detectedBpm = 0.0;
            double doubleBpm = 0.0;
            bool canForgetCorrection = false;

            bool fusedNormalise = false;
            bool fusedNormaliseAvailable = false;

            bool updaterAvailable = false;
            bool updateCheckRunning = false;

            bool hasDiagnostics = false;
            bool abletonAvailable = false;
            juce::String version;
        };

        SettingsPanel();
        ~SettingsPanel() override;

        /** The models page, wired by the editor exactly as before. */
        ModelManagerPanel& models() { return modelsPage; }

        void setSettings(const Settings& settings);

        void showPage(Page page);
        Page currentPage() const noexcept { return page; }

        std::function<void()> onClose;

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

        void paint(juce::Graphics&) override;
        void resized() override;

        /** Escape closes, matching every other dismissable surface. */
        bool keyPressed(const juce::KeyPress&) override;

        /** The scrim eats clicks so the panel behind cannot be operated. */
        void mouseUp(const juce::MouseEvent&) override {}

    private:
        /** The settings page: rows of controls, no popups anywhere. */
        class Preferences;

        juce::Rectangle<int> cardBounds() const;

        Page page = Page::settings;

        juce::Label titleLabel;
        SegmentedControl tabs{"Models", "Settings"};
        juce::TextButton closeButton{"Close"};

        ModelManagerPanel modelsPage;
        std::unique_ptr<Preferences> settingsPage;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsPanel)
    };
}
