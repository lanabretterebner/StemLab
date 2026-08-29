#include "ReaperBridge.h"
#include "SourceLabel.h"
#include "StemLabTheme.h"

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

    /*  Peak building is sliced across timer ticks: enough work per tick to
        finish an ordinary stem in a couple of seconds, short enough that
        REAPER's UI never stalls. The per-file cap is a safety valve for a
        source that never reports itself done.
    */
    constexpr int peakTimerIntervalMs = 25;
    constexpr int peakSlicesPerTick = 48;
    constexpr int peakMaxSlicesPerFile = 40000;

    /*  REAPER caches a media file's peaks beside it as "<filename>.reapeaks",
        media extension included. A project that redirects its peaks to
        another folder leaves nothing here, and a miss means build it, so
        that case behaves exactly as it did before this check existed.

        Not "strictly newer": peaks are written moments after the media they
        describe, and both timestamps land in the same second often enough
        that requiring a later one would rebuild everything every time.
    */
    bool peakFileIsCurrent (const juce::File& file)
    {
        const auto peaks = file.getSiblingFile (file.getFileName() + ".reapeaks");

        return peaks.existsAsFile()
            && peaks.getLastModificationTime() >= file.getLastModificationTime();
    }

    // Same palette the Ableton Remote Script uses, so a user moving between
    // hosts sees the same stem identity. The values live in StemLabTheme.
    bool stemColour (const juce::String& stem, int& r, int& g, int& b)
    {
        const auto colour = stemlab::theme::palette::stemIdentityColour (stem);

        if (! colour.has_value())
            return false;

        r = colour->getRed();
        g = colour->getGreen();
        b = colour->getBlue();
        return true;
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

    // REAPER names an imported item's track after the file and its take
    // after the file without the extension, so joining them blindly gave
    // "Song.wav / Song" - here and on every stem the label goes on to name.
    result.label = stemlab::source::joinSourceLabel (trackName.toStdString(),
                                                     takeName.toStdString());

    if (result.label.isEmpty())
        result.label = file.getFileName();

    result.file = file;
    result.item = item;
    result.ok = true;
    return result;
}

InsertResult insertStemTracks (
    const Api& api,
    const juce::Array<StemToInsert>& stems,
    const InsertAnchor& anchor)
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

    api.Undo_BeginBlock2 (nullptr);

    juce::StringArray failures;

    /*  Folder depths have to balance even when a track cannot be created,
        or REAPER is left with a folder that never closes. A skipped entry
        hands its close to the last track that was created and carries an
        open forward to the next one.
    */
    MediaTrack* lastTrack = nullptr;
    int lastTrackDepth = 0;
    int carriedDepth = 0;

    auto setFolderDepth = [&api] (MediaTrack* track, int depth)
    {
        api.SetMediaTrackInfo_Value (
            track,
            "I_FOLDERDEPTH",
            static_cast<double> (depth));
    };

    for (const auto& stem : stems)
    {
        const auto displayName =
            stem.name.isNotEmpty() ? stem.name : juce::String ("Stem");

        // An empty file is deliberate: a group whose own audio is missing
        // still has to exist as the folder holding its sub-stems.
        PCM_source* source = nullptr;

        if (stem.file != juce::File())
        {
            if (stem.file.existsAsFile())
            {
                source =
                    api.PCM_Source_CreateFromFile (
                        stem.file.getFullPathName().toRawUTF8());

                if (source == nullptr)
                    failures.add (displayName + " (could not open audio)");
            }
            else
            {
                failures.add (displayName + " (file missing)");
            }
        }

        api.InsertTrackAtIndex (insertIndex, true);

        auto* track = api.GetTrack (nullptr, insertIndex);

        if (track == nullptr)
        {
            if (source != nullptr && api.PCM_Source_Destroy != nullptr)
                api.PCM_Source_Destroy (source);

            failures.add (displayName + " (track creation failed)");

            if (stem.folderDepth < 0 && lastTrack != nullptr)
            {
                lastTrackDepth += stem.folderDepth;
                setFolderDepth (lastTrack, lastTrackDepth);
            }
            else
            {
                carriedDepth += stem.folderDepth;
            }

            continue;
        }

        ++insertIndex;

        {
            // GetSetMediaTrackInfo_String writes through the buffer on set.
            char buffer[512] = { 0 };

            displayName.copyToUTF8 (
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

            if (stemColour (stem.colourStem, r, g, b))
            {
                api.SetMediaTrackInfo_Value (
                    track,
                    "I_CUSTOMCOLOR",
                    static_cast<double> (
                        api.ColorToNative (r, g, b) | 0x1000000));
            }
        }

        lastTrackDepth = carriedDepth + stem.folderDepth;
        carriedDepth = 0;
        lastTrack = track;

        setFolderDepth (track, lastTrackDepth);

        if (source == nullptr)
            continue;

        auto* item = api.AddMediaItemToTrack (track);
        auto* take = item != nullptr
            ? api.AddTakeToMediaItem (item)
            : nullptr;

        if (take == nullptr)
        {
            if (api.PCM_Source_Destroy != nullptr)
                api.PCM_Source_Destroy (source);

            failures.add (displayName + " (item creation failed)");
            continue;
        }

        // The take owns the source from here on.
        api.SetMediaItemTake_Source (take, source);

        {
            char buffer[512] = { 0 };

            displayName.copyToUTF8 (
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

        /*  A group's own audio is the sum of its children, so it goes in
            muted: unmute it (and mute the folder's contents) to hear the
            unsplit stem instead.
        */
        if (stem.muted)
        {
            api.SetMediaItemInfo_Value (
                item,
                "B_MUTE",
                1.0);
        }

        result.insertedFiles.addIfNotAlreadyThere (stem.file);
        ++result.inserted;
    }

    // Whatever folder depth the loop could not place belongs on the last
    // track that exists, so the project structure still balances.
    if (carriedDepth != 0 && lastTrack != nullptr)
    {
        lastTrackDepth += carriedDepth;
        setFolderDepth (lastTrack, lastTrackDepth);
    }

    /*  Mute the item the stems came from. Without this the project plays
        the original and its separation on top of each other, which is
        never what Insert Stems is for. REAPER is asked to confirm the
        pointer first - the user may have deleted or replaced the item
        since Use Selected Item read it.
    */
    if (result.inserted > 0
        && anchor.sourceItem != nullptr
        && api.ValidatePtr2 != nullptr
        && api.ValidatePtr2 (nullptr, anchor.sourceItem, "MediaItem*"))
    {
        api.SetMediaItemInfo_Value (
            anchor.sourceItem,
            "B_MUTE",
            1.0);

        result.mutedSourceItem = true;
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

        return result;
    }

    juce::String message =
        "Inserted "
        + juce::String (result.inserted)
        + (result.inserted == 1 ? " stem" : " stems");

    if (! failures.isEmpty())
        message += ", skipped " + failures.joinIntoString (", ");
    else if (result.mutedSourceItem)
        message += " - source item muted";

    result.message = message;
    return result;
}

// ================================================================== peaks

PeakBuilder::PeakBuilder (const Api& apiIn, const juce::Array<juce::File>& files)
    : api (apiIn)
{
    if (api.PCM_Source_CreateFromFile == nullptr
        || api.PCM_Source_BuildPeaks == nullptr)
    {
        // An older REAPER simply keeps its existing behaviour: peaks appear
        // whenever something else asks REAPER to build them.
        finished = true;
        return;
    }

    // Insert, undo, re-insert is how a user retries placement, and the
    // peaks built by the first attempt are still on disk and still valid.
    for (const auto& file : files)
        if (file.existsAsFile() && ! peakFileIsCurrent (file))
            pending.addIfNotAlreadyThere (file);

    if (pending.isEmpty())
    {
        finished = true;
        return;
    }

    startTimer (peakTimerIntervalMs);
}

PeakBuilder::~PeakBuilder()
{
    stopTimer();
    closeCurrentSource();
}

void PeakBuilder::closeCurrentSource()
{
    if (current == nullptr)
        return;

    api.PCM_Source_BuildPeaks (current, 2);

    if (api.PCM_Source_Destroy != nullptr)
        api.PCM_Source_Destroy (current);

    current = nullptr;
    slicesOnCurrent = 0;
}

bool PeakBuilder::startNextFile()
{
    while (! pending.isEmpty())
    {
        const auto file = pending.removeAndReturn (0);

        // Peaks can arrive between the queue being built and a file's turn -
        // REAPER builds them itself for anything it draws - and a source
        // opened for nothing still costs a create, a tick and a destroy.
        if (peakFileIsCurrent (file))
            continue;

        current =
            api.PCM_Source_CreateFromFile (
                file.getFullPathName().toRawUTF8());

        if (current == nullptr)
            continue;

        slicesOnCurrent = 0;

        // The start call is not gated on its return value: a source that
        // needs nothing simply reports done on the first slice below.
        api.PCM_Source_BuildPeaks (current, 0);
        return true;
    }

    return false;
}

void PeakBuilder::finish()
{
    stopTimer();
    finished = true;

    // The items were drawn before their peaks existed; this is what turns
    // the empty lanes into waveforms.
    if (api.UpdateArrange != nullptr)
        api.UpdateArrange();
}

void PeakBuilder::timerCallback()
{
    // Nothing in flight and nothing left to start: end the run rather than
    // spend a slice budget - every slice blocks the thread REAPER draws on -
    // on a list where there is no work to do.
    if (current == nullptr && ! startNextFile())
    {
        finish();
        return;
    }

    for (int slice = 0; slice < peakSlicesPerTick; ++slice)
    {
        // A source that reports itself done, and one that never will, both
        // end the file here rather than spend the rest of the tick on it.
        // The cap is the safety valve for the second kind.
        const bool done = api.PCM_Source_BuildPeaks (current, 1) == 0
                       || ++slicesOnCurrent >= peakMaxSlicesPerFile;

        if (! done)
            continue;

        closeCurrentSource();

        // Last file: finish now instead of waking once more to find the
        // list empty.
        if (pending.isEmpty())
            finish();

        return;
    }
}
}
