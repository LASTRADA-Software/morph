# The One Table, Part F — How a Column Shows Its Cells, and Acting on Rows — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every table one formatter of a cell's text (places, rounding, empty text, units, labels, tone, tooltip, locked rows), and let a user act on rows: select the whole view, have the server resolve it, keep changed rows in view, follow a changed key, and see busy, failed and locked rows, with the application's own menu entries and a header it can disable.

**Architecture:** Task 15 adds `ColumnDisplay` and its companions to the table's description (`include/morph/table/description.hpp`) and one client function, `ui::formatCell`, which every backend, copy, the filter menu and the status line call. Task 17 adds `RowSelection`, `resolve` and the view's membership digest on the server (`include/morph/table/row_selection.hpp`, the view cache), retained rows in the query, the engine and the window, `ChangedRow::from` in the result walk, and the client's busy, failed and locked rows, menus and disabled header in `ui::TableRows` and the interpreter's lowering.

**Tech Stack:** C++23, glaze, Catch2, the Qt Quick backend (offscreen `morph_qt_quick_tests`), the terminal backend (`morph_tui_tests`).

**Spec:** `docs/spec/table/table.md` — "How a column shows its cells" (lines 369–455), "Values with units › Display" (2586–2593), "Acting on rows" and all its subsections (2258–2451), the matching rows of "Tests" (3032–3046), "Invariants" 31–35, "Failure modes" (3314–3322), Delivery order steps 9a and 9b (3378–3379), and the 9a/9b rows of "Neighbouring specs" (3345–3355). Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md`, Tasks 15 and 17.

Part F of 7; index: 2026-11-17-one-table.md

## Global Constraints

Every line of the roadmap's Global Constraints applies. Those this part leans on, verbatim:

- AGENTS.md "Comments and documentation": a comment or spec states what the code does now and why; no history, no issue numbers, no commit hashes.
- Every public symbol has complete Doxygen `@param`/`@tparam`/`@return`; the docs build runs `WARN_AS_ERROR = FAIL_ON_WARNINGS` (`cmake -S . -B build/doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF && cmake --build build/doc --target doc`).
- morph is public: no company, product or customer names in code, tests, specs or commit messages; examples use the neutral `lims` domain.
- Commits are authored `yaraslau <mail@yaraslau.com>` and end with the session's attribution lines.
- No GUI application is launched by hand or by an agent; Qt Quick behaviour is tested through `morph_qt_quick_tests` (offscreen).
- AGENTS.md "Verify rather than assert": every test names the mutation it was seen to fail on, and the task runs that mutation once and records the failing output in its commit message body.
- Limits, verbatim: `QueryLimits::maxSelectedRows = 1'000'000`, `maxRetained = 10'000`, `maxKeys = 1000`; `SessionOptions::maxCopyRows = 10'000`, `retainHold = 2 s`.
- A spec that describes code takes its part in the task whose step its row names: `docs/spec/ui/document.md` takes "a column's display parts, `tone` and `tooltip`" in Task 15 (step 9a) and "`cellMenu`, `headerMenu`, `retainChanged`, `headerEnabled`" and the element values `selectionSet`, `busyCount`, `selectedCountKind` in Task 17 (step 9b). A backend call this part adds is described in `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md` in the task that adds it.

**Build and run.** The standard task loop of the roadmap, with:

```bash
cmake --preset cl-debug                                              # once per tree
cmake --build --preset cl-debug --target morph_tests morph_table_tests
ctest --test-dir build/cl-debug -R "<pattern>" --output-on-failure

# Socket cases: morph_net_tests is POSIX-only (index decision 18); on Windows run them in WSL, or leave them to the
# Linux CI leg
cmake --preset linux-everything && cmake --build --preset linux-everything --target morph_net_tests
ctest --preset linux-everything -R "<pattern>" --output-on-failure

# Qt Quick and terminal cases (their own tree; offscreen, nothing is launched)
cmake --preset cl-qt-debug -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON   # once per tree
cmake --build build/cl-qt-debug --target morph_qt_quick_tests morph_tui_tests
ctest --test-dir build/cl-qt-debug -R "<pattern>" --output-on-failure
```

**Status line messages.** A transient refusal or notice goes through Part D's `ScreenCore::tableMessage(ElementState& table, std::string text)` (Task 9d, index decision 11; an empty text clears it), and, when it offers a choice ("Run on these"), through the overload `tableMessage(ElementState&, std::string text, std::vector<StatusEntry> entries)` that Task 17a adds. `ui::StatusEntry{std::string id; std::string label; bool operator==(StatusEntry const&) const = default;}` lives in `view.hpp` beside Part D's `TableStatus` (index decision 38: without the operator `TableStatus`'s defaulted `operator==` is deleted and its `Prop` does not compile), and `TableStatus` gains `std::vector<StatusEntry> entries` and `std::vector<StatusEntry> standingEntries`; the lowering keeps each entry's handler in a map keyed by `id`, and the backend reports a choice by `id` (`TableWidget::setOnStatusEntry(std::function<void(std::string)>)`). **Standing states** are not messages (index decision 19): "Saving n rows", "n rows could not be saved" with "Select them" / "Clear marks", and "n rows shown that no longer match" with "Show only matching rows" are a persistent part of `TableStatus` (`TableStatus::standing`, a `std::string`, with `standingEntries`), recomputed by the lowering from `RowStates::busyCount()`, `failedKeys()` and `counts().retained` whenever one changes; they stay until their source is zero, through the 6-second message hold and the user's gestures. When several hold, `standing` joins them with ". " in the order saving, failed, unmatched.

**Shared test fixtures from other parts.** `tests/test_ui_table_model.cpp` and `tests/ui_table_support.hpp` are Part D's (Task 9). The harness is `ui::testing::TableHarness` (index decision 42, added by Task 9d), a type of its own in `morph::ui::testing` beside the existing `Harness`, which is not edited: its constructor calls `session.setLocale("en")`, and its catalog is `tableCatalog()` (`corpusCatalog()` plus `Table.samples` and `Table.samples.describe` on `Lims_SampleModel`) plus the actions each later task adds. Its members are Part D's: `h.mount(text)`, `h.settle()`, `h.advance(ms)`, `h.backend` (`RecordingBackend`), `h.dispatcher`, `h.host`, `h.diagnostics`, `h.session`. Every helper is a free function taking the fixture, Part D's among them: `replyDescribe(h)`, `replyPage(h, json)`, `samplesDescription(n)`, `describeJson(description)`, `pageJson(offset, count, total, columns, kind, undecided, awaiting)`, `lastQuery(h)` (the last `TableQuery` sent), `statusOf(h)` (the `TableStatus`; `.message` is the status line's message) and `formatFor(tag)`. A table's widget id, which the `RecordingBackend` drivers take, is `h.backend.find("Table", "testId", "t")`, so this part's table nodes carry `"testId":"t"`. `formatFor` (a `FormatContext` built from the locale table; a default-constructed `FormatContext` has no group separator, `render/locale_format.hpp:108`). Part E (Task 12) adds the `RecordingBackend` drivers `menuOf(table, column)`, `chooseMenu(table, column, labels)`, `moveColumn`, `clickFilter`, `pressCopy(table)` and `clipboardText()`, and the defaulted `HeaderClick` on the existing `clickHeader(int, std::size_t)`; Part D's Task 10a adds `selectTableRange`. Every `column` argument is an index into `Table::columns` (index decision 37).

**Documents.** Every test mounts a whole document with a `models` block and an alias (index decision 42); a table node names the alias, and the table's record is `Lims_SampleModel.samples`. The test file's `doc(view, mutations = "{}")` returns

```json
{"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},"mutations":<mutations>,"view":<view>}
```

and the sketches below write `h.mount(doc(R"(…)"))` with a node `{"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",…}`. A mutation is `{"model":"sample","action":"<id>","body":{"obj":{…}}}`, as the corpus documents write one (`tests/test_ui_interpreter_commands.cpp:31`).

**Catalog actions.** A task whose client tests run an action adds it to `TableHarness`'s catalog in that task, before `h.mount`, with its schema text, `effects` and `records` (index decision 42), through `addAction(TableHarness& h, std::string id, std::string schemaJson, ui::ActionInfo info)`. Task 17a adds that helper to `tests/ui_table_support.hpp` with what it needs: a member `std::map<std::string, std::pair<std::string, ui::ActionInfo>, std::less<>> extraActions` on `TableHarness`, which the `Catalog::action` and `Catalog::schema` functions of the harness's catalog (`include/morph/ui/document.hpp:101`) consult before `tableCatalog()`'s own; the session's catalog and `mount`'s are that one catalog. This part's four actions all change the table's record, so the test file has one builder:

```cpp
// The catalog entry of an action on Lims_SampleModel that declares `updatesTable` on `samples` and returns
// {"rows": ChangedRows}: the held record's keys are at "rows.rows", by the member "key".
morph::ui::ActionInfo samplesAction() {
    morph::ui::ActionInfo info;
    info.modelType = "Lims_SampleModel";
    info.effects = std::vector<morph::ui::ActionEffect>{{.kind = "updates", .record = "Lims_SampleModel.samples"}};
    info.records = {morph::ui::ActionRecordInfo{
        .record = "Lims_SampleModel.samples", .keyField = "key", .paths = {"rows.rows"}}};
    return info;
}
// An action whose member `rows` is a RowSelection of the table, and one whose member `row` is a RowRef of it.
inline constexpr std::string_view kSelectionSchema =
    R"({"type":"object","properties":{"rows":{"type":"object","x-selectionOf":"Lims_SampleModel.samples"}}})";
inline constexpr std::string_view kRowSchema =
    R"({"type":"object","properties":{"row":{"x-rowOf":"Lims_SampleModel.samples"}}})";
```

| Task | Action | Schema | Mutation in the document |
|---|---|---|---|
| 17a | `Lims_TagRows` | `kSelectionSchema` | `"tag":{"model":"sample","action":"Lims_TagRows","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}}` |
| 17b | `Lims_CloseRows` | `kSelectionSchema` | `"close":{"model":"sample","action":"Lims_CloseRows","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}}` |
| 17c | `Lims_SaveAs` | `kSelectionSchema` | `"save":{"model":"sample","action":"Lims_SaveAs","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}}` |
| 17c | `Lims_TouchRow` | `kRowSchema` | `"touch":{"model":"sample","action":"Lims_TouchRow","body":{"obj":{"row":{"ref":"picked"}}}}` (`picked` a `state` name the test sets) |

Each is added as `addAction(h, "<id>", std::string{<schema>}, samplesAction())`.

**Helpers this part adds** to `tests/ui_table_support.hpp`, in the task that first calls them, all free functions taking `TableHarness&` (index decisions 20 and 42): `value(json)` (a `ui::Value` from JSON text: `ui::parseJson` then `ui::valueFromJson`, `include/morph/ui/value.hpp:752` and `:570`, both `REQUIRE`d), `elementValue(h, id, name)` (an element value), `reportsOf(h)` (the reasons of `h.diagnostics`), `replyDescribe(h, json)` (an overload answering the describe request with this text; Part D's `replyDescribe(h)` serves `samplesDescription`), `cellText(h, key, column)` and `columnOf(h, id)` (the `ui::TableColumn` given to `setColumns`), `cellAccent(h, key, column)` (the last `setCellAccent` of that cell) — Task 15; `addAction` (above), `fireEvent(h, record, keys)` (a record change event), `runMutation(h, name)`, `settleMutation(h, name, replyJson)`, `refuseMutation(h, name, code)`, and the `RecordingBackend` drivers `selectAll(table)`, `clickRow(table, key)`, `ctrlClickRow(table, key)`, `chooseStatusEntry(table, id)` — Task 17a; `rowMarks(h, key)` (the last `setRowMarks` of that row) — Task 17b; `rightClickCell(table, key, column)` — Task 17d. In a comment-only case `run`, `settle(mutation, …)`, `refuse`, `click`, `ctrlClick` are `runMutation`, `settleMutation`, `refuseMutation`, `clickRow`, `ctrlClickRow`, and `element(id, name)` is `elementValue(h, id, name)`. `tests/net/test_push_tables.cpp` is Part A's (Task 2), with its server-and-client harness; `morph_net_tests` runs on `linux-everything` only (index decision 18).

## Review Focus

1. **Negative and boundary values in the display.** `-0.125` at two places under `HalfUp` is `-0.13`, under `HalfEven` `-0.12`; a value that rounds to zero shows no sign (`-0.001` → `0.00`); `0` with `empty` set shows `0`; the `de` locale with grouping shows `-12.500,50`. Test: Task 15, `"format cell: negative halves and empty text"`.
2. **A double whose binary value is below its decimal text.** `1.005` at two places under `HalfUp` is `1.01`, as a person reads the value, though the binary double is 1.00499…. Test: Task 15, `"format cell: a double rounds from its shortest decimal text"`.
3. **A key of `except` that leaves the view.** A view selection of 10 rows less key 3, then an edit takes row 3 out of the view: the count is 9 and exact, not 8. Test: Task 17a, `"view selection: a key of except that leaves the view is dropped from it"`.
4. **A key change onto a key the client already holds.** `ChangedRow{key: 7, from: 3}` while row 7 is held and selected: the selection holds 7 once, and row 3 is fetched again. Test: Task 17c, `"key change: onto a key already held"`.
5. **Two actions over overlapping rows.** Run A names rows {1, 2}, run B rows {2, 3}; A settles: row 2 stays busy until B settles, and `busyCount` is 2 then 0. Test: Task 17c, `"busy rows: overlapping runs"`.

---

## Task 15: How a column shows its cells (table.md step 9a)

**Spec:** "How a column shows its cells" (369–455); "Values with units › Display" (2586–2593); test rows "A column's display", "Companion columns travel with their column", "Locked rows and companions" (companion half); invariant 32.

**Files:**
- Modify: `include/morph/table/description.hpp` — `Rounding`, `ValueLabel`, `ColumnDisplay`; `TableColumn::{display, tone, tooltip}`; `TableDescription::{rowTone, rowLock}`; `validateDisplay`, `CompanionOverrides`, `columnsWithCompanions`, `companionColumns`. Glaze meta writing `Rounding` as `"halfEven"` / `"halfUp"`.
- Create: `include/morph/ui/cell_format.hpp` — `CellLook`, `DisplayUnits`, `formatCell`, `cellLook`, `cellOfOperand`, `operandOf`, `distinctTexts`, `headerText`, `headerAccessibleName`.
- Modify: `include/morph/ui/view.hpp` (`ui::TableColumn::unit`, `CellAccent`, `AccentedCell`, `accentedCell`), `include/morph/ui/backend.hpp` (`TableWidget::setCellAccent`), `include/morph/ui/mount.hpp` (binds an accented cell's `accent` in the cell's own scope to `setCellAccent`), `include/morph/ui/testing/recording_backend.hpp` (records each `setCellAccent` and each column's `unit`), `src/qt_quick/table.cpp`, `src/qt_quick/kinds.hpp`, `src/qt_quick/qml/MorphTable.qml`, `src/tui/table_widget.hpp`, `src/tui/table_widget.cpp`.
- Modify: `src/ui/interpreter.cpp` (a column's display parts, `tone`, `tooltip`; the default cell's text through `formatCell`; the companions in the fetched columns; companions out of the layout), `src/ui/document.cpp` (load-time checks of the new parts).
- Create: `tests/test_ui_cell_format.cpp` (add to `morph_tests` in `tests/CMakeLists.txt`, beside `test_ui_table.cpp`).
- Test: `tests/table/test_table_description.cpp`, `tests/test_ui_table_model.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_table.cpp`.
- Docs: `docs/spec/ui/document.md` (the `table` row: a column's parts `decimals`, `rounding`, `padPlaces`, `grouping`, `empty`, `unit`, `date`, `labels`, `tone`, `tooltip`), `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md` (`setCellAccent`, a column's `unit`).

**Interfaces:**
- Consumes: `table::TableColumn`, `TableDescription`, `describe` (Task 1); `table::Unloaded`, `ColumnInfo::level` (Task 3); `DistinctValue{text, operand, count}` (Task 8a); `table::UnitInfo` (qualified in `ui` code: `ui::UnitInfo` already exists in `expression.hpp`), `UnitId`, `UnitRegistry`, `UnitRegistryBuilder`, `Measure`, `ColumnKind::Measure` (Task 8c); `ui::FormatContext` (`include/morph/ui/expression.hpp:325`), `ui::localeFacts`, `ui::withoutGrouping` (`include/morph/ui/locale_table.hpp`); `render::formatCanonicalNumber` (`include/morph/render/locale_format.hpp:708`); `math::checkedRoundToDecimalPlaces`, `math::RoundingMode` (`include/morph/util/rational.hpp`); `table::parseDecimal`, `table::displayText` (`include/morph/table/data_source.hpp`); `ui::formatDate(std::string_view pattern, time::DateTime local, std::int32_t offsetMinutes)` (`include/morph/ui/date_pattern.hpp:327`, in `morph::ui`, not `detail`); Task 9d's `TableSource::cellView` and `setCellLoad`, and Task 9's `formatFor` test helper; Task 9's `WindowRows`, its fetched-column set, and `ui::detail::standInCellText`; Part E's copy (`ui::copyText`, `IViewBackend::copyText`, Task 12f), values list (`table_filter_value_<i>`, Task 12d) and column chooser (`table_columns`, Task 12e).
- Produces (later tasks and Part G's editing rely on these exact names):

```cpp
namespace morph::table {
enum class Rounding : std::uint8_t { HalfEven, HalfUp };          // JSON "halfEven", "halfUp"
// Both declare operator== (index decision 38): TableColumn's and TableDescription's defaulted operator== cover
// them, and are deleted without it.
struct ValueLabel {
    std::string operand;
    std::string text;
    [[nodiscard]] bool operator==(ValueLabel const&) const = default;
};
struct ColumnDisplay {
    std::optional<std::uint32_t> decimals;
    Rounding rounding = Rounding::HalfEven;
    bool padPlaces = true;
    bool grouping = false;
    std::string empty;
    std::string unit;
    std::string date;
    std::vector<ValueLabel> labels;
    [[nodiscard]] bool operator==(ColumnDisplay const&) const = default;
};
// TableColumn gains: ColumnDisplay display; std::string tone; std::string tooltip;
// TableDescription gains: std::string rowTone; std::string rowLock;
[[nodiscard]] std::expected<void, TableError> validateDisplay(TableDescription const& description);
struct CompanionOverrides {
    std::set<std::string, std::less<>> tone;     // column ids whose document part gives `tone`
    std::set<std::string, std::less<>> tooltip;  // column ids whose document part gives `tooltip`
    bool rowTone = false;                        // the node gives `rowTone`
};
[[nodiscard]] std::vector<std::string> columnsWithCompanions(TableDescription const& description,
                                                             std::span<std::string const> columns,
                                                             CompanionOverrides const& overrides = {});
[[nodiscard]] std::vector<std::string> companionColumns(TableDescription const& description);  // sorted
}  // namespace morph::table

namespace morph::ui {
enum class CellLook : std::uint8_t { Text, Empty, Unloaded, Invalid };
struct DisplayUnits {
    table::UnitRegistry const* units = nullptr;                       // not owned; null: a Measure cell is Invalid
    std::map<std::string, std::string, std::less<>> byDimension;      // dimension -> unit id the user chose
};
[[nodiscard]] std::string formatCell(table::Cell const& cell, table::TableColumn const& column,
                                     FormatContext const& locale, DisplayUnits const& units = {});
[[nodiscard]] CellLook cellLook(table::Cell const& cell, table::TableColumn const& column,
                                DisplayUnits const& units = {});
[[nodiscard]] table::Cell cellOfOperand(std::string_view operand, table::TableColumn const& column,
                                        DisplayUnits const& units = {});
[[nodiscard]] std::optional<std::string> operandOf(table::Cell const& cell, table::TableColumn const& column,
                                                   DisplayUnits const& units = {});
[[nodiscard]] std::vector<std::string> distinctTexts(std::span<table::DistinctValue const> values,
                                                     table::TableColumn const& column, FormatContext const& locale,
                                                     DisplayUnits const& units = {});
[[nodiscard]] std::string headerText(std::string_view label, std::string_view unit);            // "Mass [kg]"
[[nodiscard]] std::string headerAccessibleName(std::string_view label, std::string_view unit);  // "Mass, kg"
// view.hpp. What a cell shows beside its text (index decision 39). `invalid`: the value could not be read as the
// column's kind; the backend draws its error mark and `tooltip` is "The value could not be read".
struct CellAccent {
    std::optional<Tone> tone;
    std::string tooltip;
    bool invalid = false;
    [[nodiscard]] bool operator==(CellAccent const&) const = default;
};
// A cell with an accent: a node a `TableSource::cellView` returns (or a cell of a `TableRowView`). Anywhere but a
// table cell it mounts its content and the accent is not read.
struct AccentedCell { Node content; Prop<CellAccent> accent; };
[[nodiscard]] Node accentedCell(Node content, Prop<CellAccent> accent);
// ui::TableColumn (view.hpp) gains: std::string unit;
// TableWidget (backend.hpp) gains, with a default that does nothing, as `setCellLoad` has:
//   virtual void setCellAccent(Widget& row, std::size_t column, CellAccent const& accent);
//   // `column`: an index into `Table::columns` (index decision 37)
}  // namespace morph::ui
```

**Cells, not rows, carry the accent** (index decisions 28 and 39). A table mounted through `TableSource` builds no `TableRowView`; `TableRowView` stays the C++ `table<Row>()` path's only and gains nothing here. The lowering's `cellView(column, rowSignal)` returns `accentedCell(content, accent)`; Part G's Task 19 wraps that as `markedCell(accentedCell(content, accent), marks)`, and the mount unwraps a `MarkedCell` first, then an `AccentedCell`, then mounts `content`. The mount binds `accent` in the cell's own scope and calls `setCellAccent(row, column, accent)` after the row's key, on every change, and whenever a pooled cell is attached to another row widget (Task 10a), whether or not the accent changed; a pooled cell rebound to another row (its signal set) re-evaluates its accent with its content.

The third parameter is the session's `ui::FormatContext`, as table.md ("Where it is formatted") names it: the session's formatting (numeric facts, date patterns, display zone), and it is what the existing default cell (`textOf(*value, scope->formatting())`, `src/ui/interpreter.cpp`) formats with. `DisplayUnits` carries what a `Measure` cell needs and the three-argument call does not have.

- [ ] **Step 1: Write the failing description tests** in `tests/table/test_table_description.cpp`

```cpp
TEST_CASE("description: a column's display is refused where it cannot apply", "[table][description][display]") {
    using namespace morph::table;
    auto base = [] {
        TableDescription d;
        d.id = "samples";
        d.key = "id";
        d.columns = {
            TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
            TableColumn{.info = {.id = "mass", .kind = ColumnKind::Decimal}, .label = "Mass",
                        .display = {.decimals = 2, .unit = "kg"}, .tone = "massTone", .tooltip = "massTip"},
            TableColumn{.info = {.id = "amount", .kind = ColumnKind::Measure}, .label = "Amount"},
            TableColumn{.info = {.id = "massTone", .kind = ColumnKind::Text}, .shown = false},
            TableColumn{.info = {.id = "massTip", .kind = ColumnKind::Text}, .shown = false},
            TableColumn{.info = {.id = "lockedBy", .kind = ColumnKind::Text}, .shown = false},
        };
        d.rowLock = "lockedBy";
        return d;
    };
    REQUIRE(validateDisplay(base()).has_value());

    auto measureDecimals = base();
    measureDecimals.columns[2].display.decimals = 3;
    CHECK(validateDisplay(measureDecimals).error().code == TableErrorCode::SourceContract);
    auto measureUnit = base();
    measureUnit.columns[2].display.unit = "kg";
    CHECK(validateDisplay(measureUnit).error().code == TableErrorCode::SourceContract);
    auto missingTone = base();
    missingTone.columns[1].tone = "nope";
    CHECK(validateDisplay(missingTone).error().code == TableErrorCode::SourceContract);
    auto numericTooltip = base();
    numericTooltip.columns[1].tooltip = "id";
    CHECK(validateDisplay(numericTooltip).error().code == TableErrorCode::SourceContract);
    auto missingLock = base();
    missingLock.rowLock = "nope";
    CHECK(validateDisplay(missingLock).error().code == TableErrorCode::SourceContract);
    CHECK(validateDisplay(missingLock).error().column == "nope");
}
// Mutation: validateDisplay skipping rowLock, which accepts `missingLock`.

TEST_CASE("description: companions travel with their column", "[table][description][display]") {
    using namespace morph::table;
    TableDescription d;  // id, mass (tone massTone, tooltip massTip), name; rowTone rowState; rowLock lockedBy
    d.key = "id";
    d.columns = {
        TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}},
        TableColumn{.info = {.id = "mass", .kind = ColumnKind::Decimal}, .tone = "massTone", .tooltip = "massTip"},
        TableColumn{.info = {.id = "name", .kind = ColumnKind::Text}},
        TableColumn{.info = {.id = "massTone", .kind = ColumnKind::Text}, .shown = false},
        TableColumn{.info = {.id = "massTip", .kind = ColumnKind::Text}, .shown = false},
        TableColumn{.info = {.id = "rowState", .kind = ColumnKind::Text}, .shown = false},
        TableColumn{.info = {.id = "lockedBy", .kind = ColumnKind::Text}, .shown = false},
    };
    d.rowTone = "rowState";
    d.rowLock = "lockedBy";
    std::vector<std::string> const mass{"mass"};
    std::vector<std::string> const name{"name"};
    CHECK(columnsWithCompanions(d, mass) ==
          std::vector<std::string>{"mass", "massTone", "massTip", "rowState", "lockedBy"});
    CHECK(columnsWithCompanions(d, name) == std::vector<std::string>{"name", "rowState", "lockedBy"});
    CHECK(columnsWithCompanions(d, mass, {.tone = {"mass"}, .tooltip = {}, .rowTone = true}) ==
          std::vector<std::string>{"mass", "massTip", "lockedBy"});
    CHECK(companionColumns(d) == std::vector<std::string>{"lockedBy", "massTip", "massTone", "rowState"});
}
// Mutation: companions added for every column of the description, which puts massTone in `name`'s list.

TEST_CASE("description: a display travels in the description and changes its revision",
          "[table][description][display]") {
    // A description whose `mass` column has display {decimals 2, rounding HalfUp, labels [{"3","three"}]}:
    // its JSON holds "rounding":"halfUp" and "labels":[{"operand":"3","text":"three"}]; it reads back equal;
    // its revision differs from the same description with rounding HalfEven.
}
// Mutation: `Rounding` without glaze meta, which writes 1 for "halfUp".

TEST_CASE("description: a served table with a bad companion is refused", "[table][description][display]") {
    using namespace morph::table;
    auto const source = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, {{Cell{std::int64_t{1}}}});
    TableDescription d;
    d.id = "samples";
    d.key = "id";
    d.columns = {TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"}};
    ServedTable good{.description = d, .source = source, .levels = {}, .mayRead = {}, .scope = {}, .services = {},
                     .limits = {}};
    REQUIRE(detail::answerQuery(detail::pinned(good), TableQuery{}).has_value());
    ServedTable bad = good;
    bad.description.rowLock = "nope";
    auto const refused = detail::answerQuery(detail::pinned(bad), TableQuery{});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    CHECK(refused.error().column == "nope");
}
// Mutation: the validateDisplay call removed from answerQuery, which answers the query for `bad`.
```

Write the third case's assertions as the comment states (`CHECK(json.find("\"rounding\":\"halfUp\"") != npos)`, a read-back `==`, two `describe` revisions `!=`). The fourth is what shows `validateDisplay` is called: the first two pass with a `validateDisplay` no serving path ever reaches.

- [ ] **Step 2: Run them; they fail to compile** (`validateDisplay`, `TableColumn::display` not declared).

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile errors naming `validateDisplay` and `display`.

- [ ] **Step 3: Implement the description additions** in `include/morph/table/description.hpp` with the Interfaces block's signatures.
  - `validateDisplay` returns `TableError{.code = SourceContract, .column = <the offending id>}` for: `decimals` or `unit` set on a `Measure` column (column = that column); a `tone`, `tooltip`, `rowTone` or `rowLock` that names no column, or names a column whose kind is not `Text` (column = the named id).
  - Call `validateDisplay(served.description)` first in `detail::answerQuery` (before `validate`; its error is the query's), and in `DescribeTable<F>::invoke` beside the check that refuses a `rowScoped` table with an empty `scope` as `SourceContract` (Task 2; grep `SourceContract` in `include/morph/core/registry.hpp` and `description.hpp`), where it is refused the way that check refuses.
  - `columnsWithCompanions`: for each id of `columns` in order, the id, then its `tone` companion unless `overrides.tone` holds the id, then its `tooltip` companion unless `overrides.tooltip` holds it; then `rowTone` unless `overrides.rowTone`; then `rowLock`. Empty names are skipped; an id already listed is not listed again. `rowLock` travels with every row as `rowTone` does: a locked row cannot be drawn without it.
  - `companionColumns`: every non-empty `tone`, `tooltip`, `rowTone` and `rowLock` name, sorted, without duplicates.

- [ ] **Step 4: Run them; they pass.**

Run: `ctest --test-dir build/cl-debug -R "description:" --output-on-failure`
Expected: all `description:` cases pass.

- [ ] **Step 5: Write the failing formatter tests** in `tests/test_ui_cell_format.cpp` (add the file to `morph_tests`)

```cpp
namespace {
using morph::table::Cell;
using morph::table::ColumnKind;
using morph::table::Rounding;
using morph::table::TableColumn;

morph::math::Rational dec(std::string_view text) { return morph::table::parseDecimal(text).value(); }
TableColumn numeric(ColumnKind kind, morph::table::ColumnDisplay display) {
    return TableColumn{.info = {.id = "v", .kind = kind}, .label = "V", .display = std::move(display)};
}
// Part D's formatFor (tests/ui_table_support.hpp) fills the separators and patterns from the locale table. A
// default-constructed FormatContext has no group separator, so `{}` would show 12500 for a grouped column.
morph::ui::FormatContext en() { return formatFor("en"); }   // "," groups, "." decimal, ISO dates
morph::ui::FormatContext de() { return formatFor("de"); }   // "." groups, "," decimal, dd.MM.yyyy
}  // namespace

TEST_CASE("format cell: a column's places and rounding", "[ui][cell_format]") {
    using morph::ui::formatCell;
    CHECK(formatCell(Cell{dec("0.125")}, numeric(ColumnKind::Decimal, {.decimals = 2}), en()) == "0.12");
    CHECK(formatCell(Cell{dec("0.125")},
                     numeric(ColumnKind::Decimal, {.decimals = 2, .rounding = Rounding::HalfUp}), en()) == "0.13");
    CHECK(formatCell(Cell{dec("1.5")}, numeric(ColumnKind::Decimal, {.decimals = 2}), en()) == "1.50");
    CHECK(formatCell(Cell{dec("1.5")}, numeric(ColumnKind::Decimal, {.decimals = 2, .padPlaces = false}), en()) ==
          "1.5");
    CHECK(formatCell(Cell{dec("2")}, numeric(ColumnKind::Decimal, {.decimals = 2, .padPlaces = false}), en()) ==
          "2");
    CHECK(formatCell(Cell{std::int64_t{5}}, numeric(ColumnKind::Integer, {.decimals = 2}), en()) == "5.00");
    CHECK(formatCell(Cell{dec("12.50")}, numeric(ColumnKind::Decimal, {}), en()) == "12.50");  // own places
    CHECK(formatCell(Cell{0.1}, numeric(ColumnKind::Number, {}), en()) == "0.1");             // shortest
    CHECK(formatCell(Cell{std::int64_t{12500}}, numeric(ColumnKind::Integer, {.grouping = true}), en()) ==
          "12,500");
    CHECK(formatCell(Cell{std::int64_t{12500}}, numeric(ColumnKind::Integer, {}), en()) == "12500");
}
// Mutation: `rounding` ignored (always HalfEven), which shows "0.12" under HalfUp.

TEST_CASE("format cell: negative halves and empty text", "[ui][cell_format][review]") {
    using morph::ui::formatCell;
    auto const halfUp = numeric(ColumnKind::Decimal, {.decimals = 2, .rounding = Rounding::HalfUp, .empty = "–"});
    auto const halfEven = numeric(ColumnKind::Decimal, {.decimals = 2, .empty = "–"});
    CHECK(formatCell(Cell{dec("-0.125")}, halfUp, en()) == "-0.13");
    CHECK(formatCell(Cell{dec("-0.125")}, halfEven, en()) == "-0.12");
    CHECK(formatCell(Cell{dec("-0.001")}, halfEven, en()) == "0.00");  // zero shows no sign
    CHECK(formatCell(Cell{std::int64_t{0}}, numeric(ColumnKind::Integer, {.empty = "–"}), en()) == "0");
    CHECK(formatCell(Cell{std::monostate{}}, halfEven, en()) == "–");
    CHECK(formatCell(Cell{std::monostate{}}, numeric(ColumnKind::Decimal, {}), en()).empty());
    CHECK(formatCell(Cell{dec("-12500.5")}, numeric(ColumnKind::Decimal, {.decimals = 2, .grouping = true}), de()) ==
          "-12.500,50");
    CHECK(formatCell(Cell{dec("-12500.5")}, numeric(ColumnKind::Decimal, {.decimals = 2}), de()) == "-12500,50");
}
// Mutation: a zero value shown as `empty`, which gives "–" for 0.

TEST_CASE("format cell: a double rounds from its shortest decimal text", "[ui][cell_format][review]") {
    using morph::ui::formatCell;
    auto const halfUp = numeric(ColumnKind::Number, {.decimals = 2, .rounding = Rounding::HalfUp});
    CHECK(formatCell(Cell{1.005}, halfUp, en()) == "1.01");
    CHECK(formatCell(Cell{2.675}, halfUp, en()) == "2.68");
    CHECK(formatCell(Cell{0.125}, numeric(ColumnKind::Number, {.decimals = 2}), en()) == "0.12");
    CHECK(formatCell(Cell{1e21}, numeric(ColumnKind::Number, {}), en()) == "1000000000000000000000");
}
// Mutation: std::format("{:.2f}") on the binary value, which gives "1.00" for 1.005.

TEST_CASE("format cell: labels, dates, quantities and measures", "[ui][cell_format]") {
    using morph::ui::formatCell;
    auto const available = TableColumn{
        .info = {.id = "ok", .kind = ColumnKind::Bool},
        .display = {.labels = {{.operand = "true", .text = "available"}, {.operand = "false", .text = "not available"}}}};
    CHECK(formatCell(Cell{true}, available, en()) == "available");
    CHECK(formatCell(Cell{false}, available, en()) == "not available");
    auto const coded = numeric(ColumnKind::Integer, {.labels = {{.operand = "3", .text = "three"}}});
    CHECK(formatCell(Cell{std::int64_t{3}}, coded, en()) == "three");
    CHECK(formatCell(Cell{std::int64_t{4}}, coded, en()) == "4");

    auto const date = TableColumn{.info = {.id = "d", .kind = ColumnKind::Date}};
    CHECK(formatCell(Cell{std::int64_t{20'000}}, date, en()) == "2024-10-04");
    CHECK(formatCell(Cell{std::int64_t{20'000}}, date, de()) == "04.10.2024");
    auto const patterned = TableColumn{.info = {.id = "d", .kind = ColumnKind::Date}, .display = {.date = "dd-MMM-yyyy"}};
    CHECK(formatCell(Cell{std::int64_t{20'000}}, patterned, de()) == "04-Oct-2024");

    auto const grams = morph::table::QuantityCell{.amount = dec("250"), .toCanonical = dec("0.001")};
    CHECK(formatCell(Cell{grams}, numeric(ColumnKind::Quantity, {.decimals = 3}), en()) == "0.250");

    morph::table::UnitRegistryBuilder builder;
    REQUIRE(builder.add({.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}));
    REQUIRE(builder.add({.id = "g", .symbol = "g", .dimension = "mass", .toBase = dec("0.001"), .decimals = 0}));
    auto const registry = std::move(builder).build();
    morph::ui::DisplayUnits const units{.units = registry.get(), .byDimension = {}};
    auto const amount = TableColumn{.info = {.id = "amount", .kind = ColumnKind::Measure}};
    auto const kg = *registry->find("kg");
    auto const g = *registry->find("g");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("0.3"), .unit = kg}}, amount, en(), units) == "0.300 kg");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("200"), .unit = g}}, amount, en(), units) == "200 g");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("1.2345"), .unit = kg}}, amount, en(), units) ==
          "1.234 kg");
    morph::ui::DisplayUnits const inKg{.units = registry.get(), .byDimension = {{"mass", "kg"}}};
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("200"), .unit = g}}, amount, en(), inKg) == "0.200 kg");
}
// Mutation: a Measure shown at the column's `decimals` instead of its unit's, which gives "0.3 kg".

TEST_CASE("format cell: empty, unloaded and invalid differ", "[ui][cell_format]") {
    using morph::ui::CellLook;
    using morph::ui::cellLook;
    auto const integer = numeric(ColumnKind::Integer, {.empty = "–"});
    CHECK(cellLook(Cell{std::monostate{}}, integer) == CellLook::Empty);
    CHECK(cellLook(Cell{morph::table::Unloaded{}}, integer) == CellLook::Unloaded);
    CHECK(morph::ui::formatCell(Cell{morph::table::Unloaded{}}, integer, en()).empty());
    CHECK(cellLook(Cell{std::string{"abc"}}, integer) == CellLook::Invalid);
    CHECK(morph::ui::formatCell(Cell{std::string{"abc"}}, integer, en()).empty());
    CHECK(cellLook(Cell{std::string{"12"}}, integer) == CellLook::Text);
    CHECK(morph::ui::formatCell(Cell{std::string{"12"}}, numeric(ColumnKind::Integer, {.decimals = 1}), en()) ==
          "12.0");
    auto const amount = TableColumn{.info = {.id = "amount", .kind = ColumnKind::Measure}};
    CHECK(cellLook(Cell{morph::table::Measure{.amount = dec("1"), .unit = morph::table::kUnknownUnit}}, amount) == CellLook::Invalid);
}
// Mutation: `empty` shown for an unloaded cell, which gives "–".

TEST_CASE("format cell: values that show alike keep their places in the values list", "[ui][cell_format]") {
    std::vector<morph::table::DistinctValue> const values{
        {.text = {}, .operand = "0.124", .count = 1},
        {.text = {}, .operand = "0.123", .count = 2},
        {.text = {}, .operand = "0.5", .count = 1},
    };
    CHECK(morph::ui::distinctTexts(values, numeric(ColumnKind::Decimal, {.decimals = 2}), en()) ==
          std::vector<std::string>{"0.124", "0.123", "0.50"});
    CHECK(morph::ui::operandOf(Cell{dec("0.125")}, numeric(ColumnKind::Decimal, {.decimals = 2})) == "0.125");
    CHECK(morph::ui::operandOf(Cell{true}, TableColumn{.info = {.id = "b", .kind = ColumnKind::Bool}}) == "true");
    CHECK_FALSE(morph::ui::operandOf(Cell{std::monostate{}}, numeric(ColumnKind::Integer, {})).has_value());
    CHECK_FALSE(morph::ui::operandOf(Cell{morph::table::Unloaded{}}, numeric(ColumnKind::Integer, {})).has_value());
    CHECK(morph::ui::headerText("Mass", "kg") == "Mass [kg]");
    CHECK(morph::ui::headerText("Mass", "") == "Mass");
    CHECK(morph::ui::headerAccessibleName("Mass", "kg") == "Mass, kg");
}
// Mutation: the values list formatting every entry by `decimals`, which shows "0.12" twice.
```

`UnitRegistryBuilder::add(UnitInfo) -> std::expected<UnitId, TableError>`, `build() &&`, `UnitRegistry::find` and `kUnknownUnit` are Task 8c's (Part C). `formatFor(tag)` is Part D's (Task 9a, `tests/ui_table_support.hpp`); when this task lands before Task 9, it adds the header with that one function, written as Part D writes it (`LocaleFacts const& f = ui::localeFacts(tag);` then `FormatContext{.locale = std::string{tag}, .numeric = f.numeric, .currency = f.currency, .datePattern = std::string{f.datePattern}, .dateTimePattern = std::string{f.dateTimePattern}}`), and Task 9 extends the file.

`DisplayUnits::units` is not owned. Part C's `table::registryOf(description)` returns `std::expected<std::shared_ptr<UnitRegistry const>, TableError>`; the lowering calls it once per description revision and keeps the `shared_ptr` beside the table's description for as long as that revision is held, handing `registry.get()` to every `DisplayUnits` it builds. When `registryOf` fails, the lowering reports the error once for the table and passes `units = nullptr`, under which a `Measure` cell is `Invalid`. `byDimension` is the column's entry of Task 8d's `ColumnLayoutValue::units`.

- [ ] **Step 6: Run them; they fail to compile** (`morph/ui/cell_format.hpp` missing).

Run: `cmake --build --preset cl-debug --target morph_tests`
Expected: `cannot open include file 'morph/ui/cell_format.hpp'`.

- [ ] **Step 7: Implement `include/morph/ui/cell_format.hpp`** with the Interfaces block's signatures. The rules the tests do not fix:
  - **The number path.** Every numeric cell becomes canonical decimal text (`-?digits(.digits)?`), is rounded on that text, then written by `render::formatCanonicalNumber` with `locale.numeric` when `display.grouping`, else `ui::withoutGrouping(locale.numeric)`. A `Rational` with `decimals` set rounds exactly with `math::checkedRoundToDecimalPlaces(value, DecimalPlaces{d}, mode)` (`HalfUp` → `RoundingMode::HalfAwayFromZero`), a failed round is `CellLook::Invalid`; without `decimals` it keeps its declared places (`units::detail::formatRationalDecimal`). A `double` is written with `std::to_chars(…, std::chars_format::fixed)` (its shortest round-trip text), then rounded as text. An `int64` is its digits. A `QuantityCell` is `amount × toCanonical`, multiplied with checked arithmetic (an overflow is `Invalid`).
  - **Rounding text** (one private helper, used for `double` and padding): keep `d` fraction digits; the dropped tail decides: above half rounds the magnitude up, below keeps it, exactly half rounds up under `HalfUp` and to the even last kept digit under `HalfEven`. Then `padPlaces` pads to `d` digits, or else trailing zeros and a bare point are removed. A result whose digits are all zero has no sign.
  - **Order of decisions.** `Unloaded` → `""`. `std::monostate` → `display.empty`. A `std::string` in an `Integer`, `Key`, `Decimal`, `Quantity`, `Number` or `Bool` column is read for the kind (`std::from_chars`, `table::parseDecimal`, `"true"`/`"false"`); text that does not read is `Invalid` and shows `""`; text that reads is formatted as the value it reads as. A string in a `Date`, `DateTime`, `Text` or `Custom` column shows as it is. Then `labels`: the first label whose `operand` equals `operandOf(cell)` gives the text (a `Decimal` or `Number` operand is compared by value: `"3"` matches 3.0). Then the kind's own text.
  - **Dates.** `Date` is days since 1970-01-01, `DateTime` UTC seconds shifted by `locale.utcOffsetMinutes`; the pattern is `display.date`, else `locale.datePattern` / `locale.dateTimePattern`, else ISO (`2024-10-04`, `2024-10-04 08:30`); written by `ui::formatDate(pattern, local, locale.utcOffsetMinutes)`, `local` being the `time::DateTime` of the days (at midnight) or of the shifted seconds. An empty result (a pattern `checkDatePattern` refuses) falls back to ISO.
  - **Measures.** The unit's `decimals` and `symbol` from `units.units`; with `units.byDimension` naming a unit of the cell's dimension, the amount is converted exactly (`amount × from.toBase / to.toBase`) and rounded `HalfEven` to that unit's decimals. Text is the number, one ASCII space, the symbol; the ratio unit (`""`) writes no space and no symbol. No registry, or a unit the registry does not know, is `Invalid`.
  - `operandOf`: `int64` → its digits; `Rational` → `formatRationalDecimal` at its own places; `double` → shortest round-trip text; `bool` → `"true"`/`"false"`; `std::string` → itself; `QuantityCell` → its canonical amount; `Measure` → amount, a space, the unit's id; empty, unloaded and invalid → `std::nullopt`. `cellOfOperand` is its inverse for the column's kind.
  - `distinctTexts`: each operand is read with `cellOfOperand` and formatted; when two entries' texts are equal, each of those entries is formatted again with `display.decimals` cleared (its own places).

- [ ] **Step 8: Run them; they pass.**

Run: `ctest --test-dir build/cl-debug -R "format cell:" --output-on-failure`
Expected: 6 test cases pass.

- [ ] **Step 9: Write the failing lowering tests** in `tests/test_ui_table_model.cpp`. Each mounts a model-backed table over `companionDescription()`: the columns `id` (Integer, key), `mass` (Decimal, `display = {decimals 2, rounding HalfUp, unit "kg"}`, `tone = "massTone"`, `tooltip = "massTip"`), `name` (Text), `count` (Integer), the companions `massTone`, `massTip` and `lockedBy` (Text, `shown: false`) and `rowLock = "lockedBy"`. `kCompanionPage` is the page JSON of one row, written in the test with the description's revision: `{id 1, mass 0.125, name "A", count "abc", massTone "warn", massTip "Above limit", lockedBy ""}` (`count` holds text, an invalid cell). `kTable` is the node `{"kind":"table","id":"t","testId":"t","model":"sample","table":"samples"}`.

```cpp
using morph::ui::testing::TableHarness;

TEST_CASE("table display: a model-backed table draws its description's display", "[ui][table][display]") {
    TableHarness h;
    h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, kCompanionPage);
    CHECK(cellText(h, 1, "mass") == "0.13");
    CHECK(cellAccent(h, 1, "mass") ==
          morph::ui::CellAccent{.tone = morph::ui::Tone::Warn, .tooltip = "Above limit", .invalid = false});
    CHECK(cellAccent(h, 1, "name") == morph::ui::CellAccent{});
    CHECK(columnOf(h, "mass").unit == "kg");
}
// Mutation: the default cell's text left on the stand-in formatter, which shows "0.125".

TEST_CASE("table display: an invalid cell draws its mark and no text", "[ui][table][display]") {
    TableHarness h;
    h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, kCompanionPage);
    CHECK(cellText(h, 1, "count").empty());
    CHECK(cellAccent(h, 1, "count") ==
          morph::ui::CellAccent{.tone = std::nullopt, .tooltip = "The value could not be read", .invalid = true});
    CHECK_FALSE(cellAccent(h, 1, "mass").invalid);
}
// Mutation: the accent built from the companions only (cellLook not read), which leaves `invalid` false.

TEST_CASE("table display: companions are fetched with their column only", "[ui][table][display]") {
    {
        TableHarness h;
        h.mount(doc(R"({"kind":"table","id":"t","model":"sample","table":"samples","columns":[{"id":"name"}]})"));
        replyDescribe(h, describeJson(companionDescription()));
        CHECK(lastQuery(h).columns == std::vector<std::string>{"id", "name", "lockedBy"});
    }
    {
        TableHarness h;
        h.mount(doc(R"({"kind":"table","id":"t","model":"sample","table":"samples",
                        "columns":[{"id":"mass","tone":{"lit":"ok"}}]})"));
        replyDescribe(h, describeJson(companionDescription()));
        CHECK(lastQuery(h).columns == std::vector<std::string>{"id", "mass", "massTip", "lockedBy"});
        replyPage(h, kCompanionPage);
        CHECK(cellAccent(h, 1, "mass").tone == morph::ui::Tone::Ok);   // the document's, not the companion's "warn"
    }
}
// Mutation: companions fetched for every described column, which puts massTone in the first query.

TEST_CASE("table display: companion columns are not arranged", "[ui][table][display]") {
    TableHarness h;
    h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, kCompanionPage);
    CHECK(elementValue(h, "t", "visibleColumns") == value(R"(["id","mass","name","count"])"));
    CHECK(elementValue(h, "t", "hiddenCount") == morph::ui::Value{std::int64_t{0}});
    // Part E's column chooser (`Columns…`, dialog `table_columns`) has table_columns_show_id, _mass, _name and
    // _count, and no table_columns_show_massTone, _massTip or _lockedBy.
}
// Mutation: the chooser listing every column of the description, which lists table_columns_show_massTone.

TEST_CASE("table display: a document's parts override the description", "[ui][table][display]") {
    TableHarness h;
    h.mount(doc(R"({"kind":"table","id":"t","model":"sample","table":"samples",
                    "columns":[{"id":"mass","decimals":1,"rounding":"halfEven","empty":"n/a"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, kCompanionPage);
    CHECK(cellText(h, 1, "mass") == "0.1");
}
// Mutation: document parts read only on a `rows` table, which keeps "0.13".

TEST_CASE("table display: an unknown tone is reported once for its column", "[ui][table][display]") {
    // Two rows whose massTone is "purple": cellAccent(h, key, "mass").tone is empty for both, and `reportsOf(h)`
    // holds exactly one entry, "column `mass`'s tone is \"purple\"; a tone is neutral, info, ok, warn or err".
}
// Mutation: reporting per row, which reports twice.

TEST_CASE("table display: copy writes the shown text and the header with its unit", "[ui][table][display]") {
    // A node with "selectionMode":"multiple" and "columns":[{"id":"id"},{"id":"mass"},{"id":"name"}]; select row 1,
    // "Copy with headers": clipboardText() == "Id\tMass [kg]\tName\n1\t0.13\tA".
}
// Mutation: copy writing displayText, which writes 0.125.

TEST_CASE("table display: text never orders", "[ui][table][display]") {
    // A `rows` table over [{id:1, v:10}, {id:2, v:9}] with column v {"decimals":2,"sortable":true}, sorted asc:
    // cellText reads "9.00" then "10.00" by view row. With the node's `filters` on v `eq "0.13"` (the literal shape
    // Task 9d's `filters` key reads) over a row {v: 0.125} shown as "0.13" (decimals 2, rounding halfUp):
    // elementValue(h, "t", "viewCount") == 0. A Bool column labelled "available"/"not available", sorted asc: the
    // row with false comes first.
}
// Mutation: a sort over the formatted text, which orders "10.00" before "9.00".
```

`companionDescription()` is a function in the test file returning the `TableDescription` above; every case answers the describe request with it through `replyDescribe(h, json)`. Write the commented cases' assertions exactly as stated.

- [ ] **Step 10: Write the failing backend tests.** In `tests/qt_quick/test_qt_quick_table.cpp`: `"qt_quick table: a toned cell keeps its selection mark, a tooltip cell is described, a unit header is named with its unit"` — a table with columns `{label "Mass", unit "kg"}` and one row whose cell gets `CellAccent{Tone::Warn, "Above limit"}`, selected: the cell item's tint is the theme's `warn` surface and the row's selection mark is still drawn; the cell's `Accessible.description` is `"Above limit"` and it shows the "more here" mark; the header item's `Accessible.name` is `"Mass, kg"`; a second cell given `CellAccent{.tone = {}, .tooltip = "The value could not be read", .invalid = true}` draws the theme's error mark with that text as its tooltip and accessible description, and after `setCellAccent(row, column, CellAccent{})` draws neither. Mutations: the tint painted over the selection mark; `invalid` ignored by `MorphTable.qml`, which draws no error mark. In `tests/tui/test_tui_table.cpp`: `"tui table: a header with a unit reads Mass [kg]"` — the header row's text holds `Mass [kg]`; a cell given an `invalid` accent draws the terminal's error glyph in place of its text. Mutations: `unit` ignored; `invalid` ignored.

- [ ] **Step 11: Run them; they fail** (`unit`, `setCellAccent` undeclared).

Run: `cmake --build --preset cl-debug --target morph_tests` and `cmake --build build/cl-qt-debug --target morph_qt_quick_tests morph_tui_tests`
Expected: compile errors naming `setCellAccent`, `unit`, `CellAccent`.

- [ ] **Step 12: Implement the view, mount, backends and lowering.**
  - `view.hpp`/`backend.hpp`/`mount.hpp`: the members in the Interfaces block and the mount rule under it ("Cells, not rows, carry the accent"): an `AccentedCell` is unwrapped where a cell is mounted, its `accent` bound in the cell's own scope and sent with `setCellAccent(row, column, accent)`, `column` an index into `Table::columns`. `ui::TableColumn::unit` goes to `setColumns`. The recording backend records both, the accent per row key and column.
  - Qt Quick (`src/qt_quick/table.cpp`, `MorphTable.qml`): a toned cell is filled with the theme's tone surface under the selection and focus marks; an `invalid` cell draws the theme's error mark in place of its text; a cell with a tooltip draws the theme's "more here" mark, shows the tooltip on hover after the theme's delay and on keyboard focus at once, and carries it as `Accessible.description`; a header with a unit draws the unit on a second, muted line and has `Accessible.name` `headerAccessibleName(label, unit)`.
  - Terminal (`src/tui/table_widget.cpp`): a header is `headerText(label, unit)`; a toned cell uses the tone's colour; an `invalid` cell draws the error glyph; a tooltip is shown in the status area while the cell is focused.
  - `src/ui/interpreter.cpp`: a column part's `decimals`, `rounding`, `padPlaces`, `grouping`, `empty`, `unit`, `date` and `labels` (literals) build a `table::ColumnDisplay` that overrides the description's per member; on a table over `rows` they are its only source. A part's `tone` and `tooltip` are expressions in the row's scope and win over the description's companions; the lowering then passes them in `CompanionOverrides`. The default cell's text is `formatCell(cell, column, scope->formatting(), displayUnits)`. `cellView(column, rowSignal)` returns `accentedCell(content, accent)` for every column, whether its content is the default cell or the column's `cell` template. `accent` is a binding in the cell's row scope that gives a `CellAccent`: `tone` from the part's `tone` expression, else the tone the companion's field of the row's record names (`neutral`, `info`, `ok`, `warn`, `err`; anything else is none, reported once for the column); `tooltip` from the part's `tooltip` expression, else the companion's field; and, when `cellLook(cell, column, displayUnits)` is `CellLook::Invalid`, `invalid = true` with the tooltip `"The value could not be read"` in place of any other. A column with no tone, no tooltip and a valid cell has the accent `CellAccent{}`. An unloaded cell's placeholder is Task 9d's `setCellLoad`, from the column's level, and no part of the accent. The fetched column set is `columnsWithCompanions(description, windowColumns, overrides)`. The columns of `companionColumns(description)` are left out of the `ColumnLayout` the lowering builds, the column chooser and the header menus. An unknown tone name is reported once per column with the message in the test. A `Measure` column with no `align` part aligns to the end, with digits of one width, as an `end` column does ("Values with units › Display").
  - **Task 9's stand-in.** Task 9 formats a model-backed table's cells through a stand-in until this task lands. It is `ui::detail::standInCellText(table::Cell const&, table::TableColumn const&)` in `include/morph/ui/table_rows.hpp` (index decision 9). Find every call with `grep -rn "standInCellText" include src tests`, replace each (Part E's copy among them) with `formatCell(cell, column, scope->formatting(), displayUnits)`, and delete the stand-in; then the same grep prints nothing. Also `grep -n "displayText(\|textOf(" src/ui/interpreter.cpp`: a call that turns a table cell into its shown text goes through `formatCell` too. If Task 9 has not landed, Task 9 calls `formatCell` directly and this bullet does nothing.
  - Part E's copy (Task 12f) writes each cell with `formatCell` and each label with `headerText(label, unit)` before `ui::copyText(labels, rows)` and `IViewBackend::copyText`. Part E's values list (Task 12d, dialog `table_filter`) labels `table_filter_value_<i>` with `distinctTexts(values, column, locale, units)[i] + " (" + count + ")"` in place of the reply's `text`.
  - `src/ui/document.cpp`: `rounding` is a literal `"halfEven"` or `"halfUp"`; `decimals` a literal integer 0 to 18; `labels` a literal list of `{operand, text}`; each other value is reported at its path as the existing column parts are.

- [ ] **Step 13: Run the task's tests and their targets; all pass.**

Run: `ctest --test-dir build/cl-debug -R "format cell:|description:|table display:" --output-on-failure` and `ctest --test-dir build/cl-qt-debug -R "qt_quick table:|tui table:" --output-on-failure`, then the whole `morph_tests`, `morph_table_tests`, `morph_qt_quick_tests` and `morph_tui_tests`.
Expected: all pass.

- [ ] **Step 14: Mutations.** Apply each mutation named above by hand, run its test, see it fail, revert; keep each failing line for the commit body.

- [ ] **Step 15: Docs.** `docs/spec/ui/document.md`: the `table` row gains a column's parts `decimals`, `rounding`, `padPlaces`, `grouping`, `empty`, `unit`, `date`, `labels`, `tone`, `tooltip`, each in one clause with its rule from "How a column shows its cells". `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md`: `setCellAccent` and a column's `unit`. Run the docs build; it is green.

- [ ] **Step 16: Commit.**

```bash
git add include/morph/table/description.hpp include/morph/ui/cell_format.hpp include/morph/ui/view.hpp \
        include/morph/ui/backend.hpp include/morph/ui/mount.hpp include/morph/ui/testing/recording_backend.hpp \
        src/ui/interpreter.cpp src/ui/document.cpp src/qt_quick src/tui tests docs/spec
git commit -m "feat(table): column display, tone and tooltip"
```

---

## Task 17: Acting on rows (table.md step 9b)

Needs Tasks 9 and 15. Four commits, each its own reviewer gate.

### Task 17a: A selection of the view

**Takes over Ctrl+A and the select-all box.** Until this task, Ctrl+A and the select-all box over a source table go to `setOnSelectRange` over every row of the view (Task 10b), which the lowering refuses past `SessionOptions::maxRangeRows`. That refusal is interim: this task routes both, in Qt Quick and the terminal, to the whole-view selection (`setOnSelectAll`), which needs no keys and has no such bound, and removes the range path for them.

**Spec:** "A selection of the view" (2264–2357); test rows "A selection of the view", "Resolving from elsewhere", "The view revision is membership", "Copy of a selection of the view"; invariants 33, 34; failure modes 3314–3315.

**Files:**
- Create: `include/morph/table/row_selection.hpp` — `RowSelection<fn>`, `SelectionPolicy`, `SelectedRows`, `ResolveRequest<fn>`, `RowRef<fn>`, `resolve`, `detail::membership`.
- Modify: `include/morph/table/data_source.hpp` (`TableErrorCode::ViewChanged`, wire name `"viewChanged"` in the enum's glaze meta at `data_source.hpp:934`), `include/morph/table/query.hpp` (`QueryLimits::maxSelectedRows`; `Page::viewRevision`; `PageRows::viewRevision`, filled by `apply`), `include/morph/table/view_cache.hpp` (`MembershipDigest`; the entry keeps the digest), `include/morph/table/description.hpp` (`GenericPage::viewRevision`, written and read), `include/morph/core/registry.hpp` (`BRIDGE_REGISTER_TABLE` adds `Table.<id>.resolve`), `include/morph/forms/forms.hpp` (`DeclaresSchemaValue`).
- Modify: `include/morph/ui/table_rows.hpp` (`ViewSelection`, `TableSelection`, `CountKind`, `SelectedCount`, `SelectAllRefusal`, the `TableRows` members below, `selectAllRefusalText`), `include/morph/ui/view.hpp` (`StatusEntry`; `TableStatus::{entries, standing, standingEntries}`; `onSelectAll`, `selectionView` and `onStatusEntry` on `ui::Table` and `TableOptions`), `include/morph/ui/backend.hpp` (`TableWidget::setOnSelectAll`, `setSelectionView`, `setOnStatusEntry`), `include/morph/ui/mount.hpp`, the recording, Qt Quick and terminal table widgets, `include/morph/ui/interpreter.hpp` and `src/ui/interpreter.cpp` (`RunObserver`, `Session::addRunObserver`/`removeRunObserver`, `TableInput`, `tableInputsOf`; the model-backed table's selection, element values `selectionSet` and `selectedCountKind`; the `viewChanged` refusal and its "Run on these" entry; Task 12's copy of a view selection).
- Test: `tests/table/test_table_selection.cpp`, `tests/net/test_push_tables.cpp`, `tests/test_ui_table_model.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_table.cpp`.
- Docs: `docs/spec/ui/document.md` (element values `selectionSet`, `selectedCountKind`), `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md` (`setOnSelectAll`, `setSelectionView`, `setOnStatusEntry`).

**Interfaces:**
- Consumes: `ServedTable`, `TableContext`, `TableOf<fn>`, `BRIDGE_REGISTER_TABLE`, `table::scoped` (Task 2); `Match`, `CompiledFilter::evaluate` (Task 3); `TableRuntime`, `currentTableRuntime`, `TableRuntimeScope` (Task 2; `views` from Task 6), `detail::answerQuery` and `AnswerContext` (Tasks 2 and 6); `ViewCache` and its entry for a validated sort and filter (Task 6); `TableRows`, `WindowRows`, `EngineRows`, `TableCountsView` (Task 9); `TableRows::keysOf` and range selection in server mode (Task 10); the copy path (Task 12f) and `ScreenCore::tableMessage` (Task 9d); `model::ActionRefusal` (`include/morph/core/action_refusal.hpp`); `model::recordRegistry()` (`include/morph/core/records.hpp:189`).
- Produces:

```cpp
namespace morph::table {
template <auto Function>
struct RowSelection {
    struct View {
        FilterSpec filters;
        std::vector<RowId> retained;
        std::string revision;
    };
    std::vector<RowId> keys;
    std::optional<View> view;
    std::vector<RowId> except;
    /// The schema keyword and its value: {"x-selectionOf", "<Model>.<id>"}.
    [[nodiscard]] static std::pair<std::string_view, std::string> schemaValue();
};
enum class SelectionPolicy : std::uint8_t { Exact, Current };                 // JSON "exact", "current"
struct SelectedRows { std::vector<RowId> keys; std::int64_t dropped = 0; };
template <auto Function>
struct ResolveRequest { RowSelection<Function> selection; SelectionPolicy policy = SelectionPolicy::Exact; };
template <auto Function>
struct RowRef {                                                               // JSON: the key itself
    RowId key;
    [[nodiscard]] static std::pair<std::string_view, std::string> schemaValue();   // {"x-rowOf", record}
};
template <auto Function, typename Model>
[[nodiscard]] async::Completion<std::expected<SelectedRows, TableError>>
resolve(Model& model, RowSelection<Function> const& selection, TableContext const& context, SelectionPolicy policy);
// `Model&`, as table.md declares it (index decision 23): table functions are non-const, and `resolve` reaches
// one through Task 2's table::detail::serve<Function>(model, context), which also handles a typed<Row> table.

// Consumed, not defined here (index decision 6): Task 2's TableRuntime{exec::IExecutor* work; ViewCache* views /* Task 6 */},
// currentTableRuntime() and TableRuntimeScope.

class MembershipDigest {                                  // view_cache.hpp
public:
    void add(RowId const& key) noexcept;
    [[nodiscard]] std::string text() const;               // 48 lowercase hex digits
};
// QueryLimits: std::int64_t maxSelectedRows = 1'000'000;
// Page<Row>, PageRows, GenericPage: std::string viewRevision;
// TableErrorCode::ViewChanged ("viewChanged")
namespace detail {
struct Membership { std::vector<std::uint32_t> rows; MembershipDigest digest; };   // rows in source order
[[nodiscard]] std::expected<Membership, TableError>
membership(ServedTable const& served, FilterSpec const& filters, std::span<RowId const> retained,
           ViewCache* views);
}
}  // namespace morph::table

namespace morph::ui {
struct ViewSelection { table::FilterSpec filters; std::vector<table::RowId> retained; std::string revision; };
struct TableSelection {                               // JSON as RowSelection's
    std::vector<table::RowId> keys;
    std::optional<ViewSelection> view;
    std::vector<table::RowId> except;
};
enum class CountKind : std::uint8_t { Exact, Approximate };   // element value "exact" / "approximate"
struct SelectedCount { std::int64_t rows = 0; CountKind kind = CountKind::Exact; };
struct SelectAllRefusal { std::int64_t undecided = 0; std::vector<std::string> levels; };
// TableRows gains (pure virtual; EngineRows and WindowRows implement):
//   [[nodiscard]] virtual std::expected<ViewSelection, SelectAllRefusal> selectView() const = 0;
//   [[nodiscard]] virtual SelectedCount selectedCount(TableSelection const& selection) const = 0;
//   virtual async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>>
//       rowsOf(std::int64_t first, std::int64_t count, std::span<std::string const> columns) = 0;
[[nodiscard]] std::string selectAllRefusalText(SelectAllRefusal const& refusal, FormatContext const& locale);
enum class TableInputKind : std::uint8_t { Selection, Row };
struct TableInput { std::string member; std::string record; TableInputKind kind; };
[[nodiscard]] std::vector<TableInput> tableInputsOf(std::string_view schemaJson);
struct RunStarted { std::uint64_t run; std::string const& action; Value const& body; };
struct RunSettled {
    std::uint64_t run; std::string const& action; Value const& body; bool succeeded; Value const& reply;
    std::string const& refusalCode;                 // the ActionRefusal code, empty for none
    std::function<void(Value body)> rerun;          // runs the same mutation again with another body
};
class RunObserver {
public:
    virtual ~RunObserver() = default;
    virtual void started(RunStarted const& run) = 0;
    virtual void settled(RunSettled const& run) = 0;
};
// Session gains: void addRunObserver(RunObserver* observer); void removeRunObserver(RunObserver* observer);
// StatusEntry (view.hpp): std::string id; std::string label; bool operator==(StatusEntry const&) const = default;
// ui::Table and TableOptions both gain (index decision 41; the lowering fills both):
//   std::function<void()> onSelectAll;                     // set: the mount calls setOnSelectAll with it
//   Prop<std::optional<std::vector<Key>>> selectionView;   // engaged: the selection is the view less these keys;
//                                                          // the mount calls setSelectionView on each change, and
//                                                          // setSelection(selection) when it disengages
//   std::function<void(std::string id)> onStatusEntry;     // a status-line entry the user chose, by its id
// TableWidget gains: virtual void setOnSelectAll(std::function<void()> onSelectAll) = 0;
//                    virtual void setSelectionView(std::vector<Key> const& except) = 0;
//                    virtual void setOnStatusEntry(std::function<void(std::string id)> onStatusEntry) = 0;
}  // namespace morph::ui
```

- [ ] **Step 1: Write the failing server tests** in `tests/table/test_table_selection.cpp`. The file's new cases share one model:

```cpp
namespace {
using namespace morph::table;
// Ten rows, keys 0..9: status "open" for 0..5, "closed" for 6..9; mass i/10.
struct SelectionModel {
    std::shared_ptr<VectorSource> source = tabletest::makeSource(
        {tabletest::col("status", ColumnKind::Text), tabletest::col("mass", ColumnKind::Decimal)},
        [] {
            std::vector<std::vector<Cell>> rows;
            for (int i = 0; i < 10; ++i) {
                rows.push_back({Cell{std::string{i < 6 ? "open" : "closed"}},
                                Cell{tabletest::dec(std::to_string(i) + ".0")}});
            }
            return rows;
        }());
    QueryLimits limits{};
    [[nodiscard]] ServedTable rows(TableContext const& /*context*/) {   // non-const, as the spec's table functions
        return ServedTable{.description = {}, .source = source, .levels = {}, .mayRead = {}, .scope = {},
                           .services = {}, .limits = limits};
    }
};
FilterSpec open() { return FilterSpec{.columns = {{"status", ColumnFilter{.include = {{.eq = "open"}}}}}}; }
// TableContext holds a reference: one caller context outlives every call in the file.
TableContext const& caller() {
    static morph::session::Context const session{};
    static TableContext const context{.session = session};
    return context;
}
// resolve with no TableRuntime settles on an InlineExecutor before it returns: `then` runs at once.
template <typename T> T settledNow(morph::async::Completion<T>& completion);   // `then` into an optional, REQUIRE set
std::string revisionOf(SelectionModel& model, FilterSpec const& filters, std::vector<RowId> retained = {}) {
    return detail::membership(model.rows(caller()), filters, retained, nullptr).value().digest.text();
}
}  // namespace

TEST_CASE("view selection: the view revision is membership", "[table][selection][revision]") {
    SelectionModel model;
    std::string const before = revisionOf(model, open());
    model.source->patchRow(RowId{std::int64_t{2}}, {Cell{std::string{"open"}}, Cell{tabletest::dec("9.5")}});
    CHECK(revisionOf(model, open()) == before);                       // a cell changed, membership did not
    TableQuery ascending{.sort = {{.column = "mass", .dir = SortDirection::Ascending}}, .filters = open()};
    TableQuery descending{.sort = {{.column = "mass", .dir = SortDirection::Descending}}, .filters = open()};
    CHECK(apply(*model.source, ascending).value().viewRevision == before);
    CHECK(apply(*model.source, descending).value().viewRevision == before);  // the sort is not membership
    model.source->patchRow(RowId{std::int64_t{7}}, {Cell{std::string{"open"}}, Cell{tabletest::dec("0.7")}});
    CHECK(revisionOf(model, open()) != before);                       // a row came in
    CHECK(revisionOf(model, open(), {RowId{std::int64_t{8}}}) != revisionOf(model, open()));
}
// Mutation: the revision taken from the snapshot's version, which changes it on the first edit.

TEST_CASE("view selection: resolve under Exact is the view less except, in source order",
          "[table][selection][resolve]") {
    SelectionModel model;
    RowSelection<&SelectionModel::rows> selection{
        .keys = {}, .view = {{.filters = open(), .retained = {}, .revision = revisionOf(model, open())}},
        .except = {RowId{std::int64_t{3}}}};
    auto exact = resolve(model, selection, caller(), SelectionPolicy::Exact);
    auto const rows = settledNow(exact).value();
    CHECK(rows.keys == std::vector<RowId>{std::int64_t{0}, std::int64_t{1}, std::int64_t{2}, std::int64_t{4},
                                          std::int64_t{5}});
    CHECK(rows.dropped == 0);

    model.source->patchRow(RowId{std::int64_t{7}}, {Cell{std::string{"open"}}, Cell{tabletest::dec("0.7")}});
    auto changed = resolve(model, selection, caller(), SelectionPolicy::Exact);
    CHECK(settledNow(changed).error().code == TableErrorCode::ViewChanged);
    auto current = resolve(model, selection, caller(), SelectionPolicy::Current);
    CHECK(settledNow(current).value().keys ==
          std::vector<RowId>{std::int64_t{0}, std::int64_t{1}, std::int64_t{2}, std::int64_t{4}, std::int64_t{5},
                             std::int64_t{7}});
}
// Mutation: `viewRevision` that ignores the source snapshot (a digest of the filter alone), which resolves the
// edited view under Exact.

TEST_CASE("view selection: a key selection resolves to the keys the caller sees", "[table][selection][resolve]") {
    SelectionModel model;
    // The caller's source is rows 0..4 only, through Task 2's table::scoped.
    // RowSelection{.keys = {3, 1, 8}}: keys == {1, 3} (source order), dropped == 1.
}
// Mutation: dropped keys left in the reply, which returns 8.

TEST_CASE("view selection: a view over maxSelectedRows is LimitExceeded", "[table][selection][resolve]") {
    CHECK(QueryLimits{}.maxSelectedRows == 1'000'000);
    SelectionModel model;
    model.limits.maxSelectedRows = 5;  // the open view has 6 rows
    // resolve of the open view with except {3} under Current: error().code == TableErrorCode::LimitExceeded
    // (the bound is on the view, 6 rows, not on what is left of it, 5).
    // With maxSelectedRows = 6 the same selection resolves to {0,1,2,4,5}.
}
// Mutation: the bound compared against the view less `except` (5 rows), which resolves the first case.

TEST_CASE("view selection: retained rows are in the membership", "[table][selection][resolve]") {
    // A view selection of open() with retained {8} and the revision revisionOf(model, open(), {8}):
    // under Exact, keys == {0,1,2,3,4,5,8}. Retained {42}, a key the source lacks, adds nothing.
}
// Mutation: retained rows left out of `membership`, which refuses with ViewChanged.

TEST_CASE("view selection: its JSON", "[table][selection]") {
    // {"keys":[1,"a"]} reads as two keys and no view; {"view":{"filters":{"columns":{"status":{"include":
    // [{"eq":"open"}]}}},"retained":[8],"revision":"r"},"except":[3]} round-trips byte for byte through glaze;
    // SelectionPolicy writes "exact" and "current".
}
// Mutation: `SelectionPolicy` without glaze meta, which writes 0 and 1.
```

Write the commented cases' assertions exactly as their comments state.

**The table is registered.** `resolve` reaches the table through Task 2's `detail::serve<Function>`, which reads `TableTraits<Function>` (its `row`, to tell a plain table from a `typed<Row>` one), and the primary `TableTraits` has only `registered = false`: an unregistered function does not compile there. So the model is not in the anonymous namespace: it is `seltest::SelectionModel`, with at file scope `BRIDGE_REGISTER_TABLE(seltest::SelectionModel, "rows", &seltest::SelectionModel::rows)` and the model registration Part A's own `tests/table` cases use for a model with a table (Task 2); the anonymous namespace keeps the helpers and a `using seltest::SelectionModel;`.

- [ ] **Step 2: Run them; they fail to compile** (`row_selection.hpp` missing).

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: `cannot open include file 'morph/table/row_selection.hpp'`.

- [ ] **Step 3: Implement the server side.**
  - **`MembershipDigest`.** Membership is a set of keys, so the digest does not depend on the order it is taken in, and a retained row merged into a reply is one more `add`. For each key, `h = fnv1a64(k)` (`morph::model::detail::fnv1a64`, `include/morph/core/payload_schema.hpp:213`) over `"i:" + decimal` for an integer key and `"s:" + text` for a text key, and `m = splitmix64(h)` (`z = h + 0x9E3779B97F4A7C15; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9; z = (z ^ (z >> 27)) * 0x94D049BB133111EB; z ^ (z >> 31)`). The digest keeps `count`, `sum += h` and `mix += m`, all modulo 2^64; `text()` is the three as 16 lowercase hex digits each, in that order. A caller adds each key once.
  - **Where it is taken.** The pass that computes a view in `apply` (and the view cache's computation, Task 6) adds the key of each row it puts in the view; the cache entry stores the `MembershipDigest` beside its order; `PageRows`, `Page<Row>` and `GenericPage` carry `viewRevision = digest.text()`, page and keyed replies alike. A distinct-values reply leaves it empty.
  - **`detail::membership`.** Takes the view of `filters` with an empty sort from `views` when one is given (the entry under that key, computed by the cache's single flight when absent), else computes it with `apply`; with an empty sort the view's order is source order. Each key of `retained` the source has and the view lacks is merged in at its source row and added to a copy of the digest. A view whose size after merging is above `served.limits.maxSelectedRows` is `LimitExceeded`.
  - **`resolve`.** It takes `Model&` (as table.md declares it, index decision 23) and gets the `ServedTable` with Task 2's `table::detail::serve<Function>(model, context)` on the caller's thread (the model's strand), so the caller's scope and `mayRead` apply and a `typed<Row>` table works as a plain one. The rest runs on `currentTableRuntime()->work` with `currentTableRuntime()->views`. The returned `Completion` comes from `Completion<…>::makeSettleable(exec)` (`include/morph/core/completion.hpp:738`), `exec` being the runtime's `work`; with no runtime, or a null `work`, the work runs inline with no view cache and `exec` is a function-local static `exec::InlineExecutor` (`include/morph/core/executor.hpp:384`), so the promise is settled and `then` runs before `resolve` returns. Tests rely on that: `settledNow` attaches `then` and `REQUIRE`s the value at once.
  - **The schema.** `forms.hpp` gains `DeclaresSchemaValue<T>` (`{ T::schemaValue() } -> std::convertible_to<std::pair<std::string_view, std::string>>`), and `annotateSchemaMarker<Member>` also writes `property[first] = second` for such a type. `RowSelection<fn>::schemaValue()` and `RowRef<fn>::schemaValue()` return `{"x-selectionOf" | "x-rowOf", name}`, `name` being the `RecordInfo::name` in `model::recordRegistry()` whose `type` is `typeid(TableOf<fn>)`, or empty when none is registered.
  - **`Table.<id>.resolve`.** `BRIDGE_REGISTER_TABLE` registers it beside `Table.<id>` and `Table.<id>.describe`: body `ResolveRequest<fn>`, result `SelectedRows`, run like `Table.<id>`: the table's function for the caller's session, then `resolve`'s work on `tableWork`. A `TableError` surfaces as `Table.<id>`'s errors do.
  - **The runtime.** Nothing to add: `RemoteServer` installs Task 2's `TableRuntimeScope` (`work` = `ServerConfig::tableWork`, `views` = `ServerConfig::views`) around each dispatch, so a Task handler reads it when it calls `resolve`; `Table.<id>.resolve` reaches the same work through `detail::answerQuery`'s `AnswerContext`.
  - **A handler's refusal.** A Task handler that gets `ViewChanged` from `resolve` throws `model::ActionRefusal{"morph.table.viewChanged", {}, error.message}`; add `[[noreturn]] void table::refuse(TableError const&)` mapping `ViewChanged` to that code and every other code to `"morph.table." + <the code's wire name>`.

- [ ] **Step 4: Run them; they pass.**

Run: `ctest --test-dir build/cl-debug -R "view selection:" --output-on-failure`
Expected: 6 test cases pass.

- [ ] **Step 5: Write the failing bridge tests** in `tests/net/test_push_tables.cpp`. Register in the file a model `selnet::Rows` with table `rows` (the same ten rows; a row scope by principal: `alice` sees 0..9, `bob` sees 0..4, with `scope` = the principal), an action `TagRows{table::RowSelection<&Rows::rows> rows;}` whose Task handler awaits `resolve(*this, rows, context, Exact)`, refuses through `table::refuse` and returns `ChangedRows<&Rows::rows>` of the keys, an action `MoveRow{std::int64_t key; std::string status;}` declaring `updatesTable<&Rows::rows>`, and a model `selnet::Other` whose action `Count{table::ResolveRequest<&Rows::rows> request;}` sends `Table.rows.resolve`.

```cpp
TEST_CASE("tables: an action resolves a view selection on its own model", "[net][tables][selection]") {
    // alice: Table.rows page with filters open() -> reply.viewRevision == R.
    // TagRows {view: {filters: open(), retained: [], revision: R}, except: [3]} -> rows.keys {0,1,2,4,5}.
    // MoveRow {key 7, status "open"}, then TagRows with revision R -> refused, code "morph.table.viewChanged".
}
// Mutation: viewRevision that ignores the source snapshot, which tags rows under the old revision.

TEST_CASE("tables: Table.<id>.resolve answers for the caller", "[net][tables][selection]") {
    // bob runs Other.Count with {selection: {keys: [1, 3, 8]}, policy: "current"} -> keys {1, 3}, dropped 1.
    // bob with a view of open() under Current -> keys {0,1,2,3,4}: no row outside bob's scope.
    // With the table's limits.maxSelectedRows = 5, alice's view of open() -> LimitExceeded.
}
// Mutation: resolve run under the server's principal, which returns rows outside bob's scope.

TEST_CASE("tables: a selection's schema names its table", "[net][tables][selection]") {
    // The catalog's schema of TagRows has properties.rows["x-selectionOf"] == "selnet::Rows.rows" (the record's
    // registered name); a RowRef<&Rows::rows> member has "x-rowOf" with the same name.
}
// Mutation: schemaValue returning an empty name, which leaves the client unable to mark busy rows.
```

Write each commented assertion exactly; use the harness Task 2 created in this file.

- [ ] **Step 6: Run them; they fail** (`Table.rows.resolve` is not a registered action; the schema lacks `x-selectionOf`).

Run: `cmake --build --preset linux-everything --target morph_net_tests && ctest --preset linux-everything -R "tables: .*(resolve|selection)" --output-on-failure`
Expected: FAIL.

- [ ] **Step 7: Make them pass** with Step 3's code (the registration and the runtime scope). Run Step 6's command: PASS.

- [ ] **Step 8: Write the failing client tests** in `tests/test_ui_table_model.cpp`. Fixtures, in the test file, shared by Tasks 17a to 17c:

```cpp
// id (Integer, key), status (Text), and the lock companion. Ten rows, keys 0..9: "open" for 0..5, "closed" for 6..9.
morph::table::TableDescription selectionDescription() {
    using namespace morph::table;
    TableDescription d;
    d.id = "samples";
    d.key = "id";
    d.columns = {
        TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
        TableColumn{.info = {.id = "status", .kind = ColumnKind::Text}, .label = "Status"},
        TableColumn{.info = {.id = "lockedBy", .kind = ColumnKind::Text}, .shown = false},
    };
    d.rowLock = "lockedBy";
    return describe(std::move(d));
}
// One row of a page: its key, its status and its lock text.
struct SelRow { std::int64_t key; std::string status = "open"; std::string lockedBy; };
// A page reply of selectionDescription(): {"revision": <its revision>, "columns":["id","status","lockedBy"],
// "keys":[…], "rows":[[key,"status","lockedBy"],…], "total": total, "offset": 0, "viewRevision": viewRevision,
// "unmatched": unmatched}. A keyed reply is the same text with "indices".
std::string selectionPage(std::vector<SelRow> const& rows, std::int64_t total, std::string_view viewRevision,
                          std::vector<std::int64_t> const& unmatched = {});
std::vector<SelRow> openRows() { return {{0}, {1}, {2}, {3}, {4}, {5}}; }

// The table of Tasks 17a to 17c: filtered to open(), with the mutations of the "Catalog actions" table.
inline constexpr std::string_view kSelectionTable = R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
    "selectionMode":"multiple","filters":{"columns":{"status":{"include":[{"eq":"open"}]}}}})";
inline constexpr std::string_view kSelectionMutations = R"({
    "tag":{"model":"sample","action":"Lims_TagRows","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}}})";
// Mounts it and answers the describe request and the first page: six open rows, total 6, viewRevision "v1".
void mountSelection(TableHarness& h, std::string_view table = kSelectionTable,
                    std::string_view mutations = kSelectionMutations) {
    addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
    h.mount(doc(table, mutations));
    replyDescribe(h, describeJson(selectionDescription()));
    replyPage(h, selectionPage(openRows(), 6, "v1"));
}
```

`filters` is written as the literal `FilterSpec` Task 9d's `filters` key reads. Tasks 17b and 17c pass their own table text and mutations to `mountSelection` and add their own action first.

```cpp
TEST_CASE("view selection: Ctrl+A selects the view and fetches no row", "[ui][table][selection]") {
    TableHarness h;
    mountSelection(h);
    int const table = h.backend.find("Table", "testId", "t");
    auto const before = lastQuery(h);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    h.backend.selectAll(table);                                   // the backend's Ctrl+A
    CHECK(h.dispatcher.count("Table.samples") == sent);           // nothing sent
    CHECK(lastQuery(h) == before);
    CHECK(elementValue(h, "t", "selectionSet") ==
          value(R"({"view":{"filters":{"columns":{"status":{"include":[{"eq":"open"}]}}},
                    "retained":[],"revision":"v1"},"except":[]})"));
    CHECK(elementValue(h, "t", "selection") == value("[]"));
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{6}});
    CHECK(elementValue(h, "t", "selectedCountKind") == morph::ui::Value{std::string{"exact"}});
    h.backend.ctrlClickRow(table, std::int64_t{3});
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{5}});
    h.backend.clickRow(table, std::int64_t{2});
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[2]})"));
}
// Mutation: select-all selecting the held keys, which sends no view and counts the held rows only.

TEST_CASE("view selection: a key of except that leaves the view is dropped from it", "[ui][table][selection][review]") {
    // mountSelection; selectAll, ctrlClick(3): count 5. fireEvent(record, [3]); the keyed reply leaves 3 out of the
    // view (it became "closed"): selectionPage of rows 0,1,2,4,5, total 5, viewRevision "v2": selectedCount == 5,
    // selectedCountKind == "exact", selectionSet's except == [].
    // Again from the start, with the event's keyed fetch left unanswered (key 3 held but owed a refetch): kind
    // "approximate" and statusOf(h).counts == "about 5 of 6 selected".
}
// Mutation: except keys never pruned, which counts 4.

TEST_CASE("view selection: its life", "[ui][table][selection]") {
    // selectAll; a header sort: selectionSet still has "view". A filter change: selectionSet == {"keys":[]}.
}
// Mutation: the selection ended by a sort, which empties it on the first step.

TEST_CASE("view selection: not while membership is open", "[ui][table][selection]") {
    // A reply with undecided 40: selectAll changes nothing; statusOf(h).message == "Select all is available once 40 rows are
    // decided". A reply with undecided 0 and awaiting ["recipe"], `recipe` the level of the filtered column (a
    // description whose `status` column has level "recipe"): statusOf(h).message == "Select all is available once
    // recipe data has loaded". awaiting ["recipe"] where only the sorted column has that level: selectAll selects
    // the view.
}
// Mutation: select-all ignoring `undecided`, which selects a view whose rows are not known.

TEST_CASE("view selection: the rows changed since the user selected them", "[ui][table][selection]") {
    // mountSelection; selectAll, ctrlClick(3), runMutation(h, "tag"); a page reply of seven rows, total 7,
    // viewRevision "v2" arrives; refuseMutation(h, "tag", "morph.table.viewChanged"):
    // statusOf(h).message == "The rows changed since you selected them: 6 rows now. Run on these?" with one entry
    // labelled "Run on these". chooseStatusEntry(table, <its id>) sends Lims_TagRows again whose rows.view.revision
    // is "v2" and whose except is [3]. Not choosing it sends nothing.
}
// Mutation: the rerun sending the old revision, which is refused again.

TEST_CASE("view selection: a Shift range toggles a range of except", "[ui][table][selection]") {
    // Six open rows 0..5 at view rows 0..5. selectAll; ctrlClick(1): anchor 1, except == [1]. A Shift range from
    // the anchor to view row 3 (selectTableRange, keys 1,2,3 through keysOf): except == [1,2,3], selectedCount == 3.
    // ctrlClick(5) twice (anchor 5, selected again); a Shift range from it to view row 2 (keys 2..5):
    // except == [1], selectedCount == 5. With maxRangeRows = 2, a range of 3 rows leaves except unchanged and
    // statusOf(h).message == "A range selects at most 2 rows".
}
// Mutation: a Shift range in a view selection replacing the selection with the range's keys.

TEST_CASE("view selection: copying it fetches the view's rows in order", "[ui][table][selection]") {
    // 6 open rows, a window holding 2; selectAll, ctrlClick(1), ctrlClick(4); Copy: the window sends page queries
    // naming the visible columns, and clipboardText() holds rows 0,2,3,5 in view order, one line each. With
    // SessionOptions::maxCopyRows = 3, statusOf(h).message == "A copy holds at most 3 rows" and clipboardText() is unchanged.
}
// Mutation: copying the held rows only, which writes rows 0 and 2.

TEST_CASE("table rows: a new description revision drops a view selection that names a removed column",
          "[ui][table][selection]") {
    // Extends Part D's Review Focus 1 test (Task 9, same file; index decision 16). Description rev A with columns
    // id, status, c; filters on status and on c; selectAll, ctrlClick(3). A page reply names revision B, and the
    // describe reply B lacks c: selectionSet == {"keys":[]} (the view selection was of a filter that named c);
    // lastQuery(h).filters.columns does not contain "c"; a Lims_TagRows run afterwards carries no "c" anywhere in
    // its body. With the filter on status only, the same revision change keeps the view selection, its `except`
    // [3] and its filters.
}
// Mutation: setDescription leaving the view selection as it was, which sends a view whose filter names c.
```

`selectAll`, `clickRow`, `ctrlClickRow` and `chooseStatusEntry` (this part's `RecordingBackend` drivers) call `setOnSelectAll`'s handler, `onSelectionChange` with `Replace` / `Extend`, and `setOnStatusEntry`'s handler; Write each commented assertion exactly as stated.

- [ ] **Step 8a: Write the failing backend tests.** In `tests/qt_quick/test_qt_quick_table.cpp`: `"qt_quick table: select-all asks the handler and marks the view"` — a table of five rows, `SelectionMode::Multiple`, `setOnSelectAll` set: `Ctrl+A` and a click on the select-all box each call the handler once and `onSelectionChange` is not called; after `setSelectionView({key 2})` every row item but key 2's draws selected, and a `Ctrl`+click on row 1 reports `Extend` with the four other keys less 1; after `setSelection({})` no row draws selected. Mutation: `setOnSelectAll` ignored, under which `Ctrl+A` reports the five keys through `onSelectionChange`. `"qt_quick table: a status entry reports its id"` — `setStatus` with `message = "m"`, `entries = {{"run", "Run on these"}}`, `standing = "1 row could not be saved"`, `standingEntries = {{"select", "Select them"}, {"clear", "Clear marks"}}`: the status line shows the three entries as buttons with those labels; a click on "Clear marks" calls the `setOnStatusEntry` handler with `"clear"`. Mutation: the entries drawn as plain text, under which the click reports nothing. In `tests/tui/test_tui_table.cpp`: `"tui table: select-all asks the handler; a status entry reports its id"` — the same two assertions through the terminal's keys (`Ctrl+A`; the entry's key as the status line shows it). Mutation: `setOnStatusEntry` ignored.

- [ ] **Step 9: Run them; they fail** (`selectionSet` is not an element value).

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "view selection:" --output-on-failure`; `cmake --build build/cl-qt-debug --target morph_qt_quick_tests morph_tui_tests`
Expected: FAIL; the backend tests do not compile (`setOnSelectAll`).

- [ ] **Step 10: Implement the client side.**
  - **`WindowRows`.** `selectView()` returns `SelectAllRefusal{undecided, levels}` when `counts().undecided > 0`, or when `counts().awaiting` holds a level of a column in the current filter (a column filter's column or a group filter's columns; levels from the description's `ColumnInfo::level`), `levels` then being those; otherwise `{filters, retained, viewRevision}` of the latest page reply. `selectedCount` for a view: the view's total less every key of `except` the window holds in the view, a key owed a refetch included (it still subtracts); a key of `except` a reply left out of the view is removed from `except` by the lowering. `kind` is `Exact` when the total is `Exact` and every key of `except` is held and owed nothing (`PageWindow::owed()` names none of them), else `Approximate` (so 6 less a held-but-owed 3 is "about 5 of 6"). For keys: `keys.size()`, `Exact`. `rowsOf` sends page queries of `pageSize` rows naming `columns` and completes with every row of the range.
  - `rowsOf` completes with Part E's `FetchedRow{key, viewRow, cells}` (Task 12f), one per view row of the range.
  - **`EngineRows`.** `selectView` returns the engine's filter, retained keys and the digest of its view; the lowering never uses it, since a table held whole selects all by keys. `rowsOf` reads the engine.
  - **`selectAllRefusalText`.** `"Select all is available once " + n + (n == 1 ? " row is decided" : " rows are decided")`, `n` written with the locale's grouping; with no undecided rows, `"Select all is available once " + levels joined with " and " + " data has loaded"`.
  - **The backend.** `setOnSelectAll(handler)`: when set, Ctrl+A, the select-all box and "Select all" call it instead of selecting keys. `setSelectionView(except)`: marks every row whose key is not in `except` until the next `setSelection`. In that state a Ctrl+click reports `Extend` with the keys now selected among the existing keys; the lowering reads every existing key absent from the report as added to `except`, and every present one as removed from it. A plain click reports `Replace` and ends the view selection. A `Shift` range in a view selection (Part D's Task 10a range report by view rows, `selectTableRange`) is resolved through `TableRows::keysOf(first, count)`, bounded by `SessionOptions::maxRangeRows` (over it: Task 10a's refusal through `tableMessage`, and `except` unchanged); a range from an anchor that is selected removes its keys from `except`, one from an anchor in `except` adds them.
  - **A new description revision** (Part D's `setDescription`): a view selection whose filter names a column the new description lacks becomes `TableSelection{}`; one that names none is kept.
  - **The view and the mount.** `ui::Table` and `TableOptions` both gain `onSelectAll`, `selectionView` and `onStatusEntry` (index decision 41). The mount calls `setOnSelectAll(onSelectAll)` when it is set, `setSelectionView(except)` whenever `selectionView` is engaged and changes, `setSelection(selection)` when it disengages, and `setOnStatusEntry(onStatusEntry)`. The lowering holds each status entry's handler in a map by `id` and runs it from `onStatusEntry`.
  - **The lowering.** A model-backed table with `selectionMode: multiple` sets `Table::onSelectAll`; the handler takes `selectView()`, posts `selectAllRefusalText(…)` through `tableMessage`, or makes the element's selection `TableSelection{.view = …}`, which engages `selectionView` with its `except`. A filter change, and "Show only matching rows" (Task 17b), make it `TableSelection{}`; a sort change keeps it. `selectionSet` is the selection's JSON on every table (`{"keys": […]}` on a table over `rows`); `selection` is the key list, `[]` while the selection is a view; `selectedCount` and `selectedCountKind` come from `selectedCount`. The status line's count reads `"about " + n + " of " + total + " selected"` when the kind is `Approximate`.
  - **Run ids.** A `RunStarted::run` is session-wide: `Session` keeps one counter, and `MutationRuntime::send` takes the next id from it for the observers (index decision 24); `MutationRuntime`'s own `_runs` (`src/ui/interpreter.cpp:2396`) is per mutation and two mutations both number their first run 1, so it is not what observers see.
  - **Runs.** `MutationRuntime` calls each `RunObserver` of its session in `send` (`started`) and `settle` (`settled`, with the refusal's code and a `rerun` that calls `run` with a new body and the run's bindings). `tableInputsOf(schema)` lists the schema's top-level properties carrying `x-selectionOf` (`Selection`) or `x-rowOf` (`Row`), with the record the keyword names. Each model-backed table registers one `RunObserver` while mounted. On `settled` with code `morph.table.viewChanged`, for each `Selection` input whose body member is a view selection of this table's record, it takes `selectView()` again (same `except`), posts `"The rows changed since you selected them: " + n + " rows now. Run on these?"` through `tableMessage` with the status entry "Run on these", `n` the new `selectedCount`; choosing the entry calls `rerun` with that member replaced.
  - **Copy** (Part E's Task 12f path): a view selection whose `selectedCount` is above `maxCopyRows` is refused with `tableText::copyBound(maxCopyRows)` through `tableMessage`; otherwise `rowsOf(0, total, visibleColumns)` page by page, rows of `except` left out, in view order.

- [ ] **Step 11: Run them and their targets; all pass.**

Run: `ctest --test-dir build/cl-debug -R "view selection:|table rows:" --output-on-failure`, then all of `morph_tests` and `morph_table_tests`; `ctest --test-dir build/cl-qt-debug -R "qt_quick table:|tui table:" --output-on-failure` and all of `morph_qt_quick_tests` and `morph_tui_tests`; on `linux-everything`, `ctest --preset linux-everything -R "tables:" --output-on-failure` and all of `morph_net_tests`.
Expected: PASS.

- [ ] **Step 12: Mutations, docs, commit.** Run each named mutation and keep its failing line. `docs/spec/ui/document.md`: element values `selectionSet` (every table) and `selectedCountKind`; a model-backed table's select-all selects the view. `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md`: `setOnSelectAll`, `setSelectionView`, `setOnStatusEntry` and the status line's entries. Docs build green.

```bash
git add include/morph/table include/morph/core include/morph/forms/forms.hpp include/morph/ui src/ui src/qt_quick src/tui tests docs/spec
git commit -m "feat(table): select the whole view and resolve it on the server"
```

### Task 17b: Rows kept in view

**Spec:** "Rows an action changed › Kept in view" (2382–2408); test rows "Retained rows", "Retained rows and the event"; invariant 35; failure mode 3317.

**Carried from 17a.** Task 17a leaves `ViewSelection::retained` empty on the client (`WindowRows::selectView` sends `retained: []`), since the window's retained set lands here. This task must fill it from the window's retained keys, the same set its page queries send as `retain`. Once retained rows enter a reply's `viewRevision` (as the spec requires), a selection of the view sent with an empty `retained` names a membership the server never computes, and every `Exact` resolve of it is `ViewChanged`: add a client case that selects the view while rows are retained, runs an action under `Exact`, and checks that it is not refused, with the mutation "retained left empty in `selectView`".

**Files:**
- Modify: `include/morph/table/query.hpp` (`TableQuery::retain`, `QueryLimits::maxRetained`, `Page::unmatched`, `PageRows::unmatched`; `validate` bounds `retain`), `include/morph/table/description.hpp` (`GenericPage::unmatched`, written and read; `TableQuery::retain` on the wire), `include/morph/table/view_cache.hpp` (`detail::mergeRetained`, used per request), `include/morph/table/engine.hpp` (`Engine::retain`, `retained`, `isUnmatched`; `detail::JobInput::retained`), `include/morph/ui/table_rows.hpp` (`TableCountsView::retained`; `TableRows::retain`, `clearRetained`, `unmatched`, `holdRefetches`, `releaseRefetches`; `RetainResult`), `include/morph/ui/interpreter.hpp` (`SessionOptions::retainHold`), `src/ui/interpreter.cpp` (`retainChanged`, the status line entries, muted unmatched rows), `include/morph/ui/view.hpp` (`RowMarks`, `TableRowLook::marks`), `include/morph/ui/backend.hpp` (`TableWidget::setRowMarks`), `include/morph/ui/mount.hpp`, the three table widgets.
- Test: `tests/table/test_table_selection.cpp`, `tests/test_ui_table_model.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_table.cpp`.
- Docs: `docs/spec/ui/document.md` (`retainChanged`), `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md` (`setRowMarks`).

**Interfaces:**
- Consumes: Task 17a's `MembershipDigest`, `RunObserver`, `tableInputsOf`; Task 9's `ChangedRows` reading from a reply (here `changedRowsOf`, below); `ui::ActionInfo::{effects, records}` (`include/morph/ui/document.hpp`).
- Produces:

```cpp
namespace morph::table {
// TableQuery: std::vector<RowId> retain;          QueryLimits: std::size_t maxRetained = 10'000;
// Page<Row>, PageRows, GenericPage: std::vector<std::int64_t> unmatched;   // indices into `rows`
// Engine:
//   std::expected<void, TableError> retain(std::vector<RowId> keys);   // replaces the retained set
//   [[nodiscard]] std::span<RowId const> retained() const noexcept;
//   [[nodiscard]] bool isUnmatched(std::size_t viewRow) const;
namespace detail {
struct MergedView {
    std::vector<std::uint32_t> order;      // source rows, in view order
    std::vector<std::int64_t> unmatched;   // view rows of retained rows the filter rejects
    std::int64_t undecided = 0;
    MembershipDigest digest;
};
[[nodiscard]] MergedView mergeRetained(std::span<std::uint32_t const> baseOrder, std::int64_t baseUndecided,
                                       MembershipDigest baseDigest, std::span<std::uint32_t const> retainedRows,
                                       std::function<bool(std::uint32_t, std::uint32_t)> const& before,
                                       std::function<Match(std::uint32_t)> const& match,
                                       std::function<RowId(std::uint32_t)> const& keyOf);
}
}  // namespace morph::table

namespace morph::ui {
enum class RetainResult : std::uint8_t { Retained, OverBound };
// TableCountsView: std::int64_t retained = 0;    // held view rows that are retained and unmatched
// TableRows gains:
//   [[nodiscard]] virtual RetainResult retain(std::span<table::RowId const> keys) = 0;   // adds
//   virtual void clearRetained() = 0;
//   [[nodiscard]] virtual bool unmatched(table::RowId const& key) const = 0;   // by key: a row look has no index
//   virtual void holdRefetches(std::uint64_t run) = 0;
//   virtual void releaseRefetches(std::uint64_t run) = 0;
// SessionOptions: std::chrono::milliseconds retainHold{2000};
// view.hpp. A row's own marks (index decision 39): they do not vary per cell, so the row look carries them.
struct RowMarks {
    bool busy = false;
    bool locked = false;
    bool unmatched = false;
    std::string tooltip;
    [[nodiscard]] bool operator==(RowMarks const&) const = default;
};
// TableRowLook (Task 9d) gains: std::optional<Prop<RowMarks>> marks;   // bound in the row's scope by `rowView`
// TableWidget: virtual void setRowMarks(Widget& row, RowMarks const& marks) = 0;
struct ChangedRowsValue { std::vector<table::ChangedRow> rows; std::vector<table::FailedRow> failed; };
[[nodiscard]] ChangedRowsValue changedRowsOf(Value const& reply, ActionInfo const& action, std::string_view record);
}  // namespace morph::ui
```

- [ ] **Step 1: Write the failing engine and server tests** in `tests/table/test_table_selection.cpp`, over Task 17a's `SelectionModel`.

```cpp
TEST_CASE("retained rows: they stay in the view, listed in unmatched", "[table][retain]") {
    SelectionModel model;
    TableQuery query{.sort = {{.column = "mass", .dir = SortDirection::Descending}}, .filters = open(),
                     .retain = {RowId{std::int64_t{1}}, RowId{std::int64_t{7}}, RowId{std::int64_t{8}}}};
    for (std::int64_t key : {1, 7, 8}) {   // the action closed row 1; rows 7 and 8 were closed already
        model.source->patchRow(RowId{key}, {Cell{std::string{"closed"}}, Cell{tabletest::dec(std::to_string(key) + ".0")}});
    }
    auto const page = apply(*model.source, query).value();
    std::vector<RowId> keys;
    for (std::size_t i = 0; i < page.rows.size(); ++i) { keys.push_back(model.source->snapshot()->rowId(page.rows[i])); }
    CHECK(keys == std::vector<RowId>{std::int64_t{8}, std::int64_t{7}, std::int64_t{5}, std::int64_t{4},
                                     std::int64_t{3}, std::int64_t{2}, std::int64_t{1}, std::int64_t{0}});
    CHECK(page.unmatched == std::vector<std::int64_t>{0, 1, 6});
    CHECK(page.total == 8);
    TableQuery const plain{.sort = query.sort, .filters = open()};
    CHECK(page.viewRevision != apply(*model.source, plain).value().viewRevision);
}
// Mutation: retained rows appended after the base view instead of merged by the sort's keys.

TEST_CASE("retained rows: the shared view entry is not changed by one request's rows", "[table][retain]") {
    // Through Task 6's ViewCache: a request retaining {7}, then a request of the same sort and filter retaining
    // nothing: the second reply has 5 rows, no 7, and empty `unmatched`; the cache computed the view once.
}
// Mutation: the merged view written into the shared view cache entry, which serves one session's retained rows to
// another.

TEST_CASE("retained rows: retaining adds, never removes", "[table][retain][property]") {
    // Property over 200 random tables (seeded): for random filters and a random retain set R of up to 20 keys,
    // view(with R) ⊇ view(without R); view(with R) \ view(without R) == R ∩ source keys \ view(without R);
    // `unmatched` names exactly the rows of R whose filter evaluates to No.
}
// Mutation: a retained row counted twice when the filter passes it too, which breaks the second equation.

TEST_CASE("retained rows: the bound and the wire", "[table][retain]") {
    CHECK(QueryLimits{}.maxRetained == 10'000);
    // validate of a query retaining 10'001 keys: LimitExceeded; 10'000: accepted. A GenericPage with unmatched
    // [0, 6] round-trips; a page read without "unmatched" has it empty.
}
// Mutation: no bound, which accepts 10'001.

TEST_CASE("retained rows: a client-mode engine does the same", "[table][retain]") {
    // An Engine over the same source, filter open(), sort mass desc; patch rows 1,7,8 closed; retain({1,7,8}):
    // the view's keys are {8,7,5,4,3,2,1,0}; isUnmatched(0), (1), (6) are true, the rest false; setFilter(open())
    // clears the retained set (retained().empty()) and the view is {5,4,3,2,0}.
}
// Mutation: Engine::retain ignored by the filter run, which leaves 1, 7 and 8 out.
```

Write the commented cases' assertions exactly; `apply`'s `PageRows` holds source rows in `rows` (use its existing member if it is named differently).

- [ ] **Step 2: Run them; they fail to compile** (`TableQuery::retain`).

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile error naming `retain`.

- [ ] **Step 3: Implement.** `mergeRetained`: R = the retained rows (source rows) not in `baseOrder`; sort R by `before`, which is the query's comparator over the sort's key columns with ties to the lower source row (the engine's order); merge R into `baseOrder` with the same comparator, so the result is the view the sort would give over base ∪ R. Every row of R is in the view whatever its `match` (a row whose filter is `Unknown` is in the view, not undecided): `undecided = baseUndecided −` the rows of R that were undecided; `digest` = `baseDigest` plus each key of R; `unmatched` lists the view row of each row of R whose `match` is `No`, ascending. `apply` and the view cache path compute the base view as before and call `mergeRetained` per request when `retain` is not empty; the cache entry is never written with it. The total is the merged view's size. A keyed query reads the merged view; distinct values ignore `retain`. A reply's `unmatched` holds indices into its own `rows`. `Engine::retain` stores the keys and runs the filter again with them admitted; `setFilter` clears them.

- [ ] **Step 4: Run them; they pass.**

Run: `ctest --test-dir build/cl-debug -R "retained rows:" --output-on-failure`
Expected: 5 test cases pass.

- [ ] **Step 5: Write the failing client tests** in `tests/test_ui_table_model.cpp`, over the ten rows, a table with `"retainChanged": true` filtered to `open()`, and the mutation `close` of the "Catalog actions" table. Each case starts with `addAction(h, "Lims_CloseRows", std::string{kSelectionSchema}, samplesAction());` and `mountSelection(h, <kSelectionTable with "retainChanged":true>, <the `close` mutation>)`; `samplesAction()` is what makes `Lims_CloseRows` an action with an effect on the table's record and a `ChangedRows` at `rows.rows`.

```cpp
TEST_CASE("retained rows: the rows an action changed stay in view", "[ui][table][retain]") {
    // run close; settle with {"rows":{"rows":[{"key":1},{"key":2},{"key":3}],"failed":[]}}: the next query carries
    // retain [1,2,3]; answer it with those rows unmatched: they are drawn with RowMarks{.unmatched = true,
    // .tooltip = "This row no longer matches the filter"}, counts().retained == 3, statusOf(h).standing ==
    // "3 rows shown that no longer match" with the standing entry "Show only matching rows"; it is still there after
    // h.advance(7000) and a click on a row. Choosing "Show only matching rows": the next
    // query has no retain, and a view selection, if any, is empty. A filter change does the same.
}
// Mutation: retained keys dropped when the next page arrives, which loses them from the second query.

TEST_CASE("retained rows: an action over the bound retains none", "[ui][table][retain]") {
    // With 9'999 keys retained, a reply naming 2 changed rows: no key is added, statusOf(h).message ==
    // "The changed rows are not kept in view: a table keeps at most 10,000"; the 9'999 stay. With 9'998 retained,
    // the same reply adds both.
}
// Mutation: a partial add up to the bound, which retains one of the two.

TEST_CASE("retained rows: an event before its action's reply is held", "[ui][table][retain]") {
    // run close (it declares an effect on the table); fireEvent(record, [1]) before the reply: no query is sent.
    // settle the run: exactly one query is sent, keyed or page per Task 7's refresh plan, carrying retain [1].
    // A second run, an event, and the scheduler advanced by retainHold (2 s) without a reply: one query is sent
    // then, and none when the reply arrives later except its retained rows' refetch.
    // On a table without retainChanged, the event's query is sent at once.
}
// Mutation: no hold, under which row 1 leaves the view and comes back.
```

Write the commented assertions exactly as stated. `rowMarks(h, key)` is the last `setRowMarks` the recording backend got for the row with that key.

- [ ] **Step 5a: Write the failing backend tests.** In `tests/qt_quick/test_qt_quick_table.cpp`: `"qt_quick table: a row's marks are drawn"` — three rows; `setRowMarks(row 0, {.unmatched = true, .tooltip = "This row no longer matches the filter"})`, `setRowMarks(row 1, {.busy = true})`, `setRowMarks(row 2, {.locked = true, .tooltip = "Being edited by another user"})`: row 0's item has the theme's muted opacity and that tooltip, row 1's shows the busy mark item, row 2's the locked hatch item and its tooltip; after `setRowMarks(row, RowMarks{})` each draws as an unmarked row. Mutation: `MorphTable.qml` ignoring `busy`, under which row 1 shows no busy mark. In `tests/tui/test_tui_table.cpp`: `"tui table: a row's marks are drawn"` — the same three rows: row 0's cells carry the dim attribute, row 1's line begins with the busy glyph, row 2's with the lock glyph. Mutation: `setRowMarks` doing nothing.

- [ ] **Step 6: Run them; they fail.**

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "retained rows:" --output-on-failure`
Expected: FAIL.

- [ ] **Step 7: Implement.**
  - `changedRowsOf(reply, action, record)`: for each path of the `ActionRecordInfo` of `record`, the path names a `ChangedRows`' `rows` member (`"<member>.rows"`, or `"rows"` for the result itself); the value at the path's parent is read as `{"rows": [{"key", "from"?}], "failed": [{"key", "message"}]}`. A number is an integer key, a string a text key.
  - `WindowRows::retain(keys)`: the keys not already retained are added when the set stays within `maxRetained`, else none is and the result is `OverBound`; each query from then on carries the set in `retain`. `clearRetained` empties it. `unmatched(key)` is true for a held row the last reply that supplied it listed in `unmatched`, false for a key the window does not hold; `counts().retained` counts those. `holdRefetches(run)` records the run and, while any run is recorded and for at most `SessionOptions::retainHold` from the first, makes a record event only mark what it would fetch; `releaseRefetches(run)` (or the hold's timer, on the session's scheduler) sends what was marked as one refetch. `EngineRows` maps these to `Engine::retain`, `isUnmatched` at the key's view row (false for a key not in the view), and no-op holds.
  - The lowering, on a table with `retainChanged` (a literal flag, default `false`): its `RunObserver::started` calls `holdRefetches(run)` for an action whose catalog `effects` name this table's record; `settled` calls `retain` with the keys of `changedRowsOf(reply, …).rows` (and their `from` keys, Task 17c), posts the over-bound message through `tableMessage` on `OverBound` (a refusal: transient), then `releaseRefetches(run)`. A filter change and "Show only matching rows" call `clearRetained` and end a view selection. Retained rows are not part of the stored table state.
  - `RowMarks` (index decision 39): `TableRowLook::marks` is bound by the lowering's `rowView` in the row's scope, as `tone` and `label` are; the mount calls `setRowMarks(row, marks)` after `setRowKey` for a row whose look has `marks`, and on every change. In this task the binding gives `RowMarks{.unmatched = rows->unmatched(key), .tooltip = unmatched ? "This row no longer matches the filter" : ""}`, `key` read from the row's record; Task 17c adds `busy`, `locked` and their tooltips to the same binding. `TableRows` and `RowStates` are not reactive, so the lowering holds one `reactive::Signal<std::uint64_t>` per table (`marksRevision`), which the binding reads first and the lowering bumps after every change of the retained set, after every reply that changes a held row's `unmatched`, and (Task 17c) after every `RowStates` change; without that read a row that becomes unmatched is not drawn again. Qt Quick draws `unmatched` muted, `busy` with the theme's busy mark, `locked` with the theme's locked hatch, and `tooltip` as the row's tooltip; the terminal draws `unmatched` dim and prefixes a busy row with the busy glyph and a locked row with the lock glyph.

- [ ] **Step 8: Run them and the targets; all pass.**

Run: `ctest --test-dir build/cl-debug -R "retained rows:" --output-on-failure`, then all of `morph_tests`, `morph_table_tests`, and in `build/cl-qt-debug` `ctest --test-dir build/cl-qt-debug -R "a row's marks" --output-on-failure`, then all of `morph_qt_quick_tests`, `morph_tui_tests`.
Expected: PASS.

- [ ] **Step 9: Mutations, docs, commit.** `docs/spec/ui/document.md`: `retainChanged`. Backend specs: `setRowMarks`. Docs build green.

```bash
git add include/morph/table include/morph/ui src/ui src/qt_quick src/tui tests docs/spec
git commit -m "feat(table): keep rows an action changed in view"
```

### Task 17c: Key changes, busy, failed and locked rows

**Spec:** "A key that changes" (2409–2420), "Rows at work, rows that failed" (2422–2443), "A locked row" (452–455); test rows "A key that changes", "Busy and failed rows", "Locked rows and companions" (the lock half); failure mode 3318.

**Files:**
- Modify: `include/morph/table/description.hpp` (`ChangedRow::from` on Part A's `ChangedRow{RowId key;}`; `FailedRow`; `ChangedRows::failed`), `include/morph/core/records.hpp` (`RecordInfo::previousKeyField`), `include/morph/core/registry.hpp` (`BRIDGE_REGISTER_TABLE` sets it to `"from"`), `include/morph/core/changes.hpp` (`addUpdatesChange` collects it), `include/morph/ui/table_rows.hpp` (`RowStates`, `TableRows::states`, `TableRows::moveKey`, `lockText`, `refusesEdit`, `TableCountsView::busy`), `include/morph/ui/view.hpp` (`KeyMoves`; `keyMoves` on `ui::Table` and `TableOptions`), `include/morph/ui/mount.hpp` (the focus it holds follows a key move), `include/morph/ui/testing/recording_backend.hpp`, `src/ui/interpreter.cpp` (busy rows from inputs, failed marks, "Select them", "Clear marks", locked rows, key moves, element value `busyCount`).
- Test: `tests/net/test_push_tables.cpp`, `tests/test_ui_table_model.cpp`.
- Docs: `docs/spec/ui/document.md` (element value `busyCount`), `docs/spec/ui/view_tree.md` (a table's `keyMoves`: the mount's focus follows a row whose key changed).

**Interfaces:**
- Consumes: Task 17a's `RunObserver`, `tableInputsOf`, `TableSelection`; Task 17b's `changedRowsOf`, `retain`, `RowMarks`; Task 15's `TableDescription::rowLock`.
- Produces:

```cpp
namespace morph::table {
struct ChangedRow { RowId key; std::optional<RowId> from; };
struct FailedRow { RowId key; std::string message; };
// ChangedRows<fn>: std::vector<ChangedRow> rows; std::vector<FailedRow> failed;
}
namespace morph::model {
// RecordInfo: std::optional<std::string> previousKeyField;   // "from" on a table's record
}
namespace morph::ui {
class RowStates {
public:
    void busyKeys(std::uint64_t run, std::vector<table::RowId> keys);
    void busyView(std::uint64_t run, std::vector<table::RowId> except, std::int64_t count);
    void settle(std::uint64_t run);
    void fail(std::span<table::FailedRow const> rows);
    void clearFailed(std::span<table::RowId const> keys);
    void clearFailed();
    void move(table::RowId const& from, table::RowId const& to);
    [[nodiscard]] bool isBusy(table::RowId const& key) const;
    [[nodiscard]] std::optional<std::string> failure(table::RowId const& key) const;
    [[nodiscard]] std::int64_t busyCount() const;
    [[nodiscard]] std::vector<table::RowId> failedKeys() const;   // in the order they failed
};
// TableRows gains:
//   [[nodiscard]] virtual RowStates& states() = 0;
//   [[nodiscard]] virtual RowStates const& states() const = 0;
//   virtual void moveKey(table::RowId const& from, table::RowId const& to) = 0;   // retained and states
// TableCountsView: std::int64_t busy = 0;
[[nodiscard]] std::optional<std::string> lockText(TableRowRef const& row, table::TableDescription const& description);
[[nodiscard]] bool refusesEdit(TableRows const& rows, std::int64_t index, table::TableDescription const& description);
struct KeyMoves {                                                                             // view.hpp
    std::uint64_t generation = 0;
    std::vector<std::pair<Key, Key>> moves;   // from, to
    [[nodiscard]] bool operator==(KeyMoves const&) const = default;
};
// ui::Table and TableOptions both gain (index decision 41): Prop<KeyMoves> keyMoves;
// No backend call: the selection reaches a backend through `selection` and `setSelection`, and focus is the
// mount's, held by (row key, column id) since Task 10a.
}
```

`refusesEdit` is what Part G's cell editing (editing.md "Read-only is one rule", Task 24) consults: a busy or locked row takes no cell edit.

- [ ] **Step 1: Write the failing walk test** in `tests/net/test_push_tables.cpp`: `"tables: a changed key's event names both keys"` — the file's model gains `SaveAs{std::int64_t key;}` returning `{"rows": {"rows": [{"key": 10, "from": 2}], "failed": [{"key": 4, "message": "Locked"}]}}` and declaring `updatesTable<&Rows::rows>`; a watcher of `selnet::Rows.rows` receives one event whose keys are exactly `{10, 2}` (in any order) and not `4`. Mutation: the walk ignoring `from`, which leaves the old row's watchers untold (keys `{10}`).

- [ ] **Step 2: Run it; it fails** (keys `{10}`).

Run: `cmake --build --preset linux-everything --target morph_net_tests && ctest --preset linux-everything -R "tables: a changed key" --output-on-failure`
Expected: FAIL, keys `{10}`.

- [ ] **Step 3: Implement.** `RecordInfo::previousKeyField` (absent by default); `BRIDGE_REGISTER_TABLE` sets it to `"from"` on the table's record; `addUpdatesChange` (`include/morph/core/changes.hpp:180`), after collecting `keyField` at each path, collects `*info->previousKeyField` at the same paths when it is set. A `failed` row is never at a `rows` path, so it is not collected. Run Step 2's command: PASS.

- [ ] **Step 4: Write the failing client tests** in `tests/test_ui_table_model.cpp`, over the ten rows of Task 17a (no filter) and a description with `rowLock = "lockedBy"`; the mutations `save` and `touch` of the "Catalog actions" table. Each case starts with `addAction(h, "Lims_SaveAs", std::string{kSelectionSchema}, samplesAction());` and `addAction(h, "Lims_TouchRow", std::string{kRowSchema}, samplesAction());`, then `mountSelection(h, <the table node without "filters">, <the `save` and `touch` mutations>)` with a first page of the ten rows (row 6's `lockedBy` is `"Being edited by another user"`); the document declares the `state` name `picked`, which a case sets before it runs `touch`.

```cpp
TEST_CASE("key change: the session that ran the action moves what it holds", "[ui][table][keys]") {
    // Rows 2 and 5 selected (keys), focus on row 2, row 2 failed from an earlier run, row 2 retained.
    // run save; settle with {"rows":{"rows":[{"key":10,"from":2}],"failed":[]}}: selectionSet == {"keys":[10,5]}
    // (key 10 in key 2's place), the mount's focus is on key 10 (the last setFocusCell names key 10's view row
    // once key 10 is in the view, and no setFocusCell named another row meanwhile), states().failure(10) holds
    // the message and
    // failure(2) is empty, the retained set holds 10 and not 2; fireEvent(record, [10, 2]) sends one keyed query
    // naming both 10 and 2.
}
// Mutation: the client ignoring `from`, which leaves 2 selected and 10 not.

TEST_CASE("key change: the action's event before its reply", "[ui][table][keys]") {
    // Rows 2 and 5 selected, focus on 2. runMutation save; fireEvent(record, [10, 2]) and the keyed reply that
    // leaves 2 out of the view and holds 10, all before the reply: selectionSet still holds 2. settleMutation with
    // {"rows":{"rows":[{"key":10,"from":2}],"failed":[]}}: selectionSet == {"keys":[10,5]} and the mount's focus
    // is on key 10. The same with a reply that names no `from`: after settle, 2 leaves the selection and focus is
    // on the row now at 2's view index.
}
// Mutation: no deferral while the run is in flight, which drops 2 before `from` names it and selects [5].

TEST_CASE("key change: onto a key already held", "[ui][table][keys][review]") {
    // Rows 3 and 7 held; 7 and 3 selected. settle with {"rows":{"rows":[{"key":7,"from":3}],"failed":[]}}:
    // selectionSet == {"keys":[7]} (7 once); fireEvent(record, [7, 3]): the keyed query names 3 and 7.
}
// Mutation: a move that appends `to` without checking it is held, which selects 7 twice.

TEST_CASE("key change: a from the client does not hold", "[ui][table][keys]") {
    // settle with {"rows":{"rows":[{"key":10,"from":99}],"failed":[]}}: nothing moves (selection, focus, states
    // unchanged); the event's keyed query names 10.
}
// Mutation: a move inventing state for 99, which adds 10 to the selection.

TEST_CASE("key change: another session's event drops the old key", "[ui][table][keys]") {
    // Row 2 selected and focused; fireEvent(record, [10, 2]) with no run of this session, the keyed reply without
    // row 2: 2 leaves the selection, focus moves to the row now at 2's view index.
}
// Mutation: a selection kept for keys that left the view, which keeps 2.

TEST_CASE("busy rows: a run's rows are busy until it settles", "[ui][table][busy]") {
    // Keys 1, 2 selected; run save: rows 1 and 2 drawn with RowMarks{.busy = true}; busyCount == 2;
    // statusOf(h).standing == "Saving 2 rows"; refusesEdit for both. refuse(save, "lims.rows.denied"): no row is busy,
    // busyCount == 0. A view selection of 6 rows less 3 held as except: every held row but 3 is busy, busyCount 5.
    // run touch {"row": 4}: row 4 busy.
}
// Mutation: busy state cleared only on success, which leaves rows 1 and 2 busy after the refusal.

TEST_CASE("busy rows: overlapping runs", "[ui][table][busy][review]") {
    // Run A over keys {1,2}, run B over {2,3}: busyCount == 3. Settle A: row 2 still busy, busyCount == 2.
    // Settle B: busyCount == 0.
}
// Mutation: a busy set keyed by row only, which frees row 2 when A settles.

TEST_CASE("busy rows: two mutations' runs do not share an id", "[ui][table][busy]") {
    // Mutations `save` and `touch`, each running for the first time: save over {1, 2}, touch {"row": 5}. Both
    // runs are busy (busyCount == 3); settle touch: rows 1 and 2 stay busy, row 5 does not (busyCount == 2);
    // settle save: busyCount == 0.
}
// Mutation: run ids taken from MutationRuntime's own counter, under which settling touch (run 1) frees save's rows.

TEST_CASE("failed rows: marked until the row's next change", "[ui][table][failed]") {
    // run save over {4, 5}; settle with {"rows":{"rows":[{"key":5}],"failed":[{"key":4,"message":"Locked"}]}}:
    // row 4 has the err tone and tooltip "Locked"; statusOf(h).standing == "1 row could not be saved", standingEntries labelled "Select them" and
    // "Clear marks". "Select them": selectionSet == {"keys":[4]}. An event naming 5 (the action's own): 4 stays
    // marked. An event naming 4 after the run settled: the mark goes. Again failed; "Clear marks": the mark goes.
    // Again failed; a new run naming row 4: the mark goes when the run starts. On a rowScoped table, an event with
    // no keys leaves the mark.
}
// Mutation: a failed mark cleared by any event, which clears it on the action's own event.

TEST_CASE("locked rows: a locked row draws locked and refuses an edit", "[ui][table][locked]") {
    // Row 6's lockedBy is "Being edited by another user": RowMarks{.locked = true, .tooltip = "Being edited by
    // another user"}, lockText == that text, refusesEdit true; it can be selected (click(6) selects it) and copied.
    // Row 5's lockedBy is "": not locked, refusesEdit false.
}
// Mutation: lockText reading `rowTone`'s column, which locks no row.
```

Write each commented assertion exactly as stated.

- [ ] **Step 5: Run them; they fail.**

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "key change:|busy rows:|failed rows:|locked rows:" --output-on-failure`
Expected: FAIL.

- [ ] **Step 6: Implement.**
  - `RowStates`: busy runs are kept per run (a key run's keys; a view run's `except` and the `count` it was started with); `isBusy(k)` is true when any key run holds `k` or any view run does not name `k` in its `except`; `settle(run)` drops the run whatever its outcome. **`busyCount`**: with no view run in flight, the number of distinct keys across the key runs (runs {1,2} and {2,3} count 3); with a view run in flight, the largest `count` among the view runs plus the distinct keys of key runs that every view run names in its `except` (those keys are outside the view runs' rows). `fail` records each key's message; `clearFailed(keys)` and `clearFailed()` drop marks. `move(from, to)`: moves the busy membership and the failed mark of `from` to `to`; when `to` already has one, `to`'s is kept and `from`'s dropped.
  - The lowering's `RunObserver`: `started` reads `tableInputsOf(Catalog::schema(action))`; for each input naming this table's record, a `Selection` member's key selection is `busyKeys(run, keys)`, its view selection `busyView(run, except, selectedCount)`, a `Row` member `busyKeys(run, {key})`; each key the run names has its failed mark cleared. `settled` calls `settle(run)`; then, on success, with `changedRowsOf(reply, …)`: `fail(failed)`; for each row with `from` that this session holds in any way (selection, focus, retained, states), `TableRows::moveKey(from, key)`, the selection's `from` replaced by `key` (kept once if `key` is already selected), and a `KeyMoves` entry for the mount, whose generation increases; a `from` the client does not hold moves nothing. An event after the run settled clears the failed marks of the keys it names; an event with no keys clears none.
  - **Held while an action is in flight.** A key leaving the view drops it from the selection, the focus and `RowStates`; but while a run of this session whose action's catalog `effects` name this table's record is in flight, that drop is deferred for the keys that left: they are kept (and still counted) until every such run has settled, so the action's own event, which can precede its reply (table.md:2396–2402), does not lose the old key a `from` then names. At settle the moves are applied first, then each deferred key that is still out of the view and was not a `from` is dropped as any key that left the view.
  - The mount applies each new `KeyMoves` generation's moves to what it holds by key: when its focus (Task 10a's `(row key, column id)`) is on `from`, the focus key becomes `to` before the `ViewChange` that removes `from` is applied, so the rule "a focused row that leaves the view moves focus to the row now at its view index" does not fire for it, and `setFocusCell` follows `to` when it is in the view; a `highlightKey` on `from` moves the same way. Nothing is sent to the backend for the move itself: the selection mark moves because the lowering's `selection` now holds `to` (`setSelection`). A generation the mount has applied is not applied again.
  - Drawing (index decision 39): Task 17b's `TableRowLook::marks` binding now gives `{busy = states().isBusy(key), locked = lockText has a value, unmatched = rows->unmatched(key), tooltip = lockText, else failure(key), else the unmatched text}`, and reads `marksRevision` first; the lowering bumps `marksRevision` after every `busyKeys`, `busyView`, `settle`, `fail`, `clearFailed` and `move`. A failed row's tone is `err` through `TableRowLook::tone`: the lowering's tone binding gives `Tone::Err` while `failure(key)` has a value, else the row's `rowTone`, and reads `marksRevision` too. `lockText` is the row's `rowLock` cell when it is non-empty text. `refusesEdit` is busy or locked.
  - Element value `busyCount`; the standing part of the status line (not `tableMessage`) holds `"Saving " + n + (n == 1 ? " row" : " rows")` while `busyCount > 0`, and `n + (n == 1 ? " row could not be saved" : " rows could not be saved")` with the standing entries "Select them" (selection = `failedKeys()`) and "Clear marks" (`clearFailed()`), while `failedKeys()` is not empty.

- [ ] **Step 7: Run them and the targets; all pass.**

Run: `ctest --test-dir build/cl-debug -R "key change:|busy rows:|failed rows:|locked rows:" --output-on-failure` and all of `morph_tests`; `ctest --preset linux-everything -R "tables:" --output-on-failure` and all of `morph_net_tests`; and `morph_qt_quick_tests`, `morph_tui_tests` in `build/cl-qt-debug` (this task adds no backend call: busy and locked rows are drawn by Task 17b's `setRowMarks`, whose three flags Task 17b's backend tests cover).
Expected: PASS.

- [ ] **Step 8: Mutations, docs, commit.** `docs/spec/ui/document.md`: element value `busyCount`. `docs/spec/ui/view_tree.md`: `keyMoves`. Docs build green.

```bash
git add include/morph/table include/morph/core include/morph/ui src/ui tests docs/spec
git commit -m "feat(table): key changes, busy, failed and locked rows"
```

### Task 17d: The application's menu entries and a disabled header

**Spec:** "The application's menu entries" (2359–2378), "The header while the application is busy" (2445–2451); test rows "The application's menu entries", "A disabled header"; invariant 31.

**Files:**
- Modify: `include/morph/ui/view.hpp` (`cellMenu`, `headerEntries`, `headerEnabled` on `ui::Table` and `TableOptions`), `include/morph/ui/backend.hpp` (`TableWidget::setCellMenu`), `include/morph/ui/mount.hpp`, the three table widgets, `src/ui/interpreter.cpp` (`cellMenu`, `headerMenu`, `headerEnabled`; "Filter by this value", "Exclude this value", "Filter by this value in any <group>"), `src/ui/document.cpp` (load-time checks).
- Test: `tests/test_ui_table_model.cpp`, `tests/qt_quick/test_qt_quick_table.cpp`.
- Docs: `docs/spec/ui/document.md` (`cellMenu`, `headerMenu`, `headerEnabled`), `docs/spec/ui/backend_contract.md` and `docs/spec/qt_quick/backend.md` (`setCellMenu`).

**Interfaces:**
- Consumes: Part E's `TableOptions::menu(std::optional<std::size_t>)` / `onMenu` and `TableWidget::setMenu` (Task 12b), `ui::headerMenu(HeaderMenuInput)`, `ui::menuEntries` and `MenuCommand::{Copy, CopyWithHeaders}` (Task 12a), its copy (Task 12f) and `ScreenCore::tableMessage` (Task 9d); Task 8a's `FilterEntry::oneOf`; Task 8b's `GroupFilter{group, columns, mode, match, combine, include, exclude}`; Task 15's `operandOf`, `cellLook`; the `menu` node's entry lowering in `src/ui/interpreter.cpp` (its `{label, icon, keys, checked, enabled, onClick, items, description, visible}` shape).
- Produces:

```cpp
namespace morph::ui {
// ui::Table and TableOptions both gain (index decision 41; `column` an index into Table::columns, decision 37):
//   std::function<std::vector<MenuItem>(Key const& row, std::size_t column)> cellMenu;  // built-ins, separator, app's
//   std::function<std::vector<MenuItem>(std::size_t column)> headerEntries;              // the app's only
//   Prop<bool> headerEnabled = true;
// TableWidget gains:
//   virtual void setCellMenu(std::function<std::vector<MenuEntry>(Key const& row, std::size_t column)> entries,
//                            std::function<void(Key const& row, std::size_t column,
//                                               std::vector<std::size_t> const& path)> chosen) = 0;
}
```

`path` is the chosen entry's index at each menu level.

**Who composes a menu.** `MenuItem` (`view.hpp:715`) holds `Prop`s and an `Action`; `MenuEntry` (`backend.hpp:358`) is the plain value a backend draws. The mount owns the conversion in both menus, and the lowering never returns a `MenuItem` where a `MenuEntry` is expected:
- **Reading an item.** When a menu opens, the mount reads each item's `label`, `checked`, `enabled`, `description` and `visible` once, untracked (`runtime.untracked`), into a `MenuEntry`; hidden items and the separators they strand are left out, as `Menu` does, and the mount keeps, for that open menu, the item each entry path came from. The `Prop`s are bindings the lowering built over the scope it made for that row and column (or that column), so no scope is entered at open time.
- **A cell's menu.** `setCellMenu`'s `entries(row, column)` is the mount's conversion of `cellMenu(row, column)`; its `chosen(row, column, path)` runs the kept item's `onSelect`.
- **A header's menu.** Part E's `menu(column)` keeps returning the built-in `MenuEntry`s only, and `onMenu` keeps taking paths into them. With `headerEntries` set and a column given, what the mount hands to `TableWidget::setMenu` is `menu(column)`, then a separator entry, then the conversion of `headerEntries(column)` (nothing is appended when it converts to no entry); a chosen path whose first index is below `menu(column).size()` goes to `onMenu` unchanged, and one past the separator runs the kept item's `onSelect` with its first index less `menu(column).size() + 1`. The table's own menu (no column) is `menu(nullopt)` alone.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp`, each over a `TableHarness` and a whole document (`doc(view)`, with the `state` names `picked`, `opened` and `idle` the cases set and read through `elementValue`), over a model-backed table with columns `id`, `mass` (Decimal), `companyA`/`companyB` (Text, declared group `company`, label "Company") and `"selectionMode":"multiple"`:

```cpp
TEST_CASE("table menus: the application's entries read the row, the column and the value", "[ui][table][menu]") {
    // cellMenu [{"label": {"fn":"concat","args":[{"ref":"column"}," = ",{"ref":"value"}," of ",{"ref":"row.id"}]},
    //            "onClick": [{"set":"picked","to":{"ref":"row.id"}}]}]:
    // rightClickCell(table, 2, <mass>): the menu of row 2's mass cell is ["Copy", "Copy with headers", "Filter by this value", "Exclude this value",
    // separator, "mass = 0.2 of 2"]; choosing the last sets picked to 2.
    // Rows 1 and 3 selected, a right-click on row 2: the selection is {"keys":[2]}; a right-click on row 1 while
    // 1 and 3 are selected keeps both.
    // headerMenu [{"label":"Units…","visible":{"fn":"eq","args":[{"ref":"column"},"mass"]}}]: menuOf(table, <mass>)
    // ends with a separator and "Units…"; the id header's has no "Units…".
}
// Mutation: the cell menu read in the table's scope, which reads `row` as null.

TEST_CASE("table menus: filter by this value", "[ui][table][menu]") {
    // "Filter by this value" on mass 0.2: filters == {"columns":{"mass":{"include":[{"oneOf":["0.2"]}]}}},
    // replacing mass's earlier entries. "Exclude this value" on 0.3 then 0.4: exclude == [{"oneOf":["0.3","0.4"]}].
    // On an empty cell: include == [{"isEmpty":true}]. On an unloaded cell both entries are disabled.
    // On companyA "F": the menu has "Filter by this value in any Company", which sets
    // groups == [{"group":"company","include":[{"oneOf":["F"]}]}].
}
// Mutation: "Filter by this value" adding to include instead of replacing it, which keeps the earlier entry.

TEST_CASE("table menus: an entry's keys work while the table has focus", "[ui][table][menu]") {
    // cellMenu [{"label":"Open","keys":"Ctrl+O","onClick":[{"set":"opened","to":{"ref":"row.id"}}]}], focus on
    // row 3: Ctrl+O sets opened to 3; with focus elsewhere, Ctrl+O does nothing. A query of this table names
    // the columns the entries read (`id`) for every row.
}
// Mutation: keys bound only while the menu is open.

TEST_CASE("table menus: a disabled header", "[ui][table][header]") {
    // headerEnabled {"ref":"idle"}, idle false: clickHeader, clickFilter and moveColumn change nothing,
    // send no query, and each sets statusOf(h).message to "Sorting, filtering and arranging are unavailable right now";
    // selecting, copying and cellMenu entries still work. idle becomes true: a header click sorts.
}
// Mutation: headerEnabled read once at mount, which leaves the header enabled after the condition turns false.
```

And in `tests/qt_quick/test_qt_quick_table.cpp`: `"qt_quick table: a right-click opens the cell menu on that cell and selects its row alone"` — rows 1 and 3 selected, a right-click on row 2's second cell: the backend's `onSelectionChange` reports `Replace` `{2}`, the focused cell is (2, 1), and the popup's entries are the ones `setCellMenu` returned; the menu key on the focused cell opens the same menu. Mutation: the menu opened without moving focus.

Write each commented assertion exactly as stated.

- [ ] **Step 2: Run them; they fail.**

Run: `cmake --build --preset cl-debug --target morph_tests && ctest --test-dir build/cl-debug -R "table menus:" --output-on-failure`; `cmake --build build/cl-qt-debug --target morph_qt_quick_tests && ctest --test-dir build/cl-qt-debug -R "qt_quick table: a right-click" --output-on-failure`
Expected: FAIL.

- [ ] **Step 3: Implement.**
  - The lowering builds `cellMenu(row, column)`: "Copy" and "Copy with headers" (Part E's `MenuCommand::Copy` / `CopyWithHeaders`), "Filter by this value", "Exclude this value", for a column of a declared group "Filter by this value in any " + the group's label, a separator, then the document's `cellMenu` entries lowered in the row's scope with `column` (the column's id) and `value` (the cell as a document value) bound beside `row`. Its `headerEntries(column)` is the document's `headerMenu` lowered in a scope with `column`; the lowering's `menu(column)` and `onMenu` stay Part E's (`ui::menuEntries(ui::headerMenu(input))` and the built-in commands), and the mount appends and routes the application's entries as "Who composes a menu" says.
  - "Filter by this value": the column's filter becomes `{include: [{oneOf: [operandOf(cell)]}]}`; "Exclude this value": `operandOf(cell)` is appended to the column's `exclude` `oneOf` entry, made when absent; an empty cell uses `{isEmpty: true}`; both are disabled when `cellLook` is `Unloaded` or `Invalid`. The group entry sets the filter of group `<id>` to `{group: id, include: [{oneOf: [operand]}]}`, replacing that group's filter.
  - A right-click, the menu key or a long press on a cell focuses it, makes the selection that row alone when it is not selected (reported as `Replace`), and opens the menu `setCellMenu`'s `entries` returns. An entry's `keys` are bound while the table has focus and run the entry for the focused cell. The columns the entries' expressions read are fetched for every row, as `rowTone`'s are (Task 9's column analysis).
  - `headerEnabled` (a condition, default `true`) is a `Prop<bool>` on `ui::Table` and `TableOptions`: while false, the mount disables the header-menu entries whose `MenuCommand` sorts, filters or arranges, `Columns` and `SortDialog` included (all but `Copy` and `CopyWithHeaders`), and the refusal itself is the lowering's: its `onSort`, `onFilter`, `onGroupFilter`, `onColumnMove` and `onColumnResize` handlers (Part E) read `headerEnabled` when called, change nothing, and post the message in the test through `ScreenCore::tableMessage`. The mount, which cannot reach `ScreenCore`, only greys the entries. Selection, scrolling, copy and the application's entries stay.
  - `src/ui/document.cpp`: `cellMenu` and `headerMenu` take the `menu` node's entry shape and are reported as its entries are.

- [ ] **Step 4: Run them and the targets; all pass.** Step 2's commands, then all of `morph_tests`, `morph_qt_quick_tests`, `morph_tui_tests`. Expected: PASS.

- [ ] **Step 5: Mutations, docs, commit.** `docs/spec/ui/document.md`: `cellMenu`, `headerMenu`, `headerEnabled`. Backend specs: `setCellMenu`. Docs build green.

```bash
git add include/morph/ui src/ui src/qt_quick src/tui tests docs/spec
git commit -m "feat(ui): application entries in table menus and a disabled header"
```
