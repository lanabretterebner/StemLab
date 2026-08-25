# StemLab UI Guide

A map of the current interface and of where every visual decision lives,
written as preparation for the UI overhaul. After reading it you should know
what the UI contains, which file to open to change any given look, which
behaviors must survive a restyle untouched, and what the approved redesign
adds on top.

The approved design lives in [`docs/redesign/`](redesign/README.md): the
"Nocturne" system, draft **1a "Lanes"** (see
`redesign/screenshots/1a-lanes-spec.png`). The last section of this guide
maps the current interface onto it.

## Where The Interface Lives

```text
plugin/Source/StemLabTheme.h        Every colour, palette, font, and layout
                                    metric, as named tokens. No colour or
                                    font literal remains in the editor, and
                                    layout keeps only hairline 0-1 px trims
                                    inline.
plugin/Source/StemLabLookAndFeel.h  The restyling hook for JUCE's stock
                                    widgets. Currently transparent: it
                                    inherits LookAndFeel_V4's dark scheme
                                    unchanged.
plugin/Source/PluginEditor.h/.cpp   Controls, layout, and waveform painting.
                                    Asks the theme for visual values; only
                                    hairline 0-1 px trims stay inline.
plugin/Source/PluginProcessor.h     UI-facing state the editor polls: status
                                    text, progress, stem files, enablement,
                                    the saved waveform-palette choice.
plugin/Source/ReaperBridge.cpp      Colours inserted REAPER tracks with the
                                    stem identity palette from the theme.
integrations/ableton/StemLabRemote/ _stem_color duplicates the stem identity
                                    palette in Python (it cannot include the
                                    C++ header). Keep it byte-identical.
```

A redesign that only changes colours, fonts, and spacing edits
`StemLabTheme.h`. One that restyles widget shapes (button corners, toggle
marks, scrollbars, popup menus) overrides draw methods in
`StemLabLookAndFeel`. Only a re-arrangement of the interface itself needs to
touch `PluginEditor`.

## Window Layout

One window, no pages or tabs. Top to bottom (`resized()` lays out in exactly
this order):

```text
+----------------------------------------------------------+
| StemLab                                       [Settings] |  header (title +
| subtitle: workflow hint, varies by host                  |  subtitle)
|  +------------------------------------------------------+
|  | [Capture] [Play] ([Record PC])   capture status      ||  input row
|  | ([Record System] [Record Input]  - standalone only)  ||  recording row
|  | [x] StemLab refinement                               ||  refinement
|  | [        Separate All Stems        ]                 ||  primary action
|  | status line                                          ||
|  | [############ progress 42% ############]             ||
|  | Elapsed 01:23  |  ETA 00:41                          ||
|  | Audition stems, then choose what to save             ||  stems heading
|  | +--------------------------------------------------+ ||
|  | | [>] [x] Vocals  [~~~waveform~~~] [Play] [...]    | ||  stem tree
|  | |       [x] Lead    [~~waveform~~] [Play] [...]    | ||  (viewport,
|  | | [x] Drums       [~~~waveform~~~] [Play] [...]    | ||  scrolls when
|  | | ... six root stems + adaptive child rows ...     | ||  rows overflow)
|  | +--------------------------------------------------+ ||
|  | [Save Selected...] [Choose File Location]            ||  action row
|  +------------------------------------------------------+
+----------------------------------------------------------+
```

The window opens at 680x680 and resizes between 540x540 and 1400x1200.
Vertical slack goes to the stem rows (up to 72 px each), never to dead
space. The panel behind everything below the header is painted in
`StemLabAudioProcessorEditor::paint`, which also draws the drag-and-drop
highlight state.

### Responsive behavior

Two independent breakpoints, both at 620 px (`theme::metrics::compactWidth`
/ `compactHeight`):

- **narrow** (width < 620): button widths shrink, outer padding shrinks.
- **shallow** (height < 620): row heights, header height, and vertical gaps
  shrink, outer padding shrinks.

Every adaptive metric in `StemLabTheme.h` takes the relevant flag and
returns the compact or regular value; `resized()` computes `narrow` and
`shallow` once at the top.

## Components

Three classes, all in `PluginEditor.h/.cpp`:

- `StemLabAudioProcessorEditor` - the whole window. Owns the header, the
  control rows, six fixed root-stem rows (checkbox, waveform, play,
  adaptive-split menu, expand toggle), and the action row.
- `StemWaveformComponent` - one waveform tile. Paints the thumbnail with a
  per-slice colour driven by local volume, the timing grid, the playhead,
  and the time badge. Converts clicks into preview seeks and drags into
  external file drags.
- `RecursiveStemRowComponent` - one row of the adaptive stem tree (children
  produced by Adaptive Split / De-Reverb), indented by depth, with its own
  select toggle, expand button, play button, action menu, and waveform.

Root rows and adaptive rows are laid out by different code with different
metric groups (`theme::metrics::stemTree` vs `theme::metrics::adaptiveRow`);
visually they are meant to read as one tree.

## Host Modes

The editor renders one of four variants, decided once at construction from
`processor.isStandaloneApp()` and `processor.getHostIntegration()`:

| | Standalone | REAPER | Ableton Live | Other host |
|---|---|---|---|---|
| Capture button | Select File | Use Selected Item | Use Live Clip | Select File |
| Recording | Record System + Record Input row | Record PC in input row | Record PC in input row | Record PC in input row |
| Action row | Save Selected + location | Insert Stems + Save Selected + location | Send Selected + Retry + location | Save Selected + location |
| Subtitle / stems heading | "...then save" | "...then insert" | "...then send (to Ableton)" | "...then save" |

The system/PC record button appears only where
`StemLabAudioProcessor::isSystemAudioCaptureSupported()` says capture
works; Record Input is always present in standalone (and never in a host).
A redesign must keep all four variants in mind: the action row in
particular has three different button sets.

## Dynamic State

The editor is a passive view over the processor. A 20 Hz timer
(`theme::metrics::uiRefreshHz`) plus a ChangeListener drive
`refreshFromProcessor()`, which recomputes for every widget:

- **Enablement** - almost everything disables while capturing or while the
  separation engine runs; stem controls additionally require a finished
  job, and the per-stem play buttons, waveforms, and split menus also
  require the stem file to exist (the root checkboxes do not). Send
  Selected also requires at least one selected stem.
- **Text** - Play/Pause flips per preview target; Record buttons flip to
  Stop variants; the capture label cycles between hints, recording time,
  and file name; status/progress/timing mirror engine state.
- **Structure** - `syncRecursiveRows()` rebuilds the adaptive rows when the
  set of visible tree items changes (expand/collapse, new splits).

Waveform tiles repaint on every tick; their painting cost is budgeted
around that rate (see the slice-width token note in the theme).

## Interactions To Preserve

- **Drop audio anywhere on the window** to load it as the source. While a
  drag hovers, the panel border switches to the accent colour and a "Drop
  audio to load" overlay appears. Stems of the current job are rejected so
  a returning drag cannot wipe the finished job.
- **Click a waveform** to seek the preview; **drag a waveform** out of the
  window to export the stem file to any DAW or file manager. The gesture is
  decided at 8 px of travel (`clickVersusDragThreshold`); the decision is
  deliberately made on release so drags never disturb playback.
- **Popup menus**: Settings (audio settings, waveform palette, separation
  engine, engine path, diagnostics, Ableton setup), per-stem adaptive-split
  menus (`...`), and per-adaptive-row action menus (De-Reverb, Adaptive
  Split Further).
- **Waveform palettes**: the user picks Spectrum (volume-mapped ramp) or a
  solid hue; the choice persists in plugin state. Palette math lives in
  `theme::palette`.
- **First-run dialog** (standalone portable builds only) offering Ableton
  setup.

## The Two Palettes

Do not conflate these when restyling:

- `theme::palette::solidWaveformColour` / `spectrumColourForLevel` - what
  the *editor* draws waveforms with; user-selectable, cosmetic.
- `theme::palette::stemIdentityColour` - the track colours *DAWs* receive
  for inserted stems, shared byte-for-byte with the Ableton Remote Script
  (`_stem_color` in `integrations/ableton/StemLabRemote/__init__.py`).

The hues are similar but not identical (e.g. drums `0xFF9A42` identity vs
Amber `0xFFB342` waveform). Unifying them would change what users already
see in their DAWs - treat that as a product decision inside the redesign,
and if taken, change the Remote Script in the same commit.

## Known Quirks (Pre-Existing, Preserved)

Found while extracting the theme; left untouched because this preparation
changes no behavior. Candidates for the redesign to resolve:

- `paint()` places the background panel using a fixed 18 px margin and
  78 px header reserve (`theme::metrics::panel::paintMargin` /
  `headerReserve`), while `resized()` shrinks its padding at compact sizes,
  so at small windows the painted panel and the laid-out controls disagree
  by a few pixels.
- Tooltips are set on several buttons, but no `juce::TooltipWindow` is ever
  created, so in the standalone app they never display.
- `stopButton` is added to the tree but permanently hidden-by-zero-bounds
  and disabled; `bridgeLabel` receives Ableton bridge status text but is
  never added to the tree. Both are vestigial.
- The root-stem checkbox shows the capitalized stem name; adaptive rows
  derive labels (and "est. N sources" suffixes) from engine output.

## The Approved Redesign: Nocturne 1a "Lanes"

`docs/redesign/README.md` is the spec; `redesign/styles.css` is the token
sheet; `redesign/StemLab Drafts.dc.html` is the interactive reference
(draft 1a at the top is approved; 1b/1c are alternates). Colors, spacing,
sizes and copy in 1a are final intent, recreated in JUCE against the
existing `PluginEditor`/`PluginProcessor` split.

### Current -> target, element by element

| Current | Target (1a) | Nature of change |
|---|---|---|
| Title label + subtitle text | Brand glyph + "StemLab" (Inter 17/500), no subtitle | Restyle; subtitle's workflow hint disappears |
| Settings text button | 32x32 icon button (sliders) | Restyle |
| Capture button + capture label | Recessed source strip: file block plus "Use Selected Item" and "Record PC" | Rearrange; same processor calls |
| Refinement checkbox + Separate button | One split control: Refine toggle segment + Separate segment | Rearrange; same two processor flags/calls |
| Status/progress/timing rows mid-panel | Footer: status line + slim progress bar with "34% - ETA 05:12" | Move + restyle; same processor state |
| Stem checkbox rows + per-slice colored waveforms | Six aligned lanes: include checkbox, name, waveform well (unplayed neutral, played accent), S/M buttons, layers icon | Rearrange + restyle; see capability gaps |
| Per-stem Play/Pause buttons | One shared transport: play circle, time, scrubber, A/B "Original / Stems" | **New playback model** |
| Per-stem "..." adaptive-split menu | Layers icon per lane ("Split this stem further") | Simplify; adaptive tree/De-Reverb needs a home |
| Save Selected / Insert Stems / Choose File Location | Footer: folder path + "Change" ghost button, "Save Stems", filled "Insert Stems" | Restyle |
| User-selectable waveform palettes (Spectrum/solid hues) | Single accent-on-neutral waveform treatment | Product decision: palette setting likely retires |
| Resizable 540x540..1400x1200, two compact breakpoints | Fixed-width 880px reference, height fits content (~620px) | Product decision: fixed vs. resizable |

### Capability gaps (engine/processor work the redesign needs)

The current preview plays **one** stem (or the source) at a time through
`playCompletedStem` / `toggleStandalonePlayback`. Draft 1a needs:

- **Stem-mix playback**: all ready stems mixed, respecting per-lane
  solo/mute, on one shared clock.
- **A/B monitoring**: instant switch between the untouched source and the
  stem mix at the same position.
- **Shared playhead + scrubber**: one transport position rendered across
  all six lanes and a seekable scrubber; per-lane click-to-seek goes away
  or seeks the shared clock.
- **Solo/mute state** per stem in the processor (UI-only today: nothing
  stores it).
- **Record-armed pulse** on the Record PC dot (timer-driven paint state).

None of this exists in `StemLabAudioProcessor` yet; plan it as processor
work that lands before or with the visual rebuild.

### Also decide during implementation

- **Host modes**: the spec shows REAPER ("Use Selected Item", "Insert
  Stems"). Standalone/Ableton/generic keep their own capture and action
  wording inside the same visual language - the table in "Host Modes" above
  lists every variant that needs a Nocturne equivalent.
- **Adaptive stem tree**: 1a shows six fixed lanes. Child stems from
  adaptive splits (and De-Reverb) need either indented child lanes or a
  drill-in view; the split icon's action is specified, the result's
  presentation is not.
- **Inter font**: bundle via `juce_add_binary_data` and register in
  `StemLabLookAndFeel::getTypefaceForFont`, or accept the platform default.
- **Vestigial widgets**: the redesign is the moment to delete `stopButton`
  and `bridgeLabel`, and to add a `TooltipWindow` so the specified tooltips
  actually show.

### Order of work

1. **Retune tokens.** Point `StemLabTheme.h` values at the Nocturne sheet
   (`redesign/styles.css`): ground `#161826`, surface `#232532`, accent
   `#9184d9` + its 100..900 ramp, text `#e9e9ed`, radii 4/6/8/14. The
   whole current layout re-skins immediately - a useful intermediate even
   before lanes land.
2. **Widget shapes.** Override `drawButtonBackground`, `drawToggleButton`,
   `drawProgressBar`, `drawScrollbar`, `drawPopupMenuItem`, focus outlines
   in `StemLabLookAndFeel` (already installed on the editor; every child
   widget resolves through it). Take values from the theme, never
   literals.
3. **Processor capabilities** from the gap list above, unit-testable
   without the new visuals.
4. **Structural rebuild** of `resized()`/`paint()` into the 1a regions.
   Move `StemWaveformComponent` and `RecursiveStemRowComponent` into their
   own files first - they are self-contained and `PluginEditor.cpp` is
   large.
5. Verify against the four host modes; if resizability survives, check
   both compact axes (540x540, 540x1200, 1400x540) plus the default.
