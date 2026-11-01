# LIMS gallery batch O: table columns that start from their content, a badge the terminal raises, a panel's rounded fill, and the e-signature summary's border

Batch O closes the framework gaps in the after-N findings (`lg-nr-findings-A.md`, `lg-nr-findings-B.md`) and the two
defects found while designing it (morph#1075, morph#1076). The screen fixes and regressions in those findings belong
to lane O-S (`lg-os-brief.md`), which runs in parallel on `examples/lims_gallery`. This design lists the screen work
that follows from the framework items.

The rules of batches G to N hold:
- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- the terminal keeps its cells, and a purely visual change says what it leaves unchanged there;
- vocal over silent.

The gallery is judged on the 1215×830 grab, where the page is about 1000 px, as the mockups' window is. On the
1000×830 grab (a page of about 785 px), nothing may clip, overlap or lose content, except where a column is told it
may elide.

Every claim was checked against `lg-fold-n` at `227b4f06b` by reading the code in `/home/yaraslau/repo/morph-lims-e1`.
The grabs are in `lg-cmp/app-n/`; the mockup renders are in `lg-cmp/mock/`. Each premise says whether it was
**measured** (with PIL, on those grabs) or **read** (inferred from the code). Nothing was built or run for this design.

An independent review (`lg-o-review.md`) found 5 blockers and 12 should-fixes. This revision takes all of them, with
the coordinator's choices; see "Review round 2".

## Probes

- **P1. Review's worksheet at 1215** (measured on `review-1215x830.png`; the column starts are the header ink less
  the 8 px inset, as cross-checked by the review).
  - **The table.** It runs from x 517 to 1198, 681 px. Sample (`fixed(11)` mono) is about 87 px, Result
    (`content()`) about 75 and Prior lot (`content()`) about 82, so `L`, the width left for the stretch columns, is
    about 430 px.
  - **The stretch columns** share it 71 : 71 : 215 : 71, which is 1 : 1 : 3 : 1. Analyte, Spec., Evaluation (a tag
    cell) and Change all have `wrap: false` and `shrink`.
  - **Their natural widths**, content plus the 16 px inset:
    - Analyte about 91 ("Assay, mean": ink 73 px, plus a pixel or two of advance);
    - Spec. 80 ("95.0–105.0");
    - Evaluation 204 (the amber tag's fill is 188 px, x 920–1107);
    - Change about 221 (estimated ±10 from the elided run; the full note is never drawn).
  - **The header widths** (`h`, at least 8 units, about 61.7 px): "Evaluation" about 72 px; the others are at the
    8-unit floor.
  - A unit is about 7.7 px: samples' `fixed(7)` Priority measures 70 = 7u + 16, and `fixed(10)` Status measures
    93 = 10u + 16.
- **P2. The same table at 1920** (measured): Evaluation is 565 px and Change 189 px, which is 3 : 1. The width goes by
  the weights alone: the tag leaves about 370 px of Evaluation empty, while Change elides "…integration co…".
- **P3. The mockup's table** (read: `09-Review.dc.html`). It is CSS's automatic layout, `width:100%`, with
  `td{white-space:nowrap}`, in a card with `overflow:hidden`. Every column takes its content's width, and the card
  cuts the last column ("Ch", "0.").
- **P4. How columns are sized today** (read).
  - **Qt Quick** (`MorphTable.columnWidths`, `:89-150`):
    - fixed and content columns are placed first;
    - the stretch columns share what is left by weight alone, each floored at its header, or at 8 units with
      `shrink`;
    - the shares sum to that width exactly.
    - `reportCellWidth` (`:215-226`) stores a width for every column. Only content columns read it, and
      `seedMissingColumns` and `invalidateAllColumns` scan only content columns.
  - **The terminal** (`TableImpl::columnWidths`, `table_widget.cpp:234-249`): a stretch item starts at 0 and takes
    its weighted share; it is raised to its header (or `min(h, 4)` with `shrink`); then `shrinkFromLast` takes any
    deficit from every column, last first.
  - **Both** ignore a stretch column's content.
- **P5. Placement is synchronous, and it re-enters** (read).
  - **The chain.** `MorphTableRow.place()` sets `cell.width` (`:92`, `:96`). A cell whose `implicitWidth` then
    changes calls `place` again through a `DirectConnection` (`table.cpp:348-353`). It also calls `reportCellWidth`
    (`:366-373`), which reassigns `columnCache`; that re-evaluates `widths`, and `onWidthsChanged: place()` runs
    nested inside the first `place`.
  - **The damage.** The outer frame then resumes with stale locals (`const w`, `x += w`) and writes
    `cells.implicitWidth = x` last.
  - **Why it is harmless today, and stops being so.** A content column's report is a fixed point, and a stretch
    column's report changes nothing. Under O1 it does.
  - **Cells whose `implicitWidth` follows their width:**
    - a nested `MorphTable`, whose `implicitWidth` is its `columnsWidth(width)` (`MorphTable.qml:326`), through its
      header `Row`;
    - any stack that holds one;
    - a `MorphText` with **runs** that wraps: its `implicitWidth` is `label.implicitWidth` (`MorphText.qml:52-56`),
      which "follows the width it is given" on Qt 6.8 (`:45-47`). CI runs Qt 6.8.1; this machine runs 6.12.

    A plain text, wrapping or not, already reads font advances.
- **P6. The terminal never word-wraps** (read: `leaf_widgets.cpp:146-185`; `paintRuns` honours only line feeds,
  `widget.cpp:895-925`). A plain text in a `wrap: true` column that is too narrow is cut at the column's edge without
  an ellipsis (`tui/frontend.md:95`). Only `wrap: false` gets one.
- **P7. The e-signature summary** (measured). Its border is (221,225,230), `#DDE1E6`, while the mockup draws
  `#E6E9ED` around a `#F6F7F9` fill with `border-radius: 6px`, in the dialog and in the compact sheet. It is the only
  framed box inside a dialog or a drawer across the ten mockups.
- **P8. Per-surface borders exist** (read). `color.border` is a scheme role resolved per surface. The gallery already
  sets it on its `pane` and `selected` surfaces. Aliases resolve across layers.
- **P9. A hidden cell on the terminal** (read). `TableImpl::naturalWidths` (`table_widget.cpp:220-229`) measures every
  cell of a shown row, hidden ones included. Qt skips them (`MorphTable.qml:191`).
- **P10. A panel's square fill** (measured; morph#1076). On `esignature-1215x830.png`, pixel (379,238) is
  (246,247,249), the box's fill, *outside* the rounded border, whose arc runs through x 382–385 at that row.
  - The frame (`MorphPanel.qml:50-55`) has `radius: Theme.radiusCard` and a transparent fill.
  - The fill is the widget's surface rectangle (`MorphWidget.qml:470-481`), whose `surfaceRadius` (`:81`) is 0 for
    every surface but `card`.
- **P11. The terminal's badge in a stack** (read; morph#1075).
  - `requestedSize` gives a `fixed` child its amount (`widget.cpp:613-623`), and `distribute` reads only the
    document's bounds (`:701-708`). So console's `fixed(9)` tag "[Out of spec]" (13 cells,
    `display_widgets.cpp:261-263`) is drawn in 9 and cut by the canvas.
  - The contract says a badge's minimum is its natural width and that a stack holding one is raised with it
    (`backend_contract.md:278`, `:282`), and Qt Quick does that. It also says a percent child on the terminal is
    bounded "by its bounds alone" (`:278`).
  - The canvas clipping is read, not run.

## The items

| Item | Verdict | Where it lands | Lane |
|---|---|---|---|
| O1: stretch columns share by weight alone | **Measured** (P1, P2); the cause is **read** (P4) | the table rule in the contract: a stretch column starts from its content, shares only the spare by weight, and gives in a fixed order when short; Qt's placement made re-entry-safe; a table natural width that does not follow its own width | O-F |
| O1b: a hidden cell widens its column on the terminal | **Read** (P9) | skip hidden cells. Smaller than an issue, so not filed | O-F |
| O4: the terminal cuts a badge narrower than its natural width (morph#1075) | **Read** (P11) | the terminal raises a badge, and a stack or panel that holds one, to its natural width, as Qt does; the contract's percent sentence is fixed | O-F |
| O5: a panel's fill is square under its rounded frame (morph#1076) | **Measured** (P10) | a panel's surface fill follows its frame's radius, for every surface | O-F |
| O2: the e-signature summary's border | **Measured** (P7); the fix is **read** (P8) | no framework change: a gallery surface whose `color.border` aliases `color.dialogRule` | O-S |

## O1: a stretch column starts from its content

### Problem

P1 and P2. At 1920, Evaluation keeps about 370 px empty while Change elides. At 1215, Analyte and Spec. elide while
Evaluation has room to spare. The mockups' tables are CSS automatic tables (P3). No screen choice fixes both widths.

### Options considered

1. **A new sizing kind** (`auto`). Rejected: about thirty sites (batch N's `percent`), for a rule the default should
   already be.
2. **Proportional shrinking**, CSS's rule between min- and max-content. Rejected for one-line columns: at P1 it takes
   about 47 % of every excess and elides Analyte to about 76 px. It is kept for columns whose cells wrap (step 2).
3. **Shrinking from the last column alone.** Rejected: a long wrapping column early in a table keeps its one-line
   width while later ones go to their headers (samples' Product and Tests).
4. **Chosen: CSS's automatic layout, with ui/1's elision in place of overflow.** A column has three widths (natural,
   least, floor) and the rule has four steps, the same on both frontends.

### Decision

**This becomes the default for every stretch column in a table.**
- **What it changes.** A table with one stretch column is unchanged: step 1 gives it `L`, and steps 2 to 4 give
  `max(L, f)`, as today. Only tables with two or more stretch columns move. In the tree those are review, samples and
  stability's test table; no other example has one (read, `interpreter.hpp:9206`: an absent width is `content()`).
- **What stays as it is.** Grid tracks and stacks keep their rules.
- **Versioning** (`VERSIONING.md:80-90`). This contradicts `backend_contract.md:269`: "a stretch column takes what
  the others leave". morph is 0.1.0, where that is allowed. The spec change records it in `backend_contract.md`'s
  Table cells paragraph as a deliberate behaviour change at 0.x, and the commit says so.
- **The capability dropped.** Columns of a set proportion whatever their content (equal lot A / lot B / lot C
  columns that scale with the table) can no longer be said. `fixed` is absolute units, and a literal `percent` table
  column stays refused (`document.hpp:4102-4108`). This is dropped deliberately. A later batch can allow `percent`
  columns when a screen needs them, and `document.md` says so.

**The widths of a stretch column.** All of them include the cell inset on Qt Quick, as every column width does.

| Width | Symbol | What it is |
|---|---|---|
| Header width | `h` | As today: the label, the sort mark's room and the inset; at least 8 units on Qt Quick |
| Natural | `n` | The larger of `h` and the widest shown cell's natural width. Each cell's natural width is read from `widthFreeNatural` (below) when the cell gives one, else from `implicitWidth` |
| Least | `l` | In a `wrap: false` column, `n`. In a `wrap: true` column, the larger of `h` and the widest shown cell's least width. On Qt Quick a cell's least width is `shrinkWidth >= 0 ? min(shrinkWidth, natural) : natural`, with `shrinkWidth` defined as "the narrowest the widget can be laid out without losing content" (`MorphWidget.qml:46-49`); `MorphText` binds it to its widest word and `MorphStack` aggregates it. On the terminal see S3, below. It is clamped so that `h ≤ l ≤ n` |
| Floor | `f` | As today: `h`, or with `shrink`, 8 units on Qt Quick and `min(h, 4)` cells on the terminal |

The table decides between least and natural from the column's `wrap`, which it knows before it places anything. It
never reads the cell's `oneLine`, which `place()` sets late.

**The rule.** `L` is the width left after the fixed and content columns, the lead or the gutter and, on the terminal,
the gaps.
1. **Everything fits (`Σn ≤ L`).** Each stretch column is `n` plus its share of `L − Σn` by weight. A weight now
   shares the spare only.
2. **Wrapping columns give (`Σl ≤ L < Σn`).** Each column is `l + (n − l) × (L − Σl) / (Σn − Σl)`. Only a column
   with `n > l` narrows, and weights take no part.
3. **Eliding columns give (`Σf ≤ L < Σl`).** Each column starts at `l`, and the deficit is taken:
   - first from the columns with `shrink`, from the last towards the first, each down to its `f`;
   - then from the other stretch columns, in the same order.
4. **Short of the floors (`L < Σf`).** Every stretch column is at `f`, and what happens next is as today: Qt Quick
   scrolls sideways once the columns are 1 px or more wider than the table; the terminal runs `shrinkFromLast` over
   every column, then cuts at the edge. That difference between the frontends predates this batch, and it stays.

**What the steps guarantee.**
- They meet without a jump, so every column's width is non-decreasing in the table's width.
- In steps 1 to 3 the stretch columns sum to `L` exactly:
  - on Qt Quick, the last column a step changes takes `L` less the others;
  - on the terminal, step 1 uses `shareSpare`, step 2 rounds down and hands out the remainder one cell at a time from
    the first column with `n > l`, and step 3 is in whole cells already.

**`shrink`, two meanings.** It already lets a column go under its header. It now also makes the column give before
the other stretch columns. A column that may go under its header but should keep its content late has no spelling.
`document.md` says so; no finding needs one.

**Which rows count:** the shown, built rows. Heading rows and hidden cells are skipped (O1b brings the terminal into
line), and a paged table reads its page.
- **A virtualized Qt table** reads its realized rows. Both caches only grow, as content columns do (`rescanColumn`'s
  monotonic rule). A wider row that scrolls in widens its column once, and it moves the other stretch columns once,
  since the spare falls or the deficit grows. A row that scrolls out moves nothing.
- **The terminal** does not virtualize.
- **Widths now follow the shown rows** (S5). A filter, a page or review's All/Flagged toggle can move stretch
  columns, as it already moves content columns and as a CSS table does.

**No column bounds and no user widths.** A table column has no `minWidth` or `maxWidth`, and this batch adds none.
ui/1 has no user column width: there is no column drag, and the `columns` preference point's editor is not lowered.
When one lands, a width the user set is a fixed column. `document.md` says so.

### Qt Quick: placement and natural widths that do not loop

- **`place()` is re-entry-safe** (B1). `MorphTableRow` gains `placing` and `placeAgain`, both private.
  - A call made while `placing` is set only sets `placeAgain` and returns.
  - The outer call loops while `placeAgain` is set, reading `widths` afresh on every pass, at most 3 times in one
    call stack. If `placeAgain` is still set after that, it ends with `Qt.callLater(place)`, so a further pass is a
    real event-loop pass. No stale local survives a pass, and the call depth is bounded.
  - A test-only counter, `placePasses`, counts the passes, as `rowScans` counts rescans.
  - This was chosen over queuing the width report: it is smaller (one function, one file), and it also covers the
    re-entry that exists today through `implicitHeightChanged` and `implicitWidthChanged` → `place`. A queued report
    would leave that re-entry in place.
- **A natural width that does not follow the widget's own width** (B2, B3).
  - **The property.** `MorphWidget.widthFreeNatural`, default −1, means unknown. The table's report prefers it over
    `implicitWidth`.
  - **Who sets it:**
    - `MorphText` sets it unconditionally. It is computed from font advances: for a plain text and for runs on one
      line it is today's `implicitWidth`, and for **runs that wrap** it is the widest of the runs' lines (the runs'
      text split at line feeds, which a run's text can contain, `plainOf`), measured with `runsAdvance`, plus
      `wrapSlack` and a required text's mark. There is no probe: the follower appears on CI's Qt 6.8.1, not on this
      machine's 6.12.
    - `MorphTable` sets it as its **natural width**: the lead plus each column's natural width (a fixed column's
      amount, a content column's cache, a stretch column's `n`), computed from the caches and the headers, never from
      `width`. `MorphTable.implicitWidth` is unchanged.
    - `MorphStack` and `MorphPanel` pass it on. While any shown child gives one, a stack computes its own by hand along
      its axis: in a row, the sum of each child's `widthFreeNatural`, or its `implicitWidth` where it gives none, plus
      gaps and insets; in a column, the largest. A panel adds its padding and border. Otherwise both give −1.
  - **The cache** is written in whole pixels (`Math.ceil`), as defence in depth. It is not the argument.
- **The caches.**
  - `columnCache` keeps the natural width of every content and stretch column.
  - `leastCache` keeps the least width of every stretch column.
  - Both are written outside the `widths` binding, so no QML binding loop can form.
  - `seedMissingColumns` and `invalidateAllColumns` scan stretch columns too.
- **The report.**
  - `TableRowImpl::adopted` reports the cell itself (`reportCell(index, cell)`).
  - It connects `implicitWidthChanged`, `shrinkWidthChanged` and, where the cell declares it,
    `widthFreeNaturalChanged`.
  - The table reads both widths from the cell, whatever the column's kind. A content column of wrapping runs therefore
    loses the same echo.
- **`columnWidths(total)`** implements steps 1 to 4.
- **`qt_quick/backend.md`** states the re-entry rule, the property and both caches.

### Terminal

- **`layout::planColumns`.**
  - Its signature is `planColumns(std::span<ColumnItem const>, Track) -> std::vector<int>`, in `src/tui/layout.*`,
    with `ColumnItem{sizing, natural, least, floor, minimum, shrinks}`.
  - Fixed and content items ask for what `requestOf` gives them.
  - Steps 1 to 3 run over the stretch items. Step 4 is today's `shrinkFromLast` over every item down to `minimum`.
  - `distribute` is unchanged.
- **`TableImpl::columnWidths`** builds the items from:
  - `naturalWidths()`, which skips hidden cells (O1b);
  - a new `leastWidths(columns)`. For a **direct text cell** in a `wrap: true` column it gives the cell's
    `TextImpl::leastWidth()`: the widest whitespace-separated word of its text or of its runs' text, plus the required
    mark. For every other cell it gives `requestedSize(cell).width`, that is, its natural width (S3, below).
- **S3: a column that gives way must not lose text silently.**
  - The terminal cannot wrap (P6), so in a table it now draws an ellipsis in the last cell of every line of a text
    that its column cuts, whatever the column's `wrap`.
  - `arrangeRows` calls `setElided(true)` on every direct text cell, and the plain-text path already elides each line
    (`elidedLine` per line).
  - The runs path gains a multi-line elision: `elidedRuns` applied to each line that line feeds separate, rather than
    joining the lines as a `wrap: false` column does.
  - A text inside a container cell is not a direct cell, so its least width is its natural width. The terminal
    therefore never narrows a column below what such a text needs, and it never cuts one silently.
  - This is the one documented width difference: Qt Quick narrows a container of wrapping text, through
    `shrinkWidth`, where the terminal does not. `tui/frontend.md` says so.

### API surface

- **No new public type, key or method.** `TableColumn::width` and `shrink` keep their types.
- **Doxygen:**
  - `view.hpp:1178` (width) and `:1196-1200` (shrink);
  - `author.hpp:4458` and `:4478-4480`.
- **Internal:**
  - `MorphTable.qml`, `MorphTableRow.qml`, `MorphText.qml`, `MorphWidget.qml`, `MorphStack.qml`, `MorphPanel.qml`
    and `src/qt_quick/table.cpp`;
  - `src/tui/layout.*`, `table_widget.*` and `leaf_widgets.*` (`leastWidth`, the multi-line runs elision).

### Spec

- **`backend_contract.md`, Table cells:**
  - the three widths and the four steps;
  - "a hidden cell widens no column";
  - the 0.x behaviour-change note.
- **`document.md`, `table` columns:**
  - a stretch width starts from content, and its weight shares the spare;
  - `shrink`'s two meanings;
  - no content-blind proportions (and `percent` stays refused);
  - a future user width is a fixed column.
- **`view_tree.md`** and **`author.md`:** `TableColumn`.
- **`qt_quick/backend.md`, Table:**
  - the steps and the caches (monotonic when virtualized);
  - `widthFreeNatural` and the table's natural width;
  - the re-entry rule;
  - the paragraphs at `:275` and `:343`.
- **`tui/frontend.md`, Table:**
  - the steps in cells;
  - least widths (direct text cells only);
  - every text a column cuts draws "…".

### Tests

**Conformance.**
- **Placement.** The cases go in `layoutConformanceCases()` (`backend_conformance.hpp:3415`) and run in both
  frontend runners. Each case reads geometry before any other check, so that `tests/test_ui_conformance.cpp:386-395`
  sees it fail on the recording probe with "reads no bounds".
- **Cells and measurement.**
  - Every cell is a **direct text cell**.
  - A column is measured from its cell's `x` to the next cell's `x`, or to the table's right edge for the last.
  - "Whole" means the cell's `textExtentOf` width is within a unit of a reference text's: the same string, content-
    sized, outside the table.
- **The numbers** are recomputed for Qt Quick at a unit of the average character width plus 2.1 units of inset per
  column, and for the terminal with a 2-cell gutter and 1-cell gaps.

| Case | Setup | Assertions | Control |
|---|---|---|---|
| `stretchColumnStartsFromItsContent` | `fixed(60)`; two `stretch(1)` columns, `wrap: false`, holding "a" and a 30-character text. Qt Quick: n = 8u and 32.1u, so 17.95u and 42.05u. Terminal: n = 1 and 30 in 57, so 14 and 43 | the second text is whole, and the second column is more than 5 units wider than the first | weights alone give equal columns (Qt 30/30, terminal 28/29), so it fails |
| `stretchDeficitFromShrinkLast` | `fixed(55)`; three `stretch(1)` columns, `wrap: false`, the last two with `shrink`, each holding a 20-character text. Qt Quick: Σn = 66.3u against 55u, so D = 11.3u and the third is 10.8u (floor 8u). Terminal: 60 in 51, so D = 9 and the third is 11 (floor 4) | the first two texts are whole and their columns are within a unit of each other; the third is at least 3 units narrower than the second | weights alone, or proportional shrinking, make the second and third equal |
| `wrappingColumnGivesFirst` | `fixed(40)`; column 1 `stretch(1)`, `wrap: true`, a 40-character text of 4-letter words; column 2 `stretch(1)`, `wrap: false`, 24 characters. Qt Quick: l1 = 8u, Σl = 34.1u ≤ 40u, so column 2 is 26.1u. Terminal: Σl = 4 + 24 = 28 ≤ 37, so column 1 is 13 and column 2 is 24 | column 2's text is whole; column 1 is narrower than its text's reference extent | without step 2, column 2 is last and gives first, and its text elides on both frontends |

**Qt Quick** (`tests/qt_quick/test_qt_quick_table.cpp`):
- **Review's shape at 691 px and at 1393 px** (the 1215 and 1920 tables after R1's `percent(27)`), with R1's columns
  and P1's cell widths, in both views (Flagged, and All with "Total impurities").
  - At 691, Analyte, Result, Spec. and Prior lot are whole in Flagged, the tag is not elided (`label.truncated` false)
    in both views, Change is at its floor, and the columns sum to the width exactly.
  - At 1393 nothing elides.
  - **Control:** today's rule elides the tag at 691 (Evaluation is about 120 px).
- **The sweep, a guard:** 200 to 1600 px in 1 px steps, with a fixed column, a content column and four stretch columns
  (one wrapping, one with `shrink`).
  - Each width is at least the previous width less 1e-6.
  - Wherever `Σf ≤ L`, the sum is exact and `hflick.interactive` is false.
  - **It also discriminates:**
    - at a width in step 1, every stretch column is at least its `n`;
    - at a width in step 2, the wrapping column is below its `n` and the one-line columns are at their `n`.

    Today's rule fails both.
- **No misaligned cells** (B1). A row holds four stretch columns. One cell's `implicitWidth` is bound, through a
  `QQmlExpression`, to `width < 100 ? width + 20 : width`: it changes when placed, so it re-enters `place` with a new
  `widths`.
  - After settling, every cell's `x` equals its header label's `x` less the inset, and `cells.implicitWidth` equals
    the sum of `widths` plus the lead.
  - No `Binding loop` message is logged, and `placePasses` is at most 3 per row in one call stack.
  - **Control:** without the guard, the outer frame's stale `x` misaligns the cells after the follower.
- **A nested table settles to the same layout** (B2).
  - A stretch column's cell is a `MorphTable` with two stretch columns, beside two one-line stretch columns.
  - Build it at 900 px, widen to 1400, narrow to 700, and wait until `placePasses` stops changing, at most 50
    event-loop turns.
  - The sibling columns' widths then equal those of the same table built fresh at 700 px, within 0.5 px.
  - **Control:** with the report reading `implicitWidth` instead of `widthFreeNatural`, the nested table keeps its
    widened width as its natural width, the siblings are narrower than the fresh build, and the case fails. This was
    worked: at 1400 step 1 hands the nested table the spare, and at 700 step 3 takes the deficit from the siblings
    first.
- **`widthFreeNatural` of runs that wrap** equals `runsAdvance` of the widest line, plus `wrapSlack`, whatever its
  width: set 400, then 120, and read it each time.
  - **Control:** reading `label.implicitWidth` gives a different value at 120 on Qt 6.8.
  - On Qt 6.12 the control may not fail, and the test says so; the computed-value assertion holds on both.
- **Virtualized.** In a table above its threshold, scrolling a wider stretch cell into view widens its column, and
  scrolling it back out does not narrow it. The least cache does the same in a wrapping column.
- **Existing Qt table tests** (S1). None changes:
  - `:175`, `:800`, `:857` and `:884` have one stretch column;
  - `:199` asserts only the sum and that nothing scrolls;
  - the `rowScans` tests use content columns.

  The commit says that none was edited.

**Terminal:**
- **`tests/tui/test_tui_layout.cpp`, `planColumns`:**

  | Items | Length | Result |
  |---|---|---|
  | n 10, 10, weights 1 and 3 | 30 (gap 0) | [13, 17] |
  | n 30 l 8; n 12 l 12 | 30 | [18, 12] |
  | n 12; n 12 shrink; n 12 shrink; floors 4 | 30 | [12, 12, 6] |
  | n 12 shrink; n 12 | 20 | [8, 12] |
  | floors over the length | — | the floors, then `shrinkFromLast` |

  - **A property test** over random items with `f ≤ l ≤ n` checks the sum, that every width is non-decreasing in the
    length, and that every width is at least its floor.
  - **Control:** with natural widths ignored, the first case gives [8, 22].
- **`tests/tui/test_tui_table.cpp`:**
  - review's shape in cells: Analyte and Spec. are whole, Change gives first;
  - a hidden 30-cell cell leaves its content column at its header (O1b; **control:** 30);
  - a `wrap: true` direct text cell narrowed by step 2 ends its line with "…" (S3; **control:** today's paint cuts
    it with no mark);
  - a container cell of wrapping text keeps its natural least width.
- **`tests/tui/test_tui_widgets.cpp`:**
  - `TextImpl::leastWidth` for plain text, for runs and for a required text;
  - multi-line runs elided line by line.

**Gallery** (O-S, after O-F lands; S2):
- **The end-to-end case.** It goes in `examples/lims_gallery/tests/test_lims_gallery_render.cpp`, which already
  mounts each screen offscreen and fails on any Qt warning, a `Binding loop` included. It mounts review expanded at a
  1215×830 window and finds the `results` table.
  - **In Flagged:** Analyte, Result, Spec. and Prior lot are not elided, the Evaluation tag's `label.truncated` is
    false, and `hflick.interactive` is false.
  - **In All**, after a click on "All 7": the tag is not truncated, and `hflick.interactive` is false.
  - **Control:** run against O-F with O1 reverted. Evaluation is about 120 px, the tag truncates, and the case
    fails. O-S does this once and reports it.
- **The sizing strings.** `test_review.cpp:49-52` changes with R1.

## O1b: a hidden cell widens no column on the terminal

- **Fix.** `TableImpl::naturalWidths` skips `!cell->shown()`.
- **Tests.** The test is listed under O1. **Control:** without the skip, the column is 30.

## O4: the terminal raises a badge to its natural width (morph#1075)

**Problem.** P11. The contract and Qt Quick raise a badge, and a stack that holds one, to the badge's natural width.
The terminal gives a `fixed` or `percent` badge its amount and cuts it, closing bracket included. The contract also
contradicts itself about percent children.

**Decision.** The terminal follows the documented Qt Quick reading.
- **A content minimum on the terminal.** `WidgetBase` gains `virtual int contentMinimumWidth() const`, 0 by default.
  - `BadgeImpl` returns its natural width.
  - `StackImpl` returns, along a row, the sum of its shown children's content minimums plus its gaps and its padding;
    across a column, the largest plus its padding.
  - `PanelImpl` returns its content's plus its border and padding.
  - A table, a scroll and every other leaf return 0, since they take their own overflow, as the contract says.
- **Where it applies.** In a stack's placement (`widget.cpp:701-708`) each item's `minimum` becomes
  `max(bounds.minimum, child->contentMinimumWidth())`. Along the axis that is `layout::Item::minimum`, which
  `distribute` raises to, `shrinkFromLast` excepted. Across, `crossExtent` raises the same way.
  - A stack whose minimums do not fit overflows and is cut, as the Bounds paragraph already says.
  - A table cell is unchanged: the table places cells itself, and a badge there elides (batch N).
- **The contract.** "A child sized by a `percent` is placed as a fixed child is: on both frontends a stack's or a
  panel's content minimum raises it; only a `stretch` sibling takes the spare width."
  - The terminal's "its bounds alone apply" is removed.
  - Qt Quick raising a fixed child to its content minimum is documented behaviour, so it is not reported.
- **Spec:**
  - `backend_contract.md`: Content that does not fit (`:273-284`), Bounds (`:314-317`);
  - `tui/frontend.md`: Stack and Badge.

**Tests.**
- **Conformance** (`layoutConformanceCases()`):
  - `badgeRaisedToItsNaturalWidth`: a `fixed(5)` badge "Out of spec" in a row reads its bounds first. Its width is
    within a unit of the same badge's width when content-sized, and the row's next child starts after it.
  - The same badge as a `percent(5)` child of a `fixed(60)` row.
  - **Control:** the terminal today gives 5 units and fails; Qt Quick already passes.
- **`tests/tui/test_tui_widgets.cpp`:**
  - a `fixed(9)` "[Out of spec]" in a row paints its closing `]` in its 13th cell;
  - a column holding it has a content minimum of 13;
  - a table cell badge still elides in a 9-cell column (batch N's test, unchanged).
- **The gallery.** On the terminal, console's tag column is raised to the tag. O-S's `fixed(10)` stays right for Qt
  Quick.

## O5: a panel's fill follows its frame's radius (morph#1076)

**Problem.** P10. A panel that names a theme-drawn surface draws its fill as a square rectangle under the rounded
frame, so the fill shows outside the arc at every corner. This affects every surface but `card`.

**Decision.**
- `MorphWidget` gains `property real frameRadius: -1`, and `surfaceRadius` becomes:
  - `round ? height / 2`;
  - otherwise `frameRadius >= 0 ? frameRadius`;
  - otherwise `surface === "card" ? Theme.radiusCard : 0`.
- `MorphPanel` sets `frameRadius: Theme.radiusCard`, which is its frame's radius.
- The fill and the frame then share one radius for every surface: `page`, `card` and declared ones.
- **The terminal** draws no fill and no radius, so nothing changes there.

**Spec:** `qt_quick/backend.md`, Panel ("a panel's surface fill takes its frame's radius").

**Tests** (`tests/qt_quick/test_qt_quick_widgets.cpp`).
- A panel naming `page`, padded, inside a white card, is grabbed with `grabToImage`.
- The pixel one in from the panel's corner on the diagonal (outside a 6 px arc) is the card's colour. The pixel at the
  panel's centre is the page colour.
- The same holds for a declared surface.
- **Control:** today's `surfaceRadius` of 0 puts the page colour at the corner.

## O2: the e-signature summary's border (lane O-S, no framework change)

**Decision.** O-S uses the existing per-surface border (P8), not a new rule.
- **The rule was rejected.** Under it, a panel in a dialog or a drawer would take `color.dialogRule`. That needs a new
  inherited flag (a popup's content is reparented to the overlay, so the panel cannot find its dialog by walking
  `parent`), and it widens a token's meaning on the strength of one box.
- **The verdict changes if** a second framed box inside a dialog or a drawer needs the rule's colour. The rule is then
  the design, per `AGENTS.md`'s "the same example defect in a second place".

**The work.** This is O-S's, now.
- `lims_gallery.cpp` declares a surface `recordWell` as
  `{"color": {"$type": "color", "surface": {"$value": "{color.background}"}, "border": {"$value": "{color.dialogRule}"}}}`.
  A declared surface's fill is its `color.surface`, so the fill keeps the page's `#F6F7F9`.
- `esignature.cpp:92` uses `.surface("recordWell")`.

**Acceptance.**
- At 1215 and 390 the border pixel is (230,233,237), the fill is unchanged, and the logs are empty.
- **After O5 lands,** the corner pixel outside the arc (P10's (379,238) at 1215) is the dialog's white.
- An alias that does not resolve is refused when the bundle loads.
- The terminal reads no theme, so it does not change.

## O3: the scan for other framework gaps

Findings A has one framework item, O2; findings B has one, O1. The one conditional item, console's 3 px input
offset, is not a framework gap:
- Qt Quick raises a `fixed(9)` badge to its content minimum, as documented, so `fixed(10)` is the screen fix and O-S
  has it.
- It exposed the terminal's contradiction, which became O4.

Also not counted:
- receipt's focus halo (a grab artifact, inferred);
- the console head's height (a theme value);
- every screen item.

## Screen work after O-F (lane O-S)

What O-S changes once O-F's O1 commit has landed:

| Item | Work |
|---|---|
| **R1 review's columns** | `review.cpp`'s worksheet columns: Sample `fixed(11)` mono; **Analyte** `stretch()`, `wrap: false`, **`shrink`**; Result `content()`; **Spec.** **`content()`**, `wrap: false`; Prior lot `content()`; **Evaluation** `stretch()`, `wrap: false`, **no `shrink`**; **Change** `stretch()`, `wrap: false`, **`shrink`**. Fix the column comments: each states why (Spec. is a limit and never elides; Analyte gives before the tag) |
| **R1 the queue** | `percent(27)` with max 37, in place of 28, up front. It is 270 px at 1215, 212 at 1000 and still 284 (the max) at 1920. Fix the comment |
| **R1 tests** | `test_review.cpp:49-52`'s sizing strings; the end-to-end case in `test_lims_gallery_render.cpp` (above), with its control run once against O1 reverted |
| **R2 re-grabs** | Review in **both views** at 1215, 1000 and 1920, and at 390. Samples at 1215 and 1000, once with a search filter applied (S5). Stability: its Test/Method table is on step 3 at 1000 (S2), but no mockup draws that state, so it is judged for breakage only at 1000 and is not compared with a mockup |
| **R3 the e-signature box** | O2, now; the corner check after O5 |
| **R4** | Spot-check audit at 1215: one stretch column, so unchanged by construction |

### Review's arithmetic (B5, S4)

Inferred from P1's widths, ±2 px except where the table says otherwise. With `percent(27)`, the table is 691 px at 1215
and 535 px at 1000. Spec. as `content()` takes 80 px, so the stretch columns have `L'` of about 360 px at 1215 and
about 204 px at 1000.

| View, width | Stretch naturals (Analyte, Eval., Change) | Deficit | Who gives | Result |
|---|---|---|---|---|
| Flagged, 1215 | 91, 204, 221 (Σ 516) | 156 | Change (shrink, last) gives 156 of its 159 | Change about 65 ("0.…"); **everything else whole, the tag included.** If Change is up to 10 px wider than estimated, Analyte gives the rest first, and the tag stays whole |
| All 7, 1215 ("Total impurities" makes Analyte 106) | 106, 204, 221 (Σ 531) | 171 | Change 159, then Analyte (shrink) 12 | **Tag whole**, with Analyte's 32 px of capacity to spare. Analyte is about 94: "Total impurities" (about 90 of ink in 78 of room) elides, and "Assay, prep 1" (75) just fits |
| Flagged, 1000 | Σ 516 | 312 | Change 159, Analyte 29, then Evaluation (not shrink) 124 | Σf = 61.7 + 72 + 61.7 = 195 ≤ 204: **no sideways scroll**, about 9 px of margin. Evaluation is about 80, so the tag reads "Out o…"; Analyte "Assay, …"; Spec. whole |
| All 7, 1000 | Σ 531 | 327 | Change 159, Analyte 44, Evaluation 124 | as Flagged: the tag elides (accepted at 1000 only) |
| either view, 1920 (the table about 1393) | — | none | step 1 | everything whole; the spare by weight, 1 : 1 : 1 |

**Can every column but Change be whole in All at 1215?** No, not with Change shown at its floor. Sample 87, Result
75, Spec. 80, Prior lot 82, Analyte 106, Evaluation 204 and Change at its 61.7 floor need about 696 px against 691. The
shortfall of about 5 px is inside the estimates' error, so it may also fit. The column split above puts that shortfall
on Analyte, never on the tag. Making Evaluation `shrink` instead would put it on the tag.

**O-S's acceptance.**
- **At 1215, in both views:** the tag is whole; Analyte, Spec., Result and Prior lot are whole in Flagged; in All,
  only Analyte's "Total impurities" may elide.
- **At 1000:** nothing scrolls sideways or clips. The tag and Analyte may elide; Spec. is whole.
- If 1000 scrolls (the 9 px margin fails), report it with the measured widths. Do not trade the 1215 result for it.

## Decisions

1. **O1 is the table's default rule, not a new kind.** CSS's automatic layout, with ui/1's elision in place of
   overflow.
   - It is recorded as a 0.x behaviour change.
   - Content-blind proportional columns are dropped deliberately.
   - Grids and stacks are unchanged.
2. **Proportional shrinking applies only to wrapping columns** (step 2). Order-based shrinking, `shrink` first,
   applies to one-line columns (step 3).
3. **`shrink` now carries the priority as well as the floor**, a double meaning stated in `document.md`. Review uses it
   to make Analyte give before the tag.
4. **Least width is `shrinkWidth` on Qt Quick, decided by the column's `wrap`** (B4). On the terminal it is the widest
   word for a direct text cell, and the natural width for anything else. The terminal elides every text that a table
   column cuts (S3). The one documented difference: a container of wrapping text narrows on Qt Quick, not on the
   terminal.
5. **Qt placement** (B1): a re-entry guard with a bounded synchronous loop and a `Qt.callLater` tail. It was preferred
   to a queued report because it is smaller and covers today's re-entry too.
6. **Natural widths that do not follow width** (B2, B3): `widthFreeNatural` on `MorphWidget`, set by `MorphText`
   unconditionally and by `MorphTable` (its natural width), and passed on by stacks and panels. `MorphTable.implicitWidth`
   is unchanged. Whole pixels are only defence in depth.
7. **Which rows count:** the shown, built rows. A virtualized Qt table's caches only grow. Stretch widths now follow
   filters, pages and toggles.
8. **O4: the terminal raises a badge, and a stack or panel that holds one, to its natural width**, as Qt Quick does.
   The contract's percent sentence is made true on both frontends. Qt Quick's raising stays unreported, because it is
   documented.
9. **O5: a panel's fill takes its frame's radius** for every surface.
10. **O2 needs no framework change.** It becomes a rule if a second case appears.
11. **Review** (B5, S4):
    - `percent(27)` up front;
    - Spec. `content()`;
    - Analyte and Change `shrink`, Evaluation without it.

    The tag is whole at 1215 in both views. At 1000 the tag may elide, which the coordinator accepted.

## Defects and records

- **Filed:**
  - morph#1075 (the terminal's badge): closed by O4;
  - morph#1076 (the panel's square fill): closed by O5.
- **O1, if it is tracked:** an enhancement, "A table's stretch columns share width by weight and ignore their
  content". Measured in P1 and P2.
- **Not filed:**
  - O1b and S3's terminal ellipsis: fixes smaller than an issue, made in O-F;
  - O2: the existing per-surface border expresses it.

## Lanes

The lanes are the coordinator's two Sonnet lanes from `227b4f06b`, in separate worktrees, with no shared file.
- **O-F** owns `include/morph`, `src`, `tests` and `docs/spec`; its items are O1, O1b, O4 and O5.
- **O-S** owns `examples/lims_gallery`, where it does the screen fixes, O2 now, and R1 to R4 after O-F.

Every build, test and tidy run goes under `flock $S/morph-build.lock`, with `-j10` at most.

### O-F, one commit per item, in this order

1. **O5:** `MorphWidget.qml`, `MorphPanel.qml`, the pixel test, the spec. It is independent, and it unblocks O2's
   corner check.
2. **O1b:** `table_widget.cpp`, its test and the spec sentence.
3. **O4:**
   - `widget.{hpp,cpp}` (`contentMinimumWidth`, stack placement), `display_widgets.*`, `container_widgets.*`;
   - the conformance case and the terminal tests;
   - `backend_contract.md` and `tui/frontend.md`.
4. **O1:**
   - **Qt Quick:** `MorphTableRow.qml` (the re-entry guard), `MorphWidget.qml` (`widthFreeNatural`), `MorphText.qml`,
     `MorphStack.qml`, `MorphPanel.qml`, `MorphTable.qml` (the caches, the natural width, the steps) and `table.cpp`
     (`reportCell`, the connections).
   - **Terminal:** `layout.*` (`planColumns`), `table_widget.*` (`leastWidths`, setting every direct text cell
     elided), `leaf_widgets.*` (`leastWidth`), `widget.cpp` (multi-line runs elision).
   - **Tests:** the conformance cases and the frontend tests.
   - **Docs:** the specs and the Doxygen.
5. **Hand-off.** Run `lims_gallery_tests`, `lims_gallery_render_smoke` and every example's suite, and hand any
   failure to O-S without fixing it. Grab review in both views and samples at 1215, and check that the logs have no
   line.

**Before reporting done,** O-F re-runs:
- every suite that holds a touched test file;
- all three conformance runners;
- the docs target (clang, `WARN_AS_ERROR`);
- clang-tidy 22.1.8 on the touched TUs;
- qmllint on Qt 6.8.1, if a build of it is present.

**One mutation per feature,** each reported as failing:
- natural widths ignored;
- step 2 skipped;
- the re-entry guard removed;
- the report reading `implicitWidth`;
- O1b's skip removed;
- O4's minimum removed;
- O5's `frameRadius` removed.

## Risks

- **Width changes.** Every table with two or more stretch columns changes width: review, samples and stability's test
  table. Widths also move with filters, pages and review's toggle (S5).
- **Review's margins are thin:**
  - Flagged at 1215: Change has about 3 px of capacity left before Analyte gives;
  - All at 1215: Analyte's "Total impurities" elides;
  - 1000: about 9 px before the table would scroll sideways.

  All of these are inferred from P1's widths.
- **`widthFreeNatural` reaches stacks and panels.** A stack's hand-computed width must match `layout.implicitWidth`
  wherever no child follows its width. A test asserts that equality on a stack of texts and badges.
- **The `Qt.callLater` tail.** A real follower settles over event-loop passes, so a grab may need a turn or two
  longer. The render smoke test already waits for idle.
- **O4 moves terminal layouts** wherever a badge sits in a stack narrower than its natural width. In the tree that is
  console's `fixed(9)`. Every TUI test with a badge in a narrow stack is re-run.
- **The terminal's step 4 still differs from Qt Quick's**, as before this batch.
- **Recurring CI traps:**
  - Doxygen under clang;
  - GCC 15 `-O3` (no braced `std::vector` arguments in hot test code);
  - MSVC `typename`;
  - Qt keywords as identifiers.

## Self-review (round 1)

1. `reportCellWidth` stores widths for every column; only the readers and the seeding were content-only.
2. The terminal never wraps (P6). This drove S3, and the least width now applies only to direct text cells.
3. Shrinking from the last column alone fails wrapping columns (samples), which is why step 2 exists.
4. Proportional shrinking was checked at P1's widths before it was rejected for one-line columns.
5. A table with one stretch column is unchanged; the review confirmed it on both frontends.
6. The `planColumns` examples were recomputed with `shareSpare`'s rounding, giving [13, 17].

## Review round 2

Every item of `lg-o-review.md` is taken, with the coordinator's choices.

| Item | Resolution |
|---|---|
| **B1** synchronous re-entry of `place()` leaves stale positions | `MorphTableRow.place()` gets a `placing`/`placeAgain` guard, reads `widths` afresh on each pass, runs at most 3 synchronous passes, then ends with `Qt.callLater(place)`, and counts `placePasses`. Chosen over a queued report: smaller, and it covers today's `implicitHeightChanged`/`implicitWidthChanged` re-entry too. The "no misaligned cells" test fails without the guard. `qt_quick/backend.md` states the rule (O1, Qt Quick) |
| **B2** echo settles on a history-dependent layout; the control cannot fail; wrong follower | The known follower is removed at its source: `MorphTable` gives its natural width, computed from its caches and headers and never from `width`, through `MorphWidget.widthFreeNatural`, which stacks and panels pass on. `implicitWidth` is unchanged. Whole pixels are only defence in depth. The settle test uses a nested `MorphTable` (900, then 1400, then 700 px) against a fresh build at 700. Its control (reading `implicitWidth`) was worked to fail. Passes are counted with `placePasses` |
| **B3** the probe runs on the wrong Qt | No probe. `MorphText` sets `widthFreeNatural` unconditionally, from advances. Its test asserts the computed value; the control's failure is expected on 6.8 only, and the test says so |
| **B4** container least widths; cases cannot pass | Qt Quick's least width is `shrinkWidth` (through containers), decided by the column's `wrap`, with `shrinkWidthChanged` connected. On the terminal it is the widest word for a direct text cell and natural otherwise (with S3). The conformance cases use direct text cells, measure columns by the next cell's `x`, and were recomputed on both frontends, `wrappingColumnGivesFirst` included |
| **B5** the review's arithmetic; the All view | The arithmetic is redone for both views at 1215, 1000 and 1920. `percent(27)` is taken up front. Spec. is `content()`. Analyte and Change are `shrink` and Evaluation is not, so the tag is whole at 1215 in both views and Analyte's "Total impurities" takes All's shortfall. The table states plainly that every column but Change cannot all be whole in All at 1215 (about 5 px short, within error). At 1000 the tag elides, which is accepted. The acceptance covers both views |
| **S1** wrong list of changed tests | Corrected: no existing Qt, terminal or conformance table test changes, and the commit says none was edited. The real change is `test_review.cpp:49-52` (O-S) |
| **S2** tests that pass if O1 does nothing | The sweep is labelled a guard and gains discriminating step-1 and step-2 assertions. The equality test for `naturalAdvance` is replaced by a computed-value test. O-S adds an end-to-end case to `test_lims_gallery_render.cpp`, with its control run against O1 reverted. Stability's step-3 table is judged for breakage only (no mockup draws it) |
| **S3** the terminal cuts silently the column that gives first | The terminal elides every text a table column cuts ("…" on each line, the runs too), and its least width is the widest word only for a direct text cell, so a container's text is never narrowed past what it needs. Stated in `tui/frontend.md` |
| **S4** a limit should not elide | Spec. is `content()`. At 1000 its margin is about 9 px before the table would scroll sideways, recorded as a risk |
| **S5** stretch widths move with the shown rows | Stated in O1 and in Risks. O-S grabs samples with a filter applied, and review in both views |
| **S6** default with conditions | The 0.x behaviour change is recorded in `backend_contract.md` and in the commit. The dropped content-blind proportions are stated (`percent` stays refused), and `shrink`'s double meaning is noted in `document.md` |
| **S7** conformance mechanics | The cases go in `layoutConformanceCases()` and read geometry first, so the recording probe fails them with "reads no bounds". The numbers are computed with 2.1u of inset (D = 11.3u on Qt Quick, 9 cells on the terminal) |
| **S8** virtualized growth moves siblings | Stated: one move per new maximum. The least cache is monotonic too, and the virtualized test covers it |
| **S9** sweep tolerance | Monotonic within 1e-6; the sum stays exact |
| **S10** panel's square fill (morph#1076) | Now **O5**, an O-F item: `frameRadius` on `MorphWidget`, set by `MorphPanel`, with a pixel test and a control. O2's acceptance adds the corner pixel after O5 lands, and notes that the terminal does not change |
| **S11** terminal badge (morph#1075) | Now **O4**, an O-F item: the terminal raises a badge, and a stack or panel holding one, to its natural width, per the documented Qt reading. The contract's percent sentence is fixed. Qt Quick's raising stays documented and unreported. Console names the reproducer, and its clipping is marked read |
| **S12** accuracy | The Doxygen lines are corrected (`view.hpp:1178`, `:1196-1200`; `author.hpp:4458`, `:4478-4480`). P5 scopes the text follower to runs. `widthFreeNatural` for runs splits at line feeds inside runs and keeps `wrapSlack` |
