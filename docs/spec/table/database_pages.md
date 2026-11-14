# `morph::table` — database pages

Design spec for the seam that lets a model which holds its rows in a database page them there: `table::planQuery`,
the validated and normalised query; `Page::totalKind`, which says whether a reported total is the count or a lower
bound; and `table::conformanceFailures`, the helper that holds a model's translation to `apply`'s answers. Read
[`engine.md`](engine.md) first; this file extends its "Server mode" section.

## Contents

- [Why](#why)
- [Shape](#shape)
- [`planQuery`](#planquery)
- [Order: the model's row key](#order-the-models-row-key)
- [Semantics a translation keeps](#semantics-a-translation-keeps)
- [Per-kind translation](#per-kind-translation)
- [Text](#text)
- [Refusing what a store cannot translate](#refusing-what-a-store-cannot-translate)
- [Totals](#totals)
- [Conformance](#conformance)
- [Aggregate reports](#aggregate-reports)
- [What the seam does not do](#what-the-seam-does-not-do)
- [Alternatives considered](#alternatives-considered)
- [Failure modes](#failure-modes)

## Why

`apply` takes the server's rows as a `DataSource` and sorts, filters and slices them in memory. A list action over a
table that lives in a database then has to read every row of the table before it can return fifty of them.

Measured on lims's ledger (Release build, SQLite on disk, one process): `ListEntries` over 10^6 entries took 527.6 s
and its peak resident memory went from 46 MB to 2,777 MB; a trial balance over 10^7 posted lines took 168.2 s and
peaked at 10,857 MB. `ListEntries` makes about five queries per entry (header, lines, an account lookup per line, the
company's currency), so most of its time is that per-entry pattern, not the volume of data; one flat read of the
narrow rows was not measured. Paging the rows once they were in memory took 140 ms. What the measurement does show is
that memory and time grow with the table, whatever the per-row cost.

A database sorts, filters, offsets, limits and counts by itself. What it lacks is morph's interpretation of a
`TableQuery`: which columns exist and may be read, the bounds of `QueryLimits`, which operators a column's kind
defines, how an operand parses for its kind, and that a page request is clamped rather than refused. If every model
re-derived those from the raw `TableQuery`, two models would answer one query two ways, and neither would match
`apply`. The seam makes the interpretation one function whose output a model translates, and makes `apply` the
oracle the translation is held to.

## Shape

```text
 TableQuery (untrusted body)
      │  table::planQuery(query, columns, limits, mayRead, services, translatable)
      ▼
 QueryPlan  — validated, clamped, operands parsed to their column's kind
      │  the model translates it into its own query (SQL, an index scan, a remote call)
      ▼
 Page<Row> { rows, total, totalKind, offset }
```

Header-only, in `include/morph/table/query.hpp` beside `validate` and `apply`; the helper is
`include/morph/table/plan_conformance.hpp`. Neither depends on a database.

## `planQuery`

```cpp
[[nodiscard]] std::expected<QueryPlan, TableError>
planQuery(TableQuery const& query, std::span<ColumnInfo const> columns, QueryLimits const& limits = {},
          std::function<bool(std::string_view)> const& mayRead = {}, Services const& services = {},
          std::function<bool(std::string_view, PlanUse)> const& translatable = {});
```

`planQuery` is, in order: `validate`; the `translatable` refusal; the guarded compile step; a pure projection of the
`CompiledFilter` into `QueryPlan`. `apply` calls `validate` and the same guarded compile step itself, so the
refusals of `apply` and of `planQuery` are the same values by construction, not by parallel code.

The guarded compile step is one function, `compileFilterGuarded`, which `apply`, `Engine` and `planQuery` all call:
`compileFilter`, with an exception from the collator or the date parser returned as `ServiceFailed`. A throwing
service therefore never escapes any of the three.

```cpp
enum class PlanUse : std::uint8_t { Sort, Filter };
using FilterOp = CompiledFilter::Op;
using PlannedOperand = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string>;

struct PlannedSortKey {
    std::string column;
    ColumnKind kind;
    std::string comparator;   // a Custom column's comparator name; empty otherwise
    SortDirection dir;
};

struct PlannedCondition {
    std::string column;
    ColumnKind kind;
    FilterOp op;
    PlannedOperand value;     // the operand, typed by `kind`; empty for IsEmpty and NotEmpty
    PlannedOperand upper;     // Between's second operand; empty otherwise
    std::string text;         // the operand as the request gave it (Between: the first)
    std::string upperText;    // Between's second operand as given
    bool caseSensitive;       // text operators
};

struct PlannedColumnFilter {
    std::string column;
    Combine combine;
    std::vector<PlannedCondition> include;   // empty means no condition
    std::vector<PlannedCondition> exclude;   // a row matching any is rejected
};

struct PlannedGroup {
    std::vector<PlannedColumnFilter> columns;   // a row matches when the entries match on any column
};

struct QueryPlan {
    std::vector<PlannedSortKey> sort;           // most significant first
    std::vector<PlannedColumnFilter> filters;   // ANDed, in evaluation order
    std::vector<PlannedGroup> groups;           // ANDed with `filters`
    std::int64_t offset;                        // in [0, limits.maxOffset]
    std::int64_t limit;                         // clamped to limits.maxLimit; zero is a count-only request
    std::int64_t totalCap;                      // QueryLimits::totalCap
};
```

- A group's columns carry `combine` as the group's, and their `exclude` is always empty: a group has no exclude.
- An entry that applies to none of a group's columns is the error `compileFilter` returns; an entry that applies to
  some appears only under those columns.
- `PlannedOperand` is `std::int64_t` for `Bool` (0 or 1), as the engine holds it.
- An empty filter produces empty `filters` and `groups`.
- `planQuery` does not touch rows and is deterministic for equal inputs.
- A plan has no `wantTotal`: a model always reports a total (see Totals). `limit == 0` is the count-only request.

## Order: the model's row key

`apply` orders rows by the sort chain and breaks every tie by source row, so its order is total and stable, and an
empty sort is source order. A store has no source order, and an `ORDER BY` on a non-unique key, or none, makes
`OFFSET`/`LIMIT` pages repeat or skip rows. The contract on a store-backed model:

- It names a unique, stable row key, and its source order is ascending by that key.
- It appends that key, ascending, as the last term of every ordering, including the empty sort.
- The oracle is `apply` over the same rows supplied in that key order.

The plan does not carry the key: `RowId` is the model's.

## Semantics a translation keeps

The engine's rules (engine.md, "Keys and ordering" and "Filters") are the contract; a store's defaults differ from
them in these places, and the translation must not inherit the default.

- **Empty and invalid cells sort after valid ones in either direction.** A store's `NULL` ordering is its own
  (first ascending in some, last in others); the translation orders by "is empty" first, then by the value.
- **No operator but `IsEmpty` and `NotEmpty` matches an empty or invalid cell.** `IsEmpty` matches empty cells only,
  so `NotEmpty` also matches invalid ones; a typed store has no invalid cells, and the model states how its columns
  map (an unparsable stored value is a model decision, and the conformance corpus covers what it chose).
- **`exclude` keeps rows whose cell is empty.** A row matches no exclude entry on an empty cell, so it is kept. The
  natural `NOT (x = 'a')` is unknown for a missing `x` and drops the row; render an exclude as "the cell is present
  and meets the condition", negated. The same holds for `Combine::All` and for group entries.
- **Ties break by the row key** (above).

## Per-kind translation

Obligations on the store's query; SQL is an illustration.

| Kind | Operand | Obligation |
|---|---|---|
| `Integer`, `Key` | `Rational` | The operand may be non-integral: `eq 1.5` matches nothing, `lt 1.5` matches values up to 1. Do not round the operand. |
| `Decimal` | `Rational` | Compare exactly. A value stored as numerator and denominator is compared by cross-multiplication or a common scale, never by a floating-point quotient. |
| `Quantity` | `Rational` in the canonical unit | Compare `amount * toCanonical`. A store holding amount and unit per row compares canonical values. |
| `Number` | `double` | A NaN cell is invalid, not a value. |
| `Date` | `int64` days since 1970-01-01 | Compare days. |
| `DateTime` | `int64` whole UTC seconds | The engine floors cells to seconds. A store holding finer instants (milliseconds) orders and ties by the floored second, and `eq` is a range over that second. |
| `Bool` | `int64` 0 or 1 | `false` before `true`. |
| `Text` | see [Text](#text) | |
| `Custom` | none | Ordered by the named comparator; see [Refusing](#refusing-what-a-store-cannot-translate). |

An enum is a `Text` column holding the enumerator's name; the store's ordinal orders differently, so an enum is
translated as text (a mapped name column) or refused.

## Text

The engine compares a text operand to a cell after both pass through the collator's `fold`, and orders text by the
collator's `sortKey`. A store holds neither. `PlannedCondition::value` holds the folded operand (raw when
`caseSensitive`), and `text` holds the operand as given, so the model can use its store's mechanism.

Text results equal the oracle's only when the store compares and orders by the same collator. Three ways satisfy that:

- a stored fold column and a stored sort-key column, written through the same collator, which the query compares and
  orders by; or
- a store collation documented to coincide with the collator. The default collator folds ASCII and Latin-1 letters
  and orders by code point, and SQLite's `NOCASE` folds exactly ASCII, so `COLLATE NOCASE` reproduces the default
  collator over ASCII data; or
- an expression the store evaluates over the stored text that equals the collator's fold, which the query compares and
  orders by. It is exact when the expression maps every input to the collator's output byte for byte: for the default
  collator that is the ASCII lower-casing plus the lowering of each two-byte Latin-1 capital (`C3 80` to `C3 9E`
  except `C3 97`), where no replacement's output is the input of another, and the store's own lower-casing folds
  nothing the collator leaves alone. Whether a given store's function does is a property of that store's build, so
  the model's conformance run against the real store is what holds it.

A text column for which none of them holds is refused in sort and filter through `translatable`, never answered
differently. A case-sensitive pattern needs a case-sensitive mechanism (`instr`, `GLOB`, a binary collation): a
store's `LIKE` may be case-insensitive by default, so `escapeLikePattern` alone is enough only on the folded path.

## Refusing what a store cannot translate

Some queries the engine accepts have no translation: a `Custom` sort key (its comparator is a function), a column
that is a serialisation or is computed, text under a non-matching collation. `planQuery` takes a predicate
`translatable(column, use)`, asked of the request's columns (including every column of a group, whether or not the
group's entries apply to it); a sort or filter on a column it refuses is `TableError{Unavailable, column}` with a
message naming the column. The predicate runs after `validate`, so the usual refusals come first and are identical to
`apply`'s. The conformance helper compares `apply` and the model on the queries the predicate accepts, and checks
that the refusals are equal values. Which columns a client may offer to sort and filter belongs in the schema and is
not part of this seam.

## Totals

`Page<Row>` has `TotalKind totalKind = TotalKind::Exact`, with `enum class TotalKind { Exact, AtLeast }`, written in JSON
by name, `"exact"` and `"atLeast"`, as the module's other wire enums are:

- `Exact`: `total` is the count of the filtered view.
- `AtLeast`: `total` is a lower bound and the view has at least that many rows; a client shows "100,000 or more".
- `total` absent: not counted.

`apply` reports `Exact` at or below `QueryLimits::totalCap` and omits the total above it. A model
either counts exactly, at any size, and reports `Exact`; or counts up to `totalCap + 1` rows and, when it found more
than `totalCap`, reports `AtLeast` with `total = totalCap`. Rules:

- An estimate (a statistics table, a cached count) is never `Exact`; a model may report one only as absent.
- The count and the page run in one read transaction, so `total` agrees with the rows.
- The count covers the model's own scope (company, visibility) ANDed with the plan's filter, as the page does.
- A `limit` of zero returns the count and no rows.
- The conformance helper holds an `Exact` total to the true count on either side of the cap: where `apply` omits the
  total it counts the view itself.

`PageWindow<Row>::totalKind()` returns the kind of the latest reply that carried a total; `reset` clears it with the
total. `PageWindow` stops fetching past an `Exact` total only; past an `AtLeast` total it keeps fetching until a
reply is short. A glaze-read `Page` without the member reads `Exact`.

## Conformance

`conformanceFailures` in `include/morph/table/plan_conformance.hpp` runs a corpus of queries through `apply` over the
rows in key order and through the model's callback `PlanModel` (`QueryPlan -> ModelPage`, the page's row keys, offset, total and
total kind), and returns one description per
disagreement: rows (compared by key), offset, total and kind, and refusals (compared by code and column).
`standardCorpus` derives queries from a source's own cells: every operator each column's kind defines, both sort
directions on every column, an offset past the end, a zero limit, an exclude, a group. An operand is the cell's text
as `displayText` shows it, except that a `DateTime` cell (whole UTC seconds) is written as ISO 8601 UTC
(`2026-11-10T09:00:00Z`) and a `Date` cell (days) as `YYYY-MM-DD`: bare numbers do not parse as dates, so a query
built from them would be refused by `apply` and by the plan alike and compare nothing.

Identical refusals count as agreement, so `conformanceFailures` also guards against a corpus that compared nothing: it
reports a column that the corpus filters with an operand (or, for sort-only queries, orders) when at least one query on it was
refused identically by `apply` and the plan and none was compared with the model. A query the model's `translatable`
refuses is skipped and counts as neither, so a column the model declares untranslatable is not reported.

It lives under `include/morph/table`, not in the test tree, so a model in another tree includes it; it depends on
neither a test framework nor a database, returning descriptions for the caller's test to assert on. morph does not
depend on a database, so the framework test runs it against an in-memory reference model, a plan walker written from
the plan alone, with the total cap lowered so the above-cap rules run on a small table. A model over a store runs it
against its real store: that run belongs to the module that owns the store.

The corpus is only as discriminating as the data. The data a store-backed model feeds it must include duplicate sort
keys, empty cells in each column, accents and case in text, sub-second date-times, quantities in more than one unit,
and non-integral operands on integer columns. `standardCorpus` takes its operands from the rows' own cells and adds a
half-step operand (`eq 1.5`, `lt 1.5`) for every integer and key column, so a translation that rounds an operand is
reported; the caller chooses the rows for the rest.

## Aggregate reports

A report that sums rows (a trial balance, stock on hand) computes its rows in the store, as a `SUM ... GROUP BY`, and
pages them one of two ways: with `apply` when the aggregate is small (a few hundred accounts), or with a plan
translated over the aggregate as a subquery: filters on the group key go to the inner `WHERE`, filters on the sums to
an outer `WHERE`, and the ordering to the outer query. A store-side sum of decimals needs a common scale (a value
held as numerator and denominator is not summed as a floating-point quotient). The plan does not describe the
aggregation, and `planQuery` is not given it.

## What the seam does not do

- It does not translate to SQL; morph has no ORM.
- It does not aggregate; if two modules hand-roll "aggregate in the store, then `apply`", an aggregate seam is due.
- It does not offer keyset paging: `PageRequest` is offset-based and a client fetches by offset. An offset costs time
  linear in the offset in most stores, up to `QueryLimits::maxOffset`.
- It does not change `validate` or the defaults of `QueryLimits`.

## Alternatives considered

- **A SQL dialect in morph.** Couples morph to databases and to one dialect per backend; the model already knows its
  store, and the plan is the part that is not store-specific.
- **Streaming every row to the engine.** Memory and time grow with the table, which is what was measured.
- **Loading the narrow rows in one query, then `apply`.** Removes the per-row queries but not the growth with the
  table: at 10^7 rows the resident rows do not fit the criteria.
- **Each model validating the raw `TableQuery`.** Re-derives the operator and operand rules per model and drifts from
  `apply`.
- **A boolean `totalExact`.** A present total is already exact, so it distinguished nothing a client can show.

## Failure modes

| Condition | Result |
|---|---|
| Query over a limit, unknown or hidden column | the `TableError` `validate` returns |
| Column or use the model's `translatable` refuses | `Unavailable`, naming the column |
| Operator the kind does not define, or an operand that does not parse | the `TableError` `compileFilter` returns |
| The collator or the date parser throws while operands compile | `ServiceFailed`, from `apply`, `Engine` and `planQuery` alike |
