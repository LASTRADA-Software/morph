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
| G12 | The select's dropdown drawn as the mockups' input and menu: padded field, themed open list with radius and shadow, hover on the theme's token, the current option marked with ✓ in the strong weight | every screen with a select (from the user running the app) | Value 4 px from the border; Basic-style list; current option italic |

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

## After batch F

Every screen was grabbed again once batch F landed and compared with the mockups and the earlier grabs. Batch F's
items show as fixed on every screen they target. Its regressions (a selected item's fill covering its content, a
wrapping row in a row breaking its text per character, the shell bar's lost gutter, heads wrapping with room to
spare, cell padding narrowing fixed columns, the console's page not filling) are fixed on the branch before batch G.

Added to batch G:

- G4 also covers a banner's action button, which still draws in the Basic style ("Go to the field", "Open run").
- G7 now also matters for columns batch F's padding narrowed ("% LC", "22 min overdue", "Impurity B", audit's
  "When").

Added screen fixes:

- E-signature compact: "Sign and approve" full width; no description; "Password incorrect." shown once.
- Stability compact: "Summary · first pull" closed by default.
- Plate: the to-place IDs in mono.
- Review: the worksheet pane on the pane surface; 16 px above the "Review comment" caption; compact card IDs in mono.
- Audit compact: row dividers between entries, a "Load 25 more" button, the record line in mono.
- Samples compact: the cards as one divided list.
- Console compact: Scan in the header.
- The gallery's `empty` well surface pairs muted text with fills it's measured against; its contrast reports are the
  mockup's colours and stay.

Not in any batch: an eyebrow line above a head's title (instrument's breadcrumb), the dialog title's 16 px size and a
rule above a dialog's footer (batch G owns both per its design), section labels in small grey capitals (audit).

## After batch G

A last comparison after batch G found two regressions (dialog actions at the left, fixed; review's tall row, which
was its change column wrapping off-screen, fixed in the screen) and medium differences, fixed in the framework (a
folded section is its 44 px header, a section header keeps its inset, tabs fit as many as fit before "More", a head
action shows its icon) and in the screens.

Left for later, each a framework addition rather than a fix:

- A pinned bar that bleeds to the window's edge on compact. The shell's page inset holds it 16 px in; a bar would
  need to cancel the inset it sits in, or a screen would need to opt out of the page inset.
- A square, glyph-sized button for pagers (audit, samples, console, results).
- A control inside a selectable entry, so samples' compact cards can carry their checkbox and the selection's
  accent bar together.
- Runs in a head's title, for plate's mono "PL-0193".
- An eyebrow line above a head's title, and section labels in small capitals.

## After batch I

All ten screens were re-grabbed at 1000×830 and 390×844 after batch I's items landed. The sidebar shows at 1000 px
(`expanded` begins at 108 units), a head's lead sits where its screen puts it, a head that is one line is centred,
a joined page continues the head's block with its rule under the whole block, a dialog's rules span the overlay, and
the icons that paint `currentColor` follow their control.

Closed and checked in the grabs:

- Receipt's session line and counts close the title's line; the principal ("Dr. A. Meyer") closes every expanded
  head; the session table's header band reaches its card's edges.
- Results' "In progress" sits at the right of the eyebrow's line; its compact head carries the sample as the title
  and the previous/next buttons join it.
- Console's compact head is dark with a white menu and scan icon and no rule; its alert's "Open run" is a framed
  button, stretched under the text on compact; the two bottom panes are one card.
- Plate's subtitle follows its title; its compact buttons sit in the head's white block, and the wells read "A1 ·
  Standard 1, 100 ng/mL" as one text.
- Review's phone worksheet carries its sheet line, title and tags in the head; its tabs scroll their own padding and
  the flagged rows run edge to edge.
- The pagers draw chevrons; a banner's link is the accent, not the default blue; the audit facets rail runs the
  page's full height.

Left, each environmental or a design limit rather than a gap:

- Console's and review's expanded tables are wider than their panes at 1000 px, as the mockups' own are; the fonts
  (Inter, JetBrains Mono) are not installed here, so character widths are wider than the designs'.
- A tab's page is its own scroll, so a nested `scroll` in a page does not fill it: review's pages pad themselves with
  a column instead.
- The `Show` control of review's compact results keeps a gap above it that the design does not have.
