StemLabRemote
=============

Invisible Ableton Live background integration for StemLab.

Responsibilities:
- return the selected/current Arrangement audio clip source file to StemLab,
- import selected completed stems as Arrangement tracks/clips,
- turn a transcribed stem into one editable Arrangement MIDI clip,
- put the analysed tempo into the Set, turning Warp off on the clips playing
  the source file first, so the audio the tempo was measured from still plays
  at its own rate,
- report import progress and completion back to the VST.

StemLabRemote owns no MIDI controls and requires no MIDI Input/Output.

One-time setup:

Settings > Link, Tempo & MIDI
Control Surface = StemLabRemote
Input = None
Output = None

No Max for Live device is required.
