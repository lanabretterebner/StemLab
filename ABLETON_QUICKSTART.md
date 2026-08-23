# StemLab - Ableton Live quick start

The normal Windows release already contains everything StemLab needs:

```text
StemLab.exe
StemLab.vst3
StemLabRemote\
Engine\
```

No Max for Live device is required.

## Recommended setup

1. Download and extract `StemLab-Windows.zip` from GitHub Releases.
2. Run `StemLab.exe` once.
3. On the first-launch screen click **Set Up Ableton**.

You can also run the same setup later from:

```text
Settings > Ableton Live > Install / Repair Ableton Integration...
```

The setup requests Administrator permission only for the normal VST3 folder and
installs:

```text
StemLab.vst3
  -> C:\Program Files\Common Files\VST3\StemLab.vst3

StemLabRemote\
  -> <your Ableton User Library>\Remote Scripts\StemLabRemote
```

The large `Engine\` directory is **not copied**. The VST3 reuses the Engine in
the extracted StemLab folder. Keep that folder in place.

If the User Library cannot be detected automatically, StemLab opens a folder
picker. In Ableton you can locate the correct folder with:

```text
Browser > right-click User Library > Show in Explorer
```

StemLab does not force-close Ableton. If Live is open, save your project and
fully quit Live before running setup again.

## Enable StemLabRemote

Restart Ableton Live, then open:

```text
Settings > Link, Tempo & MIDI
```

Set:

```text
Control Surface = StemLabRemote
Input = None
Output = None
```

## Verify the VST3

If StemLab does not appear in Live:

```text
Settings > Plug-ins > Rescan
```

The VST3 is installed to the standard Windows location:

```text
C:\Program Files\Common Files\VST3\StemLab.vst3
```

## Normal workflow

1. Put **StemLab** on an audio track.
2. Select an Arrangement audio clip.
3. Click **Use Live Clip**.
4. Click **Separate All Stems**.
5. Audition the completed stems.
6. Check the stems you want.
7. Click **Send Selected**.

`StemLabRemote` creates the selected Arrangement tracks beneath the source
track.
