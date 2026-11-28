# Slow Columns: Unloaded Cells, Partial Pages, the Level and View Caches, the Window Under Changes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A table served over `apply` and read through a `PageWindow` shows a first page before a slow level loads, shows its cells as unloaded, loads a level only when a named, sorted or filtered column needs it, and updates exactly the rows that arrived, with no polling.

**Architecture:** The engine learns a fourth cell state (`Unloaded`), a three-valued filter and an undecided set (Task 3). Pages and the generic page carry which cells are unloaded and answer keyed queries (Task 4). The server gains a shared, immutable-snapshot level cache that loads each value once (Task 5), an `announce`/`attach` path and a view cache so a sort runs once for every watcher (Task 6). The client's `PageWindow` holds columns per page and turns change events into keyed fetches with probes or page refetches, and the session reads and watches without cache entries (Task 7).

**Tech Stack:** C++23, header-only morph, glaze, Catch2, CMake/Ninja (MSVC preset `cl-debug`; timings on `cl-release`).

**Spec:** `docs/spec/table/table.md` — "The unloaded cell", "Semantics of an unloaded cell" (all), "Data levels", "Partial pages", "Keyed queries", "Arrival", "The window under changes" (all), "Caching layers" through "Which client cache a screen uses", and their rows of "Tests", "Invariants", "Failure modes", "Performance" (S1–S9, L1–L8, C1–C3, W3, W4, K1–K7, E1–E4), "Neighbouring specs" (steps 2a–2d) and "Delivery order" (steps 2a–2d). Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md`, Tasks 3–7.
Part B of 7; index: 2026-11-17-one-table.md

## Global Constraints

Every line of the roadmap's Global Constraints applies; the ones this part touches most:

- Comments state what the code does now and why; no history, no issue numbers (AGENTS.md).
- Every public symbol has complete Doxygen `@param`/`@tparam`/`@return`; docs build: `cmake -S . -B build/doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF && cmake --build build/doc --target doc` exits 0.
- No company, product or customer names; examples use `lims`.
- Commits authored `yaraslau <mail@yaraslau.com>`, ending with the session's attribution lines.
- No GUI application is launched.
- Every test carries a `// Mutation: …` comment naming the change it was seen to fail on; the task applies that mutation once and pastes the failing line into the commit body.
- Limits verbatim: `QueryLimits::maxKeys = 1000` (`maxColumns = 512` is Task 1's); `PageWindow(pageSize = 200, maxPages = 8, maxKeys = 1000, maxColumns = 96)`; `LevelCacheOptions{maxBytes = 256U << 20U, expectedBytes = 0 (64 bytes a column), batch = 64, orderLease = 60 s, retryAfter = 30 s, retryMax = 600 s}`; `ViewCacheOptions::maxBytes = 64U << 20U`; `effects::kMaxChangeKeys = 4096` (existing); level cache shards: 256.
- Timings are never CI gates. Counts and bytes are, and run as ordinary test cases. Timings run by tag `"[.benchmark]"` on `cl-release` and are written into table.md's Performance tables; each benchmark first `REQUIRE`s that its feature was active.
- A spec that describes code takes its part in the task that builds it: `docs/spec/forms/views.md` (Task 3), `docs/spec/core/backend.md` "Fan-out" (Task 6), `docs/spec/core/registry.md` and `docs/spec/ui/interpreter.md` "Queries and the cache" (Task 7).

**Commands** (`<target>`, `<pattern>` per step):

```bash
cmake --preset cl-debug
cmake --build --preset cl-debug --target <target>
ctest --test-dir build/cl-debug -R "<pattern>" --output-on-failure
# timings
cmake --preset cl-release && cmake --build --preset cl-release --target <target>
build/cl-release/bin/<target>.exe "<tag>"
```

Targets: `morph_table_tests` (tests/table/*), `morph_tests` (tests/test_*.cpp at the top level), `morph_net_tests` (tests/net/*). Catch2 test names are the ctest names, so `-R` takes a regex over them. `morph_net_tests` is POSIX-only (index decision 18): its cases build and run on Linux with `cmake --preset linux-everything && cmake --build --preset linux-everything --target morph_net_tests && ctest --test-dir build/linux-everything -R "<pattern>" --output-on-failure`; on Windows only the `cl-debug` targets above run.

## Interfaces from other parts

These follow the index's "Cross-part decisions" and Part A (`2026-11-17-one-table-a-foundation.md`).

- **A1. The answer path** (decision 5) is `table::detail::answerQuery(ServedTable const& served, TableQuery const& query, AnswerContext const& context = {}) -> std::expected<GenericPage, TableError>` in `description.hpp`. Task 2 defines it with an empty `struct AnswerContext {}`. `QueryTable<F>::invoke` gets the `ServedTable` through `table::detail::serve<Function>(model, context)` (decision 23), replaces `served.source` on the strand with a `detail::PinnedSource` of one snapshot, and runs `answerQuery` on the table executor, so `answerQuery` never snapshots a live source (tests that pass a live source get it pinned by `detail::pinned(served)`). Tasks 4 and 5 change its body; Task 6 adds `AnswerContext::{views, record, rowScoped}` (decision 48).
- **A2. Where table work runs** (decisions 1, 6). `ServerConfig::tableWork` (Task 2); `table::TableRuntime{exec::IExecutor* work = nullptr;}`, `table::currentTableRuntime()` and `table::TableRuntimeScope`, installed by `RemoteServer` around a dispatch (Task 2). Task 6 adds `ViewCache* views` to `TableRuntime` and `ServerConfig::views`; `QueryTable::invoke` builds `AnswerContext{.views = runtime->views}` from it.
- **A3. The fixture** (Tasks 0, 2). `WideTableSource` is internally synchronised and `WideTableModel::configure(wide, small)` may be called at any time (it drops both sources; the next table call rebuilds them). The key column is `id`, Integer, value `row + 1`, so row `r`'s key is `r + 1` and a table of n rows has keys 1…n. The model type id is `"WideTableModel"`, so the records are `"WideTableModel.wide"`, `"WideTableModel.small"`, `"WideTableModel.scoped"`. The `recipe` level's columns are named by Part A's layout (`bench::WideColumn::level`): `d000`…`d009` (Decimal) and `t001`…`t010` (Text) in `wide`; `small` (`wideColumns(30)`) has only `d000`…`d005` and `t001`…`t006` of them. Part A cannot set `ColumnInfo::level` (the member is Task 3's) and serves `TableDescription::levels` empty; Task 3a sets both (its Step 5). `scoped` serves `small`'s rows to every principal and varies its columns: every principal lacks `d000`…`d005` except `"extended"` (all 30), and `"restricted"` also lacks `t000`…`t006`. No fixture table scopes rows.
- **A4. The record walk** (Task 2). `ResultSlot` gains `bool opaque = false` and `effects::ActionRecord` gains `bool opaque = false`; `Table.<id>`'s registration adds an opaque slot, so `recordsOf("Table.small") == {ActionRecord{.record = "WideTableModel.small", .keyField = "key", .paths = {}, .opaque = true}}`. A holder type with `heldRecord`/`heldMember()` reports its record at that member. Task 7b adds `open` and `partial` beside `opaque`, and copies all three to `ActionRecordInfo`.
- **A5.** Task 1's `GenericPage`, `writeGenericPage`, `readGenericPage`, `TableQuery::columns` and `QueryLimits::maxColumns` (decision 2), with `validate`'s `columns` rules; the writer writes `Unloaded` as `null` once Task 3 adds it (Task 3a gives `detail::appendCellJson` that branch).
- **A6. Equality** (decision 38). `QuantityCell::operator==` is Task 0's, so `Cell == Cell` compiles; `tabletest::sameCell(Cell const&, Cell const&)` (Task 0, `tests/table/table_fixtures.hpp`) compares places too. `GenericPage` has no `operator==`: a test that says a page "reads back equal" compares its members, cells by `sameCell`.
- **A7. Groups** (decision 43). Task 8b makes `detail::PinnedSource`, `LeveledSource` and the scoped source forward `DataSource::groups()`; nothing here overrides or hides it.

---

### Task 3: Unloaded cells in the engine (table.md step 2a)

Two commits, each its own reviewer gate: **3a** the cell, its order, the filter and the undecided set; **3b** data levels on columns, rows and pages.

**Spec:** "The unloaded cell", "Semantics of an unloaded cell", "Data levels"; Tests rows (a) engine half, (b) ×3, (c) `levelsNeeded` table, (d) engine half; invariants 1, 2, 3 (without retain), 4, 5, 6 (first and third clause), 7; Failure modes "A level never arrives", "A `Unloaded` cell in a base-level column", "A `columnLevels()` entry naming no column", "An edit of an unloaded cell".

**Files (3a):**
- Modify: `include/morph/table/data_source.hpp` (`Unloaded`, `Cell`, `ColumnInfo::level`, `TableErrorCode::SourceContract` and its glaze name, `displayText`)
- Modify: `include/morph/table/sort.hpp` (`CellState`, `KeyColumn::read`, `compareKeys`)
- Modify: `include/morph/table/filter.hpp` (`Match`, `evaluate`, `evaluateGroup`, `evaluateRow`, `FilterRun`)
- Modify: `include/morph/table/engine.hpp` (`detail::JobInput`, `detail::JobResult`, `detail::ViewJob`, `Engine`)
- Modify: `include/morph/table/query.hpp` (`apply`: undecided, `totalKind`; `PageRows::{totalKind, undecided, awaiting, failed}`)
- Modify: `include/morph/table/edits.hpp` (`CellEdits::commit`)
- Modify: `include/morph/table/description.hpp` (`detail::appendCellJson` writes `Unloaded` as `null`, so the header compiles with the new alternative; Task 4 tests it)
- Modify: `tests/table/table_fixtures.hpp` (`levelCol`, `slowColumn`)
- Modify: `tests/table/wide_table_model.{hpp,cpp}` (`WideTableSource::columns()` sets `ColumnInfo::level` from `WideColumn::level`; the model's three descriptions set `levels = {"recipe"}`)
- Create: `tests/table/test_table_unloaded_cells.cpp` (add to `morph_table_tests` in `tests/CMakeLists.txt`)
- Test: `tests/table/test_table_sort.cpp`, `tests/table/test_table_filter.cpp`, `tests/table/test_table_changes.cpp`

**Files (3b):**
- Modify: `include/morph/table/query.hpp` (`ColumnLevel`, `LevelLoaded`, `RowsSnapshot`, `RowsSource`, typed `apply`, `LevelsNeeded`, `levelsNeeded`, `UnloadedRow`, `Page::{unloadedRows, undecided, awaiting, failed}`)
- Modify: `include/morph/forms/forms.hpp` (`detail::mergeSchemaExtras` writes `x-level`), `include/morph/forms/views.hpp` (`buildColumnEntry` copies `x-level`), `include/morph/forms/engine/collection.hpp` (`CollectionColumn::level`)
- Modify: `tests/table/wide_table_model.{hpp,cpp}` (`bench::UnloadedSource`, `bench::withUnloaded`), `tests/table/bench_table.cpp` (S1, S2, S4–S6)
- Test: `tests/table/test_table_unloaded_cells.cpp`, `tests/table/test_table_query.cpp`, `tests/test_views.cpp`
- Docs: `docs/spec/forms/views.md` (`x-level` on a schema property; `deriveColumns` copies it)

**Interfaces:**
- Consumes: `bench::WideTableOptions`, `bench::WideTableSource` (Task 0).
- Produces (3a):
  - `EngineStats::filterRuns` (`std::size_t`): incremented by the `Engine` when it applies a job result whose `detail::JobResult::filtered` is set; `ViewJob::plan()` sets `filtered` when it constructs a `FilterRun` (a repair never does)
  - `struct Unloaded { friend constexpr bool operator==(Unloaded, Unloaded) noexcept = default; };`
  - `using Cell = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string, bool, QuantityCell, Unloaded>;` (Task 8c appends `Measure` after it)
  - `ColumnInfo::level` (`std::string`, after `format`; part of `operator==`). It lands in 3a because the base-level rule needs it: a cell may be `Unloaded` only in a column with a level.
  - `TableErrorCode::SourceContract` (JSON `"sourceContract"`, appended last)
  - `CellState::Unloaded` (appended after `Invalid`)
  - `enum class Match : std::uint8_t { No, Yes, Unknown };` in `filter.hpp`
  - `static Match CompiledFilter::evaluate(Column const& filter, KeyColumn const& keys, std::uint32_t row);` `static bool passes(...)` returns `evaluate(...) == Match::Yes`
  - `static Match CompiledFilter::evaluateGroup(Group const& group, std::span<KeyColumn const* const> keys, std::uint32_t row);` (keys by column index)
  - `Match CompiledFilter::evaluateRow(std::span<KeyColumn const* const> keys, std::uint32_t row) const;`
  - `std::vector<std::uint32_t> FilterRun::take() &&` (the `Yes` rows), `std::vector<std::uint32_t> FilterRun::takeUndecided() &&` (the `Unknown` rows); both keep candidate order; each may be called once, in either order
  - `detail::JobInput::previousUndecided`, `detail::JobResult::undecided` (`std::vector<std::uint32_t>`, source rows ascending)
  - `std::optional<TableError> const& detail::ViewJob::failure() const noexcept` (replaces the `std::optional<std::string>`)
  - `std::size_t Engine::undecidedRowCount() const noexcept;` `std::span<std::uint32_t const> Engine::undecidedRows() const noexcept;` `void Engine::onUndecidedChange(std::function<void(std::size_t)> handler);`
  - `PageRows::{TotalKind totalKind; std::int64_t undecided; std::vector<std::string> awaiting; std::vector<std::string> failed;}`
- Produces (3b):
  - `struct ColumnLevel { std::string_view column; std::string_view level; };`
  - `template <typename Row> using LevelLoaded = std::function<bool(Row const& row, std::string_view level)>;`
  - `RowsSource(rows, key, kinds = {}, comparators = {}, LevelLoaded<Row> loaded = {})`; typed `apply(rows, key, query, options = {}, kinds = {}, comparators = {}, LevelLoaded<Row> loaded = {})`
  - `struct LevelsNeeded { std::vector<std::string> order; std::vector<std::string> display; };`
  - `[[nodiscard]] LevelsNeeded levelsNeeded(TableQuery const& query, std::span<ColumnInfo const> columns, std::function<bool(std::string_view)> const& mayRead = {}, Services const& services = {});`
  - `struct UnloadedRow { std::int64_t index = 0; std::vector<std::string> levels; std::vector<std::string> failed; [[nodiscard]] bool operator==(UnloadedRow const&) const = default; };` (decision 38)
  - `Page<Row>::{std::vector<UnloadedRow> unloadedRows; std::int64_t undecided = 0; std::vector<std::string> awaiting; std::vector<std::string> failed;}`
  - `CollectionColumn::level` (`std::string`)
  - Test helpers (3a): `tabletest::levelCol(std::string id, ColumnKind kind, std::string level) -> ColumnInfo`; `tabletest::slowColumn(ColumnKind kind, std::vector<Cell> cells) -> std::shared_ptr<VectorSource>` (one column `c` at level `"slow"`, keys 0…n−1). (3b): `class bench::UnloadedSource final : public DataSource` and `bench::withUnloaded(std::shared_ptr<DataSource> source, std::vector<std::string> columns, std::string level, double loadedFraction, std::uint64_t seed) -> std::shared_ptr<bench::UnloadedSource>` (a wrapping source: the named columns get `level`, and a deterministic `1 - loadedFraction` of rows, chosen by `(seed, row)`, read `Unloaded` there). `void UnloadedSource::load(std::span<std::size_t const> rows)` marks those source rows loaded, publishes a new snapshot and notifies its own listeners `RowChange{Updated, rows}`: this is how a test makes rows arrive. `std::vector<std::size_t> UnloadedSource::unloadedRows() const` lists the rows still unloaded, ascending. It subscribes to the wrapped source and forwards its changes

#### 3a — the cell, its order, the filter and the undecided set

- [ ] **Step 1: Write the failing order tests** in `tests/table/test_table_sort.cpp`

```cpp
// Mutation: compareKeys tying unloaded with empty (rank Empty, Invalid and Unloaded alike).
TEST_CASE("table: unloaded cells are the tail after the empties in both directions", "[table][sort][unloaded]") {
    using morph::table::Unloaded;
    auto source = tabletest::slowColumn(ColumnKind::Integer,
                                    {Cell{std::int64_t{3}}, Cell{Unloaded{}}, Cell{}, Cell{std::int64_t{1}},
                                     Cell{Unloaded{}}, Cell{std::string{"x"}}});
    for (auto const dir : {SortDirection::Ascending, SortDirection::Descending}) {
        Engine engine{source};
        REQUIRE(engine.setSort({SortKey{.column = "c", .dir = dir}}));
        auto const keys = tabletest::viewKeys(engine);
        auto const valid = dir == SortDirection::Ascending ? std::vector<RowId>{3, 0} : std::vector<RowId>{0, 3};
        CHECK(std::vector<RowId>(keys.begin(), keys.begin() + 2) == valid);
        CHECK(std::vector<RowId>(keys.begin() + 2, keys.begin() + 4) == std::vector<RowId>{2, 5});  // empty, invalid
        CHECK(std::vector<RowId>(keys.begin() + 4, keys.end()) == std::vector<RowId>{1, 4});        // unloaded
    }
}
```

Every column that holds an `Unloaded` cell in this plan's tests is declared with a level (`levelCol`, `slowColumn`); a base-level one is the `SourceContract` case.

Add `"table: rows with no unloaded sort cell keep their order under any completion"` (invariant 2): 200 random tables of 40 rows × 2 `Integer` columns at level `"slow"` from `std::mt19937{seed}` with 20% `Unloaded`; for each, 20 random completions replacing every `Unloaded` with an integer; for every pair of rows with no unloaded cell in the sort columns, their relative order under `setSort({a asc, b desc})` is the same before and after. Mutation: `compareKeys` answering 0 whenever either cell is `Unloaded` (an unloaded cell tying with every cell, which breaks transitivity, so the sort may order two loaded rows by where the unloaded rows sit). Ranking `Unloaded` as `Valid` with key 0 is not a usable mutation: the relative order of two loaded rows depends on their own keys under any consistent comparator, so the property holds and the case passes.

- [ ] **Step 2: Write the failing filter tests** in `tests/table/test_table_filter.cpp`

```cpp
// Mutation: evaluate returning passes (two-valued), which rejects the unloaded row.
TEST_CASE("table: an unloaded cell is unknown to every operator", "[table][filter][unloaded]") {
    using morph::table::Unloaded;
    struct Case { ColumnKind kind; FilterEntry entry; };
    std::vector<Case> const cases{
        {ColumnKind::Integer, {.eq = "1"}}, {ColumnKind::Integer, {.ne = "1"}}, {ColumnKind::Integer, {.lt = "1"}},
        {ColumnKind::Integer, {.le = "1"}}, {ColumnKind::Integer, {.gt = "1"}}, {ColumnKind::Integer, {.ge = "1"}},
        {ColumnKind::Decimal, {.between = std::array<std::string, 2>{"1", "2"}}},
        {ColumnKind::Text, {.contains = "a"}}, {ColumnKind::Text, {.startsWith = "a"}},
        {ColumnKind::Text, {.endsWith = "a"}}, {ColumnKind::Date, {.eq = "2026-01-01"}},
        {ColumnKind::Bool, {.eq = "true"}}, {ColumnKind::Text, {.isEmpty = true}},
        {ColumnKind::Text, {.notEmpty = true}}};
    for (auto const& [kind, entry] : cases) {
        for (bool const exclude : {false, true}) {
            for (auto const combine : {Combine::Any, Combine::All}) {
                auto source = tabletest::slowColumn(kind, {Cell{Unloaded{}}});
                ColumnFilter filter{.combine = combine};
                (exclude ? filter.exclude : filter.include).push_back(entry);
                auto page = apply(*source, TableQuery{.filters = FilterSpec{.columns = {{"c", filter}}}});
                REQUIRE(page.has_value());
                CHECK(page->rows.empty());
                CHECK(page->undecided == 1);
                CHECK(page->total == 0);
                CHECK(page->totalKind == TotalKind::AtLeast);
            }
        }
    }
}
```

Add:
- `"table: a group admits on one loaded match and is unknown otherwise"`: columns `a`, `b` Text at level `"slow"`; group `{columns {a,b}, include {contains "x"}}`; rows `{x, Unloaded}` → view; `{Unloaded, y}` → undecided; `{y, z}` → rejected; `{Unloaded, Unloaded}` → undecided. Mutation: a group evaluated as `all` of its columns.
- `"table: a row unknown at one stage and rejected at a later one is rejected"`: filters `a eq "1"` (row's `a` unloaded) and `b eq "2"` (row's `b` = `"3"`): `rows` empty, `undecided == 0`, `totalKind == Exact`. Mutation: `FilterRun` dropping the undecided mark when a later stage answers `No` (keeping the row undecided).
- `"table: the filter is sound under every completion"` (invariant 1): 200 random tables of 30 rows × 3 columns (`Integer`, `Text`, `Bool`, level `"slow"`) with 25% `Unloaded`, random filters of 1–3 column entries and 0–1 group from `{eq, ne, lt, gt, contains, isEmpty, notEmpty}` in include and exclude; 20 completions each: every row of the view is in the completed view, every rejected row is rejected there. Mutation: `evaluate` answering `No` for `ne` on an unloaded cell.

- [ ] **Step 3: Write the failing engine tests** — `tests/table/test_table_unloaded_cells.cpp` (header: what the file covers; each case its `// Mutation:`)

- `"table: a base-level unloaded cell is a source contract error"`: `apply` over a source whose column `c` (`tabletest::col`, level `""`) holds `Unloaded` in row 1 returns `TableErrorCode::SourceContract` with `.column == "c"` when sorting by `c`. A synchronous `Engine engine{source}` returns a failed job from the call that asked for it, so `engine.setSort({SortKey{.column = "c"}})` returns the same error (not through `onError`), `engine.sort()` is empty and `viewKeys` is as before. `onError` is for a job no call could return: over a fully loaded copy of the table already sorted by `c`, with an `onError` handler set, `source->updateRow(1, {Cell{Unloaded{}}})` calls the handler once with `SourceContract` and `.column == "c"`, and `viewKeys` is as before the update. The same table with `c` at level `"slow"` sorts with row 1 last. Mutation: key building without the base-level check (both halves: `setSort` succeeds, the handler is not called).
- `"table: cellAt returns the unloaded cell"`: `engine.cellAt(0, 0)` holds `Unloaded` and `displayText(engine.cellAt(0, 0)) == ""`. Mutation: `displayText` returning `"…"`.
- `"table: an edit of an unloaded cell is refused on the cell"`: a `VectorSource` with columns `c` (level `"slow"`, row 0 `Unloaded`) and `d` (base, row 0 `"x"`); `CellEdits edits{*source, commit, &engine}`; `edits.commit({.row = 0, .column = "c", .value = Cell{std::int64_t{5}}})`: the commit function is not called and `edits.error(0, "c") == "not loaded yet"`; an edit of loaded column `d` of the same row is committed. Mutation: `commit` not looking at the cell.
- `"table: selectAll does not select undecided rows"` and `"table: a selected row that becomes undecided is pruned"` (`SelectionOptions{.pruneOnFilter = true}`; row 1 selected, its filtered cell updated to `Unloaded` via `updateRow`: `selection.contains(1)` is false). Mutations: `selectAll` over `view ∪ undecided` (the first case); `Selection::viewChanged` keeping a selected row that is in the undecided set (the second, which the first mutation does not reach).

Engine half of (d) in `tests/table/test_table_changes.cpp`:

```cpp
// Mutation: a repair that ignores previousUndecided (an arrived row stays undecided).
TEST_CASE("table: an undecided row that arrives is one insert and one count change", "[table][changes][unloaded]") {
    using morph::table::Unloaded;
    auto source = tabletest::makeSource({tabletest::levelCol("a", ColumnKind::Integer, "slow")},
                                        {{Cell{std::int64_t{1}}}, {Cell{Unloaded{}}}, {Cell{std::int64_t{3}}}});
    Engine engine{source};
    REQUIRE(engine.setSort({SortKey{.column = "a"}}));
    REQUIRE(engine.setFilter(FilterSpec{.columns = {{"a", ColumnFilter{.include = {FilterEntry{.gt = "0"}}}}}}));
    CHECK(engine.undecidedRowCount() == 1);
    std::vector<std::size_t> counts;
    std::vector<std::string> order;
    ViewChange last;  // the engine has one view handler: record here rather than through FollowingModel
    engine.onViewChange([&](ViewChange const& change) { last = change; order.emplace_back("view"); });
    engine.onUndecidedChange([&](std::size_t count) { counts.push_back(count); order.emplace_back("undecided"); });
    source->updateRow(1, {Cell{std::int64_t{2}}});
    CHECK(tabletest::viewKeys(engine) == std::vector<RowId>{0, 1, 2});
    REQUIRE(last.ops.size() == 1);
    CHECK(last.ops[0].kind == ViewOp::Kind::Inserted);
    CHECK(last.ops[0].first == 1);
    CHECK(last.ops[0].count == 1);
    CHECK(counts == std::vector<std::size_t>{0});
    CHECK(order == std::vector<std::string>{"view", "undecided"});
}
```

Add:
- `"table: an undecided row rejected on arrival is no view operation"`: same table, `updateRow(1, {Cell{std::int64_t{-5}}})`: `onViewChange` not called, `counts == {0}`. Mutation: `onUndecidedChange` called only with a view change.
- `"table: a displayed row whose level arrives is changed in place"` for `ReorderPolicy::Immediate` and `Deferred`: sorted by `a`, filtered on nothing, row 1's non-sort column `b` goes from `Unloaded` to `"x"`: one `Changed` op at view row 1, no `Moved`. Mutation: a repair appending the dirty rows after the clean ones rather than merging them, which reports the arrived row as `Moved`.
- `"table: a cell dropped back to unloaded moves to the tail or leaves the view"` (Review Focus 2): sorted ascending by `a`, unfiltered, row 0's `a` updated from `1` to `Unloaded`: row 0 is last; with `a gt 0` filtered, the same update makes it undecided (`viewKeys` lacks 0, `undecidedRowCount() == 1`). Mutation: `KeyColumn::set` keeping the old key for a cell that became `Unloaded`.
- `"table: under deferred reorder an undecided row takes its true place at once"` (Review Focus 5): `EngineOptions{.reorder = ReorderPolicy::Deferred}`, view `{0: 1, 2: 3}`, undecided row 1 arrives with `2`: `viewKeys == {0, 1, 2}` before `settleNow()`, and `heldRows()` does not contain 1. Mutation: the repair holding entering rows like updated ones.

- [ ] **Step 4: Run the new cases to verify they fail**

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile errors naming `Unloaded`, `undecided`, `onUndecidedChange`, `totalKind` on `PageRows`.

- [ ] **Step 5: Implement the cell, `ColumnInfo::level`, `displayText` and `SourceContract`** in `data_source.hpp`

`ColumnInfo::level` carries the spec's Doxygen ("The data level the column's cells need. Empty is the base level, which every row has when it exists."); `tabletest::col` keeps its signature (level empty). `displayText` gets an `Unloaded` branch returning `{}`, and `detail::appendCellJson` (`description.hpp`) one writing `null`. The glaze enumeration of `TableErrorCode` adds `"sourceContract", SourceContract`.

The fixture takes its levels here, because nothing before this commit could: `WideTableSource::columns()` sets each column's `level` from `WideColumn::level` (`"recipe"` on `d000`…`d009` and `t001`…`t010`), so the description columns `WideTableModel` builds from them carry it, and `wide`, `small` and `scoped` set `description.levels = {"recipe"}` (a description that lacks every recipe column sets none). Until Task 5 gives the level a cache, the cells of those columns are read from the source and are loaded. A Part A case that pins `levels` empty, or a description revision computed without levels, is updated in this commit.

- [ ] **Step 6: Implement the order** in `sort.hpp`

`KeyColumn::read` first maps `Unloaded` to `CellState::Unloaded` (before the empty check and the kind switch) and stores no key. `compareKeys` compares a rank before keys, ignoring `dir`:

```cpp
// Valid 0; Empty and Invalid 1 (they tie); Unloaded 2 (they tie). Ranks never flip with the direction.
int const rankA = rank(keys.states[left]);
int const rankB = rank(keys.states[right]);
if (rankA != rankB) { return rankA < rankB ? -1 : 1; }
if (rankA != 0) { return 0; }
```

`RowOrder` already breaks ties by source row.

- [ ] **Step 7: Implement the three-valued filter** in `filter.hpp`

- `evaluate`: a cell whose state is `Unloaded` is `Unknown` whatever the entries (`isEmpty`, `notEmpty` and `exclude` included); otherwise `Yes`/`No` exactly as the current `passes` body.
- `evaluateGroup` (plain group; Task 8b adds quantifiers): `Yes` if any column is `Yes`; else `Unknown` if any is `Unknown`; else `No`.
- `evaluateRow`: `No` if any column filter or group is `No`; else `Unknown` if any is `Unknown`; else `Yes`.
- `FilterRun` keeps a mark per candidate (`std::vector<std::uint8_t>`, compacted with `_rows`): a stage answering `No` drops the row, `Unknown` keeps it marked, `Yes` keeps it as it was. `take()` returns the unmarked survivors, `takeUndecided()` the marked ones.

- [ ] **Step 8: Implement the undecided set** in `engine.hpp`

- `ViewJob::step` maps an exception to `TableError{ServiceFailed, {}, "a service failed: " + what}`; the three call sites (`Engine` ×2, `apply`) use `failure()` as the error directly.
- Key building: after a needed column's keys are built, and after each dirty row is patched, a column with `level.empty()` holding a `CellState::Unloaded` fails the job with `TableError{SourceContract, <column id>, "an unloaded cell in a base-level column"}`.
- `order()`: after the filter run, `_result.undecided = std::move(run).takeUndecided()` sorted ascending.
- `repair()`: each dirty row is evaluated with `evaluateRow`: `Yes` joins `passing`, `Unknown` joins the new undecided rows, `No` neither; `_result.undecided` = (`previousUndecided` minus `dirty`, both ascending: a set difference, no filter run) ∪ the new undecided, ascending.
- `Engine` keeps `_undecided` with `_view` and passes it as `previousUndecided`; on applying a result it sets `_undecided`, then calls `onViewChange` if the view changed, then `onUndecidedChange(count)` if the count differs from the one before the job.
- A row entering the view from the undecided set is not in `previousView`, so under `Deferred` the diff inserts it at its true place; displayed rows keep their held places.

- [ ] **Step 9: Implement totals and the edit refusal**

- `apply` fills `PageRows::undecided` from the job's `undecided.size()` and `totalKind = undecided > 0 ? AtLeast : Exact`; `total` stays absent above `totalCap`.
- `CellEdits::commit`: when `_engine` is set and `_engine->viewRowOfKey(request.row)` finds a row whose `cellAt(viewRow, column)` holds `Unloaded` (column found by id in `_engine->columns()`), it records the error `"not loaded yet"` for the cell and returns without queueing; the cell's other edits are unaffected.

- [ ] **Step 10: Run the target**

Run: `cmake --build --preset cl-debug --target morph_table_tests && ctest --test-dir build/cl-debug -R "^table" --output-on-failure`
Expected: all pass, the new cases included.

- [ ] **Step 11: Apply each named mutation, see its test fail, revert. Commit**

```bash
git add include/morph/table tests/table tests/CMakeLists.txt
git commit -m "feat(table): unloaded cells, their order and the undecided set"
```

#### 3b — data levels on columns, typed rows and pages

- [ ] **Step 12: Write the failing level tests** in `tests/table/test_table_query.cpp`

```cpp
// Mutation: levelsNeeded returning every level of the columns.
TEST_CASE("table: levelsNeeded names the levels a query sorts, filters and shows", "[table][query][levels]") {
    using tabletest::levelCol;
    std::vector<ColumnInfo> const columns{tabletest::col("id", ColumnKind::Integer),
                                          levelCol("r1", ColumnKind::Decimal, "recipe"),
                                          levelCol("r2", ColumnKind::Text, "recipe"),
                                          levelCol("s1", ColumnKind::Text, "stock"),
                                          levelCol("p1", ColumnKind::Integer, "price")};
    auto const needed = [&](TableQuery const& query) { return levelsNeeded(query, columns); };
    using V = std::vector<std::string>;
    TableQuery named{.columns = V{"id", "r2"}};
    CHECK(needed(named).order.empty());
    CHECK(needed(named).display == V{"recipe"});
    TableQuery sorted{.sort = {SortKey{.column = "s1"}}, .columns = V{"id"}};
    CHECK(needed(sorted).order == V{"stock"});
    CHECK(needed(sorted).display.empty());
    TableQuery filtered{.filters = FilterSpec{.columns = {{"p1", ColumnFilter{.include = {{.gt = "1"}}}}}},
                        .columns = V{"p1"}};
    CHECK(needed(filtered).order == V{"price"});
    CHECK(needed(filtered).display == V{"price"});
    TableQuery grouped{
        .filters = FilterSpec{.groups = {GroupFilter{.columns = {"r2", "s1"}, .include = {{.contains = "a"}}}}},
        .columns = V{}};
    CHECK(needed(grouped).order == V{"recipe", "stock"});
    TableQuery emptyGroup{.filters = FilterSpec{.groups = {GroupFilter{.columns = {"r1", "s1"}}}}, .columns = V{}};
    CHECK(needed(emptyGroup).order.empty());  // a group with no entry reads no column
    CHECK(needed(TableQuery{.columns = V{"id"}}).display.empty());
    CHECK(needed(TableQuery{}).display == V{"price", "recipe", "stock"});           // columns absent: every column
    CHECK(levelsNeeded(TableQuery{}, columns, [](std::string_view c) { return c != "p1"; }).display ==
          V{"recipe", "stock"});                                                   // only what the caller may read
    CHECK(needed(TableQuery{.columns = V{}}).display.empty());                     // columns empty: keys alone
}
```

(`TableQuery::columns` is Task 1's, decision 2.)

In `tests/table/test_table_unloaded_cells.cpp`:

- `"table: apply reads only the sorted and filtered columns"` (row (a), gate S3 with levels): a recording `DataSource` wrapping `bench::withUnloaded(WideTableSource{{.rows = 1000, .columns = 300, .seed = 7}}, `d000`…`d009`, `t001`…`t010`, "recipe", 0.7, 7)` logs the column of every `cell` and `readColumn` call; `apply` sorted on one recipe decimal and filtered `contains "a"` on one recipe text returns a page; the logged columns are exactly those two. Mutation: `apply` building keys for every column.
- `"table: a typed page names exactly its unloaded rows and levels"` (row (b)): rows `struct LevelRow { std::int64_t id; std::string name; std::int64_t qty; static constexpr std::array<table::ColumnLevel, 1> columnLevels() noexcept { return {{{"qty", "stock"}}}; } };` ids 0–9, `LevelLoaded` refusing `stock` for odd ids; typed `apply` with `columns` absent: `unloadedRows` is `{{1,{"stock"},{}}, {3,…}, {5,…}, {7,…}, {9,…}}` in index order; with `columns = {"id","name"}` and no sort/filter on `qty`, `unloadedRows` is empty (the level is not needed). Mutation: the typed `apply` dropping `unloadedRows`.
- `"typed: a row's column levels reach the description"` (in `tests/table/test_table_description.cpp`): `detail::serve<&LevelModel::rows>(model, ctx).description` for a typed table of `LevelRow` has column `qty` with `info.level == "stock"` and `levels == {"stock"}`. Mutation: `columnLevels()` applied in `RowsSource` only, not in `reflectedColumns`.
- `"table: a column level naming no column is refused where columns are reflected"`: a row whose `columnLevels()` names `"nope"`: `RowsSource` construction throws `std::invalid_argument` whose message contains `nope`. Mutation: the entry ignored.
- `"table: a typed table without levels is whole"` (invariant 7): typed `apply` with no `LevelLoaded` and no `columnLevels()` yields `unloadedRows`, `awaiting`, `failed` empty, `undecided == 0`, `totalKind == Exact`, and the same `rows`, `total` as before this change; and the same for `LevelRow` (which has `columnLevels()`) with no `LevelLoaded`, since empty means every level is loaded. Mutation: `awaiting` filled with every level of the columns when `loaded` is empty (only the `LevelRow` half can fail on it: a row with no `columnLevels()` has no level to fill `awaiting` with).

In `tests/test_views.cpp`: `"views: deriveColumns copies x-level"` — for `LevelRow`, `forms::schemaJson<LevelRow>()` has `"x-level":"stock"` on `qty`, and `deriveColumns` gives that column `level == "stock"` (and `CollectionColumn::level` reads it). Mutation: `buildColumnEntry` not copying `x-level`.

- [ ] **Step 13: Run the new cases to verify they fail** — `cmake --build --preset cl-debug --target morph_table_tests morph_tests`; expected: compile errors naming `level`, `levelsNeeded`, `unloadedRows`, `ColumnLevel`.

- [ ] **Step 14: Implement levels on typed rows, pages and schemas**

- `detail::reflectedColumns<Row>()` (in `query.hpp`, which Task 2's `typed` also reads) applies `Row::columnLevels()` when the member function exists: each entry sets that column's `level`; an entry naming no column throws `std::invalid_argument{"columnLevels() names no column: <id>"}`. So `RowsSource` and Task 2's `typed<Row>` description both carry the levels, and 3b changes `typed`'s description builder (`detail::serve`'s typed branch) to set `description.levels` to the sorted, unique non-empty levels of its columns. `RowsSnapshot` holds the `LevelLoaded` and a per-column level; `cell` and `readColumn` return `Unloaded{}` for a column whose level the predicate refuses, without calling the member's reader.
- `levelsNeeded`: `order` = levels of exactly the columns `apply` builds keys for: every sort key's column, and `compileFilterGuarded(query.filters, columns, services)->columnsUsed()` (so a column filter with an entry; a group's column only when the group has an `include` entry and one of them compiles on it; nothing from a filter that does not compile) (Task 8a adds the distinct request's column or group); `display` = levels of `query.columns`, or of every column `mayRead` allows when absent; both sorted, unique, without `""`. No rows read. The typed `apply` passes `options.services.forTask()`. A second case, `"table: levelsNeeded leaves out a group's column on which no entry compiles"`: group `{r1 Decimal, s1 Text}` with `contains "a"` → `order == {"stock"}`, with `gt "5"` → `{"recipe"}`. Mutations: a group with no `include` entry adding its columns (first case); a group's column on which no entry compiles added (second case).
- Typed `apply`: copies `undecided`, `totalKind`, `awaiting`, `failed` from `PageRows`; builds `unloadedRows` for the page's rows from `LevelLoaded` over the needed levels (`order ∪ display`), each row's `levels` sorted; `failed` per row stays empty (a predicate cannot say a load failed). `awaiting` = the `order` levels the predicate refuses for at least one row of the view or the undecided set. Without a predicate all three stay empty.
- `forms::detail::mergeSchemaExtras<Row>` writes `"x-level": <level>` on each property `columnLevels()` names; `views::buildColumnEntry` adds `"x-level"` to the keys it copies; `CollectionColumn::level` reads it.
- `bench::UnloadedSource` and `bench::withUnloaded` as the Interfaces block state; its snapshot hands runs through `readColumn`, and `load(rows)` is one new snapshot and one `Updated` notification for all of `rows`.

- [ ] **Step 15: Add the benchmarks** in `tests/table/bench_table.cpp`, tag `"[.benchmark][table][unloaded]"`, each first `REQUIRE`-ing unloaded cells present:
- S1: `apply`, sort one recipe decimal, page 200, 100,000 × 300 via `withUnloaded(…, 0.7, 7)`, against the same with `loadedFraction = 1.0`. S2: text `contains` on a recipe text column.
- S4: `levelsNeeded` over 300 columns, 8 sort keys, 32 filtered columns.
- S5: an `Engine` over a `bench::UnloadedSource` (`withUnloaded(…, 0.7, 7)`) sorted on a recipe decimal, 100,000 rows; `source->load(rows)` of the first 200 and then the next 1,000 of `unloadedRows()`. Gates as ordinary cases (`"table: an arrival is one repair"`): each `load` raises `EngineStats::repairs` by one and `fullSorts` by none, and the arrived rows sit at the view rows a fresh `apply` over the same snapshot gives (the oracle). Mutation: `load` notifying `Reset` (a full sort instead of a repair).
- S6: 50,000 undecided rows (filter on a recipe column with 50% loaded), 1,000 of them decided by one `load`: gate case `"table: deciding rows runs no filter over all rows"`: one repair, `EngineStats::filterRuns` unchanged by the arrival, and each decided row is in the view or rejected as the oracle has it. Mutation: a repair that constructs a `FilterRun` over `previousUndecided`.

- [ ] **Step 16: Run the targets** — `ctest --test-dir build/cl-debug -R "^(table|views)" --output-on-failure`: all pass. Record S1, S2, S4–S6 on `cl-release` into table.md's "Benchmarks: server" table (median, p10–p90, load average).

- [ ] **Step 17: Update `docs/spec/forms/views.md`** (`x-level` on a schema property; `deriveColumns` copies it to the column as it copies `x-comparator`). Docs build green.

- [ ] **Step 18: Mutations, then commit**

```bash
git add include/morph/table include/morph/forms tests docs/spec
git commit -m "feat(table): data levels on columns, typed rows and pages"
```

---

### Task 4: Partial pages and keyed queries on the wire (table.md step 2b)

**Spec:** "Partial pages", "Keyed queries", "Data levels" (the `columns` rules of `validate`); Tests rows "`TableQuery` on the wire", "The generic page" (unloaded); Failure modes "A keyed query to a model that does not translate keys", "A query's `columns` names a column the table lacks…".

**Files:**
- Modify: `include/morph/table/query.hpp` (`TableQuery::keys`, `TableQuery::retry`; `QueryLimits::maxKeys`; `validate`'s keyed rules; `PageRows::indices`; `Page::indices`; `UnloadedLevels`; `detail::ComputedView`, `detail::computeView`, `detail::pageRows`; keyed `apply`)
- Modify: `include/morph/table/description.hpp` (`GenericPage::{unloadedRows, undecided, awaiting, failed, indices}`; the writer and reader; `detail::answerQuery`, A1)
- Test: `tests/table/test_table_query.cpp`, `tests/table/test_table_description.cpp`; benchmarks in `tests/table/bench_table.cpp`

**Interfaces:**
- Consumes: Task 3's `PageRows`, `UnloadedRow`, `Page`; Task 1's `GenericPage`, `writeGenericPage(PageRows const&, DataSource const&, std::span<std::string const> columns)`, `readGenericPage(std::string_view, TableDescription const&)`; `TableQuery::columns`, `QueryLimits::maxColumns` and their `validate` rules (Task 1, A5).
- Produces:
  - `TableQuery::keys` (`std::optional<std::vector<RowId>>`, after `columns`) and `TableQuery::retry` (`bool retry = false`, after `keys`; Task 8a puts `distinct` before it, as the spec's block orders them) — decision 25
  - `QueryLimits::maxKeys = 1000`
  - `PageRows::indices`, `Page<Row>::indices` (`std::vector<std::int64_t>`, after `failed`)
  - `struct UnloadedLevels { std::span<std::string const> levels; std::span<std::string const> failed; };`
  - `struct detail::ComputedView { std::vector<std::uint32_t> rows; std::vector<std::uint32_t> undecided; };`
  - `[[nodiscard]] std::expected<detail::ComputedView, TableError> detail::computeView(DataSource const& source, TableQuery const& validated, ApplyOptions const& options);`
  - `using detail::SourceRowOf = std::function<std::optional<std::uint32_t>(RowId const&)>;`
  - `[[nodiscard]] PageRows detail::pageRows(detail::ComputedView const& view, RowSnapshot const& snapshot, TableQuery const& validated, QueryLimits const& limits, std::span<std::uint32_t const> viewRowOf = {}, detail::SourceRowOf const& sourceRowOf = {});` (`viewRowOf`: view row by source row, `UINT32_MAX` for absent; built on demand when empty. `sourceRowOf`: a key's source row; when empty, a keyed query scans the snapshot's row ids once. Task 6 passes the view cache entry's two lookups, so a keyed query over a cached view reads no row id it was not asked for)
  - `GenericPage::{std::vector<UnloadedRow> unloadedRows; std::int64_t undecided = 0; std::vector<std::string> awaiting; std::vector<std::string> failed; std::vector<std::int64_t> indices;}`

- [ ] **Step 1: Write the failing wire tests** in `tests/table/test_table_query.cpp`

```cpp
// Mutation: TableQuery's JSON leaving out `keys` (a glz::meta naming every member but `keys`).
TEST_CASE("table: columns and keys round-trip through glaze", "[table][query][keyed]") {
    TableQuery query;
    query.columns = std::vector<std::string>{"id", "name"};
    query.keys = std::vector<RowId>{RowId{std::int64_t{7}}, RowId{std::string{"A-1"}}};
    query.retry = true;
    std::string json;
    REQUIRE_FALSE(glz::write_json(query, json));
    TableQuery back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back.columns == query.columns);
    CHECK(back.keys == query.keys);
    CHECK(back.retry);
    TableQuery bare;
    REQUIRE_FALSE(glz::read_json(bare, R"({"sort":[],"filters":{},"page":{"offset":0,"limit":200}})"));
    CHECK_FALSE(bare.columns.has_value());
    CHECK_FALSE(bare.keys.has_value());
    CHECK_FALSE(bare.retry);
}
```

Add:
- `"table: validate refuses a keyed query out of bounds"`: `keys` with `page.offset = 10` → `InvalidSpec`; 1,001 keys → `LimitExceeded`; a text key of 257 bytes → `LimitExceeded`. Mutation: the keyed checks skipped.
- `"table: a keyed query to a table without keyed queries is unavailable"` (`test_table_description.cpp`): `detail::answerQuery` over a description with `capabilities.keys = false` answers a query with `keys` set `TableErrorCode::Unavailable`; the same table answers a page. Every other table answers keyed queries through `answerQuery`, so this is the one server-side form of the spec's "a model that does not translate keys"; the client's fallback to `invalidate` is `WindowRows`'s (Task 9). Mutation: `answerQuery` ignoring `capabilities.keys`.
- `"table: an empty columns list gives a page of keys"` (generic, `test_table_description.cpp`; skip if Task 1 already pins it): `detail::answerQuery` over a 3-row table with `columns = {}` returns `columns.empty()`, `keys == {0,1,2}`, every row an empty `GenericRow`. Mutation: an empty list read as absent.
- `"table: a keyed reply carries the view's totals and each row's view row"`:

```cpp
// Mutation: indices filled with positions in the reply instead of view rows.
TEST_CASE("table: a keyed reply carries the view's totals and each row's view row", "[table][query][keyed]") {
    using morph::table::Unloaded;
    auto source = tabletest::makeSource({tabletest::levelCol("a", ColumnKind::Integer, "slow")},
                                        {{Cell{std::int64_t{40}}}, {Cell{std::int64_t{10}}}, {Cell{Unloaded{}}},
                                         {Cell{std::int64_t{30}}}, {Cell{std::int64_t{-1}}}, {Cell{std::int64_t{20}}}});
    TableQuery query{.sort = {SortKey{.column = "a"}},
                     .filters = FilterSpec{.columns = {{"a", ColumnFilter{.include = {{.gt = "0"}}}}}}};
    query.keys = std::vector<RowId>{RowId{std::int64_t{3}}, RowId{std::int64_t{1}}, RowId{std::int64_t{2}},
                                    RowId{std::int64_t{4}}, RowId{std::int64_t{99}}};
    auto page = apply(*source, query);
    REQUIRE(page.has_value());
    CHECK(page->rows == std::vector<std::size_t>{1, 3});          // view order; 2 undecided, 4 rejected, 99 absent
    CHECK(page->indices == std::vector<std::int64_t>{0, 2});
    CHECK(page->total == 4);
    CHECK(page->undecided == 1);
    CHECK(page->totalKind == TotalKind::AtLeast);
}
```

- `"generic page: unloaded rows, undecided and indices round-trip"` (`test_table_description.cpp`): a `GenericPage` with `unloadedRows {{1, {"recipe"}, {"recipe"}}}`, `undecided 40`, `awaiting {"stock"}`, `failed {"recipe"}`, `indices {5, 9}` writes and reads back with each of the five members equal (`GenericPage` has no `operator==`; `unloadedRows` compares by `UnloadedRow`'s, A6); JSON lacking all five reads as a fully loaded page (members empty, `undecided == 0`). Mutation: the reader requiring `unloadedRows`.
- `"generic page: an unloaded cell is null and named per level"`: `writeGenericPage` over a 2-row source whose row 1 has `Unloaded` in recipe column `r` and named `columns {"id","r"}` writes `rows[1][1]` as `null` and `unloadedRows == {{1, {"recipe"}, {}}}`; a level whose columns are not named is not listed. Mutation: the writer emitting a sentinel string for `Unloaded`.

- [ ] **Step 2: Run to verify failure** — `cmake --build --preset cl-debug --target morph_table_tests`: compile errors naming `keys`, `indices`, `maxKeys`.

- [ ] **Step 3: Implement `validate`'s keyed rules** in `query.hpp`

(`columns` is validated by Task 1.) `keys`: `page.offset != 0` → `InvalidSpec` ("a keyed query has no page"); more than `maxKeys` → `LimitExceeded`; a text key longer than `maxTextLength` → `LimitExceeded`. (Task 8a adds "keys with `distinct`" → `InvalidSpec`.)

- [ ] **Step 4: Split `apply` and answer keyed queries**

`apply` = `validate` → `computeView` → `pageRows`. `pageRows` fills `total` (absent above `totalCap`), `totalKind`, `undecided`; for a non-keyed query `offset` and the page's rows as before; for a keyed one, `rows` = the source rows whose key is in `keys` and whose view row exists, ascending by view row, `indices` = those view rows, `offset = 0`. Keys are matched through `sourceRowOf` when it is given (one lookup a key); otherwise by scanning the snapshot's `rowId`s once against an `std::unordered_set<RowId>` of the requested keys, which is what `apply` does. The typed `apply` copies `indices`.

Add to Step 1 `"table: a keyed page uses the lookup it is given"` (`test_table_query.cpp`): `detail::pageRows` over the view of the case above, with a `sourceRowOf` built from an `std::unordered_map<RowId, std::uint32_t>` and a snapshot wrapper that counts `rowId` calls, returns the same `rows` and `indices` as without it and makes no `rowId` call. Mutation: `pageRows` ignoring `sourceRowOf`.

- [ ] **Step 5: Implement the generic page members and the keys capability**

`detail::answerQuery` refuses a query with `keys` set as `TableError{Unavailable, {}, "this table answers no keyed query"}` when `served.description.capabilities.keys` is false.

`writeGenericPage` copies `totalKind`, `undecided`, `awaiting`, `failed`, `indices` from `PageRows`, and lists row `i` in `unloadedRows` with the sorted levels of the named columns whose cell it wrote as `Unloaded` (Task 5's answer path replaces this with the level snapshots' states). The reader accepts every new member as optional.

- [ ] **Step 6: Run the target** — `ctest --test-dir build/cl-debug -R "^(table|generic page|description)" --output-on-failure`: all pass.

- [ ] **Step 7: Benchmarks** in `bench_table.cpp` (`"[.benchmark][table][wire]"`): W3 — bytes of the W1 page (200 rows, 40 named columns from the 1,000 × 300 fixture W1 is recorded on; positions 100 to 139 hold the recipe decimals `d000` to `d009`) with all 200 rows naming one unloaded level, minus the same page with `unloadedRows` cleared, as a share of W1; W4 — a keyed reply of 20 keys and 40 columns: bytes, encode, decode. Record into table.md's "Benchmarks: wire".

- [ ] **Step 8: Mutations, then commit**

```bash
git add include/morph/table tests/table
git commit -m "feat(table): partial pages and keyed queries"
```

---

### Task 5: The level cache (table.md step 2c, first half)

**Spec:** "The server level cache" (all thirteen rules and the layout, size and use paragraphs), "A table on the server" (level caches by record); Tests rows "A value is loaded once" (cache half), "Concurrent queries share a load", "Reloaded once after invalidation" (cache half), "A load in flight at invalidation", "Visible before told", "Eviction", "A level's layout is the cache's" (cache half), "A cache's size is deep", "A reader blocks no one", "Order uses are leased", "A failed load" (cache half); invariants 11, 12, 13, 14 (layout clause); gates L1–L8.

**Files:**
- Create: `include/morph/table/level_cache.hpp`; `tests/table/test_table_level_cache.cpp` (in `morph_table_tests`)
- Modify: `include/morph/core/change_types.hpp` (`effects::AnnounceSink`, `effects::IChangeObserver`: the cache derives from it, so it lands here rather than in Task 6)
- Modify: `include/morph/table/description.hpp` (`detail::answerQuery`, A1: level snapshots, `LeveledSource`, `want`, `unloadedRows`/`awaiting`/`failed` from snapshots)
- Modify: `tests/table/wide_table_model.{hpp,cpp}` (the slow level `recipe`), `tests/table/bench_table.cpp` (L1–L3, L6)

**Interfaces:**
- Consumes: Task 3's `Unloaded`, `levelsNeeded`; Task 4's `computeView`, `pageRows`, `GenericPage` members; Task 2's `ServedTable::levels` (`std::vector<std::shared_ptr<LevelSource>>`).
- Produces (in `morph::table`, `level_cache.hpp`): the spec's block verbatim — `LevelRead`, `LevelUse`, `LevelSnapshot`, `LevelSource`, `LevelCacheOptions`, `LevelCacheStats`, `LevelCache<Value>` with `Loader`, `SizeOf`, `Cells`, both constructors, `invalidate`, `invalidateAll`, `onLoadFailed`, `stats`, and `using CellLevelCache = LevelCache<std::vector<Cell>>;` — plus:
  - `[[nodiscard]] std::size_t cellBytes(Cell const& cell) noexcept;` (in `data_source.hpp`: `sizeof(Cell)` plus a string's heap capacity when it is not in the small buffer)
  - `struct LevelInput { std::shared_ptr<LevelSource> source; std::shared_ptr<LevelSnapshot const> snapshot; };`
  - `class LeveledSource final : public DataSource` — `LeveledSource(std::shared_ptr<DataSource const> base, std::vector<ColumnInfo> columns, std::vector<LevelInput> levels);` `columns()` the given columns; `snapshot()` reads a base-level column from `base` by id and a level column from its level's snapshot at the column's place in `LevelSource::columns()` (by id), `Unloaded` unless `read` answers `Loaded`; `subscribe`/`unsubscribe` do nothing (it is one request's immutable view); `[[nodiscard]] LevelRead state(std::size_t row, std::string_view level) const;`
- Produces (in `morph::effects`, `change_types.hpp`):
  - `using AnnounceSink = std::function<void(std::string_view record, std::optional<std::vector<glz::raw_json>> keys)>;`
  - `class IChangeObserver { public: virtual ~IChangeObserver() = default; virtual void changed(Changes const& changes) = 0; virtual void bind(AnnounceSink sink) = 0; };` (special members as the file's other interfaces declare them)
- Fixture: `WideTableOptions::recipePreloaded` (`double`, default `1.0`), `WideTableOptions::recipeBatch` (`std::size_t`, default `64`), `WideTableOptions::recipeFails` (`bool`, default `false`: the loader returns no value for any key, so every key it is given fails), `bench::WideTableModel::recipeGate()` returning a `bench::LoadGate&` with `hold()`, `release()` (every call from now on passes) and `release(std::size_t calls)` (that many loader calls pass, then it holds again), `loaderCalls() -> std::vector<std::vector<RowId>>`, and `recipeCache() -> std::shared_ptr<CellLevelCache>` (what a test `attach`es and reads `stats()` from). The fixture has no record besides its tables', so the cache's `invalidatedBy` is its `records`; a change of another record in `invalidatedBy` is tested on the cache alone (Step 1, "the observer invalidates by record"). The recipe level's columns are `d000`…`d009` and `t001`…`t010` (A3); the cache's `columns` list them in that order.

- [ ] **Step 1: Write the failing cache tests** in `tests/table/test_table_level_cache.cpp`

Shared helpers in the file's anonymous namespace, gathered in `struct Rig`: `tabletest::ManualExecutor loads;` (`ManualExecutor::runAll` runs only what was queued when it was called, and a batch's completion posts the next, so a test that means "until every load has run" calls `rig.drain()`, which is `tabletest::drain(loads)`); `rig.loggedKeys()` (every key the loader was given, sorted); a `CellLevelCache` over options `{.records = {"T.rows"}, .level = "recipe", .columns = {levelCol("r", Text, "recipe")}, .loads = &loads, .clock = [&] { return now; }}` and a loader that logs its keys and returns `{key, {Cell{"v" + text(key)}}}` per key; `keys(n)` gives `RowId{0}`…`RowId{n-1}`; `readCell(snapshot, key)`; an `AnnounceSink` that records `(record, keys)` and, when `readOnAnnounce` is set, reads the cache's snapshot for each key at call time.

```cpp
// Mutation: want not checking the cache (loads held keys again): loadedKeys 400.
TEST_CASE("level cache: a value is loaded once", "[table][levels]") {
    Rig rig;
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();  // all four batches
    REQUIRE(rig.cache.want(keys(200), LevelUse::Display));
    rig.drain();
    CHECK(rig.cache.stats().loadedKeys == 200);
    CHECK(rig.loggedKeys() == keys(200));  // each once, in any order: compared sorted
    CHECK(readCell(*rig.cache.snapshot(), RowId{std::int64_t{5}}) == Cell{std::string{"v5"}});
}
```

Add, each with the spec's mutation:
- `"level cache: a key wanted while it is queued is loaded once"`: `want(keys(200), Display)`, then `rig.loads.runAll()` once (one batch of 64 stored, the next posted), then `want(keys(200), Display)` again, then `rig.drain()`: `loadedKeys == 200` and `loggedKeys() == keys(200)`. Mutation: `want` queueing a key that is already queued (272 keys loaded).
- `"level cache: concurrent wants share one load"`: this case's `loads` is a one-thread pool, not the manual executor, so the loader can block on a latch off the test thread; 8 `std::thread`s call `want(keys(60), Display)`; after the latch is released and the pool is idle, `loaderCalls == 1`. Mutation: no in-flight set.
- `"level cache: invalidated keys are absent and load once more"`: 200 loaded; `invalidate(keys 0..9)`: those read `Unloaded`, the rest `Loaded`; two `want(keys(200))` load exactly keys 0–9, once; `invalidatedKeys == 10`. Mutation: `invalidate` leaving the values.
- `"level cache: the observer invalidates by record"`: `changed(Changes{.records = {{"T.rows", keys {"3"}}}})` makes key 3 absent and announces nothing; `{{"T.rows", nullopt}}` empties the level; `{{"Recipe", nullopt}}` with `invalidatedBy {"T.rows","Recipe"}` empties it and announces `("T.rows", nullopt)`; a record in neither list changes nothing. Mutation: `changed` ignoring the record.
- `"level cache: a load in flight at invalidation does not land"`: hold the loader on key 1; `invalidate({1})`; release returning the old value: key 1 reads `Unloaded`, `droppedLoads == 1`; the next `want({1})` calls the loader. Mutation: storing without the generation check.
- `"level cache: a want after an invalidation does not wait for the stale load"` (Review Focus 1): hold the loader on key 1; `invalidate({1})`; `want({1})` again before releasing: after release and drain the loader was called twice for key 1, the second call's value is stored, `droppedLoads == 1`. Mutation: "in flight" ignoring the generation, which leaves key 1 unloaded after the stale load is dropped.
- `"level cache: values are stored before they are announced"`: `readOnAnnounce`: every announced key reads `Loaded` at announce time; after `invalidateAll()` through `changed` with another record, the announce sink reads every key `Unloaded`. Mutation: announcing before storing.
- `"level cache: eviction drops the least recently used"`: `maxBytes` fitting 100 values; load 100, read keys 50–99 by `want`, load 50 more: keys 0–49 read `Unloaded`, 50–149 `Loaded`, `evictedKeys == 50`, the announce sink got only the loads' keys (no event for an eviction); `want(keys 0..9)` loads them again. Mutation: evicting the most recently used.
- `"level cache: a value's layout is the cache's"`: cache columns `{r1, r2, r3}`; `LeveledSource` A with description columns `{id, r3, r1}`, B with `{id, r2}`: A's row 0 reads `r3`'s value under `r3` and `r1`'s under `r1`; B reads `r2` under `r2`; one loader call served both. Mutation: reading the value by the caller's column order.
- `"level cache: size is deep"`: values of twenty text cells of 40 characters; `maxBytes = 100 * <their deep size>`; after loading 150 rows, `stats().bytes <= maxBytes` and `evictedKeys == 50`. Mutation: `SizeOf` as `sizeof(Value)`.
- `"level cache: a held snapshot blocks no store"`: hold `snapshot()`; on another thread `want` + drain, `invalidate`, `changed(...)` each complete within 1 s (`std::future::wait_for`); the held snapshot still reads its old values and its `generation()` differs from a new snapshot's. Mutation: a snapshot holding a shared lock.
- `"level cache: order uses are leased and the second is refused"`: `maxBytes` fitting 150 values, `expectedBytes` = one value's size; `want(keys 0..99, Order)` loads 100; `want(keys 100..199, Order)` returns `TableErrorCode::Unavailable`, `refusedOrders == 1`, loader calls unchanged; a `Display` load of 100 other keys evicts none of 0–99; advance the clock past `orderLease`: a further `Display` load evicts from 0–99. Mutation: no lease (the loader's call count grows without end over ten alternating rounds: assert `loaderCalls` after ten rounds equals that after one).
- `"level cache: leased values outgrowing the bound stop the order load"`: `expectedBytes` one tenth of the true size; `want(keys(100), Order)` is accepted; while loading, leased bytes exceed `maxBytes`: the remaining order batches are not loaded, the lease is dropped, `("T.rows", nullopt)` is announced, and the next `want(keys(100), Order)` is `Unavailable`. Mutation: the over-bound check skipped.
- `"level cache: a failed load is remembered"`: the loader returns no value for keys 0–9 of 20: those read `Failed`; `failedLoads == 10`; `onLoadFailed` got keys 0–9 once; the sink announced keys 0–9 once; `want(keys 0..9)` loads nothing until `retryAfter` (30 s) on the clock, then once; a second failure waits 60 s; `want(…, Display, true)` loads at once; `invalidate({3})` clears key 3's failure. A key the loader returns with an empty value reads `Loaded` with empty cells. Mutation: a failed key wanted again by every request. A loader that throws fails every key of the call.
- `"level cache: a level no cache serves is unloaded and awaited"` (in `tests/table/test_table_description.cpp`; spec failure mode "A filter on a level no cache serves"): a `ServedTable` whose description has column `r` at level `"recipe"` and whose `levels` holds one recording `LevelSource`, for the level `"stock"` and nothing for `"recipe"`, filtered `r contains "a"`: `answerQuery` returns no rows, `undecided` = every row, `awaiting == {"recipe"}`, and the recording source saw no `want`. Mutation: a level without a source read from the base source.
- `"table: change keys round-trip awkward keys"` (in `tests/table/test_table_query.cpp`; Review Focus 3): `changeKeys` then `rowIdOf` gives back `RowId{std::string{"a\"b\\c é"}}` and `RowId{std::int64_t{9'007'199'254'740'993}}` exactly; `rowIdOf` of `true`, `1.5` and `{}` is `nullopt`; `changeKeys` of `RowId{7}` and `RowId{"x"}` writes `7` and `"x"`. Mutation: writing an integer key through `double`.

- [ ] **Step 2: Run to verify failure** — `cmake --build --preset cl-debug --target morph_table_tests`: `level_cache.hpp` not found.

- [ ] **Step 3: Implement `level_cache.hpp`** (the classes as the Interfaces block; the spec's Doxygen on each member)

The decisions the rules and tests leave open:

- **Storage.** 256 shards by `std::hash<RowId>`; each `std::shared_ptr<Shard const>`, a `Shard` being an immutable `std::unordered_map<RowId, std::shared_ptr<Slot const>>`. A `Slot` holds the `Value` (or the failure: count and time), its deep bytes, and `mutable std::atomic<std::int64_t> lastUse` (a cache-wide use counter) and `mutable std::atomic<std::int64_t> leaseUntil` (clock ticks; 0 none). `snapshot()` copies the 256 pointers and a generation under the mutex; the mutex is held only to copy pointers or swap in changed shards. Every store, removal and observer call copies only the shards it touches and bumps the generation.
- **Generations.** A key's generation lives in `std::unordered_map<RowId, std::uint64_t>` beside the shards (under the same mutex); `invalidate` bumps each key's, `invalidateAll` bumps a level epoch. A load records `(epoch, key generation)` per key when it starts; the store keeps a key only when both are unchanged; others are dropped and counted in `droppedLoads`. A key is "in flight" only while a load holding its current generation runs, so a `want` after an invalidation starts a new load.
- **Queue and batches.** Two LIFO stacks of wanted keys, `Display` and `Order`: each `want` pushes its keys as one block, in the order given, on top of its stack, and a batch takes keys from the top block in that order (the newest want first, each want in its own order). One loader call runs at a time per cache: when none runs, the next batch (up to `batch` keys, `Display` stack first) is posted to `loads`; its completion stores, announces, and posts the next. Keys held, in flight, already queued, or failed inside their delay are not queued again: a queued key wanted again moves to the top block of its stack (an `Order` key wanted for `Display` moves to the `Display` stack), so a key is in at most one block and is handed to the loader once.
- **Order fit.** `want(keys, Order)` computes `keys.size() × mean` (mean = stored bytes / stored values; `expectedBytes` while none; 64 × columns when that is 0) plus the bytes of slots leased by other keys; over `maxBytes` it returns `Unavailable` and changes nothing. Otherwise every key's lease becomes `now + orderLease` (a key still loading receives it when stored).
- **Eviction.** After a store, while `bytes > maxBytes`, evict the slots with the smallest `lastUse` whose lease is past; a `Display` value stored with nothing evictable is stored with `lastUse = 0` so it goes first. While leased bytes exceed `maxBytes`, clear the `Order` stack, set every lease to 0, and announce each of `records` without keys.
- **Failure delay.** `retryAfter × 2^(failures − 1)`, capped at `retryMax`, read from `clock` when `want` sees the key.
- **Announce.** After a store, the stored keys (written with `changeKeys` — this task adds `[[nodiscard]] std::vector<glz::raw_json> changeKeys(std::span<RowId const> keys);` and `[[nodiscard]] std::optional<RowId> rowIdOf(glz::raw_json const& key);` to `query.hpp`: an integer key is a JSON number, a text key a JSON string, anything else `nullopt`) go to the sink once per record of `records`; failed keys likewise, once.
- **Observer.** `changed` runs on the server strand and only removes: for each change whose record is in `invalidatedBy` — a record of `records` with keys invalidates those keys (unreadable keys invalidate the level), without keys the level; any other record invalidates the level and announces each of `records` without keys.
- `stats()` reads atomics; `bytes` is the deep total.

- [ ] **Step 4: Implement `detail::answerQuery` with levels** (A1) in `description.hpp`

For a validated query: `levelsNeeded`; one snapshot per needed level (`ServedTable::levels` by `level()`); a `LeveledSource` over Task 2's `detail::PinnedSource` (the one source snapshot the reply reads), the description's columns and those snapshots; `computeView` and `pageRows` over it; the writer over the reply's rows and named columns; `unloadedRows[i].levels` = the needed levels whose `state(row, level)` is not `Loaded`, `failed` = those `Failed`; `PageRows::awaiting` = the `order` levels with an `Unloaded` state for some row of the view or the undecided set, `failed` = those with a `Failed` one. Then `want`, every call with `retry = query.retry` (decision 25): each `display` level with the reply's keys (`Display`), each `order` level with the keys of the view and the undecided rows (`Order`); a needed level no entry of `served.levels` serves reads `Unloaded` for every row, counts as `awaiting` when it is an `order` level, and is wanted from no one; an `Unavailable` from an `Order` want refuses the query with `TableError{Unavailable, <a sorted or filtered column of that level>, …}` — the want happens before the reply is returned, so a refused order use returns no page.

- [ ] **Step 5: Add the fixture's slow level** in `wide_table_model.{hpp,cpp}`: one `CellLevelCache` shared by `wide` and `small` (`records` both; `columns` = `d000`…`d009`, `t001`…`t010` from `wideColumns(300)`; `batch` = `WideTableOptions::recipeBatch`), loader over the generator's recipe cells, sleeping `loadDelay` per key or waiting on `recipeGate()`, and returning no value at all when `recipeFails`; `recipePreloaded` of the rows stored when the cache is built (rows chosen by `(seed, row)`, so the same seed preloads the same rows); `invalidatedBy` left at its default, `records`. Each of the three table functions (`scoped` included, whose record is not in `records`: its arrivals are not announced, and no test watches it) returns `ServedTable::levels = {the cache}`; the columns' `info.level` and the descriptions' `levels = {"recipe"}` are Task 3a's (A3). `configure` rebuilds the cache with the sources. The default `recipePreloaded = 1.0` keeps every Part A page test (which reads recipe values) unchanged; the level tests and benchmarks set `0` or `0.7` explicitly.

- [ ] **Step 6: Run the target** — `ctest --test-dir build/cl-debug -R "^(level cache|table|generic page)" --output-on-failure`: all pass.

- [ ] **Step 7: Benchmarks** (`"[.benchmark][levels]"`, loader sleeping a fixed time per key): L1 (snapshot + 200 reads; 100,000 reads), L2 (bytes per row at twenty columns, and for 100,000 rows), L3 (`invalidate` of 1,000 keys; the observer call for a 4,096-key change; both while another thread holds a snapshot 40 ms — gate case `"level cache: invalidation does not wait for a reader"` asserts each returns before the reader releases), L6 (loader return to announce per batch of 64 at 100,000 held rows; gate case asserts shards copied ≤ keys stored, through a test-only counter `LevelCacheStats` does not expose — read it with a `detail::` accessor). L4, L5, L7, L8 are the gate cases of Step 1 ("concurrent wants", "a value is loaded once", "order uses", "a failed load"); L8's ten page requests inside `retryAfter` are asserted in Task 6's bridge case. Record into table.md's "Benchmarks: server level cache".

- [ ] **Step 8: Mutations, then commit**

```bash
git add include/morph/table include/morph/core/change_types.hpp tests/table tests/CMakeLists.txt
git commit -m "feat(table): the level cache"
```

---

### Task 6: Announce, attach and the view cache (table.md step 2c, second half)

**Spec:** "Arrival" steps 1–3, "The view cache", "A model's tables" (row scope), "The server level cache" (how the server learns of a cache); Tests rows "`announce`", "A cached view is the view", "One sort for many watchers", "No value crosses a scope", "A row-scoped table", "A value is loaded once" (two clients), "Reloaded once after invalidation" (through the server), "A level's layout is the cache's" (bridge), "A failed load" (bridge), (a) bridge, (c) bridge; invariants 13, 14, 29; gates S7, S9, C1–C3.

**Files:**
- Create: `include/morph/table/view_cache.hpp`; `tests/table/test_table_view_cache.cpp` (in `morph_table_tests`); `tests/net/test_push_unloaded_rows.cpp` (in `morph_net_tests`, which links `morph_wide_table_model`)
- Modify: `include/morph/core/remote.hpp` (`RemoteServer::announce`, `attach`, `viewStats`, the observer call in `fanOut`, `ServerConfig::views`, the server's `ViewCache` installed in its `TableRuntime`). It still includes no table header but `table/runtime.hpp` (Part A's rule)
- Modify: `include/morph/table/runtime.hpp` (`ViewCacheOptions`, `ViewCacheStats`, the forward declaration `class ViewCache;`, `TableRuntime::views`, `makeViewCache`, `viewCacheStats`), `src/ui/process_state.cpp` (the two functions' definitions, beside `currentTableRuntime()`; it includes `view_cache.hpp`)
- Modify: `include/morph/table/data_source.hpp` (`RowSnapshot::identity()`), `include/morph/table/query.hpp` (`RowsSnapshot::identity()`)
- Modify: `include/morph/table/description.hpp` (`AnswerContext::{views, record, rowScoped}`; `detail::answerQuery` through the view cache; `rowScoped` with an empty `scope`; `QueryTable::invoke` filling all three)
- Test: `tests/net/test_push_tables.cpp`
- Docs: `docs/spec/core/backend.md` "Fan-out" (`tableWork`, `announce`, `attach`, `IChangeObserver`, the view cache)

**Interfaces:**
- Consumes: Task 5's `LevelCache`, `LeveledSource`, `IChangeObserver`, `AnnounceSink`, `changeKeys`, `rowIdOf`; Task 4's `computeView`, `pageRows`; A1–A4.
- Produces:
  - `std::expected<void, std::string> RemoteServer::announce(std::string_view record, std::optional<std::vector<glz::raw_json>> keys);`
  - `void RemoteServer::attach(std::shared_ptr<effects::IChangeObserver> observer);`
  - `ServerConfig::views` (`table::ViewCacheOptions`)
  - `[[nodiscard]] table::ViewCacheStats RemoteServer::viewStats() const;` (the server's one view cache; callable from any thread)
  - In `table/runtime.hpp`, which stays free of the other table headers (it adds `<cstddef>`, `<functional>`, `<memory>`): `struct ViewCacheOptions { std::size_t maxBytes = 64U << 20U; std::function<void()> onCompute; };` (`onCompute`: called on the computing thread at the start of each computation; empty by default; a test records `std::this_thread::get_id()` in it), `struct ViewCacheStats { std::size_t hits, misses, computations, waits, evictions, invalidations, bytes; };`, `class ViewCache;`, `[[nodiscard]] MORPH_API std::shared_ptr<ViewCache> makeViewCache(ViewCacheOptions options);` and `[[nodiscard]] MORPH_API ViewCacheStats viewCacheStats(ViewCache const& cache);`. `RemoteServer` holds the `std::shared_ptr<ViewCache>` it got from `makeViewCache(config.views)` and never needs the complete type; `view_cache.hpp` includes `runtime.hpp` and defines the class
  - `[[nodiscard]] virtual void const* RowSnapshot::identity() const { return this; }` — two snapshots with one identity hold the same rows. `RowsSnapshot<Row>` without a `LevelLoaded` returns its rows vector's address, so the `RowsSource` that `detail::serve` builds on every call of a typed table has one identity while the model returns the same `std::shared_ptr<std::vector<Row> const>`; with a predicate it returns `this`. A snapshot that wraps another (the scoped source's, `LeveledSource`'s) does not forward it
  - `struct ViewKey { std::string record; std::string scope; std::string query; };` (`query`: the canonical JSON of the validated `sort` and `filters` only)
  - `struct ViewComputation { detail::ComputedView view; std::vector<std::string> awaiting; std::vector<std::string> failed; };` (what `compute` returns; the cache builds the entry from it)
  - `struct ViewEntry { std::shared_ptr<RowSnapshot const> snapshot; std::vector<std::pair<std::string, std::uint64_t>> levels; detail::ComputedView view; std::vector<std::string> awaiting; std::vector<std::string> failed; [[nodiscard]] std::span<std::uint32_t const> viewRowOf() const; [[nodiscard]] std::optional<std::uint32_t> sourceRowOf(RowId const& key) const; [[nodiscard]] std::size_t bytes() const; };` (`viewRowOf` and the key index are built once, at the first keyed query, under a `std::once_flag`; held only by `std::shared_ptr`, never copied or moved)
  - `class ViewCache { public: explicit ViewCache(ViewCacheOptions options = {}); [[nodiscard]] std::expected<std::shared_ptr<ViewEntry const>, TableError> find(ViewKey const& key, std::shared_ptr<RowSnapshot const> const& snapshot, std::span<std::pair<std::string, std::uint64_t> const> levels, std::function<std::expected<ViewComputation, TableError>()> const& compute); [[nodiscard]] ViewCacheStats stats() const; };`
  - `struct AnswerContext { ViewCache* views = nullptr; std::string_view record; bool rowScoped = false; };` (decision 48: `record` is the table's record name, `<model type>.<id>`, the first member of the `ViewKey`; `rowScoped` is the registration's option. `QueryTable<F>::invoke` fills them from `TableTraits<F>` and the record's `RecordInfo::privateKeys`; a caller that passes neither, as the direct tests of Tasks 2–5 do, gets `record` empty and no row-scope check) and `TableRuntime::views` (`ViewCache*`; null computes every time)

- [ ] **Step 1: Write the failing view cache tests** in `tests/table/test_table_view_cache.cpp`

- `"view cache: a cached view is the view"` (invariant 29): for seeds 0–199, `std::mt19937{seed}` builds a 300-row table of 4 columns (`Integer`, `Text` base; `Decimal`, `Text` at level `"recipe"` read through a `CellLevelCache` holding 70% of rows), a sort of 1–2 random keys and a random filter of 1–2 entries; through one `ViewCache` entry the test builds page 0, a keyed reply of 10 random keys and page 1, and `CHECK`s each `PageRows` equal (`rows`, `total`, `totalKind`, `undecided`, `indices`, `offset`) to `apply`'s over the same `LeveledSource`. Its second half stores into the sorted level and builds page 0 again, which must equal `apply`'s over the new snapshot. Mutation: an entry valid whatever its order levels' generations, which serves the old order.

Add:
- `"view cache: an arrival on an order level computes again; on a display level it does not"`: one entry sorted on recipe column `r1`; a store into the level (generation changes) → the next `find` recomputes (`computations == 2`); with the sort on a base column and only display level `r2` changing → `computations == 1`, `hits == 1`. Mutation: as the case above.
- `"view cache: a request on a newer snapshot does not join an older computation"`: request A `find`s key K with snapshot S1 and its `compute` blocks on a latch; request B `find`s K with snapshot S2 (one `updateRow` later): B computes on its own (`computations == 2`, `waits == 0`) and its entry's `view` is the one over S2; after A is released, a third `find` with S2 hits B's entry. The same holds for a different order-level generation with the same snapshot. Mutation: single flight joined on the key alone, which hands B the view over S1.
- `"view cache: fifty requests compute once"` (S7/C1 shape): 50 threads `find` the same key while `compute` waits on a latch: `computations == 1`, `waits == 49`, every result the same entry. Mutation: no single flight.
- `"view cache: the bound evicts least recently used"`: `maxBytes` fitting two entries of 10,000 rows; three keys → the first evicted, `bytes <= maxBytes`. Mutation: no bound.
- `"view cache: a keyed fetch sorts nothing"` (S9 gate): after page 0, a keyed query of 20 keys: `computations` unchanged. Mutation: a keyed query bypassing the cache.
- `"view cache: a typed table's view is cached across calls"` (`test_table_description.cpp`): a typed table whose function returns the same rows pointer each call; two `detail::answerQuery` calls, each over a fresh `detail::serve<&TypedModel::rows>(model, ctx)` (pinned), with one `ViewCache` and `record = "TypedModel.rows"` in the context: `computations == 1`, `hits == 1`; after the model replaces its rows vector, a third call computes again (`computations == 2`). Mutation: `find` comparing snapshot pointers instead of `identity()` (three computations).
- `"view cache: a row-scoped table needs a scope"` (`test_table_description.cpp`): `answerQuery` with `AnswerContext{.rowScoped = true}` over a `ServedTable` whose `scope` is empty answers `SourceContract`; with `scope = "p1"` it answers a page; with `rowScoped = false` and an empty scope it answers a page. Mutation: the check reading `served.scope` alone (the third case is refused).

- [ ] **Step 2: Write the failing bridge tests** in `tests/net/test_push_unloaded_rows.cpp`

The file's rig (anonymous namespace), built as `tests/net/test_push_two_clients.cpp` builds its server, socket and client sessions: a `RemoteServer` over a 4-thread pool with `ServerConfig{.tableWork = <its own pool>}` serving `bench::WideTableModel` (`small`: 1,000 × 30, `recipePreloaded = 0`, recipe gate held), the model's level cache `attach`ed; one or more clients, each a `ui::Session` with a watcher and a `CountingChannel` (a `ui::ModelChannel` decorator logging `(actionId, bodyJson)` of every `execute`). Records are `"WideTableModel.small"` and `"WideTableModel.wide"` (A3). The row-scope cases use a test model `UrModel` in the same file: table `rows`, registered `table::rowScoped`, whose function returns `table::scoped(<a shared 1,000-row VectorSource with a recipe column `r`>, <the principal's keys>)` with `scope` = the principal, principal `p1` seeing rows 0–499 and `p2` rows 250–749, and one `CellLevelCache` for `r` shared by both scopes; a second registration `UrEmptyScope` is the same with `scope` left empty.

```cpp
// Mutation: announce bypassing the derived-record walk.
TEST_CASE("announce: a derived record is included, many keys are whole, an unregistered record is refused",
          "[net][push][levels]") {
    // A record D registered derivedFrom the table record; a client watching D.
    // announce(small, 3 keys) → the client's event names `small` with the 3 keys and D whole-type.
    // announce(small, 4,097 keys) → `small` whole-type.
    // announce("NoSuchRecord", {}) → unexpected, and no event reaches any client within 200 ms.
}
```

Add:
- `"table over the bridge: the first page arrives before the level"` (row (a) bridge): with the gate held forever, `Table.small` sorted on `id`, `columns` = `id`, `d000` and `t001`: the reply arrives; `unloadedRows` names indices 0–199 (keys 1–200) with `{"recipe"}`; every recipe cell is `null`. Mutation: the answer path waiting for its loader.
- `"table over the bridge: a level loads only for what a query needs"` (row (c) bridge): a query naming no recipe column → `loaderCalls()` empty; naming one → the logged keys are exactly the page's 200; sorting on one → every candidate row (1,000). Mutation: `levelsNeeded` returning every level.
- `"table over the bridge: two clients load a value once"`: two clients ask the same page naming a recipe column; release the gate; `loadedKeys == 200` and each key once in the log. Mutation: `want` not checking the cache (800).
- `"table over the bridge: an updates effect invalidates without a call"`: after the level loaded, `EditWideRow` on the row keyed 5 (declares `updatesTable` on `wide` and `small`): the next query names that row unloaded and wants it; the loader loads key 5 once. Mutation: `fanOut` not handing the change to observers.
- `"table over the bridge: no value crosses a scope"`: `UrModel`'s `rows` for `p1` and `p2`, each naming `r`: each reply holds only its scope's rows; after release the loader was called once for each of rows 250–499. Mutation: a view shared across scopes (an empty `scope` in the key).
- `"table over the bridge: a row-scoped table announces without keys"`: an arrival for `UrModel.rows` reaches `p1`'s watch with `keys` absent; no event `p1` receives names a key outside 0–499; `UrEmptyScope`'s first query answers `SourceContract`. Mutation: the record registered without `privateKeys`.
- `"table over the bridge: a level's layout is the cache's"` (`test_push_tables.cpp`): `WideTableModel.scoped` for `"extended"` (recipe columns `d000`…`d005`, `t001`…`t006`) and for a default principal such as `"alice"` (`t001`…`t006` only), each naming every recipe column it has, with `recipePreloaded = 0` and the gate released. The first reply of each has its recipe cells `null` (a reply never waits for a load) and wants the page's keys; the test waits until `recipeCache()->stats().loadedKeys` reaches the page's 200 (polling the counter with a deadline, since `scoped`'s record is not announced), then asks each page again: the second replies' recipe cells equal the generator's values under their own column ids, and each key is in `loaderCalls()` once for both principals. Mutation: reading the value by the caller's column order (alice's `t001` then holds `d000`'s value).
- `"table over the bridge: failed keys are not loaded again inside the delay"` (L8): `small` configured with `recipePreloaded = 0` and `recipeFails = true`, sorted on `d000` (so `recipe` is an order level; `Page::failed` names order levels only, spec "Partial pages"): once the 1,000 order keys have failed, ten page requests cause zero further loader calls, and each reply has `awaiting` empty, `failed == {"recipe"}`, and every `unloadedRows[i].failed == {"recipe"}`; the failed keys were announced once; then one request with `retry: true` calls the loader for each of the 1,000 keys once, in `ceil(1000 / 64) = 16` calls. Mutation: a failed key wanted again by every request.
- `"table over the bridge: one sort for many watchers"` (C1, C2, gate parts, `test_push_tables.cpp`): fifty clients watching one sorted view of `wide` refetch after one announcement: `server->viewStats().computations` rises by one; each computation ran on a `tableWork` thread: the rig's `ServerConfig::views.onCompute` records `std::this_thread::get_id()` under a mutex, and every recorded id is one of the `tableWork` pool's (which the rig collects by posting a task to each of its threads). Mutations: no single flight (fifty computations); `QueryTable::invoke` running `answerQuery` inline (the recorded id is a strand thread's).
- C3 gate case `"table over the bridge: fifty sorts stay inside the bound"`: fifty clients with fifty different sorts: `server->viewStats().bytes <= maxBytes` and `evictions > 0` with `ServerConfig::views.maxBytes` set to fit ten views of `wide`. Mutation: no bound.

- [ ] **Step 3: Run to verify failure** — `cmake --build --preset cl-debug --target morph_table_tests` and, on Linux, `cmake --build --preset linux-everything --target morph_net_tests`: `view_cache.hpp` not found; `announce`, `attach` not members.

- [ ] **Step 4: Implement `view_cache.hpp`**

Wherever this step says two snapshots are the same, it means their `identity()` is equal; an entry holds its snapshot, so the address an identity names cannot be reused while the entry lives. `options.onCompute`, when set, is called first on the thread that computes.

`find`: under a mutex, look the key up; an entry whose `snapshot` is the same as the argument and whose every `levels` generation equals the arguments is a hit (its recency bumped); a stale entry is dropped (`invalidations`). On a miss, a request joins a running computation only when that computation's key, snapshot and every level generation equal its own, and then waits on its `std::shared_future` (`waits`); otherwise it computes inline, outside the mutex, and publishes through its own `std::shared_future` (running computations are kept per `(key, snapshot, generations)`). The cache builds the `ViewEntry` from the `ViewComputation`, the snapshot and the generations, in a `std::shared_ptr`; when two computations of one key finish, the entry whose snapshot and generations a later request matches is the one it finds (an insert replaces the key's entry unless that entry's computation started later, so the computation over the older snapshot, finishing last, does not displace the newer entry). A failed computation is not cached. Over `maxBytes` after an insert, least recently used entries go. `ViewEntry::bytes()` is `4 × (view + undecided)` plus, once built, `4 × source rows` for `viewRowOf` and 12 bytes a row for the key index (a vector of `(hash, source row)` sorted by hash). The cache reads `bytes()` again on every hit, so an entry that built its lookups since it was inserted is counted at its new size and the bound is applied then.

- [ ] **Step 5: Implement `detail::answerQuery` through the cache** (A1)

The `ViewKey` is `{context.record, served.scope, canonical JSON of validated sort and filters}`; with `context.views` null or `context.record` empty the view is computed for the request and not cached. The snapshot passed is the pinned source's (the base rows, whose identity survives across requests), not the `LeveledSource`'s; the levels passed are the `order` levels with their snapshots' `generation()`. `compute` runs `computeView` over the `LeveledSource` and fills `awaiting`/`failed`. `pageRows` takes the entry's view and, for a keyed query, its `viewRowOf()` and its `sourceRowOf` (Task 4's two trailing parameters), so a keyed key is one lookup. `retain` is not in the key (Task 17b merges it per request). When `context.rowScoped` is set and `served.scope` is empty, `answerQuery` answers `SourceContract` ("a row-scoped table needs a scope") before it reads anything.

- [ ] **Step 6: Implement `announce` and `attach`** in `remote.hpp`

- `announce`: refuses (`"unregistered record: <name>"`) when `effects::detail::resolved().recordNamed(record)` is null; otherwise builds `Changes` with `addRecordChange` and `addDerivedChanges`, applies the `kMaxChangeKeys` rule, and posts to the strand a fan-out with no caller (`fanOut(changes, 0, {})`'s path) that does not call the observers. Callable from any thread.
- `attach`: stores the observer and calls `observer->bind(sink)`, `sink` capturing `std::weak_ptr<RemoteServer>` and calling `announce` when the server is alive, nothing otherwise.
- `fanOut` (an execute's or a time fact's changes) calls each observer's `changed(changes)` first, on the strand, then queues events; an announce reaches the private fan-out step that skips observers.
- `ServerConfig::views` sizes the server's one `ViewCache`, made with `table::makeViewCache(config.views)` at construction and held by `std::shared_ptr` (the type stays incomplete in `remote.hpp`); `viewStats()` returns `table::viewCacheStats(*_views)`. The server puts the pointer in the `TableRuntime` it installs around a dispatch (A2), and `QueryTable<F>::invoke` passes `AnswerContext{.views = runtime->views, .record = <the table's record name>, .rowScoped = <the record's privateKeys>}` (`views` null when no runtime is installed, as under `LocalBackend`; `record` and `rowScoped` are filled either way).

- [ ] **Step 7: Run the targets** — `ctest --test-dir build/cl-debug -R "^(view cache|table)" --output-on-failure` and `ctest --test-dir build/linux-everything -R "^(table over the bridge|announce|tables)" --output-on-failure`: all pass.

- [ ] **Step 8: Benchmarks** — S7 (ten page requests of one sorted view: requests two to ten), S9 (a keyed query of 20 keys: first and next) in `bench_table.cpp`; C1–C3 timings (strand's longest task, views computed per second) in `test_push_unloaded_rows.cpp` under `"[.benchmark][net]"`. Record into table.md.

- [ ] **Step 9: Update `docs/spec/core/backend.md` "Fan-out"** (`ServerConfig::tableWork`, `views`; `announce`; `attach` and the observer call before events). Docs build green.

- [ ] **Step 10: Mutations, then commit**

```bash
git add include/morph/table include/morph/core src/ui/process_state.cpp tests docs/spec/core/backend.md
git commit -m "feat(net): announce arrivals and cache views"
```

---

### Task 7: The window under changes (table.md step 2d)

Two commits: **7a** `PageWindow`; **7b** the session's read and watch, the query cache's open, partial and opaque entries, and the bridge's end-to-end case.

**Spec:** "The window under changes" (all), "Partial pages" (the window's members), "The client's caches", "Which client cache a screen uses", "Arrival" steps 4–6; Tests rows "An event that precedes its page reply", "An ordinary edit on the same record", "Invalidation faster than a round trip", "A reply holds the named columns only" (window half), "A row the window never held", "An open entry hears every change", "A partial entry is not merged into", "An opaque entry", "A read with no entry", (b) window, (d) window and bridge; invariants 8, 9, 10, 30; gates K1–K4, K6, K7, E1–E4.

**Files (7a):**
- Modify: `include/morph/table/query.hpp` (`PageFetch::{columns, revision}`; `RowsFetch`; `Refresh`; `RefreshPlan`; `detail::WindowCore`; `PageWindow<Row>`; the declaration `template <> class PageWindow<std::vector<Cell>>;`)
- Modify: `include/morph/table/description.hpp` (the definition of `PageWindow<GenericRow>`)
- Test: `tests/table/test_table_server.cpp`; `tests/table/bench_table.cpp` (K1–K3)

**Files (7b):**
- Modify: `include/morph/ui/interpreter.hpp`, `src/ui/interpreter.cpp` (`CallOptions::stop`; `Session::read`, `Session::watchRecord`, `WatchHolder`; open, partial and opaque entries)
- Modify: `include/morph/ui/document.hpp` (`ActionRecordInfo::{open, partial, opaque}`), `include/morph/ui/author.hpp` (copies them), `include/morph/core/effects.hpp` (`ActionRecord::{open, partial}`; `opaque` is Task 2's, A4), `include/morph/core/detail/result_walker.hpp` (holder members), `include/morph/table/query.hpp` (`Page::openMembers`, `Page::partialMember`)
- Modify: `include/morph/ui/value.hpp` (`detail::tableValueFromJson`, decision 46)
- Modify: `tests/ui_interpreter_support.hpp` (`Harness(ui::Catalog catalog, bool recordWatches = false)`: the catalog is a member the session's options and `mount`'s `loadOrFail` both read, in place of their two calls of `corpusCatalog()`; the existing constructors pass `corpusCatalog()`)
- Create: `tests/test_ui_interpreter_cache.cpp` (in `morph_tests`)
- Test: `tests/net/test_push_unloaded_rows.cpp`
- Docs: `docs/spec/core/registry.md` (`openMembers`, `partialMember`, an opaque holder), `docs/spec/ui/interpreter.md` "Queries and the cache" (open, partial, opaque entries; `Session::read`, `watchRecord`)

**Interfaces:**
- Consumes: Task 4's `UnloadedRow`, `UnloadedLevels`, `GenericPage`, keyed replies; Task 6's `changeKeys`/`rowIdOf` (Task 5), `announce`.
- Produces (7a), in `query.hpp` unless noted:
  - `struct PageFetch { PageRequest request; std::vector<std::string> columns; std::uint64_t query = 0; std::uint64_t revision = 0; bool operator==(PageFetch const&) const = default; };`
  - `struct RowsFetch { std::vector<RowId> keys; std::vector<std::string> columns; std::uint64_t query = 0; std::uint64_t sequence = 0; };`
  - `enum class Refresh : std::uint8_t { Nothing, Rows, Pages };` `struct RefreshPlan { Refresh kind = Refresh::Nothing; RowsFetch rows; };`
  - `PageWindow<Row>`: `explicit PageWindow(std::int64_t pageSize = 200, std::size_t maxPages = 8, RowKey<Row> key = {}, std::size_t maxKeys = 1000, std::size_t maxColumns = 96);` `std::vector<PageFetch> visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns = {});` `std::vector<PageFetch> superseded() const;` `void cancelled(PageFetch const& fetch);` `RefreshPlan refresh(std::span<RowId const> keys);` `RefreshPlan owed();` `void invalidate();` `bool accept(PageFetch const& fetch, Page<Row> reply);` (keeps the existing `bool`: `false` when ignored) `std::vector<std::int64_t> patch(RowsFetch const& fetch, Page<Row> reply);` `UnloadedLevels unloadedLevels(std::int64_t index) const;` `std::int64_t undecided() const noexcept;` `std::span<std::string const> awaiting() const noexcept;` `std::span<std::string const> failed() const noexcept;` `std::uint64_t revision() const noexcept;` and the existing `fail`, `reset`, `query`, `row`, `total`, `totalKind`, `error`, `pagesHeld`, `pending`
  - `PageWindow<GenericRow>` (in `description.hpp`): the same members with `GenericPage` in place of `Page<Row>` and no key function parameter (`explicit PageWindow(std::int64_t pageSize = 200, std::size_t maxPages = 8, std::size_t maxKeys = 1000, std::size_t maxColumns = 96);`), and in place of `row`: `RowId const* key(std::int64_t index) const;` `Cell const* cell(std::int64_t index, std::string_view column) const;` (null when the row or the column is not held) `bool holds(std::int64_t index, std::string_view column) const;`
- Produces (7b):
  - `CallOptions::stop` (`::core::async::StopToken`; a stop sends the wire's `cancel` where the server takes one)
  - `async::Completion<std::string> Session::read(std::shared_ptr<ModelChannel> const& channel, std::string const& actionId, Value const& body, ::core::async::StopToken stop = {});` — resolves with the reply's JSON text, not a `Value`: a page's Number cells are JSON numbers with fractions, which `ui::valueFromJson` refuses, so the caller decodes the text itself (`table::readGenericPage`, Task 11c's `readTypedPage`). table.md declares the same signature (index decision 30).
  - `class WatchHolder` (decision 47: a copyable handle over a `std::shared_ptr` to one registration; the record is counted out and the handler removed when the last copy dies. `Completion<T>` hands its value to a handler as `const T&`, `core/completion.hpp`, so a `then` handler keeps the watch by copying the holder; a default-constructed holder holds nothing. The registration holds the session weakly, so a holder that outlives its session releases nothing)
  - `Session::settleReply`'s table-aware decode for an opaque entry (decision 46), through `[[nodiscard]] std::expected<Value, std::string> ui::detail::tableValueFromJson(Json const& json);` in `value.hpp`
  - `async::Completion<WatchHolder> Session::watchRecord(std::string const& record, std::function<void(effects::RecordChange const&)> handler);`
  - `effects::ActionRecord::{std::vector<std::string> open; std::vector<std::string> partial;}` beside Task 2's `opaque`, and `ui::ActionRecordInfo::{open, partial, opaque}` (all in `operator==`)
  - `static constexpr std::array<std::string_view, 2> Page<Row>::openMembers() noexcept { return {"awaiting", "distinct"}; }` and `static constexpr std::string_view Page<Row>::partialMember() noexcept { return "unloadedRows"; }` (`distinct` exists once Task 8a lands; until then the walk skips a named member the holder lacks)

#### 7a — `PageWindow`

The rules no test alone fixes, which both specialisations share through `detail::WindowCore` (all bookkeeping that does not touch a row's content: pages, keys per row, unloaded levels per row, staleness, revisions, fetches in flight, totals, sequences):

- **State.** `_query` (`reset` advances), `_revision` (`invalidate` advances), per held page `{keys, unloaded levels per row, stale, complete, columns held (generic) with the visible-call stamp of each}`, at most one fetch in flight per page (the window remembers it, so `accept` knows whether it was a whole-page or a column fetch), totals `{total, totalKind, undecided, awaiting, failed}` with `_totalsRevision`, `_sequence`, `_latestAsked: RowId → sequence` (held keys only; a key is forgotten when it leaves the held pages, so it never outgrows the held rows), `_latestKeyed` (the sequence of the latest `RowsFetch`), `_owed` (keys recorded while a page fetch is in flight).
- **`visible(first, count, columns)`** covers pages `first/pageSize − 1` to `(first + max(count,1) − 1)/pageSize + 1`, skipping pages past an exact total and pages with a fetch in flight. Not held, or held stale → a whole-page fetch `{offset p·size, limit size}` with `columns`, tagged `revision = _revision`. Held, current and short (a server cut it) → the rest of the page, with the columns held. Held, current, complete but lacking some of `columns` (generic only) → a column fetch of the same rows (`{p·size, rows held}`) naming only the missing columns, in the order given. Each returned fetch is recorded in flight. A typed window ignores `columns` and always fetches `{}`.
- **`accept`.** Ignored (`false`) when `fetch.query != _query` or the fetch is not the one in flight for its page; the in-flight record is removed either way when it matches. A column fetch: when the reply's `keys` equal the page's row for row, its cells are added (each row's unloaded levels become the union of held and reply levels) and over `maxColumns` the columns with the oldest visible-call stamp go; otherwise `invalidate()`. A whole-page fetch: the page's rows, keys and unloaded levels are replaced (a rest-of-page fetch appends as today) and `stale = fetch.revision != _revision`. With at most one fetch in flight per page and a reply accepted only as that fetch, a reply older than the held content cannot arrive: the spec's "a reply older than the held content is ignored" holds by construction, and the window keeps no content revision. A page reply with `fetch.revision >= _totalsRevision` sets `undecided`, `awaiting`, `failed` and `_totalsRevision`, and sets `total`/`totalKind` only when the reply carries a `total` (an absent total neither replaces a known one nor counts as a difference); when a present `total`, its `totalKind`, or `undecided` differ from the window's, every other held page is marked stale. **A view that shrank holds nothing past its end:** when the total the window now has is `Exact`, every held row at a view row at or past it is dropped (a page wholly past it is dropped, the page the total falls in is cut to it and counts as complete), and every fetch in flight for a page wholly past it is forgotten, so its late reply is ignored as any reply without a fetch in flight is and `pending()` does not wait for it. Without this a page past the total is never refetched (`visible` skips it) and never dropped, and its first key stays a probe that no reply can answer, so every later event would invalidate. Pages over `maxPages` go farthest first.
- **`superseded()`** returns the fetches in flight for pages outside the range `visible` last covered; **`cancelled`** removes a matching in-flight record.
- **`refresh(keys)`.** While any page fetch is in flight, the keys are appended to `_owed`. Then: no row held → `Nothing`; no key function (typed) or `awaiting()` not empty → `invalidate()`, `Pages`; otherwise the keys are the event's keys (deduplicated, held or not) plus, per run of consecutive held view rows (across adjacent held pages), the key of the run's first row unless already present; more than `maxKeys` → `invalidate()`, `Pages`; else `Rows` with `RowsFetch{keys, columns: the columns of the latest visible call (typed: {}), _query, ++_sequence}`, each key's `_latestAsked` set to the sequence and `_latestKeyed` to it.
- **`owed()`** builds a plan from `_owed` by the same rules as `refresh` without recording again; `Nothing` when `_owed` is empty. It then clears `_owed` when no page fetch is in flight, **and always when the plan is `Pages`**: the invalidation took a new revision, every reply still in flight is older than it and leaves its page stale, so the refetch that follows covers every recorded key. Keeping the keys there would never end: the client calls `visible` after every `accept`, so with two or more pages around the visible rows a fetch is in flight at every `accept`, and each `owed()` would invalidate again with no event behind it (invariant 10).
- **`patch(fetch, reply)`.** Ignored (empty result) when `fetch.query != _query`. A reply row whose key has `_latestAsked > fetch.sequence` is ignored, missing or not. `invalidate()` and an empty result when: a requested key the window holds (and no later fetch asked for) has no row; a reply row's key is held at another view row than `reply.indices`; a reply row's key is not held and its view row lies inside a held page; `fetch.sequence == _latestKeyed` and the reply's `total`, `totalKind` or `undecided` differ from the window's. Otherwise each held row with a reply row at its view row is replaced (its unloaded levels with the reply's); in a generic window, a column the page holds that the reply lacks is dropped from that page, so the next `visible` fetches it again rather than showing a cell older than its row. The replaced view rows are returned ascending. `patch` never adds, removes or reorders held rows, and never sets the totals.
- **`invalidate()`** advances `_revision` and marks every held page stale; rows stay shown, fetches stay in flight.

- [ ] **Step 1: Write the failing window tests** in `tests/table/test_table_server.cpp`

Helpers in the file: `using Win = PageWindow<GenericRow>;` `GenericPage pageOf(std::int64_t offset, std::vector<std::int64_t> keys, std::vector<std::string> columns, std::int64_t total, std::int64_t undecided = 0)` (one integer cell per column, the key's value); `GenericPage keyedOf(std::vector<std::pair<std::int64_t, std::int64_t>> keyAtIndex, columns, total, undecided = 0)`; `std::vector<RowId> ids(std::initializer_list<std::int64_t>)`.

```cpp
// Mutation: refresh not recording keys while a page fetch is in flight.
TEST_CASE("window: an event that precedes its page reply is owed", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    auto const fetches = window.visible(0, 5, cols);
    REQUIRE(fetches.size() == 2);
    CHECK(window.refresh(ids({3, 4})).kind == Refresh::Nothing);   // nothing held yet
    GenericPage reply = pageOf(0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, cols, 20);
    reply.unloadedRows = {{3, {"recipe"}, {}}, {4, {"recipe"}, {}}};
    REQUIRE(window.accept(fetches[0], std::move(reply)));
    auto const plan = window.owed();
    REQUIRE(plan.kind == Refresh::Rows);
    CHECK(std::ranges::is_permutation(plan.rows.keys, ids({3, 4, 0})));   // the keys and the run's probe
    CHECK(window.unloadedLevels(3).levels.size() == 1);
}

// Mutation: refresh skipping held rows with no unloaded level.
TEST_CASE("window: an ordinary edit on a held row is a keyed fetch, a move invalidates", "[table][window]") {
    Win window{10, 8};
    std::vector<std::string> const cols{"a"};
    for (auto const& fetch : window.visible(0, 5, cols)) {
        REQUIRE(window.accept(fetch, pageOf(fetch.request.offset, fetch.request.offset == 0
            ? std::vector<std::int64_t>{0,1,2,3,4,5,6,7,8,9} : std::vector<std::int64_t>{10,11,12,13,14,15,16,17,18,19},
            cols, 20)));
    }
    auto const plan = window.refresh(ids({7}));
    REQUIRE(plan.kind == Refresh::Rows);
    CHECK(std::ranges::is_permutation(plan.rows.keys, ids({7, 0})));   // one run, rows 0–19: one probe
    auto const replaced = window.patch(plan.rows, keyedOf({{0, 0}, {7, 7}}, cols, 20));
    CHECK(replaced == std::vector<std::int64_t>{0, 7});
    auto const again = window.refresh(ids({7}));
    auto const before = window.revision();
    CHECK(window.patch(again.rows, keyedOf({{0, 0}, {7, 12}}, cols, 20)).empty());
    CHECK(window.revision() == before + 1);
}
```

Add, each a `TEST_CASE` with its mutation:
- `"window: invalidation faster than a round trip advances one reply per trip"`: hold page 0; `invalidate`; `visible` → one fetch `f1` (revision r1); `invalidate` again (r2); `visible` → nothing (page 0 in flight); `accept(f1, rows A)`: `cell(0,"a")` shows A, the page is still stale, and `visible` returns exactly one fetch tagged r2. Mutation: `accept` ignoring every older revision.
- `"window: a column scrolled in is fetched for held pages and merged by key"` (row "A reply holds the named columns only", window half; gate K7): two complete pages with `{a}`; `visible(0, 5, {a, b})` returns two column fetches naming only `{b}`, offsets 0 and 10, limits 10; accepting replies with the same keys adds `b` (`cell(3,"b")` non-null, `cell(3,"a")` unchanged); a column reply whose `keys` differ invalidates. Mutation: a column fetch naming every column.
- `"window: over maxColumns the columns farthest from view go first"`: `maxColumns = 2`; visible `{a,b}`, then `{c}`: after the column reply, page holds `{b,c}` or `{a,c}` — assert `c` held and exactly 2 held, and the one dropped is the column least recently named (`a` when the second call was `{b, c}`). Mutation: dropping the newest.
- `"window: a page reply with another total adopts it and marks other pages stale"`: page size 10, pages 0 and 1 held with total 30; `visible(15, 10, cols)` fetches page 2; its reply says total 35: `total() == 35`, and the next `visible(15, 10, cols)` returns three fetches: pages 0 and 1 whole (offsets 0 and 10, limit 10, every column), and page 3 (offset 30), which the larger total brought into range. Mutation: totals adopted without marking (page 3's fetch alone).
- `"window: a reply without a total keeps the known one"`: the existing `"table: the window keeps a known total when a reply omits it"` still passes, and a page reply with `total` absent marks no page stale. Mutation: an absent total counted as a difference.
- `"window: a patch drops the columns its reply lacks"`: page 0 holds `{a, b}`; a keyed reply naming only `{a}` for key 3 is patched: `cell(3, "a")` is the reply's, `holds(3, "b")` is false, and `visible(0, 5, {a, b})` returns a column fetch naming `{b}`. Mutation: `patch` keeping the old `b` cells.
- `"window: a row the window never held"` (four invalidating shapes and one not): held pages 0–1 of 100 rows; a keyed reply with (i) an unheld key at view row 5, (ii) the probe at view row 1, (iii) total 101 on the latest fetch, (iv) a requested held key missing — each invalidates (`revision()` +1); (v) an unheld key at view row 80 with the total unchanged, the reply also holding the run's probe (key 0 at view row 0) → no invalidation, and `patch` returns `{0}`: the probe's row and nothing else. Mutation: no probes (shape ii undetected: assert the plan's keys contain the run's first key).
- `"window: an older keyed reply never overwrites a newer one"`: fetch F1 for key 7, then F2 for key 7; patch F2's reply (value 2), then F1's reply (value 1): `cell(7,"a")` holds 2, and F1's reply with key 7 missing does not invalidate. Mutation: no per-key sequence.
- `"window: the totals of an old keyed reply are not compared"`: F1 then F2; F1's reply carries total 30 (window 20): no invalidation. Mutation: comparing totals on every keyed reply.
- `"window: an awaited order or too many keys refetches pages"`: a page reply with `awaiting {"recipe"}` → `refresh` returns `Pages` and advances the revision; `maxKeys = 3` and an event of 3 keys plus 1 probe → `Pages`. Mutation: the key count compared before adding probes.
- `"window: superseded fetches are those left behind"` (K6 at window level): `visible(0,…)` → pages 0,1; `visible(50'000, 20)` → `superseded()` returns the fetches of pages 0 and 1; after `cancelled` for each, their late replies are ignored. Mutation: `superseded` returning every fetch.
- `"window: a typed window without a key function refreshes pages"`: `PageWindow<int>{10, 8}` holding a page: `refresh` → `Pages`. Mutation: probing without keys.
- `"window: unloaded levels agree with the page after accept"` (row (b) window): the reply's `unloadedRows {{2,{"recipe"},{"recipe"}}}` → `unloadedLevels(2).levels == {"recipe"}`, `.failed == {"recipe"}`, `unloadedLevels(1)` empty. Mutation: `accept` dropping `unloadedRows`.
- `"window: a view that empties shows no rows and asks no more"` (Review Focus 4): pages 0–1 held, total 20; a keyed reply on the latest fetch with no rows and total 0 invalidates; `visible(0, 5, cols)` returns the refetches of pages 0 and 1; the whole-page reply of page 0 with no rows and total 0 is accepted: `key(0)` and `key(10)` are null (page 1 was past the new total and is dropped), `total() == 0`, `pending()` is false although page 1's reply never came, that reply is ignored when it does (`accept` returns `false`), `visible(0, 5, cols)` returns nothing, and a later `refresh(ids({3}))` is `Nothing` (no row held, so no probe). Mutations: the total adopted without dropping what lies past it (`key(10)` stays, `pending()` stays true); the pages past it dropped but their fetches kept in flight (`pending()` stays true, the late reply is accepted). An empty reply leaving the page incomplete cannot fail this case: page 0 lies wholly past a total of 0, so it is dropped whatever its completeness.
- `"window: an owed invalidation ends"`: pages 0 and 1 held, each last reply carrying `awaiting {"recipe"}`; `invalidate()`, `visible(0, 5, cols)` → two fetches in flight; `refresh(ids({3}))` (recorded, and `Pages`); accept page 0's reply: `owed()` is `Pages` once; `visible` returns page 0's refetch; accept page 1's reply: `owed()` is `Nothing`; after the refetches `visible` asks for are accepted with no further event, `visible(0, 5, cols)` returns nothing, `pending()` is false, and `revision()` rose by exactly three since the start (the `invalidate`, the `refresh`, the one `owed`). The same holds for a `PageWindow<int>` without a key function. Mutation: `owed()` keeping `_owed` after a `Pages` plan (the revision rises at every `accept`, without end: assert it after six rounds).
- Gate K2 `"window: a refresh of 900 keys over eight runs asks 908"`: 8 pages held in eight runs (pages 0,2,4,…,14 of 100,000 rows, each its own run), an event of 900 keys: `plan.rows.keys.size() == 908`. Mutation: probing every held row.

Bridge-free typed regression: the existing `PageWindow<int>`/`PageWindow<Row>` cases keep passing with the new constructor (defaults unchanged), with two changes the new rules force: in `"table: the window ignores a reply it does not await or that no longer fits"` a late reply to a page's first fetch, made while the rest-of-page fetch is in flight, is ignored (`false`) and leaves the rest awaited, since it is not the fetch in flight for its page; in `"table: the window fetches the pages around the visible rows"` the replies carry total 700 rather than 600, since an exact total of 600 now drops page 6 (offset 600) and forgets its fetch.

- [ ] **Step 2: Run to verify failure** — `cmake --build --preset cl-debug --target morph_table_tests`: compile errors naming `refresh`, `RowsFetch`, `revision`, `PageWindow<GenericRow>` incomplete.

- [ ] **Step 3: Implement `detail::WindowCore`, `PageWindow<Row>` and `PageWindow<GenericRow>`** per the rules above. `query.hpp` declares `template <> class PageWindow<std::vector<Cell>>;` before any use so that a translation unit without `description.hpp` cannot instantiate the primary template for generic rows (it fails as incomplete instead). The generic page stores cells per column (`std::map<std::string, std::vector<Cell>, std::less<>>` per page).

- [ ] **Step 4: Run the target** — `ctest --test-dir build/cl-debug -R "^(window|table)" --output-on-failure`: all pass.

- [ ] **Step 5: Benchmarks** (`"[.benchmark][window]"`): K1 (`accept` of a 200 × 300 generic page), K2 timing (the gate case's refresh), K3 (`patch` of 200 rows). Record into table.md's "Benchmarks: client cache and window".

- [ ] **Step 6: Mutations, then commit**

```bash
git add include/morph/table tests/table
git commit -m "feat(table): the page window under changes"
```

#### 7b — reading and watching without entries, open, partial and opaque entries, end to end

- [ ] **Step 7: Write the failing cache tests** in `tests/test_ui_interpreter_cache.cpp`

The catalog: `corpusCatalog()` wrapped so that `T_Page` (model `Lims_SampleModel`) has `records = {ActionRecordInfo{.record = "T_Row", .keyField = "code", .paths = {"rows"}, .open = {"awaiting"}, .partial = {"unloadedRows"}}}` and `T_TablePage` has `{.record = "T_Row", .opaque = true}`; `Harness h{catalog};` documents query them as `kList` in `tests/test_push_cache.cpp` does.

```cpp
// Mutation: the cache ignoring `open`.
TEST_CASE("cache: an open entry is marked by a change to a key it does not hold", "[push][cache][table]") {
    Harness h{tableCatalog()};
    auto screen = h.mount(kPageList);                    // one query of T_Page
    h.reply(0, R"({"rows":[{"code":1}],"awaiting":["recipe"],"unloadedRows":[]})");
    h.session.applyChanges(changesOf("T_Row", {"99"}));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);              // refetched
    h.reply(1, R"({"rows":[{"code":1}],"awaiting":[],"unloadedRows":[]})");
    h.session.applyChanges(changesOf("T_Row", {"99"}));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);              // not open any more: key 99 is not held
}
```

Add:
- `"cache: a partial entry is not merged into"`: reply with `unloadedRows [{"index":0,"levels":["recipe"]}]`; an `updates T_Row` reply of `{code:1, qty:5}` marks the entry (one more call) and leaves `rows[0].qty` as it was; with `unloadedRows []` the same reply merges (`rows[0].qty == 5`, no call). Mutation: the cache ignoring `partial`.
- `"cache: an opaque entry is marked by every change and never merged"`: a `T_TablePage` entry and a distinct-values-shaped entry of the same action are both marked by a keyed change naming key 99, and an `updates` reply marks without merging. Mutation: the cache looking for a key member.
- `"cache: a read makes no entry"`: `session.read(channel, "T_Page", body)` resolves with the reply's text, byte for byte as the channel returned it; `cacheSize()` unchanged; a stop before the reply settles the completion as cancelled and passes `CallOptions::stop` to the channel. Mutation: `read` going through `acquire`.
- `"cache: a read's text keeps a fractional number"`: the channel replies with a generic page of one row whose Number cell is `2.5` (written by `table::writeGenericPage` over a one-column description); `session.read` resolves, and `table::readGenericPage(text, description)` decodes it to a page whose cell `(0, "n")` is the number 2.5. Mutation: `read` parsing the reply through `valueFromJson` and resolving with `canonicalJson` of the result (the completion fails: a number with a fraction is refused).
- `"cache: a watch without an entry joins the watch set"`: `watchRecord("T_Row", handler)` sends one `watch` naming `T_Row` and resolves after its reply; the test's `then` handler copies the holder out of the `WatchHolder const&` it is given and the completion is then destroyed: `watchSet()` names `T_Row` while that copy or a copy of it lives, and not after the last one is destroyed (one `unwatch`, sent then and not before); a `resync` event calls the handler once with `keys` absent; a `changes` event for `T_Row` calls it with the event's keys. Mutations: holders not counted in the watch set; the registration released when the first copy dies (the `unwatch` goes out while a copy lives).
- `"cache: an opaque entry keeps a fractional number"` (decision 46): a `T_TablePage` query replied `{"revision":"r","columns":["n","big","i"],"keys":[1],"rows":[[2.5,1e300,7]],"total":1,"totalKind":"exact","offset":0}` settles with a value and no error; `rows[0][0]` is the decimal 2.5 (`num` 25, `den` 10, one place), `rows[0][1]` is the text `"1e+300"` (`std::format("{}", 1e300)`, which `table::parseDecimal` refuses), `rows[0][2]` is the integer 7. The same reply to a `T_Page` query (not opaque) fails the entry with "the reply is not a document value". Mutations: `settleReply` using `valueFromJson` for every entry (the opaque entry is in error); the table-aware decode used for every entry (the `T_Page` entry settles).

In `tests/net/test_push_unloaded_rows.cpp`, the bridge half of (d) with gate E3:

```cpp
// Mutations: refresh asking for every held key; refresh dropping the keys the window does not hold.
TEST_CASE("table over the bridge: exactly the arrived rows update, without polling", "[net][push][levels]") {
    // WideTableModel::configure: small with recipePreloaded = 0, recipeBatch = 20; the recipe gate held.
    // No refreshEvery, no clock advanced. The CountingChannel counts page requests (no `keys`) and keyed ones apart.
    // 1. watchRecord("WideTableModel.small") resolves. PageWindow<GenericRow> w{200, 8}; w.visible(0, 30, {"id","d000"})
    //    returns two fetches (pages 0 and 1: no total is known yet); each is sent with Session::read, its text decoded
    //    with table::readGenericPage(text, *description) (the description read from the action
    //    Table.small.describe), and accepted.
    //    pageRequests == 2, keyedRequests == 0; every row's unloadedLevels names "recipe".
    // 2. Settle 200 ms: still 2 and 0.
    // 3. recipeGate().release(1): one loader call of 20 keys runs (the display wants keep each want's keys in
    //    order, newest want first, so they are the first 20 keys of one page). Let L = model.loaderCalls()[0].
    //    The cache stores, then announces L; the watch handler calls w.refresh(keys).
    // 4. The plan is Rows; its keys are L plus the probe of the one held run (key 1, the first held row) — CHECK
    //    is_permutation(plan.rows.keys, L ∪ {1}). One keyed request is sent: pageRequests == 2, keyedRequests == 1.
    // 5. w.patch returns the view rows of L, plus row 0 when key 1 was a probe, ascending; for each key of L its
    //    row's unloadedLevels is empty and cell(row, "d000") is the generator's value; a row not in L still names
    //    "recipe".
}
```

Add `"table over the bridge: the same arrival through the query cache"` (E4): a document query on `Table.small` (a page read by a `rows` table) holds the same page; the arrival marks that one entry and it refetches once: one page request per marked entry. `small` has no `Number` column (its Rationals cross as `[n, d]`, which a `Value` holds), so this case does not exercise decision 46; `"cache: an opaque entry keeps a fractional number"` above does. Mutation: the cache looking for a key member in an opaque entry (the arrival's keys match no row, and nothing refetches).

- [ ] **Step 8: Run to verify failure** — `cmake --build --preset cl-debug --target morph_tests` (and `morph_net_tests` on `linux-everything`): compile errors naming `read`, `watchRecord`, `open`.

- [ ] **Step 9: Implement the catalog's members**

- The result walk records, for each visited `Object` holder with `openMembers()` or `partialMember()`, its path and those members' paths (`ActionResult` gains `std::vector<HolderMembers> holders`, `struct HolderMembers { std::string path; std::vector<std::string> open; std::string partial; };`). A named member the holder's reflection lacks is skipped.
- Resolution gives each matched `ActionRecord` the `open` and `partial` paths of every holder whose path is a prefix of (or equal to the parent of) the record's path. `Table.<id>`'s opaque slot is Task 2's (A4). `ui::author` copies `open`, `partial` and `opaque` into `ActionRecordInfo`.

- [ ] **Step 10: Implement the cache rules** in `src/ui/interpreter.cpp`

A helper `bool memberSet(Value const& value, std::string_view path)`: set when the value at `path` is a non-empty array, an object, `true`, or a number other than zero. `markRecordChange` marks an entry whose held record is `opaque` or has any `open` path set, whatever the keys. `applyRecordEffect` treats `updates` as `changes` for an entry whose record is `opaque` or has any `open` or `partial` path set (the discarded-reply downgrade already marks only).

**A page in the query cache** (decision 46). `Session::settleReply` reads every reply through `valueFromJson`, which refuses a number with a fraction and so fails the whole entry; a generic page writes a `Number` cell as such a number. For an entry that holds an `opaque` record, `settleReply` reads the reply with `detail::tableValueFromJson` instead. It is `valueFromJson` but for a JSON number that is not an integer: that becomes the decimal `Value` its shortest round-trip text in fixed notation spells, `table::parseDecimal` of `std::to_chars(…, number, std::chars_format::fixed)` (`std::format("{}")` would write `0.0001` as `1e-04`, which `parseDecimal` refuses), and when `parseDecimal` refuses the text (more than 18 places, a numerator past `int64`) the `Value` is `std::format("{}", number)`. `tableValueFromJson` is declared in `value.hpp` and defined in `src/ui`, so `value.hpp` includes no table header. Non-finite numbers are already strings on the wire. Every other entry keeps the strict rule. A `rows` table over such a page therefore sees a `Decimal` column (Task 11a's inference), with a text among its cells where a number had no decimal form.

- [ ] **Step 11: Implement `read` and `watchRecord`**

- `CallOptions::stop`; `BridgeDispatcher::Channel::execute` uses `RawHandler::execute(…, stop)` when `stop.stop_possible()`.
- `read` sends through `channel->execute(actionId, canonicalJson(body), CallOptions{.stop = stop})` and settles on the owner with the reply's text unparsed; no entry is made, no effects applied; a stopped call settles cancelled and its reply is dropped. It does not read the reply into a `Value`: `valueFromJson` refuses a number with a fraction, and a page's Number cells are such numbers.
- `watchRecord` counts the record in `_recordRefs` as an entry does (a count leaving zero sends a `watch` for it at once, through `flushWatches`), stores the handler under a holder id, and resolves when that watch's reply arrives (at once when the record was already watched and acknowledged, or there is no watcher). `applyChanges` calls each handler whose record a change names, after marking entries; `reconnected` (reconnect and `resync`) calls each with `RecordChange{record, nullopt}`. `WatchHolder` is a `std::shared_ptr` to the registration (decision 47): the registration's destructor, which runs when the last copy of the holder dies, removes the handler and counts the record out through the session it holds weakly. The completion's own copy is one of them, so a caller that wants the watch keeps a copy and lets the completion go.

- [ ] **Step 12: Run the targets** — `ctest --test-dir build/cl-debug -R "^(cache|stale_mark|own_updates|changes_reply)" --output-on-failure` and `ctest --test-dir build/linux-everything -R "^table over the bridge" --output-on-failure`: all pass, the existing push-cache cases included.

- [ ] **Step 13: Benchmarks** — K4 (a 4,096-key event against an entry of 200 rows; storing a decoded 200 × 300 page) in `tests/bench_interpreter.cpp`; E1 (first page of sorted 100,000 × 300 `wide` with `recipe` unloaded, request to accepted page, against no level) and E2 (loader return to patched rows for 20 rows, p50/p99) in `test_push_unloaded_rows.cpp` under `"[.benchmark][net]"`. Record into table.md's "Benchmarks: client cache and window" and "Benchmarks: end to end".

- [ ] **Step 14: Docs** — `docs/spec/core/registry.md` (a holder's `openMembers()`/`partialMember()`; `ActionRecord::open`, `partial`, `opaque`; `Table.<id>` holds its record opaquely); `docs/spec/ui/interpreter.md` "Queries and the cache" (the three entry rules; `Session::read`, which resolves with the reply's JSON text because a `Value` holds no number with a fraction or an exponent and a page's Number cells are such numbers, so the caller decodes with `table::readGenericPage`; `watchRecord` and its copyable holder; an opaque entry's reply read with fractional numbers as decimals, and as text where no decimal holds them); `Session::read`'s `@return` in `interpreter.hpp` says the same. Docs build green.

- [ ] **Step 15: Mutations, then commit**

```bash
git add include/morph src/ui tests docs/spec
git commit -m "feat(ui): read and watch without entries; open, partial and opaque cache entries"
```

---

## Review Focus

Up to five input classes the spec implies and no row of its Tests table exercises; each has its test in the owning task.

1. **An invalidation between a want and its load's return, followed by another want.** A query that follows the invalidation's event while the stale load still runs must start a fresh load, not wait on one that will be dropped; otherwise the row stays unloaded until some later request. Test: Task 5, `"level cache: a want after an invalidation does not wait for the stale load"`.
2. **A loaded cell that goes back to `Unloaded`** (a server dropping what it loaded). The row moves to the unloaded tail of its sort, or becomes undecided and leaves a filtered view, with `onUndecidedChange`. Test: Task 3a, `"table: a cell dropped back to unloaded moves to the tail or leaves the view"`.
3. **Keys that are awkward JSON.** A text key with a quote, a backslash and non-ASCII, and an integer key above 2^53, through `changeKeys`/`rowIdOf` (and so through an announcement), must come back as the same `RowId`. Test: Task 5, `"table: change keys round-trip awkward keys"`.
4. **A view that empties.** Every row leaving the view (an edit, a filter's level arriving): the window must show no rows, a total of 0, and stop fetching. Test: Task 7a, `"window: a view that empties shows no rows and asks no more"`.
5. **A row entering from the undecided set under `ReorderPolicy::Deferred`.** It takes its true place at once and is not held; displayed rows keep their indices. Test: Task 3a, `"table: under deferred reorder an undecided row takes its true place at once"`.

## Self-review notes

- **Where the spec now agrees:** `Session::read` resolves with text (30) and an opaque entry's reply is decoded with fractional numbers as decimals (46); table.md states both, with their reasons. **Deviations from the spec's text, each by an index decision:** `WatchHolder` is copyable (47); `AnswerContext` carries the record and the row-scope flag (48); `ViewCacheOptions` and `ViewCacheStats` live in `table/runtime.hpp` so that `core/remote.hpp` includes no other table header, and `ViewCacheOptions::onCompute`, `RemoteServer::viewStats()` and `RowSnapshot::identity()` are additions the spec's blocks do not list.
- **Rules the window adds to the spec's:** `owed()` drops its keys when it invalidates, and a page reply with an exact total drops what the window holds past it. Both close a loop the spec's rules leave open; each has its case in Task 7a.

- **Coverage.** Spec rows (a)–(d), the cache rows, the window rows and the client-cache rows of "Tests" each map to a named case above. Gate K6's "at most three sent" and the timing rules of "When a fetch is sent" are `WindowRows`'s (Task 9); here the window's half (`superseded`) is tested. W3, W4, S1, S2, S4 timings are recorded, not gated, per "How figures are taken". L9 is not in step 2c's gate list; "A cache's size is deep" covers its behaviour.
- **Placement follows the index's Cross-part decisions:** `Page::{unloadedRows, undecided, awaiting, failed}` and `UnloadedRow` in Task 3b (decision 3); `effects::IChangeObserver`/`AnnounceSink`, `changeKeys`, `rowIdOf` in Task 5 (decision 4); `TableQuery::columns` and `maxColumns` in Task 1 (decision 2); `PageWindow`'s `key`, `cell`, `holds`, `revision` here in Task 7 (decision 8).
