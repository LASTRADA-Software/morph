# Visual review: the screens against their mockups

Every screen was grabbed on Qt Quick at 1000×830 and 390×844, and at narrower and shorter sizes where something
clipped (`morph_examples_client --app lims-gallery --screen lims-gallery.<id> --grab-size WxH --grab FILE`). Each
was compared with its mockup rendered in headless Firefox. Hover, open menus and tab switches can't be grabbed;
those findings come from the user running the app and are traced to the code.

The goal is that ui/1 produces each mockup's look itself. So a difference is fixed in the vocabulary or the
frontend, and in the screen only where the vocabulary already expresses the design.

Defects in the library are filed: #1069, #1070, #1071, #1072.

## Batch F: layout, theme and container defects

**Layout**

| # | Change | Screens | Evidence |
|---|---|---|---|
| F1 | A `switch` case keeps its content's stretch (#1069) | receipt, samples, stability, results, review (compact) | Cut at y≈390 at every window height; bottom bars float mid-screen; stability loses Back / Continue |
| F2 | A panel's content starts at its top (#1072) | console, any stretched panel | The worklist tab bar moves on every tab switch; 90 px gap above it |
| F3 | Popups stay inside the window and end-align to their trigger when they would overflow; themed popup surface (#1071) | instrument "⋯", any menu | Popup runs past the right edge; Basic-style frame |
| F4 | `row` can wrap; the screen head wraps into rows (title and badges, then actions) instead of clipping | instrument, receipt, audit, samples, plate | Actions lost at 600–1000 px; badges cut on compact ("Calibration due in 3") |
| F5 | The shell head's tools wrap only when out of room, and the shell's top bar doesn't push the screen head to a second row | every screen | 30–50 px lost; the title wraps on compact |
| F6 | Splitter panes can fill the page height and carry a surface and a pinned bar | review, console | Review's action bar can't sit at its pane's bottom |
| F7 | Container content gaps: `dialog` and `panel` bodies space their children; table cells have padding; lists can have row dividers | e-signature, receipt, results, audit, instrument | Fields touch; rows run together |

**Theme**

| # | Change | Screens | Evidence |
|---|---|---|---|
| F8 | Tone parts (`bg`, `border`, `dot`) load from a bundle, and unread members are reported (#1070) | every tone colour | Warn badge `#FFF4E5`, error banner `#FDECEA` are Qt defaults; the bundle sets `#FFF1D6`, `#FDE7E7` |
| F9 | A hover and track fill separate from `neutral`, and hover keeps text contrast | review queue and "Show", instrument progress | Hovered entries at 1.5:1; "All 7" at 1.46:1; dark progress track |
| F10 | Cards and controls are white on the page fill (a card surface), not the inherited page colour | samples, stability, plate, receipt | Panels and inputs `#F6F7F9`; buttons in tinted bars take the tint |
| F11 | A control border separate from the divider border | every checkbox and input | Unchecked checkboxes nearly vanish (`#DDE1E6`; designs use `#C3C9D2`) |
| F12 | A small type step (11–12 px) for tags, table headers, field captions and mono, and a display size for KPI numbers | every screen; console KPIs | Everything renders at body size and looks heavier than the mockups |
| F13 | Accent where the design uses it and not where it doesn't: ghost/link buttons in the accent, the active tab label in text, the selected item fill in `color.selection` | samples, stability, plate, audit, every tab bar | Ghost buttons black; tab labels green |
| F14 | A modal scrim token | every dialog | Dim too light |

## Batch G: component looks and text

| # | Change | Screens | Evidence |
|---|---|---|---|
| G1 | Badge as a tag: 4 px radius, 11 px/600, no border when filled; an outlined style; a chip look for removable badges | every screen | 24 px pills with borders in body font |
| G2 | `stepper` drawn as numbered circles with connectors and done/current/upcoming states; its footer goes to `pinned`'s bottom bar | stability | `✓ ● ○ ○` glyphs; the footer scrolls |
| G3 | `pinned` bars with their own surface and a separator | stability, samples, results, receipt | Footer sits on the grey page with no top border |
| G4 | Banner with radius 6 and its message in the tone colour | e-signature, receipt, results | Square corners, black text |
| G5 | Collapsible sections as framed cards with strong titles and 44 px rows | instrument, stability, plate, samples (compact) | Plain disclosure rows |
| G6 | Table marks as designed: the selected row gets an accent bar, an out-of-spec row a tint; the select column uses the themed checkbox | samples, results, review, console | Reversed marks; Qt default checkboxes |
| G7 | Table cells don't wrap mid-token (nowrap or elide per column), and `fixed(n)` widths fit monospace | samples, review, console, audit | "S-26-10417 / 0", "WS-2026-041 / 2" |
| G8 | Circular and dense tokens for the plate: a circle badge with a size, a dashed outline, a selection ring, a dense button | plate | Wells cut to "S…"; no coloured well markers on compact |
| G9 | Text with a role and a tone together; tones on spans; spans in the head subtitle | console, receipt, instrument | "Past due 7" small and regular; no mono breadcrumb |
| G10 | Compact tabs overflow into "More ▾" | instrument | "Audit trail" clipped |
| G11 | A monospace prominent input without an italic placeholder; right-aligned numbers in text inputs | receipt, console, results | |

## Screen fixes (the vocabulary already covers them)

- Monospace IDs: receipt (sample heading, session table, last rejection), instrument (serial, worksheet), audit (compact record line), samples (card ids).
- Units through `textInput`'s `unit`, not the caption (stability).
- Plate compact repeats "ELISA, host cell protein".
- Console compact: the card itself is the selectable item, as review's queue does; no extra "Open …" button.
- Results: the footer in `pinned`'s bottom bar; the change line as the dialog's `description`.
- Console KPI tiles stretch to equal height (`align: stretch` on the grid).
- Instrument: no visible "Booking elapsed" caption under the progress bar.
- Receipt: the lot line's spans read a null sample once every sample is handled, and the runtime reports the dropped run.
- The gallery theme's `neutral` is a fill (`#E7EAEE`), not the neutral tag's text colour.

Full per-screen tables, with colours, sizes and file references, were the review's working notes; the items above
are their union.
