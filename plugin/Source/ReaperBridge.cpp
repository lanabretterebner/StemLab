#include "ReaperBridge.h"

namespace stemlab::reaper
{
namespace
{
    /*  GetSet*Info_String takes no capacity argument on a get - REAPER
        copies the whole value into the caller's pointer. Names are typed by
        users and have no documented cap, so reads go through a heap buffer
        far past anything a project can plausibly hold rather than a small
        stack array that a long name would smash.
    */
    constexpr size_t nameBufferBytes = 32768;

    juce::String readTrackName (const Api& api, MediaTrack* track)
    {
        if (track == nullptr)
            return {};

        juce::HeapBlock<char> buffer (nameBufferBytes, true);

        if (! api.GetSetMediaTrackInfo_String (
                track,
                "P_NAME",
                buffer.getData(),
                false))
        {
            return {};
        }

        buffer[nameBufferBytes - 1] = 0;
        return juce::String::fromUTF8 (buffer.getData()).trim();
    }

    juce::String readTakeName (const Api& api, MediaItem_Take* take)
    {
        juce::HeapBlock<char> buffer (nameBufferBytes, true);

        if (! api.GetSetMediaItemTakeInfo_String (
                take,
                "P_NAME",
                buffer.getData(),
                false))
        {
            return {};
        }

        buffer[nameBufferBytes - 1] = 0;
        return juce::String::fromUTF8 (buffer.getData()).trim();
    }

    juce::String prettyStemName (const juce::String& stem)
    {
        if (stem.isEmpty())
            return "Stem";

        return stem.substring (0, 1).toUpperCase()
            + stem.substring (1).toLowerCase();
    }

    // Same palette the Ableton Remote Script uses, so a user moving between
    // hosts sees the same stem identity.
    bool stemColour (const juce::String& stem, int& r, int& g, int& b)
    {
        struct Entry { const char* name; juce::uint32 rgb; };

        static constexpr Entry entries[] =
        {
            { "vocals", 0xF15BAA },
            { "drums",  0xFF9A42 },
            { "bass",   0x34D2FF },
            { "guitar", 0x46E797 },
            { "piano",  0x8466FF },
            { "other",  0xDCEAF4 },
        };

        for (const auto& entry : entries)
        {
            if (stem.equalsIgnoreCase (entry.name))
            {
                r = static_cast<int> ((entry.rgb >> 16) & 0xff);
                g = static_cast<int> ((entry.rgb >> 8) & 0xff);
                b = static_cast<int> (entry.rgb & 0xff);
                return true;
            }
        }

        return false;
    }
}

SourceItem querySelectedItem (const Api& api)
{
    SourceItem result;

    if (! api.isValid())
    {
        result.message = "REAPER did not provide the functions StemLab needs";
        return result;
    }

    if (api.CountSelectedMediaItems (nullptr) < 1)
    {
        result.message =
            "Select an audio item in the arrangement first";
        return result;
    }

    auto* item = api.GetSelectedMediaItem (nullptr, 0);

    if (item == nullptr)
    {
        result.message = "Could not read the selected item";
        return result;
    }

    auto* take = api.GetActiveTake (item);

    if (take == nullptr)
    {
        result.message =
            "The selected item has no active take - select an audio item";
        return result;
    }

    if (api.TakeIsMIDI != nullptr
        && api.TakeIsMIDI (take))
    {
        result.message =
            "The selected item is MIDI - select an audio item";
        return result;
    }

    auto* source = api.GetMediaItemTake_Source (take);

    if (source == nullptr)
    {
        result.message = "The selected item has no audio source";
        return result;
    }

    char path[4096] = { 0 };
    api.GetMediaSourceFileName (source, path, sizeof (path));

    const auto fileName = juce::String::fromUTF8 (path).trim();
    const juce::File file (fileName);

    if (fileName.isEmpty() || ! file.existsAsFile())
    {
        result.message =
            "The selected item is not a plain audio file - "
            "glue or render it first";
        return result;
    }

    result.startSeconds =
        api.GetMediaItemInfo_Value (item, "D_POSITION");

    result.lengthSeconds =
        api.GetMediaItemInfo_Value (item, "D_LENGTH");

    result.startQN =
        api.TimeMap2_timeToQN (nullptr, result.startSeconds);

    result.startOffsetSeconds =
        api.GetMediaItemTakeInfo_Value (take, "D_STARTOFFS");

    const auto rate =
        api.GetMediaItemTakeInfo_Value (take, "D_PLAYRATE");

    result.playRate = rate > 0.0 ? rate : 1.0;

    result.preservePitch =
        api.GetMediaItemTakeInfo_Value (take, "B_PPITCH") > 0.5;

    auto* track = api.GetMediaItem_Track (item);

    if (track != nullptr)
    {
        result.trackNumber =
            static_cast<int> (
                api.GetMediaTrackInfo_Value (
                    track,
                    "IP_TRACKNUMBER"));
    }

    const auto trackName = readTrackName (api, track);
    const auto takeName = readTakeName (api, take);

    if (trackName.isNotEmpty() && takeName.isNotEmpty())
        result.label = trackName + " / " + takeName;
    else if (takeName.isNotEmpty())
        result.label = takeName;
    else if (trackName.isNotEmpty())
        result.label = trackName;
    else
        result.label = file.getFileName();

    result.file = file;
    result.ok = true;
    return result;
}

InsertResult insertStemTracks (
    const Api& api,
    const juce::Array<StemToInsert>& stems,
    const InsertAnchor& anchor,
    const juce::String& sourceLabel)
{
    InsertResult result;

    if (! api.isValid())
    {
        result.message = "REAPER did not provide the functions StemLab needs";
        return result;
    }

    if (stems.isEmpty())
    {
        result.message = "Choose at least one stem to insert";
        return result;
    }

    // Insert below the source track when we know it, otherwise below this
    // plugin's own track, otherwise at the end of the project.
    int insertIndex = api.CountTracks (nullptr);

    if (anchor.afterTrackNumber > 0)
    {
        insertIndex = anchor.afterTrackNumber;
    }
    else if (auto* ownTrack = api.getOwnTrack())
    {
        const auto ownNumber =
            static_cast<int> (
                api.GetMediaTrackInfo_Value (
                    ownTrack,
                    "IP_TRACKNUMBER"));

        if (ownNumber > 0)
            insertIndex = ownNumber;
    }

    insertIndex = juce::jlimit (
        0,
        api.CountTracks (nullptr),
        insertIndex);

    const auto baseName =
        sourceLabel.isNotEmpty() ? sourceLabel : "StemLab";

    api.Undo_BeginBlock2 (nullptr);

    juce::StringArray failures;

    for (const auto& stem : stems)
    {
        const auto pretty = prettyStemName (stem.name);

        if (! stem.file.existsAsFile())
        {
            failures.add (pretty + " (file missing)");
            continue;
        }

        auto* source =
            api.PCM_Source_CreateFromFile (
                stem.file.getFullPathName().toRawUTF8());

        if (source == nullptr)
        {
            failures.add (pretty + " (could not open audio)");
            continue;
        }

        api.InsertTrackAtIndex (insertIndex, true);

        auto* track = api.GetTrack (nullptr, insertIndex);

        if (track == nullptr)
        {
            if (api.PCM_Source_Destroy != nullptr)
                api.PCM_Source_Destroy (source);

            failures.add (pretty + " (track creation failed)");
            continue;
        }

        ++insertIndex;

        const auto trackName = baseName + " - " + pretty;

        {
            // GetSetMediaTrackInfo_String writes through the buffer on set.
            char buffer[512] = { 0 };

            trackName.copyToUTF8 (
                buffer,
                sizeof (buffer));

            api.GetSetMediaTrackInfo_String (
                track,
                "P_NAME",
                buffer,
                true);
        }

        if (api.ColorToNative != nullptr)
        {
            int r = 0, g = 0, b = 0;

            if (stemColour (stem.name, r, g, b))
            {
                api.SetMediaTrackInfo_Value (
                    track,
                    "I_CUSTOMCOLOR",
                    static_cast<double> (
                        api.ColorToNative (r, g, b) | 0x1000000));
            }
        }

        auto* item = api.AddMediaItemToTrack (track);
        auto* take = item != nullptr
            ? api.AddTakeToMediaItem (item)
            : nullptr;

        if (take == nullptr)
        {
            if (api.PCM_Source_Destroy != nullptr)
                api.PCM_Source_Destroy (source);

            failures.add (pretty + " (item creation failed)");
            continue;
        }

        // The take owns the source from here on.
        api.SetMediaItemTake_Source (take, source);

        {
            char buffer[512] = { 0 };

            trackName.copyToUTF8 (
                buffer,
                sizeof (buffer));

            api.GetSetMediaItemTakeInfo_String (
                take,
                "P_NAME",
                buffer,
                true);
        }

        api.SetMediaItemPosition (
            item,
            anchor.startSeconds,
            false);

        double length = anchor.lengthSeconds;

        if (length <= 0.0)
        {
            bool lengthIsQN = false;

            length = api.GetMediaSourceLength (
                source,
                &lengthIsQN);

            if (lengthIsQN)
            {
                // Audio sources report seconds; this path is only reachable
                // with an exotic source type. Convert through the tempo map.
                const auto startQN =
                    api.TimeMap2_timeToQN (
                        nullptr,
                        anchor.startSeconds);

                length =
                    api.TimeMap2_QNToTime (nullptr, startQN + length)
                    - anchor.startSeconds;
            }
        }

        if (length > 0.0)
        {
            api.SetMediaItemLength (
                item,
                length,
                false);
        }

        api.SetMediaItemTakeInfo_Value (
            take,
            "D_STARTOFFS",
            juce::jmax (0.0, anchor.startOffsetSeconds));

        api.SetMediaItemTakeInfo_Value (
            take,
            "D_PLAYRATE",
            anchor.playRate > 0.0 ? anchor.playRate : 1.0);

        api.SetMediaItemTakeInfo_Value (
            take,
            "B_PPITCH",
            anchor.preservePitch ? 1.0 : 0.0);

        ++result.inserted;
    }

    api.TrackList_AdjustWindows (false);
    api.UpdateArrange();

    api.Undo_EndBlock2 (
        nullptr,
        "StemLab: insert stems",
        -1);

    if (result.inserted == 0)
    {
        result.message =
            "No stems could be inserted"
            + (failures.isEmpty()
                   ? juce::String()
                   : " - " + failures.joinIntoString (", "));
    }
    else if (! failures.isEmpty())
    {
        result.message =
            "Inserted "
            + juce::String (result.inserted)
            + (result.inserted == 1 ? " stem" : " stems")
            + ", skipped "
            + failures.joinIntoString (", ");
    }
    else
    {
        result.message =
            "Inserted "
            + juce::String (result.inserted)
            + (result.inserted == 1
                   ? " stem into REAPER"
                   : " stems into REAPER");
    }

    return result;
}
}
