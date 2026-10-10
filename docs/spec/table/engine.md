# `morph::table` — the table engine

Design spec for `include/morph/table/`: the toolkit-free engine that sorts,
filters and maps the view of a table's rows, the `TableQuery` a server-mode
list action receives, and the selection and cell-edit state a table keeps.
The program-level design is spec 7
(`docs/superpowers/specs/2026-10-08-table-engine-design.md`); this file is the
reference for the code as it is.

Read this before changing anything under `include/morph/table/`, and before
writing a renderer list model or a server list action against it.

## Contents

- [Shape](#shape)
- [Rows: sources and snapshots](#rows-sources-and-snapshots)
- [Keys and ordering](#keys-and-ordering)
- [Filters](#filters)
- [ViewChange](#viewchange)
- [The engine](#the-engine)
- [Execution and threading](#execution-and-threading)
- [Changes to the rows](#changes-to-the-rows)
- [Server mode](#server-mode)
- [Selection and edits](#selection-and-edits)
- [Schema integration](#schema-integration)
- [API reference](#api-reference)
- [Measurements](#measurements)
- [Failure modes](#failure-modes)
- [Design decisions](#design-decisions)
- [Not here yet](#not-here-yet)
- [Cross-references](#cross-references)

## Shape

```text
 renderer list model          applies ViewChange as keyed list operations
      │ reads
 table::Engine                sort chain, filter, view ↔ source mapping   (owner-affine)
      │ runs                      │ uses
 detail::ViewJob              Services: TextCollator · DateParser · comparators · ProgressSink
      │ reads
 RowSnapshot (immutable)  ◄── DataSource (owner-affine): columns, snapshot(), change notifications
```

Header-only, in the base `morph` target. Nothing here includes a GUI toolkit
or the reactive runtime.

| Header | Holds |
|---|---|
| `data_source.hpp` | `ColumnKind`, `ColumnInfo`, `Cell`, `QuantityCell`, `RowId`, `TableError`, `RowSnapshot`, `TableSnapshot`, `DataSource`, `VectorSource`, `RowPatcher`, the services, `parseDecimal`, `displayText` |
| `sort.hpp` | `SortKey`, `SortChain`, `CellState`, `KeyColumn`, `buildKeys`, `compareKeys`, `RowOrder`, `ChunkedMergeSort`, `sortRows` |
| `filter.hpp` | `FilterEntry`, `Combine`, `ColumnFilter`, `GroupFilter`, `FilterSpec`, `CompiledFilter`, `compileFilter`, `FilterRun` |
| `view_change.hpp` | `ViewOp`, `ViewChange`, `diffViews`, `applyViewChange` |
| `engine.hpp` | `Engine`, `EngineOptions`, `EngineStats`, `ReorderPolicy`, `ColumnLayout` |
| `query.hpp` | `PageRequest`, `TableQuery`, `Page<Row>`, `QueryLimits`, `validate`, `escapeLikePattern`, `ApplyOptions`, `PageRows`, `apply`, `RowsSource<Row>`, `PageWindow<Row>` |
| `selection.hpp` | `TableMode`, `SelectionMode`, `Selection`, `TableCounts`, `counts` |
| `edits.hpp` | `EditRequest`, `FieldError`, `EditCommit`, `CellEdits` |

## Rows: sources and snapshots

A `DataSource` is owner-affine and publishes three things: its `columns()`, a
`snapshot()` of its rows, and change notifications to `ChangeListener`s. Cells
are read **only through a `RowSnapshot`**, which is immutable once published
and safe to read from any thread for as long as it is held. That split is what
lets a job read 100,000 rows on a worker while the owner keeps changing the
source: a change publishes a new snapshot and leaves the old one alone.

`TableSnapshot` stores rows in shared, immutable 256-row chunks.
`withRow`, `withInserted` and `withRemoved` return a new snapshot that shares
every chunk the change does not touch, so an update against a snapshot a job
still reads costs one chunk copy. `VectorSource` is the in-memory source built
on it (tests, servers, sources that cannot share their storage); it also
implements `RowPatcher`, replacing a row by key. `RowsSource<Row>` (in
`query.hpp`) is a source over a shared `std::vector<Row>` by reflection.

`readColumn(column, sink, first, last)` hands a range of one column to a
`ColumnSink` in runs, so building keys for a column is one virtual call per
run, not per cell, and stops cleanly between ranges.

A `Cell` is `monostate` (empty), `int64`, `Rational`, `double`, `string`,
`bool` or `QuantityCell` (`amount` in its own unit and the `toCanonical`
factor). A string in a non-text column is parsed for the column's kind.

## Keys and ordering

Each column a sort or filter uses gets a `KeyColumn`, built once per snapshot
and shared immutably between jobs: one typed array and a `CellState` (`Valid`,
`Empty`, `Invalid`) per row.

| Kind | Key | Read from |
|---|---|---|
| `Integer`, `Key` | `int64` | `int64`, an integral `Rational`, integer text |
| `Decimal`, `Quantity` | `Rational` | `Rational`, `QuantityCell` (`checkedMul`; overflow is invalid), `int64`, decimal text (`parseDecimal`) |
| `Number` | `double` (NaN is invalid) | `double`, `int64`, `Rational`, number text |
| `Date`, `DateTime` | `int64` days / UTC seconds | `int64`, text through the `DateParser` and `ColumnInfo::format` |
| `Bool` | 0 / 1 | `bool`, `0`/`1`, `"true"`/`"false"` |
| `Text` | collation key, fold, raw text | any cell's display text |
| `Custom` | the cell, or the display text's collation key | through the named comparator, or by text when the name is unknown |

Normative ordering rules (`compareKeys`, `RowOrder`):

- Empty and invalid cells sort **after** valid ones in either direction; among
  themselves they tie.
- Each key of a chain has its own direction.
- Ties break by source row, so `RowOrder` is a strict total order and every
  sort is stable.
- Decimals and quantities compare exactly (128-bit cross products); no exact
  key passes through a double.

`ChunkedMergeSort` sorts 512-row runs, then merges pass by pass. Every 2,048
elements it looks at the clock and the stop token; `run` returns at a deadline
or a stop request and resumes from the same place.

## Filters

`FilterSpec` is data, glaze-readable in spec 7 §6's shape:
`{ "columns": { id: { "combine", "include", "exclude" } }, "groups": [ { "columns", "combine", "include" } ] }`.
A `FilterEntry` sets exactly one operator.

| Kind | Operators |
|---|---|
| `Text` | `eq`, `ne`, `contains`, `startsWith`, `endsWith` |
| `Integer`, `Key`, `Decimal`, `Quantity`, `Number`, `Date`, `DateTime` | `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `between` (inclusive) |
| `Bool` | `eq`, `ne` |
| every kind | `isEmpty`, `notEmpty` |

Semantics, normative:

- `include` entries of one column combine by `combine` (`any`, the default, or
  `all`); a match on any `exclude` entry rejects the row; columns and groups
  combine by AND.
- **No operator but `isEmpty` and `notEmpty` matches an empty or invalid cell**,
  of any kind — `ne` included. `isEmpty` matches empty cells only.
- Values are parsed once, for the column's kind, by `compileFilter`. Integer
  columns compare against exact decimals.
- Text operators compare the collator's `fold` of both sides; an entry with
  `caseSensitive` compares the raw text. `%` and `_` are ordinary characters.
- A group matches a row when its entries match on any of its columns. An entry
  that a group column cannot evaluate (operator undefined for the kind, or a
  value that does not parse) does not apply to that column; it is an error only
  when it applies to none.

`compileFilter` returns `TableError` with `UnknownColumn`, `InvalidSpec` (no
operator or several), `UnsupportedOperator` or `InvalidValue`, naming the
column. `FilterRun` evaluates column at a time over a shrinking candidate
list, compacting in place and keeping the candidates' order — which is what
lets a filter over a cached sorted order stay sorted.

## ViewChange

`diffViews(before, after, changed, identityLimit, resetThreshold)` computes the
list operations that turn one view order into another, over integer row
identities. Replay contract, which `applyViewChange` implements and every test
checks against:

1. `Removed{first, count}`, back to front;
2. `Moved{first, to}` — erase at `first`, insert at `to` of the shortened list;
3. `Inserted{first, count}`, front to back, rows taken from the new view;
4. `Changed{first, count}` in final positions, for rows in both views whose
   content changed;
5. or a single `Reset` when there would be more than `resetThreshold` (default
   5,000) operations.

Rows on a longest increasing subsequence of the new order stay put; only the
rest move, so moving one row is one operation. Move indices come from a
Fenwick tree over gap slots, so a diff is O(n log n).

## The engine

`Engine(std::shared_ptr<DataSource>, EngineOptions)` subscribes to its source
for its lifetime. The initial view is every row in source order.

- `setSort(SortChain)` and `setFilter(FilterSpec)` return `std::expected`; on an
  error (unknown column, a filter that does not compile, or `ServiceFailed`
  when the job ran inside the call and an injected service threw) the previous
  state stays. Neither throws. Columns are checked against the source's latest
  columns.
- `columns()` are the applied view's columns, what `cellAt` indexes; a column
  past them reads an empty cell. A reset that changes the columns changes
  `columns()` when its view is applied, and publishes one `Reset`.
- `viewRowCount`, `sourceRowCount`, `sourceRowOf`, `viewRowOf`, `rowIdAt`,
  `viewRowOfKey`, `cellAt`, `view` read the **applied** view, which keeps
  showing while a job is pending.
- `onViewChange` receives each `ViewChange`, after the view changed; `onPending`
  receives `pending()` transitions; `onError` receives a `ServiceFailed` from a
  job that did not run inside the call that asked for it (the sort and filter
  then go back to those of the view shown).
- A filter change with a sort active filters the cached sorted order of the
  full source (no re-sort). A sort change with a filter active sorts only the
  surviving rows (no cache is made). Key columns are reused across requests on
  the same snapshot.
- `stats()` counts jobs, full and subset sorts, key builds, repairs, dropped
  results, owner steps and the longest owner step.

`ColumnLayout` keeps column order and visibility apart from the view: at least
one column stays visible, `reset` restores the defaults, and `rebuild` adopts a
new column list keeping what remains.

## Execution and threading

Every recomputation is a `detail::ViewJob`: a snapshot, the key columns it may
reuse, the compiled sort and filter, the previous view, and phases (keys, then
order, then diff) it runs in steps. A job never names the engine.

| Situation | Where the job runs | `pending()` |
|---|---|---|
| `EngineOptions::owner == nullptr` | inside the call, one step | never |
| fewer than `smallTable` (2,000) rows | inside the call, one step | never |
| a worker executor | the worker, to completion or a stop | until the result is applied |
| no worker | steps posted to the owner, each within `frameBudget` (8 ms) | until the result is applied |

- **One job at a time.** A request that makes the running job's result useless
  (a new sort, filter, column set or structural change) requests stop on it;
  when it reports back its result is dropped and the work starts from the
  latest state. Updated rows do not stop a job: they wait and are repaired
  after its result is applied, so a stream of updates cannot starve the view.
- **Pending work.** Dirty rows (changed since the key columns were built) and
  rows owed a `Changed` are handed to a job and handed back if it is dropped
  or fails, so a `Changed` is never lost to supersession. The cached sorted
  order is only ever of the rows the keys were built from.
- **Callbacks.** Any handler (`onViewChange`, `onPending`, `onError`, the
  progress sink, the settle scheduler, a source's listener, a `CellEdits`
  commit, a patch's notification) may call back in or destroy its caller.
  Each object calls out last, or takes a `CallbackScope` token before the call
  and returns if it expired. A handler that starts a job keeps `pending()`
  true. A listener unsubscribed during a notification is not called after
  that, even if it was registered when the notification began.
- **Exceptions.** A collator, date parser or comparator that throws ends its
  job (`ViewJob::failure`); the work it took is handed back and the error is
  `ServiceFailed`.
- **Lifetime.** Completions are posted to the owner wrapped in the engine's
  `CallbackScope` guard, so a job that outlives its engine has its result
  dropped. The destructor requests stop on the running job. The owner and
  worker executors must outlive every job the engine started.
- **Services on a worker.** `Services::forTask()` fills defaults and clones a
  collator or date parser whose `cloneForTask()` returns one. Comparators are
  called from the worker and must be safe to call concurrently. A
  `ProgressSink` hears one call per job phase on a worker, one per step with
  no worker, on the owner.
- **What touches what.** Owner: the engine, the source, `onViewChange`,
  `onPending`, `ProgressSink`. Worker: the job's own state, shared immutable
  snapshots and key columns, the cloned services. A repair copies the key
  columns it patches rather than writing ones another job may read.
- Profiler zones (`MORPH_ENABLE_TRACY`): `table.snapshot`, `table.keyBuild`,
  `table.sort` and `table.filter` (with the chain or filter size as zone text),
  `table.apply`, `table.change`; plots `table.sourceRows`, `table.viewRows`,
  `table.keyCacheColumns`.

## Changes to the rows

Notifications are coalesced: the first one in an owner turn posts a flush; the
flush takes the source's new snapshot and handles everything notified since.
With no owner executor the flush runs inside the notification.

- **Updated** rows are repaired: their keys are rebuilt on copies of the key
  columns, they are filtered, sorted among themselves and merged into the
  previous order (and into the cached full order). No sort of the whole table
  happens. The change is `Moved` and `Changed` operations.
- **Inserted**, **removed** and **reset** recompute: key caches are dropped,
  the view is computed afresh, and the old view maps to the new rows by
  `RowId`. A reset re-reads the columns and keeps the sort keys and filter
  entries whose columns remain; a remaining filter entry that no longer
  compiles clears the filter.
- A recompute while updates are pending re-keys the dirty rows on copies and
  merges them into the cached full order before it filters.

**Deferred reorder** (`ReorderPolicy::Deferred`): after a repair, every
updated row that was in the view, and every row between `beginEdit` and
`endEdit`, keeps its displayed index; the rest of the view takes its true
order around it. A held row stays even if its update took it out of the
filter. `scheduleSettle(settle, fire)` arms a timer per repair; only the most
recent one releases (any update in between makes earlier timers stale), and it
releases every held row not being edited. `endEdit` releases its row at once, and the
update its commit patched, which reaches the engine a turn later, does not hold it again: a committed
edit moves its row once.
The release stands for the notifications up to `endEdit`: a view that had
already taken in the commit's update drops it, so a later update holds the
row as usual. Holds released while a job runs are laid out with its result.
`settleNow()` releases without a timer. A sort or filter change, or a
structural change, releases everything when its view is applied, rows being
edited included; the next repair holds them again.

## Server mode

- `TableQuery { sort, filters, page }`, `PageRequest { offset = 0, limit = 200 }`,
  `Page<Row> { rows, total, offset }` — glaze-readable, in spec 7 §9's shape.
- `validate(query, columns, limits, mayRead)` applies `QueryLimits`:
  `page.limit` is clamped to `maxLimit` (1,000); a negative limit, an offset
  outside `[0, maxOffset]`, too many sort keys, filtered columns, entries,
  groups or group columns, or a value longer than `maxTextLength`, is
  `LimitExceeded`; a column that is not one of the row's is `UnknownColumn`;
  one `mayRead` refuses is `ColumnNotReadable`. A hidden column is not a
  security control, so the check is here.
- `apply(const DataSource&, query, options)` validates, compiles the filter
  against the server's services, runs one `ViewJob` in the calling thread and
  slices the page. `total` is omitted above `QueryLimits::totalCap` (100,000).
  `apply(rows, key, query, options, kinds)` does the same over typed rows and
  returns a `Page<Row>`. Text order is the server's collator; a client in
  server mode does not re-sort a page.
- `escapeLikePattern` is for a database-backed server that maps `contains` to
  SQL `LIKE`: it keeps `%` and `_` ordinary.
- `TableError` serialises with its code by name (`"unsupportedOperator"`), so a
  server returns it as the action's typed error.
- `PageWindow<Row>` is the client's view of a server-mode table: `visible(first,
  count)` returns the `PageFetch`es (a `PageRequest` tagged with the query) to
  send around the visible rows (one page of margin each side, nothing past a
  known total, nothing complete or in flight); `accept(fetch, page)` stores a
  reply and evicts the pages farthest from the visible rows beyond `maxPages`;
  `fail(fetch, error)` records the error the table shows until the next reply;
  `reset` drops everything and starts a new query when the sort or filter
  changes. A reply or failure for an older query is ignored. A reply shorter
  than asked (a server's clamped limit) leaves its page incomplete, and the
  rest is fetched from where it ended; a known total survives a reply that
  omits it.

`RowsSource<Row>` has one column per key glaze writes for the row, read
through the row's `glz::meta` when it has one (which may omit, reorder or
compute members), and infers kinds from the value types: `bool` → `Bool`, other
integers → `Integer` (an unsigned value above `INT64_MAX` is an invalid cell),
floating point → `Number`, `Rational` → `Decimal`, `Quantity` → `Quantity`,
`DateTime` and `Timestamp` → `DateTime`, a type glaze writes as one of its
members (`Ranged`, `Choice`, `Tagged`) → that member's kind, anything else →
`Text` (enums by their glaze name). `std::optional` empties, and anything glaze
writes as `null`, are empty cells. A `kinds` map overrides the inference by
column id (an `int64` of epoch days is a `Date`); a `comparators` map names a
`Custom` column's comparator. `apply` compiles the filter with its own
`Services::forTask()` copy.

## Selection and edits

`Selection::create(SelectionOptions)` refuses `pruneOnFilter` in server mode.
The selection is a set of `RowId`s (in selection order) plus an active key, so
it survives sort, filter and refetch. `Single` replaces, `None` refuses.
`selectAll(engine)` needs `Multiple` and client rows. `viewChanged(engine)`
prunes rows that left the view when `pruneOnFilter` is set. `activate` is
static: it reports the row at a view row and reaches no selection.
`counts(engine, selection)` gives a status line its numbers.

`CellEdits(patcher, commit, engine)` commits `EditRequest`s through the
injected `commit` (the table's mutation), which settles each with the patched
row or a `FieldError`. Edits to one row run one at a time in commit order;
other rows run alongside. A cell is `stale` from its commit until it settles;
an error stays on its cell until an edit of that cell succeeds. A success
patches the row through the `RowPatcher`. With an engine, a row is held
(`beginEdit`) from its first commit until its last edit settles. A completion
after the `CellEdits` is destroyed is ignored.

## Schema integration

- A member type with `static constexpr std::string_view schemaMarker() noexcept`
  marks its property in a forms schema with that keyword set to `true`.
  `TableQuery::schemaMarker()` is `"x-table"`: a client recognises a list
  action that takes a table query from its schema.
- `morph::views::detail::deriveColumns` emits each column's `kind` in the
  `ColumnKind` names, plus `title`, `x-unitAlternatives`, `x-comparator` and
  `enum`, and skips `x-hidden` members. A `std::optional` member, which
  glaze writes as `anyOf` its type and `null`, derives from its type's
  alternative
  ([../forms/views.md](../forms/views.md#column-derivation)).

## API reference

| Symbol | Contract |
|---|---|
| `DataSource::snapshot()` | Cheap; the snapshot is immutable and thread-safe to read. |
| `Engine::setSort` / `setFilter` | `std::expected<void, TableError>`; state unchanged on error. |
| `Engine::pending()` | True from an asynchronous request until its result is applied. |
| `Engine::onViewChange` | Called on the owner after the view changed; replay per [ViewChange](#viewchange). |
| `Engine::beginEdit` / `endEdit` / `settleNow` | Deferred-reorder holds. |
| `compileFilter` | Parses every value once; errors name the column. |
| `diffViews` / `applyViewChange` | The list-operation contract. |
| `validate` | Clamps the limit, refuses every other bound. |
| `apply` | Synchronous reference implementation of server mode. |
| `PageWindow` | Pages around the visible rows, bounded, with an error state. |
| `Selection` | Keyed selection; server mode has no select-all or pruning. |
| `CellEdits` | Ordered per-row commits with stale and error state. |

## Measurements

AMD Ryzen 5 7600X (6 cores, 12 threads), Linux, GCC 16.2.1, Release (`-O3`),
`morph_table_tests "[.benchmark]"`, on the code as shipped. The machine also
hosts CI runners, which were busy: load average 14.4 during the run. An
earlier revision measured on the idle machine (load 2.6) ran 1.5 to 2 times
faster, so read these as an upper bound. Median and 10th-90th percentile,
milliseconds; keys are built inside every timing.

| Scenario | 1,000 rows | 10,000 rows | 100,000 rows |
|---|---:|---:|---:|
| sort: one numeric key | 0.13 (0.12-0.16) | 1.77 (1.73-1.81) | 33.01 (30.70-35.21) |
| sort: one text key | 0.23 (0.21-0.24) | 2.68 (2.65-2.75) | 46.28 (44.41-47.41) |
| sort: three keys | 0.17 (0.13-0.18) | 2.18 (2.13-2.25) | 40.35 (39.44-41.78) |
| filter: text contains | 0.09 (0.08-0.10) | 0.94 (0.91-0.96) | 9.25 (9.18-9.34) |
| filter: numeric compare | 0.03 (0.03-0.03) | 0.33 (0.32-0.36) | 2.63 (2.60-2.73) |
| filter: group | 0.10 (0.09-0.11) | 1.00 (0.97-1.04) | 9.06 (8.94-9.23) |
| sort then filter | 0.16 (0.15-0.18) | 2.15 (2.12-2.21) | 35.71 (34.77-36.37) |
| filter then sort | 0.09 (0.08-0.10) | 1.41 (1.40-1.43) | 16.18 (15.28-18.26) |
| toggle one filter ten times, sorted | 0.16 (0.15-0.17) | 2.30 (2.28-2.35) | 17.88 (17.17-19.37) |
| 1,000 updates while sorted (one repair, no sort) | 6.51 (6.32-6.61) | 8.38 (8.19-8.65) | 27.39 (27.13-28.09) |

Owner blocking, three-key sort then a text filter, per run:

| Execution | Rows | Worst owner step, median of runs | Worst of all runs | Request to result, median |
|---|---:|---:|---:|---:|
| one step (below `smallTable`) | 1,000 | 0.18 | 0.21 | 0.29 |
| owner steps, 8 ms budget | 10,000 | 2.22 | 2.52 | 3.27 |
| worker | 10,000 | 0.00 | 0.01 | 4.05 |
| owner steps, 8 ms budget | 100,000 | 9.61 | 11.61 | 57.32 |
| worker | 100,000 | 0.03 | 0.04 | 95.71 |

A step runs past its 8 ms budget by at most one chunk of work plus whatever
the scheduler takes from it; on the idle machine the worst step was 8.03 ms.

Rows per envelope: a page of rows of ten small cells costs 212.2 bytes per row
inside an `ok` envelope; 39,361 rows fit in `kMaxEnvelopeBytes` (8 MiB), checked
against `wire::decode` both ways.

## Failure modes

| Situation | Behaviour |
|---|---|
| A filter value that does not parse | `InvalidValue` naming the column; filter unchanged. |
| A sort on an unknown column | `UnknownColumn`; sort unchanged. |
| A quantity whose canonical value overflows | The cell is invalid and sorts last. |
| A sort, filter or structural request while a job runs | The job is stopped and its result dropped; the latest state runs next. |
| An update while a job runs | It waits; a repair follows the job's result. |
| An injected service throws | `ServiceFailed`; the sort and filter stay those of the view shown. |
| A handler destroys the engine, source or `CellEdits` | The caller returns without touching itself. |
| The engine destroyed with a job in flight | The job is stopped; its completion is a no-op. |
| A reset that removes a sorted or filtered column | That sort key or filter entry is dropped. |
| A server query over a bound | `LimitExceeded` (the limit itself is clamped). |
| A commit's completion after its `CellEdits` is gone | Ignored; the destructor ended the row's engine edit. |

## Design decisions

- **Cells through snapshots, not the source.** A worker cannot read a source
  the owner is changing, so cells are read from an immutable snapshot the
  source publishes, and the source itself is touched only on the owner.
- **Chunked snapshots.** Copying the whole table on each update would make a
  stream of updates quadratic; with 256-row chunks one update is one chunk
  copy.
- **Results dropped, not merged, on supersession.** Applying a stale result
  and then the new one would emit two `ViewChange`s for one user action and a
  flicker; dropping it costs only the stopped work.
- **A repair merges; a structural change recomputes.** Merging needs stable
  row indices; inserts and removes shift them, and are rarer.
- **Self-timed benchmarks.** The spec asks for medians and spread; Catch2's
  `BENCHMARK` reports a mean and a standard deviation.

## Not here yet

- With no worker, a recompute's diff is one owner step (about 30 ms for an
  insert into 100,000 rows), and applying a `Deferred` repair lays out and
  diffs on the owner (10 to 55 ms at 100,000 rows).
- `RowsSource` cannot name a date column's text format.

- `query_rows_source.hpp` (`QueryRowsSource`, a source over a
  `morph::reactive` query's rows, diffing a refetch by key) needs the reactive
  module, which is not on `master` yet.
- The `table` document node (spec 5 §9, spec 7 §12), its renderer list model,
  and the mounting measurement belong to the UI document interpreter.

## Cross-references

- Spec 7: `docs/superpowers/specs/2026-10-08-table-engine-design.md`.
- [../core/callback_scope.md](../core/callback_scope.md) — the lifetime gate on
  every completion.
- [../core/executor.md](../core/executor.md) — owner and worker executors.
- [../util/rational.md](../util/rational.md) — exact keys.
- [../forms/views.md](../forms/views.md) — derived column metadata.
