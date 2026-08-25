# Handoff: StemLab plugin UI overhaul

## Overview
Redesign of StemLab, a stem-splitting plugin for REAPER. It replaces the current ad-hoc layout with a single compact panel: source row with the primary Separate action, six aligned stem lanes auditioned through one shared transport (solo/mute per lane, common playhead), and a footer with separation status/progress and the insert/save actions. Visual language is the "Nocturne" design system: dark blue-grey ground, Inter, one blurple accent used as lines, tints and glows.

## About the Design Files
The files in this bundle are **design references created in HTML** — prototypes showing intended look and behavior, not production code to copy directly. Recreate the design in the plugin's existing UI environment (ReaImGui/Dear ImGui, JUCE, iPlug2, LICE, web view — whatever StemLab already uses) with its established patterns. If a component maps poorly to the toolkit (e.g. glows), approximate: the tonal values and layout matter more than the exact shadow.

`StemLab Drafts.dc.html` renders three exploration drafts labeled 1a / 1b / 1c. **1a "Lanes" is the approved direction and the spec below.** 1b (pipeline rail) and 1c (card grid) are alternates kept for reference only.

## Fidelity
**High-fidelity.** Colors, spacing, sizes and copy in draft 1a are final intent. Recreate closely, mapping px to the toolkit's units.

## Screen: main panel (draft 1a)
Fixed-width reference: 880px wide panel; height fits content (~620px). Panel: background `#232532`, radius 14px, edge+shadow `0 0 0 1px #595d6c, 0 6px 18px rgba(0,0,0,0.55)`, padding 20px 22px, vertical stack with 14px gaps. Page/window ground behind it: `#161826`.

### 1. Header row
- Brand glyph: 20px waveform-bars icon (5 vertical rounded bars) in accent `#9184d9`.
- Title "StemLab", 17px, weight 500, letter-spacing -0.015em, color `#e9e9ed`.
- Right: settings icon button 32×32, radius 8, 1px border `rgba(233,233,237,0.16)`, sliders icon 15px. Hover: `rgba(233,233,237,0.07)` fill.

### 2. Source row
Recessed strip: background `#161826`, radius 8px, padding 10px 12px, horizontal flex, 12px gaps, all vertically centered.
- File block (flex:1): "mpbr1v4.wav" 13px w500; below it 11px at 50% text opacity: "00:46 · beat 0.000 · from selected item".
- Button "Use Selected Item": 13px, accent text + 1px accent border, radius 8, background accent @10% tint. Hover: accent @12-14%.
- Button "Record PC": neutral secondary (1px `rgba(233,233,237,0.16)` border), with a 10px accent dot icon.
- **Separate split control** (flex:1, min 260px, height 36px, 8px extra left margin): one rounded container, radius 8, 1px border `#b5abfc` (accent-400), background `#5d5294` (accent-700), outer glow `0 0 22px rgba(145,132,217,0.40)`. Two segments:
  - Left segment "Refine" (a toggle, on by default): background `#2b2741` (accent-900), text `#d2cefd` (accent-300) 11px, letter-spacing 0.02em, padding 0 14px 0 12px, right border `rgba(231,229,254,0.30)`; pill switch 22×12px, track `#968ae0` (accent-500), knob 8px `#f5f4ff` (accent-100) at the right (on). Hover: bg `#423a6a` (accent-800).
  - Right segment "Separate" (the primary action, fills the rest): transparent bg, text `#f5f4ff` (accent-100) 13.5px w500. Hover: `rgba(181,171,252,0.18)` fill.

### 3. Stem lanes (×6: Vocals, Drums, Bass, Guitar, Piano, Other)
Grid per row: `18px | 92px | 612px | 78px`, 12px column gap, 5px vertical padding, radius 6. Row hover: `rgba(233,233,237,0.03)` fill.
- Include checkbox 15×15, radius 4. Checked: accent fill `#9184d9`, dark check stroke `#161826`. Unchecked: transparent, 1.5px border `rgba(233,233,237,0.30)`.
- Stem name 13.5px w500.
- Waveform well: 612×40px, background `#161826`, radius 6, clipped. Waveform drawn as 2px vertical bars (~150 bars, rounded caps), unplayed portion `#595d6c` (neutral-700). Played portion (left of playhead) redrawn in accent `#9184d9`. Playhead: 1px vertical accent line at the current position with soft glow `1px 0 8px rgba(145,132,217,0.35)`, spanning all lanes at the same x (lanes share one clock).
- Controls: S and M buttons 22×22, radius 6, 10px label, 1px border `rgba(233,233,237,0.16)`, 45%-opacity text when inactive. Active mute (see Drums): bg `#3f424d` (neutral-800), text `#e4e7f5`, no border. Active solo: bg `#423a6a` (accent-800), text `#f5f4ff`.
- Recursive-split icon button 22×22: stacked-layers icon 14px (diamond top + two arcs), 45% text color; hover: accent color + accent @10% fill. Tooltip: "Split this stem further". Action: run separation again on this stem's audio.
- Muted lane (Drums in mock): waveform base drops to `#3f424d`, no played-accent overlay.
- Excluded lane (Other in mock): whole row at 45% opacity, checkbox unchecked.

### 4. Transport row (shared, below lanes)
- Play/pause: 34px circle, 1px accent border, accent glyph. Hover: accent @12% fill.
- Time "00:33 / 00:46", 12px tabular numerals, 75% text opacity.
- Scrubber (flex:1): 3px track `#3f424d`, radius 2; filled left portion accent. Clicking/dragging seeks; playhead in lanes follows.
- A/B segmented control: "Original | Stems", 12px, shared 1px `rgba(233,233,237,0.16)` border, radius 8. Active option: accent text + inset 1px accent ring. Toggles monitoring between the untouched source and the stem mix (respecting solo/mute).

### 5. Footer
Fading divider above (1px, `rgba(233,233,237,0.16)`, fades to transparent over 48px at each end). One row, 10px gaps:
- Left block (flex:1, 28px right margin), two stacked lines 6px apart:
  - Status 11.5px at 50% opacity with accent check icon: "Separated 6 stems in 03:12 · refinement on".
  - Progress row (visible while separating): 3px bar, track `#3f424d`, accent fill with glow `0 0 8px rgba(145,132,217,0.45)`; right label 11px tabular 45% opacity: "34% · ETA 05:12".
- Folder icon 14px + path "~/Stems/mpbr1v4/" 12px at 50% opacity + "Change" ghost button (accent text, no border).
- "Save Stems": secondary button 13px.
- "Insert Stems": filled primary — bg `#5d5294`, 1px border `#b5abfc`, text `#f5f4ff`, glow `0 0 22px rgba(145,132,217,0.40)`. Hover: bg `#796cbf` (accent-600).

## Interactions & Behavior
- One shared clock: all lane waveforms are time-aligned; the playhead crosses them in sync with the transport.
- Solo: exclusive-ish (multiple solos allowed, non-soloed lanes silent). Mute silences a lane; its played-portion accent disappears.
- Include checkboxes select stems for Insert/Save; the Insert button may show the count (earlier iteration used "Insert 5 Stems").
- Separate: runs separation on the source item; while running, show the footer progress bar + ETA and keep already-finished stems auditionable if the backend supports it.
- Refine toggle: enables the refinement pass; part of the Separate control so the mode travels with the action.
- Per-stem split (layers icon): recursive separation of one stem.
- Record PC: existing record-from-system-audio feature; the accent dot doubles as the recording indicator (pulse while armed/recording).
- Insert disabled (45% opacity) until at least one stem is ready and selected.
- Hover states everywhere per above; keyboard focus: 2px accent outline, offset 2px. Disabled: 45% opacity.

## State Management
- `source`: {name, path, duration, beatOffset} or none.
- `separation`: idle | running {pct, elapsed, eta} | done {count, duration, refinementUsed}.
- Per stem: {included: bool, solo: bool, mute: bool, ready: bool, audioPath}.
- Transport: {playing, positionSec, monitor: original|stems}.
- `refine`: bool (persisted setting).
- Output folder path (persisted).

## Design Tokens (Nocturne)
- Ground `#161826`; surface `#232532`; text `#e9e9ed`; divider `rgba(233,233,237,0.16)`.
- Accent `#9184d9`; ramp: 100 `#f5f4ff`, 200 `#e7e5fe`, 300 `#d2cefd`, 400 `#b5abfc`, 500 `#968ae0`, 600 `#796cbf`, 700 `#5d5294`, 800 `#423a6a`, 900 `#2b2741`.
- Neutral ramp: 100 `#f3f5fe`, 200 `#e4e7f5`, 300 `#cfd3e5`, 400 `#b2b6ca`, 500 `#9397ab`, 600 `#75798c`, 700 `#595d6c`, 800 `#3f424d`, 900 `#292b31`.
- Type: Inter (400/500); headings never bolder than 500. Sizes used: 17, 13.5, 13, 12.5, 12, 11.5, 11, 10.
- Radii: 4 (checkboxes), 6 (wells, small buttons), 8 (buttons, strips), 14 (panel).
- Shadows: sm `0 0 0 1px #3f424d`; md `0 0 0 1px #595d6c, 0 6px 18px rgba(0,0,0,0.55)`; accent glow `0 0 22px rgba(145,132,217,0.40)`.
- Muted text = text color at 40–75% opacity, per component above.
- Full token sheet in `styles.css` (CSS variables).

## Assets
- Icons: Phosphor set (phosphoricons.com) or equivalent strokes drawn in-toolkit — waveform-bars (brand), sliders (settings), record dot, play/pause, folder, check, stacked layers (per-stem split).
- Waveforms: render from the real audio peaks (the HTML uses generated placeholder data).
- Fonts: Inter 400/500 (Google Fonts or bundled).

## Screenshots
- `screenshots/1a-lanes-spec.png` — the approved design (spec).

## Files
- `StemLab Drafts.dc.html` — the interactive design reference (open in a browser; draft 1a at top is the spec; 1b/1c are alternates). Requires `support.js` and `styles.css` beside it.
- `styles.css` — Nocturne token sheet + component classes (source of all values above).
- `support.js` — runtime for the HTML reference; not part of the design.
