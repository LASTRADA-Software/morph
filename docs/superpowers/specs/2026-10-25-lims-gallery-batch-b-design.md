# LIMS gallery batch B: tables

Batch B of the LIMS gallery initiative (`examples/lims_gallery/design/batches.md`): the table vocabulary a LIMS
needs for sample lists, worklists, result worksheets, audit trails and plates. It lands on `feat/lims-gallery` as one
commit per item, in the order below; the gallery drops each workaround in the same commit as the feature that makes
it unnecessary.

Every claim below was re-checked against the code at `b080a5265` (master `842701f0d` plus the gallery and batch A).
Each item says whether its premise was **reproduced** (run on this revision) or **read** (inferred from the code).
The reproductions come from one throwaway Catch2 test, built against the worktree's headers with the corpus harness
(`tests/ui_interpreter_support.hpp`) on `RecordingBackend`, run and then kept out of the tree. Its output is quoted
where an item rests on it.

| Item | Where it lands | Verdict on the gap log |
|---|---|---|
| G2 (cells) | `TextRole`, theme, both frontends | Right; the role belongs to every text, not only to a column |
| G47, and per-column vertical alignment | column part, `TableColumn`, both frontends | Right; the same workaround is in two screens, so it is a framework seam |
| G29 | loader, interpreter, author API | Right, reproduced again; `sort`, `filters` and `highlightKey` are dropped the same way, and the author API cannot write the key at all |
| G43 | the table's selection model on every backend | Right that a click has no modifiers. The table gets ranges natively; a button's modifiers stay out of ui/1 |
| G30 | a select column drawn by the table | Right; met by the table's own selection, and a general header control stays out of ui/1 |
| G31 | expressions, loader, interpreter, `TableWidget`, both frontends | Right, reproduced: `sort` is accepted, ignored and reads `null` |
| G28 | expressions, loader, interpreter | Right; paging by slicing the rows would also lose the selection (reproduced), so a table pages itself |
| G44 | expressions | Right |
| G46 | loader, interpreter, `TableWidget`, both frontends | Right |
| G41 | loader, mount, both frontends | Right, reproduced. The selected look lands; the circle does not |

Order of landing: G2, G47 (column options, no state), then G29, G43 and G30 (the selection, in the order each builds
on the last), G31 and G28 (sort and page, which share the lowering's row pipeline), G44 and G46 (runs, which the group
rows reuse), and G41, which is a button change and touches no table code.

**Two parts.** The batch is more than one implementation lane carries: ten items, six new `TableWidget` virtuals,
five conformance cases, three layout cases, three expression forms and a keyboard model for Qt Quick tables. It lands
as two pull requests on `feat/lims-gallery`, each green on its own:

- **Part 1, columns and the selection:** G2, G47, G29, G43 and G30. Conformance cases 41 to 43, layout cases L8 and
  L9, and `setSelectColumn`, `setSelectAllLabel` and `setRowLabel`. G30's paged Ctrl+A test waits for part 2.
- **Part 2, the row pipeline:** G41, then G31, G28, G44 and G46. Conformance cases 44 and 45, layout case L10, and
  `setSort`, `setOnSort` and `setRowHeading`. G41 comes first because it touches no table code.

## The shape of the batch

Three pieces of table state appear in this batch: the selection, the sort and the page. They follow one rule, so a
document author learns it once.

- **The state lives in the screen.** A document holds the selection, the sort and the page in its own `state`. That
  is what lets the expanded table and the compact card list of design 05 share one selection and one order. It is
  also what lets a "Clear" button, a bulk action or a filter change them with an ordinary `set`.
- **The table reads it through a key of the same name.** `selection`, `sort` and `page` are expressions. A bound key
  is *controlled*, as a select's `selected` is (`docs/spec/ui/interpreter.md`, Relations): the table shows the bound
  value, and nothing else. As for a select (`interpreter.hpp:4358`), any expression but a literal `null` binds the
  key; a literal `null` is no binding.
- **What the user changes through the table itself comes back as an event.** The user changes the selection by
  clicking rows and the sort by clicking a header, so the table has `onSelectionChange` and `onSortChange`. Each
  runs with `event` set to the new value. The value changes only when the document writes it back. The user changes
  the page through the document's own pager, not through the table, so there is no `onPageChange`.
- **Without a key, the table holds the value itself.** A table with no `selection` key keeps the user's selection, as
  it does today, and a table with sortable columns and no `sort` key sorts on a header click by itself. The element
  values (`samples.selection`, `samples.sort`, `samples.page`) read the bound value when there is one, and the
  table's own otherwise.
- **`set` never names a table.** There is one way to change a table's state from a command: bind the key and set the
  state. The loader's refusal says so.

- **The state and the pipeline belong to the element.** Element values are read wherever the screen reads them
  (`elementValue`, `interpreter.hpp:4539-4557`), including from a pager that mounts before the table. So the table's
  own selection and sort, the bound values, and the sorted, paged row list are made when the element is wired, beside
  the `rows` computed it has today (`interpreter.hpp:4400-4403`). The lowering's rows slot reads the page from that
  computed, and the pager reads the same one, so the two never disagree and `rows` is evaluated once per change.

Rows are in memory. The interpreter sorts and pages the list `rows` evaluates to. Nothing here reaches a server, and
the model-backed table (`docs/spec/table/engine.md`) is untouched.

**The expression version.** The forms `sort` and `runs` and the function `slice` join `expr/1`, the version the
loader and the corpus treat as current (`kExprVocabulary`, `include/morph/ui/bundle.hpp:36`), in place, as
`concatLists` did. `expressions.md`'s rule that new vocabulary belongs to a new version (`expressions.md:10-11`) is
restated to match: forms and functions are added to the current version, and a document that uses one needs a
client that has it. The cost is that a client older than this batch refuses such a document at load, since an
unknown form or function is a load error; a server that serves older clients keeps those documents free of the three.

Design 05's table, written with the whole batch:

```json
{ "kind": "table", "id": "samples", "testId": "samples",
  "rows": { "ref": "shown" }, "key": { "row": "id" },
  "selectionMode": "multiple", "selectColumn": true, "selectAllLabel": "Select all on this page",
  "rowLabel": { "row": "id" },
  "selection": { "ref": "selected" },
  "onSelectionChange": [ { "set": "selected", "value": { "ref": "event" } } ],
  "sort": { "ref": "order" },
  "onSortChange": [ { "set": "order", "value": { "ref": "event" } } ],
  "pageSize": { "ref": "pageSize" }, "page": { "ref": "page" },
  "columns": [
    { "id": "id", "label": "Sample", "width": { "lit": { "fixed": 12 } }, "role": "mono", "sortable": true },
    { "id": "due", "label": "Due", "width": { "lit": { "fixed": 13 } }, "sortable": true,
      "sortBy": { "row": "dueAt" }, "cell": { "kind": "text", "text": { "row": "due" } } } ] }
```

with `"order": { "init": { "lit": [ { "column": "due", "direction": "asc" } ] } }` in the screen's state, and a pager
drawn from `samples.pageFirst`, `samples.pageLast`, `samples.viewCount`, `samples.page` and `samples.pageCount`.

## G2 (in cells): a monospace text role

**Problem (read).**

- A column's `role` is a `TextRole` (`include/morph/ui/view.hpp:883`), and the roles are `Normal`, `Muted`,
  `Heading`, `Error`, `Success` and `Section` (`view.hpp:199-206`, `docs/spec/ui/document.md:164`).
- The theme has one `font.family` (`docs/spec/ui/theme.md:132`). Sample, lot and worksheet ids are drawn
  proportionally. The mockups set them in `.mono` (`05-Samples.dc.html`: `font-family:"JetBrains
  Mono",ui-monospace,monospace`).
- A column's role is not checked at all. The loader's role check runs for a `text` node only (`checkTextRole`,
  `include/morph/ui/document.hpp:3028-3029`, `:3231-3245`), and the lowering reads a column's role through `roleOf`,
  which turns an unknown name into `Normal` without a report (`interpreter.hpp:5602-5603`, called at `:7188`). So a
  column with `role: "monospace"` loads and draws as body text, silently.

**Decision: one new text role, owned here for every text.** A column's `role` *is* a text role, read through the
same names (`roleNamed`, `interpreter.hpp:5576-5595`). A mono value valid only on a column would need a second role
type, or a rule that one role is refused on a `text`. So `mono` joins the role set, and serves a `text` node and a
column alike. This item owns the `mono` role and the `font.mono` token. Batch C's G2 and G3 build on them: its `mono`
span uses this token and this face, and adds no role or token of its own.

**Vocabulary.**

- `text`'s `role` and a column's `role` gain `mono`: body size, the theme's monospace family, the text colour.
- JSON: `{ "id": "id", "label": "Sample", "role": "mono" }`, or `{ "kind": "text", "text": …, "role": "mono" }`.
- Author: `TableColumn::role = "mono"`, `TextOptions::role = "mono"`; no new member.
- The size is the body size. The mockups set mono 1 px under the body (12 px against 13 px); no `font.size.mono`
  token is added. It can join the closed set later, "with the components that read it" (`theme.md`).

**Lowering and validation.**

- `roleNamed` maps `"mono"` to `TextRole::Mono`, so `roleOf` and a text's computed role (`roleProp`,
  `interpreter.hpp:5774-5787`) read it. The computed role's report ("a role is normal, muted, heading, section, error
  or success") lists `mono` too.
- `checkTextRole`'s closed set gains `mono`, and the loader runs the same check on every column part. A literal
  column role outside the set is refused, as a text's is.
- A column's computed role is evaluated once, as today. A value that names no role is reported once for the column's
  `role` path and draws as `Normal`, as a text's computed role is reported.

**View tree and backends.**

- `TextRole::Mono` is added after `Section`, so no existing value's number changes. QML reads the role as an `int`,
  so `Mono` is `6`.
- **Theme.** A new token, `font.mono`, of type `fontFamily`: a name, or a list of names in order of preference, as
  `font.family` is. Its default is none, which uses the platform's fixed-pitch font.
  - It is a sibling of `font.family`, not `font.family.mono`. `readGroup` treats an object with a `$value` as a token
    and never descends into it (`include/morph/ui/theme.hpp:563-565`), so `font.family.mono` beside a `font.family`
    would never be read.
  - `kThemeTokens` gains it (`theme.hpp:1030` holds `font.family`), and `themeValues` gives its family list in
    `families`.
- **Qt Quick.**
  - `Theme` gains `monoFamilies`, from `font.mono`. While it is empty, `applyTheme` writes the family of
    `QFontDatabase::systemFont(QFontDatabase::FixedFont)` as `platformMonoFamily`, and `monoFamilies` is that one.
  - `Theme` gains `monoFont`: those families at `Theme.bodyPixels` (`Theme.qml:91`), the size every body text uses.
    `bodySize` is the raw token and is 0 by default, so it is not the size to read.
  - `MorphText` draws `role === 6` (`Mono`) in `Theme.monoFont`.
  - A family Qt cannot resolve is reported as `font.family`'s is (`theme.md:177`): `font.mono: none of <names> is
    installed`.
- **Terminal.** A terminal draws every cell in one fixed-pitch font, so `Mono` draws as `Normal`.
- **`RecordingBackend`.** Records `role=Mono` through the existing `role` property.

**Spec.** `document.md:164` (the role list) and `:180` (a column's `role` takes the same names, checked the same way);
`view_tree.md`'s `Text` row; `theme.md` "The closed set" gains `font.mono` with the sibling rule, and "On Qt Quick"
maps it to `monoFamilies` and `monoFont`; `qt_quick/backend.md`'s text row; `tui/frontend.md`'s Text row says `Mono`
draws as `Normal`.

**Tests.**

- `tests/ui/corpus/documents.json`: `role: "mono"` loads on a `text` and on a column. `role: "monospace"` is refused on
  both, at the `role` path, with "`monospace` is not one of the values of `role`" (`document.hpp:3242-3243`).
- `tests/test_ui_interpreter_control.cpp`: a column with `role: mono` records `role=Mono` on its default cell. A
  column whose role is computed as `"monospace"` records `role=Normal` and one diagnostic at its `role` path.
- `tests/test_ui_theme.cpp`: one token file sets both `font.family` and `font.mono` as siblings, and `themeValues`
  gives both family lists in `families`. **Control:** name the mono token `font.family.mono` instead, and the mono
  family is the default: `readGroup` reads `font.family` as a token and never descends into it, so that token is
  never read.
- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - The test finds an installed fixed-pitch family rather than naming or bundling one: no font is bundled in the
    tree, and the CI image and a workstation ship different fonts. A helper beside `otherFamily`
    (`test_qt_quick_theme.cpp:324-339`) walks `QFontDatabase::families()` and returns the first family that resolves
    to itself (`QFontInfo::family()`) and whose `QFontInfo::fixedPitch()` is true. The test `REQUIRE`s one, so a
    machine without a monospace font fails instead of passing on a proportional fallback.
  - Under a theme whose `font.mono` is that family, `Theme.monoFamilies[0]` is that family, a `Mono` text's
    `font.family` is that family, and a `Normal` text's is not.
  - With no `font.mono`, a `Mono` text's `QFontInfo::fixedPitch()` is true: the platform's fixed font.
  - `font.mono` set to `NoSuchFont` reports `font.mono: none of NoSuchFont is installed`.
- **Controls:**
  - map `mono` to `Normal` in `roleNamed`, and the interpreter case reads `role=Normal`;
  - leave `MorphText`'s font alone for `Mono`, and the Qt Quick family check and the fixed-pitch check fail;
  - drop the column check from the loader, and the column `monospace` case loads.

**Gallery.** Ids become mono: design 05's Sample column, 08's and 09's Sample columns, 04's Record column, and the
plate's well labels.

**Risks.**

- A mono face is wider than a proportional one, so a content column of ids grows a little. Fixed columns holding ids
  (the gallery sizes them 12 or 13 units) were sized for the proportional face and are re-checked in the render
  smoke.
- A document whose column role was misspelt now fails to load instead of drawing body text. No column in the tree
  sets a role outside the set: `grep -rn '\.role = "' examples` finds only names of the set, and the plate's
  `well.role`, which is a data field.

## G47 and vertical alignment: a column's alignment

**Problem.**

- **G47 (read; the grab stands).** A column part is `{id, label, width, role, cell}` (`document.md:180`,
  `kColumnKeys`, `include/morph/ui/document.hpp:824-828`), and `author::TableColumn` has no alignment
  (`include/morph/ui/author.hpp:3672-3683`). Results and review both wrap a number in a `row` with
  `justify: end` (`examples/lims_gallery/src/screens/results.cpp:436-438`, `numberCell`;
  `review.cpp:488-490`, `endCell`), and the header label stays at the start. The same hand-rolled guarantee in two
  screens is the signal the framework is missing it (`AGENTS.md`, The bar).
- **Vertical alignment (read).** Batch A centres every cell in its row (`MorphTableRow.qml:42-62`;
  `StackImpl::childAreas`, `src/tui/container_widgets.cpp:59-68`, forces `Align::Center` for a table row). Design 04's
  audit trail draws every cell at the top (`04-Audit.dc.html`: `td{…;vertical-align:top;…}`), where the When and
  Record columns are two lines tall beside one-line cells. Batch A deferred this option to here.

**Vocabulary.**

- A column part gains `align`, the cell's place across the column: `start` (the default), `center` or `end`.
- A column part gains `valign`, the cell's place down the row: `start`, `center` (the default) or `end`.
- The table gains `valign`, the default for every column that does not give one.
- All three are literal names from a closed set, as a stack's `align` is (`document.md`, Load-time rules).

```json
{ "id": "area", "label": "Peak area", "width": { "lit": { "fixed": 10 } }, "align": "end" }
```

Author: `TableColumn::align` and `TableColumn::valign`, and `TableOptions::valign`, each a `std::optional<Align>`,
the type a stack's `align` already takes (`author.hpp:3365`, written through `alignName`, `:519`). An absent value
writes no key. `Align::Stretch` writes `stretch`, which the loader refuses for a column, as it refuses any other name
outside the three.

What `align` does to a cell, by kind:

- A **text** cell (the default cell, or a `text` node) draws its text at the column's start, centre or end. The text
  still fills the column, so a long text wraps or elides as before.
- A **text input** cell aligns the text it shows and edits, where the frontend can (below). The design asks for this
  for the result field (`08-Results.dc.html`: `.input.num{text-align:right}`).
- A **label-width** kind (a badge, a check box, a push button; `fillsAcross` false) sits at the column's start,
  centre or end at its natural width. Batch A places it at the start.
- **Any other** cell (a `row`, a `column`, a select) fills the column, as now, and places its own content. A column's
  alignment does not reach into a container, which has its own `justify`.
- The **header label** takes the column's `align`, so a number column's header sits over its digits.

**Lowering and validation.**

- The loader checks `align` and `valign` against their names, on a column part and on the table.
- The lowering sets `TableColumn::align` and `TableColumn::valign`, the column's own `valign` first and the table's
  second. They are set once, with the rest of the columns.
- An expression `columns` (`{id, label}` items, `interpreter.md:334-336`) reads `align` and `valign` from each item
  too. A value that is not one of the names is reported once for the table and taken as the default.

**View tree.**

- `TableColumn` gains `Align align = Align::Start` and `Align valign = Align::Center`. `Align::Stretch` is treated as
  `Start` across the column and as filling the row's height down it. Only C++ can ask for that; the document offers
  three names.
- The mount passes them through `setColumns`, which is unchanged.

**Backends.**

- **Qt Quick.**
  - `TableImpl::setColumns` (`src/qt_quick/table.cpp:30-38`) adds `align` and `valign` to each column map.
  - `place()` (`MorphTableRow.qml:42-62`):
    - A label-width cell keeps its width and is placed at `x + inset`, at the centre, or at `x + w − inset − width`.
    - Every cell's `y` follows `valign`: `0`, the centred value it has today, or `rowHeight − height`.
    - A cell that declares `textAlign` (`MorphText`, `MorphTextInput`) is given `Qt.AlignLeft`, `Qt.AlignHCenter` or
      `Qt.AlignRight`, mirrored under a right-to-left layout as Qt's own alignment is. `MorphText` maps it to its
      label's `horizontalAlignment` and `MorphTextInput` to its field's.
  - The header `Label` (`MorphTable.qml:252-268`) takes the column's `horizontalAlignment`.
  - An `end`-aligned column also draws its text with tabular figures (`font.features: {"tnum": 1}`), so digits line
    up across rows. The designs set `font-variant-numeric: tabular-nums` throughout.
- **Terminal.**
  - `StackImpl::setColumnLayout(extents, gap)` becomes `setColumnLayout(extents, gap, aligns)`, one `{across, down}`
    pair per column, and `childAreas` places each child with its own pair instead of the forced `Align::Center`.
  - A text cell's area is its natural width at the column's start, centre or end, clamped to the column. A text
    narrower than the column is then drawn where the column asks; a wider one fills the column and wraps as now.
  - The header line (`TableImpl::paint`, `src/tui/table_widget.cpp:279-286`) offsets each label the same way.
  - A text input keeps its text at the start: `core::tui::InputField` has no alignment (`grep -n align
    …/core/tui/InputField.hpp` finds nothing). The contract allows that.
- **`RecordingBackend`.** `columns` records a column's alignment only when it is not the default, as `;name=value`
  after the column, the way a menu entry records its optional fields (`recording_backend.hpp:87-110`):
  `"Peak area":fixed(10);align=End`, `"When":fixed(11);valign=Start`. A table that says nothing records exactly what
  it records today.

**Spec.**

- `document.md:180`: the column part's keys and the table's `valign`.
- `view_tree.md:120`: `TableColumn{label, width, role, align, valign}`.
- `backend_contract.md:178-183` (**Table cells**): "every cell keeps its own height and is centred in its row"
  becomes "is placed down its row by its column's `valign`, centred by default". A new sentence: a column's `align`
  places a label-width cell and the text of a text or a text input, and the header's label. A container cell fills
  its column.
- `qt_quick/backend.md:198-212` and `tui/frontend.md:95` say the same, and the terminal's field exception.

**Tests.**

- **Layout conformance cases**, run on Qt Quick and the terminal (`layoutConformanceCases`, 7 to 9):
  - L8: in a 30-unit column with `align: End`, a badge cell's right edge is the column's right edge less the
    inset, within a unit.
  - L9: in a row where one cell is three lines tall, a one-line text cell in a `valign: Start` column has its top at
    the row's top, and one in a `valign: End` column has its bottom at the row's bottom, within a unit.
  - **Control:** ignore `align` in `place()` or in the terminal's `childAreas`, and L8 fails with the badge at the
    start; force `Center` again, and L9 fails.
- `tests/qt_quick/test_qt_quick_table.cpp`: an `end` column's text cell's `horizontalAlignment` is `AlignRight`, and
  its header label's too. A text input cell's field is right-aligned. **Control:** drop the `textAlign` write, and
  both read `AlignLeft`.
- `tests/tui/test_tui_table.cpp`: a 12-wide `end` column's "0.21" occupies its last four cells, and its header ends
  in the same cell.
- `tests/test_ui_interpreter_control.cpp`: a table with `valign: start` and a column with `valign: end` records
  `;valign=Start` for the first column and `;valign=End` for that one.
- `tests/ui/corpus/documents.json`: `align: right` is refused naming `start`, `center` and `end`.

**Gallery.** `numberCell` (results) and `endCell` (review) go; their columns, and the result input's column, are
`align: end`. The audit trail's table is `valign: start`.

**Risks.**

- A column whose cell is a `row` (`analyteCell`) is unaffected by `align`, which is the stated rule. A document that
  wants a composite cell at the end still writes `justify: end` on it.
- Right-to-left layouts mirror `start` and `end`, as Qt's alignment does. No test covers that here.

## G29: a table's selection binds (#1053)

**Problem (reproduced).**

- `table` lists a `selection` key (`kTableKeys`, `document.hpp:816`). The lowering binds the view's selection to
  the element's own signal (`interpreter.hpp:7257-7275`) and never reads the key.
- There is no `onSelectionChange` key, so a document cannot learn of a user's change except by reading the element.
- `set` cannot name a table (`checkSet`, `document.hpp:3400-3418`).
- The author API cannot even write the key: `author::TableOptions::selection` is the *mode* (`author.hpp:3725`,
  written as `selectionMode` by `tableNode`).

The throwaway test mounted a `multiple` table over three rows with `selection` bound to state `["a", "c"]` and `sort`
bound to `{column: name, direction: desc}`, plus `filters` and `highlightKey`:

```
B1 selection prop=[[]] hasProp=1 text=0|none
B1 row order: x z y
B1 diagnostics:
```

The probe prints the recorded property inside its own brackets, so `[[]]` is the property `[]`: the widget marks
nothing. `selectedCount` reads 0, the rows stay in source order, `t.sort` reads `null`, and
no diagnostic names any of the four keys. And `set` of the table:

```
B2 loaded=0 refusal=document b2 at /view/children/1/onClick/0: `set` writes state or an input's draft; `t` is a table
```

**Vocabulary.**

- `selection`: an expression for a list of row keys. Bound, it is controlled, as in [the shape of the
  batch](#the-shape-of-the-batch).
- `onSelectionChange`: commands run after each user change of the selection, with `event` set to the new selection,
  a list of keys. With a bound `selection`, nothing changes until these commands write the state back. Without a
  `selection` key, the table's own selection has already changed when they run.
- The element value `selection` is the bound value when `selection` is given, and the table's own otherwise.
  `selectedCount` counts it.

```json
"selectionMode": "multiple",
"selection": { "ref": "selected" },
"onSelectionChange": [ { "set": "selected", "value": { "ref": "event" } } ]
```

Author:

- `TableOptions::selection`, today the mode, is renamed `selectionMode`, the key it writes. `TableOptions` is shared
  by `table` and `dataTable`, so the rename covers both. Seven call sites change:
  `examples/bookmarks/screens/bookmarks_screens.cpp:155,221`, `examples/gallery/src/gallery.cpp:118`,
  `examples/lims/screens/lims_screens.cpp:396` and `tests/test_ui_author.cpp:690,698,702`. Two documents show the old
  name and change with them: `docs/GETTING-STARTED.md:645` and `docs/spec/ui/author.md:163`.
- `TableOptions` gains `AnyHandle selection` and `Commands onSelectionChange`.

**Lowering.**

- `options.selection` is a slot over the key when it is given (each item through `keyOf`), and over
  `element->selection` otherwise. It is a slot even for a literal list, so a literal selection is fixed: the user's
  change runs `onSelectionChange` and snaps back. That is how a literal `selected` behaves on a select.
- `options.onSelectionChange`:
  1. computes the new selection from the report's keys and its kind (G43): for `Replace`, the reported keys; for
     `Extend`, the reported keys and the selected keys the current page leaves out (G28, below);
  2. sets `element->selection` to it when the key is unbound;
  3. runs `onSelectionChange` in the table's scope with `event` set to it, as a user event, in one batch.
- **Snap-back.** The mount already treats a bound selection as controlled (`mountKind(Table)`, `mount.hpp:1157-1160`,
  through `controlled` and `inputEvent`, `mount.hpp:441-477`). The handler now takes the keys and the kind; the mount
  passes both to it and the keys alone to the re-assertion. After the handler runs, a posted turn re-asserts the
  slot when the widget shows anything else. So a bound selection the document does not write back snaps back, with no
  new mount code. Today the slot always equals what the user picked, because the lowering writes it first.

**Validation (loader).**

- `selection` or `onSelectionChange` with no `selectionMode`, or a literal `none`, is refused: "a table with
  `selection` selects rows; give it `selectionMode` `single` or `multiple`".
- `selection` and `onSelectionChange` on a model-backed table are refused: the engine owns its selection.
- `checkSet`'s refusal for a table becomes "`set` writes state or an input's draft; `t` is a table: bind its
  `selection`, `sort` or `page` to state and set that".
- A computed `selection` that is `null` selects nothing, with no report: state that starts empty may start as `null`.
  One that is neither a list nor `null` is reported once for the table's `selection` path, and selects nothing.

**View tree and backends.** No change. `TableOptions::selection` and `onSelectionChange` exist (`view.hpp:1427-1441`),
and every backend already keeps requested keys apart from marked rows (`backend_contract.md:257-267`).

**Spec.**

- `document.md:180`: the table row lists `onSelectionChange`, and `selection` as a list of keys, controlled when bound.
- `interpreter.md:334-346`, the `table` bullet: the selection rule, the snap-back, and the merge with the page.
- `interpreter.md`'s Relations section: "a table's `selection` and `sort` are controlled as a select's `selected` is".
- `author.md`: `selectionMode`, `selection` and `onSelectionChange`.

**Tests** (a new `tests/test_ui_interpreter_table.cpp`, on `RecordingBackend`).

1. The B1 document: the table records `selection` of rows `a` and `c`, and `t.selectedCount` reads 2.
2. With `onSelectionChange` writing `event` back, `selectRows(table, {b})` makes `selected` `["b"]`, and the widget
   then marks `b` only.
3. With no `onSelectionChange`, `selectRows(table, {b})` leaves `selected` at `["a","c"]`. After a settle, the widget
   marks `a` and `c` again: the snap-back.
4. `set selected []` from a button clears the marks. That is the "Clear" button, and a bulk action's last step.
5. An unbound table keeps the user's selection and runs `onSelectionChange` with it. So does one whose `selection`
   is a literal `null`.
6. Corpus refusals: `selection` without `selectionMode`; `set` of a table, with the new text.

**Control:** restore the slot over `element->selection` alone, and cases 1 and 4 fail with `selection=[]`; write
`element->selection` even when bound, and case 3 fails because nothing snaps back.

**Gallery.**

- Design 05's `selectBox` column and `selectPage` box go, replaced by G30's select column over the `selected` state.
  Compact's cards keep a document-drawn check box, now bound to the same state the table shows.
- The console (`console.cpp:606-614`) marks the current sample with `rowTone: info`, because "a table over rows has
  no bound selection". It becomes `selectionMode: single` with `selection: [currentId]` and `onSelectionChange`
  writing it.

**Risks.**

- A document that bound `selection` and relied on it being ignored now gets a controlled table, which snaps back
  without an `onSelectionChange`. No document in the tree gives a table a `selection` key: `grep -rn '"selection":'
  tests examples` finds only two theme colour tokens.
- The author rename breaks seven call sites, all in the tree.

## G43: the table's selection model, and why a button's modifiers stay out of ui/1 (#1059)

**Problem.**

- **A click knows no modifiers (reproduced).** A button's `onClick` runs with `event` `null`:
  `B4 event in onClick: null`. The view tree's `Button::onClick` is an `Action` (`view.hpp:424`), and
  `ButtonWidget::setOnClick` takes one (`backend.hpp:155-157`).
- **The backends disagree on what a click on a row does (read).**
  - Qt Quick: a plain click selects only that row, a Ctrl-click toggles it, Shift does nothing. `TapHandler`
    reports `ControlModifier` only (`MorphTableRow.qml:137-145`; `TableImpl::tapped`, `src/qt_quick/table.cpp:166-188`).
  - Terminal: a plain click toggles the row in `Multiple` mode (`TableImpl::click`, `table_widget.cpp:373-391`,
    through `pick`, `:407-430`). `TableImpl::key` ignores every key with a modifier (`:325-327`), so there is no
    Shift+arrow range and no Ctrl+A.
  - The contract says nothing about either (`backend_contract.md:249-267`).
- **A Qt Quick table cannot be reached from the keyboard (read, not run).** Neither `MorphTable.qml` nor
  `MorphTableRow.qml` sets `activeFocusOnTab` or handles an arrow key. The only key a row takes is Enter, and only
  while focus is already inside one of its cells (`MorphTableRow.qml:82-89`). The contract's "a table takes Enter,
  which activates the current row" (`backend_contract.md:61`) therefore holds on Qt Quick only for a table with a
  focusable cell. A table of text cells has no keyboard selection at all. The Qt Quick conformance probe reaches rows
  by mouse (`selectRows`, `tests/qt_quick/test_qt_quick_conformance.cpp:126-137`), so no case notices.

**Decision.**

- **The table gets one selection model, built in, on every backend.** Range selection belongs to the table's own
  selection, as in every desktop list. The document says nothing, and `onSelectionChange` reports the result.

  | Input | `Single` | `Multiple` |
  |---|---|---|
  | Click | selects the row | selects only the row, and makes it the anchor |
  | Ctrl+click | as a click | toggles the row, keeps the rest, and makes it the anchor |
  | Shift+click | as a click | selects the rows from the anchor to this one, in place of the rest |
  | Up, Down, Home, End, PageUp, PageDown | move the current row | move the current row |
  | Space | selects the current row | toggles the current row, and makes it the anchor |
  | Shift+Up, Shift+Down | as Up and Down | move the current row and select from the anchor to it |
  | Ctrl+A | nothing (goes on outwards) | selects every row shown; with every row shown already selected, clears them |
  | Enter | activates the current row (unchanged) | activates the current row (unchanged) |

  Arrows move without selecting, as the terminal does today, so a keyboard user can reach a row and activate it
  without losing the selection. A key the focused table declares as a chord still comes first (batch A's key order).

  These are the table's own keys (step 2 of **Chords**, `backend_contract.md:55-64`), so they apply while the table or
  one of its rows holds focus. A key that a focused control inside a cell leaves goes to the ancestors' chords, not to
  the table's navigation: Up and Down in a result field do not move the current row.

  "Every row shown" is every row the backend holds, realized or not. In a virtualized table the anchor or the end of a
  range can be a row with no widget, so the range is taken over the keys in row order, not over the realized rows.

- **A report says what kind of change it is.** A paged table's backend holds one page (G28), so only the report can
  tell the interpreter whether the user replaced the selection or added to it. `enum class SelectionChange :
  std::uint8_t {Replace, Extend}` (in `view.hpp`, beside `SelectionMode`) travels with every report:

  | `Replace` | `Extend` |
  |---|---|
  | a click, a Shift-click, Shift+Up, Shift+Down, and every change in `Single` mode | a Ctrl-click, Space in `Multiple` mode, a row's box, Ctrl+A, the header box |

  A `Replace` replaces the whole selection, other pages' keys included. An `Extend` keeps every selected key the
  backend does not hold. In a table that does not page the two read the same, since the backend holds every row.
- **A button's modifiers stay out of ui/1.** The plate's Shift-click range is the only design that needs one.
  - On the terminal it would not arrive: xterm-style emulators keep Shift+click for their own text selection while
    an application tracks the mouse. So the document could not rely on it, and would need the keyboard path anyway.
  - The plate already has that keyboard path: Shift with an arrow extends the selection (`plate.cpp:328-338`).
  - Adding the modifiers means changing the `Action` of every `ButtonWidget`, or adding a second click handler, on
    four backends, for a Qt-Quick-only behaviour.

  If a design later needs a modified click on Qt Quick alone, `event` on `onClick` (`{shift, ctrl, alt, meta}`) is the
  shape to add, and it changes no existing document, since `event` reads `null` there today.

**Backends.**

- **The handler.** `TableWidget::setOnSelectionChange` takes `std::function<void(std::vector<Key>, SelectionChange)>`,
  and `TableOptions::onSelectionChange` and `Table::onSelectionChange` have the same type. A backend passes the kind
  of the input that caused the report. The lines that name the handler, all of which change or are re-checked:
  - `include/morph/ui/view.hpp:906,1437,1520`; `include/morph/ui/backend.hpp:459,487,492-493`;
    `include/morph/ui/mount.hpp:1157-1158`; `include/morph/ui/interpreter.hpp:7266`;
  - `include/morph/forms/engine/collection_view.hpp:163`, which ignores the kind;
  - `include/morph/ui/testing/recording_backend.hpp:465,1458-1460,2103`;
    `include/morph/ui/testing/backend_conformance.hpp:668,816,880,952,969,984,1004,1009`;
  - `src/qt_quick/kinds.hpp:430,458`, `src/qt_quick/table.cpp:62-63,183,185`; `src/tui/table_widget.hpp:56,100,125`,
    `src/tui/table_widget.cpp:78-79,436`;
  - `tests/test_ui_table.cpp:63,249,268,320,353,373,414`, `tests/test_ui_controlled.cpp:246`,
    `tests/tui/test_tui_backend.cpp:517`, `tests/tui/test_tui_table.cpp` (18 lines, from `:98` to `:577`),
    `tests/qt_quick/test_qt_quick_table.cpp:80,102`.

  That is 65 lines (`grep -rn 'onSelectionChange\|OnSelectionChange' include src tests examples`), most of them test
  lambdas that gain an ignored parameter. No example names it.
- **Qt Quick.**
  - `TapHandler` reports `{ctrl, shift}`, and `TableImpl::tapped` takes a `RowClick` (`Only`, `Toggle`, `Range`)
    and keeps `_anchor`, a key. `Only` and `Range` report `Replace`, `Toggle` reports `Extend`.
  - `MorphTable` is a Tab stop (`activeFocusOnTab: true`) and keeps a current row, a key. Tab into the table gives
    the active focus to the current row, or to the first row when there is none; a click on a row makes it current
    and focuses it; an arrow moves the focus to the next current row. Because the focused item is the row, the row
    background's existing `current` state (`MorphTableRow.qml:23`, focus within the row) draws it, and assistive
    technology announces the row with its `Accessible.name` and `Accessible.selected`.
  - The keys are handled in `MorphTable`, which a row's unhandled key reaches by `Keys` propagation. Arrows, Space,
    Shift+arrows, Ctrl+A and Enter follow the table above. A row's `Keys.onReturnPressed` handler moves to the table.
  - `TableImpl` keeps the full key list in row order for ranges and Ctrl+A. The mount already passes every key in
    row order to `setExistingKeys` (`mount.hpp:1201-1213`, `session.pull()`), but only above the virtualization
    threshold, and the contract says "in no particular order" (`backend.hpp:519-529`). The contract now says row order,
    and below the threshold the realized rows are the whole list. `_existingKeys` keeps that order beside its set.
  - The current row follows the same rules as the terminal's cursor (`childForgotten`, `table_widget.cpp:85-95`): a
    removed row passes it to the row that takes its place.
- **Terminal.**
  - The click reaches `TableImpl::click` without its modifiers today: `WidgetBase::release` turns the
    `core::tui::MouseEvent` into a cell (`src/tui/widget.cpp:500-505`), and `click` takes a `Point` only
    (`src/tui/widget.hpp:150`). `click` gains the event's `core::tui::Modifier` (`MouseEvent::modifiers`, which the
    terminal reports in SGR mouse mode). Every other override ignores it.
  - A plain click selects only the row, as on Qt Quick; it no longer toggles.
  - `TableImpl::key` takes Shift+Up, Shift+Down and Ctrl+A, and keeps `_anchor`.
- **`RecordingBackend`.**
  - `clickRow(table, key, RowClick)` acts as the user's click with modifiers, and reports `Replace` for `Only` and
    `Range` and `Extend` for `Toggle`. `selectRows` stays, as a plain click then Ctrl-clicks, and reports `Replace`.
    Ctrl+A reports `Extend`.
  - `press(table, "Ctrl+A")` is a built-in key of a `Table` (step 2 of `press`, `backend_contract.md:356-358`).
    `Space` and the arrows need a current row, which a recording backend does not keep, so it does not take them.

**Spec.**

- `backend_contract.md`, **Selection by key**, gains the table of inputs above and the kind each input reports;
  the Interfaces row for `TableWidget` (`:225`) shows the handler's second argument.
- **Chords** step 2 adds a table's Ctrl+A, Space and Shift+arrows.
- The RecordingBackend section adds `clickRow` and Ctrl+A.
- `qt_quick/backend.md:198-215`: the focus, the current row and the keys.
- `tui/frontend.md:95`: the table's interaction column.

**Tests.** Three new conformance cases (41 to 43), on all three backends. The probe gains
`clickRow(table, row, RowClick)`.

- 41: in `Multiple` mode, a plain click selects only the clicked row, and a Ctrl-click adds a second row and then
  removes it, keeping the first. Each report is exactly the existing rows selected; the click's is `Replace` and the
  Ctrl-clicks' are `Extend`.
- 42: a click on row 1, then a Shift-click on row 3, selects rows 1 to 3. A Shift-click on row 0 then selects rows 0
  and 1: the range from the same anchor, in place of the rest. Each Shift-click reports `Replace`.
- 43: the keyboard on a focused table of text cells. Ctrl+A on a `Multiple` table selects every row, and pressed
  again clears them. On a `Single` table it selects nothing and reaches an enclosing column's `Ctrl+A` chord, and
  after a click on row 1, Enter activates row 1. It runs on Qt Quick, so it is the keyboard coverage this item's
  heading issue asks for.

**Control:**

- 41: today's terminal (plain click toggles) fails it, and so does a backend that ignores Ctrl, or one that reports
  every change as `Extend`.
- 42: treat Shift as a plain click, and it fails; report a range as `Extend`, and it fails.
- 43: leave Ctrl+A to the chords, and it fails. Remove the Qt Quick table's key handling, and both halves fail
  there: today a text-cell row never holds focus, so Enter reaches no row (`MorphTableRow.qml:82-89`).

`test_ui_conformance.cpp`'s `RecordingProbe` gains `Fault::NoModifiers` (every click is plain). The fault test
requires cases 41 and 42 to fail under it.

Backend-own tests:

- `tests/qt_quick/test_qt_quick_table.cpp`: Tab focuses a table of text cells; Down then Space selects the second
  row; Enter activates it.
- `tests/tui/test_tui_table.cpp`: Shift+Down twice from row 0 selects rows 0 to 2.

**Gallery.** No screen changes for this item. The plate keeps its keyboard range and its help line.

**Risks.**

- On the terminal, a plain click in a `Multiple` table now replaces the selection instead of toggling the row. Space,
  Ctrl+click and G30's check boxes toggle. A modified click reaches the application only where the emulator passes
  it on: xterm keeps Ctrl+click for its own menus and Shift+click for its text selection. So on a terminal the
  dependable toggles are Space and the select column, and the dependable range is Shift with an arrow. A terminal
  multiplexer whose prefix is Ctrl+A (GNU screen's default) keeps that key too.
- On Qt Quick, a table becomes a Tab stop. A table inside a form adds one stop between fields.
- Keyboard focus and a virtualized table: the current row can scroll out of the realized window. The table keeps the
  current row's key and asks for its row when the user moves, as `TableImpl::existingSelection` keeps a selected key
  (`table.cpp:145-160`). This is checked in the commit against the virtualization tests.

## G30: a check-box column with a select-all header

**Problem (read).** A column part has no header content beyond its `label` (`document.md:180`). Both frontends draw
the header themselves: a `Repeater` of `Label`s on Qt Quick (`MorphTable.qml:252-268`), and `putString` on the
terminal (`table_widget.cpp:279-286`). The one design that needs a header control (05) needs it for "select all on
this page", next to a column of row check boxes, which the gallery builds by hand (`samples.cpp:237-259`).

**Decision: the table draws its selection as a check-box column. A general control in a header stays out of ui/1.**

- The check boxes *are* the table's selection, drawn another way. Drawn by the table, they share its model (G43):
  a box is a Ctrl-click, and the header box is Ctrl+A. They are reported once, through `onSelectionChange`, and the
  backends give them the right accessible role.
- Drawn by the document (the gallery's workaround, which G29 would make workable), the row boxes and the row's own
  selection are two controls over one state, and a click on the row beside its box does something else.
- A general header slot (a node list in a column's header) would make the header a container the mount fills, on two
  frontends that draw it as labels today. It would need its own scope (a header has no row) and a rule for how a
  control in it coexists with G31's sort click. None of the ten designs puts anything but select-all in a header. A
  filter-in-header spreadsheet idiom can come later, as its own vocabulary.

**Vocabulary.**

- `selectColumn`: a literal flag, only with `selectionMode: multiple`. The table draws a leading column of check
  boxes, one per row, and a header box.
- `selectAllLabel`: the header box's accessible name, an expression; default `"Select all"`. Use `t` to translate it.
- `rowLabel` ⓘ: each row's accessible name, evaluated in the row's scope. A row's check box carries the same name.
  Without it, the row and its box are named by the row's key as text.

Author: `TableOptions::selectColumn` (`bool`), `selectAllLabel` and `rowLabel` (`AnyHandle`).

**Behaviour, on every backend.**

- The select column comes before the document's columns. It is not one of them: `columns`, a row's cells and the
  hidden-cell rule are unchanged.
- A row's box is checked while the row is selected. Toggling it is a Ctrl-click on the row: the row toggles and
  becomes the anchor.
- The header box is checked while every row shown is selected, partly checked while some are, and clear while none
  are. Toggling it is Ctrl+A: it selects every row shown, or clears them when all are. It is disabled while the table
  shows no rows.
- "Shown" means the current page (G28). Selected keys on other pages stay selected, by the lowering's merge.

**Lowering and validation.**

- The loader refuses `selectColumn` without a literal `selectionMode: multiple`, and `selectAllLabel` without
  `selectColumn`.
- `rowLabel` is bound in each row's scope, like `rowTone`, into the row view's `label`. `selectAllLabel` is bound in
  the table's scope.

**View tree.**

- `Table` gains `bool selectColumn = false` (set once) and `Prop<std::string> selectAllLabel`.
- `TableRowView` gains `std::optional<Prop<std::string>> label`. `ui::table` passes it on the row, as it does `tone`.
- The mount, in `mountKind(Table)` (`mount.hpp:1137-1162`):
  - calls `setSelectColumn(true)` before the first row, only for `true`;
  - binds `setSelectAllLabel`;
  - binds each row's label after `setRowKey`, as it binds the tone.

**Backend contract.** `TableWidget` gains three pure virtuals:

- `setSelectColumn(bool)`: once, before any row; a new table has none.
- `setSelectAllLabel(std::string_view)`.
- `setRowLabel(Widget& row, std::string_view)`: the row's accessible name, and its box's.

They are pure, not defaulted, so a backend cannot silently draw no boxes. The implementations are `qt_quick`, `tui`,
`RecordingBackend`, and the test-only `EchoingBackend`, which forwards.

- **Qt Quick.**
  - `MorphTable` reserves a leading width of `Theme.targetHeight` for the column, before `columnWidths` shares the
    rest.
  - The header draws a tri-state `CheckBox` there. Each `MorphTableRow` draws a `CheckBox` bound to `selected`,
    whose click fires `tap` with `Toggle`. The header box's click fires `selectAll`.
  - Neither box changes its own state. A Qt Quick `CheckBox` flips `checked` on a click whatever it is bound to, and
    a selection the document refuses would leave the box showing the user's click over an unmarked row. Each box's
    `nextCheckState` returns its current state, so its look follows `selected` (or the header's count) alone, as the
    row's background does. Such a box emits `clicked` and no `toggled`, so the handlers are on `clicked`.
  - The row's `Accessible.name` and its box's are the row label.
- **Terminal.**
  - The gutter grows from 2 to 5 cells: `[x]`, `[ ]` or, on the header line, `[-]`, then the cursor mark. The box
    replaces the `*` mark.
  - A click on a box toggles that row, and a click on the header box is Ctrl+A.
- **`RecordingBackend`.** Records `selectColumn=true`, `selectAllLabel`, and `label` on a row.

**Spec.**

- `document.md:180`: the three keys.
- `view_tree.md:120`: `Table` and `TableRowView`.
- `backend_contract.md:149` and `:225`: the three setters.
- **Selection by key**: the select column's behaviour.
- `qt_quick/backend.md` and `tui/frontend.md`: the drawing.

**Tests.**

- The probe gains no member. Case 43 (Ctrl+A) is the header box's operation, and the box's drawing is each backend's
  own test.
- `tests/qt_quick/test_qt_quick_table.cpp`:
  - with two of three rows selected, the header box's `checkState` is `PartiallyChecked`;
  - clicking it selects all three, and clicking again clears them;
  - clicking a row's box toggles that row and keeps the others;
  - a row's box has `Accessible.name` equal to its row label;
  - under a bound `selection` with no `onSelectionChange`, clicking a row's box leaves it clear after a settle.
    **Control:** drop the box's `nextCheckState`, and it stays checked.
- `tests/tui/test_tui_table.cpp`: the header line starts `[-]` with one of two rows selected; a click at the header
  box's cell selects both.
- `tests/test_ui_interpreter_table.cpp`, landing with G28 in part 2: a paged table with a key selected on page 2.
  Ctrl+A on page 1 reports every key of page 1 *and* the page-2 key.
- **Control:** make the header box select through `selectRows` (replace) instead of the Ctrl+A path, and the
  interpreter case loses the page-2 key; drop `setSelectColumn` in the Qt Quick backend, and the header box is not
  found.

**Gallery.** `selectBox`'s column and `selectPage`'s toolbar box go. The table gets `selectColumn`,
`selectAllLabel: "Select all on this page"` and `rowLabel` set to the sample id. The design's
`aria-label="Select S-26-104170"` becomes the row's name, "S-26-104170", on a checkbox in a row.

**Risks.** The box's name is the row label, not "Select S-26-…". Screen readers announce the role ("check box,
checked") with it, which says the same thing without an English verb in the library.

## G31: sorting rows

**Problem (reproduced).**

- `sort` is a table key (`document.hpp:817`), and "`sort`, `filters` and `hoveredKey` are not lowered"
  (`interpreter.md:339`).
- The lowering never reads it: `grep -n '"sort"' include/morph/ui/interpreter.hpp` finds nothing.
- B1 above: a bound `sort` leaves the rows in source order (`x z y`), `t.sort` reads `null`, and nothing is reported.
- The expression language cannot sort a list either (`expressions.md:107-113`), and design 05's compact layout is a
  `forEach` of cards, which needs the same order as the table.

**Decision: the table sorts its own rows; a new expression form sorts any list.**

- **The table sorts.** A sortable column says how to order by it, and the table orders its rows by the `sort` state.
  The other choice, a `sort` key that only draws the header arrow while the document sorts `rows` itself, needs the
  author to keep two things in step. If they forget, the header arrow toggles over rows that never move, which is
  silent.
- **The table sorts before it pages** (G28). So page 2 holds the second 25 rows of the whole sorted list, not the
  second page of the source order sorted within itself.
- **A list sorts with a form**, `sort`, for a `forEach` or any other use of a list. It is a comprehension like `map`
  and `filter` (`expressions.md:41`), so it can order by any expression of the element. A function would take a field
  path as a string, which no other function does.

**Vocabulary.**

- A column part gains:
  - `sortable`: a literal flag. The column's header sorts the table when the user clicks it.
  - `sortBy` ⓘ: an expression, the value to order the rows by, evaluated per row as `key` is (with `row` bound, in the
    table's scope). Default: the row's field named by the column's `id`. So `due`, shown as text, sorts by
    `dueAt`, a timestamp.
- The table gains `sort`, a list of sort keys, most significant first. Each key is a record `{"column": <column id>,
  "direction": "asc" | "desc"}`. `[]` is the source order. This is the shape the model-backed engine sorts by (a
  chain of keys, `table::SortChain`, `include/morph/table/sort.hpp:51-70`), so `sort` means the same on both kinds of
  table. Bound, it is controlled; unbound, the table holds it, starting at `[]`.
- The table gains `onSortChange`, commands run after a header click with `event` set to the new sort list.
- A header click on a sortable column makes that column the single sort key. When it already is the first key and
  `asc`, the click gives `[{column, "desc"}]`; otherwise it gives `[{column, "asc"}]`. Any other keys are dropped. A
  click on a column that is not sortable does nothing.
- A list of several keys sorts by each in turn, as the engine does; only a document writes one, since a click writes
  one key. The header marks the first key's column.
- The element value `sort` reads the bound value, or the table's own. The B1 test's `t.sort` then reads the list.
- The form `{"sort": list, "as": "x", "by": e, "order": e}`. `order` is `"asc"` or `"desc"`; absent or `null`, it is
  ascending. Another `order` is a mismatch: the form yields `null` and reports it. A `null` list yields `null`, as
  absence propagates (`expressions.md:68-70`). `as`, `by` and `order` are its companion keys, which the rule "one form
  key and only that form's companion keys" (`expressions.md:52-53`) lists with the others.

Ordering, the same for the table and the form:

- Values compare as `lt` compares them (`expressions.md:89-92`) — numbers by value, quantities across units,
  timestamps — except strings. Strings compare case-folded, as the model-backed engine's default collator does
  (`table::CodePointCollator`, `include/morph/table/data_source.hpp:232`, `:255`): ASCII `A`-`Z` and Latin-1 `À`-`Þ`
  (except `×`) lowered, other bytes as they are, then compared by bytes. Two strings that fold the same compare by
  their own bytes, so `Paracetamol` comes before `paracetamol` every time and the order is deterministic, whatever
  the source order. `lt` itself stays byte order; sorting is the one place text folds.
- `null` sorts last in both directions, so a sample with no due date stays at the end.
- The sort is stable, and descending reverses the comparison, not the list, so equal keys keep their source order in
  both directions.
- When every `by` is `null`, the list keeps its order.
- Values that `lt` cannot order (a string against a number) are a mismatch:
  - the form yields `null` and reports the path, as every function's mismatch does (`expressions.md:73-75`);
  - the table keeps the source order and reports once for its `sort` path, so a data error does not blank a screen.

Author:

- `TableColumn::sortable` (`bool`) and `TableColumn::sortBy` (`AnyHandle`).
- `TableOptions::sort` (exists, an `AnyHandle`) and `onSortChange` (`Commands`). A helper builds a key:
  `ui::sortKey("due", ui::SortOrder::Descending)` writes `{"column": "due", "direction": "desc"}`, and
  `ui::list({...})` of them is a `sort`.
- The form: `ui::sorted(list, "x", by, order)`, beside `ui::map` and `ui::filter` and built the same way: `by` is a
  function from the element's handle to the key (`author.hpp:1171-1193`), and `order` a handle, absent for
  ascending.

**Lowering.** The `rows` lambda of `lowerRows` (`interpreter.hpp:7126-7130`) becomes a pipeline:

1. evaluate `rows`;
2. sort it by the current sort list: each key's column's `sortBy` is evaluated once per row (decorate, stable sort,
   undecorate), inside the same tracked read, so a change to the sort, the rows, or anything `sortBy` reads
   re-sorts. An empty list skips the step;
3. page it (G28).

The mount's keyed reconciliation moves the kept rows with the fewest `moveChild` calls (`view_tree.md:373-395`). A
sorted row keeps its widget, its native state and its selection.

The header click reaches the lowering as a column index (below):

1. The lowering computes the next sort list from the current one, by the click rule above.
2. It sets the table's own sort when `sort` is unbound.
3. It runs `onSortChange` with the list, as a user event.

The lowering also binds `Table::sort`, the column index and direction the header shows, from the first key of the
bound or own sort; an empty list shows none.

**Validation.**

- Loader:
  - `sortable` needs the column's `id`, which names the sort;
  - `sortBy` needs `sortable`;
  - a literal `sort` is a list whose every key names a sortable column, once, and a direction;
  - `onSortChange` needs at least one sortable column;
  - every new key is refused on a model-backed table, whose engine sorts.
- Mount: a computed `sort` of `null` is the source order, as `[]` is. One that is neither `null` nor such a list, or
  whose key names a column that is not sortable, is reported once for the `sort` path and leaves the source order.
- The form's `order` is checked as a string where its type is known.
- **The keys a table over rows accepts and ignores.** `filters` and `highlightKey` on a table over `rows` are refused
  at load: "`filters` belongs to a model-backed table; filter `rows` with `filter`", and "`highlightKey` links a
  model-backed table to a chart". B1 shows both accepted with no report today. `sort` stops being one of them here.
- **The author palette writes those keys on a table over rows.** `paletteScreen()`'s `ui::table` gives `.sort =
  ui::list({})`, `.filters = ui::list({})` and `.highlightKey` (`tests/test_ui_author.cpp:691-694`), and the test
  registers the screen, so the new refusals fail it. The palette moves `filters` and `highlightKey` to its
  `dataTable` entries, and its `ui::table` keeps `.sort = ui::list({})`, now the source order, beside a sortable
  column. `tests/ui/golden/palette.json` is regenerated in the same commit.

**View tree.**

- `TableColumn` gains `bool sortable = false`.
- `Table` gains `Prop<std::optional<TableSort>> sort`, with `TableSort{std::size_t column; SortDirection direction}`
  and `enum class SortDirection {Ascending, Descending}`.
- `Table` gains `std::function<void(std::size_t)> onSort`: "the user asked to sort by this column".
- The backend reports the click and shows the state; the lowering decides what a click means. So a backend never
  reorders rows, and every backend cycles the same way.

**Backend contract.** `TableWidget` gains two pure virtuals:

- `setSort(std::optional<TableSort>)`: marks the column's header with the direction. It never calls a handler.
- `setOnSort(std::function<void(std::size_t)>)`: called with a sortable column's index when the user clicks its
  header (or presses Enter or Space on it where the header takes focus). A column that is not sortable reports
  nothing. The index counts the columns `setColumns` gave; G30's select column has none.

- **Qt Quick.**
  - A sortable column's header `Label` gets a `TapHandler` and `activeFocusOnTab`, and fires `sort` with its index.
  - The sorted column's label shows `▲` or `▼` after its text, and its `Accessible.description` states the order.
    Qt's accessibility has no sort state to set; open question 3.
- **Terminal.**
  - The header line draws ` ▲` or ` ▼` after the sorted column's label.
  - A click on a sortable column's label calls the handler, through `TableImpl::click` on line 0, which today finds
    no row there (`rowAtLine`, `table_widget.cpp:364-371`).
  - The header takes no keyboard focus; open question 2.
- **`RecordingBackend`.**
  - Records `sort` (`2:Ascending`, or `none`).
  - `columns` marks a sortable column with `;sortable=true`.
  - `clickHeader(table, column)` calls the handler for a sortable column and does nothing for another. A user can
    click any header, so it is not a misuse and does not throw.

**Spec.**

- `document.md:180` (the keys, the sort list, the element value).
- `expressions.md`: the Forms table gains `sort`, and Semantics states the ordering.
- `interpreter.md:334-346`: the pipeline, the cycle and the reports. "`sort`, `filters` and `hoveredKey` are not
  lowered" becomes "`hoveredKey` is not lowered".
- `view_tree.md:120`, `backend_contract.md:225` (setters), `qt_quick/backend.md`, `tui/frontend.md`.

**Tests.**

- **Conformance case 44**, on all three backends; the probe gains `clickHeader(table, column)`. A click on a
  sortable column's header reports its index once, a click on another column's header reports nothing, and
  `setSort` reports nothing.
  - Case 17 ("no setter calls a handler") also gains a `setSort`.
  - **Control:** report every header click, or none, and case 44 fails.
- `tests/ui/corpus/expressions.json`:
  - strings, numbers, timestamps and `null`, in both orders;
  - `["b", "B", "a", "A"]` ascending gives `["A", "a", "B", "b"]`: folded first, bytes on a tie, whatever the source
    order (`["B", "b", "A", "a"]` gives the same);
  - stability on equal keys;
  - all-`null` keys keep the order;
  - a string against a number yields `null` with a report;
  - `order` absent is ascending, and `order` `"up"` yields `null` with a report;
  - a `null` list yields `null`.
- `tests/test_ui_interpreter_table.cpp`:
  1. The B1 document, its `order` written as a list of one key, lists `z y x`, and `t.sort` reads the list. Written
     as a bare record, it keeps the source order and reports once for the `sort` path.
  2. Unbound: `clickHeader(table, 0)` sorts ascending and again descending, and `onSortChange` saw
     `[{name, asc}]` then `[{name, desc}]`.
  3. Bound without a write-back: a header click runs `onSortChange` and the order stays.
  4. `sortBy` on a timestamp field orders the text column by time, not by text.
  5. A selected row keeps its selection, and its widget id, across a sort.
  6. Mixed key kinds: source order, one report.
  7. Bound to a two-key list `[{status, asc}, {due, asc}]`: rows order by status, then by due within a status. A
     click on the Due header then gives `[{due, asc}]`, dropping `status`.
  8. `Paracetamol`, `lactose` and `Lactose` under an ascending text column: `Lactose`, `lactose`, `Paracetamol`.
- `tests/ui/corpus/documents.json`: `sortable` without an `id`; a literal `sort` naming a non-sortable column, or one
  column twice; `filters` on a table over rows.
- **Control:** skip step 2 of the pipeline, and cases 1, 2 and 4 fail; make the form reverse the list for `desc`,
  and the stability case fails; sort by the first key only, and case 7 fails; compare strings by bytes, and case 8
  and the folding corpus case fail; drop the byte tie-break, and the corpus case's second source order gives
  `["a", "A", "b", "B"]`.

**Gallery.**

- Design 05's "Due ▲" label and the comment saying the rows come sorted (`samples.cpp:435-436`) go. The Due and
  Sample columns are sortable, with `sortBy: dueAt` on Due.
- The sample records gain `dueAt`, a timestamp; `due` stays the text the design shows.
- Compact's disabled "Sort: due, earliest first" button (`samples.cpp:536-537`) becomes a select of the sortable
  columns and directions, whose `onChoose` sets `order` to a list of one key, so the table and the cards share one
  order. The cards' `forEach` lists the `sort` form of the shown samples, `by` the first key's column (`dueAt` or
  `id`) and `order` its direction.
- The table keeps its page across a sort. The samples screen's `onSortChange` also sets `page` to 0, so a new order
  starts at its first rows.

**Risks.**

- Strings fold ASCII and Latin-1 case only, not accents and not other scripts, as the engine's default collator
  does. A locale's collation would replace both through one collator, so the two kinds of table never disagree.
- A table sorted by a column whose `sortBy` reads row-scope state cannot see it: `sortBy` runs before any row's scope
  exists, as `key` does. The loader resolves names in it as it does for `key`, so a row-scope name reads `null` at
  run time. The spec states this; the commit checks whether the loader can refuse it instead.

## G28: paging rows

**Problem.**

- **No paging (read).** A table over `rows` shows every row (`lowerRows`), and the expression language has no
  `slice` or `take` (`expressions.md:107-113`).
- **Paging by slicing loses the selection (reproduced).** A document can page by computing `rows` from a page state.
  But a user's selection change reports only the rows the table holds (`backend_contract.md:262-266`), so the keys
  selected on another page are dropped. The throwaway test selected rows 1 and 2, paged to rows 3 and 4, and selected
  row 3:

  ```
  B3 after page 0 pick: [1,2]
  B3 after paging: [1,2] widget selection=[[]]
  B3 after page 1 pick: [3]
  ```

  Design 05's "3 selected on this page" with "Select all 312" needs the selection to outlive the page.
- **The plate needs a slice of a list (read).** It shows the first five samples to place and "+ 9 more", and lists
  all fourteen instead.

**Decision: the table pages itself; a list slices with a function.**

- With paging in the table, the lowering knows the whole sorted list and the page. On a user's change it keeps the
  selected keys of the rows on other pages (below).
- The backend sees one page of rows, as it would a short table. No `TableWidget` change.
- Row virtualization (`view_tree.md:397-411`) is a different thing: it realizes widgets near the scroll position
  over *all* rows. A page is what the user sees, by design.
- The pager is the document's: a text, a select for the page size and two buttons. Every backend can draw those, and
  the designs draw them differently (05's "1–25 of 312" row, 04's pager, compact's "Load 25 more").
- `slice` covers what is not a table: the compact card list's "Load more" and the plate's first five.

**Vocabulary.**

- The table gains `pageSize`, an expression for a positive integer; absent, the table does not page.
- The table gains `page`, an expression for a 0-based page index; it needs `pageSize`; absent, the first page.
- New element values:
  - `page`: the index shown, clamped to the last page;
  - `pageCount`: at least 1;
  - `pageFirst`, `pageLast`: the 1-based positions of the first and last rows shown, both 0 when there are none.
  - A table that does not page reads `page` 0, `pageCount` 1, and `pageFirst` and `pageLast` over every row.
- `viewCount` is the number of rows the table orders (every page); `sourceCount` is the number `rows` gave. Both are
  the length of `rows` today, and stay so.
- The function `slice(list, start, count?)`: `count` elements from `start`, or to the end. A start past the end gives
  `[]`. A negative argument is a mismatch. A `null` list yields `null`, as absence propagates.

```json
{ "kind": "text", "testId": "range", "text": { "fn": "format", "args": [ "{0}–{1} of {2}",
    { "ref": "samples.pageFirst" }, { "ref": "samples.pageLast" }, { "ref": "samples.viewCount" } ] } },
{ "kind": "button", "label": "›", "a11y": { "name": "Next page" },
  "enabled": { "fn": "lt", "args": [ { "ref": "samples.page" }, { "fn": "sub", "args": [ { "ref": "samples.pageCount" }, 1 ] } ] },
  "onClick": [ { "set": "page", "value": { "fn": "add", "args": [ { "ref": "samples.page" }, 1 ] } } ] }
```

Author: `TableOptions::page` and `pageSize` (`AnyHandle`), and `ui::slice(list, start, count)`.

**Lowering.**

- **Step 3 of the pipeline** reads `pageSize` and `page` (tracked) and keeps the page's rows.
  - `pageSize` of `null`: no paging, with no report. One that is neither `null` nor a positive integer: reported once
    for its path, no paging.
  - `page` of `null`: the first page, with no report. One that is negative or not an integer: reported once, the
    first page.
  - A `page` past the last is clamped, not reported. A filter that shrinks the list while the user is on page 3 is
    normal, and `samples.page` tells the pager where it is.
- **The selection merge.** For an `Extend` report (G43), `onSelectionChange` (G29) builds the new selection as:
  1. every currently selected key whose row is in the sorted list but not on the shown page, in its current order;
  2. then the reported keys.

  A `Replace` report is the whole new selection: a plain click, a Shift-click or Shift with an arrow on page 1
  clears the keys selected on other pages, as it clears the rest of page 1. Every change in `Single` mode is a
  `Replace`, so a single-select table never holds two keys. Keys whose rows are not in `rows` at all drop on any
  user change, as the contract says.
- **`empty`** shows while `rows` is empty, not while a page is. A non-empty list always has a non-empty page.
- **Virtualization** applies to the page's rows. A page is rarely past the threshold.

**Validation.** The loader refuses:

- `page` without `pageSize`;
- a literal `pageSize` below 1;
- either key on a model-backed table, whose engine pages.

The function `slice` is checked as `concatLists` is: a list where its type is known, integers for the others.

**Spec.**

- `document.md:180`: the keys and the element values.
- `expressions.md`: the Lists row gains `slice`.
- `interpreter.md:334-346`: the pipeline's third step, the clamp, and the merge.
- `backend_contract.md`, **Selection by key**: "the lowering keeps the selected keys of the rows on other pages; a
  backend sees one page" (an interpreter rule, stated where the contract's drop rule is, so the two are read
  together).

**Tests.**

- `tests/ui/corpus/expressions.json`: `slice` with and without `count`, past the end, a negative start.
- `tests/test_ui_interpreter_table.cpp`:
  1. 57 rows, `pageSize` 25:
     - pages 0, 1 and 2 hold 25, 25 and 7 rows;
     - `pageFirst`, `pageLast` and `pageCount` read 1, 25, 3 on the first page and 51, 57, 3 on the last;
     - the third page's rows are the 51st to 57th of the *sorted* list.
  2. The B3 sequence on a paged table, with row 3 added on page 2 by a Ctrl-click (`clickRow` with `Toggle`), keeps
     1 and 2. `onSelectionChange`'s `event` is `[1,2,3]`.
  3. A key whose row the rows no longer have at all still drops on the next user change.
  4. `page` 9 on a 3-page table shows page 2, and `samples.page` reads 2.
  5. A `pageSize` of 0 computed at run time reports once and shows every row.
  6. A `Single` table with its key on page 0: a pick on page 1 gives `event` `[that key]` alone.
  7. A pager text placed before the table in the document reads `pageCount` 3 on its first frame.
  8. A `Multiple` table with row 51 selected on page 2: on page 0, a plain click on row 3 gives `event` `[3]`; a
     Ctrl-click on row 4 then gives `[3, 4]`; with row 51 selected again, a Ctrl-click on row 4 gives `[51, 4]`.
- **Control:** skip the merge, and case 2 gives `[3]`, the B3 output; page before sorting, and case 1's last page
  holds the wrong rows; merge on every report, ignoring the kind, and case 6 gives two keys and case 8's plain click
  keeps row 51; never merge, and case 8's last Ctrl-click drops it; compute the page bounds in the
  lowering instead of on the element, and case 7 reads the defaults first.

**Gallery.**

- Design 05's literal "1–25 of 312" and disabled pager buttons (`samples.cpp:444-454`) become a working pager over
  the table: a `page` state and a `pageSize` state, with the rows select bound to the latter.
- The gallery's sample list grows to more than one page (it holds eight samples today), so the pager has somewhere
  to go.
- Design 04's `pager("1–25 of 57")` and the "Load 25 more" notification (`audit.cpp:289-295`, `:358-359`) become the
  same pager, and a `slice` of the compact list by a `shownCount` state.
- The plate lists `slice(toPlace, 0, 5)` and "+ {count − 5} more".

**Risks.**

- "Select all 312" sets the selection to every key of the list (`map`). A key of a row that a later search filters
  out of `rows` drops on the next user change, as today. The design does not say otherwise; open question 5.
- A page is computed on every change of `rows`. Sorting and slicing an in-memory list of a few thousand rows is cheap
  next to the widgets, and the keyed reconciliation moves only what changed.

## G44: grouping a list into runs

**Problem (read).** The list functions are `count`, `sum`, `mean`, `min`, `max`, `any`, `all`, `first`, `includes`,
`indexOf` and `concatLists` (`expressions.md:113`), with `map` and `filter`. Nothing groups adjacent elements. The
compact plate wants "A5–A8 · Empty, 4 wells" with one Assign button for the run, and rows D to H folded into one
section.

**Vocabulary.**

- The form `{"runs": list, "as": "x", "by": e}`: the list split into runs of adjacent elements whose `by` values are
  equal. Equal is `eq`'s comparison, except that `null` equals `null`: `eq(null, null)` is `null`, since absence
  propagates (`tests/ui/corpus/expressions.json`, "eq with null"), and a run of elements without the value is one run.
- A `null` list yields `null`. `as` and `by` are its companion keys.
- Each run is a record: `{"key": <the shared value>, "count": n, "first": <element>, "last": <element>,
  "items": [ … ]}`.
- The runs keep the list's order; an empty list gives `[]`.
- It is a form, like `sort`, because `by` is an expression of the element.

```json
{ "runs": { "ref": "rowWells" }, "as": "w", "by": { "ref": "w.empty" } }
```

Author: `ui::runs(list, "x", by)`, built as `ui::filter` is: `by` is a function from the element's handle to the
key.

**Lowering.** The expression evaluator only; the interpreter has nothing to do. The static type is `list<record>`.

**Spec.** `expressions.md`'s Forms table and Semantics. G46's group rows reuse this record as a heading's `row`.

**Tests.**

- `tests/ui/corpus/expressions.json`:
  - `[1,1,2,1]` by itself gives three runs, counts 2, 1 and 1, `first` and `last` set;
  - `[]` gives `[]`;
  - `null` keys group together;
  - a `by` that errors yields `null` with a report.
- **Control:** group by equal keys across the whole list instead of adjacent ones, and the `[1,1,2,1]` case gives
  two runs.

**Gallery.** The compact plate's one line per well becomes one line per run of a plate row, "A5–A8 · Empty, 4 wells"
from `first.name`, `last.name` and `count`. The rows of empty plate rows fold into one section, from `runs` over the
plate rows by "is every well empty".

**Risks.** None outside the expression language.

## G46: a table's group heading rows

**Problem (read).** A table's rows all take the same columns (`document.md:180`); there is no row kind, grouping key,
or cell spanning columns. Design 08 draws "S-26-104176 · lot L-26-0381" across all seven columns, between one
sample's results and the next (`08-Results.dc.html`: `<td colspan="7">`, `tr.grp td{background:#FAFBFC;
font-weight:600}`).

The gallery fakes it with a data row of `kind: group`, whose sample id sits in the Sample column and whose other
cells are hidden (`results.cpp:39`, `:452-467`, `:473-476`). It reads as a row, it is counted, selected and activated
as one, and every cell expression tests the kind.

**Vocabulary.**

- The table gains `group`, a part `{by ⓘ, heading ⓘ}`.
- `by` is an expression evaluated per row as `sortBy` is.
- A heading row is drawn before each run of adjacent rows whose `by` values are equal: the runs of G44, over the
  table's sorted and paged rows.
- `heading` is the nodes the heading row draws, in a row scope of its own whose `row` is the run record of G44:
  `{"row": "key"}`, `{"row": "count"}`, `{"row": "first.lot"}`.

```json
"group": { "by": { "row": "sample" },
           "heading": { "kind": "text", "role": "section",
                        "text": { "fn": "format", "args": [ "{0} · lot {1}", { "row": "key" }, { "row": "first.lot" } ] } } }
```

Author: `TableOptions::group`, a `TableGroup{AnyHandle by; Nodes heading}`, or a builder for a heading that declares
state.

**Behaviour.**

- A heading row spans every column, and the select column too.
- It is never selected, marked or activated. A click on it does nothing, the arrows skip it, and Ctrl+A and the
  header box pass over it.
- `selectedCount`, `viewCount` and the page positions count data rows only. A page holds `pageSize` data rows, with
  their headings; a group split across two pages gets its heading on both.
- **With a sort, the table sorts within each group.** The groups are the runs of the source order. Each keeps its
  place, and its rows are sorted inside it. Sorting results by Peak area keeps each sample's results under its
  heading. Sorting the whole list and then grouping would break groups into fragments.
- Alternate-row striping restarts after each heading.

**Lowering.**

- With `group`, the pipeline is:
  1. evaluate `rows`;
  2. split it into runs by `by`, in the source order, and give each row its run's index;
  3. sort stably by the run index first and the sort's `sortBy` second, so every group keeps its place and its rows
     are ordered inside it;
  4. page the data rows;
  5. put a heading entry before the first row of each run the page holds.
- A heading's `row` is the run record of the whole group, computed in step 2, not of the part of it a page holds. A
  group split across two pages reads the same `count`, `first` and `last` on both.
- The row list the view tree gets is of entries, each either a data row or a run.
- A heading's key is the string `"\u001Fgroup:"`, the canonical JSON of the run's key, `":"`, and the run's ordinal
  among the runs with an equal key (0 for the first). Two runs can share a key, since `runs` groups adjacent rows
  only (samples A, B, A), and the ordinal keeps their headings apart; it is stable across a sort, which keeps every
  run in place. Data keys are the document's own, so open question 4 covers a collision with one: it would be
  reported as a duplicate key by the mount (`view_tree.md:473`) and the later row left out.
- The row view lowers `heading` for a heading entry, in a new row scope whose row signal is the run record, as one
  cell with `TableRowView::heading` set. It lowers the column cells for a data entry.
- The selection merge (G28) and `onSelectionChange` never see a heading key. No backend marks or reports one on a
  realized row, and the lowering also removes any heading key from a report before it runs the merge: a virtualized
  Qt Quick table knows the keys of rows it has not realized (`setExistingKeys`) but not which of them are headings,
  so its Ctrl+A or Shift-range can name one.

**Validation.**

- The loader refuses `group` on a model-backed table, and a `group` without `heading`.
- A `by` that errors for a row puts that row in the `null` group, and the error is reported once.

**View tree.**

- `TableRowView` gains `bool heading = false`. A heading row has exactly one cell; `ui::table` refuses any other count
  as it refuses a row of the wrong cell count (`view.hpp:1503-1508`).
- The mount calls `setRowHeading(row)` after `setRowKey`, before the row's tone and label.

**Backend contract.** `TableWidget` gains `setRowHeading(Widget& row)`, pure: once, right after `setRowKey`, for a
heading row. It is never selected (`setSelection` with its key marks nothing), never reported, and never activated.
The contract's container row (`backend_contract.md:149`) adds: "a heading row's one cell spans every column".

- **Qt Quick.**
  - `MorphTableRow` gains `heading`. `place()` gives its one cell `x = inset` and the sum of the widths (and the
    select column) less two insets.
  - Its `TapHandler` and keys are off, and the table's current row skips it.
  - Its background part gets `heading`, and the stock `MorphTableRowBackground` fills it with `scheme.tableHeader`,
    as the header row is filled (`qt_quick/backend.md:226-228`). That colour is transparent until a theme sets
    `color.table.header`, so in the stock theme a heading stands out by its content (a `section` text) alone.
  - The row binds `heading` on its background only when the part declares the property, so a `tableRow` restyle
    written before this batch still loads; one that wants to draw headings declares it.
- **Terminal.** The row stack gets one extent, the table's width less the gutter. The cursor skips the row, the gutter
  draws no mark or box, and Space and Enter on it do nothing (the cursor is never there).
- **`RecordingBackend`.** Records `heading=true` on the row. `selectRows` and `activateRow` of a heading row's key
  throw `std::logic_error`, as selecting a key with no row does: a user cannot select or activate a heading, so a
  test that asks for it is a misuse. `clickRow` of a heading row does nothing and reports nothing, as a user's click
  on it does, the way `clickHeader` of a column that is not sortable does nothing.

**Spec.**

- `document.md:180`.
- `interpreter.md`: the grouped pipeline, the key, the scope.
- `view_tree.md:120`: `TableRowView{cells, tone, label, heading}`.
- `backend_contract.md:149` and `:225`.
- **Selection by key**: a heading row is never marked.
- `qt_quick/backend.md` and `tui/frontend.md`.

**Tests.**

- **Conformance case 45**, on all three backends: a table of a heading row and three data rows. Ctrl+A selects the
  three data rows and not the heading; a click on the heading row, through the probe's `clickRow`, changes nothing
  and reports nothing.
  - **Control:** let a heading row be marked, and the Ctrl+A half fails on every backend; let a click select it, and
    the click half fails.
- **Layout case L10:** a heading row's cell spans every column: its width is the sum of the column widths less the
  insets, within a unit. **Control:** place it as an ordinary cell, and it is one column wide.
- `layoutConformanceCases` goes from 9 (after G47) to 10.
- `tests/test_ui_interpreter_table.cpp`:
  1. Design 08's eight lines (two samples) give two heading rows, each reading "S-26-… · lot …".
  2. `selectedCount` after Ctrl+A is the data rows' count.
  3. Sorting by a column keeps every result under its sample's heading.
  4. A page boundary inside a group repeats the heading on the next page, and both headings read the group's whole
     `count`.
  5. Rows grouped A, B, A by sample give three headings and no duplicate-key diagnostic.
- **Control:** sort the whole list before grouping, and case 3 fails; compute the runs over the page, and case 4
  reads the page's part of the count; key a heading by its run's key alone, and case 5 reports a duplicate key and
  drops the third run's heading.

**Gallery.**

- Results' `kind: group` lines go. The worksheet groups by `sample`, and the heading draws the id (mono) and the lot.
- The `group` tests in every cell expression go: `isMean` and `isTotal` stay, `group` goes.

**Risks.**

- A heading row is a widget per group. A worksheet has tens.
- A document that groups by a value its rows are not ordered by gets many one-row groups. That is what `runs` means,
  and the spec says to sort first.

## G41: a selectable push button

**Problem (reproduced).** A button's `selected` is refused unless `style: item`:

```
B5 loaded=0 refusal=document b5 at /view/selected: `selected` marks a list item; give the button `style: item`
```

The refusal is `document.hpp:3008-3012`, and the mount refuses the same in C++ (`mount.hpp:725-727`). An item is drawn
unframed with an accent tint and a leading bar (`MorphButtonBackground.qml:19-35`). So the plate's sample wells are
list entries in a grid, not wells, and a selected well looks like a selected list line.

**Decision.**

- **`selected` is allowed on a push button.** A selected push button keeps its frame and size, draws a 2 px accent
  ring outside its border, and fills with the accent tint a selected item already takes: `color.accent` at 12 %
  opacity (`scheme.accent`, `MorphButtonBackground.qml:28`). The ring is the design's "thick accent ring while
  selected"; the tint is what tells selection from focus. A focused push button draws a 2 px `color.focus` border
  inside its own (`MorphButtonBackground.qml:31-32`), and the default theme gives `color.focus` and `color.accent`
  the same value (`#1565c0`, `theme.hpp:993-994`), so a line alone would not. A `primary` or `danger` button keeps
  its own fill and takes the ring only.
- **On Qt Quick**, assistive technology announces a selected push button as checked, through its `Accessible`
  properties. The control itself is not made `checkable`: a click on a checkable button flips `checked` whatever it is
  bound to, so a button the document keeps selected would read unchecked after a click until `selected` next
  changes.
- **On the terminal**, it draws in the list's selected style (`theme.listItemSelected`) inside its brackets.
- **The circle stays out of ui/1.** A 40 px round well is a shape, and ui/1 has none on any node. Adding `shape` to a
  button reaches every frontend's control drawing for one design. It can be drawn today:
  - a theme restyle of the `button` part, for an application whose every push button may be round;
  - a `custom` component with a QML hash (`document.md`, Components), for a well alone.
- `sizing` with a `{width, height}` record of fixed units already gives the well a fixed size. A unit is not
  square (`Theme.unitWidth` against `Theme.unitHeight`), so the well is as close to the design's 40 px square as the
  units allow.

**Vocabulary.** No new key. The loader drops the `style: item` requirement for `selected`. `document.md:164` changes
"`selected` (a condition, only with `style: item`: the entry is the current one)" to "a condition: an item is the
current entry; a push button is pressed".

**Mount and backends.**

- `mountKind(Button)` stops refusing a bound or `true` `selected` on a push button, and calls `setSelected` for any
  style when bound or `true`.
- `ButtonWidget::setSelected`'s Doxygen (`backend.hpp:171-174`) covers both styles.
- **Qt Quick.** `MorphButtonBackground` draws the ring for `!item && selected`: a 2 px `scheme.accent` border outside
  the control's own, so the button's size and its label's place do not change. For `!item && !filled && selected`
  its `color` is `Qt.alpha(scheme.accent, 0.12)`, the item's selected tint, darkened while hovered or pressed as the
  unselected fill is. `MorphButton` sets
  `Accessible.checkable: true` while a push button's `selected` is bound or `true`, and `Accessible.checked:
  root.selected`. It does not set the control's `checkable`.
- **Terminal.** `ButtonImpl::paint` (`src/tui/leaf_widgets.cpp:146`) draws a selected push button's caption in
  `listItemSelected`, keeping the brackets.
- **`RecordingBackend`.** Records `selected` for any style, as it does for an item.

**Spec.** `document.md:164`; `backend_contract.md:163-167` (**Button style and selection**); `view_tree.md`'s Button
row; `qt_quick/backend.md` and `tui/frontend.md`'s button rows.

**Tests.**

- `tests/ui/corpus/documents.json`: the B5 document loads. The corpus case expecting the refusal is removed.
- `tests/test_ui_mount.cpp` (or wherever `mountKind(Button)` is tested): a push button with a bound `selected`
  records `selected=true`, then `false`.
- `tests/qt_quick/test_qt_quick_theme.cpp`: under `brandTheme()`, a selected push button's ring item is visible in
  `color.accent`, its background's `color` is `color.accent` at 0.12 alpha while a focused unselected one's is the
  surface colour, and the button's `implicitWidth` equals an unselected one's. `Accessible.checked` is true, and is
  still true after a click the document does not answer. **Control:** make the button `checkable`, and that last
  check fails.
- `tests/tui/test_tui_widgets.cpp`: a selected push button's caption cells carry `listItemSelected`, and it is as
  wide as an unselected one.
- **Control:** keep the mount's refusal, and the mount case throws; drop the ring's `visible` condition, and the Qt
  Quick colour check fails; leave the fill alone for a selected push button, and the tint check fails.

**Gallery.**

- The plate's sample wells become push buttons with `selected` bound to the selection, at a fixed size in units.
- The template wells keep their surfaces.
- The plate's help line keeps "Click selects a well", and keeps the keyboard range of G43.

**Risks.**

- A document that used `selected` with `style: item` is unchanged.
- The ring sits outside the border, so a selected button in a tight grid can touch its neighbour's ring. The plate's
  grid has `gap: 1`.
- A theme that sets `color.accent` close to `color.surface` makes the tint faint; the ring still marks the
  selection, and the focus border sits inside it.

## Totals

- `conformanceCases()` goes from 40 to 45 (41–43 from G43, 44 from G31, 45 from G46), and `test_ui_conformance.cpp`'s
  `REQUIRE(cases.size() == 40)` becomes 45. Batch C, which lands after this batch, numbers its cases from 46.
- Case 17 also covers `setSort`.
- `layoutConformanceCases()` goes from 7 to 10 (L8 and L9 from G47, L10 from G46).
- `ConformanceProbe` gains `clickRow(table, row, RowClick)` and `clickHeader(table, column)`. Each of the three probes
  implements them:
  - Qt Quick, by mouse clicks with `Qt::ControlModifier` or `Qt::ShiftModifier`;
  - the terminal, by mouse events with the modifiers set;
  - `RecordingBackend`, through its helpers.
- **What the probe cannot reach** (`backend_contract.md:505-516`) gains:
  - the header's sort mark;
  - the select column's drawing;
  - a column's text alignment.

  Each frontend's own table tests check these.
- `TableWidget` gains six pure virtuals: `setSort`, `setOnSort`, `setSelectColumn`, `setSelectAllLabel`,
  `setRowLabel` and `setRowHeading`; the column fields travel through `setColumns`. Four implementations each.
- Each new `Table` field (`selectColumn`, `selectAllLabel`, `sort`, `onSort`) is also a `TableOptions` field, which
  `ui::table` copies into the `Table` it builds, as it does `selection` (`view.hpp:1427-1442`).
- `setExistingKeys` passes the keys in row order, and its Doxygen says so.
- `expressions.md` gains `sort`, `runs` and `slice` in `expr/1`, and its version paragraph says new vocabulary joins
  the current version.

## Open questions

1. **The header cycle.** A header click on the sorting column alternates `asc` and `desc`. Should a third click return
   to the source order? Recommended: no. A document that wants an unsorted state offers a control that sets `sort` to
   `[]`, and a two-state cycle matches the designs' `aria-sort`.
2. **Sorting from the terminal keyboard.** A terminal header takes clicks, not focus. Recommended: no table keyboard
   sort in this batch. The designs' compact layouts already carry a sort control, which every backend draws. A
   focusable header line on the terminal can follow if a design asks.
3. **Sort state for assistive technology on Qt Quick.** Qt's accessibility has no sort property. Recommended: state
   the order in the header's `Accessible.description`, from a theme-independent pair of glyphs. Revisit if Qt adds a
   sort state.
4. **Heading-row keys.** A heading row's key is a reserved string (`U+001F` and `group:`). A data key with that
   prefix would collide and be reported. Recommended: keep the prefix and document it. The alternative, a third
   alternative in `ui::Key`, touches every keyed collection and every backend's key encoding for one table feature.
5. **Selected keys a filter hides.** A user's change drops a selected key whose row `rows` no longer holds, as the
   contract says, while an `Extend` keeps keys on other pages. Should a bound selection also keep keys a filter hides?
   Recommended: no. A selection the user cannot see is one a bulk action surprises them with. A document that
   wants it merges in `onSelectionChange` itself.
6. **A button's modifiers (G43).** Left out of ui/1 for the reasons above. Recommended: revisit only if a design
   needs a modified click that has no keyboard equivalent.
7. **The plain click on the terminal.** It changes from toggling to replacing, to match Qt Quick. Recommended: as
   designed. Users who toggle with the mouse use Ctrl+click or the select column, and on an emulator that keeps
   Ctrl+click, Space or the select column.
8. **Qt Quick keyboard navigation in tables.** G43 makes a Qt Quick table focusable with a current row. Recommended:
   land it inside G43, since range selection needs it, with case 43 as its conformance case. If it grows past that,
   split it into its own commit before G43.
9. **`selectAllLabel`'s default.** `"Select all"` is English text in the library, as `"Required"` already is.
   Recommended: keep it, and document `t` for translation.
