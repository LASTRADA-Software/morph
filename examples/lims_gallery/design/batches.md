# LIMS gallery: framework batches

The 56 entries in `gaps.md` are grouped into five batches. Each batch is one coherent change to the declarative UI:
one design document, one review, then the implementation, all on this branch. The gallery screens drop their
workarounds for a batch once it has landed. The batches go in this order: A makes the gallery's compact layouts
real and fixes what is broken, and the batches after it add vocabulary.

## A: Defects and responsive layout — landed

Fix what the gallery found broken, and make the width class real. Most items are in the interpreter and the Qt
Quick frontend. Landed on this branch, and the screens no longer carry its workarounds.

| Gap | What |
|---|---|
| G1 (#1046) | Set `env.widthClass` from the window, in the client and the testkit; decide what the terminal reports |
| G5 | Touch-sized controls on compact |
| G7 (#1047) | The dialog's background follows the theme's surface |
| G12 (#1048) | `close: self` in a dialog closes the dialog |
| G19 (#1049) | A head row that doesn't fit the top bar neither overlaps nor becomes unreachable |
| G21 (#1050), G22 (#1051) | `progress` follows the accent, and reads an exact decimal |
| G25 (#1052) | `forEach` honours its common properties |
| G34 (#1054) | No polish loop in `keyValue` |
| G49 (#1055) | `splitter` honours `sizes` |
| G42 | Enter on a focused button reaches its key binding (reproduce first) |
| G48 | A read-only field looks read-only |
| G15, G40, G54 | A tag keeps its own width and height in a table cell; cells align vertically; a table wider than its pane scrolls |

## B: Tables

The LIMS workhorse: sample lists, worklists, results, audit trails and plates.

| Gap | What |
|---|---|
| G29 (#1053) | Bind and set a table's selection |
| G28, G31 | Paging and sorting over in-memory rows |
| G30 | A control in a column header |
| G46 | Group heading rows |
| G47, G2 (in cells) | Numeric columns aligned to the end; a monospace column role |
| G41, G43 | A selectable cell that keeps its own look; a click that knows its modifiers (plate wells, range select) |
| G44 | Grouping a list into runs |

## C: Text, fields and small controls

How a lab form reads.

| Gap | What |
|---|---|
| G2, G3 | Monospace text; inline emphasis |
| G6, G8, G10, G17 | A dialog's description; where the required mark sits; help text under an input; a checkbox group with a legend |
| G9, G11 | A unit beside a free input; a scanner field that stands out |
| G13 | A date and time display format |
| G14, G33, G53 | A secondary destructive button; an outlined tag; a segmented control |
| G24, G39, G52 | Label-over-value pairs; a key-value pair with a tone; a multi-line list entry |

## D: Layout, overlays and navigation

How screens compose and move.

| Gap | What |
|---|---|
| G4, G16 | An overlay that becomes a sheet on compact; a bottom action bar that stays in view |
| G18, G26, G27 | An overflow menu; toolbar controls in the head; removable chips |
| G20, G51 | Choosing a tab from a command, tab overflow, tab labels that follow the data |
| G23, G32 | Grid columns of different widths; `surface` that follows the data |
| G35, G38 | Naming an element inside a `switch` case; a wizard over screen state |
| G50, G56 | `navigate` as a dialog that returns a result; a screen's own navigation |

## E: Expressions and charts

| Gap | What |
|---|---|
| G36, G37, G45 | A field of a computed value; date arithmetic; square root (SD, RSD) |
| (review) | Pharmacopeial half-up rounding, alongside half-even |
| G55 | A chart node (series, reference lines, a marked point), with a terminal fallback |
