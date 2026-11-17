# The One Table, Part D: the Table on a Screen — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put every table of a screen on the one pipeline: `TableRows` with its two implementations, the model-backed `table` node lowered onto a `PageWindow`, persisted state loaded before the first rows, the column window in the mount and both backends, and `rows` tables ordered by the engine.

**Architecture:** `include/morph/ui/table_rows.hpp` holds `TableRows`, `EngineRows`, `WindowRows` and `ValueRowsSource`; `include/morph/table/state_store.hpp` holds the store interface and the state document's reader, writer and fitting; `include/morph/ui/table_state.hpp` and `table_state_model.hpp` hold the preferences-backed store and its server model. The interpreter lowers a table node onto a `TableRows` and hands it to the mount through a new `ui::Table::source`; the mount realizes rows by view index and, from Task 10, columns through `ColumnExtents`, cell pools and a per-frame budget. The interpreter's own sort and page are removed in Task 11.

**Tech Stack:** C++23, CMake presets `cl-debug` (core, `morph_tests`, `morph_table_tests`, `morph_net_tests`) and `cl-qt-debug` with `-DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON` (for `morph_qt_quick_tests`, `morph_tui_tests`), Catch2, glaze, Qt 6 Quick offscreen.

**Spec:** `docs/spec/table/table.md`: "The pipeline", "Sources", "A document's rows as a source", "The client table model", "The renderer" (all subsections), "Every surface on the pipeline" → "The document `table` node on the engine", "What the interpreter does for a table", "The status line" (counts), "Persisted state" (interface, "The framework's stores", load before the first rows, saving sort and filters), "Where sort and filter run"; the matching rows of "Tests" and "Invariants" (16 to 21, 26 to 28, 31); gates U1–U8, K5, K8, F1–F10, T1, T3–T5, D8, W6, B1; "Neighbouring specs" rows for steps 4, 5 and 6; "Delivery order" steps 4, 5, 6. Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md` (Tasks 9, 10, 11).

Part D of 7; index: 2026-11-17-one-table.md

## Global Constraints

- Everything in the roadmap's Global Constraints and its standard task loop applies; this plan lists only what is specific to its tasks.
- Limits, verbatim: `Virtualization{threshold = 2000, overscan = 10, columnThreshold = 40, cellThreshold = 20'000, columnOverscan = 4, realizePerFrame = 240}`; `SessionOptions::tableStateTimeout = 300 ms`, `tableStateDebounce = 500 ms`, `tableSettle = 80 ms`, `tableAwaitedRefetch = 250 ms`, `maxRangeRows = 10'000`; `TableStateOptions::segmentBytes = 12 KiB`; a `save` part and a `load` head are at most 1 MiB, an envelope 8 MiB; a key member is 1 to 256 bytes; `PageWindow(pageSize = 200, maxPages = 8, maxKeys = 1000, maxColumns = 96)`; a status-line message lasts 6 s or until the next gesture on the table; client mode is bounded at 20,000 rows or 1,000,000 cells.
- Comments and specs state what the code does now and why; no history, no issue numbers (AGENTS.md).
- Count gates are ordinary tests. Timings carry the tag `[.benchmark]`, never gate CI, and are written into table.md's Performance tables with machine and load.
- No GUI application is launched. Qt Quick behaviour is tested only through the offscreen `morph_qt_quick_tests` (`QT_QPA_PLATFORM=offscreen`).
- Neutral domain only: test models and documents use `Lims_SampleModel`, table id `samples`.
- `table::UnitInfo` (Part C) and `ui::UnitInfo` (`ui/expression.hpp:307`) are different types with one name: code in `namespace morph::ui` always writes `table::UnitInfo`, `table::UnitRegistry`.
- `table::readGenericPage(json, description)` is called with no registry: Part A resolves a page's units through `registryOf(description)`.
- Qt Quick and terminal suites need their options; configure once with
  `cmake --preset cl-qt-debug -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON` and build/run there:
  `cmake --build --preset cl-qt-debug --target morph_qt_quick_tests morph_tui_tests`,
  `ctest --test-dir build/cl-qt-debug -L qt_quick -R "<pattern>" --output-on-failure` (the `qt_quick` label sets `QT_QPA_PLATFORM=offscreen`), `ctest --test-dir build/cl-qt-debug -R "<pattern>" --output-on-failure` for `morph_tui_tests`.

## Interfaces assumed from Parts A–C

The roadmap names these; it does not pin their exact signatures. This plan uses the spec's signatures where the spec gives them and the shapes below where it does not. An implementer finding a different shape in the landed code uses the landed one and fixes this list.

| From | Used as |
|---|---|
| Task 0, 2 | `bench::WideTableOptions`, `bench::WideTableSource`; `bench::WideTableModel` (registered type `bench::kWideModelType`, `"WideTableModel"`, set up with `WideTableModel::configure(WideTableOptions wide, WideTableOptions small)`), so the fixture's records are `WideTableModel.wide` and `WideTableModel.small`; the library `morph_wide_table_model` linked into `morph_tests`, `morph_net_tests` and `morph_qt_quick_tests`; B5's browser harness `tests/qt_quick/wasm/` run with `emrun`. |
| Task 1 | `TableQuery::columns` (`std::optional<std::vector<std::string>>`, absent: every column; cross-part decision 2), `table::TableColumn`, `table::TableDescription` (with `capabilities`, as the spec's block has it), `table::GenericRow`, `table::GenericPage`; `table::readGenericPage(std::string_view, TableDescription const&) -> std::expected<GenericPage, TableError>`; glaze metadata for `TableDescription` so a reply's JSON reads with `glz::read_json`. |
| Task 2 | Action ids `Table.<id>` and `Table.<id>.describe`; the record `<model type>.<id>` (here `Lims_SampleModel.samples`); its catalog entry marks `Table.<id>` opaque (`ActionRecordInfo::opaque`). |
| Task 4 | `TableQuery::keys` (`std::optional<std::vector<RowId>>`), `table::UnloadedLevels{std::span<std::string const> levels; std::span<std::string const> failed;}` (views into the window: copy before the window changes); `GenericPage::{unloadedRows, undecided, awaiting, failed, indices}`. |
| Task 5 | `table::rowIdOf(glz::raw_json const&) -> std::optional<RowId>` (and its inverse `changeKeys(std::span<RowId const>) -> std::vector<glz::raw_json>`, not used here). `effects::RecordChange::keys` absent is a whole-type change. |
| Task 7 | `Session::watchRecord`, `CallOptions::stop` and `Session::read(channel, actionId, body, stop) -> async::Completion<std::string>` (7b; resolves with the reply's JSON text, cross-part decision 30; `WindowRows` sends every fetch through it, see 9a's "Decoding replies"), and `PageFetch`, `RowsFetch`, `Refresh`, `RefreshPlan` and the `PageWindow` members exactly as table.md "The window under changes" and "The client's caches" declare them; `PageWindow<GenericRow>(pageSize, maxPages, maxKeys, maxColumns)` (no key function), `bool accept(PageFetch const&, GenericPage)` (`false`: ignored), `std::vector<std::int64_t> patch(RowsFetch const&, GenericPage)`, and the accessors of cross-part decision 8: `RowId const* key(std::int64_t)`, `Cell const* cell(std::int64_t, std::string_view column)`, `bool holds(std::int64_t, std::string_view column)`, `std::uint64_t revision()`; plus `unloadedLevels(index)`, `undecided()`, `awaiting()`, `failed()`, `total()`, `totalKind()`, `error()`, `pending()`. `WindowRows` reads rows only through these. |
| Task 8d | `ColumnLayout::value() -> table::ColumnLayoutValue{order, hidden, pinned, widths, groups, units}` and `apply(ColumnLayoutValue const&)` (cross-part decision 7); its JSON is the spec's `layout` shape. |
| Task 15 | `ui::formatCell(table::Cell const&, table::TableColumn const&, ui::FormatContext const&, DisplayUnits const& = {}) -> std::string` (cross-part decision 9), which replaces this plan's stand-in. |

## Review Focus

1. **A description that changes revision while state is held** (roadmap item 1). A sort key, a filter and the fetched columns naming column `c` when a page reply carries a new `revision` whose description lacks `c`: the client refetches the description once, drops the key and the filter (reported once each), and the next query names neither `c` nor its level. A view selection and a cell draft do not exist before Tasks 17a and 24; those tasks extend this test. Test: Task 9d.
2. **The table unmounted while its state load or a fetch is in flight.** A screen closed before a held load completes, then the load released: nothing is applied, no query is sent, no handler runs on a destroyed table, and the debounced save of a changed state goes out at once on unmount. Test: Task 9d.
3. **A stored document that is not JSON, or of a version the framework does not know.** The table starts from its defaults, the problem is reported once at the table's path, nothing is written until the user changes something, and then the framework's version overwrites it. Test: Task 9b.
4. **An empty model-backed view.** Before the first reply, and while rows are undecided, the node's `empty` content is not shown and the status line says the table is loading or counts "0 so far, N loading"; after a reply with `total: 0, totalKind: exact, undecided: 0` it is shown and the count reads "0 rows". Test: Task 9d.
5. **Rows with a duplicate key in a `rows` table.** Three rows with keys 1, 2, 1: the table shows two rows, the first occurrence of key 1 in the engine's order, the duplicate is reported as the mount reports it today (`site::kDuplicateKey`, once per duplicate), and a sort never brings the second row back. Test: Task 11a.

---

## File map

| File | Task | Holds |
|---|---|---|
| `include/morph/ui/table_rows.hpp` (new) | 9a, 11a | `SortChain`, `TableTotal`, `TableRowRef`, `TableCountsView`, `TableRows`, `EngineRows`, `WindowRows`, `WindowRowsOptions`, `WindowRowsStats`, `cellValue`, `countsText`, the stand-in cell text, `RowRecord`, `RecordOf`; `ValueRowsSource`, `kGroupRunColumn`, `EngineRows::setRestoring` (11a, 11b); `readTypedPage`, `WindowRowsOptions::{action, queryMember}` (11c) |
| `include/morph/table/state_store.hpp` (new) | 9b | `TableStateKey`, `StoredTableState`, `SaveOutcome`, `SaveResult`, `ITableStateStore`, `MemoryTableStateStore`, `StateDocument`, `readState`, `writeState`, `fitState`, `FitReport` |
| `include/morph/ui/table_state.hpp` (new) | 9c | `PreferencesTableStateStore`, `TableStates` (the session's kept states, load batching, save debounce) |
| `include/morph/ui/table_state_model.hpp` (new) | 9c | `TableStateModel` (`morph.tableState`), its request types, `TableStateOptions`, `registerTableState` |
| `include/morph/ui/view.hpp` | 9d, 10a | `TableSource`, `TableRowLook`, `CellLoad`, `Table::source`, `Table::status`, `TableStatus`, `TableColumn::id` (9d); `Virtualization` additions, `Table::arrangement`, `Table::highlightKey` (10a) |
| `include/morph/ui/mount.hpp` | 9d, 10a | rows by view index from `TableRows`; `ColumnExtents`, `windowingOf`, the column window, cell pools, the frame budget, focus by key and column |
| `include/morph/ui/backend.hpp` | 9d, 10a | `TableWidget::setStatus`, `setColumnWindow`, `setOnHorizontalRange`, `setOnSelectRange`, `setFocusCell`, `setCounts` |
| `include/morph/ui/testing/recording_backend.hpp` | 9d, 10a | records the new table calls; `scrollTableColumns`, `selectTableRange` |
| `include/morph/ui/interpreter.hpp`, `src/ui/interpreter.cpp` | 9d, 10a, 11b, 11c | `SessionOptions` members; the lowering of both table kinds onto `TableRows`; `RtScope::onMissingRowField` |
| `src/qt_quick/table.cpp`, `src/qt_quick/qml/MorphTable.qml` | 10b | the column window, pinned columns, pools, the status line |
| `src/tui/table_widget.cpp`, `.hpp` | 10b | the column range, the status line |
| `tests/ui_table_support.hpp` (new) | 9a | the test catalog, description and page builders, `GatedTableStateStore` |
| `tests/test_ui_table_model.cpp` (new), `tests/test_ui_table_state.cpp` (new) | 9 to 11 | in `morph_tests` |
| `tests/table/test_table_state.cpp` (new) | 9b | in `morph_table_tests` |

---

### Task 9: `TableRows` and the model-backed table on a screen (table.md step 4)

Four commits, each its own reviewer gate.

#### 9a — `TableRows`, `EngineRows` and `WindowRows`

**Files:**
- Create: `include/morph/ui/table_rows.hpp`, `tests/ui_table_support.hpp`, `tests/test_ui_table_model.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_ui_table_model.cpp`, and in 9c `test_ui_table_state.cpp`, to `add_executable(morph_tests …)`), `include/morph/ui/interpreter.hpp` (`SessionOptions` members below)

**Interfaces:**
- Consumes: Task 7's `PageWindow<GenericRow>`, `Session::read` (the reply's JSON text on the owner; no cache entry; a stop settles it cancelled), `Session::watchRecord`; Task 1's description and generic page; Task 5's `rowIdOf`; `table::Engine` (existing: `setSort`, `setFilter`, `viewRowCount`, `rowIdAt`, `sourceRowOf`, `onViewChange`, `pending`).
- Produces (namespace `morph::ui`, every member as table.md "The client table model" declares it):

```cpp
using SortChain = std::vector<table::SortKey>;
struct TableTotal;          // rows, kind, known
struct TableRowRef;         // key, row
struct TableCountsView;     // view, source, undecided, pending, awaiting, failed
class TableRows;            // total, row, unloadedLevels, visible, keysOf, setSort, setFilter, counts, onChange

/// A row's key and its record, owned (cross-part decision 28).
struct RowRecord { table::RowId key; Value row; };
/// What EngineRows reads a row's key and record through: its source's snapshot and a source row.
using RecordOf = std::function<RowRecord(table::RowSnapshot const& snapshot, std::size_t sourceRow)>;
class EngineRows final : public TableRows {
public:
    EngineRows(std::shared_ptr<table::DataSource> source, RecordOf recordOf, table::EngineOptions options);
    [[nodiscard]] table::Engine& engine() noexcept;
    [[nodiscard]] std::size_t recordsBuilt() const noexcept;   // calls of recordOf, for the memo's test
};

struct WindowRowsOptions {
    std::shared_ptr<ModelChannel> channel;      // the model's channel
    std::string table;                          // the table id: actions "Table.<table>", "Table.<table>.describe"
    std::string record;                         // "<model type>.<table>", watched for the window's life
    Value body;                                 // the node's `body`, the query's fixed part
    std::int64_t pageSize = 200;
    std::size_t maxPages = 8;
    std::size_t maxKeys = 1000;
    std::size_t maxColumns = 96;
    bool restoring = false;                     // sends no query until `release`
};
struct WindowRowsStats {
    std::size_t pageFetches = 0;                // page and column fetches sent
    std::size_t rowsFetches = 0;                // keyed fetches sent
    std::size_t cancelled = 0;
    std::size_t cellsConverted = 0;             // cells turned into a row record's fields (K8)
};
class WindowRows final : public TableRows {
public:
    WindowRows(Session& session, WindowRowsOptions options);
    void setDescription(table::TableDescription description);   // fits sort, filters and columns to it
    [[nodiscard]] table::TableDescription const* description() const noexcept;
    void onStaleDescription(std::function<void()> handler);     // a reply named another revision
    void release();                                             // leaves the restoring state: the first query
    void retry();                                               // the query again with `TableQuery::retry`
    [[nodiscard]] std::optional<table::TableError> const& error() const noexcept;
    [[nodiscard]] WindowRowsStats const& stats() const noexcept;
    // Added by Part G's Task 24 (cross-part decision 35), not by this part:
    // void onRowsAbsent(std::function<void(std::span<table::RowId const>)> handler);  // keys a keyed fetch named and its reply holds no row for
};

/// A cell as the document reads it.
[[nodiscard]] Value cellValue(table::Cell const& cell, table::ColumnInfo const& column);
/// The status line's counts part.
[[nodiscard]] std::string countsText(TableCountsView const& counts, std::int64_t selected, FormatContext const& format);
namespace detail {
/// The text of a cell until `ui::formatCell` lands (Task 15 replaces every call).
[[nodiscard]] std::string standInCellText(table::Cell const& cell, table::TableColumn const& column);
}
```

- `SessionOptions` gains `std::chrono::milliseconds tableSettle{80}`, `std::chrono::milliseconds tableAwaitedRefetch{250}` (this commit) and, in 9c, `std::shared_ptr<table::ITableStateStore> tableState`, `std::chrono::milliseconds tableStateTimeout{300}`, `std::chrono::milliseconds tableStateDebounce{500}`, `std::string application` (the `application` of every table-state key).

Decisions the signatures do not carry:

- `EngineRows` memoises `RecordOf`'s result per source row of the engine's `viewSnapshot()`; the memo is dropped for the rows an `Updated` names and whole on a new snapshot of `Reset`. `row(index)` returns a `TableRowRef` whose references point into the memo and hold until the next `onChange`.
- Decoding replies. Every page, keyed and column fetch is sent as `session.read(options.channel, "Table." + table, body, token)` (cross-part decision 30): no cache entry is made, no effect is applied, and the fetch's stop token cancels it. `Session::read` resolves with the reply's **text**, which `WindowRows` decodes as `table::readGenericPage(text, *description())`; a reply is never read into a `Value`, because `ui::valueFromJson` (`value.hpp:570`) refuses every JSON number with a fraction or an exponent ("a number with a fraction is not exact"), so a page holding one `Number` cell (`2.5`, `1e308`, `-0.0`) would fail whole. The description is read from the `Table.<table>.describe` query entry (W6), whose `Value` is lossless for a description — every number in one is an integer or a `math::Rational` written as `{"num","den","dp"}` (`rational.hpp:1747`), which `valueFromJson` reads as a `Decimal` and `valueToJson` writes back unchanged — as `glz::read_json<table::TableDescription>(ui::detail::canonicalJson(value))` (`interpreter.hpp:410`). A description that does not read sets `error()` to `TableError{.code = InvalidValue}` and sends no page fetch.
- `cellValue`: empty and unloaded → `null`; `int64` in `Integer`/`Key` → `Int`; in `Date` → the `String` `YYYY-MM-DD`; in `DateTime` → `Timestamp` of seconds × 1000 ms; `Rational` → `Decimal{rational}` (its places kept); `double` → `Decimal` parsed by `table::parseDecimal` from `std::to_chars`'s shortest text, a non-finite one `null`; text → `String`; `bool` → `Bool`; `QuantityCell` → `Decimal` of `amount × toCanonical`; a `Measure` (once Task 8c lands) → `Quantity{amount, unit id}`.
- `standInCellText` is `table::displayText(cell)`, and the empty string for an unloaded cell.
- `countsText`, numbers grouped with `format.numeric`'s separator: nothing known (`!view.known`) → `""`; `undecided > 0` → `"<view> so far, <undecided> loading"`; `selected > 0` → `"<selected> of <view> selected"`, `<view>` followed by `" or more"` under `AtLeast`; `AtLeast` → `"<view> or more"`; otherwise `"1 row"` or `"<view> rows"`.
- `WindowRows` row records: `row(index)` returns a record holding one field per column of the latest `visible(…, columns)` set for which `PageWindow::holds(index, column)`, each read with `cell(index, column)` and its key with `key(index)`, memoised per key until the row's cells change; `cellsConverted` counts each conversion. A record of a 300-column table of which twenty are named converts twenty cells.
- `WindowRows` timing: a fetch for a page adjacent to a held page goes at once; any other waits until `visible` has stayed within one page for `tableSettle`, measured on `SessionOptions::scheduler`; every `superseded()` fetch is cancelled through its stop token and handed to `cancelled`. While `awaiting()` is not empty, the first refetch after an event goes at once and the next no sooner than `tableAwaitedRefetch` after it.
- `WindowRows` order of work: `watchRecord` first; the first page fetch only after it resolves, after `setDescription`, and (with `restoring`) after `release`. Its handler maps a change with `keys` to `refresh` of each key read by `rowIdOf` (a key that does not read invalidates), and a change without `keys` to `invalidate()`; after every `accept` it sends `owed()` and calls `visible` again.
- A reply whose `revision` differs from the held description's calls the `onStaleDescription` handler once per revision and is still accepted.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp`, over `Harness` and the helpers of `tests/ui_table_support.hpp`: `tableCatalog()` (the corpus catalog plus `Table.samples` and `Table.samples.describe` on `Lims_SampleModel`), `samplesDescription(int columns)` (`id` Integer key, `name` Text, then `c3…` Decimal), `describeJson(TableDescription)`, `pageJson(offset, count, total, columns, TotalKind kind = Exact, std::int64_t undecided = 0, std::vector<std::string> awaiting = {})` whose row keys are `offset + i` and names `"row-<key>"`; `formatFor(std::string_view tag) -> FormatContext`, built from the locale table as `Session::scopedFormatting` builds one (`interpreter.cpp:777`): `LocaleFacts const& f = ui::localeFacts(tag);` then `FormatContext{.locale = std::string{tag}, .numeric = f.numeric, .currency = f.currency, .datePattern = std::string{f.datePattern}, .dateTimePattern = std::string{f.dateTimePattern}}` (`f.numeric` views static storage, `locale_table.hpp:78`). A default `FormatContext` has an empty `groupSeparator` (`render::NumericLocale{}`), and so does a `Session` until `setLocale` runs (`interpreter.hpp:598`), so `Harness`'s constructor calls `session.setLocale("en")`: every count asserted through a screen ("9,093 rows", "100,000 rows") is grouped by the locale table, not by a default. `h.columnIds(first, count)` is the ids of the description's columns in order; `h.window(rows)` is a test accessor to the `PageWindow` (`WindowRows` befriends `ui_table_support`'s `WindowRowsProbe`).

```cpp
// Mutation: send a fetch per `visible` call (K6 fails: 40 page calls).
TEST_CASE("window rows: a scroll that passes sends no fetch until it rests", "[ui][table]") {
    Harness h;  WindowRows rows = h.windowRows("samples");           // helper: options, description, release
    h.answerFirstPage(rows, /*total*/ 100'000);
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t at = 0; at < 100'000; at += 2'500) { rows.visible(at, 30, h.columns); h.advance(10ms); }
    CHECK(h.dispatcher.count("Table.samples") == before);           // inside tableSettle each time
    h.advance(80ms);
    CHECK(h.dispatcher.count("Table.samples") <= before + 3);       // K6: at most three sent
    CHECK(rows.stats().cancelled + 3 >= rows.stats().pageFetches - 1);
    rows.visible(99'990, 30, h.columns);                            // the next page from a held one: at once
}
// Mutation: a refetch per event (ten fetches).
TEST_CASE("window rows: an awaited order is throttled", "[ui][table]") {
    // first page replies with awaiting = {"recipe"}; ten keyed events within 100 ms
    CHECK(fetchesAfterEvents == 1);                                 // one at once
    h.advance(250ms);  CHECK(fetchesAfterEvents == 2);              // one after tableAwaitedRefetch
}
// Mutation: build the record from every column the window holds for the row (converts 40, not 20).  Gate K8.
TEST_CASE("window rows: a row of 300 columns converts the twenty it is asked for", "[ui][table]") {
    Harness h;  WindowRows rows = h.windowRows("samples", samplesDescription(300));
    std::vector<std::string> const forty = h.columnIds(0, 40);      // id, name, c3 … c40
    std::vector<std::string> const twenty = h.columnIds(0, 20);     // the first twenty of them
    rows.visible(0, 50, forty);
    h.replyPage(pageJson(0, 200, 9093, forty));                     // the window now holds 40 columns of row 0
    REQUIRE(h.window(rows).holds(0, "c40"));                        // the precondition K8 needs: 40 held
    rows.visible(0, 50, twenty);                                    // held already: no fetch
    REQUIRE(rows.row(0).has_value());
    CHECK(rows.row(0)->row.asRecord()->size() == 20);
    CHECK(rows.stats().cellsConverted == 20);                       // K8; the mutation converts 40
    static_cast<void>(rows.row(0));  CHECK(rows.stats().cellsConverted == 20);  // memoised
}
// Mutation: decoding through a `Value` (the text read by `ui::valueFromJson`, written back with `valueToJson`, then `readGenericPage`): valueFromJson refuses 2.5 and the page fails whole.
TEST_CASE("window rows: a page with a Number cell is read from the reply's text", "[ui][table]") {
    // description: id Integer key, ratio Number
    h.replyPage(R"({"revision":"r1","columns":["id","ratio"],"keys":[1,2],"rows":[[1,2.5],[2,1e308]],"total":2})");
    REQUIRE_FALSE(rows.error().has_value());
    CHECK(rows.row(0)->row.field("ratio")->asDecimal() != nullptr);  // cellValue: double → Decimal
    CHECK(rows.total().rows == 2);
}
// Mutation: send the first page before `release`.
TEST_CASE("window rows: a restoring window sends no query until released", "[ui][table]") {
    CHECK(h.dispatcher.count("Table.samples") == 0);
    rows.setSort({{.column = "name", .dir = table::SortDirection::Descending}});
    rows.release();
    CHECK(h.dispatcher.count("Table.samples") == 1);                // T3: one query, carrying the sort
    CHECK(h.dispatcher.calls.back().body.contains(R"("column":"name")"));
}
// Mutations: countsText ignoring `undecided`; countsText grouping with a fixed "," (the "de" line fails).
TEST_CASE("status line: counts for exact, lower-bound and undecided totals", "[ui][table]") {
    using enum table::TotalKind;
    FormatContext const en = formatFor("en");
    FormatContext const de = formatFor("de");
    REQUIRE(en.numeric.groupSeparator == ",");                      // the helper took the locale table's row
    CHECK(countsText({.view = {9093, Exact, true}}, 0, de) == "9.093 rows");
    CHECK(countsText({.view = {9093, Exact, true}}, 0, en) == "9,093 rows");
    CHECK(countsText({.view = {100000, AtLeast, true}}, 0, en) == "100,000 or more");
    CHECK(countsText({.view = {12, AtLeast, true}, .undecided = 40}, 0, en) == "12 so far, 40 loading");
    CHECK(countsText({.view = {9093, Exact, true}}, 3, en) == "3 of 9,093 selected");
    CHECK(countsText({.view = {1, Exact, true}}, 0, en) == "1 row");
    CHECK(countsText({}, 0, en).empty());
}
// Mutation: EngineRows::keysOf completing on a later turn.
TEST_CASE("engine rows: every row is present, visible does nothing, keysOf completes at once", "[ui][table]") {}
// Mutation: no memo (recordsBuilt grows by the view size on every read).
TEST_CASE("engine rows: a row's record is built once until the row changes", "[ui][table]") {
    static_cast<void>(rows.row(0)); static_cast<void>(rows.row(0));  CHECK(rows.recordsBuilt() == 1);
    patchRow(source, 0);  settle();  static_cast<void>(rows.row(0));  CHECK(rows.recordsBuilt() == 2);
}
```

- [ ] **Step 2: Run to verify they fail**
Run: `cmake --build --preset cl-debug --target morph_tests` → FAIL: `ui/table_rows.hpp` not found.

- [ ] **Step 3: Implement** the interfaces above in `include/morph/ui/table_rows.hpp` and the `SessionOptions` members in `interpreter.hpp`, with complete Doxygen.

- [ ] **Step 4: Run to verify they pass**
Run: `ctest --test-dir build/cl-debug -R "window rows|engine rows|status line" --output-on-failure` → all pass; then the whole `morph_tests` target green.

- [ ] **Step 5: Mutations** (each named above): apply, see the named case fail, revert; paste each failing line into the commit body.

- [ ] **Step 6: Commit** `feat(ui): table rows over the engine and over a page window`.

#### 9b — The store interface and the state document

Needs Task 8c: `fitState` reads `TableDescription::units` directly.

**Files:**
- Create: `include/morph/table/state_store.hpp`, `tests/table/test_table_state.cpp`
- Modify: `tests/CMakeLists.txt` (`table/test_table_state.cpp` in `morph_table_tests`), `tests/table/bench_table.cpp` (T1)

**Interfaces:**
- Consumes: `table::ColumnLayoutValue` and `ColumnLayout::apply` (8d), `table::TableDescription`, `table::FilterSpec`, `table::compileFilter` (existing compile path in `filter.hpp`).
- Produces (namespace `morph::table`): `TableStateKey`, `StoredTableState`, `SaveOutcome`, `SaveResult`, `ITableStateStore` exactly as table.md "Persisted state" declares them (`shares()` included, default `false`), and:

```cpp
class MemoryTableStateStore final : public ITableStateStore {
public:
    explicit MemoryTableStateStore(exec::IExecutor& executor);  // completions delivered there
    // revisions are "1", "2", … per key. Part E's Task 14a adds a defaulted second parameter, giving
    // `explicit MemoryTableStateStore(exec::IExecutor&, Sharing = Sharing::Private)`; every call here stays valid.
};

inline constexpr int kStateVersion = 1;
struct StateDocument {                  // part "state"
    int version = kStateVersion;
    std::optional<ColumnLayoutValue> layout;
    std::optional<SortChain> sort;
    std::optional<FilterSpec> filters;
    std::optional<std::string> view;
    CarriedState carried;               // what the caller's description lacks, written back as it was
};
/// Parts of a stored state that name a column or group the caller's description lacks. Not applied; merged back
/// by `writeState` into the places they had.
struct CarriedState {
    struct Column {
        std::string id;
        std::string after;              // the id it followed in the stored `order`; empty: it was first
        bool hidden = false;
        std::optional<int> width;
        std::map<std::string, std::string> units;   // dimension -> unit id, as stored
    };
    std::vector<Column> columns;        // in stored order
    std::vector<std::pair<std::size_t, SortKey>> sort;   // each key with its index in the stored chain
    std::map<std::string, ColumnFilter> filters;         // by the absent column's id
    std::vector<GroupFilter> groupFilters;               // naming an absent group
    std::vector<ColumnGroupInfo> groups;                 // user groups that name an absent column
    [[nodiscard]] bool operator==(CarriedState const&) const = default;
};
struct FitDrop {
    enum class What : std::uint8_t { Column, SortKey, Filter, GroupFilter, Group, Unit };
    What what;
    std::string id;                     // the column, group or "column/dimension" of a unit
    bool carried = false;               // kept in `carried` (the description lacks it) or gone
    [[nodiscard]] bool operator==(FitDrop const&) const = default;
};
struct FitReport { std::vector<FitDrop> dropped; };   // in the order of the fitting list
[[nodiscard]] std::expected<StateDocument, std::string> readState(std::string_view json);   // migrates forward
[[nodiscard]] std::string writeState(StateDocument const& document);
[[nodiscard]] StateDocument fitState(StateDocument document, TableDescription const& description, FitReport& report);
```

Decisions:
- `readState` refuses text that is not JSON and a `version` above `kStateVersion` with a message; a lower version is migrated one version at a time (none exist yet, so the migration table is empty and its loop is tested by a version-0 fixture added when a version 2 exists — not now).
- `fitState` applies table.md's fitting list in order: columns as `ColumnLayout::apply` fits them; sort keys on missing or unsortable columns dropped; filters on missing columns or groups, filters that do not compile for the column's kind, and units not in `description.units` dropped; user groups left with fewer than two columns dropped with their filter. A drop because the description lacks the column or group moves the entry into `carried`; any other drop is gone. A user group that names an absent column is carried whole (it is whole again when the column returns); one left with fewer than two present columns for any other reason is gone with its filter. A user-formed group's filter is a `GroupFilter` with `group` empty and `columns` set to the group's columns (Part C's `compileGroup` takes exactly one of `group` and `columns`), so `fitState` finds the filter of a user group by `columns` equal to that group's `columns` (as sets), and moves or drops it with the group; a `GroupFilter` naming a declared `group` is fitted against `description.groups` by id.
- `writeState` merges `carried` back: a column into `order` right after its `after` id, or first when `after` is empty, or at the end when `after` is itself absent from the written order; its hidden flag, width and units with it; a sort key at its stored index, clamped to the chain's length; filters, group filters and groups appended. Reading that text with the same description carries the same entries again.
- The lowering reports, once each at the table's path, the drops the spec says are reported: `Filter`, `GroupFilter` and `Unit`. `Column`, `SortKey` and `Group` drops are not reported.

- [ ] **Step 1: Write the failing tests**

```cpp
// Mutation: fitState returning the document unchanged.
TEST_CASE("table state: stale state is fitted", "[table][state]") {
    // id, name, weight (a Measure column of dimension "mass"; units lists g and kg), declared group g1 = {name, alias}
    TableDescription const d = describedSamples();
    StateDocument const stored = readState(R"({"version":1,
      "layout":{"order":["id","gone","name","weight"],"hidden":["id","name","weight"],"pinned":2,"widths":{"gone":9},
                "units":{"weight":{"mass":"zz"}}},
      "sort":[{"column":"gone","direction":"asc"},{"column":"name","direction":"desc"}],
      "filters":{"columns":{"gone":{"include":[{"eq":"x"}]},"weight":{"include":[{"gt":"abc"}]}},
                 "groups":[{"group":"g2","columns":["a","b"]}]}})").value();
    FitReport report;
    StateDocument const fitted = fitState(stored, d, report);
    CHECK_FALSE(fitted.layout->order.empty());
    CHECK(fitted.layout->hidden.size() < 3);                              // one column at least is visible
    CHECK(*fitted.sort == SortChain{{.column = "name", .dir = SortDirection::Descending}});
    CHECK(fitted.filters->columns.empty());
    CHECK(fitted.filters->groups.empty());
    CHECK_FALSE(fitted.layout->units.contains("weight"));                 // unknown unit "zz" dropped
    using W = FitDrop::What;
    CHECK(report.dropped == std::vector<FitDrop>{
        {W::Column, "gone", true}, {W::SortKey, "gone", true}, {W::Filter, "gone", true},
        {W::Filter, "weight", false}, {W::GroupFilter, "g2", true}, {W::Unit, "weight/mass", false}});
    CHECK(fitted.carried.columns == std::vector<CarriedState::Column>{{.id = "gone", .after = "id", .width = 9}});
    StateDocument const again = readState(writeState(fitted)).value();
    CHECK(again.layout->order == std::vector<std::string>{"id", "gone", "name", "weight"});   // its place kept
    CHECK(again.sort->at(0).column == "gone");                            // carried key back at index 0
    CHECK(again.filters->columns.contains("gone"));
    CHECK_FALSE(again.filters->columns.contains("weight"));               // did not compile: gone at the next save
}
// Mutation: fitState looking up a user group's filter by `group` id (an empty id finds nothing: the filter stays).
TEST_CASE("table state: a user group and its filter are fitted together", "[table][state]") {
    // layout.groups: u1 = {name, gone}, u2 = {name, alias}; filters.groups: {columns [name, gone]}, {columns [name, alias]}
    // describedSamples() has id, name, alias, weight and lacks "gone": u1 and its filter are carried, u2 and its stay.
    FitReport report;  StateDocument const fitted = fitState(userGroups(), describedSamples(), report);
    CHECK(fitted.filters->groups.size() == 1);                            // u2's, matched by its columns
    CHECK(fitted.carried.groups.size() == 1);  CHECK(fitted.carried.groupFilters.size() == 1);   // u1's
    CHECK(fitted.carried.groupFilters.front().group.empty());
}
// Review Focus 3. Mutation: readState accepting an unknown version.
TEST_CASE("table state: a document that is not JSON or of an unknown version is refused", "[table][state]") {
    CHECK_FALSE(readState("{not json").has_value());
    CHECK_FALSE(readState(R"({"version":2,"sort":[]})").has_value());
    CHECK(readState(R"({"version":1})").has_value());
}
// Mutation: save ignoring `expected`.
TEST_CASE("memory table state store: a save with a stale revision is a conflict", "[table][state]") {
    // save "a" → Saved rev "1"; save "b" expected "1" → Saved "2"; save "c" expected "1" → Conflict, revision "2"
}
```
T1 benchmark: `"table state benchmark: parse, fit and apply 300 columns, 8 keys, 32 filters"` tagged `[.benchmark][table][state]`; it asserts first that every one of the 8 keys and 32 filters survives fitting.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_table_tests` → FAIL: `state_store.hpp` not found.
- [ ] **Step 3: Implement** `state_store.hpp` as above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "table state|memory table state" --output-on-failure`; `morph_table_tests` green.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Commit** `feat(table): the table state store and the state document`.

#### 9c — The preferences-backed store and the session's kept states

**Files:**
- Create: `include/morph/ui/table_state.hpp`, `include/morph/ui/table_state_model.hpp`, `tests/test_ui_table_state.cpp`
- Modify: `include/morph/ui/interpreter.hpp` (`SessionOptions` members listed in 9a; `Session::tableStates() -> TableStates*`, null without a store), `tests/net/test_push_tables.cpp` ("The person is the store's")

**Interfaces:**
- Consumes: `IPreferencesStore`, `PrefKey` (existing), `session::current()` as `PreferencesModel` reads it, `morph::session::detail::sha256`, `Dispatcher`/`ModelChannel` (existing), 9b's interface.
- Produces:

```cpp
// table_state_model.hpp, namespace morph::ui
inline constexpr std::string_view kTableStateModel = "morph.tableState";
struct TableStateOptions {
    std::size_t segmentBytes = std::size_t{12} * 1024;
    std::size_t partBytes = std::size_t{1} << 20;            // a save part and a load head
    std::optional<std::size_t> maxBytesPerPerson;            // unset: no quota
};
struct TableStateLoad   { std::vector<table::TableStateKey> keys; };
struct TableStateHead   { bool found = false; std::string revision; std::uint64_t size = 0; std::string text; };
struct TableStateLoaded { std::vector<TableStateHead> entries; };              // one per key, in order
struct TableStateRead   { table::TableStateKey key; std::string revision; std::uint64_t offset = 0; };
struct TableStateChunk  { std::string revision; std::string text; };           // another revision: start again
struct TableStateSave   { table::TableStateKey key; std::string expected; std::string upload;
                          std::uint64_t offset = 0; std::string text; bool last = true; };
struct TableStateSaved  { table::SaveResult result; };
struct TableStateRemove { table::TableStateKey key; };
struct TableStateRemoved{ bool removed = false; };
class TableStateModel;  // execute(TableStateLoad), execute(TableStateRead), execute(TableStateSave), execute(TableStateRemove)
void registerTableState(std::shared_ptr<IPreferencesStore> store, model::detail::ActionDispatcher& dispatcher,
                        model::detail::ModelRegistryFactory& registry, TableStateOptions options = {});

// table_state.hpp
class PreferencesTableStateStore final : public table::ITableStateStore {
public:
    PreferencesTableStateStore(Dispatcher& dispatcher, exec::IExecutor& owner);
    [[nodiscard]] bool shares() const override { return true; }
};
enum class StateParts : std::uint8_t { Layout = 1, Sort = 2, Filters = 4, View = 8 };   // a bit set
class TableStates {               // one per Session with a store
public:
    TableStates(std::shared_ptr<table::ITableStateStore> store, reactive::Scheduler* scheduler,
                std::chrono::milliseconds debounce);
    [[nodiscard]] std::optional<table::StoredTableState> kept(table::TableStateKey const& key) const;
    async::Completion<std::optional<table::StoredTableState>> load(table::TableStateKey const& key);
    /// Marks @p parts of the key's state changed to those of @p document; a debounced save, one in flight per key.
    void changed(table::TableStateKey const& key, table::StateDocument const& document, StateParts parts);
    void flush(table::TableStateKey const& key);                       // unmount: save now
    void onSaveFailed(std::function<void(table::TableStateKey const&, table::SaveResult const&)> handler);
    /// The store it loads from and saves to, for a part this class does not debounce (Part E's `views`, 14b).
    [[nodiscard]] table::ITableStateStore& store() const noexcept;
    /// Replaces the session's kept copy of @p key after a save made through `store()`, so a later mount restores it.
    void keep(table::TableStateKey const& key, table::StoredTableState state);
};
```

Decisions:
- Server layout per table.md "The framework's stores": person from the verified session (none: `load` answers `found = false` for every key, `save` and `remove` are refused); `app` = the key's `application`; scope `morph.table.` + the first 32 lower-case hex digits of `sha256(model + "\x1F" + table + "\x1F" + qualifier)`; point `<part>` holds `{"key": {…in clear…}, "revision": r, "document": …}` for a document of at most `segmentBytes`, else `{"key": …, "revision": r, "segments": n, "bytes": b}` with segments at `<part>.<r>.<i>`, each a JSON string of at most `segmentBytes`. A head whose clear key differs from the asked key reads as not found. A missing segment fails the load.
- Write order, with an intent entry because `IPreferencesStore` has only `get`, `set` and `remove` by exact key (`preferences.hpp:62-73`) and so cannot list the segments under a scope: (1) read `<part>.pending`; if present, it names up to two revisions, `{"revision": r, "segments": n, "previous": r0, "previousSegments": n0}`; read the head, and remove the segments `<part>.<x>.<0…count−1>` of each named revision the head does not name, then remove `<part>.pending`; (2) when the new document or the one the head now names is segmented, write `<part>.pending` naming the new revision (`segments` 0 for an unsegmented one) and the head's (`previousSegments` 0 likewise); (3) the segments; (4) the head; (5) remove the previous revision's segments; (6) remove `<part>.pending`. A save stopped before (4) leaves the old head and its segments readable and the new segments stray; one stopped between (4) and (6) leaves the previous revision's segments stray; in both cases the next save of the key removes exactly the strays in step (1), because the head names the one revision to keep. A save that writes no segments and replaces none writes no intent. A removal that fails in steps (5) or (6) does not fail the save, whose head is written: it leaves strays that the intent names, for the next save.
- Key bounds: `application`, `table`, `qualifier` and `part` are 1 to 256 bytes; `model` is 0 to 256 bytes, because a table over a document's rows has an empty `model` (table.md "Persisted state", the key's comment, and Task 11b). **Stated deviation:** table.md's "Bounds" bullet says "Each key member is 1 to 256 bytes", which would refuse every `rows` table's key. This plan follows the key's comment; the spec is not edited by this task, and the bullet is corrected once the spec's author confirms. The model checks the bounds on every request (`InvalidValue`, naming the member) before it reads the store.
- Revision: the first 16 hex digits of `sha256(previousRevision + document)`.
- Quota. The spec's quota is "across one person's stored state", so it spans applications: with `maxBytesPerPerson` set, the model keeps the person's total at `PrefKey{.person = person, .app = std::string{kTableStateModel}, .scope = "morph.table", .point = "usage"}` (`"morph.tableState"` is not an application's id and `"morph.table"` is no state's scope, which is always `morph.table.<32 hex>`), as `{"bytes": n}`. Arithmetic, all in `std::uint64_t` document bytes (the length of the document's text as `save` received it; entries, envelopes and segment quoting are not counted): `n` is the stored value, 0 when the entry is absent or does not read as `{"bytes": <unsigned>}`; `b0` is the replaced document's size, 0 when the key has no head, else the head's `bytes` for a segmented head and the length of its `document` member's text for a small one; `b1` the new document's size. A save is refused when `n − b0 + b1 > maxBytesPerPerson` (computed as `n + b1 > quota + b0`, so no subtraction wraps): it answers `SaveResult{Refused, reason = "over the table-state quota"}` and writes nothing, `usage` included. Otherwise it writes the document (the order above), then `usage = n − b0 + b1`. A remove of a present key writes `usage = n − min(n, b0)`; of an absent key writes nothing. A multi-part save is checked once, at its `last` part, against the whole text. Without the option no `usage` entry is read or written. A preferences store has no conditional write, so two saves of one person reaching two model instances in the same moment can both pass the check; the quota can then be overshot by one document, which an operator's ceiling tolerates as the spec's `expected` rule tolerates its own race.
- `PreferencesTableStateStore::load` calls made in one turn of `owner` go out as one `TableStateLoad`; a head shorter than `size` is completed with `TableStateRead` calls by offset, restarting when the revision changes. A `save` over `partBytes` is sent as parts under one upload id (`upload` = a random 16-hex id), committed by the part with `last`.
- `TableStates` keeps every document it loaded or saved, by key, for the session's life; `load` of a kept key resolves at once with no call. `keep` replaces a kept entry (or adds one) and cancels nothing: a debounced save of the same key still goes out with its own document. `store()` and `keep` are what Part E's Task 14b uses for the `views` part, which saves with `expected` and is not debounced.
- A change before the key's load settles is held, not saved: when the load completes (or fails, or is given up on at the table's timeout and later completes) the save writes the loaded document with only the changed parts replaced (`writeState` over the loaded `StateDocument` with `layout`, `sort`, `filters` or `view` taken from the change per `parts`). A load that failed saves the change over an empty document. So a sort changed while a slow load is held never overwrites the stored filters.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_state.cpp`, over an in-process server built as `tests/ui_preferences_support.hpp`'s `prefs_test::Server` is (a `RemoteServer` over a `ThreadPoolExecutor`, the `TokenAuthorizer` that makes the request's token the verified principal, and `server->handle` called directly — no socket): `registerTableState(prefs, dispatcher, registry, options)` in place of `registerPreferences`, and a client `Dispatcher` whose channel sends each `execute` through `server->handle` with token `person`. `prefs` is `ProbePreferencesStore` (new, `tests/ui_table_support.hpp`): a `MemoryPreferencesStore` wrapper that records every key it was given so a test can list what the model cannot, with `has(PrefKey)`, `value(PrefKey) -> std::string`, `erase(PrefKey)`, `pointsUnder(scope) -> std::set<std::string>` (the points of every live entry with that scope), `largestValue()`, `setCount()`, `removeCount()`, `failSetsAfter(std::size_t n)` and `failRemovesAfter(std::size_t n)` (every `set`, or `remove`, after the n-th answers `false` and changes nothing), `healSets()` and `healRemoves()`; `usageKey(person)` is the quota entry's `PrefKey` above, `pendingPoint(key)`/`segmentPoint(key, i)`/`scopeOf(key)` the layout's names:

```cpp
// Mutation: the store calling the preferences model, which refuses the value over maxValueBytes.
TEST_CASE("table state: state of any size", "[ui][state]") {
    std::string const big = stateWithOneOfValues(20'000);           // a 20,000-value oneOf, well over 1 MiB
    REQUIRE(save(store, key, big).outcome == SaveOutcome::Saved);
    CHECK(load(store, key)->json == big);
    CHECK(prefs.largestValue() <= 12 * 1024 + kSegmentEnvelopeBytes);   // every entry within segmentBytes
    prefs.failSetsAfter(prefs.setCount() + segmentsOf(big2));          // stop after the segments of a second save
    CHECK_FALSE(trySave(store, key, big2));
    CHECK(load(store, key)->json == big);                              // the previous state still reads
    CHECK(prefs.has(pendingPoint(key)));                               // the stopped save left its intent
    prefs.healSets();
    REQUIRE(save(store, key, big3).outcome == SaveOutcome::Saved);     // the next save cleans up first
    CHECK_FALSE(prefs.has(pendingPoint(key)));
    CHECK(prefs.pointsUnder(scopeOf(key)) == headAndSegmentsOf(store, key));   // no stray segment of big2 or big
    prefs.erase(segmentPoint(key, 3));
    CHECK_THROWS(load(store, key));                                    // a missing segment is a failed load
}
// Mutation: the intent naming only the new revision (the previous revision's segments stay after a stop at step 5).
TEST_CASE("table state: a save stopped after its head leaves no stray segment behind the next save", "[ui][state]") {
    REQUIRE(save(store, key, big).outcome == SaveOutcome::Saved);
    prefs.failRemovesAfter(prefs.removeCount());                       // step 5 fails: big's segments stay
    REQUIRE(save(store, key, big2).outcome == SaveOutcome::Saved);     // the head names big2
    prefs.healRemoves();
    REQUIRE(save(store, key, small).outcome == SaveOutcome::Saved);    // unsegmented; step 1 cleans big's, then big2's
    CHECK(prefs.pointsUnder(scopeOf(key)) == std::set<std::string>{"state"});
}
// Mutation: bounds "1 to 256" on every member (a rows table's key, model "", is refused).
TEST_CASE("table state: a key's model may be empty, its table may not", "[ui][state]") {
    CHECK(save(store, keyWithModel(""), "{}").outcome == SaveOutcome::Saved);
    CHECK_THROWS(save(store, keyWithTable(""), "{}"));
    CHECK_THROWS(save(store, keyWithModel(std::string(257, 'm')), "{}"));
}
// Mutation: the quota checked against the new size alone (a replace of a large state is refused).
TEST_CASE("table state: a save over the quota is refused and the previous state still reads", "[ui][state]") {
    // registerTableState with maxBytesPerPerson = 10'000
    REQUIRE(save(store, keyA, docOf(6'000)).outcome == SaveOutcome::Saved);
    REQUIRE(save(store, keyA, docOf(9'000)).outcome == SaveOutcome::Saved);     // a replace: 9,000 in use
    auto const over = save(store, keyB, docOf(2'000));
    CHECK(over.outcome == SaveOutcome::Refused);  CHECK(over.reason == "over the table-state quota");
    CHECK_FALSE(load(store, keyB));
    CHECK(load(store, keyA)->json == docOf(9'000));
    CHECK(prefs.value(usageKey(person)) == R"({"bytes":9000})");          // 6,000 replaced, not added
    CHECK(remove(store, keyA));
    CHECK(prefs.value(usageKey(person)) == R"({"bytes":0})");
    CHECK(save(store, keyB, docOf(2'000)).outcome == SaveOutcome::Saved);      // 9,000 freed
    REQUIRE(save(store, keyC, docOf(8'000)).outcome == SaveOutcome::Saved);    // 10,000: at the quota is allowed
    CHECK(save(store, keyC, docOf(20'000)).outcome == SaveOutcome::Refused);   // 10,000 − 8,000 + 20,000 > 10,000
    CHECK(prefs.value(usageKey(person)) == R"({"bytes":10000})");         // a refusal writes nothing
}
// Mutation: b0 read as the head entry's length for a segmented head (the head is ~60 bytes: the replace is refused).
TEST_CASE("table state: the quota counts a segmented document by its bytes", "[ui][state]") {
    // maxBytesPerPerson = 40'000; save docOf(30'000) (segmented: 3 segments of 12 KiB)
    REQUIRE(save(store, keyA, docOf(30'000)).outcome == SaveOutcome::Saved);
    CHECK(save(store, keyA, docOf(35'000)).outcome == SaveOutcome::Saved);     // 30,000 − 30,000 + 35,000
    CHECK(prefs.value(usageKey(person)) == R"({"bytes":35000})");
}
// Mutation: usage written without the option set.
TEST_CASE("table state: no quota, no usage entry", "[ui][state]") {
    REQUIRE(save(store, keyA, docOf(6'000)).outcome == SaveOutcome::Saved);
    CHECK_FALSE(prefs.has(usageKey(person)));
}
// Review: a save during a held load. Mutation: saving the change as a whole new document at once.
TEST_CASE("table state: a change during a held load keeps the stored parts", "[ui][state]") {
    // gated store holding {"version":1,"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}}
    states.changed(key, docWithSort("name", Descending), StateParts::Sort);  h.advance(600ms);
    CHECK(gated.saves() == 0);                                         // held until the load settles
    gated.release();  h.advance(600ms);
    REQUIRE(gated.saves() == 1);
    CHECK(gated.lastSaved().contains(R"("contains":"a")"));          // stored filter carried
    CHECK(gated.lastSaved().contains(R"("column":"name")"));
}
// Mutation: keep() not replacing the kept copy (the remount restores the loaded document).
TEST_CASE("table state: a kept copy replaced by keep() is what the next load answers", "[ui][state]") {
    REQUIRE(loaded(states, key));
    states.keep(key, table::StoredTableState{.json = docWithSortOn(7), .revision = "9"});
    auto const again = loadNow(states, key);                           // no store call
    CHECK(store.loads() == 1);  CHECK(again->revision == "9");  CHECK(&states.store() == &store);
}
// Mutation: each table's load sent on its own (five calls).  Gate T5.
TEST_CASE("table state: five tables of a screen load in one call, and a second mount in none", "[ui][state]") {
    CHECK(dispatcher.count("load") == 1);
    remountScreen();
    CHECK(dispatcher.count("load") == 1);
}
// Mutation: no debounce (fifty saves).  Gate T4.
TEST_CASE("table state: fifty changes within a second are one save of the last", "[ui][state]") {
    REQUIRE(loaded(states, key));
    for (int i = 0; i < 50; ++i) { states.changed(key, docWithSortOn(i), StateParts::Sort); h.advance(10ms); }
    h.advance(500ms);
    CHECK(store.saves() == 1);  CHECK(readState(store.lastSaved())->sort == docWithSortOn(49).sort);
}
```
In `tests/net/test_push_tables.cpp`: `"table state: the person is the store's"` — two principals over one socket server save different `state` documents under one `TableStateKey` and each loads its own; a `TableStateLoad` body carrying a `"person"` member is read with that member ignored (glaze `error_on_unknown_keys = false`), so no request names another person. Mutation: the model keying by a body member.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `table_state.hpp` not found. `morph_net_tests` is POSIX-only (cross-part decision 18): build and run its case on `linux-everything` (`cmake --preset linux-everything && cmake --build --preset linux-everything --target morph_net_tests`).
- [ ] **Step 3: Implement** the interfaces above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "table state" --output-on-failure` and `morph_tests` green; `ctest --test-dir build/linux-everything -R "table state" --output-on-failure` for the `morph_net_tests` case.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Commit** `feat(ui): table state in the application's preferences store`.

#### 9d — The model-backed `table` node on the engine

**Files:**
- Modify: `include/morph/ui/view.hpp` (`TableSource`, `TableRowLook`, `CellLoad`, `TableStatus`, `TableColumn::id`, `Table::source`, `Table::status`, `Table::onRetry`), `include/morph/ui/mount.hpp` (`mountKind(Table)` reads `Table::source` when set; cells per column with their own row signal; `MountStats`, `Mounted::stats()`), `include/morph/ui/backend.hpp` (`TableWidget::setStatus`, `setOnRetry`, `setCellLoad`), `include/morph/ui/testing/recording_backend.hpp` (records `status`, cell loads; `retryTable`), `include/morph/ui/interpreter.hpp` and `src/ui/interpreter.cpp` (the lowering; `RtScope::onMissingRowField`; `ScreenCore::tableMessage`, `kStatusMessageHold`; element values), `tests/test_ui_table_model.cpp`, `tests/qt_quick/test_qt_quick_table.cpp` (F6, F7, F1)
- Docs: `docs/spec/ui/interpreter.md` ("Not yet lowered" loses the model-backed `table` entry; "Lowering" describes the lowering onto `TableRows`), `docs/spec/ui/document.md` (the `table` kind: `model` with `table` or `action`, `body`, `status`, `persist`, `empty` on a model-backed table, element values `viewCount`, `viewCountKind`, `undecided`, `awaiting`, `failed`, `pending`), `docs/spec/ui/preferences.md` (table state has its own model, `morph.tableState`, not bounded by `maxValueBytes`; the `columns` and `saved` points read and write the table's state), `docs/spec/table/engine.md` ("Not here yet": the model-backed table entry goes).

**Interfaces:**
- Consumes: 9a, 9b, 9c; Task 7's query-cache rules (the description is an entry of `Table.<id>.describe`).
- Produces:

```cpp
// view.hpp
class TableRows;
/// Whether a cell's value is here (cross-part decision 29). Read from the row's `TableRows::unloadedLevels` and the
/// column's level.
enum class CellLoad : std::uint8_t { Loaded, Unloaded, Failed };
/// A row's own parts beyond its cells, built once per realized row key, bound in the row's scope.
struct TableRowLook {
    std::optional<Prop<std::optional<Tone>>> tone;     // `rowTone`; disengaged: none
    std::optional<Prop<std::string>> label;            // `rowLabel`; disengaged: the key as text
    std::optional<Prop<std::vector<RowSpan>>> spans;   // `rowSpans` (a rows table, Task 11b)
    Node heading;                                      // non-null: a heading row; no cell is built for it
};
/// Rows by view index, and cells as a mount concept (cross-part decision 28).
struct TableSource {
    std::shared_ptr<TableRows> rows;
    /// One cell's view for the column at @p column (an index into `Table::columns`), built once per cell widget from
    /// a signal the mount sets to the record of the row that cell shows. A cell owns its signal: from Task 10a a
    /// pooled cell is rebound to another row by setting it, never by building the view again.
    std::function<Node(std::size_t column, reactive::Signal<Value> const& row)> cellView;
    /// The row's own parts, built once per realized row key from a signal the mount keeps equal to its record.
    std::function<TableRowLook(reactive::Signal<Value> const& row)> rowView;
    /// Column ids named to every `visible` call whatever the column window holds: what `rowView`, `rowTone`,
    /// `rowLabel` and `onActivate` read. Laid-out columns in the window are added to it per call.
    std::vector<std::string> alwaysFetched;
    /// Per `Table::columns` index, the level of that column (`ColumnInfo::level`); empty: the column has none.
    std::vector<std::string> levels;
};
// TableColumn gains: std::string id;   // the column's id; empty for a table built from C++ with no ids
struct TableStatus {
    std::string counts;    // the leading part
    std::string state;     // a failed level or fetch; empty when none
    bool retry = false;    // the state offers "Retry"
    std::string message;   // the latest refusal or notice; empty when none
    bool shown = false;    // whether the line takes space
    [[nodiscard]] bool operator==(TableStatus const&) const = default;
};
// struct Table gains:
std::optional<TableSource> source;     // when set, `rows` is ignored and rows come by view index
Prop<TableStatus> status;
std::function<void()> onRetry;

// backend.hpp, TableWidget gains (default: does nothing)
virtual void setStatus(TableStatus const& status);
virtual void setOnRetry(std::function<void()> onRetry);
/// A cell's load state; called after the row's key, and on every change. `Unloaded`: a placeholder of a fixed width
/// in the column's alignment; `Failed`: an error mark of the same width. Neither widens the column.
virtual void setCellLoad(Widget& row, std::size_t column, CellLoad load);

// mount.hpp
struct MountStats {
    std::size_t cellBinds = 0;     // sets of a cell's row signal that changed its record (each re-evaluates the cell)
    std::size_t rowBinds = 0;      // sets of a realized row's own signal that changed its record (`rowView`'s)
    std::size_t cellsCreated = 0;  // `cellView` calls: cell widgets built
    std::size_t cellsRebound = 0;  // pooled cells bound to another row or column (Task 10a; 0 before)
};
// Mounted gains: [[nodiscard]] MountStats const& stats() const noexcept;   // the whole mount's, for tests

// interpreter.hpp, RtScope gains
/// Called with a field a row expression read that the row's record does not hold; set on a server-mode row scope.
std::function<void(std::string_view field)> const* onMissingRowField = nullptr;

// interpreter.hpp
/// How long a status-line message stays, unless the next gesture on the table clears it first.
inline constexpr std::chrono::seconds kStatusMessageHold{6};
// ScreenCore gains (src/ui/interpreter.cpp):
/// Shows @p text as the table's status-line message for `kStatusMessageHold`; an empty @p text clears it.
void tableMessage(ElementState& table, std::string text);
```

Decisions:
- The mount over a `TableSource`: rows are always windowed (a server-mode table is); `setRealizedWindow(first, count, total)` with `total = rows->total().rows`; a realized index whose `row()` is empty mounts a placeholder row of the estimated height and becomes a keyed row when it arrives; `onChange` applies the `ViewChange` to the keyed realized rows, so a row keeps its widget, native state and selection when it moves; the visible range and the columns (`alwaysFetched` plus the laid-out columns' ids) go to `rows->visible`.
- Cells (cross-part decision 28). A realized row has its own `reactive::Signal<Value>` (for `rowView`) and, per laid-out column, a cell: `{reactive::Signal<Value> row; Node view; widget}`, the view built once by `cellView(column, cell.row)` (`cellsCreated`). The mount sets a row's signal and each of its cells' signals to the record `rows->row(index)->row` when the row is realized, and again for every row a `ViewChange` names as updated; nothing else sets them. Each set that changes the held value (the signal's `EqualityPolicy::Auto` skips an equal one, `signal.hpp:90`) counts one `rowBinds` or one `cellBinds`. So F6's arrival for 50 rows of 20 columns is 1,000 `cellBinds` and 50 `rowBinds`, and K5's refetch that changed 20 of 200 rows is 20 `rowBinds`. Cells are not pooled in this task; Task 10a pools exactly these cells, and `cellsRebound` stays 0 until then.
- Cell load state (cross-part decision 29). For each realized cell of column `c`: `Loaded` when `levels[c]` is empty or not in `rows->unloadedLevels(index).levels`; `Failed` when it is in `.failed`; otherwise `Unloaded`. The mount calls `setCellLoad(row, c, load)` after the row's key and whenever a `ViewChange` updates the row, only when the state changed. The recording backend records it; Qt Quick and the terminal draw it in Task 10b. The cell's own view stays mounted and bound under a placeholder, so a level's arrival is one `setCellLoad(…, Loaded)` and one bind, not a new cell.
- The lowering: `model` with `table` binds the model's channel as any query does, makes a document query on `Table.<table>.describe` (one per mounted table, W6) and a `WindowRows` with `restoring` set when the session has a store and `persist` is not `false`. Columns: the description's columns with `shown`, overridden per id by the node's `columns` parts; a node with `columns` lays out those; each `ui::TableColumn::id` is the column's id. `cellView` lowers the column's `cell` (or the default text cell over `standInCellText`) in a row scope over the cell's signal; `rowView` lowers `rowTone` and `rowLabel` in a row scope over the row's signal. `alwaysFetched`: the description's key, plus the columns `rowTone`, `rowLabel` and `onActivate` read as `row.<id>` (found when the document loads), plus every column when any of them, or a column's `cell`, `tone` or `tooltip`, reads the row otherwise (`row[name]`, the row passed whole), reported once as a warning naming the node. A column's `cell`, `tone` and `tooltip` read in its own cell, so their `row.<id>` columns are added to `visible` only while that column is in the window. `levels`: each laid-out column's `ColumnInfo::level` from the description.
- `RtScope::onMissingRowField` on a server-mode row scope. Its handler is called from inside a binding's evaluation, where a signal write is refused and reported (`signal.hpp:117-124`, `kSetInComputed`), so it does no work there: it adds the field to a pending set and, when the set was empty, posts one task with `runtime.owner().post`. That task adds the pending fields that are ids of the description's columns to the table's extra columns and calls `rows->visible` once with them (one fetch for every miss of one turn), then clears the set. A field that is not a described column id is dropped with no fetch and no report (it reads as absent, as any missing field does); it is never sent, since the server would refuse the query with `UnknownColumn`. A field already in the extra set is not added again. The read is `null` meanwhile, as the spec says it reads as absent; when the column arrives, the row's record gains the field and the binding runs again.
- `pageSize`/`page` on a model-backed table are reported once and ignored; `body` is the query's fixed part; header sorts and `filters` go to `setSort`/`setFilter`.
- State: key `{application = SessionOptions::application, model = <model type>, table = <table id>, qualifier = persist string or the screen's id, part = "state"}`. The load starts at mount; when it completes within `tableStateTimeout` the fitted sort and filters are applied (bound `sort` or `filters` are the document's and are neither applied nor saved; a literal `sort` is the default a stored one replaces) and `release()` is called; past the timeout `release()` is called with the defaults and a later state is applied (one more query) unless the user scrolled, selected or changed the arrangement meanwhile. A failed load is reported once through the session and changes still save. The stored layout part is carried unchanged in this task: nothing here changes a layout.
- `status`: a model-backed table's `shown` is always true; the counts are `countsText(counts(), selectedCount, formatting)`; a fetch error fills `state`; a failed level fills `state` with `"<Level> data could not be loaded."` and `retry = true`, whose handler calls `WindowRows::retry`.
- Messages (cross-part decision 11). `ScreenCore::tableMessage(ElementState& table, std::string text)` is the one channel every later task posts a transient refusal or notice through (Parts E, F, G). A non-empty `text` sets `TableStatus::message` and (re)starts a `kStatusMessageHold` timer on `SessionOptions::scheduler`; when it fires and the message is still that text, the message is cleared. An empty `text` clears the message and the timer and notifies nothing; the lowering calls `tableMessage(table, "")` on each user gesture on the table this task lowers (a header click, a selection change, a scroll report, Retry), and later parts do the same for theirs. Under `status: false` a non-empty `text` is sent once to `InterpreterHost::notify(text, "warning")` (`interpreter.hpp:290`) and `TableStatus::message` stays empty. With no scheduler the message stays until the next gesture.
- `empty` on a model-backed table shows only when `total().known`, `total().rows == 0`, `kind == Exact` and `counts().undecided == 0`.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp` (document: `{"vocab":"ui/1","id":"samples","view":{"kind":"table","id":"list","model":"Lims_SampleModel","table":"samples"}}`; `Harness(std::shared_ptr<table::ITableStateStore>)` is a new constructor that sets `SessionOptions::tableState` and `application = "lims"`; `GatedTableStateStore` holds each load until `release()` or `fail()`; `RecordingBackend` gains `cellLoad(table, rowKey, column) -> CellLoad` (the last `setCellLoad`, `Loaded` when none) and `retryTable(table)`; `h.stats()` is `Mounted::stats()`; `h.core()` and `h.element(id)` reach the screen's `ScreenCore` and an element's state; `h.host.notified` lists `notify` calls):

```cpp
// Mutation: a lowering that needs the node's `columns`.
TEST_CASE("model table: a client given only the catalog draws its columns, sorts, filters and pages it", "[ui][table]") {
    // describe reply: 3 columns; first page of 200 of total 9093
    CHECK(h.headerLabels() == std::vector<std::string>{"Id", "Name", "C3"});
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);                 // W6: one request
    h.clickHeader("Name");  CHECK(lastQuery(h).sort.at(0).column == "name");
}
// Mutation: a table that ignores its store sends the default query.  Gate T3.
TEST_CASE("model table: persisted state before the first query", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(h.owner, R"({"version":1,"sort":[{"column":"name","direction":"desc"}],
                                                                      "filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}})");
    auto screen = h.mount(kModelTable);  h.replyDescribe();
    CHECK(h.dispatcher.count("Table.samples") == 0);
    store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name", .dir = table::SortDirection::Descending}});
    CHECK(lastQuery(h).filters.columns.contains("name"));
}
// Mutation: no debounce; unmount not flushing.
TEST_CASE("model table: saves are coalesced, and unmounting saves at once", "[ui][table][state]") {
    for (int i = 0; i < 50; ++i) { h.clickHeader(i % 2 ? "Name" : "Id"); h.advance(5ms); }
    h.advance(500ms);  CHECK(store->saves() == 1);
    h.clickHeader("Name");  screen.reset();  h.settle();  CHECK(store->saves() == 2);   // at once, no debounce
    // a change during a save in flight is one more save after it
}
// Mutation: applying a late state after the user scrolled.
TEST_CASE("model table: a slow or failed load", "[ui][table][state]") {
    h.advance(300ms);  CHECK(h.dispatcher.count("Table.samples") == 1);     // defaults after tableStateTimeout
    store->release();  h.settle();  CHECK(h.dispatcher.count("Table.samples") == 2);   // applied: one more query
    // second case: scrolled before the late state → not applied, no second query
    // third case: fail() → one diagnostic, defaults, a later sort still saves
}
// Mutation: applying a stored sort over a bound one.
TEST_CASE("model table: bound values win", "[ui][table][state]") {
    // `sort` bound to state: stored sort not applied, not saved; stored filters applied; `persist: false` → no load, no save
}
// Mutation: the lowering refetching every held row's cells after a page refetch.  Gate K5.
TEST_CASE("model table: a refetch that changed 20 of 200 rows evaluates 20 rows again", "[ui][table]") {
    CHECK(rowEvaluations(h) == 20);
}
// Review Focus 1. Mutation: setDescription not dropping keys and filters on a removed column.
TEST_CASE("table rows: a new description revision drops what names a removed column", "[ui][table]") {
    // description rev A with columns id, name, c; sort on c, filter on c, visible columns {id, name, c}
    // a page reply names revision B → exactly one more describe call
    // describe reply B lacks c
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);
    auto const q = lastQuery(h);
    CHECK(std::ranges::none_of(q.sort, [](auto const& k) { return k.column == "c"; }));
    CHECK_FALSE(q.filters.columns.contains("c"));
    CHECK(std::ranges::find(*q.columns, "c") == q.columns->end());
    CHECK(diagnosticsMentioning(h, "c") == 2);                                 // the key and the filter, once each
}
// Review Focus 2. Mutation: the load completion capturing the table by reference with no liveness check.
TEST_CASE("model table: unmounted while its state load is held", "[ui][table][state]") {
    h.clickHeader("Name");                       // nothing loaded yet: a change marks the state unsaved
    screen.reset();  store->release();  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);
    CHECK(store->saves() == 1);                  // flushed at unmount
}
// Review Focus 4. Mutation: `empty` shown while `total().known` is false.
TEST_CASE("model table: empty waits for the first reply and for undecided rows", "[ui][table]") {
    CHECK_FALSE(h.backend.find("Text", "text", "No samples"));               // before any reply
    h.replyPage(pageJson(0, 0, 0, cols, TotalKind::AtLeast, /*undecided*/ 40));
    CHECK_FALSE(h.backend.find("Text", "text", "No samples"));
    CHECK(statusOf(h).counts == "0 so far, 40 loading");
    h.replyPage(pageJson(0, 0, 0, cols));
    CHECK(h.backend.find("Text", "text", "No samples"));
    CHECK(statusOf(h).counts == "0 rows");
}
// Mutation: a status line built without `failed`.
TEST_CASE("model table: a failed level offers Retry, which sends `retry`", "[ui][table]") {
    CHECK(statusOf(h).retry);  h.backend.retryTable(tableId);  CHECK(lastQuery(h).retry);
}
// Mutation: a refetch per event.  Gate E3 on a screen.
TEST_CASE("model table: an arrival is one keyed request", "[ui][table]") {
    // after the first page, a RecordChange with 20 keys → exactly one more Table.samples call, naming `keys`
}
// Mutation: the mount ignoring unloadedLevels (every cell Loaded).  Cross-part decision 29.
TEST_CASE("model table: a cell of an unloaded level is a placeholder, of a failed one an error mark", "[ui][table]") {
    // description: id, name, c3 with level "recipe"; page: row 0 unloaded {"recipe"}, row 1 failed {"recipe"}
    CHECK(h.backend.cellLoad(tableId, rowKey(0), 2) == CellLoad::Unloaded);
    CHECK(h.backend.cellLoad(tableId, rowKey(0), 1) == CellLoad::Loaded);      // name has no level
    CHECK(h.backend.cellLoad(tableId, rowKey(1), 2) == CellLoad::Failed);
    auto const created = h.stats().cellsCreated;
    h.replyKeyed(rowsWithRecipe({1}));                                         // the level arrives for row 0
    CHECK(h.backend.cellLoad(tableId, rowKey(0), 2) == CellLoad::Loaded);
    CHECK(h.stats().cellsCreated == created);                                  // the same cell, bound again
}
// Mutations: visible() called inside the binding (the write is refused, kSetInComputed reported, no fetch);
// an undescribed field sent (the query names "nosuch").
TEST_CASE("model table: a row field the window lacks is fetched once, after the binding", "[ui][table]") {
    // a column's cell reads row.c7 and row.nosuch; c7 is described but not laid out
    h.replyPage(pageJson(0, 200, 9093, laidOut));  std::size_t const before = h.dispatcher.count("Table.samples");
    CHECK(h.diagnostics().empty());                                            // no write refused inside a computation
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == before + 1);                  // one column fetch for every row's miss
    CHECK(std::ranges::find(*lastQuery(h).columns, "c7") != lastQuery(h).columns->end());
    CHECK(std::ranges::find(*lastQuery(h).columns, "nosuch") == lastQuery(h).columns->end());
}
// Mutations: no hold timer (the message stays); status:false still filling `message`.
TEST_CASE("model table: a message lasts six seconds or until the next gesture", "[ui][table]") {
    h.core().tableMessage(h.element("list"), "Refused");  CHECK(statusOf(h).message == "Refused");
    h.advance(5'999ms);  CHECK(statusOf(h).message == "Refused");
    h.advance(1ms);      CHECK(statusOf(h).message.empty());
    h.core().tableMessage(h.element("list"), "Again");  h.clickHeader("Name");  CHECK(statusOf(h).message.empty());
    h.core().tableMessage(h.element("list"), "");  CHECK(h.host.notified.empty());   // an empty text notifies nothing
    // second document with "status": false: one notify("Refused", "warning"), and statusOf(h).message stays empty
}
```

In `tests/qt_quick/test_qt_quick_table.cpp`, over the fixture's `wide` model through a scripted dispatcher (the description and pages written from `bench::WideTableSource` at 100,000 × 300, the node declaring twenty columns):

```cpp
// Mutation: rebind every cell of a changed row (F6 counts 50 × 20 × rows' columns).  Gate F6.
TEST_CASE("qt_quick model table: an arrival for 50 rows re-evaluates their 1,000 cells and no other", "[qt_quick]") {
    CHECK(cellEvaluations == 1000);
}
// Mutation: widen a column on every cell report.  Gate F7.
TEST_CASE("qt_quick model table: each column grows at most once on an arrival", "[qt_quick]") {
    for (int column = 0; column < 20; ++column) CHECK(widthChanges(column) <= 1);
}
```
`cellEvaluations` is the change in `Mounted::stats().cellBinds` across the arrival (the definition is in the Decisions above: one per cell signal set that changed its record), and `rowEvaluations(h)` in K5 the change in `rowBinds`. F1 is a `[.benchmark]` case in the same file: from the page reply to the first rendered frame, recorded.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: no `Table::source`; the model table cases report the node as not lowered.
- [ ] **Step 3: Implement** the interfaces and decisions above; remove the "Not yet lowered" entry for a model-backed `table` from the interpreter.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "model table|table rows" --output-on-failure` (the existing `table` suites, `test_ui_table.cpp` and `test_ui_mount.cpp`, stay green: a `Table` without `source` mounts as before); `morph_tests` green; then `cmake --build --preset cl-qt-debug --target morph_qt_quick_tests` and `ctest --test-dir build/cl-qt-debug -L qt_quick -R "model table" --output-on-failure` green.
- [ ] **Step 5: Mutations**, as named. Record F1 (and U6's F1–F7/E1–E3 figures, measured with `morph_qt_quick_tests "[.benchmark]"` on `cl-qt-release`) in table.md's Performance tables.
- [ ] **Step 6: Neighbouring specs and docs build.** Update the four specs listed under Docs. Run the roadmap's docs build: green.
- [ ] **Step 7: Commit** `feat(ui): the model-backed table on a screen, with stored sort and filters`.

---

### Task 10: The column window (table.md step 5)

Two commits and an exit criterion.

#### 10a — The column window in the mount

**Files:**
- Modify: `include/morph/ui/view.hpp` (`Virtualization` additions; `Table::arrangement`; `Table::highlightKey`), `include/morph/ui/mount.hpp`, `include/morph/ui/backend.hpp`, `include/morph/ui/testing/recording_backend.hpp`, `src/ui/interpreter.cpp` (range selection through `keysOf`; `maxRangeRows`; the `highlightKey` lowering), `include/morph/ui/interpreter.hpp` (`SessionOptions::maxRangeRows = 10'000`)
- Test: `tests/test_ui_mount.cpp`, `tests/test_ui_table_model.cpp`

**Interfaces:**

```cpp
// view.hpp
struct Virtualization {               // existing members kept; added, verbatim from the spec:
    std::size_t columnThreshold = 40;
    std::size_t cellThreshold = 20'000;
    std::size_t columnOverscan = 4;
    std::size_t realizePerFrame = 240;
};
struct TableArrangement {             // which columns show, in which order, how many pinned, user widths
    std::vector<std::size_t> visible; // indices into Table::columns, in layout order; empty: every column
    std::size_t pinned = 0;
    std::map<std::size_t, int> widths;
    [[nodiscard]] bool operator==(TableArrangement const&) const = default;
};
// Table gains: Prop<TableArrangement> arrangement;
//              Prop<std::optional<Key>> highlightKey;   // the row a linked chart highlights; a model-backed table's

// mount.hpp
struct Windowing { bool rows = false; bool columns = false; };
[[nodiscard]] Windowing windowingOf(Virtualization const& v, std::size_t rows, std::size_t visibleColumns, bool serverMode);
class ColumnExtents {
public:
    void assign(std::span<int const> widths);                       // visible columns, layout order
    bool grow(std::size_t column, int width);                       // only grows; false when unchanged
    void set(std::size_t column, int width);                        // a width the user set
    [[nodiscard]] std::pair<std::size_t, std::size_t> columnsAt(int offset, int viewport) const;  // [first, last)
    [[nodiscard]] int offsetOf(std::size_t column) const;
    [[nodiscard]] int widthOf(std::size_t column) const;
    [[nodiscard]] int totalWidth() const noexcept;
    [[nodiscard]] std::size_t columnReads() const noexcept;        // F9 counter
};
struct ColumnWindow { std::vector<std::size_t> columns; int leading = 0; int trailing = 0; };  // pinned, then the range
[[nodiscard]] ColumnWindow columnWindow(ColumnExtents const& extents, std::size_t pinned, int scrollX, int viewport,
                                        std::size_t overscan);
// MountStats (9d) gains: std::size_t placeholders = 0;   // cells drawn blank for the frame budget

// backend.hpp, TableWidget gains (defaults do nothing; a backend that windows columns overrides them)
virtual void setColumnWindow(std::span<std::size_t const> columns, std::span<int const> offsets,
                             std::span<int const> widths, int totalWidth);
virtual void setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler);
virtual void setHorizontalOffset(int scrollX);                       // anchoring
virtual void setOnSelectRange(std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange)> handler);
virtual void setFocusCell(std::size_t viewRow, std::size_t column);  // column: its place in the layout
virtual void setOnFocusMove(std::function<void(FocusMove)> handler);
virtual void setCounts(std::optional<std::int64_t> rows, std::size_t columns);  // nullopt: unknown (AtLeast)
virtual void scrollToRow(std::size_t viewRow);                         // brings a view row into view
virtual void setHighlightedRow(std::optional<std::size_t> viewRow);    // marks one row; nullopt: none
enum class FocusMove : std::uint8_t { Up, Down, Left, Right, Home, End, First, Last, PageUp, PageDown };
```

Decisions:
- `windowingOf`: rows when `serverMode` or `rows >= threshold` or `rows × visibleColumns > cellThreshold`; columns when `visibleColumns > columnThreshold` or `rows × visibleColumns > cellThreshold`.
- `ColumnExtents` keeps widths in a Fenwick tree (prefix sums in O(log n), `columnsAt` by binary descent on the tree), so a width change and a lookup read O(log n) nodes; `columnReads` counts per-column width reads only.
- Pools: the pooled unit is 9d's cell — its own `reactive::Signal<Value> row`, the `Node` `cellView` built over it once, and its widget — which owns the cell's scope, so its bindings live as long as the pooled cell. Cells of the default cell of one `TextRole` share a pool keyed by role; a column with its own `cell` template has a pool keyed by its column index. A cell leaving either window is detached and pooled, its signal left as it was. One entering takes a pooled cell and is rebound by setting its signal to the record of its new row (`cellsRebound`, and one `cellBinds` when the record differs) and, for a role pool, by the mount telling it its new column; `cellView` is not called again. Only an empty pool creates a cell (`cellsCreated`).
- The frame: one pass of the windowing Effect realizes or rebinds at most `realizePerFrame` cells, nearest the viewport centre first; the rest are placeholders of their column's width (`placeholders`) and are realized in later turns posted with `_rt->owner().post`, one batch of `realizePerFrame` a turn.
- Anchoring: a growth of a column left of the first fully visible unpinned column calls `setHorizontalOffset(scrollX + growth)` in the same pass. The vertical rule is the same for rows above the first visible row.
- Pinned: the first `arrangement.pinned` visible columns are always in the window; while their widths exceed half the viewport, the last pinned columns scroll with the rest (display only; `pinned` is not changed).
- Focus is `(row key, column id)`, held by the mount; that row and column stay realized; a focused row that leaves the view moves focus to the row now at its view index, or the last row; a move onto an unfetched index focuses the placeholder (announced as loading) and the row takes focus when it arrives; `Last` with an `AtLeast` total goes to the last row known.
- `setCounts(rows, columns)`: `rows` the view's total when `Exact`, `nullopt` under `AtLeast`; `columns` the visible column count.
- `highlightKey` (a model-backed table's only; the loader already refuses it on `rows`, `document.cpp:3306`). On each change to a non-null key the lowering cancels a keyed fetch still in flight for an earlier key and sends one `TableQuery{keys = {k}, columns = [], sort, filters}` (`Table.<table>`, as `keysOf`'s fetches go); the reply's `indices[0]` (Task 4) is the row's view index `i`. It then calls `rows->visible(max(0, i − overscan), viewportRows + 2 × overscan, columns)`, `scrollToRow(i)` and `setHighlightedRow(i)`, and keeps the key: a `ViewChange` that moves the key moves the mark, one that removes it clears it. A reply with no row (the key is filtered out or gone) clears the mark and scrolls nothing; a `null` key clears it. It never changes the selection or the keyboard focus.
- Range selection on a `TableSource`: the backend reports two view rows; the lowering calls `keysOf(min, count)`; more than `maxRangeRows` rows is refused with the message `"A range selects at most 10,000 rows"` (number grouped by the locale) and selects nothing. `WindowRows::keysOf` fetches pages naming no columns (`columns: []`) for what it does not hold.
- A hidden column has no header, width or cells; hiding returns its realized cells to their pool; moving a column inside the window creates nothing (D8).

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_mount.cpp` (RecordingBackend gains `scrollTableColumns(id, scrollX, viewport)`, `focusMove(id, FocusMove)`, `selectTableRange(id, anchor, focus)`, and records `setColumnWindow`, `setCounts`, `setFocusCell`):

```cpp
TEST_CASE("windowing: the three thresholds", "[ui][mount]") {
    Virtualization const v{};
    CHECK(windowingOf(v, 1999, 300, false).rows);   CHECK(windowingOf(v, 1999, 300, false).columns);
    CHECK_FALSE(windowingOf(v, 1999, 10, false).rows);   CHECK_FALSE(windowingOf(v, 1999, 10, false).columns);
    CHECK(windowingOf(v, 2000, 10, false).rows);    CHECK_FALSE(windowingOf(v, 2000, 10, false).columns);
    CHECK(windowingOf(v, 100, 41, false).columns);  CHECK_FALSE(windowingOf(v, 100, 41, false).rows);
    CHECK(windowingOf(v, 10, 10, true).rows);
}
// Mutation: a column window that ignores cellThreshold.  Gate F2 on the recording backend.
TEST_CASE("table window: 1,999 × 300 realizes only the windows", "[ui][mount]") {
    // 30 rows and 20 columns in view, 2 pinned
    CHECK(cellsAlive(h) <= realizedRows(h) * (2 + 20 + 2 * 4));
    CHECK(headerCellsAlive(h) == 2 + windowColumns(h));
}
// Mutation: release a leaving cell without pooling it.  Gate F8.
TEST_CASE("table window: a fling creates no cell once the pools are warm", "[ui][mount]") {
    flingRows(h, 2000);  auto const created = h.stats().cellsCreated;
    flingRows(h, 2000);  CHECK(h.stats().cellsCreated == created);
    CHECK(maxRebindsPerTurn(h) <= 240);
}
// Mutation: no anchoring.
TEST_CASE("table window: a column growing left of the view keeps the columns in view where they are", "[ui][mount]") {}
// Mutation: unpin by changing `pinned`.
TEST_CASE("table window: pinned columns stay realized, and a narrow table unpins for display only", "[ui][mount]") {}
// Mutation: reading every column's width per pass.  Gate F9.
TEST_CASE("table window: a pass reads no width outside the column window", "[ui][mount]") {}
// Mutation: rebuild rows on a layout change.  Gate D8.
TEST_CASE("table window: hiding and moving a column create no cell widgets", "[ui][mount]") {
    CHECK(h.stats().cellsCreated == before);  CHECK(pooledAfterHide == realizedRows(h));
}
// Mutation: focus held by widget, which recycling drops.  Gate F10.
TEST_CASE("table focus: the focused cell survives a scroll of 10,000 rows and 200 columns", "[ui][mount]") {
    CHECK(focusedCellRealized(h));  CHECK(h.backend.lastCounts(table) == std::pair{std::optional<std::int64_t>{100'000}, 300uz});
}
TEST_CASE("table focus: Ctrl+End over rows not fetched lands on the last row when it arrives", "[ui][mount]") {}
TEST_CASE("table focus: the reported row count is the total, or unknown for a lower bound", "[ui][mount]") {}
// Mutation: rebinding a pooled cell by calling cellView again (cellsCreated grows on the second fling).
TEST_CASE("table window: a pooled cell is rebound by its row signal", "[ui][mount]") {
    flingRows(h, 2000);  auto const views = cellViewCalls(h);
    flingRows(h, 2000);  CHECK(cellViewCalls(h) == views);  CHECK(h.stats().cellsRebound > 0);
}
```
In `tests/test_ui_table_model.cpp`: `"model table: a range over rows not held resolves through pages that name no columns"` (fetches carry `"columns":[]`; the selection is the keys of view rows 500 to 1,499); `"model table: a range over maxRangeRows is refused and selects nothing"` (status message, selection unchanged). Mutation: `keysOf` fetching the visible columns. `"model table: highlightKey scrolls to the row and marks it"`: `highlightKey` bound to a state set to key 5,012; one `Table.samples` call naming `"keys":[5012]` and `"columns":[]`; replying with `indices: [5011]` records `scrollToRow(5011)` and `setHighlightedRow(5011)`, a page fetch covering 5,011, and no change to `selection`; setting the key to one whose reply has no row records `setHighlightedRow(nullopt)` and no scroll; setting two keys in one turn sends the second fetch and cancels the first (`stats().cancelled` + 1). Mutations: the view index taken from the reply's position (0) instead of `indices`; the highlight written into the selection.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `windowingOf`, `ColumnExtents` undeclared.
- [ ] **Step 3: Implement** the interfaces and decisions above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "windowing|table window|table focus|model table" --output-on-failure`; `morph_tests` green.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Commit** `feat(ui): the column window in the mount`.

#### 10b — The column window in Qt Quick and the terminal

**Files:**
- Modify: `src/qt_quick/table.cpp`, `src/qt_quick/qml/MorphTable.qml` (the column window, pinned columns, the header for windowed columns only, horizontal anchoring, focus cell, counts for assistive technology, the status line from `setStatus` with its "Retry" button), `src/tui/table_widget.cpp`, `src/tui/table_widget.hpp` (the column range, the status line)
- Test: `tests/qt_quick/test_qt_quick_table.cpp`, `tests/tui/test_tui_table.cpp`
- Docs: `docs/spec/qt_quick/backend.md` ("Containers and lists": the column window, cell pools, pinned columns, the status line; "Tests": the new cases).

**Interfaces:** consumes 10a's `TableWidget` members; produces nothing new.

Decisions:
- MorphTable exposes, for tests, the QML properties `realizedColumnFirst`, `realizedColumnCount`, `pinnedShown`, `headerCellCount`, `statusText`, `rowMeasures` (cell height measurements made), the invokables `rowHeight(viewRow)` and `cellLoadAt(viewRow, column)` (`0` Loaded, `1` Unloaded, `2` Failed, as drawn), and keeps `rowScans`, `realizedFirst`, `realizedCount`, `totalRows`.
- The status line is one `Text` row under the rows, an accessible polite live region (`Accessible.role: Accessible.StaticText`, announced on change), with a "Retry" button while `TableStatus::retry`; it takes no space while `shown` is false.
- The terminal table draws the cells of the columns in the window only, and the status line as its last line.
- Cell load state (from 9d's `setCellLoad`). Qt Quick: an `Unloaded` cell draws a rounded bar of a fixed 3 em width in the column's alignment over the cell's content, which stays bound but invisible; a `Failed` cell draws an error mark of the same width whose tooltip and accessible name say "Could not be loaded". The terminal draws `···` for `Unloaded` and `!` for `Failed`, in the column's alignment. Neither is measured into the column's width, so a level arriving widens a column at most once (F7).
- Row height (table.md "Cells"), in `MorphTable`, which is what measures cells: a row's height is the tallest of its realized cells and only grows while the row is realized; a row that leaves the row window forgets its height; an unrealized row counts as the mean of the measured heights. When a row above the first visible row grows, the vertical offset grows by the same amount in the same frame (the mount's anchoring, 10a). A table whose columns are all `wrap: false` has one row height and measures no cell (`rowMeasures` stays 0). The terminal's rows are one line each and have no rule here.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/qt_quick/test_qt_quick_table.cpp, a model-backed table of the fixture's `wide` at 100,000 × 300, 1,280 × 800.
// Mutation: MorphTable instantiating a cell per column of a realized row.  Gate F2.
TEST_CASE("qt_quick table window: cells and header cells alive are the windows'", "[qt_quick]") {
    CHECK(cellItems(scene) <= realizedRows * (pinned + inView + 2 * 4));
    CHECK(tableItem.property("headerCellCount").toInt() == pinned + windowColumns);
}
// Mutation: a pool keyed by column for default cells.  Gate F8.
TEST_CASE("qt_quick table window: flings create no cell once the pools are warm", "[qt_quick]") {}
// Gate F10.
TEST_CASE("qt_quick table window: focus and counts after scrolling far away", "[qt_quick]") {}
// Gate D8, Qt Quick half.
TEST_CASE("qt_quick table window: hiding and moving a column create no cell items", "[qt_quick]") {}
TEST_CASE("qt_quick table: the status line shows counts and Retry", "[qt_quick]") {
    CHECK(tableItem.property("statusText").toString() == QStringLiteral("100,000 rows"));
}
// Mutation: MorphTable ignoring setCellLoad (an unloaded cell draws its empty content).  Cross-part decision 29.
TEST_CASE("qt_quick table: an unloaded cell is a placeholder and a failed one an error mark", "[qt_quick]") {
    // the fixture's `wide` with `recipe` unloaded for row 0 and failed for row 1; d000 is a recipe column
    CHECK(cellLoadAt(tableItem, 0, d000) == 1);  CHECK(cellLoadAt(tableItem, 1, d000) == 2);
    CHECK(columnWidth(tableItem, d000) == widthBefore);                          // the placeholder widened nothing
}
// Mutations: a row's height recomputed from its realized cells each pass (it shrinks); measuring wrap:false tables.
TEST_CASE("qt_quick table: a row's height only grows while it is realized", "[qt_quick]") {
    // a 2-line wrapped cell in column 60 of row 3; scroll right to it, then back
    scrollColumnsTo(scene, 60);  int const tall = rowHeight(tableItem, 3);
    scrollColumnsTo(scene, 0);   CHECK(rowHeight(tableItem, 3) == tall);       // scrolled out: not shorter
    // a second table, every column wrap:false: rowMeasures == 0 after a fling
}
// tests/tui/test_tui_table.cpp. Mutation: the terminal table ignoring the column range.  Gate U7.
TEST_CASE("tui table: cell widgets alive are rows in view times columns in view", "[tui]") {}
// Mutation: the terminal table ignoring setCellLoad.
TEST_CASE("tui table: an unloaded cell draws its placeholder and a failed one its mark", "[tui]") {
    CHECK(screenLine(term, 1).contains("···"));  CHECK(screenLine(term, 2).contains("!"));
}
```
Benchmarks, `[.benchmark][qt_quick]`, in the same file: F3 (vertical at 20 rows/s, a fling at 2,000 rows/s, a thumb drag), F4 (horizontal at 10 and 100 columns/s), F5 (memory per realized row), each first asserting the table is windowed both ways, timed per step through `QQuickRenderControl` (`polishItems`, `sync`, `render`) under `offscreen` and the software scene graph.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-qt-debug --target morph_qt_quick_tests morph_tui_tests` then `ctest --test-dir build/cl-qt-debug -L qt_quick -R "table window" --output-on-failure` → FAIL on `headerCellCount` being invalid.
- [ ] **Step 3: Implement** the backends' halves.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-qt-debug -L qt_quick --output-on-failure` and `ctest --test-dir build/cl-qt-debug -R "tui table" --output-on-failure`: all green, the existing virtualized-table cases included.
- [ ] **Step 5: Mutations**, as named. Record F1 to F5 from `build/cl-qt-release` (`cmake --preset cl-qt-release -DMORPH_BUILD_QT_QUICK=ON`, then `morph_qt_quick_tests "[.benchmark]"` with `QT_QPA_PLATFORM=offscreen`) into table.md's "The frontend at 300 columns" table, against step 0's baseline.
- [ ] **Step 6: Neighbouring spec** `docs/spec/qt_quick/backend.md` as listed; docs build green.
- [ ] **Step 7: Commit** `feat(qt_quick): the column window, cell pools and the status line`.

#### 10c — Exit criterion U8

U8 is "U1 and U2 at 20,000 rows, and U6's scrolls, in the WebAssembly build in a browser … the frame budgets of F3 and F4 hold". What can be done without a person at a browser:

- [ ] **Build**: the Qt WebAssembly build of `morph_qt_quick` already builds in `.github/workflows/wasm-demo.yml` (Qt `wasm_singlethread`, emsdk 4.0.7, `qt-cmake … -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON`). Confirm this branch still builds there (`cmake --build build-wasm --target morph_examples_client`) and that the count gates of the single-threaded path still run under Node in the WebAssembly job Task 0 adds.
- [ ] **Measure with Task 0's B5 harness.** Part A adds `tests/qt_quick/wasm/` (built only when `EMSCRIPTEN AND MORPH_BUILD_QT_QUICK`), run in a headless browser with `emrun --browser chrome --browser_args=--headless=new`, printing figures to the console. Add `tests/qt_quick/wasm/u8_scrolls.cpp` beside `wide_rows_baseline.cpp`: it mounts a 20,000-row `rows` table (U1: a header click on a text column; U2: one row changed) and a model-backed table over an in-process `bench::WideTableModel` at 100,000 × 300, scrolls both as F3 and F4 do, and prints U1, U2 and each scroll's p50/p99 frame time. Build it in the WebAssembly tree configured as `wasm-demo.yml` does (`qt-cmake … -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TESTS=ON`), run it with the same `emrun` line, and record the figures beside the native ones. The exit criterion holds when every scroll's p50 ≤ 8 ms and p99 ≤ 16.6 ms.
- [ ] **What stays open.** The run is local: no CI job drives a browser, and on a machine without Chrome it cannot run. If it cannot be run, Task 10 lands with its native gates and the executor writes "U8 not measured" in the commit body and tells the user. No other command is invented for it.

---

### Task 11: Tables over `rows` on the engine (table.md step 6)

Three commits: 11a and 11b are step 6; 11c is the typed-list-action half of step 4's model-backed table, placed here because it reads its column kinds with 11a's inference.

#### 11a — `ValueRowsSource`

**Files:**
- Modify: `include/morph/ui/table_rows.hpp`
- Test: `tests/test_ui_table_model.cpp`, `tests/bench_interpreter.cpp` (U1 to U4)

**Interfaces:**

```cpp
/// The expression language's text order: the default collator's fold, then the text itself.
class ValueRowsCollator final : public table::TextCollator {
public:
    [[nodiscard]] std::string sortKey(std::string_view text) const override;   // fold(text) + '\0' + text
    [[nodiscard]] std::string fold(std::string_view text) const override;      // CodePointCollator's
    [[nodiscard]] std::size_t folds() const noexcept;                           // U3 counter
};
struct ValueColumn {
    std::string id;
    std::optional<table::ColumnKind> kind;           // the column part's literal `kind`; empty: inferred
    std::function<Value(Value const& row)> sortBy;   // the column's `sortBy` in the row's scope, tracked; empty: the field
};
/// The hidden leading sort key of a grouped rows table: an Integer column of each row's run index.
inline constexpr std::string_view kGroupRunColumn = "group";
class ValueRowsSource final : public table::DataSource {
public:
    ValueRowsSource(reactive::Runtime& runtime, std::vector<ValueColumn> columns,
                    std::function<Key(Value const& row)> keyOf,
                    std::shared_ptr<table::UnitRegistry const> units = {});   // Quantity cells' units; null: none
    void assign(ValueList rows);                                   // a new list: match by key, then by cell
    /// The `group` part's `by`, read per row; empty: no groups. Adds the hidden column `kGroupRunColumn`.
    void setGroup(std::function<Value(Value const& row)> by);
    /// The run a source row belongs to: its index among the runs of adjacent rows with equal `by`, in source order.
    [[nodiscard]] std::size_t runOf(std::size_t sourceRow) const;
    /// Each run's heading record `{key, count, first, last, items}`, by run index, as `TableView::runs` holds them.
    [[nodiscard]] std::span<Value const> runRecords() const noexcept;
    void use(std::string_view column);                             // a sort, a filter or a values list began
    void unuse(std::string_view column);                           // its last use ended: its cells go
    [[nodiscard]] RecordOf recordOf() const;                       // for EngineRows
    [[nodiscard]] std::size_t cellsHeld() const noexcept;          // U4 counter
    [[nodiscard]] std::size_t cellsDerived() const noexcept;       // U2 counter
};
```

Decisions:
- A cell of a column in use is a `reactive::Computed` over the row's signal and what `sortBy` reads; when one changes, the source notifies `Updated` for that row alone; a change every row's cell reads derives the column again and notifies `Reset`.
- `assign`: a row whose `Value` shares storage with the held one is kept unread; another object derives the in-use cells and compares them with the held cells, `Updated` only on a difference; new keys `Inserted`, missing keys `Removed`. A later duplicate of a key is dropped and reported through `site::kDuplicateKey`, once per duplicate, as the mount reports it today.
- Kind inference, by `ui::ValueKind` (`value.hpp:132`; there is no floating-point `Value`, since `valueFromJson` refuses a number with a fraction, so no `rows` column is ever `Number`). Each non-null cell votes; the column's kind and each cell:

  | `ValueKind` | Votes for | Cell |
  |---|---|---|
  | `Null` | nothing | empty (`std::monostate`) |
  | `Bool` | `Bool` | `bool` |
  | `Int` | `Integer` | `int64`; in a `Decimal` column a `Rational` of 0 places |
  | `Decimal` | `Decimal` | its `math::Rational`, places kept |
  | `String` | `Text` | the text |
  | `Timestamp` | `DateTime` | UTC **seconds**: `floor(ms / 1000)` (toward −∞, so −1 ms is −1 s) |
  | `Quantity` | `Measure` | `Measure{value, units->find(unit)}`; a unit the registry lacks, or no registry, is an invalid cell (the "unknown unit" case) |
  | `List`, `Record` | nothing of its own | invalid, except a `Record` of exactly `num`, `den`, `unit` (cross-part decision 22), read as a `Quantity` |

  All votes alike → that kind; only `Int` and `Decimal` → `Decimal`; any other mix takes the first non-null cell's kind, and a cell not of it is invalid (sorts last, reported once at the `sort` path). A literal `kind` on the column part wins over inference, and a cell not of it is invalid. A change of the inferred kind is a `Reset` of that column. **Stated deviation:** a `DateTime` cell holds seconds (`data_source.hpp:56`), so two timestamps in one second tie and keep their source order, where the expression language's `sort` orders to the millisecond. A rows table orders as a model's `DateTime` column does, which is what "One order on every surface" compares.
- Groups. With `setGroup`, the source holds the column `kGroupRunColumn` (`ColumnKind::Integer`, not in `ColumnInfo`s the lowering lays out): row `r`'s cell is `runOf(r)`, runs counted over the rows in source order, a new run starting where `by` differs from the previous row's (`Value ==`). The lowering puts `{kGroupRunColumn, Ascending}` before every sort chain it passes to the engine, so groups keep their places and rows sort within them; `assign` recomputes the runs and notifies `Reset` of that column only when a run boundary moved. `runRecords()` are the heading records the interpreter builds today (`TableView::runs`), placed by the lowering after paging (11b).
- The source keeps each row's `Value` beside its cells; `recordOf` returns its key and that value.

- [ ] **Step 1: Write the failing tests**

```cpp
// Mutation: a diff by row value (every field compared).
TEST_CASE("value rows source: a document's rows as a source", "[ui][table]") {
    src.use("name");  src.assign(rows());  listener.clear();
    src.assign(freshDecodeOf(rows()));               CHECK(listener.changes.empty());
    src.assign(withName(rows(), 7, "zeta"));         CHECK(listener.changes == std::vector{updated({7})});
    sortByReadsChange(src, 3);                       CHECK(listener.changes.back() == updated({3}));
    CHECK(src.cellsHeld() == rows().size());         // one column in use; "notes" holds no cells
    // integers with decimals are one Decimal column; a text among them is one invalid cell, reported once
}
// Review Focus 5. Mutation: keep the last duplicate.
TEST_CASE("value rows source: a duplicate key keeps its first row", "[ui][table]") {
    src.assign(list({row(1, "a"), row(2, "b"), row(1, "c")}));
    CHECK(src.snapshot()->rowCount() == 2);
    CHECK(duplicateReports(h) == 1);
    engine.setSort({{.column = "name", .dir = table::SortDirection::Descending}});
    CHECK(namesInView(engine) == std::vector<std::string>{"b", "a"});
}
// Mutation: Timestamp cells kept in milliseconds (row b, 1'700'000'000'500 ms, reads as a date 1,000× too late).
TEST_CASE("value rows source: a timestamp is a DateTime of whole seconds", "[ui][table]") {
    src.assign(list({row(1, ts(1'700'000'000'000)), row(2, ts(1'700'000'000'500)), row(3, ts(-1))}));
    CHECK(src.columns()[timeColumn].kind == table::ColumnKind::DateTime);
    CHECK(cellOf(src, 0) == table::Cell{std::int64_t{1'700'000'000}});
    CHECK(cellOf(src, 1) == table::Cell{std::int64_t{1'700'000'000}});   // the same second: they tie
    CHECK(cellOf(src, 2) == table::Cell{std::int64_t{-1}});              // floor, not truncation toward 0
    engine.setSort({{.column = "at", .dir = table::SortDirection::Descending}});
    CHECK(keysInView(engine) == std::vector<std::int64_t>{1, 2, 3});     // the tie keeps source order
}
// Mutation: a missing case in the inference (each kind of the table above, one column each).
TEST_CASE("value rows source: every value kind infers its column kind", "[ui][table]") {
    // columns b, i, d, s, t, q, l over one row of each ValueKind, and a mixed column i+d
    CHECK(kinds(src) == std::map<std::string, table::ColumnKind>{{"b", Bool}, {"i", Integer}, {"d", Decimal},
        {"s", Text}, {"t", DateTime}, {"q", Measure}, {"l", Integer}, {"id_d", Decimal}});   // "l": its Int cell
    CHECK(invalidCells(src, "q") == 1);                                  // the second row's unit "zz" is unknown
    CHECK(invalidCells(src, "l") == 1);                                  // the list cell
}
// Mutation: the run index computed after the sort (groups split).
TEST_CASE("value rows source: a grouped table sorts within its runs", "[ui][table]") {
    src.setGroup(byField("site"));  src.assign(list({row(1, "A", "z"), row(2, "A", "a"), row(3, "B", "m")}));
    engine.setSort({{.column = std::string{kGroupRunColumn}}, {.column = "name"}});
    CHECK(keysInView(engine) == std::vector<std::int64_t>{2, 1, 3});
    CHECK(src.runRecords().size() == 2);
}
// Mutation: fold inside the comparison.  Gate U3.
TEST_CASE("value rows source: a sort of 100,000 rows folds each text once", "[ui][table]") {
    CHECK(collator->folds() == 100'000);
}
```
Benchmarks `[.benchmark][interpreter]` in `bench_interpreter.cpp`: U1 (a header click on a text column of 100,000 rows), U2 (one row changed, shared and fresh decode; gates inside: `EngineStats::repairs == 1`, `fullSorts` unchanged, `cellsDerived` equal to one column's rows for the fresh decode), U4 (memory of 10,000 × 300 with one column sorted; gate `cellsHeld() == 10'000`).

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `ValueRowsSource` undeclared.
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "value rows source" --output-on-failure`.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Commit** `feat(ui): a document's rows as a table source`.

#### 11b — The interpreter's tables on the engine

**Files:**
- Modify: `include/morph/ui/interpreter.hpp` (`TableView` stays as the shape element values read; `ElementState::tableRows` (`std::shared_ptr<TableRows>`) replaces `sortBound`'s use for ordering), `src/ui/interpreter.cpp` (`sortedRows` and the ordering half of `tableView` removed; the lowering of a `rows` table onto `ValueRowsSource` → `Engine` → `EngineRows`; `pageSize`/`page` as a slice of the engine's view; `group` as a hidden leading sort key `ValueRowsSource` computes, headings placed after paging; `rowSpans` on the page's rows, drawn only unsorted and below the windowing thresholds; select-all through `table::Selection::selectAll`)
- Test: `tests/test_ui_table_model.cpp`, `tests/test_ui_table.cpp`, `tests/bench_interpreter.cpp` (B1)
- Docs: `docs/spec/ui/interpreter.md` ("Lowering", **The rows' pipeline** and **Groups** describe the engine's order and what the lowering does around it), `docs/spec/ui/document.md` (a column's `kind`; the client-mode bound warning), `docs/spec/table/engine.md` ("Not here yet": the single-threaded steps entry).

**Interfaces:**
- Consumes: 11a; `table::Engine` with `EngineOptions{owner = &runtime.owner(), worker = nullptr, frameBudget = 4 ms, services.collator = ValueRowsCollator}`.
- Produces: `void EngineRows::setRestoring(bool restoring)` (while `true`, `total().known` is `false` and `row()` answers nothing, so the mount draws placeholder rows; the engine still runs); otherwise nothing new in the API, and every existing table element value keeps its name and shape.

Decisions:
- A plain list over 20,000 rows or 1,000,000 cells (rows × the node's columns) is reported once as a warning naming the table: `"table <id> holds <n> rows; a table over rows is ordered on the client up to 20,000 rows or 1,000,000 cells — serve it as a model's table"`.
- A table with no `id` gets no state key: a `persist` other than `false` on it is refused when the document loads.
- The mount: a `rows` table lowers onto `Table::source` as a model-backed one does (9d's `TableSource`), `cellView` over each cell's signal, `rowView` giving `rowTone`, `rowLabel`, `rowSpans` and, for a group heading's key, `heading` (the lowered `heading` content), so the two kinds share one mount path.
- Persisted state for a `rows` table (table.md "Persisted state": "a client-mode table draws placeholder rows"). With a session store, an `id` and `persist` not `false`, the key is `{application = SessionOptions::application, model = "" (a table over a document's rows has none; 9c's bounds allow it), table = the node's id, qualifier = the persist string or the screen's id, part = "state"}`. The lowering calls `setRestoring(true)` before the first `assign`, starts the load through `Session::tableStates()`, and on completion within `tableStateTimeout` applies the fitted sort and filters (bound ones are the document's, as in 9d) and calls `setRestoring(false)`: the first rows drawn are in the stored order. Past the timeout it calls `setRestoring(false)` with the defaults, and a later state is applied unless the user touched the table, as in 9d. Sort and filter changes go to `TableStates::changed` with the same debounce and unmount flush as 9d's; the description passed to `fitState` is built from the node's columns and the inferred kinds (`TableDescription{id, columns, key}` with each `TableColumn{info, label, sortable, filterable}`).

- [ ] **Step 1: Write the failing tests**

```cpp
// Mutation: the interpreter's pipeline left in place for `rows` tables.
TEST_CASE("one order on every surface: a rows table and a model table order alike", "[ui][table]") {
    // the same 200 rows (texts with accents and mixed case, decimals, nulls) as `rows` and as pages of `samples`
    // the model-table side's pages are written by `table::apply` with ValueRowsCollator as the served collator
    for (auto const& chain : {byName, byNameDesc, byWeightThenName})
        CHECK(rowsTableOrder(h, chain) == modelTableOrder(h, chain));
}
// Mutation: sorting the whole list on a row change.  Gate U2 counts.
TEST_CASE("rows table: one changed row is one repair", "[ui][table]") {
    CHECK(engineOf(h).stats().repairs == 1);  CHECK(engineOf(h).stats().fullSorts == sortsBefore);
}
// Mutation: no bound check.
TEST_CASE("rows table: a list over 20,000 rows is reported once", "[ui][table]") {}
// Mutation: drawing the rows before the load settles (three keyed rows mount in source order first).
TEST_CASE("rows table: persisted state before the first rows", "[ui][table][state]") {
    // Harness(gated store holding {"version":1,"sort":[{"column":"name","direction":"desc"}]}); a rows table "list"
    // over rows a, b, c with a sortable "name"
    CHECK(h.backend.rowKeysMounted("list").empty());                     // placeholders only while restoring
    CHECK(store->lastLoadKey().model.empty());  CHECK(store->lastLoadKey().table == "list");
    store->release();  h.settle();
    CHECK(h.backend.rowKeysMounted("list") == std::vector<std::string>{"c", "b", "a"});   // first drawn: stored order
    h.clickHeader("Name");  h.advance(500ms);  CHECK(store->saves() == 1);
}
// Mutation: a rows table without `id` given a key (the load goes out with table "").
TEST_CASE("rows table: a table without an id persists nothing", "[ui][table][state]") {
    CHECK(store->loads() == 0);
}
// RecordingBackend::rowKeysMounted(table): the keys of its `setRowKey` calls, in call order.
// Invariant 17: the existing `rows` table suites (test_ui_table.cpp, test_ui_interpreter_table.cpp,
// test_ui_table_row_span.cpp) stay green with no expectation changed.
```
The collection half of "One order on every surface" is Task 13's (step 8): this case covers the `rows` and model-backed surfaces and Task 13 adds the collection. B1 at 20,000 rows is a `[.benchmark]` case in `bench_interpreter.cpp` reading `EngineStats::maxOwnerStepMicros` after a three-key sort and a text filter, asserting first that the engine ran in more than one owner step.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `EngineRows::setRestoring` undeclared; with it stubbed, `ctest --test-dir build/cl-debug -R "one order on every surface|rows table" --output-on-failure` → FAIL: orders differ (the interpreter's `Evaluator::sortRows` is still in use) and no load is made.
- [ ] **Step 3: Implement** as above; delete `ScreenCore::sortedRows` and the ordering part of `tableView`.
- [ ] **Step 4: Run to verify they pass** — the new cases and the whole `morph_tests` target green, then `morph_qt_quick_tests` and `morph_tui_tests` on `cl-qt-debug` green (their table cases mount through the interpreter).
- [ ] **Step 5: Mutations**, as named. Record U1 to U4 and B1 (10,000, 20,000 and 100,000 rows) from `cl-release` into table.md's Performance tables.
- [ ] **Step 6: Neighbouring specs** as listed; docs build green.
- [ ] **Step 7: Commit** `feat(ui): tables over rows on the engine`.

#### 11c — A typed list action as a model-backed table

table.md "The document `table` node on the engine": a node with `model` and `action` names a typed list action; "its description is derived from the action's schema, and the reader turns each row object into cells; it returns whole rows whatever `columns` says".

**What the code gives, and the position taken.** The client's catalog holds an action's **request** schema only (`Catalog::schema`, `document.hpp`: `forms::schemaJson<A>()`, as `RemoteServer`'s `schemas` envelope serves it, `registry.hpp:657`); no reply schema reaches a client. The reply's row type reaches one only through a `v-*` view document (`Catalog::view`, whose `v-columns` `views.hpp:476` derives from the query's row type). So the description is built from, in order: the node's `columns` parts (ids required), else `Catalog::view(action)`'s `v-columns` (ids and labels) and `v-rowKey`; the key from the node's `key`, else `v-rowKey`, else `"id"`; each column's kind from its literal `kind`, else inferred from the first reply's rows by 11a's table and then fixed for the window's life (a later cell not of it is invalid). The request schema must have one top-level member marked `x-table` (`TableQuery::schemaMarker()`, `query.hpp:77`): the query is written into that member of `body`. A node whose action has no such member, or which has neither `columns` nor a view document, is refused when the document loads (`"a table over an action needs a query member marked x-table"`, `"a table over an action needs its columns, or a view document for the action"`). **Stated deviation:** the spec's "derived from the action's schema" cannot be met from the reply schema until a catalog carries one; this is the nearest the client can derive.

**Files:**
- Modify: `include/morph/ui/table_rows.hpp` (`WindowRowsOptions::{action, queryMember}`; `readTypedPage`), `src/ui/interpreter.cpp` (the `model` + `action` lowering), `src/ui/document.cpp` (the two refusals)
- Test: `tests/test_ui_table_model.cpp`
- Docs: `docs/spec/ui/document.md` (the `table` kind with `model` and `action`), `docs/spec/ui/interpreter.md` ("Lowering").

**Interfaces:**

```cpp
// WindowRowsOptions gains
std::string action;        // non-empty: a typed list action's id, sent in place of "Table.<table>"; `table` empty
std::string queryMember;   // with `action`: the body member the query is written into (the x-table member)
/// A typed list action's reply (`Page<Row>`'s JSON: `rows`, `total`, `totalKind`, `offset`, `indices`) as a generic
/// page over @p description's columns: each row object's member of a column's id read by its kind.
[[nodiscard]] std::expected<table::GenericPage, table::TableError> readTypedPage(std::string_view json,
                                                                                table::TableDescription const& description);
```

Decisions:
- `readTypedPage` reads the text with a `glz::generic_u64` DOM (as `buildActionDescription` does, `registry.hpp:688`, so int64 bounds survive): a member absent or `null` → empty; an integer → `int64` (a `double` in a `Number` column); a number with a fraction → `double`; a string → text, or under `Date` `YYYY-MM-DD` → days, under `DateTime` ISO-8601 → floor seconds; `true`/`false` → `bool`; `{num, den, dp}` → `Rational`; with `unit` under `Measure` → `Measure` through `units->find` (unknown → invalid); anything else → the member's JSON text, invalid outside `Text`. The key is the row's member named by the description's `key`, read as a `RowId`. `columns` is every description column, `revision` empty (so `onStaleDescription` never fires).
- `WindowRows` with `action` sends `session.read(options.channel, action, body with body[queryMember] = the query, token)` and decodes the text with `readTypedPage`; the reply holds whole rows, so `visible`'s columns name what the record converts (K8 holds) but not what is fetched. Keyed refetches and `highlightKey` send `keys` in the query member; an action whose reply has no `indices` for a keyed query invalidates the window (`PageWindow::patch`'s rule), which costs a page refetch and not a wrong row.
- The record watched is each `ActionInfo::records` entry of the action (`document.hpp:56`); with none, the window refetches only on `invalidate` (reconnect).

- [ ] **Step 1: Write the failing tests**

```cpp
// Mutation: readTypedPage reading {num, den, dp} as text (the Decimal column's cells are invalid).
TEST_CASE("model table over an action: a typed list action's rows read into the window", "[ui][table]") {
    // node {"kind":"table","model":"Lims_SampleModel","action":"Lims_SampleModel.list","key":"id",
    //       "columns":[{"id":"id"},{"id":"name"},{"id":"weight"},{"id":"ratio"}]}; the request's `query` member is x-table
    h.replyAction(R"({"rows":[{"id":1,"name":"a","weight":{"num":5,"den":2,"dp":1},"ratio":0.5}],"total":1})");
    CHECK(lastBody(h).contains(R"("query":{)"));                        // the query in the x-table member
    CHECK(kindOf(rows, "weight") == table::ColumnKind::Decimal);  CHECK(kindOf(rows, "ratio") == table::ColumnKind::Number);
    CHECK(rows.row(0)->row.field("weight")->asDecimal() != nullptr);
    CHECK(statusOf(h).counts == "1 row");
}
// Mutation: the lowering accepting an action with no x-table member (the first fetch sends a bare body).
TEST_CASE("model table over an action: an action without a query member is refused at load", "[ui][table]") {
    CHECK(loadRefusal(kActionTableNoQuery) == "a table over an action needs a query member marked x-table");
}
```

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `readTypedPage` undeclared.
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "model table over an action" --output-on-failure`; `morph_tests` green.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Neighbouring specs** as listed; docs build green.
- [ ] **Step 7: Commit** `feat(ui): a typed list action as a model-backed table`.

---

## Stated deviations and decisions for the spec's author

Each is taken by this plan and stated where it applies; none edits table.md. The spec is updated once its author confirms.

1. **An empty `model` in a table-state key** (9c). table.md's "Bounds" says every key member is 1 to 256 bytes; the key's own comment, and every `rows` table, need `model` empty. The plan allows `model` 0 to 256 bytes.
2. **Pages are read from the reply's text** (9a, cross-part decision 30). A `Value` cannot hold a JSON number with a fraction, so `Session::read` resolves with the reply's JSON text rather than the spec's `Completion<Value>`, and `WindowRows` decodes it with `readGenericPage` (or `readTypedPage`). The description still comes through the query cache, losslessly.
3. **A `rows` table orders timestamps to the second** (11a), as a `DateTime` column holds them; the expression language's `sort` orders to the millisecond.
4. **A typed list action's description** (11c) comes from the node's `columns` or the action's view document, with kinds inferred from the first reply, because no client catalog carries a reply schema.
5. **The quota spans applications** (9c): one usage entry per person, under the model's own id, as "across one person's stored state" reads.

## Self-review notes

- Spec coverage: "The pipeline" and "Sources" — 9a (`EngineRows`, `WindowRows`), 11a (`ValueRowsSource`); `VectorSource` is Task 13's. "The client table model" — 9a, 9d. "The renderer" — 10a, 10b; its "Range selection in server mode" lands in 10a (cross-part decision 13), although Delivery order lists range selection under step 7. "The document `table` node" — model-backed rows in 9d, `rows` rows in 11b; `cellMenu`, `headerMenu`, `layout`, `configurable`, `locked`, display parts and `maxSortKeys` are Tasks 12, 15, 16, 17. "Where sort and filter run" — the client-mode bound warning in 11b; the refusal of a client-mode sort on a column with a level needs a `rows` column with a level, which a document cannot declare yet, so it has no task here. "Persisted state" (sort and filters, both stores, load before rows) — 9b to 9d; saved and shared views are Task 14's.
- Cross-part decisions 11 (`tableMessage`, `kStatusMessageHold`: 9d), 28 (`TableSource::cellView`/`rowView`/`alwaysFetched`, cells owning a row signal, `RecordOf` → `RowRecord`: 9a, 9d, 10a) and 29 (`CellLoad`, `setCellLoad`: 9d, 10b). `highlightKey` and `model` + `action` are 10a and 11c.
- Gates: U1–U4 (11a/11b), U6 and F1, F6, F7, K5, K8, T1, T3–T5, W6 (9), F1–F5, F8–F10, U7, D8 (10), U8 (10c, measured locally in a headless browser), B1 (11b).
