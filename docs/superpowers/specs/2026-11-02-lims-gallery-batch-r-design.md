# LIMS gallery batch R: an input that marks its cut, a compact head whose end follows what ends it, a themed `actionBar` anywhere, a caption on its value's first baseline, and a grid whose columns follow its room

Batch R covers every finding the after-Q comparison left open (`lg-qr-findings-A.md`, `lg-qr-findings-B.md`). Each
finding becomes one of three things:
- a framework item (R1 to R5), for lane R-F;
- a screen item (T1 to T10), for lane R-S;
- an accepted difference (A12 to A14), with its reason. A1 to A11 stand.

The screen items are numbered T, so they do not clash with batch Q's S items. The review (`lg-r-review.md`: 2 blockers,
9 should-fixes) is taken in full; see "Review round 2" at the end. Its items are written RB1, RB2 and RS1 to RS9 there.

The rules of batches G to Q hold:
- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- the terminal keeps its cells, and a purely visual change says what it leaves unchanged there;
- vocal over silent.

The gallery is judged on the 1215×830 grab, where the page is about 1000 px wide. On the 1000×830 grab (a page of
about 785 px), nothing may clip, overlap or lose content, except where a column is told it may elide.

**Sources and evidence.**
- **Code.** Read on `lg-fold-q` (`git -C morph-lims-e2 show lg-fold-q:<path>`) and in the `morph-lims-e2` worktree on
  `lg-qs` (head `dcefd3d15`). The two differ only in lint fixes. `morph-lims-e1` was not opened.
- **How each premise is marked:**
  - **measured**: a PIL read of `lg-cmp/app-q/`, by the reviewers or by me, as stated;
  - **reproduced**: run here or by the review, with the output shown or named;
  - **read**: inferred from the code.
- **What was run.** Plain-QML probes and pixel scripts only, in `$S/lgr-exp/` (Qt 6.12.0, `QT_QPA_PLATFORM=offscreen`):
  `readonly.qml`, `unit.qml` and `measure-head.py`. The review's probes are in `$S/lgr-rev-r1/` (selection over the
  overlay) and `$S/lgr-rev-r4/` (baselines). Nothing in morph was built. Nothing ran on Qt 6.8.1.
- **Issues.**
  - R1's defect is filed as **#1082** (from `lg-r-issue-readonly.md`), closed by R1.
  - No title issue is written (`lg-r-issue-title.md`): the title does not overflow (see R2).
  - R2 to R5 are not filed: R2 and R4 are fixes smaller than an issue, and R3 and R5 are vocabulary, not defects.
  - None of these gets a `triage:` label.

## Probes

- **R1. A read-only input clips its value with no mark** (brief item 1).
  - **Reproduced** (plain QML, `readonly.out`). A read-only `TextField`, 150 px wide, 13 px text, 8 px side padding,
    value "Release, finished product":

    ```
    qml: field width 150 text room 134 contentWidth 145.984375 implicitWidth 120 clipped true
    qml: label truncated true label elide 1
    ```

    The field draws "Release, finished produ"; the `Label` beside it draws "Release, finished pr…". `TextInput` has
    no `elide`.
  - **Reproduced** (the review, `lgr-rev-r1/ctx.out`): an overlay that hides the field's text by colour alone draws
    under a selection that outlives the focus (a right-click's context menu takes the focus and keeps the selection),
    so two texts overlap. The design therefore hides the overlay while a selection exists.
  - **Read, Qt:** `MorphTextInput.qml` and `MorphDateTimeInput.qml` draw their value in a `TextField` with
    `autoScroll: activeFocus`; `MorphCheckbox.qml:42`, `MorphSelect.qml:86` and `MorphText`'s `fitted` elide.
  - **Read, terminal:** `TextInputImpl::paintText` falls through to `paintField`, and `DateTimeInputImpl::paintControl`
    calls `paintField` directly (`field_widgets.cpp:510`); both render `core::tui::InputField` cut at the canvas width.
    The field's marks (`*`, `!`, `~`) are drawn over the control's last cells afterwards (`widget.hpp:396`, `:491`).
  - **Measured** (reviewer A): receipt 1000 shows "Release, finished produc", and the log is empty.
  - **Read:** audit's commit `3fb45e873` widened the rail because the date inputs showed "00:0" at 1000, with no mark.
  - **Verdict:** a defect on both frontends, #1082.
- **R2. The phone title "runs into the margin"** (brief item 2).
  - **Measured** (`measure-head.out`, review 390; confirmed by the review): the menu glyph's ink is 20–31, so the 44 px
    square starts at x ≈ 4; the title's ink is 57–382; the tag row's badge boxes span 56–376.
  - **Read:** compact, `_topLine` pads its ends by step 1 (4 px), so the line ends at 386 and the title fits it on one
    line. Nothing overflowed, and nothing should have been reported.
  - What the reviewer saw is the compact bar's end pad. `showTrail` adds the step-3 inset only after a framed trail, so
    any line that ends in text stands 4 px from the window edge, against the page's 16 px.
  - **Read:** every phone head in the mockups pads its content 16 px at both ends, except console's dark app bar, which
    ends in a ghost icon button that carries its own padding (its glyph ends about 22 px from the edge, measured by the
    review).
  - **Verdict:** mis-framed as an overflow. What is real is the trailing inset, which must follow what ends the line.
- **R3. Gaps between push buttons** (brief item 3).
  - **Read:** an overlay's `ActionBar` (`mount.hpp:1953`, `spacingStep(2)`) and `MorphFooter` (`Theme.spaceOf(2)`) are
    step 2; `stepperParts`'s footer is step 1 (`author.hpp:5145`), which a screen cannot reach.
  - **Read:** the mockups space every button pair 8 px apart, the stepper footer in both layouts included
    (`06-Stability.dc.html:89`, `:117`).
  - **Read:** `ActionBar` is a view-tree kind only: the interpreter makes it from an overlay's `actions`, ui/1 has no
    spelling for it, and outside an overlay the mount throws (`mount.hpp:1951`), so no valid tree mounts one there.
  - **Read:** the theme already has use-named tokens where 0 keeps a spacing step (`head.paddingY`, `theme.hpp:1293`),
    and `table.cellPaddingX/Y` and `input.paddingX`.
- **R4. A key-value caption is centred on a multi-line value** (brief item 4).
  - **Read:** in `MorphKeyValue.qml`'s inline `GridLayout`, neither cell sets `Layout.alignment`, so Qt centres the
    shorter one.
  - **Measured** (the review, `lgr-rev-r4/kv.out`, `mono.out`): `Qt.AlignBaseline` in a `GridLayout` puts the caption's
    first line on the value's first line whatever the value's face; `Qt.AlignTop` leaves a 12 px mono value 2.8 px off
    a 13 px caption (baselines 11.19 against 13.97). Stability's Lot, Protocol and First pull values are mono.
  - **Read:** the terminal draws each pair on one row and never wraps a value.
- **R5. The 785 px page** (brief item 5).
  - **Reproduced** (`unit.out`): `Theme.unitWidth` is 7.67 px, so 1000 px is 130 units and 1215 px is 158 units, both
    `expanded`.
  - The three conflicts are about a container's room, not the window: receipt's field and check grids beside a 31 %
    pane, audit's rail inset inside a 21 % rail, console's `fixed(9)` input beside a stretch label.
  - **Read:** the vocabulary has container-relative sizes (`stretch` with bounds, a table's stretch column, wrapping
    rows) and no grid whose column count follows its width.
  - **Read** (the review): `GridImpl::naturalSize` on the terminal plans its height at the all-columns width
    (`container_widgets.cpp:391-421`), so a grid whose columns drop must take its height from the width it was drawn
    at, as the wrapping `StackImpl` does (`:101-130`, `:225-229`).
- **Results' rows, 42 against 39 px.**
  - **Read:** the gallery's theme sets `control.height` 30 and `table.cellPaddingY` 6 (`lims_gallery.cpp:72`, `:76`),
    and a row is its tallest cell plus twice the inset: 30 + 12 = **42**. `08-Results.dc.html:35` gives its `td` 4 px
    (30 + 8 + a 1 px rule = 39); the other mockups' tables use 5 to 7 px, most 6. Accepted as A13.

## The items

| Item | Finding | Verdict | Lane |
|---|---|---|---|
| R1: a single-line input without the focus or a selection marks a cut with "…"; on Qt its full text is its tooltip | receipt 1000, "Release, finished produc" | defect #1082: **reproduced**, **read**, **measured** | R-F |
| R2: a compact head's trailing end stands step 4 from the edge unless the line ends in an unframed button | review 390, the head's text 8 px from the edge | mis-framed, **measured**; smaller than an issue | R-F |
| R3: the `actions.gap` token; `actionBar` as a ui/1 kind that mounts in place outside an overlay; the stepper footer is one | stability 390 at 4 px; three phone pairs at 4 px; three set by hand | **read** | R-F |
| R4: an inline key-value caption sits on its value's first baseline | stability's centred captions | **read**, **reproduced** (review) | R-F |
| R5: `grid.minColumnWidth`, so the columns shown follow the grid's width, on both frontends, height included | receipt 1000 | **read**, **reproduced** units | R-F |

## R1: a single-line input marks a cut

### Decision

- **The rule, on both frontends.** Take a single-line input that is not masked, does not have the focus and holds no
  selection. When its text is wider than its text room, it shows the text from the start, cut at the end with "…".
  With the focus or a selection it shows the editor as today. **The elided form is only drawn: it is never written into
  the value** (`setText`, `text`, `_field->setText` are untouched). The value, the accessible name and value,
  `probeText`, selection and every event are unchanged.
- **The tooltip is Qt Quick's,** as `MorphText`'s is: while elided, the input's tooltip is its full text unless the node
  gives one. The terminal builds no hover text for it, as it builds none for an elided text; its whole value is one Tab
  away. `tui/frontend.md` says so.
- **Scope:** `textInput` in `SingleLine` mode (read-only or editable; `Password` is its own mode and is excluded) and
  `dateTimeInput`'s typed field. Read-only is where the loss was found; an editable field without the focus loses its
  end the same way, so the rule does not depend on `readonly`. A text area wraps.
- **Why not an overflow report.** The field has its laid-out width; the cut is the field's own drawing, which every
  one-line kind marks with "…". Making the input elide makes one form treat overflow one way.
- **Qt Quick.**
  - A new `MorphFieldElision.qml`: a `Label` over a `TextField`'s text box, used by `MorphTextInput` and
    `MorphDateTimeInput`.
  - **`shown`** = `!field.activeFocus && field.selectionStart === field.selectionEnd && field.echoMode === TextInput.Normal
    && field.text !== "" && field.contentWidth > field.width - field.leftPadding - field.rightPadding`. It reads the
    field's own `contentWidth` (equal to the advance, tabular figures included, measured by the review), so no second
    font binding can drift from `textFont`.
  - **The same box as the editor's text:** `x: field.leftPadding`, `y: field.topPadding`,
    `width: field.width - field.leftPadding - field.rightPadding`, `height: field.availableHeight`,
    `verticalAlignment: field.verticalAlignment`, `horizontalAlignment: field.horizontalAlignment`, `font: field.font`,
    `elide: Text.ElideRight`, `Accessible.ignored: true`.
  - **One colour.** The input declares `readonly property color textColor` (the expression its `TextField` uses today);
    the field's `color` is `shown ? "transparent" : textColor`, and the label's is `textColor`.
  - The input's `tooltipText` is the full text while `shown` and truncated.
  - The unit, prefix and icon labels are unchanged: the text room already excludes them.
  - **Intended consequences, stated in the comment:** a window in the background clears `activeFocus`, so an edited
    field shows its elided form there; a field scrolled to its tail by an edit shows its start once it loses the focus,
    which is what the existing comment ("shows its text from the start") already promises.
- **Terminal.**
  - One helper, `paintElidedField(field, owner, reachable, canvas, strong)`, used by `TextInputImpl::paintText` and by
    `DateTimeInputImpl::paintControl` in place of their `paintField` call. When the field is unfocused, unmasked, single
    line and its text is wider than the room, it `putString`s `elidedLine(text, room)` in the field's style
    (`inputNormal`, bold when strong, `textMuted` when not reachable); otherwise it calls `paintField`.
  - **The room leaves the marks:** `room = canvas.width() − marksWidth()` (and one cell less while invalid, where
    `paintField` puts `!`), so the "…" is never under a `*`, `!` or `~`.
  - The end-aligned branch already requires the text to fit.
- **Comments:** one sentence at each site: "without the focus or a selection, a text wider than the field is drawn cut
  with an ellipsis; the value is not changed, and the editor shows it whole once focused".

### Spec

- `backend_contract.md`: the inputs paragraph (after **Read-only**), the rule on both frontends and "the elided form is
  drawn, never written into the value"; "Content that does not fit": "a single-line input elides like a one-line text
  while it has neither the focus nor a selection".
- `qt_quick/backend.md`: the TextInput and DateTimeInput rows, the tooltip included.
- `tui/frontend.md`: the TextInput and DateTimeInput rows: the room before the marks, and no hover text ("its whole
  value is shown once focused").

### Tests

- **Qt** (the suite that holds the input tests):
  - **(discriminating)** a read-only `fixed(10)` input holding a 40-character value: the overlay is visible and
    `truncated`; the field's `color` is transparent; `field.text` is the whole value; the tooltip is the whole value;
    the accessible value is unchanged.
  - **Box and baseline:** `label.y + label.baselineOffset` equals `field.y + field.baselineOffset` within 0.5 px, and the
    label's `x` and `width` equal the field's text box.
  - **Focused:** the overlay is hidden, and the field's colour equals `textColor`.
  - **Selection outlives the focus (discriminating):** focus the field, `select(3, 20)`, then give the focus to a
    `Popup { focus: true }`. The overlay is hidden and the field's colour equals `textColor`.
  - **Boundary (discriminating):** a value whose `contentWidth` lies between the text room and `field.width` elides.
  - **Never written back:** after the overlay is shown and hidden twice, `field.text` and the last `change` event are
    the whole value, and no `change` fired.
  - **Guards:** a short value draws no overlay; a password input draws none; an editable input without the focus
    elides as well; a `dateTimeInput` with a pattern wider than a `fixed(6)` field elides.
- **Terminal** (`test_tui_fields.cpp`):
  - an unfocused read-only input of 10 cells holding a 20-character value paints "…" in its last text cell, and
    `probeText()` is the whole value;
  - **marks (discriminating):** the same input uncaptioned and `required` paints "…" in the cell before its `*`;
  - focused, it paints the `InputField`'s view with no "…";
  - a `dateTimeInput` too narrow for its text paints "…";
  - a masked input never paints "…".
- **Mutations, each reported as failing:**
  - Qt: `shown` always false;
  - Qt: no selection term (fails the selection case);
  - Qt: comparing against `field.width` (fails the boundary);
  - Qt: the label centred in the field instead of the text box (fails box and baseline wherever the style's top and
    bottom padding differ; with the stock style, the lane reports whether it fails, and keeps it as a guard if not);
  - terminal: no elided branch;
  - terminal: the room without the marks (fails the marks case);
  - terminal: the date input on `paintField` (fails its case).

## R2: a compact head's trailing end follows what ends the line

### Decision

- **What ends the line decides the trailing inset,** not how tall the head is: a ghost or icon-only button carries its
  own padding; text, a badge, a select and a framed button reach their box's edge.
- **`ShellFrame::setTrailEnd(HeadTrailEnd end)`** replaces `setTrailEndInset(bool)`. `HeadTrailEnd` is `None` (nothing
  in the trail: the lead's text or badges end the line), `Unframed` (a ghost or icon-only action, or the head menu's
  trigger) or `Framed` (a framed action or a head tool, such as a select). A new frame is `None`.
- **Compact:** `_trailInset` is visible unless the end is `Unframed`, so the line ends step 4 from the edge, the page's
  gutter, except where a button that pads itself ends it, as console's scan icon does. Expanded: unchanged (step 3
  already).
- **The bar's vertical pad** keeps its rule: step 3 while the head is tall or the end is `Framed`, else step 1.
- **Every setter that changes an input of either rule ends in both.** `setHeadOneLine` calls `showBarInset()` and
  `showTrail()` (today only the first); `setTrailEnd` and `setCompact` already call both.
- **The client.** `Workspace::activeHeadTrailEnd()` replaces `activeHeadTrailEndsFramed()`, reading the last shown
  trail item in the trail's order (tools, actions, menu): a head menu → `Unframed`; else the last shown action → `Unframed`
  when `iconOnly` or `Ghost`, else `Framed`; else a shown tool → `Framed`; else `None`. `shell.cpp:517` passes it.
- **The leading end is unchanged:** the menu square's glyph sits about 20 px in.
- **Consequence (A14, corrected):** on review's phone sheet the lead block narrows from 56–386 to 56–374 (318 px). The
  title (326 px) and the tag row (badge boxes 56–376, 321 px) both no longer fit, so the title wraps between words and
  the last tag wraps to a line of its own: the head grows by **two** lines. One-line compact heads with no trail, or a
  trail ending in a framed button or a select, also move to step 4; T10 measures each.
- **Terminal:** the same widget tree, so such heads end 3 more cells from the edge.

### Spec

- `shell_frame.md`, the compact paragraph: the three ends, the rule, and the client's reading of the trail.
- `workspace.md`: `activeHeadTrailEnd`.
- The doc comments of `setTrailEnd`, `setHeadOneLine` and `HeadTrailEnd`.

### Tests

- **`test_ui_shell_frame.cpp`** (recording backend), compact unless stated:
  - `None`, one line: the inset is visible (**discriminating**: today it is hidden);
  - `Unframed`: hidden; `Framed`: visible; expanded with any end: hidden;
  - **call order (discriminating):** `setCompact(true)` with `Unframed`, then `setTrailEnd(None)`, then
    `setHeadOneLine(false)`: visible; and `setCompact(true)`, `setTrailEnd(None)`, `setHeadOneLine(true)` then
    `setHeadOneLine(false)`: the bar's gap is step 3 and the inset visible. **Control:** `setHeadOneLine` without
    `showTrail()` fails the inset part of the first.
- **`test_ui_workspace.cpp`:** `activeHeadTrailEnd` is `None` with no trail, `Unframed` with a menu or a ghost last
  action, `Framed` with a primary last action or a tool alone.
- **Qt** (`test_qt_quick_containers.cpp`, beside `:3076`): a compact frame at 390 with an eyebrow, a long title and no
  trail: the title's right edge is at most the window width minus `Theme.spaceOf(4)`. With an icon-only trail, the
  icon's box ends `Theme.spaceOf(1)` from the edge.
- **Mutations:** the inset on `Framed` only; `Unframed` treated as `None`; `setHeadOneLine` without `showTrail()`.

## R3: a row of actions, themed, anywhere

### Decision (the coordinator's, taking the review's recommendation)

- **Token `actions.gap`** (Dimension, px, default 0): the space between the actions of a row of actions; 0 keeps
  spacing step 2. `checkTheme` refuses a negative value (`actions.gap: must not be negative`), as `head.paddingY`'s.
  - Qt: `Theme.actionsGap` (`theme.cpp`'s mapping beside `head.paddingY`), and
    `readonly property real actionSpacing: actionsGap > 0 ? actionsGap : spaceOf(2)`.
  - The terminal reads no dimension: a row of actions keeps step 2 in cells.
- **`actionBar` becomes a ui/1 kind** (`document.md`): `children`, `align` and `justify` (as a row's), and the common
  keys. It lowers to `ui::ActionBar`, which gains `align` and `justify`. The author API gains
  `ui::actionBar(Nodes, ActionBarOptions{.align, .justify})`.
- **Where it mounts:** inside an overlay's content, at any depth, in the overlay's footer, as today; anywhere else, in
  place, as a horizontal stack. The second is new: today the mount throws there, so no valid document changes meaning.
  The contract sentence: "an action bar holds the actions of what holds it: the footer of the overlay it is inside,
  else the place it is written".
- **The gap:** the mount calls `setGap(2)` and then a new `StackWidget::setActionRow()` (once, before any child). Qt's
  stack then spaces its children by `Theme.actionSpacing`; the terminal's ignores it and keeps step 2; the recording
  backend records `actionRow`. `MorphFooter` spaces by `Theme.actionSpacing` too.
- **`stepperParts`' footer is an `actionBar`** (with the spacer, `extra` and the stretch Continue as today).
- **The screens** write their rows of buttons as `actionBar`s (T5), so the gap is the theme's, not a literal.
- **Stability's Back width** (65 against the mockup's 90 px) is a pixel width on one button: A12.

### Spec

- `document.md`: the `actionBar` row; `view_tree.md`: the ActionBar row (both places, `align`, `justify`).
- `backend_contract.md`: `StackWidget::setActionRow`; `theme.md`: the `actions.gap` row and its validation.
- `author.md`: `actionBar` and the stepper's footer. `qt_quick/backend.md`: the Footer and Stack rows.
  `tui/frontend.md`: an action row keeps step 2.
- `interpreter.md`: the footer actions are an `ActionBar` as before; a document's own `actionBar` lowers the same way.

### Tests

- **Document:** `actionBar` is accepted with `children`, `align`, `justify`; `wrap` and `gap` are refused (the gap is
  the theme's).
- **Mount:** outside an overlay the `ActionBar` mounts in place as a stack with `actionRow` recorded and gap 2
  (**discriminating**: today it throws); inside a dialog's content it still mounts in the footer.
- **Author:** `stepperParts(...).footer` is an `actionBar`. **Control:** a `row` with `gap: 1`.
- **Qt (discriminating):** with `actions.gap` 12, a stepper footer's Continue starts 12 px after Back's end, and a
  dialog's footer actions are 12 px apart; with 0, 8 px. **Control:** a stack that ignores `setActionRow`.
- **Theme:** `actions.gap: -1` is refused.
- **Terminal (guard):** an action row is two cells between buttons.
- **Mutations:** the stepper footer as a `row`; `setActionRow` ignored on Qt; `MorphFooter` on `spaceOf(2)` (fails the
  dialog case at 12); the in-place branch throwing.

## R4: an inline caption sits on its value's first baseline

### Decision

- **`MorphKeyValue.qml`, inline grid:** the caption `Label` and the `Value` both take `Layout.alignment:
  Qt.AlignBaseline`. A caption's first line shares its value's first baseline whichever of the two wraps, and whatever
  the value's face (strong, mono, or runs). Measured on 6.12 by the review; `AlignBaseline` is long-standing in Qt
  Quick Layouts, not run on 6.8.
- **No option.** A caption on its value's first line is what every mockup's `.kv` draws and what the terminal draws.
- **Stacked** is unchanged. **Terminal** unchanged: its values never wrap, so `tui/frontend.md`'s KeyValue row gains
  the clause "each pair is one row, so a caption is always on its value's line"; no terminal test, since none could
  fail.
- **Comment:** "a caption shares its value's first baseline".

### Spec

- `backend_contract.md`, the KeyValue paragraph: "a caption is on its value's first line".
- `qt_quick/backend.md`, the KeyValue row (baseline). `tui/frontend.md`, the clause above.

### Tests

- **Qt (discriminating):** an inline `keyValue` in a `fixed(20)` column: caption "Lot", value a mono run that wraps to
  two lines, in a test theme with `font.size.body` 13 and `font.size.mono` 12 (the gallery's sizes). `caption.y + caption.baselineOffset` equals
  `value.y + value.baselineOffset` within 0.5 px. **Controls:** no alignment (centred, off by about half a line);
  `AlignTop` (off by the faces' baseline difference, about 3 px at 13/12).
- **Qt:** `captionWidth: 4` with a caption that wraps to two lines and a one-line value: equal baselines.
- **Mutations:** the alignment removed; `AlignTop`.

## R5: a grid whose columns follow its room

### Problem

On the 785 px page, receipt's left column must hold three equal field columns and two equal check columns beside a
session pane that needs about 234 px for its badges. At 13 px no split of the row fits both. CSS answers with a grid
that drops columns as it narrows; the vocabulary answers with a window width class, and both grabs are `expanded`.

### Options

1. **A fourth window class, or a numeric `{"env": "widthUnits"}`.** Rejected: both measure the window, while the room
   decides (the sidebar's 28 units and a pane's percent come between them); a fourth class changes `"expanded"` for
   every wide window (`compactFrame`'s `!= "expanded"`, `shell.cpp:411`, and every document's
   `eq(widthClass, "expanded")`); a number re-evaluates every `env` reader at each unit of a drag; neither lets a
   padding or a sizing follow the room.
2. **Wrapping rows of `stretch` + `minWidth` children.** Possible today, but a short last line stretches its child
   across the row, so the fields lose their columns.
3. **Chosen for receipt: `grid.minColumnWidth`,** CSS's `repeat(auto-fit, minmax(m, 1fr))` capped at `columns`.
4. **Container-width classes (CSS container queries):** `{"env": "paneWidthClass"}`, the width class of the nearest
   declared container. It is the general answer to T3's and T4's questions (a sizing and a padding that should follow
   a pane), but it needs containment (a container whose width does not depend on its content: a stretch or percent
   pane), a declaration of which nodes are containers, and re-evaluation per container on resize. That is a design of
   its own. **Not built in this batch** (see Decisions). `minColumnWidth` stays correct beside it.

### Decision

- **Document:** `grid` takes `minColumnWidth`: a literal integer, 1 to 64 units; refused with `tracks` ("a grid with
  tracks sizes its own columns"); refused without `columns` or with `columns: 1`, where it could do nothing (vocal over
  silent); a computed value is refused.
- **View tree:** `Grid::minColumnWidth` (`int`, 0 none). The mount calls `GridWidget::setMinColumnWidth(units)` once,
  before any child, only when it is above 0. The interpreter lowers it; `GridOptions` gains `.minColumnWidth`.
- **Backend contract, `GridWidget::setMinColumnWidth(int units)`:** the columns shown are
  `max(1, min(columns, floor((W + g) / (m·u + g) + ε)))`, `W` the width inside the grid, `g` the gap, `u` the unit, and
  ε = 1e-6: a width that is exactly enough for n columns, computed in floating point, must give n. The natural width is
  unchanged (`columns` × its widest cell, a span divided out). The natural **height** follows the columns shown at the
  width the grid was last laid out at, on both frontends, so a grid whose columns drop pushes its siblings down.
- **Qt Quick, `MorphGrid.qml`:** `property int minColumnWidth: 0`; `place()` computes the shown count from `root.width`
  when `!tracked && minColumnWidth > 0` and uses it for the cell walk, the widths and the rows; `free.implicitHeight`
  already follows the rows placed; `naturalWidth` keeps `columns`. The binding direction is width → count → height,
  and `implicitWidth` never reads the count. `kinds.hpp`'s `GridImpl` writes the property.
- **Terminal, as the wrapping `StackImpl` does:**
  - `GridImpl::setMinColumnWidth`; `planOf(shown, width)` applies the count, with `stepCells(gap, Horizontal)` for `g`
    and 1 for `u`;
  - `naturalSize` keeps the width at `columns`, and while the key is set and a width has been drawn, computes the
    height from `planOf(shown, _drawnWidth)`;
  - `paint` records `_drawnWidth` and calls `refresh()` when it changed, so the parent lays out again;
  - a nested grid settles one frame per level, as wrapping rows do; `tui/frontend.md` says so.
- **RecordingBackend** records `minColumnWidth`. Every `GridWidget` implementer gets the override; the lane lists them
  in the commit message.

### Spec

- `document.md`: the `grid` row (the key and its refusals). `view_tree.md`: the Grid row.
- `backend_contract.md`: the `GridWidget` row (count, ε, natural width and height).
- `qt_quick/backend.md`: the Grid row. `tui/frontend.md`: the Grid row (count, height at the drawn width, one frame per
  level). `author.md`: grid options.
- `expressions.md`, after `widthClass`: "a width class is the window's; a grid's `minColumnWidth`, a stretch's bounds
  and a wrapping row follow the room a node is given".

### Tests

- **Conformance** (`layoutConformanceCases()`, both frontends; each case writes its arithmetic in a comment):
  - **`gridColumnsFollowWidth`:** a `fixed(25)` column holding a grid, `columns: 3`, `minColumnWidth: 10`, gap 0,
    three `sizedText("a")` cells: cell 2 has cell 0's x and a greater y. **Control:** the key ignored.
  - **`gridKeepsColumnsWhenRoom`:** the same at `fixed(30)`: three on one row (exactly 3.0 on both frontends; the ε
    keeps it 3 on Qt, where `30 × u / (10 × u)` is computed in floating point). **Control:** a floor without ε, run
    with a unit that is not a binary fraction (the lane sets `unitWidth` 7.7 in a Qt variant).
  - **`gridFallsToOneColumn`:** at `fixed(9)`: equal x and **strictly increasing y** for cells 0, 1, 2.
    **Control:** no floor (0 columns puts every cell at row 0, column 0 on Qt).
  - **`gridGapCounts`:** `fixed(31)`, `columns: 3`, `minColumnWidth: 10`, gap 1: two columns. Terminal:
    floor((31 + 1) / (10 + 1)) = 2, against 3 without the gap. Qt: floor((31u + 8) / (10u + 8)) = 2 while
    `u < 2 × spaceOf(1)` (7.67 < 8 in the test theme), against 3 without the gap; the test asserts that precondition
    first, so a theme change fails loudly instead of turning the case into a guard.
  - **`gridDropGrowsDown`:** a `fixed(25)` column of that grid followed by a `sizedText` sibling: the sibling's y is at
    least the bottom of the grid's last cell. **Control:** today's terminal `naturalSize` (fails on the terminal).
- **Natural width:** a content-sized grid with the key is as wide as without it. **Control:** a natural width from the
  shown count.
- **Document:** refused with `tracks`, without `columns`, with `columns: 1`, at 0 and 65, and computed.
- **Interpreter:** lowering carries it. **Recording:** recorded.
- **Qt:** resizing a stretch row from 25 to 30 units moves cell 2 back up, with no `LoopWatch` message.
- **Mutations:** the key ignored (Qt; terminal); no floor; the gap left out; no ε (with the 7.7 variant); the terminal
  height at `columns`.

## Lane R-S: screen items

R-S owns `examples/lims_gallery`. T1 to T4 do not depend on R-F and have started. **Round 2 changes none of T1 to
T4's code;** T1 gains one measurement (flagged). The rest wait for the R-F commit named.

| # | Screen | Finding | Change | When |
|---|---|---|---|---|
| T1 | plate 1215 | M: the head's subtitle wraps (+5 px) | Narrow the template tool's `width` from 24 until the subtitle is one line at 1215 with the user name shown. Measure the head at 52 px. **Round 2 (flag):** also measure the select's own elision of "Template: HCP 96, 8 standards" (a select already elides; R1 does not touch it) | now |
| T2 | review 1215 | M: Analyte elides "Assay, me…" | Not B's proposal: dropping `shrink` on Analyte would make Evaluation, the last non-shrink column, give next and cut its verdict. `percent(27)` → `percent(26)` on the queue pane. Measure Analyte whole at 1215, every queue entry whole at 1215 and 1000, and A1 unchanged at 1000. If a queue entry is cut, revert and report the deficit | now |
| T3 | console 1215 | L: result inputs 70 against 75 | `.sizing(ui::stretch()).minWidth(9).maxWidth(10)` on the input. Measure 75 to 77 px at 1215, every input's right edge in one column, and "95.0–105.0" whole at 1000. A container-width class (R5 option 4) would retire these bounds | now |
| T4 | audit 1215/1920 | L: the rail's inset is 8 against 14 | `padding(3, 3)` again, with `.minWidth(23)` on the rail. Measure a whole date and time at 1000, and that the trail table at 1000 loses no content it may not elide. If it does, keep `padding(3, 2)` and report the table's measurement so the 8 px inset can be accepted. A container-width class would retire this bound | now |
| T5 | instrument, audit, samples, results, review and plate at 390 | L: pairs 4 px apart; three set by hand | Each row of buttons becomes `ui::actionBar(...)`: `instrument.cpp:388` (with `.align = Center`), `audit.cpp:452`, `samples.cpp:695`, `results.cpp:639-647` and `:650`, `review.cpp:848`, `plate.cpp:737`. No `gap` is written. Measure 8 px | after R3 |
| T6 | stability 390 and 1215 | L: Back and Continue 4 px apart | No code (the stepper footer is an `actionBar`). Measure 8 px | after R3 |
| T7 | stability 1215/1000 | L: captions centred on two-line values | No code. Measure each caption's baseline on its value's first baseline (Product, Lot, Protocol, First pull, Containers) | after R4 |
| T8 | review 390 | L: the head's text 8 px from the edge | No code. Measure the title's, eyebrow's and tags' right ink at most 374, and the head's new height: two lines more (A14) | after R2 |
| T9 | receipt 1000 and 1215 | M: the check label elided; the sample type clipped; L: Time 54 against 70 | `minColumnWidth` on the field grid (start at 25) and the check grid (start at 32): 2 and 1 columns at 1000, 3 and 2 at 1215 (the review's arithmetic: fields 494 / 199.8 → 2 and 643 / 199.8 → 3; checks 490 / 249.5 → 1 and 639 / 249.5 → 2). Then Time `fixed(6)` again and the session pane's percent re-chosen (start at 33) so the Received badge is whole at 1000. Measure: at 1000 the label and the type whole with no "…"; at 1215 Time 62 or more and the form unchanged from Q; at 1920 the pane capped at 298; the terminal render of receipt with no cell over the next field | after R5 and R1 |
| T10 | all ten at 1215, 1000 and 390, plus 1920 for receipt, audit, stability and review | R1 to R5 | Re-grab everything. The 1000 rule; no new "…" in an input that showed its whole value in Q; every compact head's trailing inset by its end (R2); head heights; each T item on its grab; and, by hand on one screen, no input shows overlapping text after a select and right-click | after R-F |

**Standing checks for every item:** the gallery's unit and render tests are green; the render smoke logs no Qt warning
(an overflow report is one); each fix is measured on the grab it targets; 1000 is checked for clipping. A step that
misses the design by more than 2 px is reported, not tokenised. The gallery tests that pin a row's gap, the receipt's
column widths or a head's height are updated in R-S.

## Accepted, no work

A1 to A11 stand.

| # | Finding | Why |
|---|---|---|
| A12 | stability 390: Back is 65 px, against the mockup's `width: 90px` | A pixel width on one button. Back is content-sized, as a push button is everywhere else |
| A13 | results 1215: rows are 42 px against 39 | The cause is the theme's `table.cellPaddingY` 6 around a 30 px input; most mockup tables use 6, `08-Results` alone 4. **Watched:** a second control-holding table at a smaller inset would make "a cell that holds a control takes a smaller inset" a rule |
| A14 | review 390, after R2: the head grows by two lines, the title wrapping between words and the last tag wrapping to a line of its own | The mockup's phone head has no menu square beside the title, so its 358 px text box holds both on one line each; ours keeps the shell's navigation there (318 px), and the page's gutter is the rule |
| — | e-signature and instrument key-value rows 24 and 23 px apart, against 21 and 22 | Reviewer A's note, not counted: the fallback face's line height at 13 px (inferred) |
| — | plate 1000 rings touch (A9); plate's row grouping, results' "of 2", review's 7 against 24 (data); console's head 53 against 49 (A3); review 1000's footer caption wraps (fit) | As before |

## Lanes

- **R-F.** One Sonnet lane, in its own worktree from `lg-fold-q`. It owns `include/morph`, `src`, `tests` and
  `docs/spec`.
- **R-S.** In `morph-lims-e2`. T1 to T4 are under way; it rebases onto R-F, then does T5 to T10.
- **Shared files:** none. The gallery uses `actionBar` and `minColumnWidth` only after R-S rebases.
- **Builds:** under `flock $S/morph-build.lock`, `-j10` at most.
- **`morph-lims-e1`:** neither lane touches it.

### R-F, one commit per item, in this order

1. **R4:** `MorphKeyValue.qml`, two Qt tests, the specs.
2. **R3:** `theme.hpp` (the token and its check), `theme.cpp`, `Theme.qml` (`actionsGap`, `actionSpacing`); `view.hpp`
   (`ActionBar::align`, `justify`, `actionBar` options), `document.hpp`, `interpreter.hpp`, `mount.hpp` (in place, and
   `setActionRow`), `backend.hpp` (`StackWidget::setActionRow`), `author.hpp` (`actionBar`, `stepperParts`);
   `MorphStack.qml`, `MorphFooter.qml`, `kinds.hpp`/`.cpp`; the terminal's stack (a no-op override);
   `recording_backend.hpp`; the tests and the specs.
3. **R2:** `shell_frame.hpp` (`HeadTrailEnd`, `setTrailEnd`, the condition, `setHeadOneLine` calling `showTrail()`),
   `workspace.hpp` (`activeHeadTrailEnd`), `shell.cpp`; the frame, workspace and Qt tests; `shell_frame.md`,
   `workspace.md`.
4. **R1:** `MorphFieldElision.qml` (in the module's QML list), `MorphTextInput.qml`, `MorphDateTimeInput.qml`;
   `field_widgets.cpp` (`paintElidedField`); the tests and the specs. Closes #1082.
5. **R5:** `view.hpp`, `document.hpp`, `interpreter.hpp`, `mount.hpp`, `author.hpp`, `backend.hpp`; `kinds.hpp`/`.cpp`
   and `MorphGrid.qml`; `container_widgets.hpp`/`.cpp` (`_drawnWidth`); `recording_backend.hpp`; the conformance cases,
   the tests and the specs.

**Before reporting done,** R-F re-runs every touched suite, the three conformance runners, the gallery's render smoke,
the docs target under clang with `WARN_AS_ERROR` (`HeadTrailEnd`, `setTrailEnd`, `setActionRow`, `ActionBarOptions`,
`setMinColumnWidth` and `GridOptions` are public), clang-tidy on the touched TUs, and qmllint.

**Mutations, each reported as failing:** the ones listed under each item's tests: R1 seven, R2 three, R3 four, R4 two,
R5 six.

## Risks

- **R1** changes how every input without the focus or a selection draws text wider than its field. T10 looks for a
  "…" that Q did not show. The overlay tracks the field's text box, which the icon, prefix and unit change; the box
  and boundary tests cover it. A transparent colour must never outlive the overlay; the focused and selection tests
  cover it.
- **R2** moves every compact head that does not end in an unframed button to step 4 at its trailing end: 12 px
  narrower on Qt, 3 cells on the terminal. Lines near the limit wrap (A14). Terminal tests that pin those widths get
  expectations computed from the rule. `setTrailEndInset` is replaced, so its callers (one in `shell.cpp`, the tests)
  move to `setTrailEnd`.
- **R3** moves every stepper's Continue 4 px (one cell) from Back. A stepper inside a dialog's content would now put its
  footer in the dialog's footer, since an `actionBar` there mounts in the footer; no stepper is in a dialog today
  (stability only), and the author spec says so. A theme's `actions.gap` moves every overlay footer's spacing.
- **R4** moves the caption of every inline key-value wherever its value wraps or its face differs (stability,
  instrument, e-signature, `examples/gallery`); forms do not use `keyValue`.
- **R5** is opt-in. The terminal's height-at-drawn-width settles one frame per nested level, as wrapping rows do.
- **Qt 6.8.1.** Nothing needs an API newer than 6.8 (`selectionStart`, `contentWidth`, `truncated`, `baselineOffset`,
  `Qt.AlignBaseline`, `availableHeight`). The selection-over-overlay case is reachable on 6.8 through a popup or window
  deactivation (inferred); the probes ran on 6.12 only.
- **CI traps:** Doxygen under clang; MSVC `typename`; Qt keywords as identifiers; GCC 15 `-O3`; qmllint on the new QML
  file.

## Decisions

1. **An input without the focus or a selection marks a cut with "…"** on both frontends, the room before the terminal's
   marks; the elided form is never written into the value; the full-text tooltip is Qt's. #1082.
2. **A compact head's trailing end stands step 4 from the edge unless the line ends in an unframed button**
   (`HeadTrailEnd`). The phone "title overflow" was mis-framed.
3. **`actions.gap` (0 keeps step 2) is the gap of a row of actions,** and `actionBar` is a ui/1 kind that mounts in an
   overlay's footer inside one and in place elsewhere; the stepper's footer and the screens' button rows are action
   bars (the coordinator's decision).
4. **An inline key-value caption shares its value's first baseline** on Qt; the terminal already draws one row per pair.
5. **`grid.minColumnWidth`:** a grid's columns, and its height, follow its room on both frontends.
6. **Container-width classes (container queries) are the general responsive answer,** and are not built now: every
   mockup is met without them. Recorded as the follow-up that would retire T3's and T4's screen bounds (the
   coordinator's decision).
7. **Accepted:** A12 to A14.

## Records

- **Filed:** #1082, closed by R1. No `triage:` label.
- **Not filed:** R2 (mis-framed; the real fix is small), R4 (smaller than an issue), R3 and R5 (vocabulary), the
  screen items.
- **Corrections to the after-Q reports:** reviewer B's review 390 title fits its line (ink 57–382, line end 386); B's
  proposed T2 fix would make Evaluation give next; results' 42 px rows come from the theme's 6 px cell inset.
- **Watched:** A13; `MorphPanel`'s explicit 5 px header gap (from Q); A8; container-width classes (Decision 6).
- **Probes kept** in `$S/lgr-exp/`: `readonly`, `unit`, `measure-head` (`.qml`/`.py` and `.out`), the crops, and
  `lg-r-design.round1.md` (this design before round 2).

## Review round 2

| Item | Change |
|---|---|
| **RB1** selection under the overlay | `shown` also requires `selectionStart === selectionEnd`. New discriminating Qt test (select, then focus a `Popup`), and the mutation "no selection term". Qt 6.8 reach stated in Risks |
| **RB2** terminal grid height | `naturalSize` takes its height from `_drawnWidth` while the key is set, `paint` refreshes when the width changes, one frame per nested level (as wrapping rows). New conformance case `gridDropGrowsDown`, and the mutation "height at `columns`". The contract sentence now holds on both frontends |
| **RS1** marks over "…" | The terminal room is `canvas.width() − marksWidth()` (one more cell while invalid). New test: an uncaptioned required input paints "…" before its `*`; mutation "room without the marks". `DateTimeInputImpl` routed through the shared `paintElidedField` |
| **RS2** terminal tooltip | The tooltip is Qt Quick's only; `tui/frontend.md` says the whole value is shown once focused |
| **RS3** Qt test readability | Assertions use `truncated`, transparent `color`, `contentWidth` for overflow, the editor's text box (`topPadding`, `availableHeight`, `verticalAlignment`) and an equal baseline within 0.5 px; one `textColor` property; "never `setText` the elided form" stated, with a never-written-back test and `probeText()` on the terminal; window deactivation and the tail-scroll note stated as intended |
| **RS4** `setHeadOneLine` call order | It calls `showTrail()` as well as `showBarInset()`; a call-order test with the control "without `showTrail()`" |
| **RS5** the end rule and A14 | The rule follows what ends the line: `HeadTrailEnd {None, Unframed, Framed}` and `setTrailEnd`, read by `Workspace::activeHeadTrailEnd` from the last shown trail item. A14 and T8 now say two lines (the title and the tags) |
| **RS6** `kActionGap` | Replaced, by the coordinator's decision: the `actions.gap` token (0 keeps step 2), `actionBar` as a ui/1 kind mounting in place outside an overlay with `StackWidget::setActionRow`, the stepper's footer an `actionBar`, T5's rows `actionBar`s with no written gap. The terminal keeps step 2 in cells |
| **RS7** baseline, terminal guard | `Qt.AlignBaseline` on both cells; the Qt test uses a mono value and asserts equal baselines, with `AlignTop` as a second control. The terminal guard, which could not fail, is dropped for a clause in `tui/frontend.md` |
| **RS8** R5's controls | `gridFallsToOneColumn` asserts strictly increasing y; `gridGapCounts` is specified (`fixed(31)`, gap 1, 2 against 3, with its precondition asserted); `floor(x + 1e-6)` with its reason and a non-binary-unit variant as its control; `minColumnWidth` refused without `columns` or with `columns: 1` |
| **RS9** pane-width classes | Option 4 in R5; Decision 6 records container-width classes as the general answer, not built now, and the follow-up that would retire T3's and T4's bounds (the coordinator's decision) |
| **T1–T4 (R-S already started)** | No code change to any of them. T1 gains one measurement (the select's own elision). T3 and T4 note that a container class would retire their bounds; nothing to do now |
| **Review's screen notes** | T5 is `actionBar`s; T8 two lines; T9 cites the review's arithmetic and checks the terminal render (needs RB2); T10 adds the select-and-right-click check |
