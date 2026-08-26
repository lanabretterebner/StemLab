# StemLab With Ableton Live

StemLab uses two components inside Ableton:

```text
StemLab.vst3       Captures/loads audio and runs separation
StemLabRemote       Creates Arrangement tracks and clips
```

No Max for Live device or MIDI port is required.

## Build And Install

From the source repository:

```powershell
.\scripts\win\setup_dev.ps1
.\scripts\win\build_plugin.ps1
.\scripts\win\install_ableton.ps1
```

The final command installs:

```text
StemLab.vst3
  -> C:\Program Files\Common Files\VST3\StemLab.vst3

StemLabRemote
  -> <Ableton User Library>\Remote Scripts\StemLabRemote
```

If automatic User Library detection fails, locate it with **Browser > User
Library > Show in Explorer**, then run:

```powershell
.\scripts\win\install_ableton.ps1 -UserLibrary "C:\full\path\to\User Library"
```

Save and close Ableton before installing; the script refuses to force-close it.

## Enable The Remote Script

Restart Ableton and open **Settings > Link, Tempo & MIDI**. Choose:

```text
Control Surface = StemLabRemote
Input = None
Output = None
```

If StemLab is missing from the plug-in browser, open **Settings > Plug-ins** and
rescan VST3 plug-ins.

## Workflow

1. Put StemLab on an audio track.
2. Select an Arrangement audio clip.
3. Click **Use Live Clip** in StemLab.
4. Click **Separate** (pick a different engine under Settings first if you
   want one; while the job runs the same control reads **Cancel**).
5. Audition the lanes - solo/mute per stem, A/B **Original | Stems**.
6. Click **Send Stems**.

`StemLabRemote` creates the selected stems as Arrangement tracks beneath the
source track. **Retry** re-asks the Remote Script for the import if Live was
busy the first time.
