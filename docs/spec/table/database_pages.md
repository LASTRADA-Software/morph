# `morph::table` — database pages

Design spec for the seam that lets a model which holds its rows in a database page them there:
`table::plan`, the validated and normalised query, and `Page::totalExact`, a total the source counted itself above
`QueryLimits::totalCap`. Read [`engine.md`](engine.md) first; this file adds to its "Server mode" section and changes
nothing in `apply`.

## Contents

- [Why](#why)
- [Shape](#shape)
- [`plan`](#plan)
- [`Page::totalExact`](#pagetotalexact)
- [What a model does with a plan](#what-a-model-does-with-a-plan)
- [What the seam does not do](#what-the-seam-does-not-do)
- [Alternatives considered](#alternatives-considered)
- [Verification](#verification)
- [Failure modes](#failure-modes)

## Why

`apply` takes the server's rows as a `DataSource` and sorts, filters and slices them in memory. A list action over a
table that lives in a database then has to read every row of the table before it can return fifty of them.
Measured on lims's ledger (Release build, SQLite, one process): `ListEntries` over 10^6 entries took 527.6 s and its
peak resident memory went from 46 MB to 2,777 MB; a trial balance over 10^7 posted lines took 168.2 s and peaked at
10,857 MB. The page itself, once the rows were in memory, took 140 ms. The cost is loading the rows, not paging them.

A database can sort, filter, offset, limit and count by itself. What it lacks is morph's interpretation of a
`TableQuery`: which columns exist and may be read, the bounds of `QueryLimits`, which operators a column's kind
defines, how an operand parses for its kind, and that a page request is clamped rather than refused. If every model
re-derived those from the raw `TableQuery`, two models would answer one query two ways, and neither would match
`apply`. The seam makes the interpretation one function whose output a model translates.

## Shape

```text
 TableQuery (untrusted body)
      │  table::plan(query, columns, limits [, mayRead, services])
      ▼
 QueryPlan  — validated, clamped, operands parsed to their column's kind
      │  the model translates it into its own query (SQL, an index scan, a remote call)
      ▼
 Page<Row> { rows, total, totalExact, offset }
```

Header-only, in `include/morph/table/query.hpp` beside `validate` and `apply`. No new dependency; morph still knows
nothing of databases.

## `plan`

```cpp
[[nodiscard]] std::expected<QueryPlan, TableError>
plan(TableQuery const& query, std::span<ColumnInfo const> columns, QueryLimits const& limits = {},
     std::function<bool(std::string_view)> const& mayRead = {}, Services const& services = {});
```

The columns are `ColumnInfo` (the row schema `DataSource::columns()` returns), the same span `validate` and
`compileFilter` take.

`plan` is `validate` followed by `compileFilter`, with the result expressed as data a model can walk instead of as the
engine's key-column machinery. It returns the first error `validate` or `compileFilter` would, with the same code and
column, so a model's refusal and `apply`'s refusal are the same value.

```cpp
struct PlannedSortKey {
    std::string column;   // a column of the row schema
    ColumnKind kind;      // how the column orders
    SortDirection dir;
};

using Operand = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string>;

struct PlannedCondition {
    std::string column;
    ColumnKind kind;
    CompiledFilter::Op op;          // Eq, Ne, Lt, Le, Gt, Ge, Between, Contains, StartsWith, EndsWith, IsEmpty, NotEmpty
    Operand value;                  // the operand, typed by `kind`; empty for IsEmpty and NotEmpty
    Operand upper;                  // Between's second operand; empty otherwise
    bool caseSensitive = false;     // text operators only
};

struct PlannedColumnFilter {
    std::string column;
    Combine combine;                           // how `include` entries combine
    std::vector<PlannedCondition> include;     // empty means no condition
    std::vector<PlannedCondition> exclude;     // a row matching any is rejected
};

struct PlannedGroup {
    std::vector<PlannedColumnFilter> columns;  // a row matches when the entries match on any of the columns
};

struct QueryPlan {
    std::vector<PlannedSortKey> sort;          // most significant first; empty means source order
    std::vector<PlannedColumnFilter> filters;  // ANDed, in the engine's evaluation order
    std::vector<PlannedGroup> groups;          // ANDed with `filters`
    std::int64_t offset;                       // in [0, limits.maxOffset]
    std::int64_t limit;                        // clamped to limits.maxLimit
    bool wantTotal;                            // whether the page carries a total
    std::int64_t totalCap;                     // QueryLimits::totalCap
};
```

- Operand typing follows `compileFilter`: `Integer`, `Key`, `Decimal` and `Quantity` columns carry a `Rational`;
  `Number` a `double`; `Date`, `DateTime` and `Bool` an `int64`; `Text` a `std::string`, folded with the services'
  collator unless `caseSensitive`. A model translates the operand it finds, never the raw string.
- A group entry that applies to none of the group's columns is the same `InvalidSpec` or operand error `compileFilter`
  returns; an entry that applies to some columns appears only under those columns, as in the engine.
- An empty `FilterSpec` produces empty `filters` and `groups`. `wantTotal` is `true` unless the plan's limit is zero
  and the model does not need a count; a model that counts anyway sets the total (see below).
- `plan` does not touch rows, does not allocate per row, and is deterministic for equal inputs.

`apply` is reimplemented on `plan`'s validation step only; its sort, filter and slice code is unchanged, so `apply`
remains the reference and the oracle.

## `Page::totalExact`

`Page<Row>` gains `bool totalExact = false`. `PageRows` gains the same field for symmetry.

- `apply` never sets it: it reports a total at or below `totalCap` and omits it above.
- A database-backed model that counted the filtered view itself sets `total` to the count and `totalExact` to `true`
  whatever the count. The client then shows "1,000,000 entries", never "more than 100,000".
- `totalExact == true` with `total` absent is not produced; a reader treats it as no total.
- `PageWindow` keeps a known total across a reply that omits it, as before, and takes `totalExact` from the latest
  reply that carried a total. A client that does not read the field is unchanged: it sees a larger `total` than the
  engine ever produced, which it already handles as any count.
- The wire form is the glaze form of `Page`; the new member defaults to `false`, so a reply without it reads as
  before.

## What a model does with a plan

1. Call `plan` with the row schema's columns, the principal's `mayRead`, and the server's limits.
2. Translate `sort`, `filters`, `groups`, `offset` and `limit` into the store's query. `escapeLikePattern` keeps `%`
   and `_` ordinary in `Contains`, `StartsWith` and `EndsWith`.
3. Run the page query; when `wantTotal`, run the count over the same filter, ignoring `offset` and `limit`.
4. Return `Page<Row>{rows, total, totalExact = true, offset}`.

The translation, and the store's collation, belong to the model. Text order is the server's collator for `apply`; a
store whose collation differs from the services' orders text differently, and the model owns that difference.

## What the seam does not do

- It does not translate to SQL. morph has no ORM; lims's Lightweight is the example's.
- It does not aggregate. A trial balance is a sum, not a page; it needs the store's `SUM ... GROUP BY`, which
  `plan` does not describe. The seam covers the list half of the second instance, not the report half.
- It does not make `apply` faster or stream rows into the engine.
- It does not change `validate`'s behaviour or `QueryLimits`'s defaults.

## Alternatives considered

- **A SQL dialect in morph.** Couples morph to databases and to one dialect per backend; the model already knows its
  store, and the plan is the part that is not store-specific.
- **Streaming every row to the engine.** What is measured as too slow: the cost above is the load.
- **Each model validating the raw `TableQuery`.** Re-derives the engine's operator and operand rules per model and
  drifts from `apply`.

## Verification

`tests/test_table_database_pages.cpp`, tag `[table][database_pages]`:

- For every operator on every column kind it defines, a source filtered and sorted by walking the plan's conditions
  returns the same pages as `apply` over the same rows: first, middle and last page, an offset past the end, and a
  limit of zero. The walker is written in the test from the plan alone; it must not call the engine's matcher.
- `plan` refuses what `validate` and `compileFilter` refuse, each with the same code and column: offset out of range,
  negative limit, too many sort keys, filtered columns, entries, groups, group columns, an over-long value, an unknown
  column, a column `mayRead` hides, an operator the kind does not define, an operand that does not parse, an entry
  with no operator or two.
- `limit` is clamped in the plan; `wantTotal` and `totalCap` carry the limits.
- `totalExact` round-trips through glaze, a reply without it reads as `false`, and `PageWindow` keeps a total across a
  reply that omits one.

Each refusal and each operator has a mutation: change the walker's treatment of that operator, or drop the check from
`plan`, and its case fails.

## Failure modes

| Condition | Result |
|---|---|
| Query over a limit, unknown or hidden column | the `TableError` `validate` returns |
| Operator the kind does not define, or an operand that does not parse | the `TableError` `compileFilter` returns |
| A service (collator, date parser) throws while operands are parsed | `ServiceFailed` |
