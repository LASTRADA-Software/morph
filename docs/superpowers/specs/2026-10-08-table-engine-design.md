# The table engine: a table for a model, declared — design (spec 7)

An application that is mostly tables — long result sets, many typed columns, sorting and filtering
that must stay fast, cells the user edits — declares a table by naming the model's list action. The
server implements that action once; the client shows a sortable, filterable, selectable, editable
table with no GUI code written for it. This spec defines the **table engine** that does the sorting,
filtering and view mapping, the **conventions** that connect it to a model, and the **`table` node**
that exposes it in a UI document (spec 5).

The engine is a toolkit-free component of the base library. It is used on the client over the rows
a query returned, and may be used on a server over the rows it holds.

## Contents

[1 Intent](#1-intent) · [2 A table for a model](#2-a-table-for-a-model) ·
[3 Architecture](#3-architecture) · [4 Columns](#4-columns) · [5 Sort](#5-sort) · [6 Filter](#6-filter) ·
[7 Sort, filter and change](#7-sort-filter-and-change) · [8 Execution](#8-execution-and-threading) ·
[9 Client and server modes](#9-client-and-server-modes) · [10 Selection](#10-selection-and-activation) ·
[11 Editing](#11-editing-cells) · [12 The table node](#12-the-table-node) ·
[13 Rendering](#13-rendering) · [14 Packaging](#14-packaging) · [15 Measurement](#15-measurement) ·
[16 Tests](#16-tests) · [17 Risks](#17-risks)

## 1. Intent

**Goals:**

- **A table is declared, not built.** Naming a model's list action gives a working table. Columns,
  headers, units, decimals and sort behaviour come from the row type; a declaration overrides what
  the type cannot say.
- **Sorting and filtering are fast and never block the UI.** A result of about 1,000 rows sorts and
  filters imperceptibly; 10,000 to 100,000 rows are handled off the UI thread, and a superseded
  request is abandoned.
- **One engine, no toolkit.** The engine includes no GUI toolkit. A renderer applies its output.
- **Exact values.** Integers, decimals and quantities are compared exactly (spec 5 §1); no key passes
  through a double unless the column says it is a double.

**Decisions:**

- **The engine owns the view.** Sort order, filter result and the mapping between source rows and
  view rows live in the engine, not in a renderer's list model, so every renderer shows the same
  table and the logic is tested once.
- **Two modes, one declaration.** For a result that fits the client, the engine sorts and filters
  there (§9). For a result that does not, the same sort and filter values are sent to the server in
  the query body and the engine only maps the view. The document chooses per table.
- **Semantics are normative.** Filter combination, the order of unparsable cells and the stability
  of the sort are specified here (§5–6), so a client and a server that both use the engine agree
  row for row.

## 2. A table for a model

On the server, a list action's body embeds the table query and its reply is a page of rows:

```cpp
struct ListResults {
    std::optional<std::int64_t> sampleId;          // the action's own parameters
    morph::table::TableQuery    table;             // sort, filters, page; the schema marks it `x-table`
};
struct ResultRow { std::int64_t id; std::string analysis; Quantity<Mass> mass; DateTime measuredOn; };
using ListResultsReply = morph::table::Page<ResultRow>;      // { rows, total, offset }
```

```cpp
auto screen = ui::screen<"lims.results">([](ui::Scope& s) {
    auto sample = s.model<SampleModel>("sample", {.instance = s.param<int64_t>("sampleId")});
    return ui::dataTable<ListResults>(sample, {.mode = ui::TableMode::Server});
});
```

The same declaration as a document node:

```json
{ "kind": "table", "model": "sample", "action": "Lims_ListResults", "mode": "server",
  "body": { "obj": { "sampleId": { "ref": "sampleId" } } } }
```

The `x-table` marker on the body member is how the client knows the action takes a table query and
sends sort, filters and page in it. The client derives the columns from `ResultRow`'s schema (§4), builds the sort and filter controls,
fetches pages as the view needs them, and renders the table. Nothing else is written. A node that
needs more adds to it (`columns`, `selection`, `onActivate`; §12).

`morph::views::viewSchemaJson<V>()` (the collection view of spec 2 §8) already derives a column list
from a query action's row type. The table node is the same declaration seen from the document: a
`collection` is a `table` node with row and collection actions, and both read the same column
metadata.

## 3. Architecture

```text
 table node (spec 5 §9)             declared in a document
      │ uses
 renderer list model (spec 3 §5)    applies ViewChange as keyed list operations
      │ reads
 table::Engine (toolkit-free)       sort, filter, view mapping, column layout
      │ reads                          │ uses injected services
 table::DataSource                  TextCollator · DateParser · StopToken · ProgressSink
```

### DataSource

```cpp
namespace morph::table {
enum class ColumnKind : std::uint8_t { Integer, Decimal, Quantity, Number, Text, Date, DateTime, Bool, Key, Custom };

struct ColumnInfo { std::string id; ColumnKind kind; std::string comparator; /* Custom only */ };

using Cell = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string, bool>;

class DataSource {
public:
    virtual ~DataSource() = default;
    [[nodiscard]] virtual std::size_t rowCount() const = 0;
    [[nodiscard]] virtual std::span<ColumnInfo const> columns() const = 0;
    [[nodiscard]] virtual RowId rowId(std::size_t row) const = 0;          // stable across reloads
    [[nodiscard]] virtual Cell cell(std::size_t row, std::size_t column) const = 0;
    virtual void readColumn(std::size_t column, ColumnSink& sink) const;   // bulk read; the default loops over cell()
    virtual void subscribe(ChangeListener&) = 0;                           // rows added, removed, updated
};
}
```

- A column is identified by id and kind. `Quantity` and `Decimal` cells are `Rational`s (an int64
  numerator and denominator with decimal places); a value that does not fit is invalid, never
  saturated. Only a `Number` column holds a double.
- The engine builds keys with `readColumn`, which hands a column's cells to a sink in one pass, so
  100,000 rows do not cost 100,000 virtual calls and as many variant copies. A source with a faster
  path (the query's row array) overrides it.
- `RowId` is the table's `key` (spec 5 §9): `std::variant<std::int64_t, std::string>`.
- The framework supplies **`QueryRowsSource`**, a `DataSource` over a query's row list. A refetch is
  diffed by key into added, removed and updated rows; a mutation's `patch` (spec 5 §5) is a one-row
  update.

### Injected services

Each is a small interface with a deterministic default for tests. A service used on a worker is
thread-safe, or the engine clones one per task (§8).

| Service | Purpose | Default | Renderer-side implementation |
|---|---|---|---|
| `TextCollator` | Locale-aware compare and sort key | Code-point order, case-folded | The toolkit's collator for `{"env": "locale"}` |
| `DateParser` | Text to date or date-time for a format | ISO 8601 | The toolkit's parser |
| `StopToken` | Cancel a long sort or filter | `morph::async::StopToken` | — |
| `ProgressSink` | Optional progress | none | A progress indicator |

### Engine

`setFilter(FilterSpec)`, `setSort(SortChain)`, `viewRowCount()`, `sourceRowOf(viewRow)`,
`viewRowOf(sourceRow)`, and a `ViewChange` event that describes what moved, in the vocabulary of a
list model (inserted, removed, moved, changed, reset). Column order and visibility are a separate
`ColumnLayout`. Fallible calls return `std::expected` (a filter value that cannot be parsed for its
column kind is an error naming the column); the engine does not throw across its API.

## 4. Columns

**Derivation.** The columns of a table are derived from the row type's schema, as `v-columns` is
(spec 2 §8), one per member in `x-order`, skipping members the schema marks `x-hidden`. Each carries
what the schema knows: label (the schema's `title`, else the member name), kind, unit and its
alternatives, decimal places, enum values.

`morph::views::deriveColumns` emits only `field`, `label`, `v-hidden`, `x-decimalPlaces` and
`ExtUnits` today. It is extended to emit `kind`, `x-unitAlternatives`, enum values and `title`, and to
honour `x-hidden`. The kind is inferred from the member's schema: `integer` is `Integer`; `number`
with `x-displayDecimals` is `Number`; a `Rational` with `x-decimalPlaces` is `Decimal`; an `ExtUnits`
type is `Quantity`; `string` with `format: date` or `date-time` is `Date` or `DateTime`; `x-widget:
epochDays` or `epochSeconds` on an integer is `Date` or `DateTime`; `boolean` is `Bool`; a string
otherwise is `Text`; `x-comparator` names a `Custom` column.

`morph::views::ColumnOverride` grows to carry the rest, and a table node's `columns` entry accepts
the same keys:

| Key | Meaning |
|---|---|
| `field`, `label`, `hidden` | As `ColumnOverride` has them |
| `kind` | Replaces the derived kind |
| `sortable`, `filterable` | Default `true`; `false` removes the column's sort and filter controls |
| `comparator` | Names an injected comparator for a `Custom` column; an unknown name sorts by the display text |
| `width`, `align`, `pinned` | Presentation hints; a renderer may ignore them |
| `tone` | An expression over `{"row": ...}` that yields `ok`, `warn` or `err` for the cell |
| `cell` | A document node rendering the cell (spec 5 §9), for a column the default cannot show |
| `edit`, `readonly` | Make the column editable (§11), and make it read-only by an expression |

**Data-driven columns.** `columns` may be an expression that evaluates to a list (a query's result
mapped to column entries), for tables whose columns are data, such as per-attribute columns. The
engine rebuilds its layout when the list changes and keeps the sort and filter entries whose columns
remain.

**Kinds and exactness.**

| Kind | Key built | Compare |
|---|---|---|
| `Integer`, `Key` | `int64` | numeric |
| `Decimal`, `Quantity` | `Rational`; a quantity is converted to the canonical unit with checked arithmetic, and a value that overflows int64 is invalid | exact cross-multiplication in 128 bits, no allocation |
| `Number` | `double` | numeric; NaN sorts as invalid |
| `Bool` | 0 or 1 | false before true |
| `Date`, `DateTime` | `int64` days or UTC seconds | numeric |
| `Text` | Collation key from `TextCollator` | key order |
| `Custom` | Per comparator | per comparator |

A cell that is empty or cannot be parsed for its kind is **invalid**.

## 5. Sort

- Each column gets a typed key array, built once per column and cached, plus a validity flag per
  cell. A comparison is a direct compare of two keys with no parsing and no allocation.
- A multi-key sort is a comparator chain over the arrays; each key has its own direction. The sort is
  **stable**: ties break by source row.
- **Invalid cells sort after valid ones**, in either direction. A filter on a numeric or date column
  never matches an invalid cell.
- The engine caches the sorted order of the full source per sort chain.

## 6. Filter

A `FilterSpec` is data, so a document can hold it as a value, a user can save it, and a server can
receive it:

```json
{ "columns": {
    "analysis": { "include": [ { "contains": "pH" } ] },
    "mass":     { "combine": "all", "include": [ { "ge": "1.5" }, { "lt": "10" } ] },
    "status":   { "include": [ { "eq": "open" }, { "eq": "hold" } ], "exclude": [ { "eq": "void" } ] } },
  "groups": [ { "columns": ["analysis", "analyst"], "include": [ { "contains": "ph" } ] } ] }
```

- **Operators by kind.** Text: `contains`, `startsWith`, `endsWith`, `eq`, `ne`. Numeric, date and
  date-time: `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `between`. Any kind: `isEmpty`, `notEmpty`. Values
  are exact text (`"1.5"`), parsed once for the column's kind when the filter is set.
- **Combination.** The `include` entries of one column combine with `combine`: `any` (the default) or
  `all`. A match on an `exclude` entry rejects the row. A column's result and every other column's
  combine with AND. A **group** names several columns and matches a row when its entries match any of
  them, which is what a quick-search box needs; a group combines with the columns by AND.
- **Text matching.** Text operators compare through the table's collator at primary strength, so
  `eq`, `ne`, `contains`, `startsWith` and `endsWith` ignore case and accents; an entry may set
  `"caseSensitive": true`. `contains`, `startsWith` and `endsWith` treat `%` and `_` as ordinary
  characters.
- A filter is compiled when set. Evaluation is column at a time over a shrinking candidate set.
- The semantics above are pinned by characterization tests (§16) before an application's existing
  table is moved onto the engine, so a difference is a finding, not a surprise.

## 7. Sort, filter and change

- A filter change with a sort active filters the cached sorted order, O(n), with no re-sort.
- A sort change with a filter active sorts only the surviving rows.
- A change notification invalidates only the affected rows' keys. The order is repaired lazily, so a
  stream of row updates does not re-sort per row.
- **Reorder policy.** A table's `reorder` is `immediate` (the default) or `deferred`. With
  `deferred`, an updated row that would move stays in place until the view is quiet for the
  `settle` interval or the row's editor commits and loses focus; then the engine emits one `moved`.
  A user editing a sorted column therefore does not see the row leave from under the cursor.
- `ViewChange` is computed from the old and new view order, so a renderer applies the minimum list
  operations (spec 3 §5).

## 8. Execution and threading

The reactive runtime is serial and owner-affine (spec 1 §3). Where a worker thread exists, the
engine does its work off the owner:

- **Snapshot.** The engine reads an immutable, shared snapshot of the rows. `QueryRowsSource` holds
  the query's reply as shared immutable data, so taking the snapshot copies a pointer, not the rows; a
  `DataSource` that cannot share its rows hands the engine a copy-on-write array. A `DataSource` is
  never read from the worker.
- **Tasks.** Building keys, sorting, filtering and the `ViewChange` diff run on a worker of the
  application's thread pool, one task at a time per table. A task captures the snapshot, the compiled
  sort and filter and its `CallbackScope` generation, and never the engine or a signal, so a task that
  outlives its table only has its result dropped by the owner's post contract.
- **Stop.** Stopping is cooperative: the task checks the `StopToken` between chunks of the key build
  and inside a chunked merge sort, so a superseded task usually stops early and may run to
  completion, its result then dropped by generation.
- **Services.** Collator, date parser and progress sink calls happen on the worker: a service is
  thread-safe, or the engine clones one per task; `ProgressSink` notifications are posted to the owner.
- **Result.** A view mapping and its `ViewChange` are posted to the owner and applied there. `pending`
  is true from the request until then, and the previous view stays visible meanwhile (like a `Query`'s
  last value, spec 1 §4b). The engine never writes a signal from the worker.
- **No worker.** On WebAssembly and on a frontend with one thread, the engine runs the same tasks as
  cooperative chunks on the owner, each within the owner's frame budget (default 8 ms) and yielding
  between chunks; a superseding request cancels at the next boundary. The 16 ms bound of §15 holds
  without threads; throughput is lower, so the browser caps `client` mode lower than native (§9).
- **Small tables** (default under 2,000 rows) compute on the owner in one step, because the hand-off
  costs more than the work.
- **Large changes.** A `ViewChange` of more than a set number of operations (default 5,000) is a
  `reset`, which a renderer applies as a model reset and restores selection and focus by key.

## 9. Client and server modes

| | `client` | `server` |
|---|---|---|
| Rows the client holds | All | The pages it has fetched |
| Sorting, filtering | The engine, on the client | The server, from the query body |
| Suits | A result that fits one envelope (below) | Any size |
| The action | Returns all rows (no table member needed) | Embeds `TableQuery`, returns `Page<Row>` |

**The client mode bound.** An action's reply travels as one wire envelope, and `wire::decode` rejects
an envelope above `kMaxEnvelopeBytes` (8 MiB); the reply is a JSON string inside it, so escaping
shrinks the room. A table of ten small cells per row fits some tens of thousands of rows, a figure
measured in the first change and not assumed. Above it a table uses `server` mode, or `client` mode
with `pageSize`, which loads pages into the same snapshot until the result is complete.

**Hybrid.** `mode: "hybrid"` is a server search with a client engine over its result: the `body` is
sent to the server, whose reply is a result that fits one envelope; sort and filters run on the
client over it, as in `client` mode, and a change of `body` refetches. It is the mode for a table
with a heavy server-side filter panel and a header filter row on the rows it returned.

**`TableQuery`** is `{ sort: [{column, dir}], filters: FilterSpec, page: {offset, limit} }`.
`Page<Row>` is `{ rows, total, offset }`; `total` is optional and may be capped, since an exact count
of a large filtered set is itself expensive. The server may implement the action with the same
engine over its own rows (`table::apply(source, query)`), or with a database query, as long as it
honours the semantics of §5–6; a server that cannot honour an operator or a column's sort answers
with a typed error the table shows.

**Limits a server applies to every `TableQuery`**, since it is an untrusted request body:

- `page.limit` is clamped (default 1,000) and `page.offset` is bounded; the number of sort keys,
  filter columns, `include` and `exclude` entries and groups, and the length of a text value, are
  bounded, and a request over a bound is refused with a typed error.
- A column in `sort` or `filters` is one of the row schema's columns, from a closed set, and never an
  interpolated name; values are bound parameters; the wildcard characters of §6 are escaped.
- A sort or filter column is one the principal may read. The authoriser sees the action, not the
  body, so the action or the model enforces it: a hidden column is not a security control (a filter on
  it is an oracle for its value).
- The server's execute timeout and in-flight limits apply; an unindexed text scan is the usual way to
  exhaust them.

**Order.** The semantics of §5–6 are normative for what a filter matches and how ties break. The order
of text is the collator's: in `server` mode the server's collator decides the order it returns, and
the client does not re-sort a page.

In `server` mode the table is a query whose key includes the sort, the filters and the page, so
spec 5 §5's rules apply: latest wins, the last value stays while a refetch is in flight, `debounce`
batches filter typing. The view fetches the pages around the visible rows and keeps a bounded window.

## 10. Selection and activation

- **Selection** is a set of `RowId`s with modes `none`, `single` and `multiple`, and an `activeKey`
  (the row with the cursor). Both are element values of the table (spec 5 §6) and survive sort,
  filter and refetch: a row that leaves the view stays selected unless the table says
  `"selection": {"pruneOnFilter": true}`.
- **Activation** (double-click, Enter) runs `onActivate` with the row in scope; it does not change the
  selection.
- `viewCount`, `sourceCount` and `selectedCount` are element values, for a status line.
- In `server` mode a selected row may be outside the fetched window, so "select all" and
  `pruneOnFilter` are not available; selection is the set of keys the user chose.

## 11. Editing cells

A column with an `edit` entry is editable:

```json
{ "field": "mass", "edit": { "mutation": "capture", "body": { "obj": { "resultId": { "row": "id" },
                                                                       "mass":     { "ref": "event" } } },
                              "commitOn": "enter" } }
```

- The cell shows an editor chosen from the column's kind (text, number with unit, date, enum select),
  built by the forms engine's field model so validation, exact encoding and bounds are the form's.
- A commit runs the named mutation. Its `patch` replaces the row; a `serialKey` of the row id keeps
  one row's edits in order (spec 5 §5). The cell shows `stale` while the call is in flight and the
  server's field error on failure (spec 5 §7).
- With `reorder: "deferred"` (§7) the edited row holds its place until the commit settles.
- A column without `edit` is read-only; a document may make a column editable conditionally with
  `readonly` bound to an expression.

## 12. The table node

The node of spec 5 §9 (`{"kind": "table", "rows": ..., "key": ..., "columns": [...]}`) gains the
declarative form of §2 and these properties, all optional:

| Property | Meaning |
|---|---|
| `model`, `action`, `body` | The list action, as a query (§2); replaces `rows` |
| `mode` | `client`, `server` or `hybrid` (§9); default `client` |
| `columns` | A list, or an expression that yields one (§4); merged over the derived columns |
| `sort` | The initial sort chain; the user's changes replace it |
| `filters` | A `FilterSpec` expression, so an element (a search box) can drive it |
| `reorder`, `settle` | §7 |
| `rowTone` | An expression over `{"row": ...}` that yields `ok`, `warn` or `err` for the row |
| `highlightKey` | A key to emphasise, for a link with a chart (spec 5 §10) |
| `columnChooser` | A header menu for the visible columns; default `true` |
| `selection` | `{"mode": "none" \| "single" \| "multiple", "pruneOnFilter": bool}`; the default mode is `none` |
| `onActivate` | Command list; the row is in scope as `{"row": ...}` |
| `pageSize` | Server mode: rows per fetched page (default 200) |

Element values (spec 5 §6): `sort`, `filters`, `selection`, `activeKey`, `hoveredKey`, `viewCount`,
`sourceCount`, `selectedCount`, `pending`, `error`.

**Customization points** (spec 5 §12b): `columns` (order, visibility, width), plus `sort` and
`filters` of kind `saved`, so a user keeps named sorts and filters across devices. The column
chooser edits the `columns` point: it keeps at least one column visible and offers a reset to the
document's default.

**Filter controls.** The default node shows a header with a sort indicator per sortable column and a
filter control per filterable one, each built from the column's kind. A document that wants its own
controls sets `"controls": false` and drives `sort` and `filters` itself.

## 13. Rendering

- The renderer's list model applies `ViewChange` as keyed operations (spec 3 §5), so selection,
  focus and scroll position survive a sort or filter.
- A table shows `pending` as a busy indicator over the previous rows and an `error` as a banner.
- The terminal renderer shows the visible window and supports sort and filter keys.

## 14. Packaging

Header-only, in the base `morph` target, under `include/morph/table/`: `engine.hpp`, `data_source.hpp`,
`filter.hpp`, `sort.hpp`, `query.hpp` (`TableQuery`, `Page`, `apply`), `query_rows_source.hpp`. It
depends on `morph::reactive` only for `QueryRowsSource`, which is in its own header, so the engine
itself needs the standard library and `math::Rational`. The engine wraps no toolkit, so it follows
the program's packaging rule (morph is header-only except optional components that wrap a compiled
toolkit). Compile time of the sort and filter code is measured (§17); if it is not acceptable, the
headers are split so a translation unit that only needs `TableQuery` and `Page` does not include the
engine.

## 15. Measurement

Performance work is driven by numbers, from two sources.

- **Benchmarks.** Catch2 `BENCHMARK` cases tagged `[.benchmark]` over synthetic sets of 1,000,
  10,000 and 100,000 rows, with integer, decimal, quantity, accented-text and date columns and a
  share of invalid cells. Scenarios: one-key numeric and text sorts, a three-key sort, text-contains,
  numeric-compare and group filters, sort then filter, filter then sort, toggling one filter ten
  times, and 1,000 cell updates while sorted. Results are written to `docs/` as median and spread per
  scenario.
- **Traces.** Zones behind `MORPH_ENABLE_TRACY`: `table.snapshot`, `table.keyBuild`, `table.sort`,
  `table.filter`, `table.apply`, `table.change`, with the sort chain and filter summary as zone text,
  and plots for source rows, view rows and key-cache size.
- **Mounting.** The cost of mounting a table whose rows each have a row scope (state, one query,
  two bound cells) at 1,000, 10,000 and 100,000 rows: time to first frame, reactive nodes alive,
  and memory. The result decides whether row scopes follow the view's windowing (spec 3 §5): if
  10,000 rows mount within one frame budget, every row keeps its scope; otherwise a row scope is
  created when its row is instantiated by the view, and its state is kept by row key while the row
  exists.
- **Provisional targets**, to be confirmed against baselines: the owner thread is never blocked for
  more than 16 ms by a table operation at any size, with or without a worker (§8); a sort or filter
  of 1,000 rows completes within one frame, on the owner in one step; of 10,000 rows within 100 ms;
  of 100,000 rows within 1 s, off the owner. Rows per envelope (§9) is measured in the same run.

An application measures its own baseline on its own table before moving it, and compares against the
same scenario names. The change that adds `server` mode runs the benchmarks first, and the measured
numbers replace the provisional targets above in this spec before that change merges.

## 16. Tests

- **Characterization:** an application's existing sort and filter behaviour, as a corpus of
  (rows, filter, sort) → expected view, run against the engine before the application moves.
- **Sort:** multi-key, direction per key, stability, accented text and case with the real collator
  and a fake, dates, exact decimals and quantities across units, invalid cells last in both
  directions, empty source, all-equal keys.
- **Filter:** every operator by kind, `any` and `all`, exclude, groups, a numeric filter never
  matching an invalid cell, a value that does not parse for its column.
- **Change:** an update during a sort repairs lazily; `deferred` holds a row until the settle interval
  or the commit; `ViewChange` is minimal for insert, remove, move and update.
- **Execution:** a superseded sort stops and its result is dropped; `pending` brackets the work; the
  small-table path runs on the owner; results are identical on and off the owner.
- **Modes:** the same corpus gives the same view in `client` mode and through a server using
  `table::apply`; a server error is shown; pages are fetched for the visible window.
- **Selection:** selection survives sort, filter and refetch; `pruneOnFilter`; activation does not
  select.
- **Editing:** a commit runs the mutation and patches the row; field errors attach to the cell;
  serial edits to one row stay ordered.
- **Document:** a derived-columns table on `RecordingBackend`; a data-driven column list; saved sorts
  and filters round-trip; the Qt Quick renderer's list model receives the operations of `ViewChange`.
- **Benchmarks:** build and run under `[.benchmark]`; a regression gate on the scenarios once
  baselines exist.

## 17. Risks

- **Header-only compile time.** The engine is template-light but large. It is measured in the first
  change; splitting the headers is the fallback (§14).
- **Semantic drift from an application's existing table.** Characterization tests (§16) are the
  guard; a difference is decided case by case, not absorbed.
- **Server mode needs every server to honour §5–6.** `table::apply` gives a server the reference
  implementation; a database-backed server maps the operators itself and says what it cannot.
- **Collation differs between clients.** The sort key comes from the renderer's collator; a client
  and a server in server mode may order accented text differently. Server mode therefore uses the
  server's collator for the order it returns, and the client does not re-sort a page (§9).
- **Provisional thresholds.** The figures in §8 and §15 are targets, not measurements.
