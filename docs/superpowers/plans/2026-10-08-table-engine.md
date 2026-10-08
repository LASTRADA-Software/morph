# The table engine Implementation Plan

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax for tracking. One commit per task; each task is
> test-first, and each task's mutations are run and reverted before its commit.

**Goal:** `morph::table`, a toolkit-free, header-only engine that sorts, filters and maps the view of a table's
rows with exact keys and normative semantics, runs off the owner thread where a worker exists (and in cooperative
chunks on the owner where none does), and gives a server the reference implementation of a `TableQuery` through
`table::apply`. It is what the UI document's `table` node (#890) will use.

**Architecture:** A `DataSource` publishes columns, change notifications and an immutable `RowSnapshot`; the engine
never reads the source off the owner, only the snapshot. Keys are typed arrays built once per column (`KeyColumn`),
compared without parsing or allocation. Every view computation is a `ViewJob`: an object holding the snapshot, the
cached key columns it may reuse, the compiled sort and filter, and the previous view, that runs in steps (key build,
sort, filter, diff) and produces a new immutable state. The engine runs a job in one step on the owner for a small
table or with no owner executor (that is `table::apply`), on a worker otherwise, or as budgeted steps posted to the
owner when there is no worker. The result and its `ViewChange` are applied on the owner; a superseded job is stopped
through its stop token and its result is dropped by generation.

**Tech Stack:** C++23, header-only `include/morph/table/`, `math::Rational`, glaze for the wire types, Catch2
(`tests/table/`), Doxygen with `WARN_AS_ERROR`.

**Spec:** `docs/superpowers/specs/2026-10-08-table-engine-design.md` (spec 7), all of it. Cross-checked against
spec 5 (`2026-10-07-ui-document-design.md`) §9, whose `table` node is #890's, not this plan's. Issue: #889.

## Global Constraints

- **Toolkit-free.** Nothing under `include/morph/table/` includes Qt, a reactive header or a UI header. The engine
  needs the standard library, `math::Rational`, `morph::exec::IExecutor`, `CallbackScope` and glaze (for the wire
  types). `query_rows_source.hpp` is the one header that may include `morph::reactive`, and it is not in this plan
  unless `morph::reactive` is on `master` or on `origin/feature/reactive` with headers (it has a plan only, today).
- **The engine never throws across its API.** Fallible calls return `std::expected<..., TableError>`; a filter value
  that does not parse is an error naming the column.
- **No double on an exact path.** `Integer`, `Key`, `Decimal`, `Quantity`, `Date` and `DateTime` keys and filter
  values are compared exactly; only a `Number` column holds a double.
- **Owner-affine engine; immutable shared state across threads.** A job captures shared pointers to immutable data
  and its own stop token, never the engine. Nothing a job reads is mutated while it runs: a repair copies the key
  columns it patches.
- **One job at a time per engine.** A request while a job runs stops the running job and queues the new one; it
  starts when the stopped job's completion reaches the owner.
- AGENTS.md: comments state what the code does now and why — no history, no issue numbers. Every public symbol has
  complete `@param`/`@tparam`/`@return`.
- Strict warnings on GCC, Clang and MSVC (warnings as errors); clang-tidy-diff clean; a CHANGELOG `[Unreleased]`
  entry; `docs/spec/table/engine.md` for the new subsystem.
- One local build at a time, foreground. Tests live in their own executable, `morph_table_tests`, appended to
  `tests/CMakeLists.txt`, so the threaded table tests can run under the `clang-tsan` preset on their own.

## Review Focus

1. **A dropped result never lands.** A superseded job's result must not be applied, and the next job must still
   start. Pinned in Task 5 (supersede test with a gated worker).
2. **Invalid cells sort last in both directions.** A descending sort must not reverse validity. Pinned in Task 2.
3. **A numeric filter never matches an invalid cell**, including `ne`. Pinned in Task 3.
4. **`ViewChange` replays exactly.** Applying the operations to the old key list yields the new one, for random
   permutations, inserts and removes, and a single move is one operation. Pinned in Task 4.
5. **Updates do not re-sort.** A batch of row updates repairs keys of those rows only and merges them back into the
   cached order; the full-sort counter does not move. Pinned in Task 6.
6. **TSan-clean off-owner execution.** The `clang-tsan` preset runs `morph_table_tests`; instrumentation is
   confirmed with `nm`.
7. **An untrusted `TableQuery` is bounded.** Every limit of spec 7 §9 is a typed refusal or a clamp. Pinned in
   Task 7.

## Files

- Create: `include/morph/table/data_source.hpp` — `ColumnKind`, `ColumnInfo`, `RowId`, `Cell`, `QuantityCell`,
  `TableError`, `RowSnapshot`, `TableSnapshot`, `ColumnSink`, `RowChange`, `ChangeListener`, `DataSource`,
  `VectorSource`, the services (`TextCollator`, `CodePointCollator`, `DateParser`, `IsoDateParser`, `ProgressSink`,
  `Services`), `parseDecimal`.
- Create: `include/morph/table/sort.hpp` — `SortDirection`, `SortKey`, `SortChain`, `CellState`, `KeyColumn`,
  `buildKeys`, `compareKeys`, `RowOrder`, `ChunkedMergeSort`.
- Create: `include/morph/table/filter.hpp` — `FilterEntry`, `Combine`, `ColumnFilter`, `GroupFilter`, `FilterSpec`,
  `CompiledFilter`, `compileFilter`.
- Create: `include/morph/table/view_change.hpp` — `ViewOp`, `ViewChange`, `diffViews`, `applyViewChange`.
- Create: `include/morph/table/engine.hpp` — `EngineOptions`, `ReorderPolicy`, `EngineStats`, `ColumnLayout`,
  `Engine`, `detail::ViewJob`.
- Create: `include/morph/table/query.hpp` — `PageRequest`, `TableQuery`, `Page<Row>`, `QueryLimits`, `validate`,
  `PageResult`, `apply`, `RowsSource<Row>`, `PageWindow<Row>`.
- Create: `include/morph/table/selection.hpp` — `SelectionMode`, `Selection`.
- Create: `include/morph/table/edits.hpp` — `EditRequest`, `FieldError`, `RowPatcher`, `CellEdits`.
- Modify: `include/morph/forms/forms.hpp` — a member type with `schemaMarker()` marks its property (`x-table`).
- Modify: `include/morph/forms/views.hpp` — `deriveColumns` emits `kind`, `title`, `x-unitAlternatives`, `enum` and
  honours `x-hidden`.
- Create: `tests/table/*.cpp` and `tests/table/table_fixtures.hpp`; Modify: `tests/CMakeLists.txt` (appended block).
- Create: `docs/spec/table/engine.md`; Modify: `docs/spec/README.md` (one map row), `CHANGELOG.md`, spec 7 (§3, §8,
  §14, §15 and every place the code shows it wrong).

---

### Task 0: Branch and baseline

- [ ] `git switch -c feature/table-engine origin/master`; `cmake --preset gcc-debug -DMORPH_BUILD_EXAMPLES=OFF`.
  Expected: `strict=ON` in the configure output and `fastcache-cc` in `build.ninja`'s `LAUNCHER`.
- [ ] Commit this plan as the branch's first commit.

### Task 1: Data source, snapshot and services

**Interfaces (namespace `morph::table`):**

- `enum class ColumnKind : std::uint8_t { Integer, Decimal, Quantity, Number, Text, Date, DateTime, Bool, Key, Custom }`
  with glaze names `integer`, `decimal`, ….
- `using RowId = std::variant<std::int64_t, std::string>;`
- `struct QuantityCell { math::Rational amount; math::Rational toCanonical; };` — a quantity in its own unit and the
  factor to the column's canonical unit; a plain `Rational` in a quantity column is already canonical.
- `using Cell = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string, bool, QuantityCell>;`
- `struct ColumnInfo { std::string id; ColumnKind kind; std::string comparator; std::string format; };`
- `class RowSnapshot` (immutable, readable from any thread): `rowCount`, `columnCount`, `rowId`, `cell`,
  `readColumn(column, ColumnSink&, first, last)`. `TableSnapshot` stores rows in shared, copy-on-write chunks so a
  one-row update copies one chunk, not the table.
- `class DataSource`: `columns()`, `snapshot()`, `subscribe(ChangeListener&)`, `unsubscribe(ChangeListener&)`.
  `VectorSource` implements it over a `TableSnapshot` with `setRows`, `updateRow`, `insertRow`, `removeRow`.
- Services: `TextCollator { sortKey, fold, clone }`, `CodePointCollator` (code-point order, ASCII and Latin-1 case
  folded), `DateParser { parseDate, parseDateTime }`, `IsoDateParser`, `ProgressSink`, `Services` (collator, date
  parser, named comparators, progress sink).
- `std::expected<math::Rational, TableError> parseDecimal(std::string_view)` — exact decimal text.

**Tests (`tests/table/test_table_source.cpp`):** a snapshot taken twice shares storage; an update after a snapshot
leaves the old snapshot unchanged and copies one chunk; listeners receive updated/inserted/removed/reset; the
collator folds case and keeps code-point order; ISO dates and date-times parse to days and seconds, malformed text
does not; `parseDecimal("1.5")` is exactly 3/2, `"1e3"`, `""` and 19 fractional digits are refused.

**Mutations:** chunk copy-on-write copies nothing (old snapshot changes, test fails); fold leaves upper case.

- [ ] Write the tests, see them fail to compile; implement; pass; run mutations; commit
  `table: data source, snapshots and injected services`.

### Task 2: Keys and sort

**Interfaces:** `SortDirection { Ascending, Descending }` (`asc`/`desc`), `SortKey { column, dir }`,
`SortChain = std::vector<SortKey>`, `CellState { Valid, Empty, Invalid }`, `KeyColumn` (typed arrays per kind plus a
state per cell; text keeps raw text, the collator's sort key and its fold), `buildKeys(snapshot, column, info,
services, first, last, KeyColumn&)`, `compareKeys(column, a, b, dir)`, `ChunkedMergeSort` (bottom-up merge sort over
`std::uint32_t` row indices that checks a stop token and a deadline between chunks).

**Rules pinned:** quantities convert to canonical with `checkedMul`; an overflow is invalid; NaN is invalid; empty
and unparsable cells sort after valid ones in either direction; ties break by source row; a `Custom` column with an
unknown comparator sorts by its display text.

**Tests (`test_table_sort.cpp`):** multi-key with per-key direction; stability; accented text and case through a
fake accent-folding collator and through the default; dates from epoch integers and ISO text; exact decimals
(`0.1+0.2` style values that a double would tie); quantities across units; invalid last in both directions; empty
source; all-equal keys; a stop request ends the sort early.

**Mutations:** drop the validity check in the descending branch (invalid rows come first); drop the source-row
tie-break (stability fails with a non-stable merge); convert decimals through `toDouble` (the exactness case ties).

- [ ] Test first; implement; mutations; commit `table: typed sort keys and a chunked, stoppable merge sort`.

### Task 3: Filter

**Interfaces:** `FilterEntry` (one operator of `eq ne lt le gt ge between contains startsWith endsWith isEmpty
notEmpty`, plus `caseSensitive`), `Combine { Any, All }`, `ColumnFilter { combine, include, exclude }`,
`GroupFilter { columns, include }`, `FilterSpec { columns, groups }` — all glaze-readable in spec 7 §6's JSON shape;
`compileFilter(spec, columns, services) -> std::expected<CompiledFilter, TableError>`; `CompiledFilter` evaluates
column at a time over a shrinking candidate list, in chunks.

**Rules pinned:** values parse once for the column's kind; a value that does not parse is `InvalidValue` naming the
column; an operator not defined for the kind is `UnsupportedOperator`; no operator but `isEmpty`/`notEmpty` matches
an empty or invalid cell; text compares through the collator's fold unless `caseSensitive`; `%` and `_` are literal.

**Tests (`test_table_filter.cpp`):** every operator by kind; `any` and `all`; exclude; groups; numeric filter on an
invalid cell; an unparsable value; an unknown column; the spec §6 JSON example reads and compiles.

**Mutations:** let `ne` match an invalid cell; make `combine` always `any`; ignore `exclude`.

- [ ] Test first; implement; mutations; commit `table: filter specs, compiled and evaluated column at a time`.

### Task 4: ViewChange

**Interfaces:** `ViewOp { Kind { Removed, Moved, Inserted, Changed, Reset }; first; count; to; }`,
`ViewChange { std::vector<ViewOp> ops; }`, `diffViews(before, after, changed, resetThreshold)` over stable integer
identities, `applyViewChange` (reference applier used by tests and documented as the replay contract). Order:
removals from the back, moves (rows outside the longest increasing subsequence only), insertions from the front,
changes in final positions. More than `resetThreshold` operations is one `Reset`.

**Tests (`test_table_view_change.cpp`):** single insert, remove, move-first-to-last (one op), update-in-place
(one `Changed`); random permutations with inserts and removes replay exactly; a change over the threshold is reset.

**Mutations:** move every row whose index changed instead of the LIS complement (single-move case emits n−1 moves).

- [ ] Test first; implement; mutations; commit `table: minimal ViewChange from two view orders`.

### Task 5: Engine and execution

**Interfaces:** `EngineOptions { IExecutor* owner; IExecutor* worker; smallTable = 2000; resetThreshold = 5000;
frameBudget = 8ms; Services; ReorderPolicy; settle; scheduleSettle }`, `Engine(std::shared_ptr<DataSource>,
EngineOptions)`, `setSort`, `setFilter`, `viewRowCount`, `sourceRowCount`, `sourceRowOf`, `viewRowOf`, `rowIdAt`,
`viewRowOfKey`, `cellAt`, `pending`, `onViewChange`, `onPending`, `stats`, `ColumnLayout`.

**Rules pinned:** filter change with a sort active filters the cached sorted order; sort change with a filter
active sorts the survivors only; small tables and an engine with no owner executor compute in one step; with an
owner and no worker, steps are posted to the owner and each stays within the frame budget; with a worker, one job
at a time, results posted to the owner, superseded jobs stopped and dropped.

**Tests (`test_table_engine.cpp`):** a superseded sort stops (gated worker) and its result is dropped; `pending`
brackets the work and `onPending` sees true then false; the small-table path runs synchronously; results are
identical on the owner, in owner chunks and on a worker; the cached sorted order is reused (full-sort counter);
max owner step under the budget in chunk mode; `ColumnLayout` keeps one column visible and resets.

**Mutations:** apply a result without the generation check (the supersede test sees the stale order); never set
`pending` (bracket test fails); ignore the sorted cache (counter test fails).

- [ ] Test first; implement; mutations; commit `table: the engine, on and off the owner`.

### Task 6: Changes, lazy repair and deferred reorder

**Rules pinned:** updated rows re-key only themselves and merge into the cached order, coalesced per owner turn;
inserted, removed and reset rebuild; a reset with new columns keeps the sort and filter entries whose columns remain;
`deferred` holds an updated row that would move until `settle` fires with no update in between, or until its edit
ends; `ViewChange` is minimal for insert, remove, move and update.

**Tests (`test_table_changes.cpp`):** 1,000 updates in one turn give one repair and no full sort; an update moves
its row with one `Moved`; deferred holds then releases on settle and on `endEdit`; a column removed by reset drops its
sort key and filter.

**Mutations:** repair by full re-sort (counter test fails); release holds on every update (deferred test fails).

- [ ] Test first; implement; mutations; commit `table: lazy repair of updates and deferred reorder`.

### Task 7: Query, server apply, server-mode window and the x-table marker

**Interfaces:** `PageRequest { offset; limit; }`, `TableQuery { sort; filters; page; }` (`schemaMarker()` →
`x-table`), `template <class Row> struct Page { rows; total; offset; }`, `QueryLimits`, `validate(query, columns,
limits, mayRead)`, `apply(const DataSource&, const TableQuery&, ApplyOptions)`, `RowsSource<Row>` (a `DataSource`
over `std::vector<Row>` by reflection, kinds inferred from member types), `apply(std::vector<Row>, query, options)`,
`PageWindow<Row>` (pages for the visible window, bounded, error state).

**Tests (`test_table_query.cpp`):** every limit is refused or clamped; an unknown or unreadable column is refused;
the characterization corpus gives the same rows through `Engine` (client mode) and through `apply` (server mode,
pages concatenated); `TableQuery` round-trips through JSON in spec §9's shape; `forms::schemaJson` of a list action
marks the member `x-table`; `PageWindow` requests only missing pages around the visible rows, evicts beyond its
bound and shows a server error.

**Mutations:** skip the `limit` clamp; skip the readable-column check; drop the marker.

- [ ] Test first; implement; mutations; commit `table: TableQuery, Page and table::apply for server mode`.

### Task 8: Selection, activation and cell edits

**Interfaces:** `SelectionMode { None, Single, Multiple }`, `Selection` (keys, `activeKey`, `pruneOnFilter`,
`selectAll`, `prune`, `activate`), `CellEdits` (per-row serial commits through an injected commit function, stale
and field-error state per cell, a successful commit patches the row through `RowPatcher`).

**Tests (`test_table_selection.cpp`):** selection survives sort, filter and refetch; `pruneOnFilter` prunes;
activation does not select; server mode refuses select-all and prune; a commit patches the row; a field error
attaches to its cell; two edits to one row run in order.

**Mutations:** let `activate` select; start the second edit before the first settles.

- [ ] Test first; implement; mutations; commit `table: selection, activation and cell edits`.

### Task 9: Derived column metadata

`morph::views::deriveColumns` emits `kind` (spec 7 §4's inference), `title`, `x-unitAlternatives` and `enum`, and
skips `x-hidden` members. **Tests** in `test_table_columns.cpp`. **Mutation:** skip nothing for `x-hidden`.

- [ ] Test first; implement; mutations; commit `views: derive each column's kind and metadata for tables`.

### Task 10: Benchmarks and measurement

`tests/table/bench_table.cpp`, every case tagged `[.benchmark]`: 1,000 / 10,000 / 100,000 rows of integer, decimal,
quantity, accented text and date columns with a share of invalid cells; one-key numeric and text sorts, three-key
sort, text-contains, numeric-compare and group filters, sort then filter, filter then sort, toggling one filter ten
times, 1,000 cell updates while sorted; the worst owner step in chunk mode; rows per envelope. Build Release
(`gcc-release`), run `morph_table_tests "[.benchmark]"`, and write medians and spread into `docs/spec/table/engine.md`
and spec 7 §15, which replace the provisional targets. Measure the headers' compile time (§14, §17).

- [ ] Implement; run; record; commit `table: benchmarks, and the measured numbers in spec 7`.

### Task 11: Spec, docs and changelog

`docs/spec/table/engine.md` (the subsystem's authoritative reference), a map row in `docs/spec/README.md`, the
CHANGELOG entry, and spec 7 corrected wherever the code showed it wrong.

- [ ] Write; docs build; commit `docs: the table engine's spec and changelog`.

### Task 12: `query_rows_source.hpp` (conditional)

Only if `origin/feature/reactive` has `include/morph/reactive/` headers. Today it has a plan only, so this task is
left for after #887 lands, and the PR says `Refs #889`.

### Whole-branch gates

- Debug GCC build and full `ctest`.
- Docs build with `WARN_AS_ERROR`.
- `clang-tidy-diff` against `origin/master` with a clang compile database.
- `clang-tsan` on `morph_table_tests`, instrumentation confirmed with `nm | grep -c __tsan_func_entry`.
