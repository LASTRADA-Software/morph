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
- Test support (cross-part decision 42). `tests/ui_table_support.hpp` holds two types in `morph::ui::testing`, beside the existing `Harness` of `tests/ui_interpreter_support.hpp`, which is not edited. `RowsFixture` (9a) is what a `TableRows` test needs with no screen: `owner` (`StepExecutor`), `runtime`, `scheduler` (`ManualScheduler`), `session` (its `SessionOptions::catalog` is `tableCatalog()`, and its constructor calls `session.setLocale("en")`), `dispatcher` (`ScriptedDispatcher`), `settle()`, `advance(ms)`. `TableHarness` (9d) derives from it and adds `host` (`RecordingHost`), `backend` (`RecordingBackend`), `diagnostics` (a `std::vector<Refusal>` data member), `interpreter`, and `mount(text)`, which loads against `tableCatalog()`. Every helper is a free function taking the fixture (`lastQuery(h)`, `statusOf(h)`, `replyDescribe(h)`, `replyPage(h, json)`, …). A header click is the existing `h.backend.clickHeader(table, column)` (`recording_backend.hpp`), `table` being `h.backend.find("Table", "testId", …)` and `column` an index into `Table::columns`.
- Every test document is whole and names its model by an alias: `kModelTable` is `{"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","empty":[{"kind":"text","text":"No samples"}]}}`.
- A `column` argument is an index into `Table::columns`, which holds every column, hidden ones included, and does not change with the layout; only a `position` is a visible place (cross-part decision 37).
- `ui::Table` and `ui::TableOptions` (`view.hpp:2026`) are parallel structs: every member a task here adds to `Table` it adds to `TableOptions`, and the lowering fills both (cross-part decision 41).
- Equality (cross-part decision 38): `FilterEntry`, `ColumnFilter`, `GroupFilter` and `ColumnCount` have `operator==` from Part C's 8b; every struct this part adds that is compared or sits in a `Prop` declares its own.
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

1. **A description that changes revision while state is held** (roadmap item 1). A sort key, a filter and the fetched columns naming column `c` when a page reply carries a new `revision` whose description lacks `c`: the client refetches the description once, drops the key and the filter (the filter reported once; a dropped sort key is not reported, as 9b has it), mounts the table again from the new description, and the next query names neither `c` nor its level. A view selection and a cell draft do not exist before Tasks 17a and 24; those tasks extend this test. Test: Task 9d.
2. **The table unmounted while its state load or a fetch is in flight.** A screen closed before a held load completes, then the load released: nothing is applied, no query is sent, no handler runs on a destroyed table, and the debounced save of a changed state goes out at once on unmount. Test: Task 9d.
3. **A stored document that is not JSON, or of a version the framework does not know.** The table starts from its defaults, the problem is reported once at the table's path, nothing is written until the user changes something, and then the framework's version overwrites it. Tests: Task 9b (the parser), 9c (the store's merge) and 9d (the table).
4. **An empty model-backed view.** Before the first reply, and while rows are undecided, the node's `empty` content is not shown and the status line says the table is loading or counts "0 so far, N loading"; after a reply with `total: 0, totalKind: exact, undecided: 0` it is shown and the count reads "0 rows". Test: Task 9d.
5. **Rows with a duplicate key in a `rows` table.** Three rows with keys 1, 2, 1: the table shows two rows, the first occurrence of key 1 in the engine's order, the duplicate is reported as the mount reports it today (`site::kDuplicateKey`, once per duplicate), and a sort never brings the second row back. Test: Task 11a.

---

## File map

| File | Task | Holds |
|---|---|---|
| `include/morph/ui/table_rows.hpp` (new) | 9a, 11a | `TableTotal`, `TableRowRef`, `TableCountsView`, `TableRows`, `EngineRows`, `WindowRows`, `WindowRowsOptions`, `WindowRowsStats`, `cellValue`, `countsText`, the stand-in cell text, `RowRecord`, `RecordOf` (`ui::SortChain` is `table_config.hpp`'s, Task 12a); `ValueRowsSource`, `kGroupRunColumn`, `EngineRows::setRestoring` (11a, 11b); `readTypedPage`, `WindowRowsOptions::{action, queryMember}` (11c) |
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
| `src/ui/document.cpp` | 9d, 11b, 11c | the loader: a table's keys `table`, `status`, `persist`, `model` + `table` without `action` (9d); a column's `kind`, the `persist`-without-`id` refusal (11b); the `model` + `action` refusals (11c) |
| `include/morph/ui/preferences.hpp` | 9c | `PreferencesModel` refuses the names table state reserves |
| `tests/ui_table_support.hpp` (new) | 9a, 9d | `RowsFixture`, `WindowRowsProbe`, `TestRowsSource`, the test catalog, description and page builders (`pageJson`, `pageJsonAt`, `keyedJson`) (9a); `GatedTableStateStore` (9c); `TableHarness` and the screen helpers (9d) |
| `tests/test_ui_document.cpp` | 9d, 11b | the loader cases |
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
// `ui::SortChain` is `table_config.hpp`'s alias (Task 12a); this header includes it and declares no other.
struct TableTotal;          // rows, kind, known
struct TableRowRef;         // key, row
struct TableCountsView;     // view, source, undecided, pending, awaiting, failed
class TableRows;            // total, row, unloadedLevels, visible, keysOf, setSort, setFilter, counts, onChange
// `onChange` is a single slot (cross-part decision 40): a call replaces the handler, and the mount is what sets it.
// Whatever else needs a source's changes wraps the `TableRows` in a forwarding decorator and hands the wrapper to
// the mount. Its Doxygen says so.

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
    void setDescription(Value const& described);                // the describe entry's value; one that does not read sets error()
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

`ui::SortChain` is already declared in `ui/table_config.hpp` (Part E's Task 12a, which landed first) as `using SortChain = table::SortChain;`. Task 9a includes that header and reuses the alias rather than declaring it again.

- `SessionOptions` gains `std::chrono::milliseconds tableSettle{80}`, `std::chrono::milliseconds tableAwaitedRefetch{250}` (this commit) and, in 9c, `std::shared_ptr<table::ITableStateStore> tableState`, `std::chrono::milliseconds tableStateTimeout{300}`, `std::chrono::milliseconds tableStateDebounce{500}`, `std::string application` (the `application` of every table-state key).

Decisions the signatures do not carry:

- `EngineRows` memoises `RecordOf`'s result per source row of the engine's `viewSnapshot()`; the memo is dropped for the rows an `Updated` names and whole on a new snapshot of `Reset`. `row(index)` returns a `TableRowRef` whose references point into the memo and hold until the next `onChange`.
- Decoding replies. Every page, keyed and column fetch is sent as `session.read(options.channel, "Table." + table, body, token)` (cross-part decision 30): no cache entry is made, no effect is applied, and the fetch's stop token cancels it. `Session::read` resolves with the reply's **text**, which `WindowRows` decodes as `table::readGenericPage(text, *description())`; a reply is never read into a `Value`, because `ui::valueFromJson` (`value.hpp:570`) refuses every JSON number with a fraction or an exponent ("a number with a fraction is not exact"), so a page holding one `Number` cell (`2.5`, `1e308`, `-0.0`) would fail whole. The description is read from the `Table.<table>.describe` query entry (W6), whose `Value` is lossless for a description — every number in one is an integer or a `math::Rational` written as `{"num","den","dp"}` (`rational.hpp:1747`), which `valueFromJson` reads as a `Decimal` and `valueToJson` writes back unchanged — as `glz::read_json<table::TableDescription>(ui::detail::canonicalJson(value))` (`interpreter.hpp:410`). A description that does not read sets `error()` to `TableError{.code = InvalidValue}` and sends no page fetch.
- `cellValue`: empty and unloaded → `null`; `int64` in `Integer`/`Key` → `Int`; in `Date` → the `String` `YYYY-MM-DD`; in `DateTime` → `Timestamp` of seconds × 1000 ms; `Rational` → `Decimal{rational}` (its places kept); `double` → `Decimal` parsed by `table::parseDecimal` from the text `std::to_chars(first, last, value, std::chars_format::fixed)` writes (the shortest fixed-notation text that reads back as the same `double`, never an exponent, which `parseDecimal` refuses, `data_source.hpp:123`); a `double` that text does not parse for — non-finite, more than 18 fraction digits (`1e-20`), or a magnitude beyond an `int64` numerator (`1e308`) — is the `String` of `std::to_chars`'s shortest text (`"1e+308"`, `"inf"`), so the cell still shows its value and is never silently `null`, the rule cross-part decision 46 gives the query cache; text → `String`; `bool` → `Bool`; `QuantityCell` → `Decimal` of `amount × toCanonical`; a `Measure` (once Task 8c lands) → `Quantity{amount, unit id}`.
- `standInCellText` is `table::displayText(cell)`, and the empty string for an unloaded cell.
- `countsText`, numbers grouped with `format.numeric`'s separator: nothing known (`!view.known`) → `""`; `undecided > 0` → `"<view> so far, <undecided> loading"`; `selected > 0` → `"<selected> of <view> selected"`, `<view>` followed by `" or more"` under `AtLeast`; `AtLeast` → `"<view> or more"`; otherwise `"1 row"` or `"<view> rows"`.
- `WindowRows` row records: `row(index)` returns a record holding one field per column of the latest `visible(…, columns)` set for which `PageWindow::holds(index, column)`, each read with `cell(index, column)` and its key with `key(index)`, memoised per key until the row's cells change; `cellsConverted` counts each conversion. A record of a 300-column table of which twenty are named converts twenty cells.
- `WindowRows` timing: a fetch for a page held or adjacent to a held page goes at once, and so does the page of the first visible row while no page is held (the first query, and the one after a sort or filter); any other waits until `visible` has stayed within one page for `tableSettle`, measured on `SessionOptions::scheduler`; every `superseded()` fetch is cancelled through its stop token and handed to `cancelled`. While `awaiting()` is not empty, every event invalidates (table.md, "When a fetch is sent") and a refetch of a held page goes no sooner than `tableAwaitedRefetch` after the last one: the first after a quiet spell at once.
- `WindowRows` order of work: `watchRecord` first; the first page fetch only after it resolves, after `setDescription`, and (with `restoring`) after `release`. Its handler maps a change with `keys` to `refresh` of each key read by `rowIdOf` (a key that does not read invalidates), and a change without `keys` to `invalidate()`; after every `accept` it sends `owed()` and calls `visible` again.
- A reply whose `revision` differs from the held description's calls the `onStaleDescription` handler once per revision and is still accepted.
- `WindowRows`' change is the diff by key of the rows held before a reply against those held after it (table.md, "The client table model"): a new total moves the view's end (`Inserted`/`Removed` there), the reply's page is diffed by key (`Moved`, `Inserted`, `Removed`, and `Changed` for every held row of the page, whose cells were replaced), and an evicted page is `Changed` places. The operations are valid in sequence and never hold a key twice: a shrunk view loses its end before the page's diff, a grown view gains it after, and the diff covers the page's places both views have. A new query (sort, filter, description revision) is a `Reset`, and so is a page holding a key twice or a page diff over the `ViewChange` bound of 5,000 operations. Tests: a row that moves within its page is a move; a new total with the same keys moves the end; a row that came and a row that went are an insert and a remove; a page spanning a shrunk view's end, and one spanning a grown view's end. Each replays the change op by op, checking every op in bounds, the keys unique after each, `Changed` places read from the new view, and the end equal to the new view.
- `retry()`'s flag stays set until every fetch of that refetch has gone, one held back by `tableSettle` or `tableAwaitedRefetch` included. A keyed reply that does not read, and a keyed body that does not write, are handled as a failed keyed fetch: `error()` is set and the window invalidates.
- The client catalog keeps a table action's opaque holding: `ui::author::detail::recordsInfo` copies `ActionRecord::opaque`, tested through `recordsInfo` itself, which `CatalogFacts::action` uses (`"table catalog: a table's action holds its record opaquely in the client's catalog"`). `CatalogFacts::action` cannot name a table action, which is not a `Runs` action; 9d builds its catalog by hand and carries that gap.
- The reader bounds what the window indexes by: `table::readGenericPage` refuses (`InvalidValue`) a page whose `unloadedRows` index is outside its rows or not above the one before, or whose `indices` is neither empty nor one non-negative view row per row. Test `"generic page: unloaded rows and indices are bounded by the page's rows"` in `tests/table/test_table_description.cpp` (mutation: the check removed).
- Beyond the cases below, 9a tests a description that does not read (no page fetch), a reply of another revision (reported once, taken) and a change with keys (one keyed fetch that patches in place).

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp`, over `RowsFixture` (Global Constraints; `h` below) and the helpers of `tests/ui_table_support.hpp`. `h.windowRows(table, description = samplesDescription(3))` returns a `std::shared_ptr<WindowRows>` (a `WindowRows` is neither copied nor moved: its watch handler holds `this`) built with the fixture's channel, the description set and `release()` called, after answering the `watch`; `h.restoringRows(table)` is the same with `restoring = true` and no `release()`. `h.columns` is `h.columnIds(0, 3)`. `h.answerFirstPage(rows, total)` calls `rows.visible(0, 30, h.columns)` and answers every page fetch that causes — page 0, then page 1, which goes at once beside it — each with `pageJson(<its offset>, 200, total, h.columns)`, so nothing is in flight afterwards (a page 1 left in flight would be superseded by the first scroll and counted in `cancelled`). `h.replyPage(json)` answers the oldest unanswered `Table.samples` call; `h.pending("Table.samples")` counts the unanswered ones; `h.stopped(index)` is whether call `index`'s `CallOptions::stop` was requested (`ScriptedDispatcher::Call` gains `::core::async::StopToken stop`, recorded from the options of `execute`, in Task 7b's change to that file; if it is not there, this commit adds it). `h.change(keys)` delivers an `effects::RecordChange` for `Lims_SampleModel.samples` with those keys to the session, as a `changes` event does. `tableCatalog()` (the corpus catalog plus `Table.samples` and `Table.samples.describe` on `Lims_SampleModel`), `samplesDescription(int columns)` (`id` Integer key, `name` Text, then `c3…` Decimal), `describeJson(TableDescription)`, `pageJson(offset, count, total, columns, TotalKind kind = Exact, std::int64_t undecided = 0, std::vector<std::string> awaiting = {})` whose row keys are `offset + i` and names `"row-<key>"`; `formatFor(std::string_view tag) -> FormatContext`, built from the locale table as `Session::scopedFormatting` builds one (`interpreter.cpp:777`): `LocaleFacts const& f = ui::localeFacts(tag);` then `FormatContext{.locale = std::string{tag}, .numeric = f.numeric, .currency = f.currency, .datePattern = std::string{f.datePattern}, .dateTimePattern = std::string{f.dateTimePattern}}` (`f.numeric` views static storage, `locale_table.hpp:78`). A default `FormatContext` has an empty `groupSeparator` (`render::NumericLocale{}`), and so does a `Session` until `setLocale` runs (`interpreter.hpp:598`), so `RowsFixture`'s constructor calls `session.setLocale("en")` (and only it: the existing `Harness` is untouched, cross-part decision 42): every count asserted through a screen ("9,093 rows", "100,000 rows") is grouped by the locale table, not by a default. `h.columnIds(first, count)` is the ids of the description's columns in order; `h.window(rows)` is a test accessor to the `PageWindow` (`WindowRows` befriends `ui_table_support`'s `WindowRowsProbe`). `TestRowsSource(n)` is a `table::DataSource` of `n` rows `{id: i, name: "row-<i>"}` with `rename(row, name)` (notifies `Updated` for that row) and `recordOf()`; `valueToJsonText(value)` is `glz::write_json(valueToJson(value))`; `h.keysNow(rows, first, count)` calls `keysOf`, runs `h.settle()` once and returns the keys, failing the test when the completion has not delivered by then or a `Table.samples` call was made.

```cpp
// Mutations: a fetch per `visible` call (the first CHECK: 19 calls); no cancel of `superseded()` (the `stopped` and
// `cancelled` CHECKs); an adjacent page waiting for the settle (the last CHECK).  Gate K6.
TEST_CASE("window rows: a scroll that passes sends no fetch until it rests, and cancels what it left", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 100'000);                     // pages 0 and 1 are held
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t at = 2'500; at < 50'000; at += 2'500) { rows->visible(at, 30, h.columns); h.advance(10ms); }
    CHECK(h.dispatcher.count("Table.samples") == before);           // inside tableSettle each time
    h.advance(80ms);                                                 // rests at 47,500
    std::size_t const rested = h.dispatcher.count("Table.samples");
    CHECK(rested > before);  CHECK(rested <= before + 3);            // K6: at most three for a resting place
    for (std::int64_t at = 50'000; at < 100'000; at += 2'500) { rows->visible(at, 30, h.columns); h.advance(10ms); }
    h.advance(80ms);                                                 // rests at 97,500; the first rest is unanswered
    for (std::size_t call = before; call < rested; ++call) { CHECK(h.stopped(call)); }   // what it left is cancelled
    CHECK(rows->stats().cancelled == rested - before);
    std::size_t const second = h.dispatcher.count("Table.samples");
    CHECK(second > rested);  CHECK(second <= rested + 3);
    while (h.pending("Table.samples") > 0) { h.replyPage(pageJson(97'400, 200, 100'000, h.columns)); }
    rows->visible(97'790, 30, h.columns);  h.settle();               // into the page after a held one, no clock advance
    CHECK(h.dispatcher.count("Table.samples") == second + 1);        // at once
}
// Mutation: a refetch per event (the first refetch's reply is older than the events after it and is fetched again
// at once: the 249 ms CHECK counts two). A refetch in flight dedupes the events after it, so the first CHECK holds
// under the mutation; a view of one page keeps each refetch one fetch.
TEST_CASE("window rows: an awaited order is throttled", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    std::string const awaited = pageJson(0, 200, 200, h.columns, table::TotalKind::Exact, 0, {"recipe"});
    rows->visible(0, 30, h.columns);  h.replyPage(awaited);          // the order awaits `recipe`
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t key = 0; key < 10; ++key) { h.change({key}); h.advance(10ms); }   // ten events in 100 ms
    CHECK(h.dispatcher.count("Table.samples") == before + 1);        // one, at the first event
    h.replyPage(awaited);
    h.advance(149ms);  CHECK(h.dispatcher.count("Table.samples") == before + 1);   // 249 ms after it
    h.advance(1ms);    CHECK(h.dispatcher.count("Table.samples") == before + 2);   // tableAwaitedRefetch
    h.replyPage(awaited);  h.advance(1'000ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 2);        // no event since: nothing more
}
// Mutation: build the record from every column the window holds for the row (converts 40, not 20).  Gate K8.
TEST_CASE("window rows: a row of 300 columns converts the twenty it is asked for", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples", samplesDescription(300));
    std::vector<std::string> const forty = h.columnIds(0, 40);      // id, name, c3 … c40
    std::vector<std::string> const twenty = h.columnIds(0, 20);     // the first twenty of them
    rows->visible(0, 50, forty);
    h.replyPage(pageJson(0, 200, 9093, forty));                     // the window now holds 40 columns of row 0
    REQUIRE(h.window(*rows).holds(0, "c40"));                       // the precondition K8 needs: 40 held
    std::size_t const sent = h.dispatcher.count("Table.samples");
    rows->visible(0, 50, twenty);  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == sent);             // held already: no fetch
    REQUIRE(rows->row(0).has_value());
    CHECK(rows->row(0)->row.asRecord()->size() == 20);
    CHECK(rows->stats().cellsConverted == 20);                      // K8; the mutation converts 40
    static_cast<void>(rows->row(0));  CHECK(rows->stats().cellsConverted == 20);  // memoised
}
// Mutations: decoding through a `Value` (`ui::valueFromJson` refuses 2.5 and the page fails whole: `error()` is set);
// a double `parseDecimal` refuses read as `null` (the row 1 REQUIRE).
TEST_CASE("window rows: a page with a Number cell is read from the reply's text", "[ui][table]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(2);              // id Integer key, then one more column
    d.columns[1].info = table::ColumnInfo{.id = "ratio", .kind = table::ColumnKind::Number};
    auto const rows = h.windowRows("samples", table::describe(d));
    std::vector<std::string> const columns{"id", "ratio"};
    rows->visible(0, 30, columns);
    h.replyPage(R"({"revision":")" + rows->description()->revision +
                R"(","columns":["id","ratio"],"keys":[1,2],"rows":[[1,2.5],[2,1e308]],"total":2})");
    REQUIRE_FALSE(rows->error().has_value());
    CHECK(rows->total().rows == 2);
    REQUIRE(rows->row(0)->row.field("ratio")->asDecimal() != nullptr);          // cellValue: double → Decimal
    CHECK(valueToJsonText(*rows->row(0)->row.field("ratio")) == R"({"num":5,"den":2,"dp":1})");
    REQUIRE(rows->row(1)->row.field("ratio")->asString() != nullptr);           // beyond a Decimal: its text
    CHECK(*rows->row(1)->row.field("ratio")->asString() == "1e+308");
}
// Mutation: send the first page before `release`.
TEST_CASE("window rows: a restoring window sends no query until released", "[ui][table]") {
    RowsFixture h;  auto const rows = h.restoringRows("samples");
    rows->visible(0, 30, h.columns);  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);
    REQUIRE(rows->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));
    h.settle();  CHECK(h.dispatcher.count("Table.samples") == 0);   // still restoring
    rows->release();  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 1);                // T3: one query, carrying the sort
    CHECK(h.dispatcher.calls.back().body.contains(R"("column":"name")"));
    CHECK(h.dispatcher.calls.back().body.contains(R"("dir":"desc")"));   // the wire's name for a direction
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
// Mutations: `keysOf` completing only after the next view change (`keysNow` fails the test); `row` answering nothing
// outside the last `visible` range (the row 499 REQUIRE).
TEST_CASE("engine rows: every row is present, visible does nothing, keysOf completes at once", "[ui][table]") {
    RowsFixture h;  auto const source = std::make_shared<TestRowsSource>(500);
    EngineRows rows{source, source->recordOf(), table::EngineOptions{.owner = &h.owner}};
    CHECK(rows.total().rows == 500);  CHECK(rows.total().known);  CHECK(rows.total().kind == table::TotalKind::Exact);
    rows.visible(0, 10, {});  h.settle();                           // names ten rows and no column
    REQUIRE(rows.row(499).has_value());
    CHECK(rows.row(499)->key == table::RowId{std::int64_t{499}});
    CHECK(*rows.row(499)->row.field("name")->asString() == "row-499");
    CHECK(rows.unloadedLevels(499).levels.empty());
    CHECK_FALSE(rows.row(500).has_value());
    CHECK(h.keysNow(rows, 100, 3) == std::vector<table::RowId>{std::int64_t{100}, std::int64_t{101}, std::int64_t{102}});
    CHECK(rows.counts().undecided == 0);  CHECK_FALSE(rows.counts().pending);
}
// Mutation: no memo (recordsBuilt is 2 after the two reads).
TEST_CASE("engine rows: a row's record is built once until the row changes", "[ui][table]") {
    RowsFixture h;  auto const source = std::make_shared<TestRowsSource>(10);
    EngineRows rows{source, source->recordOf(), table::EngineOptions{.owner = &h.owner}};
    static_cast<void>(rows.row(0)); static_cast<void>(rows.row(0));  CHECK(rows.recordsBuilt() == 1);
    source->rename(0, "renamed");  h.settle();
    CHECK(*rows.row(0)->row.field("name")->asString() == "renamed");  CHECK(rows.recordsBuilt() == 2);
    static_cast<void>(rows.row(1));  CHECK(rows.recordsBuilt() == 3);   // another row: its own record
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

Needs Task 8c (`fitState` reads `TableDescription::units` directly) and Task 8b (`operator==` on `FilterEntry`, `ColumnFilter`, `GroupFilter` and `ColumnCount`, which `CarriedState`'s defaulted `operator==` uses; cross-part decision 38).

**Files:**
- Create: `include/morph/table/state_store.hpp`, `tests/table/test_table_state.cpp`
- Modify: `CMakeLists.txt` (`include/morph/table/state_store.hpp` in the `morph` target's `FILE_SET HEADERS`; configure refuses a public header no file set lists), `tests/CMakeLists.txt` (`table/test_table_state.cpp` in `morph_table_tests`), `tests/table/bench_table.cpp` (T1)

**Interfaces:**
- Consumes: `table::ColumnLayoutValue` and `ColumnLayout::apply` (8d), `table::TableDescription`, `table::FilterSpec`, `table::compileFilter` (existing compile path in `filter.hpp`).
- Produces (namespace `morph::table`): `TableStateKey`, `StoredTableState`, `SaveOutcome`, `SaveResult`, `ITableStateStore` exactly as table.md "Persisted state" declares them (`shares()` included, default `false`), except `publish` and `kNoDocument`, which Part E's Task 14a adds with `ViewOperation`, with one addition: `TableStateKey` declares `[[nodiscard]] bool operator==(TableStateKey const&) const = default;` (tests and the session's kept states compare keys; cross-part decision 38), and:

```cpp
class MemoryTableStateStore final : public ITableStateStore {
public:
    explicit MemoryTableStateStore(exec::IExecutor& executor);  // completions delivered there
    // revisions are "1", "2", … per key. Part E's Task 14a adds a defaulted second parameter, giving
    // `explicit MemoryTableStateStore(exec::IExecutor&, Sharing = Sharing::Private)`; every call here stays valid.
};

inline constexpr int kStateVersion = 1;
/// Parts of a stored state that name a column or group the caller's description lacks. Not applied; merged back
/// by `writeState` into the places they had.
struct CarriedState {
    struct Column {
        std::string id;
        std::string after;              // the id it followed in the stored `order`; empty: it was first
        bool hidden = false;
        std::optional<int> width;
        std::map<std::string, std::string> units;   // dimension -> unit id, as stored
        [[nodiscard]] bool operator==(Column const&) const = default;   // cross-part decision 38
    };
    std::vector<Column> columns;        // in stored order
    std::vector<std::pair<std::size_t, SortKey>> sort;   // each key with its index in the stored chain
    std::map<std::string, ColumnFilter> filters;         // by the absent column's id
    std::vector<GroupFilter> groupFilters;               // naming an absent group
    std::vector<ColumnGroupInfo> groups;                 // user groups that name an absent column
    /// `ColumnFilter` and `GroupFilter` compare by Part C's 8b `operator==` (cross-part decision 38).
    [[nodiscard]] bool operator==(CarriedState const&) const = default;
};
struct StateDocument {                  // part "state"; declared after `CarriedState`, which it holds
    int version = kStateVersion;
    std::optional<ColumnLayoutValue> layout;
    std::optional<SortChain> sort;      // `table::SortChain` (`sort.hpp:71`)
    std::optional<FilterSpec> filters;
    std::optional<std::string> view;
    CarriedState carried;               // what the caller's description lacks, written back as it was
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
- The stored shape is table.md's, not the wire's. A stored sort key is `{"column": id, "direction": "asc" | "desc"}`, while `table::SortKey`'s member is `dir` and its wire form `{"column", "dir"}` (`sort.hpp:58-62`, `engine.md` "a sort key is `{ "column", "dir" }`"). `readState` and `writeState` therefore map the `sort` part by hand through a file-local `struct StoredSortKey { std::string column; SortDirection direction; }` (glaze reflects its member names; `SortDirection`'s `glz::meta` already spells `asc`/`desc`), converting to and from `SortKey`; they never reflect `SortKey` itself, which would read `direction` as an unknown key and leave every key ascending. `layout` reads as `ColumnLayoutValue` and `filters` as `FilterSpec`, whose reflected names are the stored ones.
- `readState` refuses text that is not JSON and a `version` above `kStateVersion` with a message; a lower version is migrated one version at a time (none exist yet, so the migration table is empty and its loop is tested by a version-0 fixture added when a version 2 exists — not now).
- `fitState` applies table.md's fitting list in order: columns as `ColumnLayout::apply` fits them; sort keys on missing or unsortable columns dropped; filters on missing columns or groups, filters that do not compile for the column's kind, and units not in `description.units` dropped; user groups left with fewer than two columns dropped with their filter. A drop because the description lacks the column or group moves the entry into `carried`; any other drop is gone. A user group that names an absent column is carried whole (it is whole again when the column returns); one left with fewer than two present columns for any other reason is gone with its filter. A user-formed group's filter is a `GroupFilter` with `group` empty and `columns` set to the group's columns (Part C's `compileGroup` takes exactly one of `group` and `columns`), so `fitState` finds the filter of a user group by `columns` equal to that group's `columns` (as sets), and moves or drops it with the group; a `GroupFilter` naming a declared `group` is fitted against `description.groups` by id.
- `writeState` merges `carried` back: a column into `order` right after its `after` id, or first when `after` is empty, or at the end when `after` is itself absent from the written order; its hidden flag, width and units with it; a sort key at its stored index, clamped to the chain's length; filters, group filters and groups appended. Reading that text with the same description carries the same entries again.
- The lowering reports, once each at the table's path, the drops the spec says are reported: `Filter`, `GroupFilter` and `Unit`. `Column`, `SortKey` and `Group` drops are not reported.

- [ ] **Step 1: Write the failing tests** in `tests/table/test_table_state.cpp`. The file's fixtures, each defined once:

- `describedSamples()` is `table::describe` of: columns `id` (`Integer`, the key), `name` (`Text`), `alias` (`Text`), `weight` (`Measure`, `dimensions = {"mass"}`), in that order, each `sortable`, `filterable` and `shown`; `units` lists `g` and `kg` of dimension `mass`; one declared group `g1 = {name, alias}`. It has no column `gone`, `a` or `b` and no group `g2`.
- `userGroups()` is `readState` of `{"version":1,"layout":{"order":["id","name","alias","weight"],"groups":[{"id":"u1","label":"One","columns":["name","gone"]},{"id":"u2","label":"Two","columns":["name","alias"]}]},"filters":{"columns":{},"groups":[{"columns":["name","gone"],"include":[{"contains":"x"}]},{"columns":["name","alias"],"include":[{"contains":"y"}]}]}}`.
- `StepExecutor executor;` and `saveNow(store, key, json, expected = {}) -> SaveResult`, `loadNow(store, key) -> std::optional<StoredTableState>`: each calls the store, runs `executor.runAll()` and returns what the completion delivered (failing the test when it delivered nothing).

```cpp
// Mutation: fitState returning the document unchanged (the sort, filter and hidden CHECKs fail).
TEST_CASE("table state: stale state is fitted", "[table][state]") {
    TableDescription const d = describedSamples();
    StateDocument const stored = readState(R"({"version":1,
      "layout":{"order":["id","gone","name","alias","weight"],"hidden":["id","name","alias","weight"],"pinned":2,
                "widths":{"gone":9},"units":{"weight":{"mass":"zz"}}},
      "sort":[{"column":"gone","direction":"asc"},{"column":"name","direction":"desc"}],
      "filters":{"columns":{"gone":{"include":[{"eq":"x"}]},"weight":{"include":[{"gt":"abc"}]}},
                 "groups":[{"group":"g2","include":[{"eq":"x"}]}]}})").value();
    REQUIRE(stored.sort->at(1).dir == SortDirection::Descending);         // "direction" was read, not ignored
    FitReport report;
    StateDocument const fitted = fitState(stored, d, report);
    CHECK(fitted.layout->order == std::vector<std::string>{"id", "name", "alias", "weight"});
    CHECK(fitted.layout->hidden.size() < 4);                              // every column was hidden: one shows
    CHECK(*fitted.sort == SortChain{{.column = "name", .dir = SortDirection::Descending}});
    CHECK(fitted.filters->columns.empty());
    CHECK(fitted.filters->groups.empty());
    CHECK_FALSE(fitted.layout->units.contains("weight"));                 // unknown unit "zz" dropped
    using W = FitDrop::What;
    CHECK(report.dropped == std::vector<FitDrop>{
        {W::Column, "gone", true}, {W::SortKey, "gone", true}, {W::Filter, "gone", true},
        {W::Filter, "weight", false}, {W::GroupFilter, "g2", true}, {W::Unit, "weight/mass", false}});
    CHECK(fitted.carried.columns == std::vector<CarriedState::Column>{{.id = "gone", .after = "id", .width = 9}});
    std::string const written = writeState(fitted);
    CHECK(written.contains(R"("direction":"desc")"));                     // the stored name, not the wire's "dir"
    CHECK_FALSE(written.contains(R"("dir":)"));
    StateDocument const again = readState(written).value();
    CHECK(again.layout->order == std::vector<std::string>{"id", "gone", "name", "alias", "weight"});   // its place kept
    CHECK(again.sort->at(0).column == "gone");                            // carried key back at index 0
    CHECK(again.sort->at(1) == SortKey{.column = "name", .dir = SortDirection::Descending});
    CHECK(again.filters->columns.contains("gone"));
    CHECK_FALSE(again.filters->columns.contains("weight"));               // did not compile: gone at the next save
    REQUIRE(again.filters->groups.size() == 1);  CHECK(again.filters->groups.front().group == "g2");
    FitReport second;                                                     // reading it again carries the same
    CHECK(fitState(again, d, second).carried == fitted.carried);
}
// Mutation: fitState looking up a user group's filter by `group` id (an empty id finds nothing: u3's filter, whose
// group is dropped for having one column, stays in `filters`). u1's filter cannot show that mutation: a filter over
// listed columns that matches no user group is still fitted by its columns, and one naming the absent `gone` is
// carried either way.
TEST_CASE("table state: a user group and its filter are fitted together", "[table][state]") {
    FitReport report;  StateDocument const fitted = fitState(userGroups(), describedSamples(), report);
    REQUIRE(fitted.layout->groups.size() == 1);  CHECK(fitted.layout->groups.front().id == "u2");
    REQUIRE(fitted.filters->groups.size() == 1);                          // u2's, matched by its columns
    CHECK(fitted.filters->groups.front().columns == std::vector<std::string>{"name", "alias"});
    REQUIRE(fitted.carried.groups.size() == 1);  CHECK(fitted.carried.groups.front().id == "u1");
    REQUIRE(fitted.carried.groupFilters.size() == 1);                     // u1's, carried with its group
    CHECK(fitted.carried.groupFilters.front().group.empty());
    CHECK(fitted.carried.groupFilters.front().columns == std::vector<std::string>{"name", "gone"});
    // A user group left with one column, every column present, is gone, and its filter with it.
    StateDocument const single = readState(R"({"version":1,"layout":{"groups":[{"id":"u3","label":"Three","columns":["weight"]}]},
        "filters":{"groups":[{"columns":["weight"],"include":[{"isEmpty":true}]}]}})").value();
    FitReport dropped;  StateDocument const alone = fitState(single, describedSamples(), dropped);
    CHECK(alone.layout->groups.empty());  CHECK(alone.filters->groups.empty());  CHECK(alone.carried.groupFilters.empty());
}
// Review Focus 3 (the parser's half; the table's half is 9d's). Mutation: readState accepting an unknown version.
TEST_CASE("table state: a document that is not JSON or of an unknown version is refused", "[table][state]") {
    CHECK_FALSE(readState("{not json").has_value());
    CHECK_FALSE(readState(R"({"version":2,"sort":[]})").has_value());
    CHECK(readState(R"({"version":1})").has_value());
}
// Mutation: save ignoring `expected` (the third save answers Saved).
TEST_CASE("memory table state store: a save with a stale revision is a conflict", "[table][state]") {
    MemoryTableStateStore store{executor};
    TableStateKey const key{.application = "lims", .model = "Lims_SampleModel", .table = "samples",
                            .qualifier = "samples", .part = "views"};
    CHECK_FALSE(loadNow(store, key).has_value());
    SaveResult const first = saveNow(store, key, R"({"a":1})");
    CHECK(first.outcome == SaveOutcome::Saved);  CHECK(first.revision == "1");
    SaveResult const second = saveNow(store, key, R"({"b":1})", "1");
    CHECK(second.outcome == SaveOutcome::Saved);  CHECK(second.revision == "2");
    SaveResult const stale = saveNow(store, key, R"({"c":1})", "1");
    CHECK(stale.outcome == SaveOutcome::Conflict);  CHECK(stale.revision == "2");
    CHECK(loadNow(store, key)->json == R"({"b":1})");                     // the conflict wrote nothing
    CHECK(saveNow(store, key, R"({"d":1})").outcome == SaveOutcome::Saved);   // no `expected`: replaces
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
- Modify: `include/morph/ui/interpreter.hpp` (`SessionOptions` members listed in 9a; `Session::tableStates() -> TableStates*`, null without a store), `include/morph/ui/preferences.hpp` (`PreferencesModel` refuses the names table state reserves), `tests/test_ui_preferences.cpp` (that refusal), `tests/ui_table_support.hpp` (`ProbePreferencesStore`), `tests/net/test_push_tables.cpp` ("The person is the store's")

**Interfaces:**
- Consumes: `IPreferencesStore`, `PrefKey` (existing), `session::current()` as `PreferencesModel` reads it, `morph::session::detail::sha256`, `Dispatcher`/`ModelChannel` (existing), 9b's interface.
- Produces:

```cpp
// preferences.hpp, namespace morph::ui (see "Reserved names")
inline constexpr std::string_view kTableStateModel = "morph.tableState";
// table_state_model.hpp, namespace morph::ui
struct TableStateOptions {
    std::size_t segmentBytes = std::size_t{12} * 1024;
    std::size_t partBytes = std::size_t{1} << 20;            // a save part and a load head
    std::optional<std::size_t> maxBytesPerPerson;            // unset: no quota
};
struct TableStateLoad   { std::vector<table::TableStateKey> keys; };
struct TableStateHead   { bool found = false; std::string revision; std::uint64_t size = 0; std::string text;
                          std::string error; };        // why this key failed (a missing segment); the others still load
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
    // `owner`: where `load` answers, a kept key at once, with no store completion to deliver on.
    TableStates(std::shared_ptr<table::ITableStateStore> store, exec::IExecutor& owner,
                reactive::Scheduler* scheduler, std::chrono::milliseconds debounce);
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
- Action ids. `TableStateModel` is registered as `morph.tableState` with four actions, by the traits-only macros `preferences.hpp` uses for its own (`BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY`, `Loggable::No`): `"load"` (`TableStateLoad` → `TableStateLoaded`), `"read"` (`TableStateRead` → `TableStateChunk`), `"save"` (`TableStateSave` → `TableStateSaved`), `"remove"` (`TableStateRemove` → `TableStateRemoved`). `PreferencesTableStateStore` binds `kTableStateModel` once through its `Dispatcher` and sends exactly these ids; Part E's Task 14a adds `"publish"`.
- Reserved names. The preferences model writes to the same `IPreferencesStore` with whatever `app`, `scope` and `point` its caller names (`preferences.hpp`, `keyOf`), so without a rule a person could overwrite the quota's usage entry or a state's head through `morph.preferences`. `PreferencesModel::checkName` therefore refuses, on `load`, `set` and `remove` alike, an `app` equal to `kTableStateModel` (`"morph.tableState"`) and a `scope` that starts with `morph.table` (`std::invalid_argument`, "`<name>` is kept for table state"), before the store is called. The two constants (`kTableStateModel`, `inline constexpr std::string_view kTableStateScopePrefix = "morph.table"`) are declared in `preferences.hpp`, which `table_state_model.hpp` includes, so the model that reserves and the model that refuses read one definition.
- Server layout per table.md "The framework's stores": person from the verified session (none: `load` answers `found = false` for every key, `save` and `remove` are refused); `app` = the key's `application`; scope `morph.table.` + the first 32 lower-case hex digits of `sha256(model + "\x1F" + table + "\x1F" + qualifier)`; point `<part>` holds `{"key": {…in clear…}, "revision": r, "document": …}` for a document of at most `segmentBytes`, else `{"key": …, "revision": r, "segments": n, "bytes": b}` with segments at `<part>.<r>.<i>`, each a JSON string of at most `segmentBytes`. A head whose clear key differs from the asked key reads as not found. A missing segment fails the load.
- Write order, with an intent entry because `IPreferencesStore` has only `get`, `set` and `remove` by exact key (`preferences.hpp:62-73`) and so cannot list the segments under a scope: (1) read `<part>.pending`; if present, it names up to two revisions, `{"revision": r, "segments": n, "previous": r0, "previousSegments": n0}`; read the head, and remove the segments `<part>.<x>.<0…count−1>` of each named revision the head does not name, then remove `<part>.pending`; (2) when the new document or the one the head now names is segmented, write `<part>.pending` naming the new revision (`segments` 0 for an unsegmented one) and the head's (`previousSegments` 0 likewise); (3) the segments; (4) the head; (5) remove the previous revision's segments; (6) remove `<part>.pending`. A save stopped before (4) leaves the old head and its segments readable and the new segments stray; one stopped between (4) and (6) leaves the previous revision's segments stray; in both cases the next save of the key removes exactly the strays in step (1), because the head names the one revision to keep. A save that writes no segments and replaces none writes no intent. A removal that fails in steps (5) or (6) does not fail the save, whose head is written: it leaves strays that the intent names, for the next save.
- Key bounds: `application`, `table`, `qualifier` and `part` are 1 to 256 bytes; `model` is 0 to 256 bytes, because a table over a document's rows has an empty `model` (table.md "Persisted state", the key's comment, the "Bounds" bullet, and Task 11b). The spec already states these bounds and is not edited by this task. The model checks the bounds on every request (a `std::invalid_argument` naming the member, as the preferences model refuses a name) before it reads the store.
- Revision: the first 16 hex digits of `sha256(previousRevision + document)`.
- Quota. The spec's quota is "across one person's stored state", so it spans applications: with `maxBytesPerPerson` set, the model keeps the person's total at `PrefKey{.person = person, .app = std::string{kTableStateModel}, .scope = "morph.table", .point = "usage"}` (`"morph.tableState"` is not an application's id and `"morph.table"` is no state's scope, which is always `morph.table.<32 hex>`), as `{"bytes": n}`. Arithmetic, all in `std::uint64_t` document bytes (the length of the document's text as `save` received it; entries, envelopes and segment quoting are not counted): `n` is the stored value, 0 when the entry is absent or does not read as `{"bytes": <unsigned>}`; `b0` is the replaced document's size, 0 when the key has no head, else the head's `bytes` for a segmented head and the length of its `document` member's text for a small one; `b1` the new document's size. A save is refused when `n − b0 + b1 > maxBytesPerPerson` (computed as `n + b1 > quota + b0`, so no subtraction wraps): it answers `SaveResult{Refused, reason = "over the table-state quota"}` and writes nothing, `usage` included. Otherwise it writes the document (the order above), then `usage = n − b0 + b1`. A remove of a present key writes `usage = n − min(n, b0)`; of an absent key writes nothing. A multi-part save is checked once, at its `last` part, against the whole text. Without the option no `usage` entry is read or written. A preferences store has no conditional write, so two saves of one person reaching two model instances in the same moment can both pass the check; the quota can then be overshot by one document, which an operator's ceiling tolerates as the spec's `expected` rule tolerates its own race.
- `PreferencesTableStateStore::load` calls made in one turn of `owner` go out as one `TableStateLoad`; a head shorter than `size` is completed with `TableStateRead` calls by offset, restarting when the revision changes. A `save` over `partBytes` is sent as parts under one upload id (`upload` = a random 16-hex id), committed by the part with `last`.
- `onSaveFailed` is one slot for the session: the handler is called on the owner for every save of a `state` part that did not end `Saved` (the store's completion failed, or answered `Refused` or `Conflict`), with the key and the result (`outcome = Refused` and the failure's text as `reason` for a failed completion). The session sets it once, when it builds its `TableStates`, to a function that finds the mounted tables of that key and posts to each (9d). The kept copy is not replaced by a save that did not end `Saved`.
- `TableStates` keeps every document it loaded or saved, by key, for the session's life; `load` of a kept key resolves at once with no call. `keep` replaces a kept entry (or adds one) and cancels nothing: a debounced save of the same key still goes out with its own document. `store()` and `keep` are what Part E's Task 14b uses for the `views` part, which saves with `expected` and is not debounced.
- A change before the key's load settles is held, not saved: when the load completes (or fails, or is given up on at the table's timeout and later completes) the save writes the loaded document with only the changed parts replaced (`writeState` over the loaded `StateDocument` with `layout`, `sort`, `filters` or `view` taken from the change per `parts`). A load that failed saves the change over an empty document, and so does a load whose text `readState` refuses (not JSON, or a version above `kStateVersion`): the stored text is never merged into and never written back, so the first save after a change overwrites it with a `kStateVersion` document holding the changed parts only, and until a change nothing is written (Review Focus 3). So a sort changed while a slow load is held never overwrites the stored filters.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_state.cpp`, over an in-process server built as `tests/ui_preferences_support.hpp`'s `prefs_test::Server` is (a `RemoteServer` over a `ThreadPoolExecutor`, the `TokenAuthorizer` that makes the request's token the verified principal, and `server->handle` called directly — no socket): `registerTableState(prefs, dispatcher, registry, options)` in place of `registerPreferences`, and a client `Dispatcher` whose channel sends each `execute` through `server->handle` with token `person`. `prefs` is `ProbePreferencesStore` (new, `tests/ui_table_support.hpp`): a `MemoryPreferencesStore` wrapper that records every key it was given so a test can list what the model cannot, with `has(PrefKey)`, `value(PrefKey) -> std::string`, `erase(PrefKey)`, `pointsUnder(scope) -> std::set<std::string>` (the points of every live entry with that scope), `largestValue()`, `setCount()`, `removeCount()`, `failSetsAfter(std::size_t n)` and `failRemovesAfter(std::size_t n)` (every `set`, or `remove`, after the n-th answers `false` and changes nothing), `healSets()` and `healRemoves()`; `usageKey(person)` is the quota entry's `PrefKey` above, `pendingPoint(key)`/`segmentPoint(key, i)`/`scopeOf(key)` the layout's names:

```cpp
// Mutation: no segments (the whole document in its head, an entry over segmentBytes).
TEST_CASE("table state: state of any size", "[ui][state]") {
    std::string const big = stateWithOneOfValues(20'000);           // a 20,000-value oneOf, well over 1 MiB
    REQUIRE(save(store, key, big).outcome == SaveOutcome::Saved);
    CHECK(load(store, key)->json == big);
    CHECK(prefs.largestValue() <= 12 * 1024);                          // every entry within segmentBytes, quotes included
    prefs.failSetsAfter(prefs.setCount() + 1 + segmentsOf(big2));      // the intent and the segments are written; the head is not
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
// Mutation: the quota checked as n + b1, the replaced size not subtracted (a replace of a large state is refused).
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
// Review: a save during a held load. Mutation: the change saved over an empty document at once.
TEST_CASE("table state: a change during a held load keeps the stored parts", "[ui][state]") {
    // gated store holding {"version":1,"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}}
    states.changed(key, docWithSort("name", Descending), StateParts::Sort);  h.advance(600ms);
    CHECK(gated.saves() == 0);                                         // held until the load settles
    gated.release();  h.advance(600ms);
    REQUIRE(gated.saves() == 1);
    CHECK(gated.lastSaved().contains(R"("contains":"a")"));          // stored filter carried
    CHECK(gated.lastSaved().contains(R"("column":"name")"));
}
// Review Focus 3, the store's half. Mutation: merging the change into the stored text (the save still holds "junk").
TEST_CASE("table state: a change over a stored document that does not read replaces it", "[ui][state]") {
    for (std::string const stored : {std::string{"{not json"}, std::string{R"({"version":2,"junk":true})"}}) {
        GatedTableStateStore gated{h.owner, stored};  TableStates states{sharedOf(gated), h.owner, &h.scheduler, 500ms};
        gated.release();  REQUIRE(loaded(states, key));
        h.advance(600ms);  CHECK(gated.saves() == 0);                   // nothing is written until a change
        states.changed(key, docWithSort("name", Descending), StateParts::Sort);  h.advance(500ms);
        REQUIRE(gated.saves() == 1);
        CHECK_FALSE(gated.lastSaved().contains("junk"));
        table::StateDocument const saved = table::readState(gated.lastSaved()).value();   // the framework's version
        CHECK(saved.version == table::kStateVersion);  CHECK(saved.sort->at(0).column == "name");
    }
}
// Mutation: no call of the handler for a Refused save (`failures` stays empty).
TEST_CASE("table state: a save that is refused reaches the session's handler and keeps the kept copy", "[ui][state]") {
    GatedTableStateStore gated{h.owner, R"({"version":1})"};  TableStates states{sharedOf(gated), h.owner, &h.scheduler, 500ms};
    std::vector<table::SaveResult> failures;
    states.onSaveFailed([&](table::TableStateKey const&, table::SaveResult const& result) { failures.push_back(result); });
    gated.release();  REQUIRE(loaded(states, key));
    gated.refuseSaves("over the table-state quota");
    states.changed(key, docWithSort("name", Descending), StateParts::Sort);  h.advance(500ms);
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().outcome == table::SaveOutcome::Refused);
    CHECK(failures.front().reason == "over the table-state quota");
    CHECK(states.kept(key)->json == R"({"version":1})");                // what the store still holds
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
The fixture of this file: `h` is a `RowsFixture` (its `owner` and `scheduler`); `key` is `{application "lims", model "Lims_SampleModel", table "samples", qualifier "samples", part "state"}`; `GatedTableStateStore(executor, json)` holds each load until `release()` or `fail()`, counts `loads()` and `saves()`, keeps `lastSaved()` and `lastLoadKey()`, and after `refuseSaves(reason)` answers every save `Refused` with that reason; `sharedOf(store)` is a non-owning `std::shared_ptr` to it; `loaded(states, key)` calls `load`, settles and returns whether it delivered; `docWithSort(column, direction)` and `docWithSortOn(i)` are `StateDocument`s with that one sort key (`docWithSortOn(i)` sorts on column `c<i>`).

In `tests/test_ui_preferences.cpp`, over the existing `prefs_test::Server` (its `call(action, body, token)` and its `RecordingStore`):

```cpp
// Mutation: no reservation in `checkName` (the first call answers "ok" and the store records a `set`).
TEST_CASE("preferences: the names table state keeps are refused", "[ui][preferences][remote]") {
    prefs_test::Server server;
    std::string const scope = "morph.table.0123456789abcdef0123456789abcdef";
    CHECK(server.call("set", R"({"app":"morph.tableState","scope":"morph.table","point":"usage","value":"{\"bytes\":0}"})",
                      "alice").kind != "ok");                              // the quota's usage entry
    CHECK(server.call("set", R"({"app":"lims","scope":")" + scope + R"(","point":"state","value":"{}"})", "alice").kind != "ok");
    CHECK(server.call("remove", R"({"app":"lims","scope":")" + scope + R"(","point":"state"})", "alice").kind != "ok");
    CHECK(server.call("load", R"({"app":"lims","scope":")" + scope + R"(","points":["state"]})", "alice").kind != "ok");
    CHECK(server.store->calls().empty());                                  // nothing reached the store
    CHECK(server.call("set", R"({"app":"lims","scope":"lims.results","point":"density","value":"1"})", "alice").kind == "ok");
}
```

In `tests/net/test_push_tables.cpp`: `"table state: the person is the store's"` — two principals over one socket server save different `state` documents under one `TableStateKey` and each loads its own; a `TableStateLoad` body carrying a `"person"` member is read with that member ignored (glaze `error_on_unknown_keys = false`), so no request names another person. Mutation: the model keying by a body member.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `table_state.hpp` not found. `morph_net_tests` is POSIX-only (cross-part decision 18): build and run its case on `linux-everything` (`cmake --preset linux-everything && cmake --build --preset linux-everything --target morph_net_tests`).
- [ ] **Step 3: Implement** the interfaces above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "table state|preferences" --output-on-failure` and `morph_tests` green; `ctest --test-dir build/linux-everything -R "table state" --output-on-failure` for the `morph_net_tests` case.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Commit** `feat(ui): table state in the application's preferences store`.

#### 9d — The model-backed `table` node on the engine

**Files:**
- Modify: `include/morph/ui/view.hpp` (`TableSource`, `TableRowLook`, `CellLoad`, `TableStatus`, `TableColumn::id`, `Table::source`, `Table::status`, `Table::onRetry`), `include/morph/ui/mount.hpp` (`mountKind(Table)` reads `Table::source` when set; cells per column with their own row signal; `MountStats`, `Mounted::stats()`), `include/morph/ui/backend.hpp` (`TableWidget::setStatus`, `setOnRetry`, `setCellLoad`), `include/morph/ui/testing/recording_backend.hpp` (records `status`, cell loads; `retryTable`), `include/morph/ui/interpreter.hpp` and `src/ui/interpreter.cpp` (the lowering; `RtScope::onMissingRowField`; `ScreenCore::tableMessage`, `kStatusMessageHold`; element values), `src/ui/document.cpp` (the loader, cross-part decision 44: see "The loader" below), `tests/test_ui_document.cpp` (the loader cases), `tests/ui_table_support.hpp` (`TableHarness` and the screen helpers), `tests/test_ui_table_model.cpp`, `tests/qt_quick/test_qt_quick_table.cpp` (F6, F7, F1)
- Docs: `docs/spec/ui/interpreter.md` ("Not yet lowered" loses the model-backed `table` entry; "Lowering" describes the lowering onto `TableRows`), `docs/spec/ui/document.md` (the `table` kind: `model` with `table` or `action`, `body`, `status`, `persist`, `empty` on a model-backed table, element values `viewCount`, `viewCountKind`, `undecided`, `awaiting`, `failed`, `pending`), `docs/spec/ui/preferences.md` (table state has its own model, `morph.tableState`, not bounded by `maxValueBytes`; the preferences model refuses the `app` `morph.tableState` and every scope starting `morph.table`; the `columns` and `saved` points read and write the table's state), `docs/spec/table/engine.md` ("Not here yet": the model-backed table entry goes).

**Interfaces:**
- Consumes: 9a, 9b, 9c; Task 7's query-cache rules (the description is an entry of `Table.<id>.describe`).
- Produces:

```cpp
// view.hpp
class TableRows;
/// Whether a cell's value is here (cross-part decision 29). Read from the row's `TableRows::unloadedLevels` and the
/// column's level.
enum class CellLoad : std::uint8_t { Loaded, Unloaded, Failed };
/// A row's own parts beyond its cells, built once per realized row key, bound in the row's scope. Later parts
/// add members to this struct and do not replace it (cross-part decision 39): `std::optional<Prop<RowMarks>> marks`
/// (Task 17b) and `bool summary` (Task 22). Nothing per cell is carried here. A cell view that later tasks wrap is
/// unwrapped by the mount outermost first: `MarkedCell` (Task 19), then `AccentedCell` (Task 15), then the content.
struct TableRowLook {
    std::optional<Prop<std::optional<Tone>>> tone;     // `rowTone`; disengaged: none
    std::optional<Prop<std::string>> label;            // `rowLabel`; disengaged: the key as text
    std::optional<Prop<std::vector<RowSpan>>> spans;   // `rowSpans` (a rows table, Task 11b)
    Node heading;                                      // non-null: a heading row; no cell is built for it
};
/// Rows by view index, and cells as a mount concept (cross-part decision 28).
struct TableSource {
    std::shared_ptr<TableRows> rows;
    /// One cell's view for the column at @p column (an index into `Table::columns`, which holds hidden columns too
    /// and does not change with the layout: cross-part decision 37), built once per cell widget from
    /// a signal the mount sets to the record of the row that cell shows. A cell owns its signal: from Task 10a a
    /// pooled cell is rebound to another row by setting it, never by building the view again. What it returns is
    /// the cell's content; Task 15 wraps it as `accentedCell(content, accent)` and Task 19 as `markedCell(…, marks)`
    /// (cross-part decision 39), inside this same function.
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
// `TableOptions` (view.hpp:2026) gains the same three members (cross-part decision 41).

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
- The loader (`src/ui/document.cpp`, cross-part decision 44). Today it refuses every document this task lowers: a `table` with a `model` must name an `action` (`document.cpp:3125-3129`), `kTableKeys` (`:293`) holds none of `table`, `status`, `persist`, and a key it does not list is ignored (`:583`). This task:
  - adds to `kTableKeys` `{"table", Shape::Name}`, `{"status", Shape::Flag}` (a literal `true` or `false`; absent is the default) and `{"persist", Shape::Expr}` checked to be a literal `true`, `false` or non-empty string (`"`persist` is a literal: true, false or a name"`);
  - accepts `model` with `table` and no `action`: the alias is checked as `checkModelAction` checks it (`:2600`, "a model alias of this scope"), and, with a catalog, `Table.<table>` and `Table.<table>.describe` must both be in it and belong to the alias's model type (`"the table `<table>` is not in the catalog"`, or `checkModelAction`'s "belongs to" text);
  - refuses `table` together with `action` (`"a table names a `table` or an `action`, not both"`), and `table` without `model` (`"`table` names one of a model's tables; give the table its `model`"`);
  - reads a `filters` written as the literal `FilterSpec` object — an object whose only members are `columns` and `groups`, such as `{"columns":{"name":{"include":[{"contains":"a"}]}}}` — as that literal, checked by `glz::read_json<table::FilterSpec>` (`"`filters` is not a filter: <glaze's message>"`); any other value is an expression, as today. On a table over `rows` the key is still refused (`checkRowsTableKeys`) until Task 12;
  - keeps `model` with `action` and no `table` loading as it does today; it stays "not yet lowered" until 11c;
  - keeps `checkTablePaging` (`:3276`) as it is: `pageSize` and `page` on a model-backed table are refused at load, which is what table.md says ("Refused on a model-backed table").
  `model` is a model alias the document declares in `models`, never a type name, so every test document has a `models` block (`kModelTable`, Global Constraints).
- Before the description, and when it changes. `Table::columns` is set once (`view.hpp:1253`), and so are `TableSource::levels` and the indices `cellView` takes, while the description arrives after the mount and may take a new revision later. The lowering therefore does not return a `Table`: it returns a `ForEach` of at most one row, keyed by the held description's `revision`, whose row view is the `Table` built from that description. With no description held (before the first `describe` reply, or after one that failed) the list is empty and the node draws its status line alone, through a `Table` with no columns and no source under the key `""`, so the line's place does not jump; `empty` is not shown. A new revision is a new key: the `Table` is mounted again from the new description. What lives in the element's state and not in that row — the `WindowRows` and its window, the sort chain, the filters, the selection, the status message, the kept persisted state — survives; what is the mount's — realized rows, cells, measured widths and the scroll offset — is built again; the `WindowRows` keeps the range it was last given, so the query its new description starts asks for the rows that were visible, and the new mount then names its own. A revision change is a deployment or a change of the caller's rights, so one rebuild is the accepted cost, and it is what keeps `Table::columns` a set-once member.
- The lowering: `model` with `table` binds the model's channel as any query does, makes a document query on `Table.<table>.describe` (one per mounted table, W6) and a `WindowRows` with `restoring` set when the session has a store and `persist` is not `false`. Columns: the description's columns with `shown`, overridden per id by the node's `columns` parts; a node with `columns` lays out those; each `ui::TableColumn::id` is the column's id. `cellView` lowers the column's `cell` (or a default text cell of the record's field, as a `rows` table's is: a record holds `Value`s, not cells) in a row scope over the cell's signal; `rowView` lowers `rowTone` and `rowLabel` in a row scope over the row's signal. `alwaysFetched`: the description's key, plus the fields `rowTone`, `rowLabel` and `onActivate` name as `{"row": "<id>"}` (found when the table is lowered), plus every column when any of them, or a column's `cell`, passes the row on whole (`{"row": ""}` other than as the source of a `get`), reported once as a warning naming the node. A column's `cell` reads in its own cell, so its fields belong in `visible` only while that column is in the window; with no column window before Task 10a they are added to `alwaysFetched`, and 10a moves them. A `get` out of the whole row is the row's own read, which `onMissingRowField` names at run time. A row look carries `keep`, what its parts need alive (the row scope `onActivate` runs in), which the mount holds with the row. `levels`: each laid-out column's `ColumnInfo::level` from the description.
- `RtScope::onMissingRowField` on a server-mode row scope. Its handler is called from inside a binding's evaluation, where a signal write is refused and reported (`signal.hpp:117-124`, `kSetInComputed`), so it does no work there: it adds the field to a pending set and, when the set was empty, posts one task with `runtime.owner().post`. That task adds the pending fields to the table's extra columns and calls `rows->visible` once with them (one fetch for every miss of one turn), then clears the set. A field that is not a described column id is dropped by `WindowRows::visible`, as any id its description lacks, with no fetch and no report (it reads as absent, as any missing field does); it is never sent, since the server would refuse the query with `UnknownColumn`. A field already in the extra set is not added again. The read is `null` meanwhile, as the spec says it reads as absent; when the column arrives, the row's record gains the field and the binding runs again.
- `pageSize`/`page` on a model-backed table are refused at load (the existing check, `checkTablePaging`); `body` is the query's fixed part; header sorts and `filters` go to `setSort`/`setFilter`.
- State: key `{application = SessionOptions::application, model = <model type>, table = <table id>, qualifier = persist string or the screen's id, part = "state"}`. The load starts at mount; when it completes within `tableStateTimeout` the fitted sort and filters are applied (bound `sort` or `filters` are the document's and are neither applied nor saved; a literal `sort` is the default a stored one replaces) and `release()` is called; past the timeout `release()` is called with the defaults and a later state is applied (one more query) unless the user scrolled, selected or changed the arrangement meanwhile. A failed load is reported once through the session and changes still save. The stored layout part is carried unchanged in this task: nothing here changes a layout.
- What a dropped part is reported as. `WindowRows::setDescription` and the lowering's use of `fitState` follow one rule, 9b's: a dropped `Filter`, `GroupFilter` or `Unit` is reported once at the table's path; a dropped `Column`, `SortKey` or `Group` is not. So a sort key and a filter on a column a new revision lacks are one diagnostic, the filter's.
- A stored document that does not read (Review Focus 3). `readState` refusing the loaded text is reported once at the table's path (`"the stored state of table `<id>` is not used: <readState's message>"`), the table starts from its defaults and `release()` is called; nothing is saved until the user changes something, and that save is 9c's: a `kStateVersion` document of the changed parts.
- A save that did not end `Saved`. The session's `TableStates::onSaveFailed` handler (9c) calls `ScreenCore::tableMessage` on every mounted table whose key it names, with `"The table's arrangement could not be saved: <reason>"` (the reason alone when it is empty of one: `"The table's arrangement could not be saved"`), so under `status: false` it reaches `InterpreterHost::notify`. The table keeps the arrangement it shows; the next change saves again.
- Two things 9a leaves to this step. A failed fetch reaches `WindowRows::error()` as `TableError{Unavailable, <failure text>}`: an action refusal's `table.<code>` (`QueryTable::invoke` throws it) is not read back into `TableError::code`, so a status line that names the refusal reads it here. And `WindowRowsOptions::body`'s `query` record is overlaid by the window's own `sort`, `filters`, `page`, `columns`, `keys` and `retry`: a fixed filter in `body.query.filters` is replaced, not combined; decide here whether a node's fixed filter is combined with the header's.
- `status`: a model-backed table's `shown` is always true; the counts are `countsText(counts(), selectedCount, formatting)`; a fetch error fills `state` with `"The rows could not be loaded: <reason>"` and a failed level with `"<Level> data could not be loaded."`, both with `retry = true`, whose handler calls `WindowRows::retry`. A failed fetch changes no row, so `WindowRows::onStatus` tells the lowering of it (and of every fetch sent or answered), which bumps what the status line reads. A `body`'s `query` member is replaced by the table's own: a node's default filter is its literal `filters`, which a stored one replaces.
- Messages (cross-part decision 11). `ScreenCore::tableMessage(ElementState& table, std::string text)` is the one channel every later task posts a transient refusal or notice through (Parts E, F, G). A non-empty `text` sets `TableStatus::message` and (re)starts a `kStatusMessageHold` timer on `SessionOptions::scheduler`; when it fires and the message is still that text, the message is cleared. An empty `text` clears the message and the timer and notifies nothing; the lowering calls `tableMessage(table, "")` on each user gesture on the table this task lowers (a header click, a selection change, a scroll report, Retry), and later parts do the same for theirs. Under `status: false` a non-empty `text` is sent once to `InterpreterHost::notify(text, "warning")` (`interpreter.hpp:290`) and `TableStatus::message` stays empty. With no scheduler the message stays until the next gesture.
- `empty` on a model-backed table shows only when `total().known`, `total().rows == 0`, `kind == Exact` and `counts().undecided == 0`.

- [ ] **Step 1: Write the failing tests.** `tests/ui_table_support.hpp` gains `TableHarness` (Global Constraints) and these, each defined once and reused by every later task:

- `TableHarness()` and `TableHarness(std::shared_ptr<table::ITableStateStore>)`, which sets `SessionOptions::tableState` and `application = "lims"`. `h.host` is a `NotifyingHost` (an `InterpreterHost` that records every `notify(text, role)` in `notified`, a `std::vector<std::pair<std::string, std::string>>`; `RecordingHost` is `final`).
- What `TableHarness` offers, by name, because Parts E, F and G call it: the members `mount`, `settle`, `advance`, `reply`, `fail`, `find`, `byTestId` and `text` with the signatures the existing `Harness` has (`ui_interpreter_support.hpp:470`), `dispatcher` (with `count`, `last`, `calls`), `backend`, `host` (with `notified`), `diagnostics`, `columns`. Widgets are looked up as `h.backend.find("Table", "testId", …)`.
- The catalog's extension seam, which later tasks use for their own actions: `TableHarness::extraActions`, a `std::map<std::string, std::pair<std::string, ActionInfo>, std::less<>>` (action id → its request schema's JSON text and its `ActionInfo`), and the free function `addAction(TableHarness& h, std::string id, std::string schemaJson, ActionInfo info)`, which inserts into it. The harness's catalog is built so that its `Catalog::action` and `Catalog::schema` look in `h.extraActions` first and in `tableCatalog()` after; both the session's catalog and the one `mount` loads against are that catalog, so an action added before `mount` is known to the loader and to the session alike.
- `elementValue(screen, id, name)` is the JSON text of the element value `name` of the element `id` of a screen (`readJson(screen, {"ref":"<id>.<name>"})`), such as `elementValue(*screen, "list", "viewCount")`: the harness does not keep the screen it mounted.
- `rowKeys(TableHarness&, int table)` is not defined here: Task 13 adds it (the realized rows' keys in child order, a text key's quotes removed), and nothing in this part uses that name.
- `tableOf(h)` is `h.backend.find("Table", "testId", "list")`. A header click is `h.backend.clickHeader(tableOf(h), column)` followed by `h.settle()`; a scroll is the existing `h.backend.scrollTable(tableOf(h), first, count)`.
- `replyDescribe(h, description = samplesDescription(3))` answers the open `Table.samples.describe` call with `describeJson(table::describe(description))` and makes it `h.described`; `replyPage(h, json)` answers the oldest open `Table.samples` call (open: neither answered nor stopped); `answerPages(h, total, kind, undecided, awaiting)` answers every open `Table.samples` call with its own page, against `h.described`'s revision, until none is open; `mountShown(h, text = kModelTable, total = 9093)` mounts, replies the description and every page fetch (`answerPages`) and returns the screen. The window covers the page before and the page after the rows in view, so the page next to a held one is fetched as soon as that one arrives, and a first query after `tableSettle` asks for both at once: the cases count calls from a mark, answer every page before a check that needs none in flight (`pending`), and find the call they mean by its offset or its columns rather than by its being the latest. A page against another description than `samplesDescription(3)` is written with `pageJsonAt(revision, …)` and `keyedJsonAt(revision, …)`.
- `lastQuery(h)` is the `table::TableQuery` read from the latest `Table.samples` call's body; `statusOf(h)` the `TableStatus` the table widget was last given; `headerLabels(h)` its header captions; `diagnosticsMentioning(h, text)` the count of `h.diagnostics` whose reason contains `text`; `change(h, keys)` delivers a change of `Lims_SampleModel.samples` with those keys, and with none (`RecordChange::keys` absent: every row changed) for an empty list, which is how "the page again" is asked for; `shownText(h, text)` whether a text is mounted and no column around it is hidden, since `RecordingBackend::find` finds hidden widgets too.
- `statsOf(screen)` is `Mounted::stats()`, `coreOf(screen)` its `ScreenCore` and `elementOf(screen, id)` an element's state (`Screen` befriends the file's `ScreenProbe`).
- `RecordingBackend` gains `cellLoad(table, rowKey, column) -> CellLoad` (the last `setCellLoad`, `Loaded` when none), `retryTable(table)`, `tableStatus(table)` and `tableColumns(table)`. `samplesDescription` labels its columns as a server does (`Id`, `Name`, `C3`), and `tableCatalog()` answers no table's actions but `samples`'s. `GatedTableStateStore` is 9c's; it also holds saves after `holdSaves()` until `releaseSave()`.
- `kStored` is `{"version":1,"sort":[{"column":"name","direction":"desc"}],"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}}`; `kBoundSort` is `kModelTable` with `"state":{"order":{"init":{"lit":[{"column":"id","direction":"asc"}]}}}` and, on the table, `"sort":{"ref":"order"}`; `kNoPersist` is `kModelTable` with `"persist":false`; `kNoStatus` with `"status":false`.

In `tests/test_ui_table_model.cpp`, beside the table cases, since `tests/test_ui_document.cpp` cannot include the table support (`refusalOf(text)` loads against `tableCatalog()` and returns the refusal's reason):

```cpp
// Mutations: `table` left out of kTableKeys (the first REQUIRE: "names its `action`"); no catalog check of
// `Table.<id>` (the "nosuch" CHECK loads).
TEST_CASE("ui document: a model-backed table names a table of its model", "[ui][document]") {
    REQUIRE(loads(kModelTable, tableCatalog()));
    CHECK(loads(kNoPersist, tableCatalog()));  CHECK(loads(kNoStatus, tableCatalog()));
    CHECK(refusalOf(withTableKey(kModelTable, R"("table":"nosuch")")) == "the table `nosuch` is not in the catalog");
    CHECK(refusalOf(withTableKey(kModelTable, R"("action":"Lims_ListSamples")")) ==
          "a table names a `table` or an `action`, not both");
    CHECK(refusalOf(withTableKey(kModelTable, R"("model":"Lims_SampleModel")")) ==
          "`Lims_SampleModel` is not a model alias of this scope");          // a type is not an alias
    CHECK(refusalOf(withTableKey(kModelTable, R"("persist":{"ref":"x"})")) ==
          "`persist` is a literal: true, false or a name");
    CHECK(loads(withTableKey(kModelTable, R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"), tableCatalog()));
    CHECK(refusalOf(withTableKey(kModelTable, R"("filters":{"columns":7})")).starts_with("`filters` is not a filter"));
    CHECK(refusalOf(withTableKey(kModelTable, R"("pageSize":20)")) ==
          "a model-backed table's engine pages; `pageSize` and `page` are for a table over `rows`");
}
```
`withTableKey(text, member)` replaces the table node's member of that name, or adds it.

In `tests/test_ui_table_model.cpp`:

```cpp
// Mutation: a lowering that needs the node's `columns` (no header: `headerLabels` is empty).
TEST_CASE("model table: a client given only the catalog draws its columns, sorts and pages it", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);
    CHECK(h.dispatcher.count("Table.samples") == 0);                          // no page before its description
    replyDescribe(h);
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    replyPage(h, pageJson(0, 200, 9093, h.columns));
    CHECK(headerLabels(h) == std::vector<std::string>{"Id", "Name", "C3"});
    CHECK(h.backend.find("Text", "text", "row-0").has_value());               // the default cell, by its stand-in text
    CHECK(statusOf(h).counts == "9,093 rows");  CHECK(statusOf(h).shown);
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name"}});
    replyPage(h, pageJson(0, 200, 9093, h.columns));
    h.backend.scrollTable(tableOf(h), 4'000, 30);  h.advance(80ms);
    CHECK(lastQuery(h).page.offset == 4'000);                                 // the window pages
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);                 // W6: one request
}
// Mutation: a literal `filters` read as an expression (it evaluates to null and the query carries no filter).
TEST_CASE("model table: a literal filter and the element values reach the query and the document", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(withTableKey(kModelTable, R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"));
    replyDescribe(h);
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    REQUIRE(lastQuery(h).filters.columns.contains("name"));
    CHECK(lastQuery(h).filters.columns.at("name").include.size() == 1);
    CHECK(elementValue(h, "list", "pending") == "true");
    replyPage(h, pageJson(0, 200, 9093, h.columns, table::TotalKind::AtLeast, /*undecided*/ 40, {"recipe"}));
    CHECK(elementValue(h, "list", "viewCount") == "9093");
    CHECK(elementValue(h, "list", "viewCountKind") == R"("atLeast")");
    CHECK(elementValue(h, "list", "undecided") == "40");
    CHECK(elementValue(h, "list", "awaiting") == R"(["recipe"])");
    CHECK(elementValue(h, "list", "failed") == "[]");
    CHECK(elementValue(h, "list", "pending") == "false");
}
// Mutation: a table that ignores its store sends the default query (the count is 1 before `release`).  Gate T3.
TEST_CASE("model table: persisted state before the first query", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), kStored);
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);
    CHECK(h.dispatcher.count("Table.samples") == 0);
    CHECK(store->lastLoadKey() == table::TableStateKey{.application = "lims", .model = "Lims_SampleModel",
                                                         .table = "samples", .qualifier = "samples", .part = "state"});
    store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name", .dir = table::SortDirection::Descending}});
    CHECK(lastQuery(h).filters.columns.contains("name"));
}
// Mutations: no debounce (fifty saves); unmount not flushing (the count stays 1); a change during a save dropped.
TEST_CASE("model table: saves are coalesced, and unmounting saves at once", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();
    for (int i = 0; i < 50; ++i) { h.backend.clickHeader(tableOf(h), i % 2 == 0 ? 0 : 1);  h.advance(5ms); }
    CHECK(store->saves() == 0);                                               // inside the debounce
    h.advance(500ms);  CHECK(store->saves() == 1);
    CHECK(table::readState(store->lastSaved())->sort->at(0).column == "name");   // the last of the fifty
    store->holdSaves();
    h.backend.clickHeader(tableOf(h), 0);  h.advance(500ms);  CHECK(store->saves() == 2);   // in flight, held
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);  CHECK(store->saves() == 2);   // one per key in flight
    store->releaseSave();  h.settle();  CHECK(store->saves() == 3);           // one more after it
    store->releaseSave();  h.settle();
    h.backend.clickHeader(tableOf(h), 0);  screen.reset();  h.settle();
    CHECK(store->saves() == 4);                                               // at once, no debounce
}
// Mutations: applying a late state after the user scrolled (a second query in the second section); a failed load
// reported per table mount or not at all; a failed load that stops saving.
TEST_CASE("model table: a slow or failed load", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), kStored);
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);
    h.advance(299ms);  CHECK(h.dispatcher.count("Table.samples") == 0);
    h.advance(1ms);    REQUIRE(h.dispatcher.count("Table.samples") == 1);     // defaults after tableStateTimeout
    CHECK(lastQuery(h).sort.empty());
    replyPage(h, pageJson(0, 200, 9093, h.columns));
    SECTION("the late state is applied") {
        store->release();  h.settle();
        REQUIRE(h.dispatcher.count("Table.samples") == 2);                    // one more query
        CHECK(lastQuery(h).sort.at(0).column == "name");
    }
    SECTION("not under the user's hands") {
        h.backend.scrollTable(tableOf(h), 40, 30);  h.settle();               // within the held page: no fetch
        store->release();  h.settle();
        CHECK(h.dispatcher.count("Table.samples") == 1);
        CHECK(lastQuery(h).sort.empty());
    }
    SECTION("a failed load") {
        store->fail();  h.settle();
        CHECK(diagnosticsMentioning(h, "state") == 1);                        // reported once
        CHECK(h.dispatcher.count("Table.samples") == 1);
        h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
        CHECK(store->saves() == 1);                                           // changes still save
    }
}
// Mutations: applying a stored sort over a bound one (the query sorts by name); saving a bound sort (the saved
// document has a `sort`); `persist: false` still loading.
TEST_CASE("model table: bound values win", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), kStored);
    TableHarness h{store};
    SECTION("a bound sort") {
        auto screen = h.mount(kBoundSort);  replyDescribe(h);  store->release();  h.settle();
        REQUIRE(h.dispatcher.count("Table.samples") == 1);
        CHECK(lastQuery(h).sort == table::SortChain{{.column = "id"}});      // the document's
        CHECK(lastQuery(h).filters.columns.contains("name"));                 // the stored filter still applies
        screen.reset();  h.settle();  CHECK(store->saves() == 0);             // nothing changed, nothing saved
    }
    SECTION("persist: false") {
        auto screen = h.mount(kNoPersist);  replyDescribe(h);  h.settle();
        CHECK(store->loads() == 0);
        REQUIRE(h.dispatcher.count("Table.samples") == 1);                    // no restoring state
        h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);  screen.reset();  h.settle();
        CHECK(store->saves() == 0);
    }
}
// Review Focus 3, the table's half. Mutations: no report (the count is 0); the stored text saved back unchanged.
TEST_CASE("model table: a stored state that does not read starts from the defaults", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":2,"junk":true})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);  CHECK(lastQuery(h).sort.empty());
    CHECK(diagnosticsMentioning(h, "the stored state of table `list` is not used") == 1);
    h.advance(1'000ms);  CHECK(store->saves() == 0);                          // nothing written until a change
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
    REQUIRE(store->saves() == 1);
    CHECK_FALSE(store->lastSaved().contains("junk"));
    CHECK(table::readState(store->lastSaved())->version == table::kStateVersion);
}
// Invariant 31. Mutation: `onSaveFailed` not wired to `tableMessage` (the message stays empty).
TEST_CASE("model table: a refused save is said in the status line and keeps the arrangement", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();
    store->refuseSaves("over the table-state quota");
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
    CHECK(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
    CHECK(lastQuery(h).sort.at(0).column == "name");                          // the arrangement stays
    CHECK(h.backend.prop(tableOf(h), "sort") == "1:Ascending");
}
// Mutation: the mount leaving out the rows a `Changed` operation names (rowBinds 0). Setting every realized row's
// signal would fail nothing: a set of an equal record changes nothing and is not counted.  Gate K5.
TEST_CASE("model table: a refetch that changed 20 of 200 rows evaluates 20 rows again", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h);
    h.backend.scrollTable(tableOf(h), 0, 30);  h.settle();                    // rows 0 to 39 realized (overscan 10)
    MountStats const before = statsOf(*screen);
    change(h, std::vector<std::int64_t>{});                                   // a change without keys: the page again
    // the same page, with rows 0 to 19 renamed and rows 20 to 199 as they were
    replyPage(h, pageJsonRenaming(0, 200, 9093, h.columns, /*renamed*/ 0, 20));
    CHECK(statsOf(*screen).rowBinds - before.rowBinds == 20);                 // K5
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);
    CHECK(h.backend.find("Text", "text", "renamed-0").has_value());
}
// Review Focus 1. Mutation: setDescription not dropping keys and filters on a removed column (the query names c3).
TEST_CASE("table rows: a new description revision drops what names a removed column", "[ui][table]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(),
        R"({"version":1,"sort":[{"column":"c3","direction":"asc"}],"filters":{"columns":{"c3":{"include":[{"gt":"1"}]}}}})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();   // revision A: id, name, c3
    REQUIRE(lastQuery(h).sort.at(0).column == "c3");
    table::TableDescription const b = table::describe(samplesDescription(2));                // revision B: id, name
    replyPage(h, pageJsonAt(b.revision, 0, 200, 9093, h.columns));            // a reply names another revision
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);                 // exactly one more describe
    replyDescribe(h, samplesDescription(2));
    table::TableQuery const q = lastQuery(h);
    CHECK(std::ranges::none_of(q.sort, [](auto const& key) { return key.column == "c3"; }));
    CHECK_FALSE(q.filters.columns.contains("c3"));
    REQUIRE(q.columns.has_value());
    CHECK(std::ranges::find(*q.columns, "c3") == q.columns->end());
    CHECK(diagnosticsMentioning(h, "c3") == 1);                               // the filter; a sort key is not reported
    CHECK(headerLabels(h) == std::vector<std::string>{"Id", "Name"});         // mounted again from revision B
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);                 // once per revision
}
// Review Focus 2. Mutation: the load completion capturing the table by reference with no liveness check (a query
// is sent, or the test crashes under the sanitizer build).
TEST_CASE("model table: unmounted while its state load is held", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), kStored);
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);
    h.backend.clickHeader(tableOf(h), 0);  h.settle();   // nothing loaded yet: the change is held, not saved
    CHECK(store->saves() == 0);
    screen.reset();  h.settle();
    store->release();  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);     // nothing applied, no query
    REQUIRE(store->saves() == 1);                        // the held change, over the loaded document
    CHECK(store->lastSaved().contains(R"("contains":"a")"));   // the stored filter kept
    CHECK(table::readState(store->lastSaved())->sort->at(0).column == "id");
}
// Review Focus 4. Mutation: `empty` shown while `total().known` is false, or while rows are undecided.
TEST_CASE("model table: empty waits for the first reply and for undecided rows", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    CHECK_FALSE(h.backend.find("Text", "text", "No samples"));               // before the description
    replyDescribe(h);
    CHECK_FALSE(h.backend.find("Text", "text", "No samples"));               // before any page
    CHECK(statusOf(h).counts.empty());  CHECK(statusOf(h).shown);
    replyPage(h, pageJson(0, 0, 0, h.columns, table::TotalKind::AtLeast, /*undecided*/ 40));
    CHECK_FALSE(h.backend.find("Text", "text", "No samples"));
    CHECK(statusOf(h).counts == "0 so far, 40 loading");
    change(h, std::vector<std::int64_t>{});                                   // the level arrived: the page again
    replyPage(h, pageJson(0, 0, 0, h.columns));
    CHECK(h.backend.find("Text", "text", "No samples"));
    CHECK(statusOf(h).counts == "0 rows");
}
// Mutations: a status line built without `failed` (no state, no retry); Retry sending the query without `retry`.
TEST_CASE("model table: a failed level offers Retry, which sends `retry`", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    replyDescribe(h, withLevel(samplesDescription(3), "c3", "recipe"));
    replyPage(h, pageJsonUnloaded(0, 200, 9093, h.columns, {{.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}},
                                  /*failed*/ {"recipe"}));
    CHECK(statusOf(h).state == "Recipe data could not be loaded.");
    REQUIRE(statusOf(h).retry);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    h.backend.retryTable(tableOf(h));  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);
    CHECK(lastQuery(h).retry);
}
// Mutation: a page refetch per event (the call names no `keys`, or twenty calls go out).  Gate E3 on a screen.
TEST_CASE("model table: an arrival is one keyed request", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    std::vector<std::int64_t> keys(20);  std::ranges::iota(keys, std::int64_t{0});
    change(h, keys);
    REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);                 // exactly one more call
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(lastQuery(h).keys->size() == 20);                                   // the twenty keys; the run's probe is key 0
}
// Mutation: the mount ignoring unloadedLevels (every cell Loaded).  Cross-part decision 29.
TEST_CASE("model table: a cell of an unloaded level is a placeholder, of a failed one an error mark", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    replyDescribe(h, withLevel(samplesDescription(3), "c3", "recipe"));       // column 2 has the level
    replyPage(h, pageJsonUnloaded(0, 200, 9093, h.columns,
                                  {{.index = 0, .levels = {"recipe"}, .failed = {}},
                                   {.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}}, /*failed*/ {"recipe"}));
    int const table = tableOf(h);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 2) == CellLoad::Unloaded);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 1) == CellLoad::Loaded);      // name has no level
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{1}}, 2) == CellLoad::Failed);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{2}}, 2) == CellLoad::Loaded);
    auto const created = statsOf(*screen).cellsCreated;
    change(h, std::vector<std::int64_t>{0});                                  // the level arrives for row 0
    replyPage(h, keyedJson({{0, 0}}, 9093, h.columns));                       // a keyed reply, the level loaded
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 2) == CellLoad::Loaded);
    CHECK(statsOf(*screen).cellsCreated == created);                          // the same cell, bound again
}
// Mutations: the miss fetched inside the binding (a signal write is refused there: the debug build aborts);
// `WindowRows::visible` keeping an id the description lacks (the query names "nosuch").
TEST_CASE("model table: a row field the window lacks is fetched once, after the binding", "[ui][table]") {
    // kReadsAcross: kModelTable with "columns":[{"id":"id"},{"id":"name","cell":[{"kind":"text","text":{"get":
    //   {"row":""},"path":"c7"}},{"kind":"text","text":{"get":{"row":""},"path":"nosuch"}}]}], over
    //   samplesDescription(8): c7 is described and not laid out. Each read is its own text: a `concat` whose first
    //   argument is `null` never reads the second. A document names a row field as {"row": "<id>"},
    //   which the lowering finds; a `get` out of the whole row it does not, so the binding misses c7 at run time.
    //   The checks count the calls since the reply that name c7 (exactly one, for page 0), not every call: the
    //   page next to the first is fetched as soon as the first arrives.
    TableHarness h;  auto screen = h.mount(kReadsAcross);  replyDescribe(h, samplesDescription(8));
    std::vector<std::string> const laidOut{"id", "name"};
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.dispatcher.reply(h.dispatcher.last("Table.samples"), pageJson(0, 200, 9093, laidOut));   // no settle yet
    h.owner.runOne();                                                          // the reply is accepted, cells bind
    CHECK(h.diagnostics.empty());                                              // no write refused inside a binding
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == before + 1);                  // one fetch for every row's miss
    REQUIRE(lastQuery(h).columns.has_value());
    CHECK(std::ranges::find(*lastQuery(h).columns, "c7") != lastQuery(h).columns->end());
    CHECK(std::ranges::find(*lastQuery(h).columns, "nosuch") == lastQuery(h).columns->end());
}
// Mutations: no hold timer (the message is still there after six seconds); `status: false` still filling
// `message`; an empty text notifying.
TEST_CASE("model table: a message lasts six seconds or until the next gesture", "[ui][table]") {
    SECTION("in the status line") {
        TableHarness h;  auto screen = mountShown(h);
        detail::ScreenCore& core = coreOf(*screen);  ElementState& list = elementOf(*screen, "list");
        core.tableMessage(list, "Refused");  h.settle();  CHECK(statusOf(h).message == "Refused");
        h.advance(5'999ms);  CHECK(statusOf(h).message == "Refused");
        h.advance(1ms);      CHECK(statusOf(h).message.empty());
        core.tableMessage(list, "Again");  h.settle();
        h.backend.clickHeader(tableOf(h), 1);  h.settle();  CHECK(statusOf(h).message.empty());   // the next gesture
        core.tableMessage(list, "");  h.settle();  CHECK(h.host.notified.empty());                // notifies nothing
    }
    SECTION("status: false") {
        TableHarness h;  auto screen = mountShown(h, kNoStatus);
        coreOf(*screen).tableMessage(elementOf(*screen, "list"), "Refused");  h.settle();
        CHECK(h.host.notified == std::vector<std::pair<std::string, std::string>>{{"Refused", "warning"}});
        CHECK(statusOf(h).message.empty());  CHECK_FALSE(statusOf(h).shown);
    }
}
```
Helpers these cases add to `tests/ui_table_support.hpp`: `storeExecutor()` (a `StepExecutor` the harness's `settle()` also runs, declared before the harness so a store can be built first); `pageJsonRenaming(offset, count, total, columns, first, n)` (`pageJson` with the names of rows `first` to `first + n − 1` written `"renamed-<key>"`); `pageJsonAt(revision, …)` (9a's: `pageJson` with that `revision`); `withLevel(description, column, level)` (sets `ColumnInfo::level` and lists the level in `levels`); `pageJsonUnloaded(offset, count, total, columns, unloadedRows, failed)` (`pageJson` with `unloadedRows` and the page's `failed`); `keyedJson({{key, viewRow}…}, total, columns)` (9a's: a keyed reply with `indices`, each row's `name` `"renamed-<key>"`).

In `tests/qt_quick/test_qt_quick_table.cpp`, over the fixture's `wide` model through a scripted dispatcher (the description and pages written from `bench::WideTableSource` at 100,000 × 300, the node declaring twenty columns):

`WideScreen` is that file's fixture for it: a 1,280 × 800 offscreen window, an interpreter over a `ScriptedDispatcher`, the document `kWideTable` (a `models` block with the alias `wide` of `WideTableModel`, a `table` node naming `wide` and twenty columns `id`, then `d000`… of the `recipe` level), `shown()` (replies the description and the first page with every `recipe` cell unloaded, then renders a frame), `arrive(keys)` (a keyed change for those keys, answered with their `recipe` cells loaded, then a frame), `stats()` (`Mounted::stats()`), `realizedKeys()` and `widthChanges(column)` (how many times the table item's width for that column changed since `shown()`).

```cpp
// Mutation: the mount setting every cell of every realized row on a change (cellBinds counts 50 × 20 on any
// arrival, and 1,000 again for the second arrival of ten rows).  Gate F6.
TEST_CASE("qt_quick model table: an arrival for 50 rows re-evaluates their 1,000 cells and no other", "[qt_quick]") {
    WideScreen screen;  screen.shown();
    std::vector<std::int64_t> const keys = screen.realizedKeys();
    REQUIRE(keys.size() == 50);                                               // 30 in view, overscan 10 each side
    ui::MountStats const before = screen.stats();
    screen.arrive(keys);
    CHECK(screen.stats().cellBinds - before.cellBinds == 1'000);              // F6
    CHECK(screen.stats().rowBinds - before.rowBinds == 50);
    CHECK(screen.stats().cellsCreated == before.cellsCreated);                // nothing rebuilt
    ui::MountStats const again = screen.stats();
    screen.arrive({keys.begin(), keys.begin() + 10});                         // the same values for ten rows
    CHECK(screen.stats().cellBinds == again.cellBinds);                       // equal records: no bind
}
// Mutation: widen a column on every cell report (a column's count reaches 50).  Gate F7.
TEST_CASE("qt_quick model table: each column grows at most once on an arrival", "[qt_quick]") {
    WideScreen screen;  screen.shown();
    screen.arrive(screen.realizedKeys());
    int grown = 0;
    for (std::size_t column = 0; column < 20; ++column) {
        CHECK(screen.widthChanges(column) <= 1);  grown += screen.widthChanges(column);
    }
    CHECK(grown >= 1);                                                        // the loaded values are wider than a placeholder
}
```
`cellEvaluations` is the change in `Mounted::stats().cellBinds` across the arrival (the definition is in the Decisions above: one per cell signal set that changed its record), and `rowEvaluations(h)` in K5 the change in `rowBinds`. F1 is a `[.benchmark]` case in the same file: from the page reply to the first rendered frame, recorded.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: no `Table::source`; with the build stubbed, the loader case fails with "a `table` with a `model` names its `action`" and the model table cases fail at `mount`.
- [ ] **Step 3: Implement** the interfaces and decisions above; remove the "Not yet lowered" entry for a model-backed `table` from the interpreter.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "model table|table rows|ui document" --output-on-failure` (the existing `table` suites, `test_ui_table.cpp` and `test_ui_mount.cpp`, stay green: a `Table` without `source` mounts as before); `morph_tests` green; then `cmake --build --preset cl-qt-debug --target morph_qt_quick_tests` and `ctest --test-dir build/cl-qt-debug -L qt_quick -R "model table" --output-on-failure` green.
- [ ] **Step 5: Mutations**, as named. Record F1 (and U6's F1–F7/E1–E3 figures, measured with `morph_qt_quick_tests "[.benchmark]"` on `cl-qt-release`) in table.md's Performance tables. A host whose `cl-debug` has no Qt Quick writes F6's count gate on the recording backend in `morph_tests` ("an arrival for 50 rows re-evaluates their cells and no other") and leaves F6, F7 and F1 on Qt Quick to Task 10b, which draws the load states there.
- [ ] **Step 6: Neighbouring specs and docs build.** Update the four specs listed under Docs. Run the roadmap's docs build: green.
- [ ] **Step 7: Commit** `feat(ui): the model-backed table on a screen, with stored sort and filters`.

---

### Task 10: The column window (table.md step 5)

Two commits and an exit criterion.

#### 10a — The column window in the mount

**Files:**
- Modify: `include/morph/ui/view.hpp` (`Virtualization` additions; `Table::arrangement`, `Table::highlight`, `Table::onSelectRange`, `Table::focus`, `Table::onFocusCell`, and the same on `TableOptions`), `include/morph/ui/mount.hpp`, `include/morph/ui/backend.hpp`, `include/morph/ui/testing/recording_backend.hpp`, `src/ui/interpreter.cpp` (range selection through `keysOf`; `maxRangeRows`; the `highlightKey` lowering), `include/morph/ui/interpreter.hpp` (`SessionOptions::maxRangeRows = 10'000`)
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
/// The row a linked chart highlights: its key, and the view row the server has it at.
struct TableHighlight {
    Key key;
    std::size_t row = 0;
    [[nodiscard]] bool operator==(TableHighlight const&) const = default;
};
/// A cell by its row's key and its column's id.
struct CellRef {
    Key key;
    std::string column;
    [[nodiscard]] bool operator==(CellRef const&) const = default;
};
// Table (and TableOptions, cross-part decision 41) gains:
Prop<TableArrangement> arrangement;
/// Set by the lowering once a keyed reply has given the row's view index; the mount scrolls to `row`, marks it, and
/// follows `key` through later `ViewChange`s. Disengaged: no mark.
Prop<std::optional<TableHighlight>> highlight;
/// A range gesture a backend cannot resolve itself, as two view rows (a `TableSource` table's only).
std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange)> onSelectRange;
/// The focused cell, and its report. Task 19 re-types both from `CellRef` to `GridFocus` (cross-part decision 52),
/// so that focus can rest on a row not fetched yet; until then a focus on an unfetched view row is the mount's own.
Prop<std::optional<CellRef>> focus;
std::function<void(std::optional<CellRef>)> onFocusCell;

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
                             std::span<int const> widths, int totalWidth, std::size_t pinned);  // pinned: the leading columns shown pinned
virtual void setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler);
virtual void setHorizontalOffset(int scrollX);                       // anchoring
virtual void setOnSelectRange(std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange)> handler);
virtual void setFocusCell(std::size_t viewRow, std::size_t column);  // column: an index into Table::columns (decision 37)
/// The width a cell of @p column (an index into Table::columns) measured; the mount grows the column's extent by it.
virtual void setOnColumnMeasured(std::function<void(std::size_t column, int width)> handler);
virtual void setOnFocusMove(std::function<void(FocusMove)> handler);
virtual void setCounts(std::optional<std::int64_t> rows, std::size_t columns);  // nullopt: unknown (AtLeast)
virtual void scrollToRow(std::size_t viewRow);                         // brings a view row into view
virtual void setHighlightedRow(std::optional<std::size_t> viewRow);    // marks one row; nullopt: none
/// Places a cell of a row in a column; nothing pools it (not drawn, no place, its column blank). A backend places cells
/// by this, not by their order in the row, so a placeholder of the frame budget needs no widget.
virtual void setCellColumn(Widget& row, Widget& cell, std::optional<std::size_t> column);
enum class FocusMove : std::uint8_t { Up, Down, Left, Right, Home, End, First, Last, PageUp, PageDown };
```

Decisions:
- `windowingOf`: rows when `serverMode` or `rows >= threshold` or `rows × visibleColumns > cellThreshold`; columns when `visibleColumns > columnThreshold` or `rows × visibleColumns > cellThreshold`.
- `ColumnExtents` keeps widths in a Fenwick tree (prefix sums in O(log n), `columnsAt` by binary descent on the tree), so a width change and a lookup read O(log n) nodes; `columnReads` counts per-column width reads only.
- Pools: the pooled unit is 9d's cell — its own `reactive::Signal<Value> row`, the `Node` `cellView` built over it once, and its widget — which owns the cell's scope, so its bindings live as long as the pooled cell. Cells of the default cell of one `TextRole` share a pool keyed by role; a column with its own `cell` template has a pool keyed by its column index. A cell leaving either window is detached and pooled, its signal left as it was. One entering takes a pooled cell and is rebound by setting its signal to the record of its new row (`cellsRebound`, and one `cellBinds` when the record differs) and, for a role pool, by the mount telling it its new column; `cellView` is not called again. Only an empty pool creates a cell (`cellsCreated`).
- As built, pools are per row slot, and row slots are pooled too: the backend has no reparenting, so a cell stays in the row stack it was built in. A row that enters takes the slot (stack and cells) of one that left, its cells rebound to the new row (`cellsRebound`); within a slot, a column that leaves parks its cell (`setCellColumn(row, cell, nullopt)`) in the slot's pool of its role or column, and a column that enters takes one from there. `TableSource` gains `defaultCellView(columnSignal, rowSignal)` and `ownCell` (which columns use `cellView`), so a default cell can follow its column, and `cellReads` (each column's cell's row reads, fetched while the column is in the window). Before the backend reports a horizontal viewport every visible column is in the window, so a backend that does not window columns draws as before.
- The frame: one pass of the windowing Effect realizes or rebinds at most `realizePerFrame` cells, nearest the viewport centre first; the rest are placeholders of their column's width (`placeholders`) and are realized in later turns posted with `_rt->owner().post`, one batch of `realizePerFrame` a turn.
- Anchoring: a growth of a column left of the first fully visible unpinned column calls `setHorizontalOffset(scrollX + growth)` in the same pass. The vertical rule is the same for rows above the first visible row; the mount holds no row heights, so it is 10b's, in `MorphTable`, which measures them.
- Pinned: the first `arrangement.pinned` visible columns are always in the window; while their widths exceed half the viewport, the last pinned columns scroll with the rest (display only; `pinned` is not changed).
- Focus is `(row key, column id)`, held by the mount, reported through `Table::onFocusCell` and set from `Table::focus` (both a `CellRef` here; Task 19 re-types them, cross-part decision 52); that row and column stay realized; a focused row that leaves the view moves focus to the row now at its view index, or the last row; a move onto an unfetched index focuses the placeholder (announced as loading) and the row takes focus when it arrives; `Last` with an `AtLeast` total goes to the last row known.
- `setCounts(rows, columns)`: `rows` the view's total when `Exact`, `nullopt` under `AtLeast`; `columns` the visible column count.
- `highlightKey` (a model-backed table's only; the loader already refuses it on `rows`, `document.cpp:3306`). The document's key is `highlightKey`; what the lowering hands the mount is `Table::highlight`, because only the lowering can learn the view index of a row the window does not hold. On each change to a non-null key the lowering cancels a keyed fetch still in flight for an earlier key and sends one `TableQuery{keys = {k}, columns = [], sort, filters}` (`Table.<table>`, as `keysOf`'s fetches go); the reply's `indices[0]` (Task 4) is the row's view index `i`, and the lowering sets `highlight` to `TableHighlight{k, i}`. The mount, on a change of `highlight` to an engaged value, calls `rows->visible(max(0, i − overscan), viewportRows + 2 × overscan, columns)`, `scrollToRow(i)` and `setHighlightedRow(i)`, and keeps the key: a `ViewChange` that moves the key moves the mark, one that removes it clears it. A reply with no row (the key is filtered out or gone) sets `highlight` to nothing, which clears the mark and scrolls nothing; a `null` key does the same. It never changes the selection or the keyboard focus.
- Range selection on a `TableSource`: the backend reports two view rows through `setOnSelectRange`, which the mount wires to `Table::onSelectRange`; the lowering's handler calls `keysOf(min, count)` and, when it completes, sets the selection as the gesture's `SelectionChange` says and runs `onSelectionChange`; more than `maxRangeRows` rows is refused with the message `"A range selects at most 10,000 rows"` (number grouped by the locale) and selects nothing. `WindowRows::keysOf` fetches pages naming no columns (`columns: []`) for what it does not hold.
- A hidden column has no header, width or cells; hiding returns its realized cells to their pool; moving a column inside the window creates nothing (D8).

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_mount.cpp`. `RecordingBackend` gains the drivers `scrollTableColumns(id, scrollX, viewport)`, `focusMove(id, FocusMove)`, `selectTableRange(id, anchor, focus)`, `measureColumn(id, column, width)` (calls the `setOnColumnMeasured` handler), and records `setColumnWindow` (`windowColumns(id)`: the column indices last given; `totalWidth(id)`), `setCounts` (`lastCounts(id)`), `setFocusCell` (`focusCell(id)`: the last view row and column), `setHorizontalOffset` (`horizontalOffset(id)`: the last one, `nullopt` when never called), `scrollToRow` and `setHighlightedRow` (the props `scrolledToRow` and `highlightedRow`, `none` when cleared).

`test_ui_mount.cpp` holds `FakeRows` (`ui_table_support.hpp` pulls in the interpreter, which the mount tests do not need), a `TableRows` for mount tests: `FakeRows(rows, columns, owner, kind = Exact)` has every row present with the record `{id: i, c0: "r<i>c0", …}`, key `i`; `holdFrom(index)` makes `row()` answer nothing at and after that view index until `arrive()`, which also fires a `Reset`; `setTotal(rows, kind)`; it counts `visibleCalls` and keeps `lastVisible` (first, count, columns). `WindowFixture` (in `test_ui_mount.cpp`) mounts one `Table` over a `FakeRows` on a `RecordingBackend`: `WindowFixture f{rows, columns, {.pinned = p, .custom = {indices}}}` gives every column a fixed width of 100 and the default text cell, except the `custom` ones, which have their own `cell`; the viewport is 30 rows by 2,000 (twenty columns); `f.table` is the widget id, `f.arrangement` the `Signal<TableArrangement>` the table's `arrangement` reads, `f.focus` the signal its `focus` reads; `f.scrollRows(first)` reports rows `[first, first + 30)` and settles; `f.scrollColumns(x)` reports `(x, 2000)` and settles; `f.flingRows(n)` scrolls down over `n` rows one viewport (30 rows) at a time, running the owner one turn at a time after each step and keeping `f.maxRebindsPerTurn` (the largest growth of `cellsRebound + cellsCreated` across one turn); `f.stats()` is `Mounted::stats()`; `f.cellsAlive()` and `f.headerCellsAlive()` count the backend's live cell and header-cell widgets of the table; `f.realizedRows()` its live row widgets; `f.cellOf(key, column)` the live cell widget of that row and column, or nothing; `f.extents()` the mount's `ColumnExtents` (a test accessor on `Mounted`).

```cpp
// Mutation: `windowingOf` leaving `cellThreshold` out (the first line's `.columns` and `.rows` are false).
TEST_CASE("windowing: the three thresholds", "[ui][mount]") {
    Virtualization const v{};
    CHECK(windowingOf(v, 1999, 300, false).rows);   CHECK(windowingOf(v, 1999, 300, false).columns);
    CHECK_FALSE(windowingOf(v, 1999, 10, false).rows);   CHECK_FALSE(windowingOf(v, 1999, 10, false).columns);
    CHECK(windowingOf(v, 2000, 10, false).rows);    CHECK_FALSE(windowingOf(v, 2000, 10, false).columns);
    CHECK(windowingOf(v, 100, 41, false).columns);  CHECK_FALSE(windowingOf(v, 100, 41, false).rows);
    CHECK(windowingOf(v, 100, 40, false).columns == false);
    CHECK(windowingOf(v, 10, 10, true).rows);       CHECK_FALSE(windowingOf(v, 10, 10, true).columns);
}
// Mutation: the column window never on (`windowColumns` is all 300). Ignoring `cellThreshold` alone cannot fail
// this case: 300 visible columns are over `columnThreshold` already.  Gate F2.
TEST_CASE("table window: 1,999 × 300 realizes only the windows", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    f.scrollRows(500);  f.scrollColumns(10'000);                              // columns 100 to 119 in view
    CHECK(f.realizedRows() == 50);                                            // 30 and overscan 10 each side
    CHECK(f.backend.windowColumns(f.table).size() == 2 + 20 + 2 * 4);
    CHECK(f.cellsAlive() == f.realizedRows() * f.backend.windowColumns(f.table).size());
    CHECK(f.headerCellsAlive() == f.backend.windowColumns(f.table).size());
    CHECK(f.backend.totalWidth(f.table) == 300 * 100);
    CHECK(f.rows->lastVisible.columns.size() == 30);                          // the window's columns go to `visible`
}
// Mutations: a leaving row's slot not reused (cellsCreated grows on the second fling); no frame budget
// (maxRebindsPerTurn is 720: thirty rows entering, twenty-four cells each).  Gate F8.
TEST_CASE("table window: a fling creates no cell once the pools are warm", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 2}};
    f.flingRows(2'000);  auto const created = f.stats().cellsCreated;
    f.flingRows(2'000);
    CHECK(f.stats().cellsCreated == created);
    CHECK(f.maxRebindsPerTurn <= 240);
    CHECK(f.stats().placeholders > 0);                                        // the budget left cells blank for a turn
}
// Mutation: rebinding a pooled cell by calling cellView again (cellViewCalls grows on the second fling).
TEST_CASE("table window: a pooled cell is rebound by its row signal", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 2, .custom = {5}}};
    f.flingRows(2'000);  auto const views = f.cellViewCalls;  auto const rebound = f.stats().cellsRebound;
    f.flingRows(2'000);
    CHECK(f.cellViewCalls == views);  CHECK(f.stats().cellsRebound > rebound);
    f.scrollRows(40'000);  f.settle();
    CHECK(f.backend.prop(*f.cellOf(Key{std::int64_t{40'000}}, 5), "text") == "r40000c5");   // rebound to its new row
}
// Mutation: no anchoring (`horizontalOffset` is never set).
TEST_CASE("table window: a column growing left of the view keeps the columns in view where they are", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 0}};
    f.scrollColumns(10'000);                                                  // column 100 is the first in view
    f.backend.measureColumn(f.table, 98, 160);  f.settle();                   // an overscan column grows by 60
    CHECK(f.backend.horizontalOffset(f.table) == 10'060);
    CHECK(f.extents().offsetOf(100) == 10'060);
    f.backend.measureColumn(f.table, 98, 120);  f.settle();                   // narrower: a width only grows
    CHECK(f.extents().widthOf(98) == 160);  CHECK(f.backend.horizontalOffset(f.table) == 10'060);
    f.backend.measureColumn(f.table, 110, 180);  f.settle();                  // inside the view: no anchoring
    CHECK(f.backend.horizontalOffset(f.table) == 10'060);
}
// Mutations: pinned columns dropped from the window when scrolled away; no display unpinning (`pinnedShown` stays
// 12). The mount cannot change the fixture's signal, so "unpinning by changing `pinned`" cannot fail the case.
TEST_CASE("table window: pinned columns stay realized, and a narrow table unpins for display only", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    f.scrollColumns(20'000);
    std::vector<std::size_t> const window = f.backend.windowColumns(f.table);
    CHECK(window.at(0) == 0);  CHECK(window.at(1) == 1);                      // still the first two
    CHECK(f.cellOf(Key{std::int64_t{0}}, 1).has_value());
    TableArrangement wide = f.arrangement.peek();  wide.pinned = 12;          // 1,200 of a 2,000 viewport: over half
    f.arrangement.set(wide);  f.settle();
    CHECK(f.backend.pinnedShown(f.table) == 10);                              // the last two scroll with the rest
    CHECK(f.arrangement.peek().pinned == 12);                                 // display only
}
// Mutation: a pass that reads every column's width (columnReads grows by 300 a pass).  Gate F9.
TEST_CASE("table window: a pass reads no width outside the column window", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    f.scrollColumns(10'000);
    std::size_t const before = f.extents().columnReads();
    for (int x = 10'000; x < 12'000; x += 100) { f.scrollColumns(x); }        // twenty passes, one column each
    std::size_t const window = 2 + 20 + 2 * 4;
    CHECK(f.extents().columnReads() - before <= 20 * window);                 // the window's columns, per pass
}
// Mutations: rebuild rows on a layout change (cellsCreated grows); a hide that pools nothing (the destroyed cell
// makes the next pass throw).  Gate D8.
TEST_CASE("table window: hiding and moving a column create no cell widgets", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    auto const before = f.stats().cellsCreated;  std::size_t const alive = f.cellsAlive();
    TableArrangement hidden = f.arrangement.peek();
    hidden.visible = allColumnsBut(300, 5);                                   // column 5 was in the window
    f.arrangement.set(hidden);  f.settle();
    CHECK(f.stats().cellsCreated == before);
    CHECK(f.cellsAlive() == alive);                                           // column 26 entered with the pooled cells
    CHECK_FALSE(f.cellOf(Key{std::int64_t{0}}, 5).has_value());
    CHECK(f.headerCellsAlive() == f.backend.windowColumns(f.table).size());
    TableArrangement moved = hidden;  std::swap(moved.visible.at(3), moved.visible.at(4));
    f.arrangement.set(moved);  f.settle();
    CHECK(f.stats().cellsCreated == before);
    CHECK(f.backend.prop(*f.cellOf(Key{std::int64_t{0}}, 3), "text") == "r0c3");   // every cell still its own
    CHECK(f.rows->lastVisible.first == 0);                                    // rows and their order untouched
}
// Mutation: the focused row not kept realized (the focused cell is gone after the scroll).  Gate F10.
TEST_CASE("table focus: the focused cell survives a scroll of 10,000 rows and 200 columns", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 2}};
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c5"});  f.settle();
    f.scrollRows(10'000);  f.scrollColumns(20'000);
    REQUIRE(f.cellOf(Key{std::int64_t{7}}, 5).has_value());                   // its row and its column stay realized
    CHECK(f.backend.focusCell(f.table) == std::pair{std::size_t{7}, std::size_t{5}});
    CHECK(f.realizedRows() == 51);                                            // one row beyond the window at most
    CHECK(f.backend.lastCounts(f.table) == std::pair{std::optional<std::int64_t>{100'000}, std::size_t{300}});
}
// Mutation: `Last` clamped to the rows held (focus stays on row 199).
TEST_CASE("table focus: Ctrl+End over rows not fetched lands on the last row when it arrives", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 0}};
    std::vector<std::optional<CellRef>> reported;  f.onFocusCell = [&](auto cell) { reported.push_back(cell); };
    f.focus.set(CellRef{.key = Key{std::int64_t{0}}, .column = "c0"});  f.settle();
    f.rows->holdFrom(200);
    f.backend.focusMove(f.table, FocusMove::Last);  f.settle();
    CHECK(f.backend.focusCell(f.table).first == 99'999);                      // the placeholder holds focus
    CHECK(f.rows->lastVisible.first + f.rows->lastVisible.count >= 100'000);  // and is asked for
    f.rows->arrive();  f.settle();
    REQUIRE_FALSE(reported.empty());
    CHECK(reported.back() == CellRef{.key = Key{std::int64_t{99'999}}, .column = "c0"});
}
// Mutation: the realized row count reported (50), or a lower bound reported as exact.
TEST_CASE("table focus: the reported row count is the total, or unknown for a lower bound", "[ui][mount]") {
    WindowFixture f{9'093, 300, {.pinned = 0}};
    CHECK(f.backend.lastCounts(f.table) == std::pair{std::optional<std::int64_t>{9'093}, std::size_t{300}});
    f.rows->setTotal(100'000, table::TotalKind::AtLeast);  f.settle();
    CHECK(f.backend.lastCounts(f.table) == std::pair{std::optional<std::int64_t>{}, std::size_t{300}});
    TableArrangement fewer = f.arrangement.peek();  fewer.visible = allColumnsBut(300, 5);
    f.arrangement.set(fewer);  f.settle();
    CHECK(f.backend.lastCounts(f.table).second == 299);                       // the visible columns
    f.rows->holdFrom(200);  f.focus.set(CellRef{.key = Key{std::int64_t{0}}, .column = "c0"});  f.settle();
    f.backend.focusMove(f.table, FocusMove::Last);  f.settle();
    CHECK(f.backend.focusCell(f.table).first == 199);                         // AtLeast: the last row known
}
```
`allColumnsBut(n, i)` is the indices `0 … n − 1` without `i`; `RecordingBackend::pinnedShown(id)` is how many leading columns of the last `setColumnWindow` the mount marked pinned (the call's first `pinned` entries; the driver reads the count the mount passes in a recorded prop `pinnedShown`).

In `tests/test_ui_table_model.cpp`, over `TableHarness`. `selectTable()` is `kModelTable` with `"selectionMode":"multiple"` (a function: `withTableKey` uses Catch macros); `kHighlight` is `kModelTable` with `"state":{"hk":{"init":{"lit":null}}}`, `"highlightKey":{"ref":"hk"}` on the table and three buttons (test ids `to5012`, `to77`, `toNone`) whose `onClick` sets `hk` to `5012`, to `77`, and to `null`; `click(h, testId)` clicks one through the recording backend and settles; `replyKeyPages(h)` answers every unanswered `Table.samples` call with a page of keys alone at the offset and limit its query names (`pageJson(offset, limit, total, {})`), settling after each; `replyKeyed(h, {{key, viewRow}…})` answers the oldest open call whose query names `keys` with `keyedJson` (the page fetch the highlight causes is older and open); `stopped(h, index)` is whether a call was stopped.

```cpp
// Mutation: `keysOf` fetching the visible columns (a call's body lacks `"columns":[]`).
TEST_CASE("model table: a range over rows not held resolves through pages that name no columns", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h, selectTable(), 100'000);      // rows 0 to 199 held
    std::size_t const sent = h.dispatcher.calls.size();
    h.backend.selectTableRange(tableOf(h), 500, 1'499);  h.settle();
    REQUIRE(h.dispatcher.calls.size() > sent);
    for (std::size_t call = sent; call < h.dispatcher.calls.size(); ++call) {
        CHECK(h.dispatcher.calls.at(call).action == "Table.samples");
        CHECK(h.dispatcher.calls.at(call).body.contains(R"("columns":[])"));
    }
    replyKeyPages(h);
    CHECK(elementValue(h, "list", "selectedCount") == "1000");
    CHECK(statusOf(h).counts == "1,000 of 100,000 selected");
}
// Mutation: no bound (keysOf is called and the calls go out).
TEST_CASE("model table: a range over maxRangeRows is refused and selects nothing", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h, selectTable(), 100'000);
    std::size_t const sent = h.dispatcher.calls.size();
    h.backend.selectTableRange(tableOf(h), 0, 10'000);  h.settle();            // 10,001 rows
    CHECK(h.dispatcher.calls.size() == sent);
    CHECK(statusOf(h).message == "A range selects at most 10,000 rows");
    CHECK(elementValue(h, "list", "selectedCount") == "0");
    h.backend.selectTableRange(tableOf(h), 0, 9'999);  h.settle();             // 10,000 rows: allowed
    CHECK(h.dispatcher.calls.size() > sent);
}
// Mutations: the view index taken from the reply's position (0) instead of `indices`; the highlight written into
// the selection; no cancel of the earlier key's fetch.
TEST_CASE("model table: highlightKey scrolls to the row and marks it", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h, kHighlight, 9'093);
    int const table = tableOf(h);
    click(h, "to5012");
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(*lastQuery(h).keys == std::vector<table::RowId>{std::int64_t{5012}});
    CHECK(h.dispatcher.calls.back().body.contains(R"("columns":[])"));
    replyKeyed(h, {{5012, 5011}});
    CHECK(h.backend.prop(table, "scrolledToRow") == "5011");
    CHECK(h.backend.prop(table, "highlightedRow") == "5011");
    h.advance(80ms);
    CHECK(lastQuery(h).page.offset == 5'000);                                 // the page that holds view row 5,011
    CHECK(elementValue(h, "list", "selectedCount") == "0");                   // never the selection
    click(h, "to77");
    replyKeyed(h, {});                                                        // filtered out or gone: no row
    CHECK(h.backend.prop(table, "highlightedRow") == "none");
    CHECK(h.backend.prop(table, "scrolledToRow") == "5011");                  // nothing scrolled
    std::size_t const first = h.dispatcher.calls.size();
    click(h, "to5012");  click(h, "to77");                                    // two keys before a reply
    CHECK(stopped(h, first));                                                 // the first key's fetch is cancelled
    click(h, "toNone");  CHECK(h.backend.prop(table, "highlightedRow") == "none");
}
```

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

**Interfaces:** consumes 10a's `TableWidget` members; produces `TableWidget::setRowIndex(row, viewRow)`, which the mount calls for a realized row once it is realized and whenever its index changes: the focused row stays realized outside `setRealizedWindow`'s range, so a backend places each row by its index, not by its place among the children. Every backend places a source table's cells by `setCellColumn`, never by their order in the row.

Decisions:
- MorphTable exposes, for tests, the QML properties `realizedColumnFirst`, `realizedColumnCount`, `pinnedShown`, `headerCellCount`, `statusText`, `rowMeasures` (cell height measurements made), the invokables `rowHeight(viewRow)` and `cellLoadAt(viewRow, column)` (`0` Loaded, `1` Unloaded, `2` Failed, as drawn), and keeps `rowScans`, `realizedFirst`, `realizedCount`, `totalRows`.
- The status line is one `Text` row under the rows, an accessible polite live region (`Accessible.role: Accessible.StaticText`, announced on change), with a "Retry" button while `TableStatus::retry`; it takes no space while `shown` is false.
- Both backends mark a table as over a source when the mount gives it a horizontal-range handler, never by `setRealizedWindow`, which a long `rows` table is told too. Both answer `tableMeasure` (pixels from the theme, with `table.cellPaddingX` a side as `TableMeasure::padding`; cells, with the gap as padding), so a windowed fixed column is as wide as an unwindowed one. Over a source, Shift with an arrow, a Shift-click and Ctrl+A go to `setOnSelectRange` as view rows (Ctrl+A over every row of the view, as `Extend`, which the mount refuses past `maxRangeRows` until the whole-view selection of "Acting on rows" exists); no range runs over the realized rows alone. A click does not move the mount's focused cell in either backend: no seam carries a pointer's cell. Qt Quick's counts reach `accessibleRowCount`/`accessibleColumnCount` only, and a row does not report its view row to assistive technology: `Accessible` has neither. A source table's natural height is all its rows in both backends, so it needs a bounded height to window its rows.
- The terminal table draws the cells of the columns in the window only, and the status line as its last line. Over a source it scrolls over every row of the view, one line each, draws each realized row on the line of its view index, and reports the columns and then the rows in view before the next frame's layout (and at once for the wheel); the arrows, Home, End, PageUp, PageDown, Ctrl+Home and Ctrl+End go to the mount, and Left and Right, keeping the focused cell in view, are how the user scrolls sideways.
- Cell load state (from 9d's `setCellLoad`). Qt Quick: an `Unloaded` cell draws a rounded bar of a fixed 3 em width in the column's alignment over the cell's content, which stays bound but invisible; a `Failed` cell draws an error mark of the same width whose tooltip and accessible name say "Could not be loaded". The terminal draws `···` for `Unloaded` and `!` for `Failed`, in the column's alignment. Neither is measured into the column's width, so a level arriving widens a column at most once (F7).
- Row height (table.md "Cells"), in `MorphTable`, which is what measures cells: a row's height is the tallest of its realized cells and only grows while the row is realized; a row that leaves the row window forgets its height; an unrealized row counts as the mean of the measured heights. When a row above the first visible row grows, the vertical offset grows by the same amount in the same frame; the mount holds no row heights, so this anchoring is `MorphTable`'s, here. A table whose columns are all `wrap: false` has one row height and measures no cell (`rowMeasures` stays 0). The terminal's rows are one line each, so none grows and the terminal needs no anchoring.

- [ ] **Step 1: Write the failing tests**

`QuickWindowFixture` (in `test_qt_quick_table.cpp`) is 10a's `WindowFixture` on the Qt Quick backend: the same constructor, `rows`, `arrangement`, `focus`, `scrollRows`, `scrollColumns`, `flingRows`, `stats()` and `settle()` (which also renders a frame), in an offscreen 1,280 × 800 window with a column width of 64, so twenty columns are in view; `item()` is the `MorphTable` item, `prop(name)` one of its QML properties, `cellItems()` the visible cell items under it, `call(name, args…)` one of its invokables. `WideScreen` is 9d's. `TuiWindowFixture` (in `test_tui_table.cpp`) is the same over the terminal backend through `tests/tui/tui_harness.hpp`, in a terminal of 120 × 32 with a column width of 12 (ten columns and thirty rows in view); `cellWidgetsAlive()` counts the table's live cell widgets and `screenLine(n)` is line `n` of the drawn screen.

```cpp
// Mutation: MorphTable instantiating a cell per column of a realized row (cellItems is 50 × 300).  Gate F2.
TEST_CASE("qt_quick table window: cells and header cells alive are the windows'", "[qt_quick]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    f.scrollRows(500);  f.scrollColumns(6'400);                               // columns 100 to 119 in view
    int const window = 2 + 20 + 2 * 4;
    CHECK(f.prop("realizedColumnCount").toInt() == window);
    CHECK(f.prop("pinnedShown").toInt() == 2);
    CHECK(f.cellItems() == f.prop("realizedCount").toInt() * window);
    CHECK(f.prop("headerCellCount").toInt() == window);
}
// Mutation: a pool keyed by column for default cells (a column scrolling in creates 50 cells).  Gate F8.
TEST_CASE("qt_quick table window: flings create no cell once the pools are warm", "[qt_quick]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    f.flingRows(2'000);  for (int x = 0; x < 12'800; x += 640) { f.scrollColumns(x); }   // warm both ways
    auto const created = f.stats().cellsCreated;
    f.flingRows(2'000);  for (int x = 12'800; x < 19'200; x += 640) { f.scrollColumns(x); }
    CHECK(f.stats().cellsCreated == created);
    CHECK(f.maxRebindsPerTurn <= 240);
}
// Mutation: MorphTable keeping focus on the delegate item, which a recycled row loses.  Gate F10.
TEST_CASE("qt_quick table window: focus and counts after scrolling far away", "[qt_quick]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c5"});  f.settle();
    f.scrollRows(10'000);  f.scrollColumns(200 * 64);
    CHECK(f.call("focusedRow").toInt() == 7);  CHECK(f.call("focusedColumn").toInt() == 5);
    CHECK(f.call("cellRealized", 7, 5).toBool());
    CHECK(f.prop("accessibleRowCount").toInt() == 100'000);
    CHECK(f.prop("accessibleColumnCount").toInt() == 300);
}
// Mutation: MorphTable rebuilding its row delegates when the column window changes.  Gate D8, Qt Quick half.
TEST_CASE("qt_quick table window: hiding and moving a column create no cell items", "[qt_quick]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    auto const created = f.stats().cellsCreated;  int const items = f.cellItems();
    TableArrangement hidden = f.arrangement.peek();  hidden.visible = allColumnsBut(300, 5);
    f.arrangement.set(hidden);  f.settle();
    CHECK(f.stats().cellsCreated == created);  CHECK(f.cellItems() == items);
    CHECK_FALSE(f.call("cellRealized", 0, 5).toBool());
    TableArrangement moved = hidden;  std::swap(moved.visible.at(3), moved.visible.at(4));
    f.arrangement.set(moved);  f.settle();
    CHECK(f.stats().cellsCreated == created);  CHECK(f.cellItems() == items);
}
// Mutation: MorphTable ignoring setStatus (statusText is empty).
TEST_CASE("qt_quick table: the status line shows counts and Retry", "[qt_quick]") {
    QuickWindowFixture f{100'000, 30};                                        // its status: "100,000 rows"
    CHECK(f.prop("statusText").toString() == QStringLiteral("100,000 rows"));
    CHECK_FALSE(f.prop("retryShown").toBool());
    f.status.set(ui::TableStatus{.counts = "100,000 rows", .state = "Recipe data could not be loaded.",
                                 .retry = true, .shown = true});  f.settle();
    CHECK(f.prop("statusText").toString() == QStringLiteral("100,000 rows  Recipe data could not be loaded."));
    REQUIRE(f.prop("retryShown").toBool());
    f.scene.click(*f.item()->findChild<QQuickItem*>(QStringLiteral("retry")));
    CHECK(f.retries == 1);
}
// Mutation: MorphTable ignoring setCellLoad (an unloaded cell draws its empty content: cellLoadAt is 0).
TEST_CASE("qt_quick table: an unloaded cell is a placeholder and a failed one an error mark", "[qt_quick]") {
    QuickWindowFixture f{100, 4, {.levels = {{.column = 1, .level = "recipe"}}}};
    int const widthBefore = f.call("columnWidth", 1).toInt();
    f.rows->unload(0, "recipe");  f.rows->fail(1, "recipe");  f.settle();
    CHECK(f.call("cellLoadAt", 0, 1).toInt() == 1);
    CHECK(f.call("cellLoadAt", 1, 1).toInt() == 2);
    CHECK(f.call("cellLoadAt", 2, 1).toInt() == 0);
    CHECK(f.call("columnWidth", 1).toInt() == widthBefore);                   // a placeholder widens nothing
}
// Mutations: a row's height recomputed from its realized cells each pass (it shrinks when the tall cell leaves);
// measuring a table whose columns are all wrap:false (rowMeasures grows).
TEST_CASE("qt_quick table: a row's height only grows while it is realized", "[qt_quick]") {
    QuickWindowFixture f{2'000, 300, {.pinned = 0, .wrapped = {{.row = 3, .column = 60, .lines = 2}}}};
    int const low = f.call("rowHeight", 3).toInt();
    f.scrollColumns(60 * 64);  int const tall = f.call("rowHeight", 3).toInt();
    CHECK(tall > low);
    f.scrollColumns(0);  CHECK(f.call("rowHeight", 3).toInt() == tall);       // scrolled out: not shorter
    f.scrollRows(1'000);  f.scrollRows(0);
    CHECK(f.call("rowHeight", 3).toInt() == low);                             // it left the row window: forgotten
    QuickWindowFixture flat{2'000, 300, {.pinned = 0, .wrap = false}};
    flat.flingRows(200);  CHECK(flat.prop("rowMeasures").toInt() == 0);
}
// tests/tui/test_tui_table.cpp.
// Mutation: the terminal table ignoring the column range (300 cells a row).  Gate U7.
TEST_CASE("tui table: cell widgets alive are rows in view times columns in view", "[tui]") {
    TuiWindowFixture f{100'000, 300, {.pinned = 0}};
    f.scrollRows(5'000);  f.scrollColumns(100 * 12);
    CHECK(f.cellWidgetsAlive() == 30 * 10);                                   // the terminal keeps no overscan
    CHECK(f.screenLine(31).starts_with("100,000 rows"));                      // the status line, last
}
// Mutation: the terminal table ignoring setCellLoad (the lines show neither mark).
TEST_CASE("tui table: an unloaded cell draws its placeholder and a failed one its mark", "[tui]") {
    TuiWindowFixture f{100, 4, {.pinned = 0, .levels = {{.column = 1, .level = "recipe"}}}};
    f.rows->unload(0, "recipe");  f.rows->fail(1, "recipe");  f.settle();
    CHECK(f.screenLine(1).contains("···"));  CHECK(f.screenLine(2).contains("!"));
    CHECK_FALSE(f.screenLine(3).contains("···"));
}
```
`MorphTable` therefore also exposes, for tests, the properties `retryShown`, `accessibleRowCount`, `accessibleColumnCount` and the invokables `focusedRow()`, `focusedColumn()`, `cellRealized(viewRow, column)`, `columnWidth(column)`; `FakeRows` gains `unload(row, level)` and `fail(row, level)` (what `unloadedLevels` answers, with a `Changed` for the row); the status line and load state cases run on `QuickWindowFixture` (its `status` signal and `onRetry` count, and `FakeRows::unload`/`fail`) rather than on 9d's `WideScreen`, which 9d did not write: a fixture over a scripted interpreter cannot be written blind, and these cases test the backend's drawing, not the lowering; the fixtures' options gain `wrapped`, `wrap` and `levels`. The terminal's window has no overscan: its `Virtualization` is built with `overscan = 0` and `columnOverscan = 0` by `TuiWindowFixture`, which is what makes U7's count exact.

Benchmarks, `[.benchmark][qt_quick]`, in `tests/qt_quick/bench_qt_quick_table.cpp` (Part A's, where the step 0 baselines of the same figures are): F3 (vertical at 20 rows/s, a fling at 2,000 rows/s, a thumb drag), F4 (horizontal at 10 and 100 columns/s), F5 (memory per realized row), each first asserting the table is windowed both ways, timed per step through `QQuickRenderControl` (`polishItems`, `sync`, `render`) under `offscreen` and the software scene graph.

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
inline constexpr std::string_view kGroupRunColumn = "\x1F" "group";   // U+001F, then "group": no document column can have this id
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
    /// Cells of @p column, a column in use, that are not of its kind; 0 for a column not in use.
    [[nodiscard]] std::size_t invalidCells(std::string_view column) const;
};
```

Decisions:
- A cell of a column in use is a `reactive::Computed` over the row's signal and what `sortBy` reads; when one changes, the source notifies `Updated` for that row alone; a change every row's cell reads derives the column again and notifies `Reset`.
- `assign`: a row whose `Value` shares storage with the held one is kept unread; another object derives the in-use cells and compares them with the held cells, `Updated` only on a difference; new keys `Inserted`, missing keys `Removed`. A later duplicate of a key is dropped and reported through `site::kDuplicateKey`, once per duplicate, as the mount reports it today.
- Kind inference, by `ui::ValueKind` (`value.hpp:132`). A `Value` has no floating-point kind: a fractional number that reaches a document through the query cache is the `Decimal` its shortest text spells, or its text where that does not fit one (cross-part decision 46), so a `rows` column of fractional numbers infers as `Decimal` and no `rows` column is ever inferred `Number`; `Number` comes only from a column part's literal `kind: "number"`, under which a `Decimal` or `Int` cell is converted to the nearest `double`. A column with no non-null cell (an empty list, or all `null`) is `Text` until a cell votes, and the first vote that makes it another kind is a `Reset` of that column like any other change of kind. Each non-null cell votes; the column's kind and each cell:

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

  All votes alike → that kind; only `Int` and `Decimal` → `Decimal`; any other mix takes the first non-null cell's kind, `Decimal` when that is an `Int` and a `Decimal` voted too (table.md's "integers with decimals are one `Decimal` column and a text among them is one invalid cell"), and a cell not of it is invalid (sorts last, reported once at the `sort` path). A literal `kind` on the column part wins over inference, and a cell not of it is invalid. A change of the inferred kind is a `Reset` of that column. A `DateTime` cell holds seconds (`data_source.hpp:56`), so two timestamps in one second tie and keep their source order, where the expression language's `sort` orders to the millisecond; table.md ("A document's rows as a source", "Timestamps to the second") states this. A rows table orders as a model's `DateTime` column does, which is what "One order on every surface" compares.
- Groups. `kGroupRunColumn` begins with the control character U+001F, written as the escape `"\x1F" "group"` and never as the raw character: a column id is a document name (`[A-Za-z_][A-Za-z0-9_]*`, `document.cpp:770`), so no column a document declares, `group` included, can collide with it. With `setGroup`, the source holds the column `kGroupRunColumn` (`ColumnKind::Integer`, not in `ColumnInfo`s the lowering lays out): row `r`'s cell is `runOf(r)`, runs counted over the rows in source order, a new run starting where `by` is not the same element as the run's first row's: `ui::detail::sameElement` with the unit table `setGroup` is given, the rule of the interpreter's `group` (`Evaluator::sameRun`) and of `runs`, so `1`, `1.0` and `1.00` are one run, and the run's key is its first row's `by`. The lowering puts `{kGroupRunColumn, Ascending}` before every sort chain it passes to the engine, so groups keep their places and rows sort within them; `assign` recomputes the runs and notifies `Reset` of that column only when a run boundary moved. `runRecords()` are the heading records the interpreter builds today (`TableView::runs`), placed by the lowering after paging (11b).
- The source keeps each row's `Value` beside its cells; `recordOf` returns its key and that value.

- [ ] **Step 1: Write the failing tests** in `tests/test_ui_table_model.cpp`. Helpers, in the file: `rec({{"id", 1}, {"name", "a"}})` is a record `Value`; `sourceOf(h, columns, units = {})` a `std::shared_ptr<ValueRowsSource>` on `h.runtime` whose `keyOf` reads `id`; `col(id)` a `ValueColumn` with that id, `col(id, kind)` one with a literal kind; `RecordingListener listener{*src}` subscribes and keeps every `RowChange` in `changes` (`clear()` empties it); `freshDecodeOf(list)` is the list written to JSON and read back (no storage shared); `withField(list, row, name, value)` a copy with one row's field replaced, the other rows sharing storage; `cellAt(src, row, column)` is `src.snapshot()->cell(row, <index of column>)`; `kindOf(src, column)` its `ColumnInfo::kind`; `engineOver(h, src, collator = {})` a `table::Engine` with `EngineOptions{.owner = &h.owner, .services.collator = collator}`; `keysInView(engine)` the `int64` keys of its view rows in order; `sampleRows(n)` is `n` rows `{id: i, name: "n<i>", notes: "x", amount: i}`.

```cpp
// Mutation: a diff by row value (the fresh decode notifies, or `cellsDerived` counts every field).
TEST_CASE("value rows source: a document's rows as a source", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("notes"), col("amount")});
    RecordingListener listener{*src};
    ValueList const rows = sampleRows(100);
    src->use("name");  src->assign(rows);  h.settle();  listener.clear();
    std::size_t const derived = src->cellsDerived();
    src->assign(freshDecodeOf(rows));  h.settle();
    CHECK(listener.changes.empty());                                          // the same cells: nothing notified
    CHECK(src->cellsDerived() - derived == 100);                              // one column in use, not three fields
    src->assign(withField(freshDecodeOf(rows), 7, "name", Value{std::string{"zeta"}}));  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{7});
    CHECK(src->cellsHeld() == 100);                                           // "notes" and "amount" hold no cells
    src->use("amount");  h.settle();  CHECK(src->cellsHeld() == 200);
    src->unuse("amount");  CHECK(src->cellsHeld() == 100);                    // its last use ended: its cells go
}
// Mutations: `sortBy` read outside a tracked computation (no change is notified); a per-row change notified as Reset.
TEST_CASE("value rows source: a change to what one row's sortBy reads updates that row", "[ui][table]") {
    RowsFixture h;
    std::vector<std::unique_ptr<reactive::Signal<std::int64_t>>> rank;
    for (int i = 0; i < 10; ++i) { rank.push_back(std::make_unique<reactive::Signal<std::int64_t>>(h.runtime, i)); }
    reactive::Signal<std::int64_t> scale{h.runtime, 1};
    ValueColumn ranked = col("name");
    ranked.sortBy = [&](Value const& row) { return Value{rank.at(*row.field("id")->asInt())->get() * scale.get()}; };
    auto const src = sourceOf(h, {ranked});  RecordingListener listener{*src};
    src->use("name");  src->assign(sampleRows(10));  h.settle();  listener.clear();
    rank.at(3)->set(99);  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{3});
    CHECK(cellAt(*src, 3, "name") == table::Cell{std::int64_t{99}});
    listener.clear();  scale.set(2);  h.settle();                             // what every row's cell reads
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Reset);
}
// Review Focus 5. Mutation: keep the last duplicate (the view holds "c" and not "a").
TEST_CASE("value rows source: a duplicate key keeps its first row", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name")});
    std::size_t const reports = h.reported(ui::detail::site::kDuplicateKey);
    src->use("name");
    src->assign(ValueList{rec({{"id", 1}, {"name", "a"}}), rec({{"id", 2}, {"name", "b"}}), rec({{"id", 1}, {"name", "c"}})});
    h.settle();
    CHECK(src->snapshot()->rowCount() == 2);
    CHECK(h.reported(ui::detail::site::kDuplicateKey) - reports == 1);        // once per duplicate
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1});            // "b", then the first "a"; never "c"
    CHECK(cellAt(*src, 0, "name") == table::Cell{std::string{"a"}});
}
// Mutation: Timestamp cells kept in milliseconds (row 2 reads 1'700'000'000'500 and sorts apart from row 1).
TEST_CASE("value rows source: a timestamp is a DateTime of whole seconds", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("at")});  src->use("at");
    src->assign(ValueList{rec({{"id", 1}, {"at", ts(1'700'000'000'000)}}), rec({{"id", 2}, {"at", ts(1'700'000'000'500)}}),
                          rec({{"id", 3}, {"at", ts(-1)}})});
    h.settle();
    CHECK(kindOf(*src, "at") == table::ColumnKind::DateTime);
    CHECK(cellAt(*src, 0, "at") == table::Cell{std::int64_t{1'700'000'000}});
    CHECK(cellAt(*src, 1, "at") == table::Cell{std::int64_t{1'700'000'000}});   // the same second: they tie
    CHECK(cellAt(*src, 2, "at") == table::Cell{std::int64_t{-1}});              // floor, not truncation toward 0
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "at", .dir = table::SortDirection::Descending}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{1, 2, 3});         // the tie keeps source order
}
// Mutations: a missing case in the inference (a kind differs); an empty column given no kind (the "e" entry);
// a mixed Int and Decimal column inferred from its first cell (Integer, with one invalid cell).
TEST_CASE("value rows source: every value kind infers its column kind", "[ui][table]") {
    using enum table::ColumnKind;
    RowsFixture h;
    auto const src = sourceOf(h, {col("b"), col("i"), col("d"), col("s"), col("t"), col("q"), col("l"), col("id_d"),
                                  col("e"), col("n", Number)}, testUnits());  // testUnits(): kg and g of mass
    for (auto const* id : {"b", "i", "d", "s", "t", "q", "l", "id_d", "e", "n"}) { src->use(id); }
    src->assign(ValueList{
        rec({{"id", 1}, {"b", true}, {"i", 4}, {"d", dec("2.50")}, {"s", "x"}, {"t", ts(1'000)}, {"q", qty("3", "kg")},
             {"l", 7}, {"id_d", 4}, {"e", Value{}}, {"n", dec("0.5")}}),
        rec({{"id", 2}, {"b", false}, {"i", 5}, {"d", dec("0.125")}, {"s", "y"}, {"t", ts(2'000)}, {"q", qty("3", "zz")},
             {"l", ValueList{}}, {"id_d", dec("0.5")}, {"e", Value{}}, {"n", 2}})});
    h.settle();
    std::map<std::string, table::ColumnKind> kinds;
    for (table::ColumnInfo const& column : src->columns()) { kinds.emplace(column.id, column.kind); }
    CHECK(kinds == std::map<std::string, table::ColumnKind>{{"b", Bool}, {"i", Integer}, {"d", Decimal}, {"s", Text},
        {"t", DateTime}, {"q", Measure}, {"l", Integer}, {"id_d", Decimal}, {"e", Text}, {"n", Number}});
    CHECK(src->invalidCells("q") == 1);                                       // the second row's unit "zz" is unknown
    CHECK(src->invalidCells("l") == 1);                                       // the list cell
    CHECK(src->invalidCells("id_d") == 0);                                    // an integer is an exact decimal
    CHECK(cellAt(*src, 0, "n") == table::Cell{0.5});  CHECK(cellAt(*src, 1, "n") == table::Cell{2.0});
    RecordingListener listener{*src};
    src->assign(ValueList{rec({{"id", 1}, {"e", 9}})});  h.settle();          // the empty column's first vote
    CHECK(kindOf(*src, "e") == Integer);
    CHECK(listener.changes.back().kind == table::ChangeKind::Reset);
}
// Mutations: every row given run 0, as an index computed after the sort would leave it (groups split: 2, 3, 1);
// the run column named plain "group" (the source has two columns of that id, and the count CHECK reads 2).
TEST_CASE("value rows source: a grouped table sorts within its runs", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("group")});   // a document column named "group"
    src->setGroup([](Value const& row) { return *row.field("site"); });
    src->use("name");  src->use("group");
    src->assign(ValueList{rec({{"id", 1}, {"site", "A"}, {"name", "z"}, {"group", "q"}}),
                          rec({{"id", 2}, {"site", "A"}, {"name", "a"}, {"group", "r"}}),
                          rec({{"id", 3}, {"site", "B"}, {"name", "m"}, {"group", "p"}})});
    h.settle();
    CHECK(std::ranges::count(src->columns(), std::string{"group"}, &table::ColumnInfo::id) == 1);
    CHECK(std::ranges::count(src->columns(), std::string{kGroupRunColumn}, &table::ColumnInfo::id) == 1);
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = std::string{kGroupRunColumn}}, {.column = "name"}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1, 3});
    REQUIRE(engine->setSort({{.column = std::string{kGroupRunColumn}}, {.column = "group"}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{1, 2, 3});         // by the document's "group", in runs
    CHECK(src->runRecords().size() == 2);  CHECK(src->runOf(2) == 1);
}
// Mutation: fold inside the comparison (about 3.4 million folds).  Gate U3.
TEST_CASE("value rows source: a sort of 100,000 rows folds each text once", "[ui][table]") {
    RowsFixture h;  auto const collator = std::make_shared<ValueRowsCollator>();
    auto const src = sourceOf(h, {col("name")});  src->use("name");  src->assign(sampleRows(100'000));  h.settle();
    auto engine = engineOver(h, src, collator);
    REQUIRE(engine->setSort({{.column = "name"}}));  h.settle();
    CHECK(collator->folds() == 100'000);
    CHECK(engine->stats().fullSorts == 1);
}
// Mutations: a fresh decode deriving every column (cellsDerived is 300,000); the changed row sorting the list
// (fullSorts grows).  Gates U2 and U4, as counts.
TEST_CASE("value rows source: one changed row derives one column and repairs", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("notes"), col("amount")});
    ValueList const rows = sampleRows(10'000);
    src->use("name");  src->assign(rows);  h.settle();
    auto engine = engineOver(h, src);  REQUIRE(engine->setSort({{.column = "name"}}));  h.settle();
    CHECK(src->cellsHeld() == 10'000);                                        // U4: one column's cells
    table::EngineStats const before = engine->stats();  std::size_t const derived = src->cellsDerived();
    src->assign(withField(freshDecodeOf(rows), 42, "name", Value{std::string{"zzz"}}));  h.settle();
    CHECK(src->cellsDerived() - derived == 10'000);                           // U2: the column in use only
    CHECK(engine->stats().repairs - before.repairs == 1);
    CHECK(engine->stats().fullSorts == before.fullSorts);
    CHECK(keysInView(*engine).back() == 42);
}
```
`ts(ms)`, `dec(text)` and `qty(amount, unit)` build a `Timestamp`, a `Decimal` and a `Quantity` value; `h.reported(site)` counts the runtime's reports of that site (`RowsFixture` installs a report handler on its runtime). Benchmarks `[.benchmark][interpreter]` in `bench_interpreter.cpp` are timings only: U1 (a header click on a text column of 100,000 rows), U2 (one row changed, shared and fresh decode) and U4 (memory of 10,000 × 300 with one column sorted); their count gates are the ordinary case above, not assertions inside a hidden benchmark.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `ValueRowsSource` undeclared.
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "value rows source" --output-on-failure`.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Commit** `feat(ui): a document's rows as a table source`.

#### 11b — The interpreter's tables on the engine

**Files:**
- Modify: `src/ui/document.cpp` (the loader, cross-part decision 44: a column part's `kind`, and the `persist`-without-`id` refusal), `tests/test_ui_document.cpp` (their cases), `include/morph/ui/interpreter.hpp` (`TableView` stays as the shape element values read; `ElementState::tableRows` (`std::shared_ptr<TableRows>`) replaces `sortBound`'s use for ordering), `src/ui/interpreter.cpp` (`sortedRows` and the ordering half of `tableView` removed; the lowering of a `rows` table onto `ValueRowsSource` → `Engine` → `EngineRows`; `pageSize`/`page` as a slice of the engine's view; `group` as a hidden leading sort key `ValueRowsSource` computes, headings placed after paging; `rowSpans` on the page's rows, drawn only unsorted and below the windowing thresholds; select-all through `table::Selection::selectAll`)
- Test: `tests/test_ui_table_model.cpp`, `tests/test_ui_table.cpp`, `tests/bench_interpreter.cpp` (B1)
- Docs: `docs/spec/ui/interpreter.md` ("Lowering", **The rows' pipeline** and **Groups** describe the engine's order and what the lowering does around it), `docs/spec/ui/document.md` (a column's `kind`; the client-mode bound warning), `docs/spec/table/engine.md` ("Not here yet": the single-threaded steps entry).

**Interfaces:**
- Consumes: 11a; `table::Engine` with `EngineOptions{owner = &runtime.owner(), worker = nullptr, frameBudget = 4 ms, services.collator = ValueRowsCollator}`.
- Produces: `void EngineRows::setRestoring(bool restoring)` (while `true`, `total().known` is `false` and `row()` answers nothing, so the mount draws placeholder rows; the engine still runs); otherwise nothing new in the API, and every existing table element value keeps its name and shape.

Decisions:
- A plain list over 20,000 rows or 1,000,000 cells (rows × the node's columns) is reported once as a warning naming the table: `"table <id> holds <n> rows; a table over rows is ordered on the client up to 20,000 rows or 1,000,000 cells — serve it as a model's table"`.
- The loader (`src/ui/document.cpp`). `kColumnKeys` (`:322`) gains `{"kind", Shape::Text}`, a literal among `text`, `integer`, `decimal`, `number`, `date`, `dateTime`, `bool` (`"`kind` is one of text, integer, decimal, number, date, dateTime, bool"`). A table over `rows` with no `id` gets no state key: a `persist` it writes that is not `false` is refused when the document loads (`"a table over `rows` needs an `id` to persist its arrangement"`); with no `persist` key such a table simply persists nothing. `status` and `persist` are accepted on a `rows` table as 9d added them.
- The status line of a `rows` table (table.md "The status line"): `TableStatus::shown` is true while `state` or `message` is not empty and false otherwise, so the line takes no space on a table with nothing to say; `status: true` shows it always (with its counts), `status: false` never, a message then going to `InterpreterHost::notify` as in 9d. Its counts are `countsText(counts(), selectedCount, formatting)` whenever it shows.
- What the screen shows of a row comes from `EngineRows`' record of it, which is read again only for a row a change names. `ValueRowsSource` (11a) notifies `Updated` for a row whose value changed even when no column in use reads the change, and for the first source row of a run whose heading record changed while the runs kept their places; the engine repairs such a row in place. The lowering re-reads `runRecords()` on every `onChange`, and calls `setGroup` again, with the screen's unit table, when what `by` reads besides the row changes, since the source reads `by` untracked. Test: a refetch that changes only a displayed field redraws that row's cell (mutation: `EngineRows` keeping its record of an `Updated` row), and a heading's `count` follows a row that joins its run. A rows table's mount realizes every row below `Virtualization::threshold` (`TableSource::alwaysWindowed = false`), as table.md's "When a table is windowed" has it, while a model-backed table is always windowed: the "one order" case scrolls the model table over its 200 rows before comparing.
- An invalid cell in a column of the sort chain is reported once per table at the node's `sort` path, as the interpreter reports a sort value of a mixed kind today: the lowering reads `ValueRowsSource::invalidCells(column)` for each key after a view change and reports when it first is not 0.
- The mount: a `rows` table lowers onto `Table::source` as a model-backed one does (9d's `TableSource`), `cellView` over each cell's signal, `rowView` giving `rowTone`, `rowLabel`, `rowSpans` and, for a group heading's key, `heading` (the lowered `heading` content), so the two kinds share one mount path.
- Persisted state for a `rows` table (table.md "Persisted state": "a client-mode table draws placeholder rows"). With a session store, an `id` and `persist` not `false`, the key is `{application = SessionOptions::application, model = "" (a table over a document's rows has none; 9c's bounds allow it), table = the node's id, qualifier = the persist string or the screen's id, part = "state"}`. The lowering calls `setRestoring(true)` before the first `assign`, starts the load through `Session::tableStates()`, and on completion within `tableStateTimeout` applies the fitted sort and filters (bound ones are the document's, as in 9d) and calls `setRestoring(false)`: the first rows drawn are in the stored order. Past the timeout it calls `setRestoring(false)` with the defaults, and a later state is applied unless the user touched the table, as in 9d. Sort and filter changes go to `TableStates::changed` with the same debounce and unmount flush as 9d's; the description passed to `fitState` is built from the node's columns and the inferred kinds (`TableDescription{id, columns, key}` with each `TableColumn{info, label, sortable, filterable}`).

- [ ] **Step 1: Write the failing tests.** `tests/ui_table_support.hpp` gains `rowsDoc(itemsLiteral, tableExtra = "")`: a `ui/1` document whose state `items` is initialised to that literal list and whose view is a column of a table `list` (test id `list`) over `{"ref":"items"}`, `"key":{"row":"id"}`, columns `id`, `name` and `weight`, each `sortable`, with `tableExtra` appended to the table node, and a button `rename` whose `onClick` sets `items` to the same list with the row of id `"b"` renamed `"zz"`, or the first row when no row has that id, so the list always changes; `rowsDocNoId` is the same with no `id` on the table; `rowOrder(h)` is the keys of the table's realized rows in child order, joined by spaces; `engineOf(screen, id)` the `table::Engine` of that table's `EngineRows` (through `ScreenProbe`); `kThree` is `[{"id":"a","name":"a"},{"id":"b","name":"b"},{"id":"c","name":"c"}]`; `mixedRows()` is 200 rows whose names mix accents and case (`"Émile"`, `"emil"`, `"Zoë"`, `"zoe"`, …), with decimal weights and every tenth name and weight `null`, as a JSON list; `kRowsSamples` is `kModelTable`; `pagesOf(rows, query)` answers a `TableQuery` over those rows through `table::apply` with `ValueRowsCollator` as the served collator, as a generic page.

In `tests/test_ui_table_model.cpp` (not `test_ui_document.cpp`: `ui_table_support.hpp` there clashes with the Windows `Concurrency` namespace that file names; `loads` and `refusalOf` move into `ui_table_support.hpp`):

```cpp
// Mutations: `kind` not in kColumnKeys (the bad kind loads); no `persist` check on a rows table without an id.
TEST_CASE("ui document: a rows table's column kind and its persist need what they name", "[ui][document]") {
    CHECK(loads(rowsDoc(kThree, R"(,"persist":"mine")"), tableCatalog()));
    CHECK(refusalOf(withColumnKey(rowsDoc(kThree), "name", R"("kind":"money")")) ==
          "`kind` is one of text, integer, decimal, number, date, dateTime, bool");
    CHECK(loads(withColumnKey(rowsDoc(kThree), "name", R"("kind":"text")"), tableCatalog()));
    CHECK(refusalOf(rowsDocNoId(kThree, R"(,"persist":true)")) ==
          "a table over `rows` needs an `id` to persist its arrangement");
    CHECK(loads(rowsDocNoId(kThree, R"(,"persist":false)"), tableCatalog()));
    CHECK(loads(rowsDocNoId(kThree), tableCatalog()));
}
```

In `tests/test_ui_table_model.cpp`:

```cpp
// Mutation: the interpreter's pipeline left in place for `rows` tables (the accented names order apart).
TEST_CASE("one order on every surface: a rows table and a model table order alike", "[ui][table]") {
    std::string const rows = mixedRows();
    for (std::vector<std::size_t> const& clicks : {std::vector<std::size_t>{1},          // by name
                                                   std::vector<std::size_t>{1, 1},       // by name, descending
                                                   std::vector<std::size_t>{2}}) {       // by weight
        TableHarness asRows;  auto rowsScreen = asRows.mount(rowsDoc(rows));
        TableHarness asModel;  auto modelScreen = asModel.mount(kRowsSamples);
        replyDescribe(asModel, describedFrom(rows));
        replyPage(asModel, pagesOf(rows, lastQuery(asModel)));
        for (std::size_t column : clicks) {
            asRows.backend.clickHeader(tableOf(asRows), column);  asRows.settle();
            asModel.backend.clickHeader(tableOf(asModel), column);  asModel.settle();
            replyPage(asModel, pagesOf(rows, lastQuery(asModel)));
        }
        asModel.backend.scrollTable(tableOf(asModel), 0, 200);  asModel.settle();  // a model table is windowed
        CHECK(rowOrder(asRows) == rowOrder(asModel));
        CHECK_FALSE(rowOrder(asRows).empty());
    }
}
// Mutation: sorting the whole list on a row change (fullSorts grows).  Gate U2 counts, on a screen.
TEST_CASE("rows table: one changed row is one repair", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(kThree));
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    REQUIRE(rowOrder(h) == "a b c");
    table::EngineStats const before = engineOf(*screen, "list").stats();
    h.backend.click(h.byTestId("Button", "rename"));  h.settle();
    CHECK(engineOf(*screen, "list").stats().repairs - before.repairs == 1);
    CHECK(engineOf(*screen, "list").stats().fullSorts == before.fullSorts);
    CHECK(rowOrder(h) == "a c b");                                            // "zz" moved last
}
// Mutations: no bound check (no warning); the bound read as `>=` (a warning at 20,000). A warning per change of the
// list is not observable here: the screen reports a repeated path and reason once.
TEST_CASE("rows table: a list over 20,000 rows is reported once", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(idRows(20'001)));
    std::string const warning = "table list holds 20,001 rows; a table over rows is ordered on the client up to "
                                "20,000 rows or 1,000,000 cells — serve it as a model's table";
    CHECK(diagnosticsMentioning(h, warning) == 1);
    h.backend.click(h.byTestId("Button", "rename"));  h.settle();             // the list changes: no second report
    CHECK(diagnosticsMentioning(h, warning) == 1);
    TableHarness under;  auto small = under.mount(rowsDoc(idRows(20'000)));
    CHECK(diagnosticsMentioning(under, "serve it as a model's table") == 0);  // at the bound: nothing
}
// Mutation: drawing the rows before the load settles (the first CHECK reads "a b c").
TEST_CASE("rows table: persisted state before the first rows", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(),
                                                        R"({"version":1,"sort":[{"column":"name","direction":"desc"}]})");
    TableHarness h{store};  auto screen = h.mount(rowsDoc(kThree));
    CHECK(rowOrder(h).empty());                                               // placeholders only while restoring
    CHECK(store->lastLoadKey() == table::TableStateKey{.application = "lims", .model = "", .table = "list",
                                                         .qualifier = "samples", .part = "state"});
    store->release();  h.settle();
    CHECK(rowOrder(h) == "c b a");                                            // first drawn: the stored order
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);  CHECK(store->saves() == 1);
}
// Mutation: a rows table without `id` given a key (a load goes out with table "").
TEST_CASE("rows table: a table without an id persists nothing", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};  auto screen = h.mount(rowsDocNoId(kThree));
    CHECK(rowOrder(h) == "a b c");                                            // no restoring state
    h.backend.clickHeader(h.backend.all("Table").front(), 1);  h.advance(500ms);
    CHECK(store->loads() == 0);  CHECK(store->saves() == 0);
}
// Mutations: `shown` always true for a rows table; a message under `status: false` kept in the line.
TEST_CASE("rows table: the status line takes space only while it has something to say", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(kThree));
    CHECK_FALSE(statusOf(h).shown);
    coreOf(*screen).tableMessage(elementOf(*screen, "list"), "Refused");  h.settle();
    CHECK(statusOf(h).shown);  CHECK(statusOf(h).message == "Refused");  CHECK(statusOf(h).counts == "3 rows");
    h.advance(6'000ms);  CHECK_FALSE(statusOf(h).shown);
    TableHarness always;  auto shown = always.mount(rowsDoc(kThree, R"(,"status":true)"));
    CHECK(statusOf(always).shown);  CHECK(statusOf(always).counts == "3 rows");
}
// Mutation: no report of an invalid sort cell (the count is 0). One report per view change is not observable here:
// the screen reports a repeated path and reason once.
TEST_CASE("rows table: a cell not of its column's kind is reported once at the sort path", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(rowsDoc(R"([{"id":"a","weight":1},{"id":"b","weight":"heavy"},{"id":"c","weight":2}])"));
    auto const sortReports = [&h] {
        return std::ranges::count_if(h.diagnostics, [](Refusal const& r) { return r.path.ends_with("/sort"); });
    };
    h.backend.clickHeader(tableOf(h), 2);  h.settle();
    CHECK(rowOrder(h) == "a c b");                                            // the invalid cell sorts last
    CHECK(sortReports() == 1);
    h.backend.clickHeader(tableOf(h), 2);  h.settle();
    CHECK(rowOrder(h) == "c a b");  CHECK(sortReports() == 1);                // last in either direction, once
}
// Invariant 17: the existing `rows` table suites (test_ui_table.cpp, test_ui_interpreter_table.cpp,
// test_ui_table_row_span.cpp) stay green with no expectation changed.
```
`idRows(n)` is `n` rows `{"id": i, "name": "n<i>"}` as a JSON list; `describedFrom(rows)` the `TableDescription` of `id` (key), `name` and `weight` with the kinds 11a infers from those rows; `withColumnKey(text, column, member)` adds a member to that column part.
The collection half of "One order on every surface" is Task 13's (step 8): this case covers the `rows` and model-backed surfaces and Task 13 adds the collection. B1 at 20,000 rows is a `[.benchmark]` case in `bench_interpreter.cpp` reading `EngineStats::maxOwnerStepMicros` after a three-key sort and a text filter, asserting first that the engine ran in more than one owner step.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `EngineRows::setRestoring` undeclared; with it stubbed, `ctest --test-dir build/cl-debug -R "one order on every surface|rows table" --output-on-failure` → FAIL: orders differ (the interpreter's `Evaluator::sortRows` is still in use) and no load is made.
- [ ] **Step 3: Implement** as above; delete `ScreenCore::sortedRows` and the ordering part of `tableView`.
- [ ] **Step 4: Run to verify they pass** — the new cases and the whole `morph_tests` target green, then `morph_qt_quick_tests` and `morph_tui_tests` on `cl-qt-debug` green (their table cases mount through the interpreter).
- [ ] **Step 5: Mutations**, as named. Record U1 to U4 and B1 (10,000, 20,000 and 100,000 rows) from `cl-release` into table.md's Performance tables.
- [ ] **Step 6: Neighbouring specs** as listed; docs build green.
- [ ] **Step 7: Commit** `feat(ui): tables over rows on the engine`.

#### 11c — A typed list action as a model-backed table

table.md "The document `table` node on the engine": a node with `model` and `action` names a typed list action; "its description is derived from the action's schema, and the reader turns each row object into cells; it returns whole rows whatever `columns` says".

**What the code gives, and the position taken.** The client's catalog holds an action's **request** schema only (`Catalog::schema`, `document.hpp`: `forms::schemaJson<A>()`, as `RemoteServer`'s `schemas` envelope serves it, `registry.hpp:657`); no reply schema reaches a client. The reply's row type reaches one only through a `v-*` view document (`Catalog::view`, whose `v-columns` `views.hpp:476` derives from the query's row type). So the description is built from, in order: the node's `columns` parts (ids required), else `Catalog::view(action)`'s `v-columns` (ids and labels) and `v-rowKey`; the key from the node's `key`, else `v-rowKey`, else `"id"`; each column's kind from the node part's literal `kind`, else the `kind` its `v-columns` entry carries (a view document writes one where the row type gives it, such as `"kind":"integer"`), else inferred from the first reply's rows by the JSON-level table below and then fixed for the window's life (a later cell not of it is invalid). The request schema must have one top-level member marked `x-table` (`TableQuery::schemaMarker()`, `query.hpp:77`): the query is written into that member of `body`. A node whose action has no such member, or which has neither `columns` nor a view document, is refused when the document loads (`"a table over an action needs a query member marked x-table"`, `"a table over an action needs its columns, or a view document for the action"`). table.md ("The client table model") states this same order and both refusals, because a catalog carries no reply schema.

**Files:**
- Modify: `include/morph/ui/table_rows.hpp` (`WindowRowsOptions::{action, queryMember}`; `readTypedPage`), `src/ui/interpreter.cpp` (the `model` + `action` lowering), `src/ui/document.cpp` (the two refusals)
- Test: `tests/test_ui_table_model.cpp`
- Docs: `docs/spec/ui/document.md` (the `table` kind with `model` and `action`), `docs/spec/ui/interpreter.md` ("Lowering").

**Interfaces:**

```cpp
// WindowRowsOptions gains
std::string action;        // non-empty: a typed list action's id, sent in place of "Table.<table>"; `table` empty
std::string queryMember;   // with `action`: the body member the query is written into (the x-table member)
std::vector<std::string> inferKinds;  // with `action`: the columns the first reply with rows gives a kind
std::vector<std::string> records;     // replaces `record`: every record watched (an action's catalog records; none
                                      // for an action without one)
/// A typed list action's reply (`Page<Row>`'s JSON: `rows`, `total`, `totalKind`, `offset`, `indices`) as a generic
/// page over @p description's columns: each row object's member of a column's id read by its kind.
[[nodiscard]] std::expected<table::GenericPage, table::TableError> readTypedPage(std::string_view json,
                                                                                table::TableDescription const& description,
                                                                                table::UnitRegistry const* units = nullptr);
// document.hpp: the members a request schema marks `x-table`, which the loader and the lowering both read
[[nodiscard]] std::vector<std::string> tableQueryMembers(std::string_view schemaJson);
```

Decisions:
- Kinds of a typed reply. 11a's table reads `Value`s, which hold no JSON number with a fraction, so it cannot be used on a reply's text. A typed reply is inferred at the JSON level, each non-null member of a column's id voting:

  | JSON member | Votes for | Cell |
  |---|---|---|
  | `null`, absent | nothing | empty |
  | `true`, `false` | `Bool` | `bool` |
  | an integer | `Integer` | `int64`; in a `Decimal` column a `Rational` of 0 places, in a `Number` column a `double` |
  | a number with a fraction or an exponent | `Number` | `double` |
  | `{num, den, dp}` | `Decimal` | the `Rational`, places kept |
  | `{num, den, dp, unit}` | `Measure` | `Measure` through `units->find`, the units being the view document's (each `v-columns` entry's `ExtUnits` and `x-unitAlternatives`); a unit no one lists is its own canonical unit in a dimension of its own, added to the description, so the cell is valid |
  | a string | `Text` | the text |
  | an array, any other object | nothing of its own | its JSON text, invalid outside `Text` |

  All votes alike → that kind; only integers and `Decimal`s → `Decimal`; integers with fractional numbers, with or without `Decimal`s → `Number`; any other mix takes the first non-null member's kind and a member not of it is invalid; no vote → `Text`. This is where a typed table and a `rows` table differ, as cross-part decision 46 has it: a fractional number in a reply's text is a `double` and its column `Number`, while the same number reaching a document's `rows` through the query cache is already a `Decimal`. A string is never inferred as `Date` or `DateTime`: those need a literal `kind` or the view document's.
- Two passes over the first reply. `WindowRows` with `action` and at least one column of no known kind holds the first reply's text until it has parsed it once as a `glz::generic_u64` DOM, run the table above over the rows for those columns, and completed its description (`setDescription`); it then reads the same DOM with `readTypedPage`. Later replies are read in one pass against the fixed description. A first reply with no rows fixes nothing: the next reply that has rows is the one inferred from.
- `readTypedPage` reads the text with a `glz::generic_u64` DOM (as `buildActionDescription` does, `registry.hpp:688`, so int64 bounds survive): a member absent or `null` → empty; an integer → `int64` (a `double` in a `Number` column); a number with a fraction → `double`; a string → text, or under `Date` `YYYY-MM-DD` → days, under `DateTime` ISO-8601 → floor seconds; `true`/`false` → `bool`; `{num, den, dp}` → `Rational`; with `unit` under `Measure` → `Measure` through `units->find` (a window lists an unknown unit first, as its own canonical unit, so it is found; `readTypedPage` alone, given a registry without it, leaves it invalid); anything else → the member's JSON text, invalid outside `Text`. The key is the row's member named by the description's `key`, read as a `RowId`. `columns` is every description column, `revision` empty (so `onStaleDescription` never fires).
- `WindowRows` with `action` sends `session.read(options.channel, action, body with body[queryMember] = the query, token)` and decodes the text with `readTypedPage`; the reply holds whole rows, so `visible`'s columns name what the record converts (K8 holds) but not what is fetched. Keyed refetches and `highlightKey` send `keys` in the query member; an action whose reply has no `indices` for a keyed query invalidates the window (`PageWindow::patch`'s rule), which costs a page refetch and not a wrong row.
- The record watched is each `ActionInfo::records` entry of the action (`document.hpp:56`); with none, the window refetches only on `invalidate` (reconnect).

- [ ] **Step 1: Write the failing tests.** `kActionTable` is `{"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table","id":"list","testId":"list","model":"lab","action":"Lims_PageSamples","key":{"row":"id"},"columns":[{"id":"id"},{"id":"name"},{"id":"weight"},{"id":"ratio"},{"id":"day","kind":"date"}]}}`; the test adds `Lims_PageSamples` to the harness with `addAction(h, "Lims_PageSamples", R"({"type":"object","properties":{"query":{"type":"object","x-table":true},"site":{"type":"string"}}})", ActionInfo{.modelType = "Lims_SampleModel"})`; `kActionTableNoQuery` names `Lims_ListSamples`, whose schema (the corpus catalog's) has no `x-table` member; `kActionTableNoColumns` is `kActionTable` without `columns` (and `Lims_PageSamples` has no view document); `kindOfColumn(screen, column)` reads the window's description through `ScreenProbe`; `cellOf(screen, row, column)` the window's `cell(row, column)`.

```cpp
// Mutations: `{num, den, dp}` read as text (weight is Integer, from 3 alone, and not a Rational); a fractional number voted
// Decimal (ratio is Decimal); the query written at the body's top level (no "query" member in the body).
TEST_CASE("model table over an action: a typed list action's rows read into the window", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);                     // no describe call: none exists
    CHECK(h.dispatcher.calls.back().body.contains(R"("query":{)"));          // the query in the x-table member
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","weight":{"num":5,"den":2,"dp":1},"ratio":0.5,"day":"2026-11-17"},
                        {"id":2,"name":"b","weight":3,"ratio":2,"day":null}],"total":2})");
    using enum table::ColumnKind;
    CHECK(kindOfColumn(*screen, "id") == Integer);    CHECK(kindOfColumn(*screen, "name") == Text);
    CHECK(kindOfColumn(*screen, "weight") == Decimal);                        // {num,den,dp} with an integer
    CHECK(kindOfColumn(*screen, "ratio") == Number);                          // a fraction with an integer
    CHECK(kindOfColumn(*screen, "day") == Date);                              // the literal kind
    CHECK(*cellOf(*screen, 0, "ratio") == table::Cell{0.5});  CHECK(*cellOf(*screen, 1, "ratio") == table::Cell{2.0});
    CHECK(std::holds_alternative<math::Rational>(*cellOf(*screen, 1, "weight")));
    CHECK(*cellOf(*screen, 0, "day") == table::Cell{std::int64_t{20'774}});   // days since 1970-01-01
    CHECK(std::holds_alternative<std::monostate>(*cellOf(*screen, 1, "day")));
    CHECK(statusOf(h).counts == "2 rows");
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    CHECK(h.dispatcher.calls.back().body.contains(R"("sort":[{"column":"name")"));
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":3,"name":"c","weight":"heavy","ratio":1}],"total":1})");
    CHECK(kindOfColumn(*screen, "weight") == Decimal);                        // fixed by the first reply
}
// Mutation: the view document's `kind` ignored (id is inferred Text from a reply whose ids are strings).
TEST_CASE("model table over an action: columns and kinds come from the action's view document", "[ui][table]") {
    TableHarness h;                                                           // Lims_ListSamples has a v-columns view
    addAction(h, "Lims_ListSamples", R"({"type":"object","properties":{"query":{"type":"object","x-table":true}}})",
              ActionInfo{.modelType = "Lims_SampleModel"});
    auto screen = h.mount(withTableKey(kActionTableNoColumns, R"("action":"Lims_ListSamples")"));
    h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":[{"id":"7","name":"a"}],"total":1})");
    CHECK(headerLabels(h) == std::vector<std::string>{"Name"});               // v-columns' label; `id` is v-hidden
    CHECK(kindOfColumn(*screen, "id") == table::ColumnKind::Integer);         // v-columns' kind, not the reply's
}
// Mutations: the loader accepting an action with no x-table member (the first fetch sends a bare body); accepting
// one with neither `columns` nor a view document.
TEST_CASE("model table over an action: an action the table cannot page is refused at load", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    CHECK(refusalOf(h, kActionTableNoQuery) == "a table over an action needs a query member marked x-table");
    CHECK(refusalOf(h, kActionTableNoColumns) ==
          "a table over an action needs its columns, or a view document for the action");
}
```
`addPageSamples(h)` is the `addAction` call above; `refusalOf(h, text)` loads against the harness's catalog and returns the refusal's message.

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset cl-debug --target morph_tests` → FAIL: `readTypedPage` undeclared.
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run to verify they pass** — `ctest --test-dir build/cl-debug -R "model table over an action" --output-on-failure`; `morph_tests` green.
- [ ] **Step 5: Mutations**, as named.
- [ ] **Step 6: Neighbouring specs** as listed; docs build green.
- [ ] **Step 7: Commit** `feat(ui): a typed list action as a model-backed table`.

---

## Decisions the spec now states

Each was taken by this plan and is now stated in table.md, so no task of this part edits table.md for it.

1. **An empty `model` in a table-state key** (9c): `model` is 0 to 256 bytes, the other key members 1 to 256 ("Persisted state", "Bounds").
2. **Pages are read from the reply's text** (9a, cross-part decision 30). A `Value` cannot hold a JSON number with a fraction, so `Session::read` resolves with the reply's JSON text, and `WindowRows` decodes it with `readGenericPage` (or `readTypedPage`). The description still comes through the query cache, losslessly ("The client's caches").
3. **A `rows` table orders timestamps to the second** (11a), as a `DateTime` column holds them; the expression language's `sort` orders to the millisecond ("A document's rows as a source").
4. **A typed list action's description** (11c) comes from the node's `columns` or the action's view document, with kinds from the node, then the view document, then inferred from the first reply's JSON, because no client catalog carries a reply schema ("The client table model"). A fractional number there is a `Number`, while in a document's `rows` it is a `Decimal` (cross-part decision 46).
5. **The quota spans applications** (9c): one usage entry per person, under the model's own id ("Bounds": "across one person's stored state, in every application"); two model instances can overshoot it by one document.
6. **`kind: "number"` is the only way to a `Number` column over `rows`** (11a): a document value has no `double` ("A document's rows as a source").
7. **A new description revision mounts the table again** (9d): `Table::columns` is set once, so the columns of a new revision are a new `Table` under the same element state; the window, sort, filters and selection survive, realized cells and the scroll offset do not ("Rows on the wire").
8. **The names table state keeps are refused by the preferences model** (9c): an `app` of `morph.tableState` and a scope starting `morph.table` ("Persisted state", "Reserved names"); `docs/spec/ui/preferences.md` gains the rule in 9d's docs step.

## Self-review notes

- Spec coverage: "The pipeline" and "Sources" — 9a (`EngineRows`, `WindowRows`), 11a (`ValueRowsSource`); `VectorSource` is Task 13's. "The client table model" — 9a, 9d. "The renderer" — 10a, 10b; its "Range selection in server mode" lands in 10a (cross-part decision 13), as Delivery order step 5 lists it. "The document `table` node" — model-backed rows in 9d, `rows` rows in 11b; `cellMenu`, `headerMenu`, `layout`, `configurable`, `locked`, display parts and `maxSortKeys` are Tasks 12, 15, 16, 17. "Where sort and filter run" — the client-mode bound warning in 11b; the refusal of a client-mode sort on a column with a level needs a `rows` column with a level, which a document cannot declare yet, so it has no task here. "Persisted state" (sort and filters, both stores, load before rows) — 9b to 9d; saved and shared views are Task 14's.
- Cross-part decisions 11 (`tableMessage`, `kStatusMessageHold`: 9d), 28 (`TableSource::cellView`/`rowView`/`alwaysFetched`, cells owning a row signal, `RecordOf` → `RowRecord`: 9a, 9d, 10a) and 29 (`CellLoad`, `setCellLoad`: 9d, 10b). `highlightKey` and `model` + `action` are 10a and 11c.
- Cross-part decisions 37 (a `column` is an index into `Table::columns`: 9d, 10a), 38 (`CarriedState::Column`'s equality here, the filter types' from 8b), 39 (`TableRowLook` is extended, not replaced), 40 (`onChange` is one slot), 41 (`TableOptions` mirrors `Table`), 42 (`RowsFixture`, `TableHarness`, the catalog seam `addAction`), 44 (the loader: 9d, 11b, 11c), 46 (a fractional `rows` column is `Decimal`: 11a, 11c) and 52 (`Table::focus` and `onFocusCell` are `CellRef` here and re-typed by Task 19).
- Every test case in this part has a body and names the mutation it fails on; a helper a case uses is defined in the step that first uses it, in `tests/ui_table_support.hpp` unless the step names another file.
- Gates: U1–U4 (11a/11b), U6 and F1, F6, F7, K5, K8, T1, T3–T5, W6 (9), F1–F5, F8–F10, U7, D8 (10), U8 (10c, measured locally in a headless browser), B1 (11b).
