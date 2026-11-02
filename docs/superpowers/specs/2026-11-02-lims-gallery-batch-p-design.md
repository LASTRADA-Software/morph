# LIMS gallery batch P: equal grid columns, a caption column width, touch-size control text, a small button's height of its own, and a caption text role

Batch P covers every finding left open in the after-O comparison (`lg-or-findings-A.md`, `lg-or-findings-B.md`). Each
finding becomes one of three things:
- a framework item (P1 to P6), for lane P-F;
- a screen item (S1 to S19), for lane P-S;
- an accepted difference (A1 to A7), with its reason.

The review (`lg-p-review.md`: 1 blocker, 12 should-fixes) is taken in full. See "Review round 2" at the end. Its
should-fixes are written R-S1 to R-S12 here, so they do not clash with the screen items S1 to S19. **No screen item S1
to S10 changes.** P-S has started those, and the review required no change to them.

The rules of batches G to O hold:
- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- the terminal keeps its cells, and a purely visual change says what it leaves unchanged there;
- vocal over silent.

The gallery is judged on the 1215×830 grab, where the page is about 1000 px, as the mockups' window is. On the
1000×830 grab (a page of about 785 px), nothing may clip, overlap or lose content, except where a column is told it
may elide.

**Sources and evidence.**
- **Revisions.** Every claim was checked against `lg-fold-o` at `74bb31a79`, which the grabs were made on.
  `lg-fold-o` has since moved on by one test-only commit, `3150951bf`.
- **Where the code was read:** `/home/yaraslau/repo/morph-lims-e2`, on `lg-os` (`d0e2013be`).
  - The QML, theme and terminal files quoted here are byte-identical between the two branches, except
    `MorphWidget.qml`. Its seven differing lines are a `roomOf` helper that none of the items touch.
  - `document.hpp` and `interpreter.hpp` differ by lint fixes, so their line numbers are `lg-os`'s and may be a few
    lines off on `lg-fold-o`.
  - `morph-lims-e1` was not opened.
- **How each premise is marked:**
  - **measured**: by the reviewers, with PIL, on `lg-cmp/app-o/`, or by me, as stated;
  - **reproduced**: run here, with the output shown;
  - **read**: inferred from the code.
- **What was run.** One plain-Qt `GridLayout` experiment (Qt 6.12.0, offscreen; `lgp-exp/` in the scratchpad).
  Nothing in morph was built. The reviewer's attempt to run it on Qt 6.9.3 did not start: the image has no
  `libxkbcommon-x11`. So nothing has been run on a Qt older than 6.12.

## Probes

- **Q1. The grid's columns** (P4).
  - **Measured** (reviewer A; re-measured by the review). Receipt's Condition grid (`.columns = 2`, `gap = 1`, no
    tracks) runs from x 244 to 877 at 1215 and from 244 to 1582 at 1920. Its column 2 starts at 602 and at 998,
    a 1.27 ratio at both widths.
  - **Read:**
    - `MorphGrid.qml` puts a grid without `tracks` in a `GridLayout`, and `place()` returns early.
    - Each content-sized cell is `Layout.fillWidth`, with `horizontalStretchFactor: -1` and no preferred width
      (`MorphWidget.qml:253-284`).
    - The terminal's `planGrid(int, …)` (`layout.cpp:329-333`) gives every column `stretch(1)` with natural 0, and
      `distribute` shares a stretch item's room by weight alone. That gives equal columns.
    - The specs say "columns sharing the width" (`backend_contract.md:186`, `document.md:267`) and "equal columns"
      (`tui/frontend.md:87`, `:148`).
  - **Read: the natural widths differ too** (the review's B1). The terminal's natural width for `columns` is
    `columns × unit + gaps`, where `unit` is the widest cell, and a spanning cell counts as its width less the
    covered gaps, divided by its span (`container_widgets.cpp:408-415`). Qt's tracked `place()` sums each column's
    widest one-track cell, and it ignores spanning cells.
  - **Reproduced on plain Qt Quick**, not on morph's grid. A two-column `GridLayout` of fill cells with implicit
    widths 254 and 200 keeps a 1.27 ratio at 500, 785, 1000 and 1400 px. `horizontalStretchFactor: 1` gives 1.22 to
    0.85, which is not equal either. `uniformCellWidths: true` gives equal cells. The output is in
    `lg-p-issue-grid.md`.
  - **Verdict:** a defect in shipped library behaviour (`src/qt_quick`). Its issue body is `lg-p-issue-grid.md`.
    Both frontends pass today because no conformance case covers `columns` without tracks.
- **Q2. The caption column** (P2).
  - **Read** (`MorphKeyValue.qml`): captions sit in column 0 of a `GridLayout`, with `NoWrap` and
    `minimumWidth: implicitWidth`, so the column is as wide as the widest caption.
    - The terminal's `KeyValueImpl::captionWidth()` is the widest label (`display_widgets.cpp:484-491`).
    - `KeyValue` has no key for the column's width (`view.hpp:1521-1528`).
  - **Read** (the forms engine, from the review's R-S7): `ViewOptions::inlineLabelWidth` is in units, defaults to 16
    and throws below 1 (`form_view.hpp:60-62`, `:417`, `:521-524`). It is the caption column of
    `fieldLayout: inline`, drawn as a `fixed(width)` text. Documents cannot set it.
  - **Read** (the mockups): the caption columns are `96px` (Main, the e-signature), `130px` (instrument) and `100px`
    (stability), each followed by `minmax(0,1fr)`.
  - **Measured:** the e-signature's caption column is about 57 px against the mockup's 105 (96 plus the gap).
    Instrument's is 90 px against 143 (130 plus 13).
- **Q3. The well's size** (P3).
  - **Read:** a small button "keeps the control height" (`control_shape.hpp:30`). `MorphButtonBackground` sets
    `implicitHeight: Theme.targetHeight` for every button (`:41`).
  - **Read** (`07-Plate.dc.html`): `.well` is 40 px high in a 44 px track. `.wl .well` on the phone is 36 × 36.
  - **Fixed sizing is honoured for geometry, but it cannot express the size** (**read**).
    - A tracked grid's `place()` clamps a fixed cell to its amount.
    - The amounts are in units: `Theme.unitWidth`, about 7.7 px, across, and `Theme.unitHeight`, the body line,
      about 16 px (**inferred**), down.
    - 40 and 36 are not whole numbers of height units, and a circle would need the same length in two different
      units.
  - **Measured** (the review): on plate-1000 the well pitch is about 39.4 px, so a stretch track is about 35 px.
  - **Read** (plate's phone run line, `plate.cpp:648-674`): `padding(0, 1)`, with an Assign button shown only on an
    empty run. A row without Assign is as tall as its well.
- **Q4. A control's text on the phone** (P1).
  - **Read** (the mockups): the phone's large controls are 14 px. `.btn.lg{height:44px;font-size:14px}` appears in
    10 files and `.input.lg{…14px}` in 5. Every phone textarea sets 14 px. The desktop `.input` is 13 px.
  - **Read** (the code): the text inputs, text areas, selects, date inputs and buttons take `Theme.bodyFont`,
    `buttonFont` or `strongFont`, whatever `touchTargets` says.
  - **Measured** (reviewer B): review's phone textarea is 13 px.
- **Q5. A 12 px muted note** (P5). **Read:** the mockups draw `class="muted" style="font-size: 12px"` in:
  - review: the self-review note, and "Approving needs your signature";
  - console: the four KPI notes, the three compact KPI notes, and "mean 100.0 % · SD 0.4";
  - instrument: "(needs reason)", a menu entry's description, and the phone's stacked key-value captions;
  - plate: "dragging".

  `TextRole` has `Small` (`font.size.small`, 11 px in the text colour) but nothing at `font.size.caption` (12 px). The
  code draws all of these as `layout::muted`, at the body size (`console.cpp:487`, `:524`).
- **Q6. The console head** (A3).
  - **Measured:** every app head is 53 px (console-1215 and plate-1215, by the review).
  - **Read** (the mockups): eight designs pad the head `12px 16px` around a 30 px control. Console fixes its header
    at `height: 48px`.

## The items

| Item | Finding | Verdict | Lane |
|---|---|---|---|
| P1: `control.touchTextSize` | review 390 textarea, 13 against 14 px | **measured** symptom; the cause is **read** (Q4) | P-F |
| P2: `captionWidth` on `keyValue`, `form`, `collection`, `wizard` | e-signature 57 against 105 px; instrument 90 against 143 | **measured** (Q2) | P-F |
| P3: `button.smallHeight`, `button.smallTouchHeight`; a round button never taller than wide | plate wells 44 against 36 on the phone; 30 against 40 on the desktop (a regression) | **measured**; the gap is **read** (Q3) | P-F |
| P4: equal grid columns on Qt Quick, with the terminal's natural width | receipt's Condition columns at 1.27 | a defect: **reproduced** on plain Qt, **measured** in the gallery, **read** in morph (Q1). Issue body `lg-p-issue-grid.md` | P-F |
| P5: text role `caption` | review's self-review note, and the other Q5 sites | **read** (Q5) | P-F |
| P6: stacked key-value captions at the caption size | instrument phone: captions in the body font against 12 px | **read** from the mockup HTML, not measured; a rule `form` already follows | P-F |

## P1: control text at a touch size

### Problem

On the phone, the mockups draw a control's own text at 14 px. The app draws it at the body size, 13 px (Q4). The
difference goes with `touchTargets`, the compact width class, which the backend already sets. It is not a choice made
for each field.

### Options

1. **A `textSize` key on each input.** Rejected: every phone field would need it, the desktop could not have it, and
   screens would pick pixels.
2. **Chosen: a theme dimension, `control.touchTextSize`.** It sits beside `control.touchHeight`. 0, the default,
   keeps the body size, so a theme that does not set it is unchanged.

### Decision

- **The token.** `control.touchTextSize`, a dimension in px, default 0. While the window's width class is compact, a
  control's own text is drawn at this size. 0 keeps the body size.
- **What it covers:**
  - the text of a text input, with its prefix and its unit label;
  - the text of a text area;
  - a date input's field;
  - a select's field and its segments, the marked segment included;
  - a push button's caption at `size: normal`, normal or strong.
- **What it leaves alone:**
  - the prominent input, which keeps the heading size;
  - a `mono` control, which keeps `font.size.mono`, a role size rather than the body size;
  - a small button, which keeps the small step;
  - an item-style button;
  - field captions, popup lists (a select's options, the calendar) and the labels of radio-style selects;
  - the labels of check boxes and tabs.
- **Qt Quick.**
  - **`Theme.qml`** gains `touchTextSize` (default 0), `controlTextPixels` and `controlFontOf(weight)`. These are
    named R-S6.
    - `controlTextPixels` is `touchTargets && touchTextSize > 0 ? touchTextSize : bodyPixels`.
    - `controlFontOf(weight)`: while `controlTextPixels` equals `bodyPixels`, it returns `bodyFont` for
      `Font.Normal`, `strongFont` for `strongWeight` and `buttonFont` for `buttonWeight`. That keeps today's fonts,
      the platform font among them. Otherwise it returns `fontOf(controlTextPixels, weight)`.
  - **The uses:**
    - `MorphTextInput.textFont`, in its non-prominent, non-mono branch, keeping `figuresOf`. The unit label
      (`:127-136`) is given `font: root.textFont`; it sets no font today.
    - `MorphTextArea`, in its non-mono branch.
    - `MorphDateTimeInput`'s field (`:73`).
    - `MorphSelect`'s combo (`:55`), and its segments (`:316`): `controlFontOf(segment.marked ? strongWeight :
      Font.Normal)`.
    - `MorphButton.captionFont`, for `style === 0`, not small and not mono:
      - a normal caption is `controlFontOf(Font.Normal)`;
      - a strong caption is `controlFontOf(Theme.buttonWeight)`;
      - a caption with its own weight is `controlFontOf(captionWeight)`.
  - **`theme.cpp`** maps the token in `kDimensionProperties`.
  - **Units.** `unitWidth` and `unitHeight` keep reading the body font, so no unit moves.
- **Terminal.** Unchanged. It draws no text sizes, and it ignores this token as it ignores `control.touchHeight`.
- **C++.** One entry in `theme.hpp`'s token set, with its rule as a comment.

### Spec

- **`theme.md`:**
  - the token's row;
  - in the touch paragraph, the text-size sentence and the lists of what it covers and leaves alone;
  - the property table at `:241`.
- **`qt_quick/backend.md`:** the TextInput, TextArea, DateTimeInput, Select and Button rows name `controlFontOf`.

### Tests

- **`test_ui_theme.cpp`:** the token defaults to 0 and is read from a file.
- **`test_qt_quick_theme.cpp`.**
  - **The test theme** sets `font.family`, so `bodyFont` is `fontOf(bodyPixels)` rather than the platform font
    (R-S12). It also sets `body` 13 and `control.touchTextSize` 14.
  - **With `touchTargets` true:**
    - 14 px at the right weight: a text input, its unit label, a text area, a date field, a select's combo, a
      normal segment and a marked one (strong), a normal push button and a primary one (at `buttonWeight`);
    - unchanged: a small button at `smallPixels`, a prominent input at `headingPixels`, a mono text area at
      `monoPixels`, and an item button at 13.
  - **With `touchTargets` false:** each one is at `Theme.bodyFont.pixelSize`, with its weight.
  - **With the token at 0:** `controlFontOf` returns `bodyFont`, `strongFont` and `buttonFont`.
- **Mutation controls, each reported as failing:**
  - the text area reading `Theme.bodyFont` fails its check;
  - the strong button branch reading `Theme.buttonFont` fails the primary button's size check.

## P2: the caption column's width: `captionWidth`

### Problem

Q2. The designs give the caption column a set width, so captions align across panels and the values start at one
place. A `keyValue` sizes the column to its widest caption. The forms engine already has a set width for the same
column, `inlineLabelWidth`, but a document cannot reach it (R-S7).

### Decision

- **One concept, one spelling.** `captionWidth` is the document's name for the width of an inline caption column, in
  units. It means the same on every kind that takes it:
  - **`keyValue`:** absent, the column is as wide as the widest caption, as today.
  - **`form`, `collection` and `wizard`:** absent keeps the engine's `inlineLabelWidth` default (16). Present, it
    sets `inlineLabelWidth` (`presentForm`, `interpreter.hpp:8768-8774`).

  What absent means differs between the two families, because each keeps its current default. What the key means
  does not differ.
- **The value.** A literal integer from 1 to 64.
  - It is checked as `checkFormColumns` checks `columns` (`document.hpp:4617`).
  - It is refused when it is given with `fieldLayout: stacked`, or, on a form, without `fieldLayout: inline`: there
    is then no caption column. The refusal reads "captionWidth needs inline captions".
  - `kKeyValueKeys` (`document.hpp:1055`) and the form, collection and wizard key tables (`:1112`, `:1121` and the
    form's) each gain `{"captionWidth", Shape::Expr}`.
- **Units, not pixels**, like every width in ui/1. The gallery's 96, 100 and 130 px become 12, 13 and 17 units. With
  the 2-unit spacing they measure about 108, 115 and 146 px, against the mockups' 105, about 112 and 143: within 3 px.
- **A caption longer than the column:**
  - **Qt Quick** wraps it at word boundaries, as the design's CSS grid does and as the forms engine's `fixed` caption
    text already does. The row grows to the taller of the caption and the value.
  - **The terminal** cannot wrap. It draws the caption and its colon in `n + 1` cells, cut with "…" where they do not
    fit. The probe text and the accessible name keep the whole caption.
  - `tui/frontend.md` documents the difference. For forms, the terminal already draws a `fixed` text this way.
- **View tree.** `KeyValue` gains `std::optional<int> captionWidth`, set once.
  - The mount refuses an engaged `captionWidth` with `captions == Above`, or a value below 1. It throws
    `std::invalid_argument`, as `inlineLabelWidth` does, so a C++ author is not ignored silently (R-S8).
  - The Doxygen gives the unit, the range and the refusal.
- **Author API.**
  - `KeyValueOptions` gains `std::optional<int> captionWidth`, written only when engaged.
  - The form options' author struct does the same, where `fieldLayout` is written (`author.hpp:5488`).
- **Interpreter.** The `keyValue` branch (`interpreter.hpp:8074`) and `presentForm` read the key.
- **The backend interface.** `KeyValueWidget::setCaptionWidth(int units)` is pure virtual, and the mount calls it
  once, only when the key is engaged.
  - It is implemented by Qt Quick, the terminal, `RecordingBackend` (key `captionWidth`) and
    `tests/ui_echoing_backend.hpp`.
  - It is pure, so a backend that forgets it does not compile; a default would draw the widest caption silently.
- **Qt Quick** (`MorphKeyValue.qml`).
  - It gains `captionUnits`, −1 by default.
  - While `captionUnits` is 0 or more, the caption `Label` takes `Layout.preferredWidth`, `minimumWidth` and
    `maximumWidth` of `captionUnits * Theme.unitWidth`, and `wrapMode: Text.Wrap`.
  - This reads no width from the grid, and the comment at the label says so, in one clause.
- **Terminal** (`KeyValueImpl`).
  - It gains `_captionWidth`, a `std::optional<int>`.
  - `captionWidth()` returns `_captionWidth` when it is engaged.
  - `paint` elides the caption to `captionWidth() + 1` cells with `elidedLine`.

### Spec

- `document.md`: the `keyValue`, `form`, `collection` and `wizard` rows, with the shared definition stated once.
- `view_tree.md:128`.
- `author.md`.
- `backend_contract.md:375` and `:482`.
- `qt_quick/backend.md:186`.
- `tui/frontend.md:103`.
- The forms spec page that documents `inlineLabelWidth`: it names `captionWidth` as its document spelling.

### Tests

- **Conformance: `keyValueCaptionWidthSetsTheValueColumn`**, in `layoutConformanceCases()`. It reads geometry first.
  - **Setup:** a content-sized `keyValue` with `captionWidth: 20` and one pair, "a" and "b", in a column with
    `align: start`.
  - **Assert:** it is 23 units wide, within one.
  - **Control:** without the setter, it is about 4 units on both frontends.
- **Document:**
  - refused: non-integer, 0, 65 and computed values; `stacked` with the key; a form without `inline`;
  - the author API writes the key only when it is set;
  - a form's `captionWidth: 20` reaches `inlineLabelWidth` 20, and the round trip holds.
- **Mount:** `KeyValue{.captions = Above, .captionWidth = 12}` throws (**control:** without the check, it mounts).
- **`RecordingBackend`:** records `captionWidth` once, and nothing when the key is absent.
- **Qt Quick:**
  - every caption is `12 * unitWidth` wide, and a 30-character caption wraps and grows its pair (**control:**
    without the branch, the widths follow the captions);
  - a guard, which passes without P2 too: the values' `x` are equal across pairs.
- **Terminal:**
  - values start at column `n + 2`, and a longer caption ends with "…" at cell `n`;
  - `probeText` keeps the whole caption;
  - **control:** without the elision, the caption overwrites the value.

## P3: a small button's height of its own

### Problem

Q3. A plate well is a round button at `size: small`. The design draws it 40 px high on the desktop, above the 30 px
control, and 36 px on the phone, below the 44 px touch target. ui/1 ties every button to `targetHeight`, and fixed
sizing cannot express either size. At 1000 the track is about 35 px, so a 40 px round well would be a vertical capsule
whose ring overlaps its neighbours (R-S4).

### Options

1. **A `diameter` key.** Rejected: a screen's choice of pixels, and a key for one shape only.
2. **Pixel sizing.** Rejected: ui/1 has none, and the terminal's cells have no use for it.
3. **A `large` size.** Rejected: the well is larger than a control on the desktop and smaller on the phone.
4. **Surface-scoped heights** (R-S5 (b)). Rejected for now: surfaces carry colours, strokes and keywords, not
   dimensions. That is a larger mechanism than one finding warrants.
5. **Chosen** (R-S5 (a), renamed):
   - **`button.smallHeight`** and **`button.smallTouchHeight`**, button-scoped as `input.paddingX` and `table.*`
     are. They are no longer `control.*`, because they size a small button and no other control.
   - **`size: small` is redefined by what it is:** a compact caption, the least side padding and the small type
     step. The word no longer promises a short button. Its height is the theme's small height, which may be above
     or below the control height.

### Decision

- **The height.** A `size: small` button is drawn at `Theme.smallButtonHeight`:
  - while `touchTargets` holds: `smallTouchHeight > 0 ? smallTouchHeight : targetHeight`;
  - otherwise: `smallHeight > 0 ? smallHeight : controlHeight`.
  - A theme that sets `button.smallTouchHeight` below `control.touchHeight` takes the touch target onto itself.
    `theme.md` says so.
- **A round button is never taller than it is wide** (R-S4). It is already at least as wide as tall. When it is
  placed narrower than its height:
  - the background, the focus ring and the selection ring are a circle of the button's width, centred vertically;
  - the item keeps its height and its touch area.

  This applies to every round button, whatever its size, not just the wells. It is a drawing rule in
  `MorphButtonBackground` (its height becomes `round ? Math.min(height, width) : height`, centred), not a layout
  rule. A capsule taller than it is wide is never what `round` means.
- **Doxygen** (`control_shape.hpp:30`) for `Small`: "A compact caption: the least side padding and the small type
  step, at the theme's `button.smallHeight`, which may be above or below the control height; without it, the control
  height."
- **Qt Quick:**
  - `Theme.qml` gains `smallHeight`, `smallTouchHeight` and `smallButtonHeight`;
  - `MorphButtonBackground.implicitHeight` is `compact ? Theme.smallButtonHeight : Theme.targetHeight`, and its drawn
    height follows the round rule above;
  - `theme.cpp` maps both tokens.
- **Terminal.** Unchanged: a small button is one row, and a cell is the target.
- **C++.** Two entries in `theme.hpp`'s token set.

### Spec

- `theme.md`: two rows, the touch sentence, and "applies to every small button in the theme".
- `document.md:260`: `size: small`, and the round rule under `shape: round`.
- `backend_contract.md`: `ButtonSize` and `ControlShape::Round`.
- `qt_quick/backend.md`: the Button row.

### Tests

- **`test_ui_theme.cpp`:** both tokens default to 0 and are read from a file.
- **`test_qt_quick_theme.cpp`.** The theme sets `control.height` 30, `smallHeight` 40, `smallTouchHeight` 36 and
  `touchHeight` 44.
  - **Heights:**
    - a small round "S1" is 40 × 40, and 36 × 36 under touch;
    - a normal button stays 30, and 44 under touch;
    - with both tokens at 0, a small button's height equals a normal one's.
  - **The round rule:** a small round button placed 30 px wide in a 40 px row has a background 30 × 30, centred, and
    an item 40 high.
- **Mutation controls:**
  - `implicitHeight` back to `targetHeight` fails the 40 and 36 checks;
  - without the round rule, the background is 30 × 40.

## P4: equal grid columns on Qt Quick, with the terminal's natural width (defect)

### Problem

Q1. The issue body is `lg-p-issue-grid.md`. The dispatcher files it before P-F starts, and P-F's commit closes it.

### Options

1. **`uniformCellWidths: true`.** Rejected: it keeps two placement paths, and the `GridLayout` path has its own rules
   for spare height and minimums. The two frontends would keep drifting.
2. **Chosen: a grid without tracks is `columns` tracks of `stretch(1)`, with the terminal's natural width.**
   - `MorphGrid` drops the `GridLayout`, and `place()` lays out every grid.
   - Its natural width is the terminal's `GridImpl::naturalSize` (B1). Summing the column naturals would squeeze a
     content-sized grid's own cells, once equal columns split them.

### Decision

- **`MorphGrid.qml`.**
  - `placedTracks` is `tracks` when it is not empty. Otherwise it is `columns` copies of
    `{kind: 2, amount: 1, align: 0}`.
  - `place()` reads `placedTracks`. `content` is always `free`, and the `GridLayout` and the `tracked` switch are
    removed.
- **The natural width without tracks** (B1):
  - it is `columns × unit + space × (columns − 1)`;
  - `unit` is the largest, over the shown cells, of `ceil((naturalWidth(cell) − space × (span − 1)) / span)`, with
    `span` clamped to 1 to `columns`;
  - this is `GridImpl::naturalSize` (`container_widgets.cpp:408-415`) in pixels.
  - **With tracks** the natural width keeps today's rule. A content-sized grid of stretch tracks is the same on both
    frontends, so it is out of scope.
- **Re-placement** (R-S11). `place()` runs again on:
  - `onColumnsChanged` and `onGapChanged`;
  - a `Connections` on `Theme`, for `unitWidthChanged`, `unitHeightChanged` and `spaceChanged` (the spacing scale is
    the `space` list).

  Tracked grids gain this too.
- **A filling cell stops at its maximum**, at the track's start: `min(room, maxWidthUnits * unitWidth)`. This is the
  terminal's `crossExtent`, and it keeps what the `GridLayout`'s `Layout.maximumWidth` did.
- **What changes, said in `qt_quick/backend.md`:**
  - the columns no longer follow their content;
  - a grid given more height than its rows no longer spreads it;
  - a cell's minimum no longer widens its column;
  - the natural width is columns times the widest cell.

  All four are the terminal's behaviour.
- **The header comment** states the one rule: "Children row-major in its tracks; without tracks, in `columns` equal
  stretch tracks, as wide by nature as `columns` times the widest cell."
- **C++.** `GridImpl`'s code is unchanged. Its two Doxygen comments (`kinds.hpp:344-366`) are rewritten (R-S12):
  "places its cells itself, again whenever one's size, visibility or span changes", with nothing about a
  `GridLayout`.
- **Terminal.** Unchanged. It is the reference here, not the unit under test (R-S1).

### Spec

- `qt_quick/backend.md:169`: the Grid row, including the natural width and the re-placement triggers.
- `backend_contract.md:186`: "in `setColumns` equal columns (each `stretch(1)`), its natural width the column count
  times its widest cell".
- `document.md:267`: `columns` gives that many equal columns.

### Tests

- **Conformance: `gridColumnsAreEqual`.**
  - **Setup:** a `fixed(60)` grid, `columns: 2`, gap 0, holding `sizedText("a")` and a 30-character `sizedText`.
  - **Assert:** both cells are 30 units wide, within one. At gap 1, the second cell's `x` is the first's width plus
    one step.
  - **Qt control:** with the `GridLayout` path restored, it fails, at about 2 and 58 units (**inferred** from the
    mechanism).
  - **Terminal control** (R-S1): `planGrid(int, …)` building `content()` items from the cells' naturals gives 1 and
    30, and fails. Weight-only stretch items ignore `natural`, so feeding naturals to stretch items would not fail.
- **Conformance: `gridColumnsNaturalIsWidestTimesColumns`** (B1).
  - **Setup:** a content-sized `columns: 2` grid, gap 0, in a column with `align: start`, holding `sizedText("a")`
    and a 10-character `sizedText`.
  - **Assert:** the grid is 20 units wide, within one, and the second cell is at least 10 wide.
  - **Control:** the Σ-naturals rule gives 11 and 5.5, and fails.
- **Qt Quick** (`test_qt_quick_containers.cpp`):
  - the existing test at `:130` passes unchanged, and the commit says so;
  - **span:** a span-2 cell of 30 characters makes a content-sized 2-column grid 30 units wide (**control:** without
    the span term, it is narrower);
  - **max clamp:** a stretch cell with `maxWidth: 10` in a 60-unit, 2-column grid is 10 units wide at its track's
    start (**control:** 30);
  - **re-placement:** `setColumns(3)` after adoption gives three equal columns, and `setGap(3)` after adoption moves
    the second cell (**control:** without `onGapChanged`, it stays).
- **Gallery** (P-S, item S15; R-S2): receipt's grid is `gap = 1`, which is 4 px. Column 2 starts at
  `244 + (634 − 4) / 2 + 4 = 563` at 1215 and about 916 at 1920. Each is accepted within 2 px. The mockup's 568 comes
  from its wider gap; see A7.

### What P-F hands back for P4

Grabs of:
- receipt at 1215, 1920 and 390;
- stability at 390;
- the e-signature;
- `examples/gallery`'s Containers page;
- a forms example with `columns` above 1 (`examples/forms`), in a dialog with no fixed width.

## P5: the `caption` text role

### Decision

- **The role.** `TextRole::Caption` goes at the end of the enum: "a note under a control or a figure:
  `font.size.caption`, regular, muted". Its document name is `caption`.
  - It joins `kRoles` (`document.hpp:4232`, whose size becomes 12), the interpreter's role mapping, and the author
    API's role names.
  - A table column's `role` takes it as well.
- **Qt Quick.** `MorphText` maps role 10 to `Theme.captionFont`. `textColorOf` gives it `scheme.muted`.
- **Terminal.** It draws it as muted (`roleStyle`, `leaf_widgets.cpp:30-48`).
- **The recording backend** (R-S10). `enumName(TextRole)` (`recording_backend.hpp:292-316`) gains `Caption`. Its
  `default:` branch would otherwise record it as "unknown".
- **Spec:**
  - `document.md:259`;
  - `view_tree.md`;
  - `qt_quick/backend.md:161`;
  - `tui/frontend.md`, the Text row;
  - `theme.md:197`: `font.size.caption` also feeds this role.
- **Not covered by P5:** instrument's "(needs reason)" is a menu entry's description, not a text node (R-S9).

### Tests

- **Document:** `caption` is accepted, and unknown roles are still refused.
- **Interpreter:** it lowers to `Caption`.
- **`RecordingBackend`:** a caption text records `Caption` (**control:** without the case, "unknown").
- **Qt Quick:** the label's `pixelSize` is `captionPixels` and its colour is `scheme.muted`. **Control:** mapping it
  to `Normal` fails.
- **Terminal:** it draws in the `textMuted` style. **Control:** the same mapping fails.

## P6: stacked key-value captions at the caption size

### Problem

R-S9 found this. On the phone, instrument's stacked details draw each caption at 12 px, muted. `MorphKeyValue` draws
a stacked caption in `Theme.bodyFont`. A form's caption above its control already uses `Theme.captionFont`
(`MorphField.qml:73`). So `fieldLayout: stacked` means "a form's caption" on one kind and "body text" on another.

**Evidence:** read from `03-Instrument.dc.html`; not measured. P-S measures it on `instrument-390x844.png` before and
after.

### Decision

- **Qt Quick.** A stacked caption (`objectName: "caption"`) takes `Theme.captionFont`, as a form's caption does. The
  inline captions keep the body font, because the designs draw inline captions at the body size.
- **Terminal.** Unchanged: one size.
- **Spec:** `qt_quick/backend.md:186`.

### Tests

- **Qt Quick:** a stacked caption's `pixelSize` is `captionPixels`, and an inline caption's is `bodyPixels`.
- **Control:** reading `bodyFont` again fails.

## Lane P-S: screen items

P-S owns `examples/lims_gallery`. Items S1 to S10 are unchanged from the first version, and P-S has started them.

| # | Screen | Finding | Change | When |
|---|---|---|---|---|
| S1 | instrument | M: Qualification rows 22 px apart with no rules, against 29 with rules | `qualificationList()`: `{.gap = 2, .divided = true}` | now |
| S2 | instrument | L: event rows 27 px apart against 29 | add 2 px to the event row pitch with a step or the rows' padding | now |
| S3 | instrument | L: panel insets 8 px against `12px 14px` | the four cards: `.padding = 3` | now |
| S4 | stability | L: First pull breaks after the comma | U+00A0 after the comma in "(40 °C, 1 m)" | now |
| S5 | stability 390 | L: Add condition and the Summary 8 px apart against 12 | `gap = 3` on that column | now |
| S6 | results | L: Submit disabled under the dialog | enabled while the reason dialog is open; the handler still requires the reason | now |
| S7 | results | L: "SOP- / CHR-012" breaks at the hyphen | U+2011 | now |
| S8 | review 1215 | L: the comment card 1 px under the table against 11 | the `room` spacer, or a step-3 gap | now |
| S9 | console 1215 | L: result inputs 62 px against 75 | 2 units wider `fixed(...)`; 1000 must not clip | now |
| S10 | console 1215 | L: Instruments rows 22 px apart against 26 | a larger `gap` step | now |
| S11 | e-signature, instrument, stability | L: caption column | `captionWidth` 12, 17 and 13 | after P2 |
| S12 | plate | M: wells 44 px on the phone; 30 against 40 on the desktop | the theme sets `button.smallHeight` 40 and `button.smallTouchHeight` 36. **The compact run line's padding goes from `padding(0, 1)` to `padding(1, 1)`** (R-S3), so a row without Assign is 36 + 8 = 44 px plus the rule: a 45 px pitch. **Measure:** the phone pitch of rows A1, A2 and A9 (no Assign) and of a row with Assign, both 45; at 1215, 40 × 40 wells; at 1000, wells about 35 × 35 (the round rule, R-S4), with no ring overlapping its neighbour | after P3 |
| S13 | every screen at 390 | L: textarea 13 against 14 | the theme sets `control.touchTextSize` 14. Re-grab all ten screens at 390. A row of buttons that wraps or clips is reported, not narrowed by hand | after P1 |
| S14 | review 1215 | L: self-review note | `.role = "caption"` | after P5 |
| S15 | receipt and the other `columns` grids | L: Condition columns at 1.27 | **No screen change**: `.columns = 2` stays as the detector. **Accept** column 2 at 563 ± 2 (1215) and 916 ± 2 (1920) (R-S2). **Re-grab** receipt's field rows at 1215 and 1000, receipt's containers/courier pair at 390 (`receipt.cpp:449`), stability's month boxes at 390 (`stability.cpp:628`) and the e-signature's fields (R-S12) | after P4 |
| S16 | console, expanded and compact | the KPI notes: body against 12 px muted | `.role = "caption"` in place of `layout::muted(note)` (`console.cpp:487`) | after P5 |
| S17 | console | "mean 100.0 % · SD 0.4": body against 12 px | `.role = "caption"` (`console.cpp:524`) | after P5 |
| S18 | review | "Approving needs your signature": body against 12 px | `.role = "caption"` | after P5 |
| S19 | instrument 390 | stacked captions: body against 12 px (**read** only) | no code; measure the caption size before and after P6 | after P6 |

**Standing checks for every item:**
- the gallery's unit and render tests are green;
- the render smoke test logs no Qt warning;
- each fix is measured on the grab it targets;
- 1000 is checked for clipping.

Where a step overshoots the design by more than 2 px, P-S reports the measured value instead of inventing a token.

## Accepted, no work

| # | Finding | Why |
|---|---|---|
| A1 | review 1000: Analyte and Evaluation elide | The coordinator's decision. The page is 785 px, so this is a fit difference |
| A2 | receipt: "Sample …" heading 13 against 14 px/600 | 14 px/600 appears only on this heading. Every other group heading in the mockups is 13 px/600, which `section` draws |
| A3 | console: head 53 against 49 px | Neither a theme value nor a framework cause (Q6). Changing `head.paddingY` would move eight padded screens to fix one, and the review agrees |
| A4 | samples: bulk bar at 70 against 54 | The known head delta |
| A5 | audit: rail 210 against 220 | Batch O's trade-off |
| A6 | data: plate's run grouping, results' "of 2", review's 7 against 24 | Fixture data |
| A7 | receipt: Condition column 2 at 563 against the mockup's 568 | One `gap` step serves both the rows and the columns. Step 1 (4 px) matches the mockup's row spacing, while its column gap is about 14 px. Moving to step 3 would bring the columns within 1 px and push the rows 8 px further apart than the design |
| — | fit notes at 1000; receipt's focus halo; review's queue 270 against 280 | Not counted by the reviewers |

## Lanes

- **P-F.** One Sonnet lane, in its own worktree from `lg-fold-o`. It owns `include/morph`, `src`, `tests` and
  `docs/spec`.
- **P-S.** It is working in `morph-lims-e2` and owns `examples/lims_gallery`. It has S1 to S10 in hand, and it rebases
  onto P-F's branch for S11 to S19.
- **Shared files:** none.
- **Builds:** every build, test and tidy run goes under `flock $S/morph-build.lock`, with `-j10` at most.

### P-F, one commit per item, in this order

1. **P4:**
   - `MorphGrid.qml` and the `kinds.hpp` comments;
   - the two conformance cases and the Qt tests;
   - the specs.
   - It hands back the grabs listed under P4.
2. **P2:**
   - `view.hpp`, `author.hpp`, `document.hpp`, `interpreter.hpp`, `mount.hpp`, `backend.hpp`;
   - the four backends;
   - the forms spec;
   - the tests and the specs.
3. **P1:** `theme.hpp`, `theme.cpp`, `Theme.qml`, the five controls, the tests and `theme.md`.
4. **P3:** `theme.hpp`, `theme.cpp`, `Theme.qml`, `MorphButtonBackground.qml`, `control_shape.hpp`, the tests and the
   specs.
5. **P5:** `view.hpp`, `document.hpp`, `interpreter.hpp`, `author.hpp`, `MorphText.qml`, `MorphWidget.qml`,
   `leaf_widgets.cpp`, `recording_backend.hpp`, the tests and the specs.
6. **P6:** `MorphKeyValue.qml`, a test and a spec line.

**Before reporting done,** P-F re-runs:
- every touched suite;
- the three conformance runners;
- the docs target under clang with `WARN_AS_ERROR`;
- clang-tidy on the touched TUs;
- qmllint.

**Mutations, one or more per item, each reported as failing:**
- P4: the `GridLayout` restored; Σ-naturals; the span term removed; the max clamp removed; `onGapChanged` removed;
  the terminal's `content()` items;
- P2: the setter a no-op; the mount check removed;
- P1: the text area reading `bodyFont`; the strong branch reading `buttonFont`;
- P3: `targetHeight`; the round rule removed;
- P5: `Caption` mapped to `Normal`; `enumName` without the case;
- P6: the stacked caption reading `bodyFont`.

## Risks

- **P4 moves every `columns` grid on Qt Quick:** receipt, stability, the e-signature, `examples/gallery:91`, and
  every `form`, `collection` and `wizard` with `columns` (`form_view.hpp:493-503`).
  - With B1's natural width, a content-sized grid is as wide as the terminal makes it, so none squeezes its own cells.
    A grid placed narrower than that still cuts a wide cell, as the terminal does.
  - The render tests check warnings, not geometry, so P-F hands back the grabs listed under P4.
- **P4 drops the height sharing.** A grep (a weak check) finds no stretch-height grid in the examples.
- **The Qt version stays open.** P4's mutation control runs only on 6.12 here. The issue's condition ("passes on 6.8.1
  without the fix") stays unverified until CI runs the new case on a revert. The issue says so (R-S12).
- **P1** makes phone buttons and segments about 7 % wider. S13 checks every screen.
- **P2's pure virtual** breaks out-of-tree backends at compile time, which is intended (0.x).
- **P3 applies to every small button in a theme.** The gallery's 40 px reaches the next small button anyone adds.
  `theme.md` and the `Small` Doxygen say so (R-S5). Its phone height below the touch target is the theme's explicit
  choice. Every round button, in any theme, now draws a circle when it is narrower than tall.
- **CI traps:**
  - Doxygen under clang (`setCaptionWidth`, the new struct members);
  - MSVC `typename`;
  - Qt keywords as identifiers;
  - GCC 15 `-O3`.

## Decisions

1. **The phone's control text is the theme token `control.touchTextSize`.** It applies through
   `controlFontOf(weight)`, so strong captions and marked segments follow it.
2. **`captionWidth` is the document spelling of the inline caption column**, on `keyValue` and on the forms engine's
   `inlineLabelWidth`. It is literal units, refused without inline captions, and refused on the C++ path too. Qt
   wraps a long caption; the terminal cuts it with "…".
3. **A small button's height is button-scoped: `button.smallHeight` and `button.smallTouchHeight`.**
   - `size: small` is redefined as a compact caption whose height may exceed the control's.
   - A round button never draws taller than it is wide.
   - Surface-scoped heights are rejected for now.
4. **P4 is a defect.** Fixed as `columns` meaning `stretch(1)` tracks, with the terminal's natural width,
   re-placement on gap and theme changes, and a max clamp. The terminal is the reference.
5. **The `caption` role** goes to every 12 px muted note in the evidence.
6. **Stacked key-value captions use the caption font**, as a form's do.
7. **Accepted:** A1 to A7.

## Records

- **To file:** P4, from `lg-p-issue-grid.md` (updated for B1 and for the Qt version), labelled `bug` and
  `area: qt_quick`, with no `triage:` label.
- **Not filed:**
  - P1, P2, P3, P5 and P6 are closed in their own change, which is smaller than an issue describing it would be;
  - the screen items are example fixes, which are not tracked.
- **Watched:** S2, S8 and S10 each adjust a list's row pitch by a gap step. A third screen that needs a pitch no step
  gives is the signal for a list-row token.
- **For the dispatcher** (the review's note): `morph-lims-e2` had uncommitted S1 and S3 in `instrument.cpp` at 00:09.
  That is lane P-S's work, as expected.

## Review round 2

| Item | Change |
|---|---|
| **B1** P4's natural width squeezes content-sized grids | Without tracks, the natural width is `columns × unit + gaps`, with `unit` the widest cell (a span divided out), exactly the terminal's `GridImpl::naturalSize`. New conformance case `gridColumnsNaturalIsWidestTimesColumns` on a content-sized grid (20 units; the Σ control gives 11 and 5.5), and a Qt span test. Risks name `examples/gallery:91` and the forms engine's `columns` grids. The P4 hand-back adds the gallery Containers page and a forms example in a dialog |
| **R-S1** the terminal control does not fail | The terminal is the reference. Its control is `planGrid(int, …)` building `content()` items from the cells' naturals (1 and 30), which fails. The weight-only control is dropped, with the reason stated |
| **R-S2** acceptance used the mockup's gap | 563 ± 2 at 1215 and 916 ± 2 at 1920, computed with `gap = 1` (4 px). The 5 px to the mockup is A7, accepted, with the reason |
| **R-S3** phone rows without Assign shrink | Item S12 sets the compact run line to `padding(1, 1)`, giving a 45 px pitch, and measures rows with and without Assign |
| **R-S4** 35 × 40 wells at 1000 | Decided before the lane starts: a framework rule. A round button never draws taller than it is wide; when placed narrower, its background and rings are a circle of its width, centred. S12 expects about 35 × 35 at 1000 |
| **R-S5** P3's name inverts its meaning | Option (a), renamed. The tokens are `button.smallHeight` and `button.smallTouchHeight`. `size: small` is redefined as a compact caption whose height may be above or below the control height, in the Doxygen and in `theme.md`. Risks state that the gallery's 40 px reaches every future small button. Option (b) is rejected for now |
| **R-S6** strong fonts at the touch size | `Theme.controlFontOf(weight)` returns `bodyFont`, `strongFont` or `buttonFont` at the body size, and `fontOf(touch, weight)` otherwise. It serves normal and strong captions and the marked segment. The unit label follows `textFont`. Radio labels are left alone. The test adds a primary button and a marked segment |
| **R-S7** one name with `inlineLabelWidth` | `captionWidth` is the document spelling of the inline caption column. `form`, `collection` and `wizard` take it and map it to `inlineLabelWidth`. The wrap rule is stated for both, and the forms spec names it |
| **R-S8** the C++ path; the key table; a test that cannot fail | The mount throws on an engaged `captionWidth` with `Above` or below 1, with a test and a control. `kKeyValueKeys` and the form tables gain the key. The `x`-equality check is labelled a guard |
| **R-S9** `caption` applied to one site | Screen items S16 to S18 (console KPI notes, expanded and compact; the control-chart note; review's signature note). The instrument menu hint is recorded as outside P5. The stacked key-value captions become P6, read-only evidence, measured by S19 |
| **R-S10** a silent switch in the recording backend | `enumName` gains `Caption`, with a recording test and a control. `recording_backend.hpp` and `leaf_widgets.cpp` are in P5's file list |
| **R-S11** gap and theme changes do not re-place | `onGapChanged` and a `Connections` on `Theme` (`unitWidth`, `unitHeight`, `space`) call `place()`, with a `setGap(3)` test and a control |
| **R-S12** accuracy | `kinds.hpp:344-366`'s comments are in P4's list. P1's test sets a font family and compares against `Theme.bodyFont.pixelSize`. Item S15 re-grabs `receipt.cpp:449` and `stability.cpp:628` at 390. The issue and Risks say that the 6.8.1 condition stays unverified until CI runs the case. The review's corrected file count for Q4 (10 and 5) is in Q4 |
| Note: e2 has uncommitted S1/S3 | Expected: that is P-S's work. Recorded under Records |
