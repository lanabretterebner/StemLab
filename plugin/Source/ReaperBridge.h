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

    Both return a message instead of throwing; the processor turns that into
    a status line.
*/
namespace stemlab::reaper
{
    struct SourceItem
    {
        bool ok = false;
        juce::String message;

        juce::File file;
        juce::String label;              // "Track / Take" for the UI

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

    struct StemToInsert
    {
        juce::String name;               // "vocals", "drums", ...
        juce::File file;
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
    };

    struct InsertResult
    {
        int inserted = 0;
        juce::String message;
    };

    /** Creates the stem tracks in one undo block. Message thread only. */
    InsertResult insertStemTracks (
        const Api& api,
        const juce::Array<StemToInsert>& stems,
        const InsertAnchor& anchor,
        const juce::String& sourceLabel);
}
