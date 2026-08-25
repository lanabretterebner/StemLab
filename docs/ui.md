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
                                    it; layout values are tokens apart from
                                    a few small one-off trims at their call
                                    sites.
plugin/Source/StemLabLookAndFeel.*  Inter typefaces (bundled), stock-widget
                                    drawing (buttons by component-ID
                                    variant, progress bar, scrollbars,
                                    menus, tooltips), and the vector icon
                                    set (stemlab::icons).
plugin/Source/StemLabWidgets.*      Nocturne widgets with no stock JUCE
                                    equivalent: include checkbox, icon
                                    buttons, lane disclosure twisty, play
                                    circle, record button with pulse dot,
                                    the Separate split control, scrubber,
                                    A/B segmented control, fading divider.
plugin/Source/PluginEditor.h/.cpp   The panel itself: header, source strip,
                                    stem lanes, shared transport, footer.
                                    Layout and wiring only.
plugin/Source/PluginProcessor.*     UI-facing state plus the monitoring
                                    engine: the shared transport, the
                                    stem-mix source with per-lane
                                    solo/mute, and A/B monitor switching.
integrations/ableton/StemLabRemote/ _stem_color duplicates the cross-DAW
                                    stem identity palette in Python. Keep it
                                    byte-identical with
                                    theme::palette::stemIdentityColour.
```

A restyle edits `StemLabTheme.h` (values) and `StemLabLookAndFeel`
(drawing). Only re-arrangements of the panel touch the editor.

## The Panel

One window, designed at 904x588 (an 880px-wide Nocturne surface on a 12px
ground margin, height fitting the content stack). It resizes by **scaling**:
`StemLabPanelContent` holds every control, is laid out once at the design
size, and the editor warps it with a single `AffineTransform`. A fixed
aspect-ratio constrainer keeps the two in step between 0.70x and 2.50x, and
the size the user leaves behind persists in plugin state (`editorScale`), so
a reopened window comes back the way they left it. Nothing in the layout is
size-dependent - every metric in `StemLabTheme` stays a real pixel value.
Top to bottom:

```text
+----------------------------------------------------------------+
| ||||| StemLab                                            [=]   |  header
| +------------------------------------------------------------+ |
| | song.wav        [Use Selected Item] [• Record PC]          | |  source strip
| | 00:46 · beat 0  ...                 [Refine (o=)|Separate] | |  (recessed)
| +------------------------------------------------------------+ |
|    [x] Vocals [~~~~~~ waveform well ~~~~~~]  S  M  (=)         |  6 stem lanes
|  v [x] Drums  [~~~~~~~~~~~~~~~~~~~~~~~~~~]  S  M  (=)         |  (+ indented
|       [x] Kick [~~~~~~~~~~~~~~~~~~~~~~~~]  S  M                |  child lanes,
| (>) 00:33 / 00:46  ----------o------------  [Original|Stems]  |  transport
|  --------------------- fading divider ----------------------  |
| v Separated 6 stems in 03:12 · refinement on                   |  footer
|   [=== 34% · ETA 05:12]     (folder) ~/Stems/ Change           |
|                                  [Save Stems] [Insert Stems]   |
+----------------------------------------------------------------+
```

- **Header** — brand glyph (five accent waveform bars) + "StemLab" (Inter
  17/500) + a 32px sliders icon button opening the settings menu. The
  sliders glyph is a filled path, not a stroked one: its rails are barely a
  pixel tall, and outlining them turned the icon into a smudge.
- **Source strip** — recessed ground-coloured strip: file name + meta line
  (duration, beat offset, origin), the capture button (accent outline), the
  record button(s) (neutral outline with a pulsing accent dot while
  recording), and the **Separate split control**: a Refine toggle segment
  (accent-900 with a pill switch) fused to the primary Separate action
  (accent-700 shell, accent-400 border, outer glow). While a job runs the
  action segment reads Cancel ("Cancelling..." once requested) and aborts
  the whole job through the engine's cancel watchdog.
- **Stem lanes** — per lane: disclosure twisty (14px column, chevron shown
  only on lanes that have children), include checkbox (15px, accent fill),
  name (13.5/500), waveform well (2px rounded bars from the real peaks;
  played portion left of the shared playhead in accent, unplayed
  neutral-700, muted lanes neutral-800), Solo / Mute (22px, accent-800 /
  neutral-800 active fills), and the stacked-layers button for adaptive
  splits. Excluded lanes (checkbox off) drop to 45% opacity. Adaptive child
  lanes indent under their root and carry exactly the same controls,
  because a child stands in for its parent in the monitor mix. The twisty
  column is reserved on every lane so checkboxes and names stay on one grid;
  expand/collapse is also still in the layers menus.
- **Transport** — one shared clock: 34px play/pause circle, time readout,
  3px scrubber, and the A/B "Original | Stems" segmented control. The
  selected option is a closed pill inset 3px inside the shell - drawing it
  as the shell's own rounded rectangle clipped to one half left the ring
  open along the divider.
- **Footer** — fading divider, status line (check icon + "Separated N
  stems in MM:SS · refinement on/off"), slim progress bar + "NN% · ETA"
  while separating, output folder path + Change (ghost), and the action
  buttons. The folder icon is placed against the measured width of the
  right-aligned path text rather than the label's fixed column, so no gap
  opens up in front of a short path.

## The Shared Transport

`StemLabAudioProcessor` owns one monitoring clock with two faces, plus the
rules for what the stem face plays:

- **Original** — the untouched source file through the single-file
  transport.
- **Stems** — `StemLabStemMixSource`: one reader per audible lane summed on
  a shared clock, with per-lane solo/mute gains read from atomics on the
  audio thread (gain changes ramp across a block, so S/M never click).
  Solo rule: any solo active → only soloed lanes sound; otherwise all
  unmuted lanes sound. Include checkboxes select stems for Insert/Save and
  do not affect monitoring.
- **Which lanes are in the mix** — the leaves of the stem tree. A root stem
  that was split further hands its slot to its adaptive children, so nothing
  is ever summed together with the children it was split into. That is what
  gives a child lane a real Mute, and it replaced the old exclusive child
  audition: soloing a child now plays it through the same shared clock as
  everything else. Turning any solo on switches the monitor to Stems, the
  only face where solo means anything.
- **Rebuilds** — the loaded mix carries the tree generation it was built
  from, and the editor's refresh rebuilds it when a split lands, keeping
  position and play state.

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
- The user-selectable waveform palettes are back, under **Settings >
  Waveform Colour**, reading the `waveformColourIndex` the redesign left
  persisted but unread. Index 0 is the redesign's single-accent look,
  unchanged. The rest colour the whole lane and dim the unplayed portion of
  their own hue: **Stem Colours** paints each lane in its cross-DAW stem
  identity colour (a child lane takes its root's, so a split stem still
  reads as one family), **Spectrum** sweeps one hue across each lane, and
  four solid hues do what they say. The order in `theme::waveform` must stay
  stable - the index is saved in plugin state - and a `static_assert` in the
  editor keeps its count and `waveformColourCount` in step.
- Inter Regular/Medium are embedded from `plugin/Resources/fonts/` (OFL;
  see docs/third-party.md). `juce::Font::bold` maps to Inter Medium —
  nothing renders bolder than 500.
- The window scales rather than reflowing, and its aspect ratio is fixed:
  a lane has identical proportions at every size. Adaptive child lanes
  scroll within the lanes region instead of growing the panel.
- The panel has no resting outline. Its drop shadow is what lifts it off the
  ground; a permanent 1px edge only boxed the window in. The accent border
  still appears while audio is dragged over the window.
- Tooltips work now (the editor owns a `TooltipWindow`); the vestigial
  `stopButton`/`bridgeLabel` from the old layout are gone.
- The spec's keyboard-focus ring is intentionally not implemented: the
  plugin editor is built with `EDITOR_WANTS_KEYBOARD_FOCUS FALSE`, so
  there is no keyboard traversal to indicate. Revisit if that changes.
