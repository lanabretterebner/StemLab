#pragma once

#include <JuceHeader.h>
#include <memory>

namespace Steinberg
{
    class FUnknown;
}

/*
    Runtime access to REAPER's extension API from inside the VST3.

    REAPER (v5.02+) exposes IReaperHostApplication through the host context it
    passes to a VST3 during initialisation. Querying that interface hands the
    plugin getReaperApi(), a by-name resolver over the whole ReaScript C API,
    and getReaperParent(), which identifies the track this plugin instance
    lives on. No Remote Script, no sockets, no installer step.

    Every function pointer here is resolved once when the host context
    arrives. Anything REAPER does not provide stays null, and isValid() gates
    the whole feature so an old REAPER degrades to the local-file workflow
    instead of crashing.

    THREADING: the REAPER API is main-thread only. Call these pointers from
    the message thread - never from the audio thread or an engine thread.
*/
namespace stemlab::reaper
{
    // Opaque handles. The real definitions live inside REAPER; the plugin
    // only ever passes the pointers back.
    struct ReaProject;
    struct MediaTrack;
    struct MediaItem;
    struct MediaItem_Take;
    struct PCM_source;

    struct Api final
    {
        /*  Queries hostContext for IReaperHostApplication and resolves the
            API. Returns nullptr when the host is not REAPER (or predates the
            interface). Safe to call with a null context.
        */
        static std::unique_ptr<Api> tryCreate (
            Steinberg::FUnknown* hostContext);

        ~Api();

        /** True when every function the bridge depends on resolved. */
        bool isValid() const noexcept { return valid; }

        /** Names the bridge needed but this REAPER did not provide. */
        juce::StringArray getMissingFunctionNames() const;

        /** e.g. "7.42" - empty when GetAppVersion is unavailable. */
        juce::String getAppVersion() const;

        /** The MediaTrack this plugin instance sits on, or nullptr. */
        MediaTrack* getOwnTrack() const;

        // Selected-item query --------------------------------------------
        int (*CountSelectedMediaItems) (ReaProject*) = nullptr;
        MediaItem* (*GetSelectedMediaItem) (ReaProject*, int) = nullptr;
        MediaItem_Take* (*GetActiveTake) (MediaItem*) = nullptr;
        PCM_source* (*GetMediaItemTake_Source) (MediaItem_Take*) = nullptr;
        void (*GetMediaSourceFileName) (PCM_source*, char*, int) = nullptr;
        double (*GetMediaSourceLength) (PCM_source*, bool*) = nullptr;
        MediaTrack* (*GetMediaItem_Track) (MediaItem*) = nullptr;
        double (*GetMediaItemInfo_Value) (MediaItem*, const char*) = nullptr;
        double (*GetMediaItemTakeInfo_Value) (
            MediaItem_Take*, const char*) = nullptr;

        // Timeline conversion --------------------------------------------
        double (*TimeMap2_timeToQN) (ReaProject*, double) = nullptr;
        double (*TimeMap2_QNToTime) (ReaProject*, double) = nullptr;

        // Track + item creation ------------------------------------------
        void (*InsertTrackAtIndex) (int, bool) = nullptr;
        int (*CountTracks) (ReaProject*) = nullptr;
        MediaTrack* (*GetTrack) (ReaProject*, int) = nullptr;
        double (*GetMediaTrackInfo_Value) (
            MediaTrack*, const char*) = nullptr;
        bool (*SetMediaTrackInfo_Value) (
            MediaTrack*, const char*, double) = nullptr;
        bool (*GetSetMediaTrackInfo_String) (
            MediaTrack*, const char*, char*, bool) = nullptr;
        bool (*GetSetMediaItemTakeInfo_String) (
            MediaItem_Take*, const char*, char*, bool) = nullptr;
        PCM_source* (*PCM_Source_CreateFromFile) (const char*) = nullptr;
        MediaItem* (*AddMediaItemToTrack) (MediaTrack*) = nullptr;
        MediaItem_Take* (*AddTakeToMediaItem) (MediaItem*) = nullptr;
        bool (*SetMediaItemTake_Source) (
            MediaItem_Take*, PCM_source*) = nullptr;
        bool (*SetMediaItemPosition) (MediaItem*, double, bool) = nullptr;
        bool (*SetMediaItemLength) (MediaItem*, double, bool) = nullptr;
        bool (*SetMediaItemTakeInfo_Value) (
            MediaItem_Take*, const char*, double) = nullptr;
        bool (*SetMediaItemInfo_Value) (
            MediaItem*, const char*, double) = nullptr;

        /*  Project tempo. Optional, like the peak builder: an older REAPER
            without them simply cannot be told a tempo, and Set Host Tempo
            reports that rather than failing the whole bridge.

            SetTempoTimeSigMarker with ptidx -1 inserts; timepos or
            measurepos/beatpos, whichever is not -1, places it. Passing
            bpm 0 and both time signature fields 0 deletes.
        */
        bool (*SetCurrentBPM) (ReaProject*, double, bool) = nullptr;
        bool (*SetTempoTimeSigMarker) (
            ReaProject*, int, double, int, double, double, int, int, bool) = nullptr;
        int (*CountTempoTimeSigMarkers) (ReaProject*) = nullptr;
        bool (*DeleteTempoTimeSigMarker) (ReaProject*, int) = nullptr;

        // Housekeeping ----------------------------------------------------
        void (*Undo_BeginBlock2) (ReaProject*) = nullptr;
        void (*Undo_EndBlock2) (ReaProject*, const char*, int) = nullptr;
        void (*UpdateArrange) () = nullptr;
        void (*TrackList_AdjustWindows) (bool) = nullptr;

        // Optional - the bridge works without these.
        int (*ColorToNative) (int, int, int) = nullptr;
        void (*PCM_Source_Destroy) (PCM_source*) = nullptr;
        const char* (*GetAppVersion) () = nullptr;
        bool (*TakeIsMIDI) (MediaItem_Take*) = nullptr;

        /*  Muting the source item needs a pointer REAPER still knows about:
            the user may have deleted or re-recorded it between Use Selected
            Item and Insert Stems. Without ValidatePtr2 the mute is skipped
            rather than risking a stale pointer.
        */
        bool (*ValidatePtr2) (ReaProject*, void*, const char*) = nullptr;

        /*  Peak building for files REAPER has never seen. Items created
            through PCM_Source_CreateFromFile do not get the peak pass that
            REAPER's own import runs, so a freshly written stem draws as an
            empty lane until something asks for its .reapeaks.

            mode 0 starts a build, 1 runs a slice (non-zero = call again),
            2 finishes.
        */
        int (*PCM_Source_BuildPeaks) (PCM_source*, int) = nullptr;

    private:
        Api() = default;

        // The addRef'd IReaperHostApplication, stored as its base type so
        // this header stays free of VST3 SDK includes. The .cpp casts it
        // back to the derived interface. Released in ~Api().
        Steinberg::FUnknown* reaperHost = nullptr;

        bool valid = false;
        juce::StringArray missingFunctions;

        JUCE_DECLARE_NON_COPYABLE (Api)
    };
}
