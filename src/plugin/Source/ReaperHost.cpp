#include "ReaperHost.h"

/*
    This is the only translation unit that touches the VST3 SDK directly, so
    the SDK's build-mode requirement is satisfied here instead of polluting
    the whole target with a macro as generic as "RELEASE".
*/
#if ! defined (DEVELOPMENT) && ! defined (RELEASE)
 #ifdef NDEBUG
  #define RELEASE 1
 #else
  #define DEVELOPMENT 1
 #endif
#endif

#include <pluginterfaces/base/funknown.h>

namespace stemlab::reaper
{
namespace
{
    using namespace Steinberg;

    /*  From reaper_vst3_interfaces.h in the REAPER SDK (Cockos Incorporated,
        zlib-style licence). Redeclared here rather than vendoring the header
        because the plugin needs exactly this one interface.

        The vtable must match REAPER's exactly: FUnknown's three methods,
        then these three, in this order.
    */
    class IReaperHostApplication : public FUnknown
    {
    public:
        virtual void* PLUGIN_API getReaperApi (const char* funcname) = 0;
        virtual void* PLUGIN_API getReaperParent (uint32 whattype) = 0;
        virtual void* PLUGIN_API reaperExtended (
            uint32 call, void* parm1, void* parm2, void* parm3) = 0;

        static const FUID iid;
    };

    DECLARE_CLASS_IID (IReaperHostApplication,
                       0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C)
}

Api::~Api()
{
    if (reaperHost != nullptr)
        static_cast<IReaperHostApplication*> (reaperHost)->release();
}

std::unique_ptr<Api> Api::tryCreate (Steinberg::FUnknown* hostContext)
{
    if (hostContext == nullptr)
        return nullptr;

    IReaperHostApplication* host = nullptr;

    if (hostContext->queryInterface (
            IReaperHostApplication_iid,
            reinterpret_cast<void**> (&host)) != kResultOk
        || host == nullptr)
    {
        return nullptr;
    }

    // queryInterface addRef'd the pointer; ~Api releases it.
    std::unique_ptr<Api> api (new Api());
    api->reaperHost = host;

    auto resolve = [host, &api] (auto& target,
                                 const char* name,
                                 bool required)
    {
        target = reinterpret_cast<
            std::remove_reference_t<decltype (target)>> (
                host->getReaperApi (name));

        if (target == nullptr && required)
            api->missingFunctions.add (name);
    };

    resolve (api->CountSelectedMediaItems,     "CountSelectedMediaItems",     true);
    resolve (api->GetSelectedMediaItem,        "GetSelectedMediaItem",        true);
    resolve (api->GetActiveTake,               "GetActiveTake",               true);
    resolve (api->GetMediaItemTake_Source,     "GetMediaItemTake_Source",     true);
    resolve (api->GetMediaSourceFileName,      "GetMediaSourceFileName",      true);
    resolve (api->GetMediaSourceLength,        "GetMediaSourceLength",        true);
    resolve (api->GetMediaItem_Track,          "GetMediaItem_Track",          true);
    resolve (api->GetMediaItemInfo_Value,      "GetMediaItemInfo_Value",      true);
    resolve (api->GetMediaItemTakeInfo_Value,  "GetMediaItemTakeInfo_Value",  true);
    resolve (api->TimeMap2_timeToQN,           "TimeMap2_timeToQN",           true);
    resolve (api->TimeMap2_QNToTime,           "TimeMap2_QNToTime",           true);
    resolve (api->InsertTrackAtIndex,          "InsertTrackAtIndex",          true);
    resolve (api->CountTracks,                 "CountTracks",                 true);
    resolve (api->GetTrack,                    "GetTrack",                    true);
    resolve (api->GetMediaTrackInfo_Value,     "GetMediaTrackInfo_Value",     true);
    resolve (api->SetMediaTrackInfo_Value,     "SetMediaTrackInfo_Value",     true);
    resolve (api->GetSetMediaTrackInfo_String, "GetSetMediaTrackInfo_String", true);
    resolve (api->GetSetMediaItemTakeInfo_String,
             "GetSetMediaItemTakeInfo_String",
             true);
    resolve (api->PCM_Source_CreateFromFile,   "PCM_Source_CreateFromFile",   true);
    resolve (api->AddMediaItemToTrack,         "AddMediaItemToTrack",         true);
    resolve (api->AddTakeToMediaItem,          "AddTakeToMediaItem",          true);
    resolve (api->SetMediaItemTake_Source,     "SetMediaItemTake_Source",     true);
    resolve (api->SetMediaItemPosition,        "SetMediaItemPosition",        true);
    resolve (api->SetMediaItemLength,          "SetMediaItemLength",          true);
    resolve (api->SetMediaItemTakeInfo_Value,  "SetMediaItemTakeInfo_Value",  true);
    resolve (api->SetMediaItemInfo_Value,      "SetMediaItemInfo_Value",      true);
    // Optional: without them Set Host Tempo is unavailable, which is a
    // better outcome than refusing to bridge to REAPER at all.
    resolve (api->SetCurrentBPM,               "SetCurrentBPM",               false);
    resolve (api->SetTempoTimeSigMarker,       "SetTempoTimeSigMarker",       false);
    resolve (api->CountTempoTimeSigMarkers,    "CountTempoTimeSigMarkers",    false);
    resolve (api->DeleteTempoTimeSigMarker,    "DeleteTempoTimeSigMarker",    false);
    resolve (api->Undo_BeginBlock2,            "Undo_BeginBlock2",            true);
    resolve (api->Undo_EndBlock2,              "Undo_EndBlock2",              true);
    resolve (api->UpdateArrange,               "UpdateArrange",               true);
    resolve (api->TrackList_AdjustWindows,     "TrackList_AdjustWindows",     true);

    resolve (api->ColorToNative,               "ColorToNative",               false);
    resolve (api->PCM_Source_Destroy,          "PCM_Source_Destroy",          false);
    resolve (api->GetAppVersion,               "GetAppVersion",               false);
    resolve (api->TakeIsMIDI,                  "TakeIsMIDI",                  false);
    resolve (api->ValidatePtr2,                "ValidatePtr2",                false);
    resolve (api->PCM_Source_BuildPeaks,       "PCM_Source_BuildPeaks",       false);

    api->valid = api->missingFunctions.isEmpty();
    return api;
}

juce::StringArray Api::getMissingFunctionNames() const
{
    return missingFunctions;
}

juce::String Api::getAppVersion() const
{
    if (GetAppVersion == nullptr)
        return {};

    if (const auto* version = GetAppVersion())
        return juce::String (juce::CharPointer_UTF8 (version));

    return {};
}

MediaTrack* Api::getOwnTrack() const
{
    if (reaperHost == nullptr)
        return nullptr;

    // 1 = the track this plugin instance is inserted on.
    return static_cast<MediaTrack*> (
        static_cast<IReaperHostApplication*> (reaperHost)
            ->getReaperParent (1));
}
}
