# Editing in the Table — Implementation Plan (Part G)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `docs/spec/table/editing.md`: grids of a form, derived values, cell selection with paste and fill, row operations, header bands, spans, summary rows, fixed grids, and cells of a model's table edited through `CellEdit` and `RowEdit` actions.

**Architecture:** A grid is a form's `ObjectArray` field laid out by the table pipeline: `ui::FormRowsSource` is the `table::DataSource` over the field's `RowState`s, and `ui::GridController` owns focus, the editor, the walk, cell selection, paste, fill, clear and row operations over a `ui::GridSource`, which a form grid, a fixed grid and a model's table each implement. Every read-only decision is one function, `ui::cellReadOnly`. Derived values are rounds of the form session (`FormSession::noteCommit`, `loadDerived`) over the action's evaluation and the form's derive action; the server recomputes row members and clears derived ones. Bands, spans and summary rows are the mount's, for any table.

**Tech Stack:** C++23, CMake (MSVC preset `cl-debug`), header-only morph, glaze, Catch2, Qt 6 Quick (offscreen `morph_qt_quick_tests` only), the terminal backend (`morph_tui_tests`).

**Spec:** `docs/spec/table/editing.md` — all sections: "Two homes for an edit", "A grid of a form", "Cells of a model's table", "The cell", "Walking", "Selecting cells, clipboard and fill", "Rows of a grid", "Layouts", "Document vocabulary", "Invariants" (1–9), "API surface", "Tests", "Failure modes", "Neighbouring specs", "Delivery order" steps 1–6. It builds on `docs/spec/table/table.md` ("The client table model", "The renderer", "How a column shows its cells", "Acting on rows") and `docs/spec/forms/engine.md`. Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md` (Tasks 19–24, its Global Constraints, standard task loop and Review Focus items 3 and 5).

Part G of 7; index: 2026-11-17-one-table.md

## Global Constraints

Every constraint of the roadmap's "Global Constraints" applies; the ones that bite here, and this part's own:

- Comments and specs state what the code does now and why; no history, no issue numbers, no commit hashes (AGENTS.md).
- Every public symbol has complete Doxygen `@param`/`@tparam`/`@return`. Docs build: `cmake -S . -B build/doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF && cmake --build build/doc --target doc` — exits 0.
- morph is public: test fixtures use the neutral `lims` domain (`Lims_RecordSpecimens`, `Lims_SetLimits`, `Lims_EditSpecimenCount`); no company or customer name anywhere.
- Commits are authored `yaraslau <mail@yaraslau.com>` and end with the session's attribution lines.
- No GUI application is launched; Qt Quick behaviour is tested only in `morph_qt_quick_tests` (ctest sets `QT_QPA_PLATFORM=offscreen`).
- Every test below names its mutation; step "Mutate" of each task applies it by hand, runs the test, sees it fail, reverts, and pastes the failing line into the commit body.
- Values the spec pins, verbatim: `deriveDebounce` default **150 ms**; bands nest at most **3** levels; `enterMoves` default `down`; `walk` default `rows`; `editMode` default `cell`; summary `position` default `bottom`; a span's `rows` and `columns` default **1**; a block row's key is `"<row key>/<key>"`.
- A neighbouring spec takes its part only in the task whose step its row names (editing.md "Neighbouring specs"), and says nothing of it before.
- The read-only rule is one function. No code path decides read-only, the read-only surface or a place in the walk except through `ui::cellReadOnly` (Invariant 4).
- **A `column` argument is an index into `Table::columns`** (index #37), which holds every column, hidden ones included: every `Table` handler, `TableWidget` call and `RecordingBackend` driver this part adds or uses (`setFocusCell`, `setOnCellClick`, `setCellEditor`, `setCellMarks`, `clickCell`). A visible place in the layout is always named `position` (`setCellSelection`, `setOnCellSelect`, `selectCells`, `BandRun::first`).
- **`ui::Table` and `TableOptions` are parallel** (index #41): every member a task below adds to `Table` it adds to `TableOptions` with the same name and type, and `table(…)` copies it across.
- **Tests that mount a document** use `ui::testing::TableHarness` (`tests/ui_table_support.hpp`, index #42): a whole document with a `models` block and an alias, the task's actions added to its catalog with their schema text, `effects` and `records`, and the free-function helpers (`elementValue(h, id, name)`, `cellMarks`, `rowMarks`, `menuOf`, `clipboardText`). Its locale is `en`.
- **The loader** (`src/ui/document.cpp`) accepts each new key in the task that lowers it, with a loader test in that task (index #44).

**Commands** (verified against `tests/CMakeLists.txt`, `tests/qt_quick/CMakeLists.txt`, `tests/tui/CMakeLists.txt`, `CMakePresets.json`):

```bash
cmake --preset cl-debug                                                    # once per tree
cmake --build --preset cl-debug --target morph_tests                       # test_ui_*.cpp, tests/forms/*.cpp
cmake --build --preset cl-debug --target morph_table_tests                 # tests/table/*.cpp
ctest --test-dir build/cl-debug -R "<pattern>" --output-on-failure
# Qt Quick (offscreen, ctest sets the platform) and the terminal backend, one tree:
cmake --preset cl-qt-debug -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON
cmake --build --preset cl-qt-debug --target morph_qt_quick_tests morph_tui_tests
ctest --test-dir build/cl-qt-debug -R "<pattern>" --output-on-failure
# morph_net_tests is POSIX-only: build and run it on linux-everything
cmake --preset linux-everything && cmake --build --preset linux-everything --target morph_net_tests
ctest --test-dir build/linux-everything -R "<pattern>" --output-on-failure
```

## Interfaces consumed from Parts A–F

Names the roadmap pins are used as it gives them. Rows marked **assumed** are not pinned by the roadmap; this part uses exactly these names, and the executor checks them against the landed code before Task 19 starts and corrects this plan (not the other part) if they differ.

| From | Name | Pinned by |
|---|---|---|
| Task 1 | `table::TableColumn{ColumnInfo info; std::string label, group; bool sortable, filterable, shown;}`, `table::TableDescription{id, label, columns, key, groups, levels, units, revision}`, `table::describe` | roadmap |
| Task 2 | `table::ChangedRow`, `ChangedRows<fn>`, `BRIDGE_REGISTER_TABLE`, `effects::updatesTable<fn>`, the record `<Model>.<id>` | roadmap |
| Task 3 | `table::Unloaded`, `TableErrorCode::SourceContract`, `CellEdits::commit` refusing an unloaded cell with `FieldError{"…", "not loaded yet"}` | roadmap, table.md |
| Task 8d | `table::ColumnLayout::{setPinned, pinned, value, apply}` with `value()`/`apply` over `table::ColumnLayoutValue{order, hidden, pinned, widths, groups, units}`; existing `setVisible`, `visible`, `move`, `order` | index #7, Part C |
| Task 9 | `ui::TableRows`, `EngineRows`, `WindowRows`, `TableRowRef`, `TableCountsView`, `SortChain`; `WindowRows::setDescription(table::TableDescription)` and `onStaleDescription(std::function<void()>)` (the lowering refetches the description and calls `setDescription`) | roadmap, Part D |
| Task 9 | `EngineRows(std::shared_ptr<table::DataSource> source, RecordOf recordOf, table::EngineOptions options)`; `struct RowRecord { table::RowId key; Value row; }`; `using RecordOf = std::function<RowRecord(table::RowSnapshot const&, std::size_t sourceRow)>` (an owning record, memoised by `EngineRows`); `TableRows::row(index)` empty for a row not fetched yet, `TableRows::total()` | index #21, #28, Part D |
| Task 9 | `ui::TableStatus{counts, state, retry, message, shown}`, `Table::status`; `ui::TableColumn::id`; `ui::TableArrangement{visible, pinned, widths}`, `Table::arrangement` | index #11, #14, Part D |
| Task 10a | Mount-held focus `(row key, column id)`; `TableWidget::setFocusCell(std::size_t viewRow, std::size_t column)` (`column` an index into `Table::columns`, index #37), `setOnFocusMove(std::function<void(FocusMove)>)`, `enum class FocusMove { Up, Down, Left, Right, Home, End, First, Last, PageUp, PageDown }` | Part D |
| Task 11 | `ui::ValueRowsSource` | roadmap |
| Task 12b | `IViewBackend::copyText(std::string_view) -> bool` | index #12, Part E |
| Task 9d | `void ScreenCore::tableMessage(ElementState& table, std::string text)` (`tableMessage(table, "")` clears the message; 6 s hold, `kStatusMessageHold`); every grid and model-table message is posted through it | index #11, Part D |
| Task 15 | `ui::formatCell(table::Cell const&, table::TableColumn const&, ui::FormatContext const&, DisplayUnits const& = {}) -> std::string`; `ui::FormatContext` (`ui/expression.hpp`), `ui::localeFacts`, `ui::withoutGrouping`; `TableColumn::{display, tone, tooltip}`; `TableDescription::{rowTone, rowLock}` | index #9, Part F |
| Task 17a | `forms::DeclaresSchemaValue<T>` (a type's `schemaValue()` returning `{keyword, name}`, written by `annotateSchemaMarker` on the **property** of a member of that type), `name` the `RecordInfo::name` in `model::recordRegistry()` whose `type` is `typeid(TableOf<fn>)`; `TableInputKind`, `TableInput{member, record, kind}`, `tableInputsOf(std::string_view schemaJson)` (top-level properties only), `RunObserver` (called by `MutationRuntime`, so only for runs the session's mutations make) | Part F |
| Task 17a | `ui::StatusEntry{std::string id; std::string label;}` (by value); `TableStatus::{entries, standing, standingEntries}`; `ScreenCore::tableMessage(ElementState&, std::string text, std::vector<StatusEntry> entries)`; `TableWidget::setOnStatusEntry(std::function<void(std::string)>)`. A transient notice with a choice is a message with entries; a state that lasts while its source holds is `standing` | index #19, Part F |
| Task 17d | `TableOptions::cellMenu` (`std::function<std::vector<MenuItem>(Key const& row, std::size_t column)>`: built-ins, separator, the application's), `TableWidget::setCellMenu`; Part E's `TableOptions::menu(std::optional<std::size_t>)` | Part F, Part E |
| Task 17c | `ChangedRow::from`, `FailedRow`, `ChangedRows::failed`; `ui::RowStates` via `TableRows::states()` (`isBusy(key)`, `failure(key)`, `busyCount()`), `TableRows::moveKey(from, to)`, `ui::lockText(TableRowRef const&, TableDescription const&)`, `ui::refusesEdit(TableRows const&, std::int64_t index, TableDescription const&)` | index #10, Part F |
| Task 15 | `ui::CellAccent{optional<Tone> tone; std::string tooltip; bool invalid}`, `accentedCell(Node content, Prop<CellAccent> accent)`, `TableWidget::setCellAccent(row, column, accent)`: a lowered `cellView` returns an `accentedCell`, which this part wraps in `markedCell` | index #39, Part F |
| Task 17b | `TableRowLook::marks` (`std::optional<Prop<RowMarks>>`), `TableWidget::setRowMarks` | index #39, Part F |
| Task 9 | `TableRows::onChange` is one slot, set by the mount; this part never subscribes a second time and wraps the rows in a forwarding decorator instead | index #40, Part D |
| Task 9d | `ui::testing::TableHarness`, `elementValue`, `cellMarks`, `rowMarks`, `statusOf`, `replyDescribe`, `replyPage` in `tests/ui_table_support.hpp` | index #42, Part D |

## File structure

| File | Responsibility | Task |
|---|---|---|
| `include/morph/ui/view.hpp` | `CellRef`, `KeyOutcome`; `Table::onKey`, `keyChords`, `onText`, `onGridFocus`, `onActivateCell`, `onFocusOut`, `editing`, `editor`; `CellMarks`, `MarkedCell`, `markedCell`, `TableRowLook::modified`; `CellRange`, `Table::cellSelection`, `onCellSelect`; `RowSpan::columns`; `TableRowLook::summary`; `BandRun`; `Table::bands`; `GridFocus` | 19, 21, 22 |
| `include/morph/ui/mount.hpp` | The focus chain, `Mounted::focusNext/focusPrevious`; the cell editor in place of a cell; cell marks bound in each cell's own scope (`MarkedCell`); the cell rectangle; `bandRuns`, `columnUnitShown`, `bandedHeaderName`; `cutColumns`; summary rows | 19, 21, 22 |
| `include/morph/ui/backend.hpp` | `TableWidget::setOnText`, `setOnCellClick`, `setOnFocusOut`, `setCellEditor`, `setCellMarks`, `setRowModified`, `setCellSelection`, `setOnCellSelect`, `setBands`, `setRowSummary` (all with no-op defaults); the Tab contract | 19, 21, 22 |
| `include/morph/ui/testing/recording_backend.hpp` | Drivers `typeInto`, `clickCell`, `focusOut`, `selectCells`; records `marks:<column>`, `modified`, `editor:<column>`, `cellSelection` | 19, 21 |
| `include/morph/forms/engine/field_control.hpp` (new) | The scalar controls of a field, moved out of `form_view.hpp` so `grid.hpp` and `form_view.hpp` both include them and neither includes the other's includer | 19 |
| `include/morph/forms/engine/form_view.hpp` | Includes `field_control.hpp` and `ui/grid.hpp`; draws an `ObjectArray` whose widget is `grid` with `ui::formGrid` | 19 |
| `include/morph/ui/form_rows.hpp` (new) | `FormRowsSource` | 19 |
| `include/morph/ui/grid.hpp` (new) | `GridColumn`, `GridOptions`, `CellFacts`, `cellReadOnly`, `GridSource`, `FormGridSource`, `GridRows`, `GridController`, `formatOf`, `FormGrid`, `makeFormGrid`, `formGrid`, `PasteResult`, `pasteMessage`, `SummaryRows`, `SummarySpec`, `summaryScopeRows`, `FixedGridSource`, `ModelGridSource`, `ModelColumn` | 19–24 |
| `include/morph/forms/derive.hpp` (new) | `DeriveOf`, `Derived`, `derived<>()`, `clearDerived`, `DeriveOptions`, `DeriveRound`, `DeriveLoad`, `DeriveState` | 20 |
| `include/morph/forms/forms.hpp` | `FieldMeta::derived`, `withDerived()`; `x-derived`; row types' `computedFields` in the schema and in `recomputeAll` | 20 |
| `include/morph/forms/engine/field_model.hpp` | `FieldSpec::computed`, `derived`, `minItems`, `maxItems` | 19, 20, 21 |
| `include/morph/forms/engine/form_session.hpp` | Per-cell snapshots, `modified`, `rowModified`, `revert`; `field()` by row key; `onBeforeSubmit`; rounds; `insertRow`, `removeRows`, `restoreRows`; row-scoped and shared Choice options | 19–21 |
| `include/morph/core/registry.hpp` | `clearDerived` in `prepareAction` and `evaluateAction` | 20 |
| `include/morph/table/description.hpp` | `BandInfo`, `TableDescription::bands`, `checkBands`; `EditInfo`, `TableColumn::edit` | 22, 24 |
| `include/morph/table/edits.hpp` | Includes `description.hpp` and `forms/engine/field_model.hpp`. `CellEdit`, `RowEdit` (each with an in-class glaze meta), `checkEdits`; `CellEdits` row commits, drafts by key, `moveRow`, `keepColumns`, `rowAbsent` | 24 |
| `include/morph/forms/forms.hpp` | `annotateSchemaRoot<A>`: the action's own `schemaValue()` at the schema root | 24 |
| `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `include/morph/ui/interpreter.hpp` | Vocabulary, refusals, lowering, element values | 19–24 |
| `src/qt_quick/table.cpp` + QML, `src/tui/table_widget.cpp` | Editors in cells, marks, bands, spans, summary rows | 19, 22 |
| `tests/test_ui_grid.cpp` (new), `tests/forms/test_forms_derive.cpp` (new), `tests/grid_test_support.hpp` (new) | Grid and derive tests and their fixtures: `GridFixture` (19), `FixedGridFixture` (23), `FakeRows` and `ModelGridFixture` (24) | 19–24 |
| `tests/tui/test_tui_table.cpp` | The terminal grid: editor in a cell, Tab, bands | 19, 22 |

## Review Focus

1. **An empty grid** (roadmap Review Focus 3). A grid over an `ObjectArray` with no rows: `Tab` leaves the table at once; a paste with `appendRows` adds the block's rows up to `maxItems`, without it drops every value and the status line counts them; a summary shows `count` 0 and an empty `mean`. Tests: Task 19 `"grid: an empty grid lets Tab leave at once"`, Task 21 `"grid: paste into an empty grid"`, Task 22 `"summary: over no rows"`.
2. **Bands under a stored layout** (roadmap Review Focus 5). A stored layout hides every column of a band, then one is shown again: the band reappears over that one column with its unit; a band whose columns are all pinned draws only in the pinned part. Test: Task 22 `"bands: a stored layout hides then shows a band's column"`.
3. **Clipboard text from a spreadsheet.** A spreadsheet copies `"1,5\t2\r\n3\t4\r\n"`: CRLF line ends, a trailing line break, values padded with spaces, grouped numbers (`"1.234,5"` in a comma locale). The block is 2 × 2, no value holds `\r`, no empty third row is pasted or appended, `" 12,5 "` is 12.5 and `"1.234,5"` is 1234.5. Test: Task 21 `"grid: spreadsheet clipboard text pastes as typed values"`.
4. **A row fetch that arrives before the edit's reply.** The action's change event can reach the client before its reply (table.md "Rows an action changed"), so the window refetches the edited row while the cell is still stale. The cell stays stale with the committed text until the reply, and is modified from the reply until the next fetch after it. Test: Task 24 `"cell edits: a fetch before the reply keeps the cell stale"`.
5. **A description revision that removes an edited column** (roadmap Review Focus 1, editing half). A model table holds a draft and an error in column `count` when the description's new revision drops `count`: the draft and its error are gone, `errorCount` falls by one, and nothing is committed for it. Test: Task 24 `"cell edits: a new revision drops drafts of a removed column"`.

---

## Phase B — editing (`editing.md`)

### Task 19: The focus chain and a grid of a form (editing.md step 1)

**Spec:** editing.md "A grid of a form" (Binding, Columns are fields, Submission), "The cell" (all), "Walking", Invariants 1, 2, 4, 5; Delivery order step 1. Needs Tasks 9–11, 15, 17a, 17c and 17d (index #45): Task 10a's focus, Task 17a's status entries, Task 17c's `states()` and `moveKey` (pure virtuals `GridRows` answers) and Task 17d's `cellMenu`.

**Files:**
- Modify: `include/morph/ui/view.hpp`, `include/morph/ui/backend.hpp`, `include/morph/ui/mount.hpp`, `include/morph/ui/testing/recording_backend.hpp`
- Create: `include/morph/ui/form_rows.hpp`, `include/morph/ui/grid.hpp`, `tests/grid_test_support.hpp`, `tests/test_ui_grid.cpp` (add to `morph_tests` in `tests/CMakeLists.txt`)
- Create: `include/morph/forms/engine/field_control.hpp` (the helpers `isSet` … `controlCommon`, `optionSelect` and every scalar case of `fieldControl`, moved verbatim from `form_view.hpp` into `forms::detail::engine::scalarControl(FormSession& session, FieldState& state)` (the session stays a parameter: the date and date-time cases read `session.options().displayOffsetMinutes` and `session.pattern(date)`, `form_view.hpp:255-272`); it includes `form_session.hpp` and `ui/view.hpp`, never `form_view.hpp` or `grid.hpp`)
- Modify: `include/morph/forms/engine/form_session.hpp`, `include/morph/forms/engine/field_model.hpp` (`FieldSpec::computed`), `include/morph/forms/engine/form_view.hpp` (includes `field_control.hpp` and `../../ui/grid.hpp`; `fieldControl` calls `scalarControl(session, state)` for every scalar kind and draws an `ObjectArray` whose `spec().widget == "grid"` with `ui::formGrid`), `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `include/morph/ui/interpreter.hpp`, `tests/ui_interpreter_support.hpp` (catalog schema `Lims_RecordSpecimens`)
- Modify: `src/qt_quick/table.cpp` and its QML, `src/tui/table_widget.cpp` (cell editors, `setOnText`, `setOnCellClick`, `setOnFocusOut`, `setCellEditor`, marks); `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_table.cpp`

**The include order.** `fieldControl` (the forms control a cell editor is) lives in `form_view.hpp`, and `form_view.hpp` must draw a grid, so `grid.hpp` including `form_view.hpp` while `form_view.hpp` includes `grid.hpp` would be a cycle. The scalar controls move to `field_control.hpp`; `grid.hpp` includes `field_control.hpp`, `form_session.hpp`, `ui/view.hpp`, `ui/table_rows.hpp` and `ui/expression.hpp`; `form_view.hpp` includes `grid.hpp`. Verified against the code: `form_view.hpp` includes `ui/view.hpp`, `form_session.hpp`, `overrides.hpp` (lines 25–33) and defines `fieldControl` at line 210, whose scalar cases read the session and the field and no `ViewOptions` member (only the `Object`/`ObjectArray` cases do, through `fieldCell`).
- Docs: `docs/spec/ui/view_tree.md` (`markedCell`: a cell's marks are its cell view's, bound with the cell and rebound with it), `docs/spec/ui/backend_contract.md` (focus chain, `focusNext`/`focusPrevious`, `setOnText`, Tab delivered to a widget that declares it), `docs/spec/forms/engine.md` (the grid widget leaves "Not supported"; per-cell snapshots, `modified`, `revert`), `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md` (`form`/`field` on `table`, the `grid` overlay, `enterMoves`, `walk`, element values), `docs/spec/qt_quick/backend.md` (editors inside cells, cell and row marks), `docs/spec/table/table.md` (check it points to editing.md and lists `TableColumn::edit`, `TableDescription::bands`; add what is missing).

**Interfaces:**
- Consumes: `ui::TableRows`, `EngineRows` (Task 9); `ValueRowsSource` (Task 11); `ui::formatCell` (Task 15); Task 10a's mount-held focus, `TableWidget::setFocusCell(viewRow, column)` (`column` an index into `Table::columns`) and `setOnFocusMove(FocusMove)`; Task 15's `accentedCell` (index #39); `ui::TableColumn::id`, `Table::arrangement` and `TableStatus::message` (Task 9); `ScreenCore::tableMessage` and `TableStatus::{standing, standingEntries}`, `StatusEntry` (Tasks 9d, 17a; index #19); `TableOptions::cellMenu` (Task 17d); `forms::FormSession` (`options()` gives `locale`, `bcp47`, `datePattern`, `dateTimePattern`), `FieldState`, `RowState`, `forms::detail::engine::fieldControl` (existing, its scalar part moved to `scalarControl(session, state)`).
- Produces:
  - `view.hpp`: `struct CellRef { Key key; std::string column; bool operator==(CellRef const&) const = default; };` `struct GridFocus { std::optional<Key> key; std::size_t viewRow = 0; std::string column; bool operator==(GridFocus const&) const = default; };` — a focused cell as the mount shows it: `key` is empty while the row at `viewRow` is not fetched yet (a placeholder), `column` a column id. `Table::gridFocus` (`std::optional<Prop<std::optional<GridFocus>>>`, not `focus`, which names the mount's own focused cell already: when set, the table's focus is the grid's, and the mount shows it with Task 10a's `setFocusCell(viewRow, column)`, `viewRow` the key's view position now, or `GridFocus::viewRow` while `key` is empty, and `column` the index into `Table::columns` of the column with that id; a focused view row stays realized, key or not, so the window fetches a placeholder) and `Table::onGridFocus` (`std::function<void(GridFocus)>`: a click or a `FocusMove` the mount resolves to a cell — `key` empty when that row is not fetched — handed to the grid instead of moving the mount's own focus); `Table::onActivateCell` (`std::function<void(CellRef)>`: a double click on a cell); `Table::onFocusOut` (`std::function<void()>`: focus left the table's widget and everything inside it, an open editor included); `Table::editing` (`std::optional<Prop<std::optional<CellRef>>>`) and `Table::editor` (`std::function<Node(CellRef const&)>`): while `editing` names a cell, the mount builds `editor(cell)` once, mounts it as a child of that cell's row widget, calls `TableWidget::setCellEditor(row, column, &widget)` (`column` the index into `Table::columns`) and `widget.focus()`; when `editing` changes it calls `setCellEditor(row, column, nullptr)` and unmounts the editor (a focused cell's row stays realized, Task 10a, so the editor's row is never pooled while it is open); `enum class KeyOutcome : std::uint8_t { Handled, Ignored, FocusNext, FocusPrevious };` `Table::onKey` (`std::function<KeyOutcome(std::string const& chord)>`), `Table::keyChords` (`Prop<std::vector<std::string>>`, re-declared to the backend with `setKeys` on each change), `Table::onText` (`std::function<void(std::string)>`); `struct CellMarks { bool readOnly = false; bool modified = false; bool error = false; bool stale = false; bool muted = false; bool operator==(CellMarks const&) const = default; };` Marks are a cell's, drawn by its cell view (index #28), not a row's: `struct MarkedCell { Node content; Prop<CellMarks> marks; };` and `[[nodiscard]] Node markedCell(Node content, Prop<CellMarks> marks)` — a node a `TableSource::cellView` returns (or a cell of a `TableRowView`). The mount mounts `content` as the cell and binds `marks` in the cell's own scope, calling `TableWidget::setCellMarks(row, column, marks)` (`column` the cell's index into `Table::columns`, as `setCellLoad` and `setCellAccent` take it) after the row's key, on every change, and whenever a pooled cell is attached to another row widget (Task 10a), whether or not the marks changed. Because the binding lives with the cell and reads the cell's own row signal, a pooled cell rebound to another row (its signal set) re-evaluates its marks with its content; nothing per row holds them. Anywhere but a table cell a `MarkedCell` mounts its content and its marks are not read. Its `content` may be Task 15's `accentedCell` (index #39: a cell view is `markedCell(accentedCell(content, accent), marks)`); the mount binds the accent and the marks of the one cell, each in the cell's scope. The row's own mark is `TableRowLook::modified` (`std::optional<Prop<bool>>`, bound in the row's scope by `rowView`; the mount calls `TableWidget::setRowModified`), beside Task 9d's `tone`, `label`, `spans` and `heading` and Task 17b's `marks`. Every `Table` member above has its twin on `TableOptions` (index #41).
  - `backend.hpp` (every new member has a default that does nothing): `virtual void TableWidget::setOnText(std::function<void(std::string)> onText)` (printable text typed while a cell has focus and no editor is open); `virtual void TableWidget::setOnCellClick(std::function<void(std::size_t viewRow, std::size_t column, bool doubleClick)> onClick)` (`column` the index into `Table::columns`, index #37; a single click calls it with `false`, the second click of a double click with `true`); `virtual void TableWidget::setOnFocusOut(std::function<void()> onFocusOut)` (focus moved to a widget outside the table, its editor and its popups); `virtual void TableWidget::setCellEditor(Widget& row, std::size_t column, Widget* editor)` (`column` the index into `Table::columns`; draws `editor`, a child of `row`, over the cell's area in place of its content; null draws the cell again); `virtual void TableWidget::setCellMarks(Widget& row, std::size_t column, CellMarks const& marks)`; `virtual void TableWidget::setRowModified(Widget& row, bool modified)`. Contract: a widget whose `setKeys` lists `Tab` or `Shift+Tab` receives them instead of moving focus. Verified against the code: `TableWidget` (`backend.hpp:764`) has `setOnActivate(std::function<void(Key)>)` for a row and no cell click, and no widget has a focus-out callback; `Widget::setKeys` and `Widget::focus` exist (`backend.hpp:91`, `:94`).
  - **Chords.** `GridController::keyChords()` (tracked) is what `Table::keyChords` binds. With no editor open: `Tab`, `Shift+Tab`, `Enter`, `F2`, `Space`, `Delete`, `Escape`, `Ctrl+Enter`, and from Task 21 `Shift+Up`, `Shift+Down`, `Shift+Left`, `Shift+Right`, `Ctrl+A`, `Ctrl+C`, `Ctrl+V`, `Ctrl+D`, `Ctrl+Shift+=`, `Ctrl+-` (plain arrows are Task 10a's `FocusMove`). With an editor open: exactly `Enter`, `Tab`, `Shift+Tab`, `Escape`, `Ctrl+Enter`, so every other key (arrows, `Home`, `End`, `Delete`, `Ctrl+C`/`Ctrl+V` on the text, `Alt+Down` on a dropdown) reaches the editor. The editor is `GridSource::editor(key, column)`: `scalarControl(session, field)` for a form's field, or for the `value` field of a model cell's edit session (Task 24); its `TextInput` is drawn with no `onCommit`, so `Enter` is the table's alone. A cell whose `GridSource::toggles` is true (a `Boolean`) has no editor (`Space` toggles it).
  - **Where the editor's text lives.** In the field's draft, live: the editor is bound to `field->text()`, so every keystroke is the cell's draft at once (what `Escape` "restores the draft the cell had when it opened" presupposes, editing.md line 269). When the editor opens, the controller keeps the draft the cell held (`GridSource::draft`) and, for typing on a focused cell, writes the typed text with `write`. `Escape` writes the kept draft back with `write` and closes; it calls no `committed()`. **Commit** closes the editor and calls `committed()` exactly once when the draft differs from the kept one, and not at all when it is equal; it writes nothing, the draft being there already. `GridController::type(text)` with an editor open is `write(cell, text)` — what the editor's input does through its own binding — and exists so a test can drive a grid with no mounted editor.
  - `mount.hpp`: `bool Mounted::focusNext(std::string_view focusId)`, `bool Mounted::focusPrevious(std::string_view focusId)`; the mount answers a table's `KeyOutcome::FocusNext`/`FocusPrevious` by moving focus along the chain from the table's widget. The chain is the mounted widgets that take focus — `TextInput`, `Button`, `Checkbox`, `Select`, `Menu`, `DateTimeInput`, `Slider`, `FilePicker`, `ActionButton`, `Table`, `Tabs`, a `Badge` with `onRemove` — that are visible and enabled, in document order (a walk of the mounted tree in child order, computed when asked).
  - `form_rows.hpp`: `class FormRowsSource final : public table::DataSource` — `FormRowsSource(forms::FormSession& form, std::string arrayPath, std::vector<table::ColumnInfo> columns)`; `columns()`, `snapshot()`, `subscribe`, `unsubscribe`; `[[nodiscard]] forms::RowState* row(table::RowId const& key) const`. A row's id is `RowId{static_cast<std::int64_t>(RowState::key())}`; rows in the array's order. A cell is the member's encoding as a cell of the column's kind (`Integer` → `int64`, `Decimal`/`Quantity` → `Rational`, `Number` → `double`, `Text`/`Enum`/`Choice` → `std::string`, `Date` → days, `DateTime` → seconds, `Bool` → `bool`), `std::monostate` when blank, and, when the draft does not encode, `std::string{draft}` — an invalid cell in every column but `Text`, where it is the text itself. A draft change of one member notifies `Updated` for its row alone.
  - `grid.hpp`:
    - `enum class EnterMoves : std::uint8_t { Down, Right, None };` `enum class Walk : std::uint8_t { Rows, Columns };`
    - `struct GridColumn { std::string id; std::string label; std::string field; std::function<std::string(forms::RowState const&)> value; };` — `field` empty with `value` set is a display column.
    - `struct GridOptions { EnterMoves enterMoves = EnterMoves::Down; Walk walk = Walk::Rows; std::function<void(Key)> onActivate; };` — `onActivate` is the table's `onActivate`, which `Enter` or a double click on a read-only cell runs.
    - `struct CellFacts { forms::FieldState const* field = nullptr; bool hasEdit = true; bool overlayReadOnly = false; bool companionReadOnly = false; bool busy = false; bool locked = false; bool unloaded = false; };` and `[[nodiscard]] bool cellReadOnly(CellFacts const& facts);` — true when `field` is null or `!hasEdit`, the field's `readonly()` (which covers `x-readonly`, `x-computed`, `x-derived` and a `readonlyWhen` reaching it), or any other member is true.
    - `class GridSource` (pure virtual), **by view index**, because a model's table holds only a window of its rows: `rowCount() const -> std::size_t` (the view's rows, fetched or not); `keyAt(std::size_t viewRow) const -> std::optional<Key>` (empty while that row is not fetched); `positionOf(Key const&) const -> std::optional<std::size_t>` (the key's view position now, empty when it is not in the view); `columns() const -> std::vector<std::string>` (the ids of the visible columns in layout order: the walk's and the rectangle's order, not `Table::columns`' — a cell is always named by its column id, and `node()` looks a `Table::columns` index up by `columns[c].id`); `editor(Key const&, std::string_view column) -> Node` (the editor of an editable cell, built once per opening: the forms control of the cell's field; never called for a read-only cell or one that `toggles`); `toggles(Key const&, std::string_view column) const -> bool` (the cell's field is a `Boolean`: `Space` and a click flip it, no editor opens); `readOnly(Key const&, std::string_view column) const -> bool` (calls `cellReadOnly`); `covered(Key const&, std::string_view) const -> bool`; `draft(Key const&, std::string_view) const -> std::string`; `text(Key const&, std::string_view) const -> std::string` (display text, `formatCell`); `error(Key const&, std::string_view) const -> std::optional<std::string>`; `write(Key const&, std::string_view column, std::string draft) -> void` (a user's draft); `committed() -> void` (an edit is meant; one call per change); `marks(Key const&, std::string_view) const -> CellMarks` (tracked: every read it depends on is a tracked read, so a binding over it follows drafts, errors, edits and row states); `keyOf(Value const& record) const -> std::optional<Key>` (the key of the row a cell's row signal holds: a cell view knows its row only through that signal, which a pooled cell's rebind sets to another record; empty for a record with no key, which draws no marks); `rowModified(Key const&) const -> bool` (tracked: some cell of the row is modified, or the row was added since the load); `view() -> TableRows&` (the data rows, by view index: what `rowCount`, `keyAt` and `positionOf` count, and what `node()` mounts unless a decorator is given to it, Task 22's `SummaryRows`).
    - `class FormGridSource final : public GridSource` — `FormGridSource(forms::FormSession& form, std::string arrayPath, std::vector<GridColumn> columns, FormatContext format)` (it makes its `std::shared_ptr<FormRowsSource>` and the `GridRows` over it; every row of a form is fetched, so `keyAt` is never empty below `rowCount`; `text()` is the draft while blank or not encoding, else `formatCell` of the cell with the column's display places set to the field's places); `view()` (its `GridRows`); `rowModified(key)` is `FormSession::rowModified(arrayPath, key)`; `void setLayout(std::vector<std::string> visible)` (the visible column ids in layout order, which the lowering feeds from Task 9's `Table::arrangement`; default every column in declared order); `[[nodiscard]] forms::FieldState* field(Key const& row, std::string_view column) const`; `editor(key, column)` is `forms::detail::engine::scalarControl(form, *field(key, column))` and `toggles` is `field(key, column)->spec().kind == forms::FieldKind::Boolean`; `void onBeforeSubmit(std::function<void()> commit)` (registers `commit` with `FormSession::onBeforeSubmit` for the source's life; `GridController` passes its commit of an open editor, so the form's submit ends the edit first); `modifiedCount() const -> std::size_t`, `modifiedRows() const -> std::size_t`, `errorCount() const -> std::size_t` (all tracked).
    - `class GridRows final : public TableRows` — `GridRows(std::shared_ptr<FormRowsSource> source)`, over `EngineRows(source, source->recordOf(), table::EngineOptions{})` (index #21): the engine's view, less the rows added since the sort or the filter last changed, which follow it in the order they were added (they pass the filter until it changes). With no sort and no filter the view is the array's order, an inserted row in its place. It answers Task 17c's `states()` with a `RowStates` of its own that nothing registers into (a grid of a form runs no action per row) and `moveKey` by doing nothing (a form row's key never changes).
    - `form_rows.hpp` also gives `[[nodiscard]] RecordOf FormRowsSource::recordOf() const` — the grid's own record (index #28): `RowRecord{RowId{key}, row}`, `row` an object holding, for every member of the row type, its encoded value, `null` when blank or not encoding, and the member `#key`, the row's key (no member of a row type is named `#key`: `#` introduces a row key in `FormSession::field` paths). `FormGridSource::keyOf` reads `#key`. `summaryScopeRows` (Task 22) and a column's `value` expression read this record.
    - `class GridController` — `GridController(reactive::Runtime& runtime, GridSource& source, GridOptions options)`; `[[nodiscard]] std::optional<CellRef> focus() const` (tracked; the focused cell when its row is fetched, empty when nothing has focus or the focus stands on a placeholder); `[[nodiscard]] std::optional<GridFocus> place() const` (tracked; the focus as the mount shows it and what `Table::gridFocus` binds: `{key, positionOf(key), column}` for a focused cell, `{nullopt, viewRow, column}` after the walk stopped at a row not fetched yet; when `keyAt(viewRow)` answers, on the source's next change, the place takes that key and `focus()` names the cell); `void focusPlace(GridFocus place)` (what `Table::onGridFocus` calls: `focusCell` when `place.key` is set, else the placeholder focus); `void focusCell(CellRef cell)` (a click or an arrow; refused for a covered cell; commits an open editor on another cell first); `void activate(CellRef cell)` (what `Enter` and `F2` with no editor, and a double click, do: focuses the cell, then opens the editor holding the value on an editable cell, or calls `GridOptions::onActivate(cell.key)` on a read-only one); `[[nodiscard]] std::optional<CellRef> editing() const` (tracked); `[[nodiscard]] std::vector<std::string> keyChords() const` (tracked, per **Chords** above); `KeyOutcome press(std::string_view chord)`; `void type(std::string_view text)` (on a focused cell with no editor: keeps the cell's draft, writes `text` as the draft and opens the editor; with the editor open: `write(cell, text)`, the editor's whole new text, see **Where the editor's text lives**); `void leave()` (focus left the table: commits); `[[nodiscard]] std::string const& message() const` (tracked; the latest transient refusal or notice, which the lowering posts with `ScreenCore::tableMessage`) and `[[nodiscard]] std::vector<StatusEntry> const& messageEntries() const` (tracked; the choices that notice offers, posted with it through `tableMessage`'s entries overload); `[[nodiscard]] std::string standing() const` and `[[nodiscard]] std::vector<StatusEntry> standingEntries() const` (tracked; a state that lasts while its source holds, which the lowering writes to `TableStatus::standing`/`standingEntries`, index #19; Task 20 fills them); `void chooseEntry(std::string_view id)` (an entry chosen in the status line, `TableWidget::setOnStatusEntry`); `[[nodiscard]] std::vector<MenuItem> cellEntries(Key const& row, std::string_view column)` (the grid's entries of a cell's menu, which the lowering puts after Task 17d's built-ins and before its separator and the document's entries: here "Revert cell", enabled while `modified` holds for that cell, and "Revert row", enabled while `rowModified` holds, each calling `FormSession::revert` and then `committed()` once; Task 21 adds the row entries); `[[nodiscard]] Node node(std::vector<ui::TableColumn> columns, TableOptions options, std::optional<TableSource> base = {})` — the table node over `columns` (every column, hidden ones included, each with its `id`), wiring `gridFocus` → `place()`, `onGridFocus` → `focusPlace`, `onKey`, `keyChords`, `onText`, `onActivateCell` → `activate`, `onFocusOut` → `leave`, `editing`, `editor` → `source.editor(cell.key, cell.column)` and `cellMenu`. Its rows are Task 9d's `Table::source`. With no `base`: `TableSource{rows = source.view() (a non-owning `shared_ptr`, aliasing an empty owner: whoever holds the grid outlives the mounted node, see `FormGrid`), cellView, rowView}`, `alwaysFetched` and `levels` empty (a grid holds every row and has no levels), and `cellView(c, row)` is `markedCell(accentedCell(<the cell's text, bound to source.text(key, id)>, <CellAccent{.tooltip = source.error(key, id), .invalid = that error is set}>), <marks bound to source.marks(key, id)>)` with `key = source.keyOf(row.get())` and `id = columns[c].id`. With `base` (a model's table, Task 24; a grid under summary rows, Task 22): `rows`, `alwaysFetched` and `levels` are the base's, a cell is `markedCell(base->cellView(c, row), <the same marks>)` — the base cell is already the lowering's `accentedCell` over the column's `cell` template, tone and tooltip (index #39), so an editable table keeps every column's own view — and `rowView(row)` is the base's look with `modified` set. Either way `rowView(row)` gives `.modified = <bound to source.rowModified(keyOf(row.get()))>`, and a record with no key (`keyOf` empty: a heading, a summary row) draws no marks and no modified mark.
    - `[[nodiscard]] FormatContext formatOf(forms::FormSession const& form)` — `{.locale = options().bcp47, .numeric = options().locale, .datePattern = options().datePattern, .dateTimePattern = options().dateTimePattern}`, read when called (a later `setFormat` is read by the next call).
    - `struct FormGrid { std::shared_ptr<FormGridSource> source; std::shared_ptr<GridController> grid; Node node; };` and `[[nodiscard]] FormGrid makeFormGrid(forms::FormSession& form, forms::FieldState& array, std::vector<GridColumn> columns, GridOptions options, FormatContext format, TableOptions table = {})` — makes the source and the controller over `form.runtime()` (`FormSession::runtime()`, existing), registers the controller's commit with `source->onBeforeSubmit`, and builds `node` as `grid->node(<one ui::TableColumn per GridColumn: id, label>, table)` whose handlers each capture both `shared_ptr`s, so the node owns the grid it shows. The lowering keeps the `FormGrid` in the table's `ElementState` (element values, `setLayout`, messages). `[[nodiscard]] Node formGrid(form, array, columns, options, format)` is `makeFormGrid(…).node`, for `form_view.hpp`, which passes `formatOf(session)` and every member as a column in schema order. With no `ScreenCore` to post through (a C++ `formView`), `node()` binds `Table::status` itself: `TableStatus{.message = message(), .entries = messageEntries(), .standing = standing(), .standingEntries = standingEntries(), .shown = any of them is set}`; the lowering passes its own `status` in `table` and posts through `tableMessage` instead.
  - `form_session.hpp`: `[[nodiscard]] std::size_t FormSession::addBeforeSubmit(std::function<void()> hook)` and `void FormSession::removeBeforeSubmit(std::size_t id)`: `submit()` (`form_session.hpp:1017`; explicit mode, and the interpreter's `form_submit`) calls every hook, in registration order, before it assembles the body (`FormGridSource::onBeforeSubmit` wraps the pair). `FormSession::field(path)` also takes `rows#<key>.member` (a row by key); `[[nodiscard]] bool modified(std::string_view path) const` (tracked: reads the field's draft and unit; true when they differ from what the field held when the form was last loaded, or the field's row was added since; false for a computed or derived member, `spec().computed || spec().derived`, whose value only a round writes, and for no other: a member a `readonlyWhen` rule or `x-readonly` freezes after the user typed in it stays modified); `field_model.hpp`: `bool FieldSpec::computed = false;`, set where `x-computed` is read (`field_model.hpp:598`), since `computedInputs` alone cannot tell an `x-computed` with no inputs from none; `[[nodiscard]] bool rowModified(std::string_view arrayPath, std::uint64_t key) const`; `void revert(std::string_view path)` (programmatic: restores the loaded draft and unit; a row path reverts each member; reverting a row added since the load removes it). The load record is `std::map<std::string, LoadedCell, std::less<>>` by `FieldState::path()`, `struct LoadedCell { std::string text; std::size_t unit; }`, written for every member of every `ObjectArray` row by `prefill`, `reset`, construction, a field `refill` loads and a field `acceptSent` ends the edit of.
  - Document: `table` takes `form` (a form node id) and `field` (a path) in place of `rows`; a form field overlay takes `"widget": "grid"` with `grid`, a part holding a table node's keys; `enterMoves` (`down`/`right`/`none`), `walk` (`rows`/`columns`); element values `modifiedCount`, `modifiedRows`, `errorCount`, `editing` (`{key, column}` or `null`). Loader refusals: `form` with `rows`; a column `field` naming no member of the row type ("Refused when the document loads").
  - Status-line copy (posted through `ScreenCore::tableMessage`): typing on a read-only cell: `"This cell is read-only"`.
  - `recording_backend.hpp`: drivers `void typeInto(int tableId, std::string text)` (calls the `setOnText` handler), `void clickCell(int tableId, std::size_t viewRow, std::size_t column, bool doubleClick = false)` (`column` the index into `Table::columns`; calls the `setOnCellClick` handler; a double click first calls it with `false`, as a backend does), `void focusOut(int tableId)` (calls the `setOnFocusOut` handler; `RecordingBackend::focus(id)` of a widget outside a table that held focus calls it too); records `setCellMarks` as property `marks:<column>` on the row widget it names (`r`,`m`,`e`,`s`,`u` for the set flags, `""` for none; a pooled cell's marks land on the row widget it is attached to now), `setRowModified` as `modified`, `setCellEditor` as `editor:<column>` (the editor widget's id, `""` for null).

- [ ] **Step 1: Write the fixture** `tests/grid_test_support.hpp` — namespace `morph::test::grid`:

```cpp
// A form whose `specimens` rows are entered in a grid. `density` is computed, `limit` derived (Task 20 adds the
// `x-derived` reading; until then its `x-readonly` makes it read-only). `reviewer` freezes the grid by rule.
inline constexpr std::string_view kSpecimens = R"({"type":"object","title":"Specimens","properties":{
  "sampleId":{"type":"integer","title":"Sample","x-order":0},
  "reviewer":{"type":["string","null"],"title":"Reviewer","x-order":1},
  "specimens":{"type":"array","title":"Specimens","x-order":2,"minItems":0,"maxItems":5,
    "items":{"type":"object","properties":{
      "label":{"type":["string","null"],"title":"Label","x-order":0},
      "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":2,"x-order":1},
      "volume":{"type":["object","null"],"title":"Volume","x-decimalPlaces":1,"x-order":2},
      "density":{"type":["object","null"],"title":"Density","x-decimalPlaces":3,"x-readonly":true,
                 "x-computed":{"inputs":["mass","volume"]},"x-order":3},
      "limit":{"type":["object","null"],"title":"Limit","x-decimalPlaces":2,"x-readonly":true,"x-derived":true,
               "x-order":4},
      "note":{"type":["string","null"],"title":"Note","x-order":5}}}}},
  "required":["sampleId"],
  "x-rules":[{"kind":"readonlyWhen","fields":["specimens"],"when":{"kind":"engaged","fields":["reviewer"]}}]})";

struct GridFixture {
    morph::test::formsengine::Harness h;
    std::unique_ptr<forms::FormSession> form;
    std::unique_ptr<ui::FormGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // Loads `rows` blank rows (keys 1..rows) with sampleId 1, in a comma locale, columns in schema order.
    explicit GridFixture(std::size_t rows, ui::GridOptions options = {}, std::string schema = std::string{kSpecimens});
    [[nodiscard]] std::string json(std::string_view path) const;   // the field's encoded JSON, "" when none
};
inline ui::CellRef at(std::int64_t key, std::string column) { return ui::CellRef{ui::Key{key}, std::move(column)}; }
// Every fetched key of the source's view, in view order.
inline std::vector<ui::Key> keysOf(ui::GridSource const& source) {
    std::vector<ui::Key> keys;
    for (std::size_t i = 0; i < source.rowCount(); ++i) {
        if (auto key = source.keyAt(i)) keys.push_back(*key);
    }
    return keys;
}
```

`grid_test_support.hpp` also gives `columnsOf(GridFixture const&) -> std::vector<ui::TableColumn>` (one per member in schema order, `id` and `label` the member's name) for `GridController::node`. `GridFixture` registers the controller's commit with `source->onBeforeSubmit`, as `makeFormGrid` does, and prefills `{"sampleId":1,"specimens":[{},…]}`, builds the session with `FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"}` and the source with `ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)}` (`localeFacts` returns a `LocaleFacts` row and `withoutGrouping` takes its `render::NumericLocale`, `locale_table.hpp:117`, `:171`), so a cell of `1` at two places shows `1,00`. Add the same schema to `corpusCatalog().schema` in `tests/ui_interpreter_support.hpp` as `Lims_RecordSpecimens`.

- [ ] **Step 2: Write the failing tests** in `tests/test_ui_grid.cpp`:

```cpp
// Mutation: FormGridSource::write parsing the number itself (std::stod after replacing ','), not through the field.
TEST_CASE("grid: a cell encodes as its field does", "[ui][grid]") {
    GridFixture f{3};
    f.grid->focusCell(at(1, "mass"));
    f.grid->type("12,5");
    CHECK(f.grid->press("Enter") == ui::KeyOutcome::Handled);
    CHECK(f.json("specimens[0].mass") == R"({"num":1250,"den":100,"dp":2})");
    CHECK(f.grid->focus() == at(2, "mass"));                 // enterMoves: down
    f.grid->type("1,234");
    f.grid->press("Enter");
    CHECK(f.form->field("specimens[1].mass").draft() == "1,234");   // kept as typed
    CHECK(f.form->field("specimens[1].mass").error().peek()->code == "too-precise");
    CHECK_FALSE(f.form->ready());
    CHECK(f.source->marks(ui::Key{std::int64_t{2}}, "mass").error);
    f.grid->focusCell(at(2, "mass"));
    CHECK(f.grid->message() == "Too many decimal places");    // the forms error, while the cell has focus
}

// Mutation: GridSource::readOnly testing spec().readOnly only (the readonlyWhen section fails).
TEST_CASE("grid: read-only is one rule", "[ui][grid]") {
    GridFixture f{2};
    SECTION("computed and derived members") {
        f.grid->focusCell(at(1, "volume"));
        f.grid->type("2,0");
        f.grid->press("Tab");
        CHECK(f.grid->focus() == at(1, "note"));             // density and limit skipped
        f.grid->press("Shift+Tab");
        CHECK(f.grid->focus() == at(1, "volume"));
        for (std::string const column : {"density", "limit"}) {
            f.grid->focusCell(at(1, column));                  // an arrow or a click reaches it
            f.grid->type("5");
            CHECK_FALSE(f.grid->editing().has_value());
            CHECK(f.grid->message() == "This cell is read-only");
            CHECK(f.source->marks(ui::Key{std::int64_t{1}}, column).readOnly);
        }
    }
    SECTION("a readonlyWhen rule") {
        f.h.type(*f.form, "reviewer", "ana");
        f.grid->focusCell(at(1, "mass"));
        f.grid->type("5");
        CHECK_FALSE(f.grid->editing().has_value());
        CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "mass").readOnly);
        CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);  // no editable cell is left
    }
}

// Mutation: Tab wrapping to the first editable cell, which never leaves the grid.
TEST_CASE("grid: the walk", "[ui][grid]") {
    SECTION("enterMoves down fills a column") {
        GridFixture f{3};
        f.grid->focusCell(at(1, "mass"));
        for (auto const* text : {"1", "2", "3"}) { f.grid->type(text); f.grid->press("Enter"); }
        CHECK(f.json("specimens[2].mass") == R"({"num":300,"den":100,"dp":2})");
        CHECK(f.grid->focus() == at(3, "mass"));             // no row below and no appendRows: it stays
    }
    SECTION("Tab on the last editable cell leaves") {
        GridFixture f{2};
        f.grid->focusCell(at(2, "note"));
        CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);
        f.grid->focusCell(at(1, "label"));
        CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::FocusPrevious);
    }
    SECTION("walk: columns goes down first") {
        GridFixture f{2, ui::GridOptions{.walk = ui::Walk::Columns}};
        f.grid->focusCell(at(1, "mass"));
        f.grid->press("Tab");
        CHECK(f.grid->focus() == at(2, "mass"));
        f.grid->press("Tab");
        CHECK(f.grid->focus() == at(1, "volume"));
    }
    SECTION("Escape restores the draft the cell had when the editor opened") {
        GridFixture f{1};
        f.grid->focusCell(at(1, "label"));
        f.grid->type("a");
        f.grid->press("Enter");
        f.grid->focusCell(at(1, "label"));
        f.grid->type("b");
        f.grid->press("Escape");
        CHECK(f.form->field("specimens[0].label").draft() == "a");
    }
}

// Review Focus 1, walk half. Mutation: an empty grid answering Tab with Handled.
TEST_CASE("grid: an empty grid lets Tab leave at once", "[ui][grid]") {
    GridFixture f{0};
    CHECK_FALSE(f.grid->focus().has_value());
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);
    CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::FocusPrevious);
}

// Mutation: FormRowsSource giving the engine's view order to the array (a source that reorders the field's rows).
TEST_CASE("grid: a sorted grid keeps its record", "[ui][grid]") {
    GridFixture f{3};
    for (auto const& [key, text] : {std::pair{1, "3"}, {2, "1"}, {3, "2"}}) {
        f.grid->focusCell(at(key, "mass")); f.grid->type(text); f.grid->press("Enter");
    }
    REQUIRE(f.source->view().setSort({table::SortKey{.column = "mass"}}).has_value());
    CHECK(keysOf(*f.source) == std::vector<ui::Key>{ui::Key{std::int64_t{2}}, ui::Key{std::int64_t{3}}, ui::Key{std::int64_t{1}}});
    f.grid->focusCell(at(3, "mass")); f.grid->type("9"); f.grid->press("Enter");
    CHECK(f.json("specimens[2].mass") == R"({"num":900,"den":100,"dp":2})");   // in place, entered order
    CHECK(f.json("specimens[0].mass") == R"({"num":300,"den":100,"dp":2})");
    std::uint64_t const added = f.form->field("specimens").addRow();
    f.grid->focusCell(at(static_cast<std::int64_t>(added), "mass")); f.grid->type("0"); f.grid->press("Enter");
    CHECK(keysOf(*f.source).back() == ui::Key{static_cast<std::int64_t>(added)});   // at the end until the sort changes
    REQUIRE(f.source->view().setSort({}).has_value());
    REQUIRE(f.source->view().setSort({table::SortKey{.column = "mass"}}).has_value());
    CHECK(keysOf(*f.source).front() == ui::Key{static_cast<std::int64_t>(added)});   // 0,00 sorts first
}

// Mutation: modified() comparing the whole ObjectArray's snapshot, which marks every cell.
TEST_CASE("grid: what changed", "[ui][grid]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter");
    CHECK(f.form->modified("specimens#1.mass"));
    CHECK_FALSE(f.form->modified("specimens#1.volume"));
    CHECK_FALSE(f.form->modified("specimens#2.mass"));
    CHECK(f.form->rowModified("specimens", 1));
    CHECK_FALSE(f.form->rowModified("specimens", 2));
    CHECK(f.source->modifiedCount() == 1);
    CHECK(f.source->modifiedRows() == 1);
    f.form->revert("specimens#1.mass");
    CHECK(f.form->field("specimens[0].mass").draft().empty());
    CHECK(f.source->modifiedCount() == 0);
    std::uint64_t const added = f.form->field("specimens").addRow();
    CHECK(f.form->rowModified("specimens", added));          // a row added since the load, as a whole
}

// Mutation: modified() false for every read-only member (the readonlyWhen section loses the typed cell's mark).
TEST_CASE("grid: only computed and derived members never draw a modified mark", "[ui][grid]") {
    GridFixture f{1};
    SECTION("a cell frozen by a rule after typing stays modified") {
        f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter");
        f.h.type(*f.form, "reviewer", "ana");                  // readonlyWhen now freezes the grid
        REQUIRE(f.form->field("specimens[0].mass").readonly().get());
        CHECK(f.form->modified("specimens#1.mass"));
        CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "mass").modified);
    }
    SECTION("a computed member written programmatically is not modified") {
        f.form->assign("specimens[0].density", R"({"num":2,"den":1,"dp":3})");
        CHECK_FALSE(f.form->modified("specimens#1.density"));
        CHECK_FALSE(f.form->rowModified("specimens", 1));
    }
}

// Mutation: "Revert row" reverting the focused cell only (label keeps "x").
TEST_CASE("grid: revert cell and revert row from the cell's menu", "[ui][grid]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "label")); f.grid->type("x"); f.grid->press("Tab");
    f.grid->type("4"); f.grid->press("Enter");                 // (1, mass)
    auto entries = f.grid->cellEntries(ui::Key{std::int64_t{2}}, "mass");
    CHECK(labelsOf(entries) == std::vector<std::string>{"Revert cell", "Revert row"});
    CHECK_FALSE(enabledOf(entry(entries, "Revert cell")));       // row 2 holds nothing typed
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "mass");
    entry(entries, "Revert cell").onSelect();
    CHECK(f.form->field("specimens[0].mass").draft().empty());
    CHECK(f.form->field("specimens[0].label").draft() == "x");
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    entry(entries, "Revert row").onSelect();
    CHECK(f.form->field("specimens[0].label").draft().empty());
    CHECK_FALSE(f.form->rowModified("specimens", 1));
}
```

```cpp
// Mutation: the editor holding its text in a buffer of its own until commit (the draft is still "" while typing).
TEST_CASE("grid: the editor's text is the cell's draft, and a commit is one change", "[ui][grid]") {
    GridFixture f{2};
    std::size_t commits = 0;
    f.source->onCommitted([&] { ++commits; });               // test seam: called by committed()
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    CHECK(f.grid->editing() == at(1, "label"));
    CHECK(f.form->field("specimens[0].label").draft() == "a");   // live, before any commit
    CHECK(commits == 0);
    f.grid->type("ab");                                        // the editor's whole new text
    CHECK(f.form->field("specimens[0].label").draft() == "ab");
    f.grid->press("Enter");
    CHECK(commits == 1);
    CHECK_FALSE(f.grid->editing().has_value());
    f.grid->focusCell(at(1, "label")); f.grid->press("F2"); f.grid->press("Enter");   // opened, nothing changed
    CHECK(commits == 1);
    f.grid->focusCell(at(1, "label")); f.grid->type("zz"); f.grid->press("Escape");
    CHECK(f.form->field("specimens[0].label").draft() == "ab");
    CHECK(commits == 1);                                       // Escape is no change
}

// Mutation: FormSession::submit not calling the before-submit hooks (the editor stays open, no commit is counted).
TEST_CASE("grid: the form's submit ends an open edit first", "[ui][grid]") {
    GridFixture f{1};
    std::size_t commits = 0;
    f.source->onCommitted([&] { ++commits; });
    f.grid->focusCell(at(1, "mass")); f.grid->type("7,5");
    REQUIRE(f.grid->editing().has_value());
    f.form->submit(); f.h.settle();
    CHECK_FALSE(f.grid->editing().has_value());
    CHECK(commits == 1);
    REQUIRE(f.h.server.submits().size() == 1);
    CHECK(f.h.server.lastBody().find(R"("mass":{"num":750,"den":100,"dp":2})") != std::string::npos);
}
```

`void FormGridSource::onCommitted(std::function<void()> observer)` is a public seam (one observer, called at the end of `committed()`); the lowering uses it for nothing, Task 20's rounds go through `FormSession::noteCommit`.

`tests/grid_test_support.hpp` also gives `labelsOf(std::vector<ui::MenuItem> const&) -> std::vector<std::string>` (each entry's label as bound now), `entry(std::vector<ui::MenuItem> const&, std::string_view label) -> ui::MenuItem const&` (`REQUIRE`s it exists) and `enabledOf(ui::MenuItem const&) -> bool`; Task 21 reuses them.

Interpreter-level, same file, through `ui::testing::TableHarness` (index #42; `Lims_RecordSpecimens` added to its catalog with the schema above, `modelType` `Lims_SampleModel`, no effects and no records) and a whole document with `"models": {"sample": {"type": "Lims_SampleModel", "key": 4}}` holding `{"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"}`, a text input `before`, `{"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens","columns":[{"id":"mass","field":"mass"},{"id":"note","field":"note"}]}` and a text input `after`:

```cpp
// Mutation: the lowering's form_submit sending the body without running the session's before-submit hooks (the
// editor is still open after the click: `editing` is not null and "editor:0" still names the TextInput).
TEST_CASE("grid: submission commits the open editor and places field errors", "[ui][grid][interpreter]") {
    // (the harness formats in `en`) prefill sampleId 4 and two blank specimens; focus (1, mass), type "7.5" with the
    // editor open; click form_submit
    // CHECK elementValue(h, "grid", "editing") == "null" and the row of key 1 records "editor:0" == ""
    // CHECK the submitted body's specimens[0].mass == {"num":750,"den":100,"dp":2}
    // fail the call with field error {"specimens[1].mass": "must be positive"}
    // CHECK elementValue(h, "grid", "errorCount") == "1" and cellAccent(h, 2, "mass").tooltip == "must be positive"
}
// Mutation: the mount ignoring KeyOutcome::FocusNext.
TEST_CASE("grid: Tab on the last editable cell moves focus to the next control", "[ui][grid][interpreter]") {
    // focus (2, note); h.backend.press(h.byTestId("Table","grid"), "Tab")
    // CHECK h.backend.focused() == h.byTestId("TextInput","after")
    // press "Shift+Tab" on (1, mass): CHECK focused() == byTestId("TextInput","before")
}
// Mutation: the loader checking a column's `field` only when the grid mounts (loadDocumentText succeeds).
TEST_CASE("grid: a column naming no member is refused when the document loads", "[ui][grid][interpreter]") {
    // a column {"id":"x","field":"nosuch"}: loadDocumentText fails; the message names "nosuch"
}
// Mutation: the mount not wiring setOnFocusOut to GridController::leave (the draft stays uncommitted, "" not "3").
TEST_CASE("grid: a click focuses a cell, a double click opens its editor, focus leaving commits", "[ui][grid][interpreter]") {
    // int const table = h.byTestId("Table","grid");
    // (columns by index into Table::columns: mass 0, note 1)
    // h.backend.clickCell(table, 0, 0): CHECK elementValue(h, "grid", "editing") == "null" and the recorded focus cell is (0, 0)
    // h.backend.clickCell(table, 1, 0, true): CHECK elementValue(h, "grid", "editing") == R"({"key":2,"column":"mass"})"
    //   and the table's row 2 records "editor:0" naming a TextInput widget that has focus (h.backend.focused())
    // h.backend.edit(<that TextInput>, "3"); h.backend.focusOut(table)
    // CHECK the form field specimens[1].mass draft == "3", elementValue(h, "grid", "editing") == "null", "editor:0" recorded ""
    // h.backend.typeInto(table, "8") with focus on (1, note): the editor opens on (1, note) holding "8"
    // CHECK the table's declared keys are exactly {Enter, Tab, Shift+Tab, Escape, Ctrl+Enter} while it is open
}
```

Mount-level, same file, over `GridFixture` and a `RecordingBackend` mounting `f.grid->node(columnsOf(f), {})` (column indices into `Table::columns`, in schema order: `label` 0, `mass` 1, `volume` 2, `density` 3, `limit` 4, `note` 5):

```cpp
// Mutations: the marks bound in the row's scope instead of the cell's (the pooled cell showing row 30 keeps row 1's
// "m"); setCellMarks skipped when a pooled cell joins another row widget with equal marks.
TEST_CASE("grid: a cell's marks are drawn by its cell view and follow the row it shows", "[ui][grid][mount]") {
    // GridFixture f{40}; mounted with 20 realized rows.
    // focus (1, mass), type "4", Enter: the row widget showing key 1 records marks:1 == "m" and modified == true;
    //   its marks:3 (density, computed) contains "r"; the row widget showing key 2 records marks:1 == "" and
    //   modified == false
    // scroll to view row 25, so the row of key 1 leaves the window: the row widget showing key 30 records
    //   marks:1 == ""; Mounted::stats().cellsRebound > 0 and cellsCreated is unchanged (no cellView call)
    // f.form->revert("specimens#1.mass"); scroll back to view row 0: the row widget showing key 1 records
    //   marks:1 == "" and modified == false
}
```

These five are written out in full Catch2 with the harness calls named in the comments; the comments are the assertions.

A hidden column keeps its index (index #37), same file, mount level:

```cpp
// Mutation: the mount passing a cell's place in the layout as `column` (with `label` hidden, mass reports 0).
TEST_CASE("grid: a cell's column is its index among every column, hidden ones included", "[ui][grid][mount]") {
    // GridFixture f{2}; mounted; the arrangement hides `label` (index 0), so mass is the first visible column
    // focus (1, mass), type "4", Enter: the row widget showing key 1 records marks:1 == "m" and no marks:0 change
    // h.backend.clickCell(table, 0, 1, true): f.grid->editing() == at(1, "mass") and that row records editor:1
}
```

Qt Quick, `tests/qt_quick/test_qt_quick_table.cpp`:

```cpp
// Mutation: the QML cell ignoring setOnText.
TEST_CASE("qt_quick grid: typing on a focused cell opens its editor, Tab walks and leaves", "[qt_quick][grid]") {
    // A mounted formGrid of two rows between two text inputs: QTest::keyClicks "5" on (1, mass) shows a TextField
    // holding "5" in that cell; Tab commits it (field draft "5") and focuses (1, volume); Tab on (2, note) gives
    // focus to the second text input's item.
}
// Mutation: the QML cell sending a double click as two single clicks (no editor opens).
TEST_CASE("qt_quick grid: a double click opens the editor in the cell and focus leaving commits", "[qt_quick][grid]") {
    // QTest::mouseDClick on (2, mass): a TextField item lies inside that cell's rectangle and has active focus;
    // keyClicks "7", then a click on the second text input: field draft "7", no TextField in the table.
}
```

Terminal, `tests/tui/test_tui_table.cpp`:

```cpp
// Mutation: the terminal table ignoring setOnText (the keystroke moves nothing and opens no editor).
TEST_CASE("tui grid: typing on a focused cell opens its editor, Enter commits and moves down", "[tui][grid]") {
    // A mounted formGrid of two rows: focus (1, mass), feed the key "5": the rendered row 1 shows "5" with the
    // terminal cursor in the mass column; feed Enter: field draft "5", focus (2, mass); feed Tab on (2, note):
    // focus leaves the table to the next control.
}
```

- [ ] **Step 3: Run to see them fail**

Run: `cmake --build --preset cl-debug --target morph_tests`
Expected: compile errors — `morph/ui/grid.hpp` not found, `FormSession::modified` not declared.

- [ ] **Step 4: Implement the focus chain** in `view.hpp`, `backend.hpp`, `mount.hpp` with the signatures above, `markedCell` and the mount's binding of a cell's marks in the cell's own scope, `TableRowLook::modified`, and the `RecordingBackend` drivers and records listed under Produces. Then move the scalar controls to `field_control.hpp` with no change of behaviour: `ctest --test-dir build/cl-debug -R "form" --output-on-failure` passes unchanged before anything else in `form_view.hpp` changes.

- [ ] **Step 5: Implement per-cell snapshots** in `form_session.hpp`: `field()` by `#key`, `modified`, `rowModified`, `revert`, the load record; and the before-submit hooks (`addBeforeSubmit`, `removeBeforeSubmit`, called first in `submit()`).

- [ ] **Step 6: Implement `form_rows.hpp` and `grid.hpp`** with the signatures above. The walk, which the signatures do not determine:

```text
next(from, forward):                                  // view positions at the moment of the call
  start = positionOf(from.key) (or before the first / after the last when there is no focus)
  cells = [0, rowCount()) × columns() in walk order (Rows: row-major; Columns: column-major), stepping from start
  for each cell: keyAt(row) empty -> return Stop{row, column}       // a row not fetched yet: never walked past
                 !readOnly && !covered -> return Found{cell}
  return None
Stop{row, column}: place() becomes GridFocus{nullopt, row, column} and focus() empty (Task 10a's placeholder focus,
            which keeps the row realized, so the window fetches it); Handled. When keyAt(row) answers, place()
            takes the key and focus() names the cell; a Tab or Enter pressed before that is Handled and moves nothing.
Tab:        commit an open editor; next(focus, true)  -> Found: focus it, Handled;  Stop: as above;  None -> FocusNext
Shift+Tab:  commit; next(focus, false)               -> the same, None -> FocusPrevious
Enter (editor open): commit; Down: first editable cell in the same column below; Right: next(focus, true) within
            the row; None: stay. Nothing found: stay; a row not fetched yet below: Stop. Enter, F2 or a double
            click with no editor: activate(focus).
Space on a Boolean cell: toggles and commits, no editor. Delete with no editor: blanks the cell (write "").
Arrows: move to any cell (read-only included, covered cells passed over); Ctrl+arrow to the last cell that way.
```

Commit and `Escape` follow **Where the editor's text lives** above: the draft is live, a commit closes the editor and calls `committed()` once when the draft changed, `Escape` writes the kept draft back.

- [ ] **Step 7: Implement the lowering and the loader** in `src/ui/document.cpp`, `src/ui/interpreter.cpp`: `table` with `form`/`field` lowers to `makeFormGrid` over the form element's session, the `FormGrid` kept in the table's `ElementState`; the lowering's `form_submit` runs the session's before-submit hooks through `FormSession::submit`; the `grid` overlay adds `overrides.byField(id, …)` drawing the same; element values; refusals. `form_view.hpp` draws an `ObjectArray` whose `spec().widget == "grid"` with `ui::formGrid(session, state, <every member in schema order>, {}, ui::formatOf(session))`; the overlay's `grid` part replaces those columns and options. The lowering reads the `FormGrid`'s controller: it posts `GridController::message()` with its `messageEntries()` through `tableMessage`, writes `standing()`/`standingEntries()` into the table's `TableStatus`, and routes `setOnStatusEntry` to `chooseEntry`.

- [ ] **Step 8: Implement editors in cells** in the Qt Quick and terminal tables: `setOnText`, `setOnCellClick`, `setOnFocusOut`, `setCellEditor` (the editor's item placed over the cell's rectangle and given focus; every `column` an index into `Table::columns`), `setCellMarks`, `setRowModified`, `Tab`/`Shift+Tab` delivered to a table that lists them.

- [ ] **Step 9: Run the tests**

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "grid:" --output-on-failure`
Expected: all `grid:` cases pass. Then `ctest --test-dir build/cl-debug -R "form|table|mount" --output-on-failure` — no regressions. Qt Quick and terminal: `ctest --test-dir build/cl-qt-debug -R "qt_quick grid|tui grid" --output-on-failure` — pass.

- [ ] **Step 10: Mutate** each named mutation once; record the failing lines.

- [ ] **Step 11: Docs** per this task's Docs list; run the docs build (new public symbols).

- [ ] **Step 12: Commit**

```bash
git add include/morph/ui include/morph/forms/engine src/ui src/qt_quick src/tui tests docs/spec
git commit -m "feat(ui): grids of a form"
```

### Task 20: Derived values (editing.md step 2)

**Spec:** editing.md "Derived values", "Columns are fields" (Per row), "The editor" (choices that depend on the row), Invariants 2, 3, 9; Failure modes rows "An evaluation or a derive call fails", "A reply arrives after a newer one", "A derive reply names a path that is not a derived member"; Delivery order step 2.

**Files:**
- Create: `include/morph/forms/derive.hpp`, `tests/forms/test_forms_derive.cpp` (add to `morph_tests`)
- Modify: `include/morph/forms/forms.hpp`, `include/morph/forms/engine/field_model.hpp`, `include/morph/forms/engine/form_session.hpp`, `include/morph/core/registry.hpp`, `include/morph/ui/grid.hpp`, `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `tests/test_ui_grid.cpp`, `tests/grid_test_support.hpp`, `tests/qt_quick/test_qt_quick_table.cpp`
- Docs: `docs/spec/forms/engine.md` (rounds, `loadDerived`, `deriveDebounce`, row-dependent Choices), `docs/spec/forms/forms.md` (`computedFields` on a row type as the exception to "top-level only"; `derived()` and `x-derived`, untouched by `recomputeAll`), `docs/spec/core/registry.md` (`DeriveOf`, `Derived`; `evaluate` recomputing row members; `clearDerived`), `docs/spec/ui/document.md` and `docs/spec/ui/interpreter.md` (`derive`, `deriveDebounce`; a column's `field` expression and per-row overlays).

**Interfaces:**
- Consumes: Task 19's `GridColumn`, `FormGridSource`, `GridController`, `GridSource::committed`; `ActionDispatcher::evaluate`/`evaluateAction`/`prepareAction` (existing); `ModelChannel::execute(…, CallOptions{.evaluate = true})` (existing); `reactive::Scheduler::after` (existing).
- Produces:
  - `derive.hpp`: `template <typename Action> struct DeriveOf { Action body; };` `struct Derived { struct Value { std::string path; glz::raw_json value; }; std::vector<Value> values; };` (JSON `{"values":[{"path":…,"value":…}]}`); `template <auto MemberPtr> [[nodiscard]] FieldMeta derived(std::string_view label = {}, std::string_view help = {}) noexcept` (= `describe<MemberPtr>(label, help).withDerived()`); `template <typename A> void clearDerived(A& action)` (value-initialises every member whose `FieldMeta` is `derived`, in `A` and in each element of a `std::vector` member of a reflectable aggregate, recursively); `enum class DeriveState : std::uint8_t { Current, Running, Failed };` `struct DeriveOptions { std::function<async::Completion<std::string>(std::string bodyJson)> evaluate; std::function<async::Completion<std::string>(std::string deriveBodyJson)> derive; reactive::Scheduler* scheduler = nullptr; std::chrono::milliseconds debounce{150}; };` `struct DeriveRound { std::uint64_t sequence = 0; std::string body; std::map<std::string, std::vector<std::uint64_t>, std::less<>> rowKeys; };` `struct DeriveLoad { std::size_t loaded = 0; std::vector<std::string> refused; };`
  - `forms.hpp`: `bool FieldMeta::derived{false};` `constexpr FieldMeta withDerived() const noexcept`; the schema writes `"x-derived": true` and leaves the member out of `required`; `annotateNestedAggregate<Sub>` writes `x-computed`/`x-readonly` for `Sub::computedFields`; `recomputeAll<A>` recomputes each element of every `std::vector<R>` member with `HasComputedFields<R>` (recursively) **before** `A`'s own entries, so a top-level total reads recomputed rows.
  - `field_model.hpp`: `bool FieldSpec::derived = false;` read from `x-derived`; `FieldState::readonly()` is true for a derived member.
  - `registry.hpp`: `prepareAction` and `evaluateAction` call `forms::clearDerived(action)` before `recomputeAll`.
  - `form_session.hpp`: `void deriveWith(DeriveOptions options)`; `void noteCommit()`; `void recalculate()`; `[[nodiscard]] DeriveState deriveState() const` (tracked); `[[nodiscard]] std::string partialBody() const` (every top-level field that encodes; an `Object` or `ObjectArray` member by member, leaving out blank and non-encoding members; every row written, in order, `{}` for a row with nothing); `[[nodiscard]] DeriveRound captureRound()` (next sequence, `partialBody()`, the row keys of every `ObjectArray` by `FieldState::path()`); `DeriveLoad loadDerived(DeriveRound const& round, Derived const& reply)` (derived members only); `DeriveLoad loadDerived(DeriveRound const& round, std::string_view evaluatedBody)` (computed members only, the reply's `errors` and `ready` ignored). Both resolve `name[i]` through `round.rowKeys[name][i]` and the row's current position by key, drop a row since removed, refuse a path that names no member of their kind, write programmatically and move no snapshot. `std::uint64_t FieldState::insertRow(std::size_t position)` (a user action; position past the end appends).
  - Choices: a row member's `x-optionsDependsOn` names are looked up in its row, then among the top-level fields; a row member's options are fetched only after `void FieldState::openOptions()` (what a cell editor calls when it opens); identical `(optionsAction, request)` pairs share one fetch for the session's life, a failed fetch is not kept.
  - `grid.hpp`: `GridColumn` gains `std::function<std::string(forms::RowState const&)> fieldOf` (a member per row; tracked) and per-row overlays `decimals` (`std::function<std::optional<std::uint32_t>(forms::RowState const&)>`), `unit` (`std::function<std::optional<std::string>(…)>`), `readonly` (`std::function<bool(…)>`), `options` (`std::function<std::optional<std::vector<std::string>>(…)>`, option JSON literals kept), `placeholder` (`std::function<std::string(…)>`). A column with `fieldOf` has `ColumnKind::Text` in `FormRowsSource`, its cell the member's draft. Derived and computed cells draw `CellMarks::muted` while `deriveState()` is `Running` and `stale` while `Failed`. While `Failed`, `GridController::standing()` is `"Derived values could not be recalculated"` and `standingEntries()` is `{StatusEntry{"recalculate", "Recalculate"}}` (a standing state, index #19: it lasts until a round succeeds, through the message hold); `chooseEntry("recalculate")` calls `FormSession::recalculate()`. Both are empty in every other state.
  - Document: `form` takes `derive` (an action id of the form's model) and `deriveDebounce` (a literal integer, milliseconds); a grid column's `field` may be an expression in the row's scope, and takes `decimals`, `unit`, `readonly`, `options`, `placeholder` as expressions in the row's scope. The lowering calls `deriveWith` with `evaluate` = the form alias's channel `execute(action, body, {.evaluate = true})` and `derive` = `execute(derive, R"({"body":)" + body + "}")`, and reports each refused path once.

- [ ] **Step 1: Write the failing server tests** in `tests/forms/test_forms_derive.cpp`:

```cpp
struct DeriveBox {
    std::int64_t count = 0;
    std::int64_t perBox = 0;
    std::int64_t total = 0;   // computed: count * perBox
    std::int64_t limit = 0;   // derived: the model's
    static constexpr auto computedFields = morph::forms::computeList(
        morph::forms::computed<&DeriveBox::total, &DeriveBox::count, &DeriveBox::perBox>(
            [](auto const& box) { return box.count * box.perBox; }));
    static const std::array<morph::forms::FieldMeta, 1> fieldMetadata;
};
inline const std::array<morph::forms::FieldMeta, 1> DeriveBox::fieldMetadata{morph::forms::derived<&DeriveBox::limit>()};

struct RecordBoxes {
    std::vector<DeriveBox> boxes;
    std::int64_t sum = 0;     // computed: the boxes' totals
    static constexpr auto computedFields = morph::forms::computeList(morph::forms::computed<&RecordBoxes::sum, &RecordBoxes::boxes>(
        [](auto const& record) { std::int64_t s = 0; for (auto const& b : record.boxes) s += b.total; return s; }));
};
struct DeriveBoxModel {
    static inline RecordBoxes last{};
    bool execute(RecordBoxes const& record) { last = record; return true; }   // sets no derived member
};
BRIDGE_REGISTER_MODEL(DeriveBoxModel, "T_DeriveBoxes")
BRIDGE_REGISTER_ACTION(DeriveBoxModel, RecordBoxes, "T_RecordBoxes")

constexpr std::string_view kForged = R"({"boxes":[{"count":2,"perBox":3,"total":999,"limit":555}],"sum":999})";

// Mutation: recomputeAll skipping row members (total stays 999, sum 999); a second run with clearDerived removed
// from prepareAction (limit stays 555).
TEST_CASE("derive: the server keeps recomputeAll's values and the model's", "[forms][derive]") {
    auto holder = morph::model::detail::defaultRegistry().create("T_DeriveBoxes");
    morph::model::detail::defaultDispatcher().dispatch("T_DeriveBoxes", "T_RecordBoxes", *holder, kForged);
    RecordBoxes const& kept = DeriveBoxModel::last;
    CHECK(kept.boxes.at(0).total == 6);
    CHECK(kept.sum == 6);
    CHECK(kept.boxes.at(0).limit == 0);   // the client's 555 is cleared; the model set nothing
    auto const evaluated = morph::model::detail::defaultDispatcher().evaluate("T_DeriveBoxes", "T_RecordBoxes", kForged);
    CHECK(evaluated.body == R"({"boxes":[{"count":2,"perBox":3,"total":6,"limit":0}],"sum":6})");
}

// Mutation: annotateNestedAggregate not writing x-computed for a row type.
TEST_CASE("derive: a row type's schema marks computed and derived members", "[forms][derive]") {
    auto dom = glz::read_json<glz::generic_u64>(morph::forms::schemaJson<RecordBoxes>()).value();
    auto& box = dom["properties"]["boxes"]["items"];   // DeriveBox is used once: glaze inlines it
    CHECK(glz::write_json(box["properties"]["total"]["x-readonly"]).value() == "true");
    CHECK(glz::write_json(box["properties"]["total"]["x-computed"]["inputs"]).value() == R"(["count","perBox"])");
    CHECK(glz::write_json(box["properties"]["limit"]["x-derived"]).value() == "true");
    CHECK(glz::write_json(box["required"]).value() == R"(["count","perBox"])");
}
```

- [ ] **Step 2: Write the failing round tests** in `tests/test_ui_grid.cpp`. `GridFixture` gains `reactive::testing::ManualScheduler scheduler;` two `formsengine::FakeServer` recorders `evaluate` and `derive` whose responders return `std::nullopt` (every call stays pending until resolved by index), and `void enableDerive()`, which calls `form->deriveWith({.evaluate = …, .derive = …, .scheduler = &scheduler})` (debounce 150 ms) over them.

```cpp
using namespace std::chrono_literals;
std::string const kNoValues = R"({"values":[]})";

// Mutation: loadDerived resolving `specimens[i]` against the current rows (the value lands on the inserted row).
TEST_CASE("grid: derived values", "[ui][grid][derive]") {
    GridFixture f{3};
    f.enableDerive();
    f.grid->focusCell(at(1, "mass")); f.grid->type("4,00"); f.grid->press("Enter");
    f.grid->type("abc"); f.grid->press("Enter");                 // (2, mass): does not encode, left out
    f.scheduler.advance(149ms); f.h.settle();
    CHECK(f.evaluate.submits().empty());
    f.scheduler.advance(1ms); f.h.settle();
    REQUIRE(f.evaluate.submits().size() == 1);
    REQUIRE(f.derive.submits().size() == 1);
    CHECK(f.evaluate.submits().at(0).body == R"({"sampleId":1,"specimens":[{"mass":{"num":400,"den":100,"dp":2}},{},{}]})");
    CHECK(f.derive.submits().at(0).body == R"({"body":{"sampleId":1,"specimens":[{"mass":{"num":400,"den":100,"dp":2}},{},{}]}})");
    CHECK(f.form->deriveState() == forms::DeriveState::Running);
    CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "limit").muted);

    f.form->field("specimens").insertRow(1);                                  // above rows 2 and 3
    f.grid->focusCell(at(2, "note")); f.grid->type("typed"); f.grid->press("Enter");   // a commit during the round
    f.derive.resolveSubmit(0, R"({"values":[{"path":"specimens[1].limit","value":{"num":7,"den":1,"dp":2}},
                                            {"path":"specimens[1].note","value":"server"}]})");
    f.evaluate.resolveSubmit(0, R"({"sampleId":1,"specimens":[{"mass":{"num":400,"den":100,"dp":2},
                                    "density":{"num":2,"den":1,"dp":3}},{},{}],"errors":{},"ready":true})");
    f.h.settle();
    CHECK(f.json("specimens[2].limit") == R"({"num":700,"den":100,"dp":2})");   // row key 2, now at position 2
    CHECK(f.json("specimens[1].limit").empty());                                // the inserted row is untouched
    CHECK(f.json("specimens[0].density") == R"({"num":2000,"den":1000,"dp":3})");
    CHECK(f.form->field("specimens#2.note").draft() == "typed");               // not a derived member: refused
    CHECK(f.form->modified("specimens#2.note"));
    CHECK(f.evaluate.submits().size() == 2);                                    // the stale round: one more, at once
    CHECK(f.derive.submits().size() == 2);
    f.evaluate.resolveSubmit(1, R"({"errors":{},"ready":false})");
    f.derive.resolveSubmit(1, kNoValues);
    f.h.settle();
    CHECK(f.form->deriveState() == forms::DeriveState::Current);

    // A failed round keeps the values, marks them stale, and Recalculate runs one more.
    f.grid->focusCell(at(1, "mass")); f.grid->type("5"); f.grid->press("Enter");
    f.scheduler.advance(150ms); f.h.settle();
    f.evaluate.rejectSubmit(2, "unreachable");
    f.derive.resolveSubmit(2, kNoValues);
    f.h.settle();
    CHECK(f.form->deriveState() == forms::DeriveState::Failed);
    CHECK(f.source->marks(ui::Key{std::int64_t{2}}, "limit").stale);
    CHECK(f.json("specimens[2].limit") == R"({"num":700,"den":100,"dp":2})");
    CHECK(f.grid->standing() == "Derived values could not be recalculated");
    CHECK(f.grid->standingEntries() == std::vector<ui::StatusEntry>{{"recalculate", "Recalculate"}});
    f.grid->chooseEntry("recalculate"); f.h.settle();
    CHECK(f.evaluate.submits().size() == 4);
}

// Mutation: loadDerived without the sequence check (the late older reply overwrites 9 with 8).
TEST_CASE("grid: an older derive reply is ignored", "[ui][grid][derive]") {
    GridFixture f{2};
    auto const older = f.form->captureRound();
    auto const newer = f.form->captureRound();
    auto const value = [](std::string json) { return forms::Derived{.values = {{"specimens[1].limit", glz::raw_json{std::move(json)}}}}; };
    CHECK(f.form->loadDerived(newer, value(R"({"num":9,"den":1,"dp":2})")).loaded == 1);
    CHECK(f.form->loadDerived(older, value(R"({"num":8,"den":1,"dp":2})")).loaded == 0);
    CHECK(f.json("specimens[1].limit") == R"({"num":900,"den":100,"dp":2})");
}

// Mutation: loadDerived resolving `specimens[i]` against the current rows (row 2's path falls past the end, which
// passes for the wrong reason, so the reply also names specimens[0], which must still land on row 1).
TEST_CASE("grid: a value for a row removed since the call is dropped", "[ui][grid][derive]") {
    GridFixture f{2};
    auto const round = f.form->captureRound();                 // rows 1, 2 at sending
    f.form->field("specimens").removeRow(2);                   // then row 2 goes
    auto const load = f.form->loadDerived(round, forms::Derived{.values = {
        {"specimens[1].limit", glz::raw_json{R"({"num":1,"den":1,"dp":2})"}},
        {"specimens[0].limit", glz::raw_json{R"({"num":3,"den":1,"dp":2})"}}}});
    CHECK(load.loaded == 1);
    CHECK(load.refused.empty());                                 // a removed row is dropped, not refused
    CHECK(f.json("specimens[0].limit") == R"({"num":300,"den":100,"dp":2})");
    CHECK(f.form->field("specimens").rows().size() == 1);
}

// Mutation: a derive reply that moves the loaded snapshot of the members it writes.
TEST_CASE("grid: a derive reply moves no snapshot", "[ui][grid][derive]") {
    GridFixture f{1};
    f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter");
    auto const round = f.form->captureRound();
    auto const load = f.form->loadDerived(round, forms::Derived{.values = {{"specimens[0].limit", glz::raw_json{R"({"num":1,"den":1,"dp":2})"}},
                                                                           {"specimens[0].mass",  glz::raw_json{R"({"num":5,"den":1,"dp":2})"}}}});
    CHECK(load.loaded == 1);
    CHECK(load.refused == std::vector<std::string>{"specimens[0].mass"});
    CHECK(f.form->modified("specimens#1.mass"));
    CHECK(f.form->field("specimens[0].mass").draft() == "4");
    CHECK_FALSE(f.form->modified("specimens#1.limit"));        // a derived member draws no modified mark
}

// Mutation: the column's field resolved once for every row.
TEST_CASE("grid: kind per row", "[ui][grid][derive]") {
    // schema: rows {isChoice: boolean, choice: enum ["low","high"], amount: decimal 1 place, mass: decimal 2 places};
    // column {id: "value", fieldOf: isChoice ? "choice" : "amount"}; column "mass" with decimals overlay row 1 -> 1, row 2 -> 3
    // rows: [{isChoice: true}, {isChoice: false}]
    CHECK(f.source->field(ui::Key{std::int64_t{1}}, "value")->spec().kind == forms::FieldKind::Enum);
    CHECK(f.source->field(ui::Key{std::int64_t{2}}, "value")->spec().kind == forms::FieldKind::Decimal);
    f.grid->focusCell(at(2, "value")); f.grid->type("3,5"); f.grid->press("Enter");
    CHECK(f.json("rows[1].amount") == R"({"num":35,"den":10,"dp":1})");
    f.grid->focusCell(at(1, "mass")); f.grid->type("1,25"); f.grid->press("Enter");
    CHECK(f.form->field("rows[0].mass").error().peek()->code == "too-precise");
    f.grid->focusCell(at(2, "mass")); f.grid->type("1,255"); f.grid->press("Enter");
    CHECK(f.json("rows[1].mass") == R"({"num":1255,"den":1000,"dp":3})");
}

// Mutation: a fetch per row (no shared fetch).
TEST_CASE("grid: choices per row", "[ui][grid][derive]") {
    // rows {material: string, grade: Choice "Lims_ListGrades", x-optionsDependsOn ["material","site"]}; top-level "site"
    // prefill site "north", six rows with material A, A, B, B, C, A
    CHECK(f.h.server.fetches().empty());                     // nothing is fetched before an editor opens
    for (std::int64_t key = 1; key <= 6; ++key) { f.grid->focusCell(at(key, "grade")); f.grid->press("F2"); f.grid->press("Escape"); }
    REQUIRE(f.h.server.fetches().size() == 3);
    CHECK(f.h.server.fetches().at(0).body == R"({"material":"A","site":"north"})");
    CHECK(f.h.server.fetches().at(1).body == R"({"material":"B","site":"north"})");
    CHECK(f.h.server.fetches().at(2).body == R"({"material":"C","site":"north"})");
}
```

Qt Quick: `"qt_quick grid: a column's kind follows its row"` — row 1's `value` cell opens a ComboBox, row 2's a TextField. Mutation as "kind per row".

- [ ] **Step 3: Run to see them fail**

Run: `cmake --build --preset cl-debug --target morph_tests`
Expected: compile errors — `morph/forms/derive.hpp` not found; `FormSession::deriveWith` not declared.

- [ ] **Step 4: Implement `derive.hpp`, `FieldMeta::derived`, the schema, `recomputeAll` over rows and `clearDerived` in `prepareAction`/`evaluateAction`.**

- [ ] **Step 5: Implement rounds in `FormSession`.** The order the signatures do not determine:

```text
noteCommit():   a round in flight: mark it stale. Else: no scheduler: startRound() now; else restart the timer
                for options.debounce, whose expiry calls startRound()
startRound():   round = captureRound(); state = Running; send evaluate (if set) and derive (if set) with round.body
on a reply:     loadDerived(round, reply). loadDerived itself ignores a round whose sequence is below the last one it
                loaded for that kind of reply (evaluation or derive), and records the sequence it loads.
both settled:   any failed: state = Failed, else Current; stale: clear it and startRound() at once
recalculate():  startRound() unless a round is in flight
```

- [ ] **Step 6: Implement row-scoped Choices** (`openOptions`, lookup order, the shared fetch) and the per-row column parts in `grid.hpp`; the lowering's `derive`, `deriveDebounce`, column expressions and overlays.

- [ ] **Step 7: Run the tests**

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "derive|grid:|computed|evaluate|forms" --output-on-failure`
Expected: all pass, including the existing `test_computed_fields.cpp` and `test_ui_evaluate.cpp` cases.

- [ ] **Step 8: Mutate**, **Step 9: Docs** (and the docs build), **Step 10: Commit** `feat(forms): derived values for grids`.

### Task 21: Selecting cells, paste and rows of a grid (editing.md step 3)

**Spec:** editing.md "Selecting cells, clipboard and fill", "Rows of a grid", Invariant 7; Failure modes rows on paste, insert and delete, `selectionMode: "cells"` on a model's table.

**Files:**
- Modify: `include/morph/ui/view.hpp`, `include/morph/ui/backend.hpp`, `include/morph/ui/mount.hpp`, `include/morph/ui/testing/recording_backend.hpp`, `include/morph/ui/grid.hpp`, `include/morph/forms/engine/field_model.hpp`, `include/morph/forms/engine/form_session.hpp`, `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `src/qt_quick/table.cpp` and QML, `src/tui/table_widget.cpp`; `tests/test_ui_grid.cpp`, `tests/test_ui_mount.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`
- Docs: `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md` (`selectionMode: "cells"` and its value, `appendRows`, `rowsFixed`, the row menu entries, "Add row"), `docs/spec/ui/view_tree.md` and `docs/spec/ui/backend_contract.md` (`Table::cellSelection`, `setCellSelection`, `setOnCellSelect`).

**Interfaces:**
- Consumes: Task 19's `GridController`, `GridSource`; Task 20's `FieldState::insertRow`; Task 12b's `IViewBackend::copyText` (the lowering hands it `GridController::copy()`) and Task 9d's `ScreenCore::tableMessage` with Task 17a's entries overload; Task 17d's `TableOptions::cellMenu`; Task 19's `cellEntries`, `messageEntries`, `chooseEntry`.
- Produces:
  - `view.hpp`: `struct CellRange { CellRef anchor; CellRef focus; bool operator==(CellRange const&) const = default; };` (beside `CellRef`); `Table::cellSelection` (`std::optional<Prop<std::optional<CellRange>>>`: when set, the table selects cells, not rows) and `Table::onCellSelect` (`std::function<void(std::optional<CellRange>)>`). The mount maps between keys and view positions: it shows a range with `setCellSelection` at the corners' view positions and visible positions now (so a sort or a column move redraws the rectangle the same corners span), and turns a reported rectangle into a `CellRange` through the rows' keys and the layout's ids. A corner whose row is not in the view, or whose column is hidden, shows no rectangle; the range is kept.
  - `backend.hpp` (defaults do nothing): `virtual void TableWidget::setCellSelection(std::optional<std::array<std::size_t, 4>> rect)` (`{firstRow, rowCount, firstPosition, positionCount}` in view rows and visible positions — a rectangle is of the layout, so these are positions, not `Table::columns` indices, index #37; empty: none; never calls the handler) and `virtual void TableWidget::setOnCellSelect(std::function<void(std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow, std::size_t focusPosition)> onSelect)` (a drag from cell to cell, a `Shift`+click, a row header click — anchor `(row, 0)`, focus `(row, last position)` — and a column header's selection gesture — anchor `(0, position)`, focus `(last row, position)`). In cell mode the backend draws no row selection and reports no `onSelectionChange`. `Shift`+arrows and `Ctrl+A` are the grid's chords (declared with `setKeys`), not the backend's.
  - `recording_backend.hpp`: records `setCellSelection` as property `cellSelection` (`"r<first>+<count>,c<first>+<count>"`, `""` for none; `c` counts visible positions); driver `void selectCells(int tableId, std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow, std::size_t focusPosition)`.
  - `field_model.hpp`: `std::optional<std::size_t> FieldSpec::minItems, maxItems;` read from an array's `minItems`/`maxItems`.
  - `form_session.hpp`: `struct RemovedRow { std::size_t position; std::shared_ptr<RowState> row; };` `std::vector<RemovedRow> FieldState::removeRows(std::span<std::uint64_t const> keys);` `void FieldState::restoreRows(std::vector<RemovedRow> rows);` (each back at its position, ascending).
  - `grid.hpp`: `GridOptions` gains `bool appendRows = false; bool rowsFixed = false;` `GridController` gains `[[nodiscard]] std::optional<CellRange> selection() const` (tracked), `void select(std::optional<CellRange> range)`, `[[nodiscard]] std::string copy() const` (tab-separated display text of the rectangle, view order, layout order; a tab or line break inside a cell written as a space), `PasteResult paste(std::string_view text)`, `void fillDown()`, `void clear()`, `void insertRow(bool below)`, `void deleteRows()`, `void undo()`, `[[nodiscard]] RowOps rowOps() const` (tracked); `struct PasteResult { std::size_t written = 0; std::size_t readOnly = 0; std::size_t pastRows = 0; std::size_t pastColumns = 0; };` `[[nodiscard]] std::string pasteMessage(PasteResult const& result);` `struct RowOps { bool insert; bool remove; std::string why; };` `GridSource` gains `appendRow() -> std::optional<Key>`, `insertRow(std::size_t position) -> std::optional<Key>`, `removeRows(std::span<Key const>) -> bool` (true when one held a value), `undoRemove() -> void`, `bounds() -> std::pair<std::size_t, std::optional<std::size_t>>` (minItems, maxItems).
  - Chords: `Shift`+arrows extend from the anchor; `Ctrl+A` the whole grid; `Ctrl+C` copy; `Ctrl+V` paste; `Ctrl+D` fill down; `Delete` clear; `Ctrl+Shift+=` insert below; `Ctrl+-` delete; a row header click selects its row, a column header's selection gesture its column.
  - Copy, exact: paste message `"Pasted {written} values"`, then, when anything was dropped, `"; "` and the parts present joined by `" and "`: `"{readOnly} on read-only cells"`, `"{pastRows} past the last row"`, `"{pastColumns} past the last column"`, then `" were dropped"`. Deletion of rows holding values: the message `"Deleted {n} rows"` (`"Deleted 1 row"`) with `messageEntries()` `{StatusEntry{"undo", "Undo"}}` (index #19: a transient notice with a choice, posted through `tableMessage`'s entries overload); `chooseEntry("undo")` calls `undo()`, which restores the rows while no change has followed the deletion and does nothing after one (the next change also clears the entries). Disabled entries: at `maxItems` `"A grid holds at most {maxItems} rows"`, at `minItems` `"A grid holds at least {minItems} rows"`; with `rowsFixed` no entry is offered.
  - Paste, which the signatures do not determine: lines split on `\n`, a trailing `\r` removed from each, a final empty line dropped; cells split on `\t`; each value trimmed of spaces. Start: the focused cell; in a grid with no rows, the first visible column of the first row the paste adds. Row `r0+i` past the last row: appended when `appendRows` and the count is below `maxItems`, else every value of that line counts in `pastRows`. Column `c0+j` past the last visible column counts in `pastColumns`. A read-only or covered target counts in `readOnly`. Every other value is written with `write` (the cell's draft, in the user's locale), all in one runtime batch, then `committed()` once. Fill and clear: editable cells of the rectangle only, one batch, one `committed()`.
  - **One change per row operation.** `insertRow`, `deleteRows`, `undo`, a row appended by the walk and the "Add row" button each change the form once and call `committed()` exactly once, however many rows they touch, so each causes one derive round.
  - **Menu entries.** `cellEntries(row, column)` gains, after "Revert cell" and "Revert row" and a separator: "Insert row above", "Insert row below", and "Delete row" — "Delete rows" when the cell selection spans more than one row, deleting every row it spans. "Insert row …" is disabled at `maxItems` and "Delete …" at `minItems` (or when the deletion would go below it), each with its `MenuItem::description` the reason (the copy above). With `rowsFixed` none of the three is offered. The lowering offers the same three in the table's own menu (Part E's `TableOptions::menu` with no column) for the focused row. With `appendRows`, `node()` draws an "Add row" button under the table (test id `<table testId>_addRow`), disabled at `maxItems` with the same reason as its tooltip; it appends a row and focuses its first editable cell.
  - Document: `selectionMode: "cells"` (the default for a grid of a form; refused on a model's table), `selection`/`onSelectionChange` with `{"anchor": {"key", "column"}, "focus": {"key", "column"}}` or `null`; `appendRows`, `rowsFixed` (literal flags).

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_grid.cpp`:

```cpp
// Mutation: CellRange stored as view positions (the sort moves the corners onto other rows).
TEST_CASE("grid: cell selection", "[ui][grid][cells]") {
    GridFixture f{3};
    for (auto const& [key, text] : {std::pair{1, "3"}, {2, "1"}, {3, "2"}}) {
        f.grid->focusCell(at(key, "mass")); f.grid->type(text); f.grid->press("Enter");
    }
    f.grid->focusCell(at(1, "mass"));
    f.grid->press("Shift+Down"); f.grid->press("Shift+Right");
    CHECK(f.grid->selection() == ui::CellRange{at(1, "mass"), at(2, "volume")});
    REQUIRE(f.source->view().setSort({table::SortKey{.column = "mass"}}).has_value());
    CHECK(f.grid->selection() == ui::CellRange{at(1, "mass"), at(2, "volume")});   // corners kept by key
    CHECK(f.grid->copy() == "1,00\t\n2,00\t\n3,00\t");    // rows 2, 3, 1 now lie between the corners' positions
}
// Mutation: the loader accepting selectionMode "cells" on a model-backed table (loadDocumentText succeeds).
TEST_CASE("grid: cells on a model's table are refused when the document loads", "[ui][grid][cells]") {
    // {"kind":"table","model":"m","table":"specimens","selectionMode":"cells"}: loadDocumentText fails, message names selectionMode
}

// Mutation: a paste that shifts a value past a read-only cell into the next editable one.
TEST_CASE("grid: paste, fill, clear", "[ui][grid][cells]") {
    std::string schema{kSpecimens};
    schema.replace(schema.find(R"("maxItems":5)"), 12, R"("maxItems":2)");
    GridFixture f{1, ui::GridOptions{.appendRows = true}, schema};
    f.enableDerive();
    f.grid->focusCell(at(1, "volume"));                       // columns: volume, density (read-only), limit (read-only), note
    auto const result = f.grid->paste("1\t2\t3\ta\n4\t5\t6\tb\n7\t8\t9\tc");   // 3 rows × 4 columns
    CHECK(result.written == 4);                                // volume and note of two rows
    CHECK(result.readOnly == 4);
    CHECK(result.pastRows == 4);
    CHECK(f.grid->message() == "Pasted 4 values; 4 on read-only cells and 4 past the last row were dropped");
    CHECK(f.form->field("specimens[0].note").draft() == "a");  // the block keeps its shape
    CHECK(f.form->field("specimens[1].volume").draft() == "4");
    CHECK(f.form->field("specimens").rows().size() == 2);     // one row added, to maxItems
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 1);                   // one paste, one round
    f.grid->select(ui::CellRange{at(1, "volume"), at(2, "note")});
    f.grid->fillDown();
    CHECK(f.form->field("specimens[1].volume").draft() == "1");
    CHECK(f.form->field("specimens[1].note").draft() == "a");
    f.grid->clear();
    CHECK(f.form->field("specimens[0].volume").draft().empty());
    CHECK(f.form->field("specimens[1].note").draft().empty());
    CHECK(f.form->field("specimens[0].density").draft().empty());   // read-only: never written
}

// Review Focus 1, paste half. Mutation: an empty grid's paste starting nowhere and appending nothing.
TEST_CASE("grid: paste into an empty grid", "[ui][grid][cells]") {
    SECTION("with appendRows") {
        GridFixture f{0, ui::GridOptions{.appendRows = true}};
        auto const result = f.grid->paste("x\t1\ny\t2\nz\t3");
        CHECK(result.written == 6);
        CHECK(f.form->field("specimens").rows().size() == 3);
        CHECK(f.form->field("specimens[2].label").draft() == "z");
        CHECK(f.form->field("specimens[2].mass").draft() == "3");
    }
    SECTION("without appendRows") {
        GridFixture f{0};
        auto const result = f.grid->paste("x\t1\ny\t2");
        CHECK(result.written == 0);
        CHECK(result.pastRows == 4);
        CHECK(f.form->field("specimens").rows().empty());
        CHECK(f.grid->message() == "Pasted 0 values; 4 past the last row were dropped");
    }
}

// Review Focus 3. Mutation: lines split on "\n" only, keeping "\r" and the trailing empty line.
TEST_CASE("grid: spreadsheet clipboard text pastes as typed values", "[ui][grid][cells]") {
    GridFixture f{2, ui::GridOptions{.appendRows = true}};
    f.grid->focusCell(at(1, "mass"));
    auto const result = f.grid->paste(" 12,5 \t1.234,5\r\n3\t4\r\n");
    CHECK(result.written == 4);
    CHECK(result.pastRows == 0);
    CHECK(f.form->field("specimens").rows().size() == 2);    // no third row from the trailing line break
    CHECK(f.json("specimens[0].mass") == R"({"num":1250,"den":100,"dp":2})");
    CHECK(f.json("specimens[0].volume") == R"({"num":12345,"den":10,"dp":1})");
    CHECK(f.form->field("specimens[1].volume").draft() == "4");
}

// Mutation: insert and delete without checking minItems and maxItems.
TEST_CASE("grid: rows of a grid", "[ui][grid][rows]") {
    std::string schema{kSpecimens};
    schema.replace(schema.find(R"("minItems":0)"), 12, R"("minItems":1)");
    schema.replace(schema.find(R"("maxItems":5)"), 12, R"("maxItems":2)");
    GridFixture f{1, {}, schema};
    f.grid->focusCell(at(1, "label"));
    CHECK_FALSE(f.grid->rowOps().remove);
    CHECK(f.grid->rowOps().why == "A grid holds at least 1 rows");
    f.grid->press("Ctrl+Shift+=");
    CHECK(f.form->field("specimens").rows().size() == 2);
    CHECK_FALSE(f.grid->rowOps().insert);
    CHECK(f.grid->rowOps().why == "A grid holds at most 2 rows");
    f.grid->type("kept"); f.grid->press("Enter");             // (1, label)
    f.grid->focusCell(at(1, "label"));
    f.grid->press("Ctrl+-");
    CHECK(f.form->field("specimens").rows().size() == 1);
    CHECK(f.grid->message() == "Deleted 1 row");
    CHECK(f.grid->messageEntries() == std::vector<ui::StatusEntry>{{"undo", "Undo"}});
    f.grid->chooseEntry("undo");
    CHECK(f.form->field("specimens[0].label").draft() == "kept");
    SECTION("rowsFixed offers none") {
        GridFixture fixed{2, ui::GridOptions{.rowsFixed = true}};
        CHECK_FALSE(fixed.grid->rowOps().insert);
        CHECK_FALSE(fixed.grid->rowOps().remove);
        fixed.grid->focusCell(at(2, "note"));
        CHECK(fixed.grid->press("Tab") == ui::KeyOutcome::FocusNext);
    }
    SECTION("appendRows adds a row on Tab past the end") {
        GridFixture grow{1, ui::GridOptions{.appendRows = true}};
        grow.grid->focusCell(at(1, "note"));
        CHECK(grow.grid->press("Tab") == ui::KeyOutcome::Handled);
        CHECK(grow.form->field("specimens").rows().size() == 2);
        CHECK(grow.grid->focus() == at(2, "label"));
    }
}

// Mutation: the row entries offered with no bound check (Insert enabled at maxItems).
TEST_CASE("grid: row entries in the cell's menu and Add row", "[ui][grid][rows]") {
    std::string schema{kSpecimens};
    schema.replace(schema.find(R"("maxItems":5)"), 12, R"("maxItems":3)");
    GridFixture f{2, ui::GridOptions{.appendRows = true}, schema};
    auto entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    CHECK(labelsOf(entries) == std::vector<std::string>{"Revert cell", "Revert row", "", "Insert row above",
                                                        "Insert row below", "Delete row"});   // "" the separator
    entry(entries, "Insert row above").onSelect();
    CHECK(keysOf(*f.source).size() == 3);
    CHECK(keysOf(*f.source).at(1) == ui::Key{std::int64_t{1}});   // the new row stands above row 1
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    CHECK_FALSE(enabledOf(entry(entries, "Insert row below")));
    CHECK(descriptionOf(entry(entries, "Insert row below")) == "A grid holds at most 3 rows");
    f.grid->select(ui::CellRange{at(1, "label"), at(2, "mass")});
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    entry(entries, "Delete rows").onSelect();
    CHECK(keysOf(*f.source).size() == 1);
    SECTION("rowsFixed offers no row entry") {
        GridFixture fixed{2, ui::GridOptions{.rowsFixed = true}};
        CHECK(labelsOf(fixed.grid->cellEntries(ui::Key{std::int64_t{1}}, "label")) ==
              std::vector<std::string>{"Revert cell", "Revert row"});
    }
}

// Mutation: insertRow calling committed() once per member it creates (several rounds for one insertion).
TEST_CASE("grid: each row operation is one change", "[ui][grid][rows]") {
    GridFixture f{3};
    f.enableDerive();
    f.grid->focusCell(at(1, "label"));
    f.grid->insertRow(true);
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 1);
    f.evaluate.resolveSubmit(0, R"({"errors":{},"ready":false})"); f.derive.resolveSubmit(0, kNoValues); f.h.settle();
    f.grid->focusCell(at(1, "label")); f.grid->type("x"); f.grid->press("Enter");
    f.scheduler.advance(150ms); f.h.settle();
    f.evaluate.resolveSubmit(1, R"({"errors":{},"ready":false})"); f.derive.resolveSubmit(1, kNoValues); f.h.settle();
    f.grid->select(ui::CellRange{at(1, "label"), at(4, "label")});   // view [1, 4, 2, 3]: rows 1 and 4
    f.grid->deleteRows();                                      // two rows, one holding a value
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 3);
    f.evaluate.resolveSubmit(2, R"({"errors":{},"ready":false})"); f.derive.resolveSubmit(2, kNoValues); f.h.settle();
    f.grid->chooseEntry("undo");
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 4);
}
```

`tests/grid_test_support.hpp` gains `descriptionOf(ui::MenuItem const&) -> std::string`.

`tests/test_ui_mount.cpp`, over `RecordingBackend` and a client-mode table node with `cellSelection` bound to a signal and three rows keyed 1, 2, 3, columns `a`, `b`, `c`:

```cpp
// Mutations: the mount sending the rectangle once at its first positions (the sort leaves "r0+2,c0+2" recorded);
// the mount reading a reported position as a `Table::columns` index (with `a` hidden, position 0 reports column a).
TEST_CASE("mount: a cell selection is shown by its corners' positions now", "[ui][mount][cells]") {
    // selection {anchor (1, a), focus (2, b)}: CHECK the table records cellSelection == "r0+2,c0+2"
    // re-sort the rows to 3, 1, 2: CHECK cellSelection == "r1+2,c0+2"
    // h.backend.selectCells(table, 0, 1, 2, 2): onCellSelect receives {anchor (3, b), focus (2, c)}
    // hide column a (index 0): CHECK cellSelection == "" (a corner's column is hidden; the range is kept);
    //   h.backend.selectCells(table, 0, 0, 0, 1) now reports {anchor (3, b), focus (3, c)}: positions, not indices
    // the bound selection set to null: CHECK cellSelection == ""
}
```

Qt Quick: `"qt_quick grid: a drag selects a rectangle of cells and a row header click its row"` — a mouse press on (1, mass), a move to (2, volume) and a release report `onCellSelect(0, 1, 1, 2)`, and the QML draws the rectangle `setCellSelection` gives, not a row selection. Mutation: the QML table reporting the drag as a row selection.

- [ ] **Step 2: Run to see them fail** — `cmake --build --preset cl-debug --target morph_tests`; expected: `GridController::paste` not declared.

- [ ] **Step 3: Implement** `minItems`/`maxItems`, `removeRows`/`restoreRows`, then the selection, copy, paste, fill, clear and row operations in `grid.hpp`; then the lowering, the loader's refusal, and the backends' row header and column header selection gestures.

- [ ] **Step 4: Run** `ctest --test-dir build/cl-debug -R "grid:" --output-on-failure` — all pass.

- [ ] **Step 5: Mutate**, **Step 6: Docs**, **Step 7: Commit** `feat(ui): cell selection, paste and row operations in grids`.

### Task 22: Bands, spans and summary rows (editing.md step 4)

**Spec:** editing.md "Header bands", "Spans", "Summary rows", Invariant 8; Failure modes rows on bands, spans and `summaries`; table.md "Column groups" (a group draws as a one-level band).

**Files:**
- Modify: `include/morph/table/description.hpp`, `include/morph/ui/view.hpp`, `include/morph/ui/mount.hpp`, `include/morph/ui/backend.hpp`, `include/morph/ui/grid.hpp`, `include/morph/ui/form_rows.hpp` (`setGroup`, `runRecords`), `include/morph/ui/testing/recording_backend.hpp`, `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `include/morph/ui/interpreter.hpp`, `src/qt_quick/table.cpp` and QML, `src/tui/table_widget.cpp`
- Tests: `tests/test_ui_mount.cpp`, `tests/test_ui_grid.cpp`, `tests/table/test_table_description.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_table.cpp`
- Docs: `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md` (`bands`, a span part's `columns`, `summaries` on `table` and `group`), `docs/spec/ui/view_tree.md` (spans across columns, summary rows), `docs/spec/qt_quick/backend.md` (bands and spans).

**Interfaces:**
- Consumes: `table::ColumnLayout` (Task 8d), `ui::TableColumn::id` and `Table::arrangement` (Task 9; the mount maps the arrangement's visible indices to ids for `bandRuns`), `ui::formatCell` (Task 15), Task 19's `GridSource::covered`, `GridRows`, `TableRows`.
- Produces:
  - `description.hpp`: `struct BandInfo { std::string label; std::string unit; std::vector<std::string> columns; std::vector<BandInfo> bands; bool operator==(BandInfo const&) const = default; };` `std::vector<BandInfo> TableDescription::bands;` `[[nodiscard]] std::expected<void, TableError> checkBands(std::span<BandInfo const> bands, std::span<TableColumn const> columns);` — `SourceContract` for a nesting past 3 levels, a band naming no column, a child over a column its parent lacks, a column in two bands of one level, or a column id the table does not have; the message names the band's label.
  - `view.hpp`: `struct BandRun { std::size_t level = 0; std::string label; std::string unit; std::size_t first = 0; std::size_t count = 0; bool pinned = false; bool operator==(BandRun const&) const = default; };` (`first` and `count` are visible positions, not `Table::columns` indices) `std::vector<table::BandInfo> Table::bands;` `std::size_t RowSpan::columns = 1;` (covered columns to the right, by visible position, the cell's own included) `bool TableRowLook::summary = false;` (index #39: the flag is the row look's, beside Part D's `heading` and Task 17b's `marks`; `TableRowView`, the C++ `table<Row>()` path's, gains nothing). The mount calls `TableWidget::setRowSummary(row)` once, right after `setRowKey`, for a row whose `rowView` returned `summary == true`, as it calls `setRowHeading`; such a row builds its cells with `cellView` as any row does.
  - `mount.hpp`: `[[nodiscard]] std::vector<BandRun> bandRuns(std::span<table::BandInfo const> bands, std::span<std::string const> visible, std::size_t pinned);` — `visible` the window's visible column ids in layout order; one run per maximal run of a band's columns standing together, split at the pinned edge; levels from 0 (outermost); a band with no visible column yields none. `[[nodiscard]] bool columnUnitShown(std::span<table::BandInfo const> bands, std::string_view columnId, std::string_view unit);` (false when an enclosing band's unit equals `unit`). `[[nodiscard]] std::string bandedHeaderName(std::span<table::BandInfo const> bands, std::string_view columnId, std::string_view label);` (outermost band first, joined by `", "`). `struct CellSpan { std::string column; std::size_t rows = 1; std::size_t columns = 1; };` `[[nodiscard]] std::size_t cutColumns(CellSpan const& span, std::span<std::string const> declared, std::span<std::string const> visible);` — the span covers its column, then each next visible column while it is the next declared one, up to `columns`.
  - `backend.hpp`: `virtual void TableWidget::setBands(std::vector<BandRun> const& runs) {}`; `virtual void TableWidget::setRowSummary(Widget& row) {}` (a summary row: drawn on the theme's `cellSummary` surface, never selected, activated, focused for an edit or pasted into; copied when a copy covers it; called once, right after `setRowKey`). `setRowSpans` carries `RowSpan::columns`. `recording_backend.hpp` records `setRowSummary` as property `summary` (`"true"`) on the row, and a span as `"<column index>:<rows>"` while `columns == 1` — the format `docs/spec/ui/backend_contract.md` gives and `test_ui_table_row_span.cpp` asserts, unchanged — and as `"<column index>:<rows>x<columns>"` otherwise (`0:2x3`).
  - `grid.hpp`: `[[nodiscard]] ValueList summaryScopeRows(TableRows const& rows, std::int64_t first, std::int64_t count);` — the records of `count` view rows of `rows` from `first`, in view order (a grid row's record holds each member's encoded value, `null` when blank or not encoding); `rows` is always the **data** rows (`GridSource::view()`, or a `rows` table's `EngineRows`), never the decorator below.
  - **How a summary row reaches the mount.** `grid.hpp`: `struct SummarySpec { std::string key; bool top = false; };` and `class SummaryRows final : public TableRows` — `SummaryRows(std::shared_ptr<TableRows> data, std::vector<SummarySpec> table, std::vector<SummarySpec> perRun, std::function<std::vector<std::pair<std::int64_t, std::int64_t>>()> runs)` — a forwarding decorator (index #40): it is what the mount is handed in place of `data`, it sets `data`'s one `onChange` slot itself and calls the mount's handler with the change translated to its own indices, and it forwards `visible`, `keysOf`, `setSort`, `setFilter`, `counts`, `states`, `moveKey` and `unloadedLevels` (indices translated). Its view is: the table's `top` summaries, then the data rows in view order with each run's `perRun` summaries after the run's last row (`runs()` gives each run's `{first, count}` in `data`'s view; empty for an ungrouped table), then the table's bottom summaries. A summary row's `row(index)` is `TableRowRef{key, record}` with the key below and the record `{"#summary": <spec key>, "#first": <first data row>, "#count": <data rows in scope>}`; `total().rows` and `counts().view` stay `data`'s (a summary is outside the count), and `keysOf` skips summary rows (outside the selection). `[[nodiscard]] std::optional<std::int64_t> mounted(std::int64_t dataRow) const` and `[[nodiscard]] std::optional<std::int64_t> data(std::int64_t mountedRow) const` map between the two index spaces. With a `SummaryRows` in place, the lowering passes `TableSource{.rows = the decorator, …}` as `GridController::node`'s `base`; `GridSource::rowCount`, `keyAt`, `positionOf`, the walk, paste and the cell rectangle stay over the data rows, the mount maps a `GridFocus::viewRow` and a rectangle's rows through `mounted()` before `setFocusCell` and `setCellSelection`, and a click, a `FocusMove` or a reported rectangle that lands on a summary row is mapped to the nearest data row by `data()`. `rowView` returns `TableRowLook{.summary = true}` for a record holding `#summary`; `cellView(c, row)` for such a record evaluates the spec's `cells[columns[c].id]` in a scope whose `rows` is `summaryScopeRows(data, #first, #count)` and `column` the column's id, shown by the column's display (`formatCell`), and an empty text cell for a column the spec does not name. A table with no `summaries` gets no decorator.
  - **`group` on a grid of a form.** `form_rows.hpp`: `void FormRowsSource::setGroup(std::function<Value(forms::RowState const&)> by)` — as Task 11a's `ValueRowsSource::setGroup`: the source gains the column `kGroupRunColumn` (`ColumnKind::Integer`, not laid out), a row's cell its run index, runs counted over the array's order, a new run where `by` differs from the previous row's; `[[nodiscard]] std::span<Value const> runRecords() const` gives the heading records. The lowering of a form grid with `group` puts `{kGroupRunColumn, Ascending}` before every sort chain and places the headings as Task 11b's lowering does for a `rows` table, through the same code; a heading row has no key in `GridSource` (it is not a data row), so `SummaryRows::runs` and the heading placement share one index translation. `void FormGridSource::setSpans(std::function<std::vector<CellSpan>(forms::RowState const&)> spansOf);` — `covered(key, column)` is true for a cell another row's span covers (rows below, columns right by `cutColumns`), while the grid is unsorted and under its windowing thresholds.
  - Document: `bands` on `table` (parts `{label, unit, columns, bands}`, refused as `checkBands` refuses); a `rowSpans` part's `columns`; `summaries` on `table` and on a `group` part, parts `{key, label, position, cells}` (`position` `bottom` or `top`; `cells` maps column ids to expressions over `rows` and `column`), refused on a model-backed table when the document loads. A summary row's key is `Key{std::string}`: `"\x1Fsummary:"` + its key (a group's: `"\x1Fsummary:"` + the run's ordinal + `"/"` + its key). A summary cell's test id is `summary_<key>_<column>` (a group's `summary_<run ordinal>_<key>_<column>`). A user-formed or declared column group draws as a band of one level.

- [ ] **Step 1: Write the failing tests.** `tests/test_ui_mount.cpp`:

```cpp
namespace {
std::vector<table::BandInfo> const kMassBand{{.label = "Mass retained", .unit = "g", .columns = {"m8", "m4", "m2"}}};
std::vector<std::string> const kIds{"name", "m8", "m4", "m2", "total"};
}

// Mutation: a band drawn from its first to its last column's place, which covers a foreign column.
TEST_CASE("bands: a band follows the layout", "[ui][mount][bands]") {
    CHECK(ui::bandRuns(kMassBand, kIds, 0) == std::vector<ui::BandRun>{{0, "Mass retained", "g", 1, 3, false}});
    std::vector<std::string> const moved{"name", "m8", "m2", "total", "m4"};
    CHECK(ui::bandRuns(kMassBand, moved, 0) ==
          std::vector<ui::BandRun>{{0, "Mass retained", "g", 1, 2, false}, {0, "Mass retained", "g", 4, 1, false}});
    CHECK(ui::bandRuns(kMassBand, kIds, 2) ==
          std::vector<ui::BandRun>{{0, "Mass retained", "g", 1, 1, true}, {0, "Mass retained", "g", 2, 2, false}});
    CHECK(ui::bandRuns(kMassBand, std::vector<std::string>{"name", "total"}, 0).empty());
    CHECK_FALSE(ui::columnUnitShown(kMassBand, "m8", "g"));
    CHECK(ui::columnUnitShown(kMassBand, "total", "g"));
    CHECK(ui::bandedHeaderName(kMassBand, "m4", "sieve 4 mm") == "Mass retained, sieve 4 mm");
}

// Review Focus 2. Mutation: bandRuns computed once from the declared order and not from the layout's visible columns.
TEST_CASE("bands: a stored layout hides then shows a band's column", "[ui][mount][bands]") {
    table::ColumnLayout layout{kIds};
    table::ColumnLayout stored{kIds};
    for (auto const* id : {"m8", "m4", "m2"}) stored.setVisible(id, false);
    layout.apply(stored.value());
    CHECK(ui::bandRuns(kMassBand, layout.visible(), layout.pinned()).empty());
    layout.setVisible("m4", true);
    CHECK(ui::bandRuns(kMassBand, layout.visible(), layout.pinned()) ==
          std::vector<ui::BandRun>{{0, "Mass retained", "g", 1, 1, false}});
    table::ColumnLayout pinnedAll{kIds};
    pinnedAll.setPinned(4);
    CHECK(ui::bandRuns(kMassBand, pinnedAll.visible(), pinnedAll.pinned()) ==
          std::vector<ui::BandRun>{{0, "Mass retained", "g", 1, 3, true}});
}

// Mutation: covered cells left focusable (the walk lands on one).
TEST_CASE("spans: a cell spanning rows and columns covers them", "[ui][mount][spans]") {
    std::vector<std::string> const declared{"a", "b", "c", "d"};
    CHECK(ui::cutColumns({.column = "a", .rows = 2, .columns = 3}, declared, declared) == 3);
    CHECK(ui::cutColumns({.column = "a", .rows = 2, .columns = 3}, declared, std::vector<std::string>{"a", "c", "b", "d"}) == 1);
    CHECK(ui::cutColumns({.column = "a", .rows = 2, .columns = 3}, declared, std::vector<std::string>{"a", "b", "d"}) == 2);
}

// Mutation: the mount dropping RowSpan::columns before setRowSpans (the row records "0:2").
TEST_CASE("spans: a span across columns reaches the backend", "[ui][mount][spans]") {
    // a TableSource of three rows over columns a, b, c, d whose rowView gives row 1
    // spans = {RowSpan{.column = 0, .rows = 2, .columns = 3}} and row 3 spans = {RowSpan{.column = 1, .rows = 1}}
    CHECK(h.backend.prop(rowOf(h, 1), "rowSpans") == "0:2x3");
    CHECK(h.backend.prop(rowOf(h, 3), "rowSpans") == "1:1");      // columns 1 keeps the existing format
}

// Mutations: setRowSummary called for every row (row 1 records summary); summary rows counted in the realized
// window's total (total 4); the flag read from TableRowView (no row records summary).
TEST_CASE("summary: a summary row is drawn as one, outside the rows", "[ui][mount][summary]") {
    // three data rows keyed 1..3 in an EngineRows `data`; SummaryRows{data, {{"stats"}, {"head", true}}, {}, none}
    // mounted as TableSource{.rows = decorator}, rowView giving .summary for a record holding #summary
    // CHECK the mounted keys in order: "\x1Fsummary:head", 1, 2, 3, "\x1Fsummary:stats"
    // CHECK the two summary row widgets record summary == "true", set right after rowKey (the log's next line),
    //   and rows 1..3 record none
    // CHECK decorator.total().rows == 3, decorator.mounted(0) == 1, decorator.data(4) == 2 (the nearest data row)
    // CHECK keysOf(0, 5) completes with {1, 2, 3}
    // data re-sorted to 3, 1, 2: the mount's one onChange handler ran (keys now head, 3, 1, 2, stats)
}
```

`tests/table/test_table_description.cpp`:

```cpp
// Mutation: checkBands not walking child bands.
TEST_CASE("description: bands are checked", "[table][description][bands]") {
    // columns m8, m4, m2, total
    CHECK(table::checkBands(kMassBand, columns).has_value());
    // four levels deep; a band over no column; a child over "total" which its parent lacks; "m8" in two bands of one level
    // each: CHECK(error().code == table::TableErrorCode::SourceContract) and the message contains the band's label
}
```

`tests/test_ui_grid.cpp`:

```cpp
// Mutation: a paste writing into a covered cell (the spanning cell's value changes twice, or the covered field is written).
TEST_CASE("grid: spans pass the walk and drop pasted values", "[ui][grid][spans]") {
    GridFixture f{3};
    f.source->setSpans([](forms::RowState const& row) {
        return row.key() == 1 ? std::vector<ui::CellSpan>{{.column = "mass", .rows = 2, .columns = 2}} : std::vector<ui::CellSpan>{};
    });
    CHECK(f.source->covered(ui::Key{std::int64_t{2}}, "mass"));
    CHECK(f.source->covered(ui::Key{std::int64_t{1}}, "volume"));
    CHECK(f.source->covered(ui::Key{std::int64_t{2}}, "volume"));
    f.grid->focusCell(at(1, "mass"));
    f.grid->press("Tab");
    CHECK(f.grid->focus() == at(1, "note"));            // volume covered, density and limit read-only
    f.grid->focusCell(at(1, "mass")); f.grid->type("1"); f.grid->press("Enter");
    CHECK(f.grid->focus() == at(3, "mass"));            // (2, mass) is covered
    f.grid->focusCell(at(1, "mass"));
    auto const result = f.grid->paste("1\n2\n3");
    CHECK(result.written == 2);
    CHECK(result.readOnly == 1);
    CHECK(f.form->field("specimens[1].mass").draft().empty());
    table::ColumnLayout layout{{"label", "mass", "volume", "density", "limit", "note"}};
    // moving volume away cuts the span: with volume after note, (1, volume) is no longer covered
    layout.move("volume", 5);
    f.source->setLayout(layout.visible());              // the grid's visible columns follow the layout
    CHECK_FALSE(f.source->covered(ui::Key{std::int64_t{1}}, "volume"));
}

// Mutation: the grid's walk and paste running over the decorator's rows (Tab lands on the summary row; a paste
// counts it as a read-only row instead of running past the last row).
TEST_CASE("summary: a grid's walk, paste and scope do not see summary rows", "[ui][grid][summary]") {
    GridFixture f{2};
    auto const data = std::shared_ptr<ui::TableRows>{std::shared_ptr<void>{}, &f.source->view()};
    ui::SummaryRows rows{data, {{.key = "stats"}}, {}, [] { return std::vector<std::pair<std::int64_t, std::int64_t>>{}; }};
    CHECK(f.source->rowCount() == 2);
    f.grid->focusCell(at(2, "note"));
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);     // nothing below row 2 for the grid
    f.grid->focusCell(at(2, "label"));
    auto const result = f.grid->paste("a\nb");
    CHECK(result.written == 1);
    CHECK(result.pastRows == 1);
    CHECK(result.readOnly == 0);
    CHECK(ui::summaryScopeRows(f.source->view(), 0, 2).size() == 2);
    CHECK(rows.mounted(1) == std::optional<std::int64_t>{1});
    CHECK(rows.row(2)->key == ui::Key{std::string{"\x1Fsummary:stats"}});
}

// Mutation: summaryScopeRows reading the source's snapshot, which ignores the filter.
TEST_CASE("summary: rows follow the filter", "[ui][grid][summary]") {
    GridFixture f{3};
    for (auto const& [key, text] : {std::pair{1, "10"}, {2, "12"}, {3, "14"}}) {
        f.grid->focusCell(at(key, "mass")); f.grid->type(text); f.grid->press("Enter");
    }
    REQUIRE(f.source->view().setFilter(table::FilterSpec{.columns = {{"mass",
              table::ColumnFilter{.include = {table::FilterEntry{.gt = "11"}}}}}}).has_value());
    ValueList const rows = ui::summaryScopeRows(f.source->view(), 0, f.source->view().total().rows);
    REQUIRE(rows.size() == 2);
    CHECK(canonicalJson(*rows.at(0).field("mass")) == R"({"num":1200,"den":100,"dp":2})");
}
```

Interpreter cases (`TableHarness`, which formats in `en`; a whole document with a `models` block: the `Lims_RecordSpecimens` form and a grid with `"summaries":[{"key":"stats","cells":{"mass":{"fn":"mean","args":[{"map":{"ref":"rows"},"as":"r","to":{"ref":"r.mass"}}]},"label":{"fn":"count","args":[{"ref":"rows"}]}}},{"key":"sd","cells":{"mass":{"fn":"stdev","args":[{"map":{"ref":"rows"},"as":"r","to":{"ref":"r.mass"}},2]}}}]`):

```cpp
// Mutation: summaries evaluated once at lowering (they do not follow a committed value).
TEST_CASE("summary: mean and stdev follow every committed value", "[ui][grid][summary][interpreter]") {
    // masses 10.00 / 12.00 / 14.00 typed through the grid
    CHECK(h.text("summary_stats_mass") == "12.00");
    CHECK(h.text("summary_sd_mass") == "2.00");
    CHECK(h.text("summary_stats_label") == "3");
    // type 16.00 into row 3
    CHECK(h.text("summary_stats_mass") == "12.67");   // 38/3 at the column's 2 places
}
// Review Focus 1, summary half. Mutation: count over the source rows or mean of [] shown as "0".
TEST_CASE("summary: over no rows", "[ui][grid][summary][interpreter]") {
    // the same document with no specimens
    CHECK(h.text("summary_stats_label") == "0");
    CHECK(h.text("summary_stats_mass").empty());
}
// Mutations: a group's summary evaluated over every row of the table (both runs show 12.00); FormRowsSource with
// no run column (the grid draws no heading and one run).
TEST_CASE("summary: group summaries are per run", "[ui][grid][summary][interpreter]") {
    // the grid with "group": {"by": {"ref": "row.label"}, "heading": …, "summaries": [{key: "m", cells: {mass: mean}}]};
    // labels A, A, B; masses 10, 12, 14
    CHECK(h.text("summary_0_m_mass") == "11.00");
    CHECK(h.text("summary_1_m_mass") == "14.00");
    // rowKeys(h, "grid") in order: heading A, 1, 2, "\x1Fsummary:0/m", heading B, 3, "\x1Fsummary:1/m"
    // sorting the grid by mass descending keeps the runs: A's rows are 2, 1 and its summary still 11.00
    // Enter on (2, mass) with enterMoves down lands on (3, mass): the heading and the summary are passed over
}
// Mutation: the loader checking `summaries` on the table node and not on its group part (the second loads).
TEST_CASE("summary: a model-backed table refuses summaries", "[ui][summary][interpreter]") {
    // {"kind":"table","model":"m","table":"specimens","summaries":[…]} and a group part with summaries: both refused
}
```

Qt Quick: `"qt_quick table: a band draws over each run of its columns and across the pinned edge"` — after `setBands({{0,"Mass retained","g",1,2,false},{0,"Mass retained","g",4,1,false}})` the header holds two band items whose x and width equal the covered columns' extents. Mutation: the QML band item placed from its first run's x to its last run's end (one item, as wide as columns 1–4). TUI: `"tui table: bands draw above the header"` — the rendered header's first line holds `Mass retained [g]` over columns 1–3. Mutation: the terminal table drawing the band over the declared columns' widths instead of the runs `setBands` gave (the label starts over column 0).

- [ ] **Step 2: Run to see them fail** — `cmake --build --preset cl-debug --target morph_tests`; expected: `ui::bandRuns` not declared.

- [ ] **Step 3: Implement** `BandInfo`, `checkBands`, `bandRuns`, `columnUnitShown`, `bandedHeaderName`, `cutColumns`; the mount's bands (over the column window's columns), its column spans, `TableRowLook::summary` → `setRowSummary`; `SummaryRows`, `summaryScopeRows`, `FormRowsSource::setGroup`; the lowering (the decorator handed to the mount, the index mapping for focus and the cell rectangle) and loader; then the Qt Quick and terminal drawing.

- [ ] **Step 4: Run** `ctest --test-dir build/cl-debug -R "bands|spans|summary|description" --output-on-failure` (and the Qt Quick and TUI patterns `qt_quick table` and `tui table`) — all pass; the existing row-span cases in `test_ui_table_row_span.cpp` still pass with `columns` 1.

- [ ] **Step 5: Mutate**, **Step 6: Docs**, **Step 7: Commit** `feat(ui): header bands, spans and summary rows`.

### Task 23: Fixed grids (editing.md step 5)

**Spec:** editing.md "Fixed grids" (with blocks), Failure modes rows "Two `layoutRows` parts with one key" and "`form` and `rows` both on a table, `layoutRows` with `rows` or `field`, or a `{field}` cell in a fixed grid without `form`".

**Files:** `include/morph/ui/grid.hpp`, `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `include/morph/ui/interpreter.hpp`, `tests/ui_interpreter_support.hpp` (schema `Lims_SetLimits`), `tests/test_ui_grid.cpp`; docs `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md` (`layoutRows`, cell parts, blocks).

**Interfaces:**
- Consumes: Task 19–21 `GridSource`, `GridController`, `cellReadOnly`; Task 22 `CellSpan`, `cutColumns`.
- Produces: `grid.hpp`: `struct FixedCell { std::string field; std::function<std::string()> value; std::string text; std::size_t rows = 1; std::size_t columns = 1; std::function<bool()> readonly; std::function<std::optional<std::uint32_t>()> decimals; };` `struct FixedRow { std::string key; std::string label; std::map<std::string, FixedCell, std::less<>> cells; };` `struct FixedBlock { std::string forEach; std::vector<FixedRow> rows; };` `class FixedGridSource final : public GridSource` — `FixedGridSource(forms::FormSession* form, std::vector<std::variant<FixedRow, FixedBlock>> rows, std::vector<GridColumn> columns, FormatContext format)` (`form` null for a fixed grid of `{text}` and `{value}` cells only, which the loader allows without `form`; a `{field}` cell with `form` null is the loader's refusal, so the source never meets one); every row is held, so `keyAt` is never empty below `rowCount`; row keys are `Key{std::string}`: a fixed row's `key`, a block row's `"<RowState::key()>/<key>"`; a block's `{field}` path is relative to the specimen's row (`specimens#<key>.<field>`); a cell with `{text}` or `{value}` is read-only. Not sorted, filtered or rearranged by rows; columns may be hidden and resized. Its `view()` is an `EngineRows` over an internal `table::DataSource` the source owns (one `Text` column `#key`, one row per expanded row, in order, `Reset` on each change of a block's array rows), built with `table::EngineOptions{}` and never sorted or filtered, its `recordOf` giving `RowRecord{RowId{key}, {"#key": key}}`, which `keyOf` reads; `states()` and `moveKey` are `EngineRows`'; `editor(key, column)` is `scalarControl(*form, field)` for a `{field}` cell; `rowModified(key)` is true while a `{field}` cell of the row is `FormSession::modified`.

- [ ] **Step 1: Write the failing tests** — the fixture, in `tests/grid_test_support.hpp`:

```cpp
// A fixed grid over the Lims_SetLimits form: columns `min`, `max` (and `v` for the block case), in a comma locale.
struct FixedGridFixture {
    morph::test::formsengine::Harness h;
    std::unique_ptr<forms::FormSession> form;
    std::unique_ptr<ui::FixedGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // Prefills `body`, builds the source over `rows` and `columns` and the controller with default options.
    FixedGridFixture(std::string_view body, std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> rows,
                     std::vector<ui::GridColumn> columns);
    [[nodiscard]] std::string json(std::string_view path) const;
};
```

Each case below declares `FixedGridFixture f{…}` with the rows its comment gives. `Lims_SetLimits` schema: `limits` object with `density {min, max}` (decimals, 3 places), `moisture {min, max}` (decimals, 1 place), `method` (enum `["A","B"]`), `count` (integer); and `specimens` (array of `{mass: decimal 2}`).

```cpp
// Mutation: the walk asking the field's spec of the column's first row for every row (one kind per column).
TEST_CASE("fixed grid: four parameters walk, paste and validate per cell", "[ui][grid][fixed]") {
    // rows: density {min: limits.density.min, max: limits.density.max}; moisture {min, max};
    //       method {min: {field: limits.method, columns: 2}}; count {min: limits.count, max: {text: "—"}}
    f.grid->focusCell(ui::CellRef{ui::Key{std::string{"density"}}, "min"});
    std::vector<std::pair<std::string, std::string>> walked;
    for (int i = 0; i < 5; ++i) { f.grid->press("Tab"); walked.emplace_back(std::get<std::string>(f.grid->focus()->key), f.grid->focus()->column); }
    CHECK(walked == std::vector<std::pair<std::string, std::string>>{
        {"density", "max"}, {"moisture", "min"}, {"moisture", "max"}, {"method", "min"}, {"count", "min"}});
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);          // count.max is text: read-only
    f.grid->focusCell(ui::CellRef{ui::Key{std::string{"density"}}, "min"});
    auto const result = f.grid->paste("1,2345\t2\n0,55\tx");
    CHECK(result.written == 4);
    CHECK(f.form->field("limits.density.min").error().peek()->code == "too-precise");   // 3 places
    CHECK(f.json("limits.density.max") == R"({"num":2000,"den":1000,"dp":3})");
    CHECK(f.form->field("limits.moisture.min").error().peek()->code == "too-precise");  // 1 place
    CHECK(f.form->field("limits.moisture.max").error().peek()->code == "malformed");
}

// Mutation: block row keys by position, which move every row's state on insertion.
TEST_CASE("fixed grid: a forEach block repeats per specimen with stable keys", "[ui][grid][fixed]") {
    // rows: [{forEach: "specimens", rows: [{key: "mass", cells: {v: {field: "mass"}}}]}], two specimens (keys 1, 2)
    CHECK(keysOf(*f.source) == std::vector<ui::Key>{ui::Key{std::string{"1/mass"}}, ui::Key{std::string{"2/mass"}}});
    f.grid->focusCell(ui::CellRef{ui::Key{std::string{"2/mass"}}, "v"}); f.grid->type("5"); f.grid->press("Enter");
    std::uint64_t const added = f.form->field("specimens").insertRow(0);
    CHECK(keysOf(*f.source).front() == ui::Key{std::to_string(added) + "/mass"});
    CHECK(f.source->draft(ui::Key{std::string{"2/mass"}}, "v") == "5");
}

// Mutation: the loader's duplicate-key check comparing keys within one block only (two top-level "density" load).
TEST_CASE("fixed grid: the loader refuses what the spec refuses", "[ui][grid][fixed]") {
    // each refused by loadDocumentText: two layoutRows parts with key "density"; layoutRows with rows;
    // layoutRows with field; a {field} cell without form; form with rows
}
```

- [ ] **Step 2: Run to see them fail**; **Step 3: Implement** `FixedGridSource` and the lowering of `layoutRows` (expanding blocks on each change of the block's array rows); **Step 4: Run** `ctest --test-dir build/cl-debug -R "fixed grid" --output-on-failure` — pass; **Step 5: Mutate**; **Step 6: Docs**; **Step 7: Commit** `feat(ui): fixed grids`.

### Task 24: Cells of a model's table (editing.md step 6)

**Spec:** editing.md "Cells of a model's table", "Read-only and computed" (the model-table half), "Walking" (moving by view position), Invariant 6; Failure modes rows on edit kinds, `editMode`, refused edits, `FieldError`/`FailedRow`; table.md "Rows an action changed" (a key that changes), "Rows at work, rows that failed". Needs Tasks 9 and 17.


**Carried from Task 19's review:** `GridController::place()` resolves a placeholder's key (`keyAt(viewRow)`) untracked, so the `gridFocus` binding does not run again when the row arrives. It is dormant for a grid of a form, whose rows are all fetched, and is fixed here: `place()` reads the source's row arrival tracked (the view's change, or `keyAt` through a tracked read), with a case where the walk stops at an unfetched row, the row then arrives, and `focus()` names the cell with no further gesture (mutation: the read untracked again).

**Files:**
- Modify: `include/morph/table/edits.hpp`, `include/morph/table/description.hpp`, `include/morph/forms/forms.hpp` (`annotateSchemaRoot<A>`), `include/morph/ui/interpreter.hpp` and `src/ui/interpreter.cpp` (`TableInputKind::CellEdit` in `tableInputsOf`, read at the schema root, so a `CellEdit`/`RowEdit` run the session's mutations make marks its row busy through `RowStates`), `include/morph/ui/grid.hpp`, `include/morph/ui/table_rows.hpp` (`WindowRows::onRowsAbsent`), `tests/grid_test_support.hpp` (`FakeRows`, `ModelGridFixture`), `tests/ui_table_support.hpp` (the two edit actions in `TableHarness`'s catalog), `src/ui/document.cpp`, `src/ui/interpreter.cpp`, `include/morph/ui/interpreter.hpp`, `tests/ui_interpreter_support.hpp` (schemas `Lims_EditSpecimenCount`, `Lims_EditSpecimenRow`)
- Tests: `tests/table/test_table_pending.cpp` (the `CellEdits` cases, beside its existing ones), `tests/table/test_table_description.cpp`, `tests/test_ui_table_model.cpp`, `tests/net/test_push_tables.cpp`
- Docs: `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md` (`editMode`, an editable model column's lowering), `docs/spec/table/engine.md` (`CellEdits` row commits, drafts by key).

**Interfaces:**
- Consumes: `TableRows`, `WindowRows::setDescription`, `onStaleDescription` (Task 9); `TableRows::states()` (`isBusy`, `failure`), `TableRows::moveKey`, `lockText`, `refusesEdit`, `FailedRow`, `ChangedRows::failed` (Task 17c); `DeclaresSchemaValue`, `model::recordRegistry()`, `TableInputKind`, `tableInputsOf` (Task 17a); `ScreenCore::tableMessage` (Task 9d); `Session::can(action)` (existing); `forms::FormModel::fromSchema`, `forms::FormSession` (existing); Task 19's `GridController`, `GridSource`, `cellReadOnly`; `ModelChannel::execute(actionId, body, CallOptions)` (existing, `interpreter.hpp:119`).
- Produces:
  - `edits.hpp`: `template <auto Function, typename T> struct CellEdit { RowId key; T value; };` and `template <auto Function, typename Values> struct RowEdit { RowId key; Values values; };`, each with a `static std::pair<std::string_view, std::string> schemaValue()` (Task 17a's `DeclaresSchemaValue`) returning `{"x-cellEditOf", name}` / `{"x-rowEditOf", name}`, `name` the `RecordInfo::name` in `model::recordRegistry()` whose `type` is `typeid(TableOf<Function>)`, and an in-class glaze meta, `struct glaze { static constexpr auto value = glz::object("key", &CellEdit::key, "value", &CellEdit::value); };` (`RowEdit`: `glz::object("key", &RowEdit::key, "values", &RowEdit::values)`). The meta names the class by its injected name and declares no alias: `CellEdit`'s second template parameter is `T`, and a nested `using T = …` would redeclare it, which is ill-formed. An application's action derives from one (`struct EditSpecimenCount : table::CellEdit<&SpecimensModel::specimens, std::int64_t> {};`) so each action is its own type, and the derived type finds the meta through its base. Glaze's pure reflection cannot read such a type: it counts members by aggregate initialisation, and `D{any}` initialises the whole base from one placeholder, so it counts 1 member and then binds 1 name to a type with 2 (`glaze/reflection/to_tuple.hpp`, `count_members`, at glaze 7.4, `MORPH_GLAZE_VERSION`). This is inferred from reading glaze's source; no compiler run checked it here, so Step 1's first test is the check (if pure reflection does handle the base, the test still passes and the meta is harmless).
  - `forms.hpp`: `template <typename A> void annotateSchemaRoot(glz::generic_u64& dom)` — when `DeclaresSchemaValue<A>`, `dom[first] = second` at the schema **root**; called in `mergeSchemaExtras<A>` beside `annotateSubmitMode<A>(dom)` (`forms.hpp:3332`). Task 17a's `annotateSchemaMarker` writes a member's keyword on its property; an edit action is the marked type itself, so its keyword has no property to sit on.
  - `TableInputKind` gains `CellEdit`; `tableInputsOf` also reads the root: `x-cellEditOf` or `x-rowEditOf` there yields `TableInput{.member = "key", .record = name, .kind = TableInputKind::CellEdit}`, the busy key being the body's `key`. `description.hpp`: `struct EditInfo { std::string action; std::string member; std::string readonly; bool operator==(EditInfo const&) const = default; };`, declared before `TableColumn`, whose `edit` member holds it — not in `edits.hpp`, which must include `description.hpp` (for `TableOf<Function>`, `TableDescription` and `recordRegistry`), so the other way round would be a cycle; `edits.hpp` re-exports it by that include, and this task corrects the header named in editing.md's "API surface" row. `edits.hpp` (which also includes `forms/engine/field_model.hpp`, for `forms::FormModel::fromSchema` and `forms::FieldKind`; `field_model.hpp` includes nothing of `table/` or `ui/`): `[[nodiscard]] std::expected<void, TableError> checkEdits(TableDescription const& description, std::function<std::optional<std::string>(std::string_view action)> const& schemaOf);` — `SourceContract` when a column's kind does not fit its field's kind, or `member` is not a member of `values`; the fits are `Integer` ↔ {Integer, Slider, Enum, Choice}, `Key` ↔ {Integer, Choice}, `Decimal` ↔ {Decimal}, `Quantity` ↔ {Quantity}, `Measure` ↔ {Quantity}, `Number` ↔ {Number, Slider}, `Text` ↔ {Text, Multiline, Enum, Choice}, `Date` ↔ {Date, EpochDate}, `DateTime` ↔ {DateTime, EpochDateTime}, `Bool` ↔ {Boolean}, `Custom` ↔ {Text, Enum, Choice}. Task 2's describe path calls it with the dispatcher's action schemas.
  - `edits.hpp`, `CellEdits` additions: `struct EditCell { std::string column; std::string json; std::string member; };` (`member`: the name the action's `FieldError` uses for this cell — `"value"` for a `CellEdit`, the `EditInfo::member` for a `RowEdit`; empty means the column id) `struct RowEditRequest { RowId row; std::vector<EditCell> cells; };` `using RowEditDone = std::function<void(std::expected<void, std::vector<FieldError>>)>;` `using RowEditCommit = std::function<void(RowEditRequest const&, RowEditDone)>;` `explicit CellEdits(RowEditCommit commit);` (a model's table: no patcher, no engine); `void commitRow(RowEditRequest request);` (queued with the row's other edits, in order); `void setDraft(RowId const& row, std::string_view column, std::string text, std::optional<std::string> error);` `[[nodiscard]] std::optional<std::string> draft(RowId const&, std::string_view) const;` `void clearDraft(RowId const&, std::string_view);` `[[nodiscard]] bool modified(RowId const&, std::string_view) const;` (from a success until `rowFetched`); `void rowFetched(RowId const& row);` (ends the row's modified marks and its settled drafts; a stale cell is left as it is); `void rowAbsent(RowId const& row);` (a fetch that named the row returned none — it was deleted, or no longer matches: drops the row's drafts, errors, modified marks and its edits not yet sent; an edit in flight settles as it would and leaves nothing behind); `void moveRow(RowId const& from, RowId const& to);` (drafts, errors, modified marks and queued edits); `void keepColumns(std::span<std::string const> columns);` (drops drafts, errors and marks of every other column); `[[nodiscard]] std::size_t errorCount() const;` `[[nodiscard]] std::size_t modifiedCount() const;` `stale` and `error` see row commits too. **Where a refusal lands**, in `CellEdits`' settle of a row commit: a `FieldError` whose `column` equals a cell's `member` (or, with `member` empty, its `column`) is that cell's error; one that matches no cell of the request, or names nothing, is the error of every cell of the request; a cell of the request no error reached keeps its draft and takes no error. `TableColumn::edit` (`std::optional<EditInfo>`).
  - `grid.hpp`: `struct ModelColumn { std::string id; std::optional<table::EditInfo> edit; std::shared_ptr<forms::FormModel const> model; std::string path; };` — for an editable column, `model` is `forms::FormModel::fromSchema(edit->action, <the action's schema>)` (a `FormModel` is made only by `fromSchema`, `field_model.hpp:799`) and `path` the field in it, `"value"` for a `CellEdit` and `"values." + edit->member` for a `RowEdit`; both empty for a column with no edit. `enum class EditMode : std::uint8_t { Cell, Row };` `struct ModelGridContext { reactive::Runtime* runtime; forms::ChoiceFetcher choices; forms::FormSessionOptions session; FormatContext format; std::function<bool(std::string_view action)> may; };` `class ModelGridSource final : public GridSource` — `ModelGridSource(std::shared_ptr<TableRows> rows, table::CellEdits& edits, std::vector<ModelColumn> columns, EditMode mode, ModelGridContext context)`. A cell's field is the field `path` of a `forms::FormSession{*context.runtime, *model, <a submitter that is never called>, context.choices, context.session}` the source makes per column on first use and keeps (one session per editable column, loaded with the row's value when an editor opens); `editor(key, column)` is `scalarControl(session, session.field(path))`, a draft is encoded by that field (`FieldState::encoded()`, which is `forms::encodeScalar` with the session's `ValueContext`), `toggles` is that field's kind being `Boolean`, and `text(key, column)` is the draft while one is held, else `formatCell(cell, description column, context.format)`; `rowCount()` is `rows->total().rows`, `keyAt(i)` the key of `rows->row(i)`, empty while that row is not fetched (Task 19's walk stops there, leaving `place()` on the placeholder); read-only through `cellReadOnly` with `hasEdit = edit && context.may(edit->action)`, `companionReadOnly` from the `EditInfo::readonly` column's `true`, `busy` and `locked` from `refusesEdit(rows, index, description)` (busy is `rows.states().isBusy(key)`, locked is `lockText(row, description)`); `unloaded` from the row's cell. `view()` is the source's **forwarding decorator** over `rows` (index #40): a `TableRows` that forwards every member to `rows`, sets `rows`' one `onChange` slot itself, and on each change first calls `edits.rowFetched(key)` for every row the change inserts or updates, advances the source's signal, and then calls the handler the mount set on it. The lowering hands the mount this decorator (as `base.rows`), never `rows`; nothing subscribes to `rows` a second time; `keyOf(record)` reads the record's field named by the key of the description `setDescription` last gave it, as a `RowId` (that column is always fetched, Task 9d's `alwaysFetched`); `rowModified(key)` is true while `edits` holds a modified mark or a draft for any of the row's columns. `marks(key, column)` is `{.readOnly = readOnly(key, column), .modified = edits.modified(row, column), .error = edits.error(row, column).has_value(), .stale = edits.stale(row, column)}` with `row` the key as a `RowId`. `CellEdits` is not reactive, so the source holds a `reactive::Signal<std::uint64_t>` it advances after each call it makes on `edits` and in each `RowEditDone`; `marks` and `rowModified` read it first, which makes them tracked reads, and the lowering mounts the table through `GridController::node(columns, options, base)` with `base` the `TableSource` it builds for any model table (Task 9d's `cellView`, `rowView`, `alwaysFetched`, `levels`) and `base.rows` the decorator above, so every column keeps its `cell` template, tone, tooltip and accent (a cell is `markedCell(base.cellView(c, row), marks)`, index #39) and a model's table draws its marks per cell from each cell's own row signal, as a form grid does. **The source's own edits are not busy runs**: the lowering's `RowEditCommit` sends through `ModelChannel::execute(action, body, {})` directly, not through a `MutationRuntime`, so no `RunObserver` sees it and nothing registers it in `RowStates`; a row with its own edits queued or in flight takes further edits (queued behind them by `CellEdits`) and shows them stale. A `CellEdit`/`RowEdit` run made by a document mutation of the session, or by `Table.*` on another table, does go through `MutationRuntime`, so `tableInputsOf` marks its row busy and the cell refuses. `void ModelGridSource::rowsAbsent(std::span<table::RowId const> keys)` calls `edits.rowAbsent` for each; the lowering wires it to `WindowRows::onRowsAbsent`. Cell mode: a commit sends `RowEditRequest{key, {{column, json, member}}}` (`member` as `EditCell` defines it); row mode: a row's cells are collected while focus stays in the row and sent as one request when focus leaves it or on `Ctrl+Enter`; a refusal sets each `FieldError` on its cell and, while the table has focus, moves focus to the first refused cell. A success whose `ChangedRows` does not name the row calls `rowFetched` at once. `void ModelGridSource::moveKey(Key const& from, Key const& to)` calls `rows.moveKey(from, to)` and `edits.moveRow(from, to)`; the lowering of an editable table calls it where Task 17c's lowering calls `TableRows::moveKey` for a `ChangedRow::from`. `void ModelGridSource::setDescription(table::TableDescription const&)` calls `keepColumns` with its column ids; the lowering calls it wherever it calls `WindowRows::setDescription`. A `FailedRow` for the row is a `FieldError` on each cell of the request.
  - `table_rows.hpp`: `void WindowRows::onRowsAbsent(std::function<void(std::span<table::RowId const>)> handler)` — called once per keyed fetch reply with the keys that fetch named and the reply holds no row for (a change event's refetch of a deleted row, or of one that left the filter). A Part D type; this task adds the member and its test, and Part D's plan names it as G's.
  - Status-line copy (through `ScreenCore::tableMessage`): an edit refused at a cell: unloaded `"This value is not loaded yet"`, busy `"This row is being saved"`, locked: the row's lock text.
  - Document: `editMode` (`cell`, the default, or `row`) on a model's table; refused on any other table.

- [ ] **Step 1: Write the failing table tests** — `tests/table/test_table_pending.cpp`:

```cpp
namespace {
struct RowCommits {
    std::vector<table::RowEditRequest> sent;
    std::vector<table::RowEditDone> done;
    table::RowEditCommit commit() { return [this](auto const& r, auto d) { sent.push_back(r); done.push_back(std::move(d)); }; }
};
table::RowId const k1{std::int64_t{1}};
}

// Mutation: rowFetched clearing a stale cell's draft.
TEST_CASE("cell edits: stale until the reply, modified until the next fetch", "[table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "count", "9", std::nullopt);
    edits.commitRow({k1, {{"count", "9"}}});
    CHECK(edits.stale(k1, "count"));
    c.done.at(0)(std::expected<void, std::vector<table::FieldError>>{});
    CHECK_FALSE(edits.stale(k1, "count"));
    CHECK(edits.modified(k1, "count"));
    CHECK(edits.draft(k1, "count") == "9");
    edits.rowFetched(k1);
    CHECK_FALSE(edits.modified(k1, "count"));
    CHECK_FALSE(edits.draft(k1, "count").has_value());
}

// Review Focus 4. Mutation: rowFetched ending stale edits too.
TEST_CASE("cell edits: a fetch before the reply keeps the cell stale", "[table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "count", "9", std::nullopt);
    edits.commitRow({k1, {{"count", "9"}}});
    edits.rowFetched(k1);                                    // the change event's refetch, before the reply
    CHECK(edits.stale(k1, "count"));
    CHECK(edits.draft(k1, "count") == "9");
    c.done.at(0)(std::expected<void, std::vector<table::FieldError>>{});
    CHECK(edits.modified(k1, "count"));
    edits.rowFetched(k1);
    CHECK_FALSE(edits.modified(k1, "count"));
}

// Mutation: a row's second request started before the first settles.
TEST_CASE("cell edits: a row's edits run one at a time, a refusal lands on each cell", "[table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.commitRow({k1, {{"label", R"("a")"}, {"note", R"("b")"}}});
    edits.commitRow({k1, {{"note", R"("c")"}}});
    CHECK(c.sent.size() == 1);
    c.done.at(0)(std::unexpected(std::vector<table::FieldError>{{"label", "taken"}, {"note", "too long"}}));
    CHECK(edits.error(k1, "label") == "taken");
    CHECK(edits.error(k1, "note") == "too long");
    CHECK(c.sent.size() == 2);
    CHECK(edits.errorCount() == 2);
}

// Mutation: settle filing an error under FieldError::column as given (the error sits on a column "value" or
// "remarkText" that the table does not have, and the cells show none).
TEST_CASE("cell edits: a refusal lands on the cell whose member it names", "[table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.commitRow({k1, {{.column = "count", .json = "11", .member = "value"}}});
    c.done.at(0)(std::unexpected(std::vector<table::FieldError>{{"value", "must be at most 10"}}));
    CHECK(edits.error(k1, "count") == "must be at most 10");
    CHECK_FALSE(edits.error(k1, "value").has_value());
    table::RowId const k2{std::int64_t{2}};
    edits.commitRow({k2, {{.column = "label", .json = R"("a")", .member = "label"},
                          {.column = "remark", .json = R"("b")", .member = "remarkText"}}});
    c.done.at(1)(std::unexpected(std::vector<table::FieldError>{{"remarkText", "too long"}}));
    CHECK(edits.error(k2, "remark") == "too long");
    CHECK_FALSE(edits.error(k2, "label").has_value());      // no error reached it
    table::RowId const k3{std::int64_t{3}};
    edits.commitRow({k3, {{.column = "label", .json = R"("a")", .member = "label"},
                          {.column = "remark", .json = R"("b")", .member = "remarkText"}}});
    c.done.at(2)(std::unexpected(std::vector<table::FieldError>{{"", "The row is closed"}}));
    CHECK(edits.error(k3, "label") == "The row is closed");   // names no cell: every cell of the request
    CHECK(edits.error(k3, "remark") == "The row is closed");
}

// Mutation: moveRow moving only drafts (the error stays on the old key).
TEST_CASE("cell edits: drafts are held by key and follow a key change", "[table][edits]") {
    table::CellEdits edits{[](auto const&, auto) {}};
    edits.setDraft(table::RowId{std::int64_t{5}}, "count", "abc", "Not a number");
    CHECK(edits.errorCount() == 1);
    edits.moveRow(table::RowId{std::int64_t{5}}, table::RowId{std::int64_t{9}});
    CHECK_FALSE(edits.draft(table::RowId{std::int64_t{5}}, "count").has_value());
    CHECK(edits.draft(table::RowId{std::int64_t{9}}, "count") == "abc");
    CHECK(edits.errorCount() == 1);
}

// Review Focus 5. Mutation: keepColumns dropping drafts but not their errors.
TEST_CASE("cell edits: a new revision drops drafts of a removed column", "[table][edits]") {
    table::CellEdits edits{[](auto const&, auto) {}};
    edits.setDraft(k1, "count", "abc", "Not a number");
    edits.setDraft(k1, "note", "kept", std::nullopt);
    std::vector<std::string> const kept{"label", "note"};
    edits.keepColumns(kept);
    CHECK_FALSE(edits.draft(k1, "count").has_value());
    CHECK(edits.draft(k1, "note") == "kept");
    CHECK(edits.errorCount() == 0);
}

// Mutation: rowAbsent ignored (the deleted row's draft and error stay, errorCount 1).
TEST_CASE("cell edits: a row absent from a refetch loses its drafts", "[table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "count", "abc", "Not a number");
    edits.setDraft(table::RowId{std::int64_t{2}}, "count", "7", std::nullopt);
    edits.rowAbsent(k1);
    CHECK_FALSE(edits.draft(k1, "count").has_value());
    CHECK_FALSE(edits.error(k1, "count").has_value());
    CHECK(edits.errorCount() == 0);
    CHECK(edits.draft(table::RowId{std::int64_t{2}}, "count") == "7");   // other rows untouched
}
```

`tests/test_ui_table_model.cpp`, the action types over a model the file registers (`struct EditRowsModel { std::vector<SpecimenRow> rows; table::ServedTable specimens(table::TableContext const&); };`, `BRIDGE_REGISTER_MODEL(EditRowsModel, "T_EditRows")`, `BRIDGE_REGISTER_TABLE(EditRowsModel, "specimens", &EditRowsModel::specimens)`, so the record is `T_EditRows.specimens`):

```cpp
using SpecimensModel = EditRowsModel;
struct EditSpecimenCount : table::CellEdit<&SpecimensModel::specimens, std::int64_t> {};
struct SpecimenValues { std::optional<std::string> label; std::optional<std::string> remark; };
struct EditSpecimenRow : table::RowEdit<&SpecimensModel::specimens, SpecimenValues> {};

// Mutation: annotateSchemaRoot left out of mergeSchemaExtras (no root keyword; tableInputsOf finds no input).
TEST_CASE("cell edits: an edit action reads, writes and names its table at the schema root", "[ui][table][edit]") {
    static_assert(glz::reflect<EditSpecimenCount>::size == 2);
    EditSpecimenCount edit{};
    REQUIRE_FALSE(glz::read_json(edit, R"({"key":4,"value":9})"));
    CHECK(edit.value == 9);
    CHECK(glz::write_json(edit).value() == R"({"key":4,"value":9})");
    auto dom = glz::read_json<glz::generic_u64>(forms::schemaJson<EditSpecimenCount>()).value();
    CHECK(glz::write_json(dom["x-cellEditOf"]).value() == R"("T_EditRows.specimens")");
    CHECK_FALSE(dom["properties"]["key"].contains("x-cellEditOf"));
    auto const inputs = ui::tableInputsOf(forms::schemaJson<EditSpecimenRow>());
    REQUIRE(inputs.size() == 1);
    CHECK(inputs.at(0).member == "key");
    CHECK(inputs.at(0).record == "T_EditRows.specimens");
    CHECK(inputs.at(0).kind == ui::TableInputKind::CellEdit);
}

// Mutation: onRowsAbsent called with every key the fetch named (row 2, which came back, is reported too).
TEST_CASE("table rows: a keyed refetch that returns no row reports it absent", "[ui][table][edit]") {
    // Task 9's Harness over a WindowRows of rows 1..3; onRowsAbsent records its calls
    // a change event naming keys 2 and 3; the keyed fetch's reply holds row 2 only
    // CHECK the handler was called once with exactly {3}
}
```

`tests/table/test_table_description.cpp`:

```cpp
// Mutation: checkEdits not comparing kinds.
TEST_CASE("description: an edit whose field does not fit its column is a source contract", "[table][description][edits]") {
    // column "count" (Integer) with edit {action "Lims_EditSpecimenCount"} whose value schema is {"type":"string"}:
    CHECK(table::checkEdits(description, schemaOf).error().code == table::TableErrorCode::SourceContract);
    // column "label" (Text) with edit {action "Lims_EditSpecimenRow", member "nosuch"}: SourceContract, message names "nosuch"
    // with value {"type":"integer"} and member "label" present: has_value()
}
```

- [ ] **Step 2: Write the failing UI tests** — `tests/test_ui_table_model.cpp`, over the fixture below, declared in `tests/grid_test_support.hpp` (`RowCommits` moves there from the table test, which includes the header):

```cpp
// Rows of a model's table held by the test: a view of keys, some not fetched, with states, locks and unloaded cells.
class FakeRows final : public ui::TableRows {
public:
    struct Row { table::RowId key; Value record; bool fetched = true; std::set<std::string> unloaded; };
    std::vector<Row> view;                                    // in view order
    ui::RowStates rowStates;
    std::vector<std::pair<table::RowId, table::RowId>> moved;  // moveKey's calls
    void deliver(std::size_t index);                           // marks the row fetched; reports it Updated
    void reorder(std::vector<table::RowId> keys);              // the view in this order; reports the moves
    void drop(table::RowId const& key);                        // out of the view; restore(row, index) puts it back
    void restore(Row row, std::size_t index);
    void refetch(table::RowId const& key, Value record);       // replaces the record; reports it Updated
    // TableRows: total() {view.size(), Exact, true}; row(i) empty while !fetched; unloadedLevels from `unloaded`;
    // visible/keysOf/setSort/setFilter trivial; states() -> rowStates; moveKey records; onChange keeps ONE handler.
};

struct ModelGridFixture {
    morph::test::formsengine::Harness h;
    std::shared_ptr<FakeRows> rows = std::make_shared<FakeRows>();
    RowCommits commits;
    table::CellEdits edits{commits.commit()};
    std::set<std::string, std::less<>> permitted{"Lims_EditSpecimenCount", "Lims_EditSpecimenRow"};
    std::unique_ptr<ui::ModelGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // `keys` rows with label, remark, count, countFrozen, note, lockedBy fields; the description names `key` as
    // the key and `lockedBy` as rowLock; ModelGridContext{&h.rt, h.server.fetcher(), {}, FormatContext{}, may}
    // with `may` reading `permitted`; the columns' models are fromSchema of the two schemas below.
    explicit ModelGridFixture(std::vector<std::int64_t> keys, ui::EditMode mode = ui::EditMode::Cell);
};
```

Columns: `label` and `remark` (Text, edit `Lims_EditSpecimenRow`, members `label` and `remarkText` — the second differs from its column id on purpose), `count` (Integer, edit `Lims_EditSpecimenCount`, readonly companion `countFrozen`), `note` (no edit). Each case declares `ModelGridFixture f{{…}}`; `f.rows->rowStates` is what the comments call `states()`. The schemas, also added to `TableHarness`'s catalog: `Lims_EditSpecimenCount` `{"type":"object","x-cellEditOf":"Lims_SampleModel.samples","properties":{"key":{"type":"integer"},"value":{"type":"integer","maximum":10}},"required":["key","value"]}`, `Lims_EditSpecimenRow` `{"type":"object","x-rowEditOf":"Lims_SampleModel.samples","properties":{"key":{"type":"integer"},"values":{"type":"object","properties":{"label":{"type":["string","null"]},"remarkText":{"type":["string","null"]}}}},"required":["key"]}`, each with `effects` `updatesTable` of `Lims_SampleModel.samples` and that record in `records`.

```cpp
// Mutation: the busy test left out of ModelGridSource::readOnly (row 3 takes the edit).
TEST_CASE("model table: read-only is one rule", "[ui][table][edit]") {
    // rows 1..5; row 2 count unloaded; row 3 busy (states().busyKeys(7, {3}): another action's run); row 4's rowLock
    // cell "Being edited by another user";
    // row 5 countFrozen true
    for (std::int64_t key : {2, 3, 4, 5}) CHECK(f.source->readOnly(ui::Key{key}, "count"));
    CHECK(f.source->readOnly(ui::Key{std::int64_t{1}}, "note"));            // no edit action
    f.grid->focusCell(at(2, "count"));                                      // arrows and clicks reach read-only cells
    f.grid->type("7");
    CHECK(f.grid->message() == "This value is not loaded yet");
    f.grid->focusCell(at(3, "count")); f.grid->type("7");
    CHECK(f.grid->message() == "This row is being saved");
    f.grid->focusCell(at(4, "count")); f.grid->type("7");
    CHECK(f.grid->message() == "Being edited by another user");
    CHECK(f.commits.sent.empty());
    f.grid->focusCell(at(1, "count")); f.grid->type("3"); f.grid->press("Enter");
    CHECK(f.commits.sent.size() == 1);
    CHECK(f.grid->focus() == at(1, "count"));          // Enter down passes rows 2–5, all read-only in count: it stays
    f.permitted = {"Lims_EditSpecimenRow"};                                  // count's action refused
    CHECK(f.source->readOnly(ui::Key{std::int64_t{1}}, "count"));
}

// Mutation: the commit sent through MutationRuntime (its RunObserver marks row 1 busy and the second cell refuses).
TEST_CASE("model table: the table's own edits do not make its row busy", "[ui][table][edit]") {
    // rows 1..2, all editable; EditMode::Cell
    // focus (1, label), type "a", Enter: one request in flight; CHECK_FALSE f.rows.states().isBusy(1)
    // focus (1, remark), type "b", Enter: CHECK f.grid->message() is empty, f.commits.sent.size() == 1 (queued),
    //   and the (1, remark) cell is stale
    // settle the first: CHECK f.commits.sent.size() == 2
    // another action's run on row 2 (f.rows.states().busyKeys(7, {2})): type on (2, label): message
    //   "This row is being saved", nothing sent
}

// Mutation: the walk passing over a row not fetched yet as if it were read-only (focus lands on row 5).
TEST_CASE("model table: the walk stops at a row not fetched yet", "[ui][table][edit]") {
    // ModelGridFixture f{{1, 2, 3, 4, 5}}; f.rows->view[2].fetched = false; all editable
    // focus (2, count), type "1", Enter:
    //   CHECK_FALSE(f.grid->focus().has_value());
    //   CHECK(f.grid->place() == ui::GridFocus{std::nullopt, 2, "count"});
    //   CHECK_FALSE(f.source->keyAt(2).has_value()); f.commits.sent.size() == 1 (row 2 only)
    // Enter again before the row arrives: Handled, place() unchanged
    // f.rows->deliver(2): CHECK(f.grid->focus() == at(3, "count")) and place() == GridFocus{Key{3}, 2, "count"}
    // type "2", Enter: CHECK f.commits.sent.back().row == RowId{3}
}
```

Interpreter-level, same file, through `ui::testing::TableHarness` (index #42: a whole document with a `models` block and an alias over `Table.samples`; `Lims_EditSpecimenCount` and `Lims_EditSpecimenRow` in its catalog as above and in its permitted actions unless a case removes one), `replyDescribe`, `replyPage`, `statusOf`, `elementValue`, `cellText`, `cellAccent` and `cellMarks(h, key, column)` (index #42's helper; this task adds none of its own) — so the lowering is tested on Windows, where `morph_net_tests` does not build:

```cpp
// Mutation: the lowering leaving a column with `edit` read-only (no request is sent).
TEST_CASE("model table: an editable column lowers to a cell edit", "[ui][table][edit][interpreter]") {
    // a description whose `count` column has edit {action "Lims_EditSpecimenCount"}; a page of rows 1..3
    // the node gives `count` a `cell` template (a text with testId "count_<key>") and a `tone` expression
    // h.backend.clickCell(table, 0, <count's index in Table::columns>, true); edit the editor to "4"; press Enter
    // CHECK the template drew before the edit (h.text("count_1")) and cellAccent(h, 1, "count").tone is the
    //   expression's: an editable column keeps the lowering's cell view (second mutation: node() replacing
    //   base.cellView with a plain text cell)
    // CHECK the channel received execute("Lims_EditSpecimenCount", {"key":1,"value":4}) and no MutationRuntime run
    //   started (statusOf(h).standing is empty: nothing "Saving")
    // reply {"rows":{"rows":[{"key":1}]}} and a change event naming 1; replyPage with count 4 for row 1:
    //   before the page: CHECK cellMarks(h, 1, "count") == "m" (drawn by the cell's view); after it:
    //   CHECK cellText(h, 1, "count") == "4" and cellMarks(h, 1, "count") == ""
}
// Mutation: the lowering ignoring the catalog's permitted set (the refused principal's cell opens an editor).
TEST_CASE("model table: a column whose action is not permitted is read-only", "[ui][table][edit][interpreter]") {
    // tableCatalog without Lims_EditSpecimenCount permitted: a double click on (1, count) opens no editor;
    // typing "4" posts "This cell is read-only" (statusOf(h).message)
}
// Mutation: the loader accepting editMode on a `rows` table.
TEST_CASE("model table: editMode is refused on a table that is not a model's", "[ui][table][edit][interpreter]") {
    // {"kind":"table","rows":{"ref":"items"},"editMode":"row"}: loadDocumentText fails, the message names editMode;
    // {"kind":"table","model":"m","table":"specimens","editMode":"sideways"}: fails, naming the value;
    // {"kind":"table","model":"m","table":"specimens","editMode":"row"}: loads, and a Tab from (1, label) to
    //   (1, remark) after typing sends nothing until focus leaves row 1
}

// Mutation: the walk following the edited row instead of the view position at the commit.
TEST_CASE("model table: Enter after an edit that moves its row lands on the row that was below", "[ui][table][edit]") {
    // view [1, 2, 3], all editable; focus (1, count), type "9", Enter; f.rows->reorder({2, 3, 1})
    CHECK(f.grid->focus() == at(2, "count"));
}

// Mutation: ModelGridSource setting rows->onChange itself beside the mount (whichever is set second wins: either
// the mounted row keeps the old text, or the modified mark never clears).
TEST_CASE("model table: the mount and the drafts share the rows' one change slot", "[ui][table][edit]") {
    // ModelGridFixture f{{1, 2}}; mounted through f.grid->node(columns, {}, base) with base.rows = &f.source->view()
    // focus (1, count), type "9", Enter; settle the request ok: the row of key 1 records marks:<count> == "m"
    // f.rows->refetch(1, <count 9>): CHECK the mounted cell's text is "9" (the mount's handler ran) and
    //   marks:<count> == "" (rowFetched ran)
}

// Mutation: drafts held by the window (FakeRows dropping row 5 loses them).
TEST_CASE("model table: cells of a model's table", "[ui][table][edit]") {
    // cell mode: focus (1, count), type "9", Enter -> one RowEditRequest{1, {{"count","9"}}}; marks stale;
    //            reply ok with ChangedRows naming 1 -> marks modified; FakeRows refetches 1 -> not modified
    // refusal:   type "9" on (2, count) -> the request's cell is {"count", "9", "value"}; reply
    //            FieldError{"value","Row 2 is closed"} -> error "Row 2 is closed" on (2, count), draft "9" kept;
    //            type "11" on (3, count): the field's own maximum refuses it ("above-maximum"), nothing is sent
    // scroll:    type "abc" on (5, count) -> errorCount 1; FakeRows drops then restores row 5 -> draft "abc", error kept
    // from:      f.source->moveKey(5, 9) -> draft and error on (9, count); FakeRows recorded moveKey(5, 9)
    // row mode:  EditMode::Row; type "a" on (1, label), Tab, type "b" on (1, remark), Enter -> nothing sent;
    //            focusCell (2, label) -> one RowEditRequest{1, {{"label", "\"a\"", "label"},
    //            {"remark", "\"b\"", "remarkText"}}};
    //            a refusal FieldError{"remarkText", "too long"} -> error on (1, remark), none on (1, label), and
    //            focus back on (1, remark);
    //            on row 2, type "c" on (2, label) and press Ctrl+Enter -> one request for row 2 with focus still there
}
```

`tests/net/test_push_tables.cpp`, over Task 2's fixture with a model registering table `specimens`, `Lims_EditSpecimenCount : table::CellEdit<&SpecimensModel::specimens, std::int64_t>` declaring `effects::updatesTable<&SpecimensModel::specimens>` and returning `ChangedRows`:

```cpp
// Mutation: the client sending {"key":…, "value":…} under the wrong action id (the model never sees it).
TEST_CASE("push tables: a cell edit reaches the model and the change event refetches the row", "[net][tables][edit]") {
    // client commits count = 4 on row 1: the server model's row 1 count == 4; the other client's window refetches
    // row 1; the editing client's cell is not modified after its refetch; the action's schema carries
    // "x-cellEditOf":"<SpecimensModel>.specimens"
}
// Mutation: the server's catalog listing every action as permitted (the viewer's column stays editable).
TEST_CASE("push tables: a principal refused the edit action sees the column read-only", "[net][tables][edit]") {
    // the authoriser refuses Lims_EditSpecimenCount for principal "viewer": the catalog's permitted set lacks it, and
    // the lowered count column is read-only for that client
}
```

Write each commented case out in Catch2 with the fixture's calls; the comments are the assertions and their order.

- [ ] **Step 3: Run to see them fail** — `cmake --build --preset cl-debug --target morph_table_tests`; expected: `table::RowEditRequest` not declared.

- [ ] **Step 4: Implement** `EditInfo` and `TableColumn::edit` in `description.hpp`; `CellEdit`, `RowEdit`, `checkEdits` (called from Task 2's describe path) and the `CellEdits` additions in `edits.hpp`; busy rows from `x-cellEditOf`/`x-rowEditOf` inputs (the lowering's `RunObserver::started` reads `TableInputKind::CellEdit` as `busyKeys(run, {body.key})`); `ModelGridSource` with its decorator and per-column edit sessions; the lowering of editable model columns (each `ModelColumn::model` from the catalog's schema, the request's `member` from the column's `EditInfo`, the table mounted through `node(columns, options, base)`), `editMode` and its refusal.

- [ ] **Step 5: Run**

Run: `cmake --build --preset cl-debug --target morph_table_tests morph_tests && ctest --test-dir build/cl-debug -R "cell edits|description|model table|pending|table rows" --output-on-failure`, and on `linux-everything` (where `morph_net_tests` builds) `ctest --test-dir build/linux-everything -R "push tables" --output-on-failure`
Expected: all pass, including every existing case in `test_table_pending.cpp`, `test_table_reentrancy.cpp` and `test_table_selection.cpp`.

- [ ] **Step 6: Mutate**, **Step 7: Docs** (docs build), **Step 8: Commit** `feat(table): edit cells of a model's table`.

---

## Decisions where the spec leaves a choice

Each is what this plan implements. The first, second, fourth and sixth are already stated in editing.md and table.md, so no task edits the spec for them; the task that lands each of the others states it in the spec it updates.

- **A row busy with its own cell edits still takes edits.** editing.md queues edits to one row in order and refuses an edit in a busy row; a second cell of a row could never be entered while the first is saving if the table's own edits made the row busy. The table's own edits are therefore not busy runs, as editing.md ("A table's own edits make no row busy") and table.md ("Rows at work, rows that failed") state: they are sent through `ModelChannel::execute` directly, never registered in `RowStates`, and shown stale instead; a row busy with any other run (a document mutation, another table's action) refuses the edit (Task 24).
- **A row-mode refusal brings focus back.** editing.md commits a row when focus leaves it, so focus has left by the time a refusal arrives; it is moved back to the first refused cell while the table has focus, as editing.md ("A row at once") states (Task 24).
- **Computed and derived cells draw no modified mark.** editing.md line 300 marks any cell whose value differs from the loaded one; its test row (521) wants typing to mark "that cell and its row, not its neighbours", while a derive reply changes the neighbouring computed cells of that row. `FormSession::modified` is false for `x-computed` and `x-derived` members only; a cell the user typed in and a rule then froze stays modified (Task 19).
- **A deletion offers Undo; it does not ask first.** Rows are deleted at once and the status line says `"Deleted {n} rows"` with the entry "Undo", as editing.md ("Rows of a grid") states (Task 21).
- **Status-line copy the spec does not fix** is pinned in Tasks 19, 20, 21 and 24 (`"This cell is read-only"`, the standing `"Derived values could not be recalculated"` with the entry "Recalculate", the paste and row messages, the refusal messages). A failed derive round is a standing state, beside the ones index decision 19 lists, since it lasts until a round succeeds.
- **Busy rows from a `CellEdit` input** are listed in the roadmap's Task 17c, which lands before `CellEdit` exists; Task 24 adds `TableInputKind::CellEdit` (`x-cellEditOf`, `x-rowEditOf`) to Task 17a's `tableInputsOf`. `refusesEdit` stays as Part F defines it; the table's own edits never reach `RowStates`, so no exemption is needed on top of it.
- **A row absent from a refetch loses its drafts.** editing.md keeps a draft by key while its row scrolls away; a keyed fetch that names the row and returns none means the row was deleted or left the filter, and its drafts are dropped (`CellEdits::rowAbsent`, fed by `WindowRows::onRowsAbsent`, Task 24).
- **The editor's text is the cell's draft.** editing.md's `Escape` "restores the draft the cell had when it opened" (line 269) and its submit "first commits an open cell editor" (line 175) are both met with the draft written live: a commit closes the editor and is the one moment a change is meant (`committed()`, the derive round), so the form's submit runs the grids' commits first through `FormSession`'s before-submit hooks (Task 19).
- **A column group stays the group line, not a band.** editing.md says a group draws as a band of one level; the
  group line carries the group's filter button, which a band cannot, so a group draws as a one-level heading on its
  own line below the bands, over its columns while they stand together. editing.md ("Header bands") and table.md
  ("Column groups") say so (Task 22).
- **Summary rows are rows of a decorator, not of the data.** editing.md puts them "outside the order, the filter and the selection" (line 414) and gives them no place in the pipeline. `SummaryRows` wraps the data rows for the mount alone; counts, key ranges, the walk, paste and the `rows` scope read the data rows (Task 22).
- **A refusal that names no cell marks every cell of its request**, and one cell's name is its `EditCell::member` (Task 24); editing.md only says a `FieldError` naming `value` settles the cell.
- **`EditInfo` is declared in `description.hpp`**, which `edits.hpp` includes; editing.md's API surface lists it under `edits.hpp`, which would make the two headers include each other. Task 24 corrects that row.
- **`CellEdits` cases live in `tests/table/test_table_pending.cpp`**, beside that class's existing cases, in addition to the roadmap's `tests/test_ui_table_model.cpp` and `tests/net/test_push_tables.cpp`. `tests/forms/` is a new directory whose files join `morph_tests`.

## Self-review notes

- **Coverage.** Every editing.md Tests row has a test: "A grid is a form", "Read-only is one rule (grid)", "The walk" (grid), "A sorted grid keeps its record", "Submission", "What changed" — Task 19; "Derived values", "The server decides", "Kind per row", "Choices per row" — Task 20; "Cell selection", "Paste, fill, clear", "Rows of a grid" — Task 21; "Bands", "Spans", "Summary rows" — Task 22; "Fixed grids" — Task 23; "Cells of a model's table", "Read-only is one rule (model table)", "The walk" (model table) — Task 24. Invariants: 1 (Task 19, the grid writes only through `FieldState`), 2 (Task 20 "derived values": a typed member survives the reply), 3 (Task 20: an older reply ignored, values land by row key), 4 (`cellReadOnly`, Tasks 19 and 24), 5 (Task 19 "a sorted grid keeps its record"), 6 (Task 24 "read-only is one rule"), 7 (Task 21 "paste, fill, clear"), 8 (Task 22 "spans"), 9 (Task 20 "the server decides"). Every Failure modes row has its refusal or its test in the task that lands the key.
- **Review findings applied.** The summary flag is `TableRowLook::summary` (index #39); summary rows reach the mount through `SummaryRows`, and `group` on a form grid through `FormRowsSource::setGroup` (Task 22); a placeholder focus is `GridController::place()` / `GridFocus` (Tasks 19, 24); `ModelGridSource` builds its edit sessions from the action's schema and a `ModelGridContext`, and shares the rows' one change slot through a forwarding decorator (index #40, Task 24); the `CellEdit` meta declares no alias; `scalarControl` takes the session; `GridSource` gives the editor and `toggles`, and `node()` takes the columns and a base `TableSource`; the editor's text is the live draft and submit runs before-submit hooks; `makeFormGrid` returns the grid's objects; `EditInfo` is `description.hpp`'s; a refusal lands by `EditCell::member`; the fixed and model grid fixtures are declared; a span across columns records `<index>:<rows>x<columns>`; every `column` argument is a `Table::columns` index (index #37) and a rectangle's are positions; document tests use `TableHarness` (index #42).
- **Not covered by a test of its own:** the accessible band name is asserted only at function level (`bandedHeaderName`); the Qt Quick backend's live region and its reading order are left to `qt_quick/backend.md`'s existing conformance.
