# StemLab UI Guide

A map of the interface and of where every visual decision lives. The UI
implements the "Nocturne" design system, draft **1a "Lanes"** — the design
handoff in [`docs/redesign/`](redesign/README.md) is the spec
(`redesign/screenshots/1a-lanes-spec.png` shows the approved mock).

## Where The Interface Lives

```text
plugin/Source/StemLabTheme.h        Every colour (the Nocturne ground /
                                    surface / accent + neutral ramps), font,
                                    and layout metric, as named tokens. No
                                    colour or font literal exists outside
                                    it; layout keeps only hairline 0-1 px
                                    trims inline.
plugin/Source/StemLabLookAndFeel.*  Inter typefaces (bundled), stock-widget
                                    drawing (buttons by component-ID
                                    variant, progress bar, scrollbars,
                                    menus, tooltips), and the vector icon
                                    set (stemlab::icons).
plugin/Source/StemLabWidgets.*      Nocturne widgets with no stock JUCE
                                    equivalent: include checkbox, icon
                                    buttons, play circle, record button
                                    with pulse dot, the Separate split
                                    control, scrubber, A/B segmented
                                    control, fading divider.
plugin/Source/PluginEditor.h/.cpp   The panel itself: header, source strip,
                                    stem lanes, shared transport, footer.
                                    Layout and wiring only.
plugin/Source/PluginProcessor.*     UI-facing state plus the monitoring
                                    engine: the shared transport, the
                                    stem-mix source with per-stem
                                    solo/mute, and A/B monitor switching.
integrations/ableton/StemLabRemote/ _stem_color duplicates the cross-DAW
                                    stem identity palette in Python. Keep it
                                    byte-identical with
                                    theme::palette::stemIdentityColour.
```

A restyle edits `StemLabTheme.h` (values) and `StemLabLookAndFeel`
(drawing). Only re-arrangements of the panel touch the editor.

## The Panel

One fixed-size window: 904x588 (an 880px-wide Nocturne surface on a 12px
ground margin, height fitting the content stack). Top to bottom:

```text
+----------------------------------------------------------------+
| ||||| StemLab                                            [=]   |  header
| +------------------------------------------------------------+ |
| | song.wav        [Use Selected Item] [• Record PC]          | |  source strip
| | 00:46 · beat 0  ...                 [Refine (o=)|Separate] | |  (recessed)
| +------------------------------------------------------------+ |
| [x] Vocals   [~~~~~~~~ waveform well ~~~~~~~~]  S  M  (=)      |  6 stem lanes
| [x] Drums    [~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~]  S  M  (=)      |  (+ indented
|  ... adaptive child lanes indent under their root ...          |  child lanes,
| (>) 00:33 / 00:46  ----------o------------  [Original|Stems]  |  transport
|  --------------------- fading divider ----------------------  |
| v Separated 6 stems in 03:12 · refinement on                   |  footer
|   [=== 34% · ETA 05:12]     (folder) ~/Stems/ Change           |
|                                  [Save Stems] [Insert Stems]   |
+----------------------------------------------------------------+
```

- **Header** — brand glyph (five accent waveform bars) + "StemLab" (Inter
  17/500) + a 32px sliders icon button opening the settings menu.
- **Source strip** — recessed ground-coloured strip: file name + meta line
  (duration, beat offset, origin), the capture button (accent outline), the
  record button(s) (neutral outline with a pulsing accent dot while
  recording), and the **Separate split control**: a Refine toggle segment
  (accent-900 with a pill switch) fused to the primary Separate action
  (accent-700 shell, accent-400 border, outer glow).
- **Stem lanes** — per lane: include checkbox (15px, accent fill), name
  (13.5/500), waveform well (2px rounded bars from the real peaks; played
  portion left of the shared playhead in accent, unplayed neutral-700,
  muted lanes neutral-800), Solo / Mute (22px, accent-800 / neutral-800
  active fills), and the stacked-layers button for adaptive splits.
  Excluded lanes (checkbox off) drop to 45% opacity. Adaptive child lanes
  indent under their root with an audition Solo and their own layers menu;
  expand/collapse lives in the layers menus.
- **Transport** — one shared clock: 34px play/pause circle, time readout,
  3px scrubber, and the A/B "Original | Stems" segmented control.
- **Footer** — fading divider, status line (check icon + "Separated N
  stems in MM:SS · refinement on/off"), slim progress bar + "NN% · ETA"
  while separating, output folder path + Change (ghost), and the action
  buttons.

## The Shared Transport

`StemLabAudioProcessor` owns one monitoring clock with three faces:

- **Original** — the untouched source file through the single-file
  transport.
- **Stems** — `StemLabStemMixSource`: one reader per completed stem summed
  on a shared clock, with per-stem solo/mute gains read from atomics on the
  audio thread (gain changes ramp across a block, so S/M never click).
  Solo rule: any solo active → only soloed stems sound; otherwise all
  unmuted stems sound. Include checkboxes select stems for Insert/Save and
  do not affect monitoring.
- **Child audition** — a child lane's S button plays exactly that child
  through the single-file transport, at the shared position.

Switching faces transfers position and play state, so A/B is seamless.
Lane waveform clicks and the scrubber both seek the shared clock; every
lane draws the playhead at the same time position. When a separation
finishes, the editor flips monitoring to Stems automatically.

In the standalone app the monitor plays through the device manager; in a
host, `processBlock` replaces the track's audio while the monitor plays
(audition/solo semantics rather than layering).

## Host Modes

Decided once at construction:

| | Standalone | REAPER | Ableton Live | Other host |
|---|---|---|---|---|
| Capture button | Select File | Use Selected Item | Use Live Clip | Select File |
| Recording | Record PC + Record In | Record PC | Record PC | Record PC |
| Footer actions | Save Stems (primary) | Save Stems + Insert Stems (primary) | Retry + Send Stems (primary) | Save Stems (primary) |

The system/PC record button appears only where
`StemLabAudioProcessor::isSystemAudioCaptureSupported()` says capture
works; Record In is standalone-only. Ableton's workflow stays send-only
(no local save).

## Dynamic State

A 20 Hz timer plus a ChangeListener drive `refreshFromProcessor()`:
enablement (everything gates on capturing / engine running / job done;
lane controls additionally need their stem file), play/pause and record
button flips, the time readout and scrubber position, the status line and
progress/ETA, and `syncLanes()` rebuilding adaptive child lanes when the
tree changes. Lanes repaint every tick to advance the playhead.

## Interactions To Preserve

- **Drop audio anywhere on the panel** to load it as the source (stems of
  the current job are rejected so a returning drag cannot wipe the job).
  The panel border and a tint signal the drag.
- **Click a lane's waveform** to seek the shared clock; **drag it** out of
  the window to export that stem file to any DAW or file manager (decided
  at 8px of travel).
- **Settings menu**: audio settings (standalone), separation engine,
  engine executable, diagnostics, Ableton setup (Windows).
- **First-run dialog** for portable standalone builds.

## Design Decisions Of Record

- The interface accent is Nocturne's blurple; the **cross-DAW stem
  identity palette** (REAPER/Ableton track colours) is deliberately
  independent of it and must stay byte-identical with the Remote Script.
- The pre-Nocturne user-selectable waveform palettes (Spectrum/solid hues)
  retired with the redesign; `waveformColourIndex` persists in plugin
  state for compatibility but nothing reads it in the UI.
- Inter Regular/Medium are embedded from `plugin/Resources/fonts/` (OFL;
  see docs/third-party.md). `juce::Font::bold` maps to Inter Medium —
  nothing renders bolder than 500.
- The window is fixed-size per the spec; adaptive child lanes scroll
  within the lanes region instead of growing the panel.
- Tooltips work now (the editor owns a `TooltipWindow`); the vestigial
  `stopButton`/`bridgeLabel` from the old layout are gone.
