#pragma once

#include "ReaperHost.h"

/*
    The two REAPER workflows StemLab needs, as pure message-thread functions
    over a resolved Api:

      querySelectedItem   "Use Selected Item" - which file, where on the
                          timeline, and how the take plays it back.

      insertStemTracks    "Insert Stems" - one new track per stem, placed
                          directly under the source (or this plugin's) track,
                          each item aligned with the original selection.
                          Stems that were split further become REAPER folder
                          tracks holding their sub-stems, and the source item
                          is muted so the project plays the stems instead of
                          the original.

      PeakBuilder         REAPER only builds peaks for media it imported
                          itself, so freshly written stems draw as empty
                          lanes until their .reapeaks exists. This walks the
                          new files on the message thread, a slice per timer
                          tick, and refreshes the arrangement when it is done.

    Both functions return a message instead of throwing; the processor turns
    that into a status line.
*/
namespace stemlab::reaper
{
    struct SourceItem
    {
        bool ok = false;
        juce::String message;

        juce::File file;
        juce::String label;              // "Track / Take" for the UI

        MediaItem* item = nullptr;       // the item itself, for muting later

        double startSeconds = 0.0;       // item position
        double startQN = 0.0;            // same position in quarter notes
        double lengthSeconds = 0.0;      // item length
        double startOffsetSeconds = 0.0; // take D_STARTOFFS
        double playRate = 1.0;           // take D_PLAYRATE
        bool preservePitch = true;       // take B_PPITCH

        int trackNumber = 0;             // 1-based source track, 0 unknown
    };

    /** Reads the first selected arrangement item. Message thread only. */
    SourceItem querySelectedItem (const Api& api);

    /*  One track to create, in the order the tracks should appear.

        The stem tree is flattened before it gets here: the processor knows
        which nodes have children and which the user selected, and turns
        that into a plain list plus REAPER's own folder-depth encoding, so
        this file only has to create tracks and items in order.
    */
    struct StemToInsert
    {
        juce::String name;               // track/take name, already pretty
        juce::String colorStem;         // identity-palette key ("vocals"...)
        juce::File file;                 // empty = folder track with no item

        /*  A stem that was split further is still inserted, so the user can
            unmute it and A/B the group against its parts - but it arrives
            muted, because its children already carry the same audio.
        */
        bool muted = false;

        /*  REAPER's I_FOLDERDEPTH: 1 opens a folder on this track, 0 keeps
            the current level, -n closes n folders after it.
        */
        int folderDepth = 0;
    };

    /*  How inserted items are placed. When the job came from
        querySelectedItem this echoes that item's geometry, so the stems
        line up with the exact region the user selected - including a
        trimmed take's start offset and play rate. When the source was a
        dropped file there is no geometry to echo: lengthSeconds <= 0 makes
        each item as long as its own stem audio.
    */
    struct InsertAnchor
    {
        double startSeconds = 0.0;
        double lengthSeconds = 0.0;
        double startOffsetSeconds = 0.0;
        double playRate = 1.0;
        bool preservePitch = true;

        // 1-based track to insert below; 0 = below this plugin's own track,
        // falling back to the end of the track list.
        int afterTrackNumber = 0;

        /*  The arrangement item the stems were separated from. Muted as
            part of the insert so the project does not play the original
            and its stems on top of each other. Only ever dereferenced
            after REAPER confirms the pointer is still live.
        */
        MediaItem* sourceItem = nullptr;
    };

    struct InsertResult
    {
        int inserted = 0;
        juce::String message;

        // The audio actually placed in the project, for the peak pass.
        juce::Array<juce::File> insertedFiles;

        bool mutedSourceItem = false;
    };

    /** Creates the stem tracks in one undo block. Message thread only. */
    InsertResult insertStemTracks (
        const Api& api,
        const juce::Array<StemToInsert>& stems,
        const InsertAnchor& anchor);

    /**
     * Builds REAPER's peak cache for files it has not seen before.
     *
     * Message thread only, like everything else here: one slice per timer
     * tick so a long stem never freezes the UI, and its own PCM_source per
     * file so nothing depends on an item the user may delete meanwhile.
     * Destroying the builder abandons the work safely.
     *
     * A file whose ".reapeaks" sidecar is already current is skipped, so
     * the insert / undo / re-insert cycle a user retries placement with
     * does not rebuild every peak from scratch each time.
     */
    class PeakBuilder final : private juce::Timer
    {
    public:
        PeakBuilder (const Api& api, const juce::Array<juce::File>& files);
        ~PeakBuilder() override;

    private:
        void timerCallback() override;
        bool startNextFile();
        void closeCurrentSource();

        /** Ends the run: stops the timer and asks REAPER to redraw the
            arrangement, which is what turns the placed items' empty lanes
            into waveforms. */
        void finish();

        const Api& api;
        juce::Array<juce::File> pending;
        PCM_source* current = nullptr;
        int slicesOnCurrent = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PeakBuilder)
    };
}
