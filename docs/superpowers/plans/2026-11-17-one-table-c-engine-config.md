# The One Table, Part C — the Engine's Configuring Additions: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `morph::table` value-set filters and distinct values, column groups with quantifiers and `together`, values with units over a run-time unit registry, and a column layout with widths, pinning and one fitting rule (table.md Delivery order step 3).

**Architecture:** Everything lands in the engine headers (`include/morph/table/{data_source,sort,filter,query,engine}.hpp`) plus one UI parser (`include/morph/ui/measure.hpp`). Filters stay compile-once / evaluate-per-cell: a `oneOf` becomes one hashed set, a group becomes per-cell entry bit masks found by one value-index lookup, `Together` is a bipartite matching by augmenting paths, and every evaluation is three-valued (`Match`) over unloaded cells. A `Measure` key is a pair (dimension place, exact base amount) built from tables indexed by `UnitId`, so key building never looks a unit up by name.

**Tech Stack:** C++23, header-only morph, glaze, Catch2 (`morph_table_tests`), MSVC preset `cl-debug` (timings on `cl-release`).

**Spec:** `docs/spec/table/table.md` — "Filters from the header" (value-set operator, distinct values, date buckets), "Column groups", "Values with units" (all subsections), "Columns: order, visibility, pinning, width" (the engine half), "Semantics of an unloaded cell" ("Unloaded cells in filters", "Unique values"), Invariants 1, 22–25, Tests rows "`oneOf`", "Distinct values", "Column groups", "Values with units", "Unit numbers across registries", "Column layout" (engine half), "The generic page" (measure half), Benchmarks D1–D6, G1–G7, M1–M5, Neighbouring specs row `../forms/views.md` (step 3), Delivery order step 3. Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md`, Task 8.
Part C of 7; index: 2026-11-17-one-table.md

## Global Constraints

Every line of the roadmap's Global Constraints applies. The ones this part touches, verbatim:

- AGENTS.md "Comments and documentation": a comment or spec states what the code does now and why; no history, no issue numbers, no commit hashes.
- Every public symbol has complete Doxygen `@param`/`@tparam`/`@return`; the docs build runs `WARN_AS_ERROR = FAIL_ON_WARNINGS` (`cmake -S . -B build/doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF && cmake --build build/doc --target doc`).
- morph is public: no company, product or customer names in code, tests, specs or commit messages; examples use the neutral `lims` domain.
- Commits are authored `yaraslau <mail@yaraslau.com>` and end with the session's attribution lines.
- AGENTS.md "Verify rather than assert": every test names the mutation it was seen to fail on, and the task runs that mutation once and records the failing output in its commit message body.
- Limits, verbatim: `QueryLimits::maxSetValues = 10'000`, `maxQuerySetValues = 20'000`, `maxQuerySetBytes = 1 MiB` (1'048'576 bytes), `maxGroupColumns = 16`; `DistinctRequest::limit = 200`.
- Timings are never CI gates. Count gates (D3, D5, D6, G5, G7, M5) are ordinary tests; timings (D1–D4, G1–G4, G6, G7, M1–M4) are `[.benchmark]` cases in `tests/table/bench_table.cpp`, run on `cl-release`, and written into table.md's "Benchmarks: configuring", "Benchmarks: column groups" and "Benchmarks: values with units" tables in the same commit.
- No GUI application is launched.

Commands (the roadmap's standard loop):

```bash
cmake --preset cl-debug
cmake --build --preset cl-debug --target morph_table_tests
ctest --test-dir build/cl-debug -R "<pattern>" --output-on-failure
# timings only:
cmake --preset cl-release && cmake --build --preset cl-release --target morph_table_tests
build/cl-release/tests/morph_table_tests.exe "[.benchmark][<tag>]"
```

## Review Focus

1. **An empty or repeating value set.** `oneOf: []` (a menu with nothing ticked) matches no row in `include` and rejects no row in `exclude`; `["alpha", "ALPHA"]` on a folded text column is one member, matches the same rows as `["alpha"]`, and counts as two toward `maxSetValues`. Test goes to Task 8a (`"table: an empty or repeating oneOf"`).
2. **A values-list search that is not text.** On a decimal column a search `"1.5"` finds `11.5` and `1.5` (substring of the value's text), an accented search `"É"` finds `"école"` and `"Ecole"` under the collator's fold, and an `offset` past the end returns no values while `distinct` still counts them. Test goes to Task 8a (`"distinct: search on numbers and accents, and an offset past the end"`).
3. **A group naming a column twice, `exactly 0`, and `together` over too many entries.** `columns: ["a", "a"]` is `InvalidSpec` (a matching that may use a column twice is not `Together`); `exactly 0` passes exactly the rows where no column satisfies; `together` with 4 entries over 3 columns is `InvalidSpec`. Test goes to Task 8b (`"groups: duplicate columns, exactly zero and too many together entries"`).
4. **Negative and zero amounts with units.** `-300 g` sorts before `-0.2 kg`; `0 g`, `0 kg` and a plain `0` in a mass-only column: the first two tie and are one distinct value, the third is `InvalidValue` as an operand; `-0.5 %` sorts in the ratio group after every mass in both directions. Test goes to Task 8c (`"measure: negative and zero amounts"`).
5. **A stored layout that repeats itself.** An `order` naming an id twice keeps its first place; a width of `0` or `-5` is dropped; `pinned: 40` over three visible columns is 3; a group listing a column twice keeps it once. Test goes to Task 8d (`"column layout: a stored layout that repeats itself"`).

---

## Decisions

- **`x-units` is not emitted.** table.md 2534-2536 has a typed row's schema list the column's units as `x-units`. That schema is `forms::schemaJson<Row>()`, built from the type alone, while the units are the application's run-time registry (2500-2502), and 2505 says a client learns units "from the table's description … and from nowhere else". 8c therefore marks a `MeasureValue` property `x-measure` only, and the units reach a client in `TableDescription::units` (filled by `DescribeTable`). The spec's `x-units` sentence should be changed to say that. Reading a document row's `{num, den, unit}` in `ValueRowsSource` is Task 11a's, and dropping a stored display unit that names a removed unit is Task 12c's (the index assigns both).

## Order inside Part C

As the index's cross-part decision 17 fixes: 8a needs Task 3; **8b needs 8a** (its pooled values list extends `distinctValues`, and its `Together` value index reuses 8a's `ValueSet`); **8c needs Tasks 1, 2 and 5** (the generic page, the describe reply, the level cache); 8d is independent. 8a, 8b and 8c all touch `filter.hpp` and `query.hpp`: they merge one at a time, in the order 8a, 8b, 8c, each rebased on the last.

## Interfaces consumed from other parts

These are the names this part relies on. Where the roadmap does not pin a detail, the assumption is stated; the implementer checks it against the landed code first and adapts only the call, never the name another task produced.

- **Task 3 (Part B):** `struct Unloaded {}` (a `Cell` alternative after `QuantityCell`); `CellState::Unloaded`; `enum class Match : std::uint8_t { No, Yes, Unknown }`; `static Match CompiledFilter::evaluate(Column const&, KeyColumn const&, std::uint32_t)`; `passes == (evaluate == Yes)`; `static Match CompiledFilter::evaluateGroup(Group const&, std::span<KeyColumn const* const> keys, std::uint32_t row)` (keys by column index; the plain-group rule, which 8b replaces with quantifiers); `Match CompiledFilter::evaluateRow(std::span<KeyColumn const* const>, std::uint32_t) const`; `FilterRun::take() &&` and `FilterRun::takeUndecided() &&`, whose group stage calls `evaluateGroup`; `PageRows::{TotalKind totalKind; std::int64_t undecided; …}`; `Engine::undecidedRowCount()`; `TableErrorCode::SourceContract` (appended last); `ColumnInfo::level` (a member after `format`).
- **Task 1 (Part A):** `TableDescription{…, groups, levels, units, …}`, `ColumnGroupInfo{id, label, columns}` with `operator== = default`, `GenericPage`, `writeGenericPage(PageRows const&, DataSource const&, std::span<std::string const>)`, `readGenericPage(std::string_view, TableDescription const&)`. Task 1 leaves `TableDescription::units` out; **8c adds it after `levels`**. `UnitInfo` does not exist before 8c.
- **Task 2 (Part A):** `ServedTable{description, source, levels, mayRead, scope, services, limits}`; `DescribeTable<F>::invoke` returning `describe(served.description)`; `table::detail::answerQuery(ServedTable const&, TableQuery const&, AnswerContext const& = {})` (index decision 5).
- **Task 4 (Part B):** `TableQuery::keys` (`std::optional<std::vector<RowId>>`, after `columns`); `apply` = `validate` -> `detail::computeView` -> `detail::pageRows`.
- **Task 5 (Part B):** the spec's block verbatim: `CellLevelCache`, `LevelCacheOptions` (no `units` member; 8c adds it), `LevelCacheStats::{loaderCalls, invalidatedKeys}`, `LevelSource::snapshot()`, `LevelSnapshot::read`, `invalidateAll()`; `cellBytes(Cell const&)` in `data_source.hpp` (`sizeof(Cell)` for a `Measure`, which owns no heap).
- **Task 7 (Part B):** `Page<Row>::openMembers()` already returns `{"awaiting", "distinct"}`; 8a only adds the `distinct` member it names.
- **Task 0 (Part A):** `bench::WideTableSource`, `bench::WideTableOptions{rows, columns, seed, loadDelay}` in `tests/table/wide_table_model.{hpp,cpp}`. `bench::WideColumn{info, role, sparse, level, group}` from `WideTableSource::layout()`: `m000`...`m009` have role `Measure` and `ColumnKind::Decimal` until 8c; `t004`...`t006` carry `group = "g3"`, `t011`...`t026` `group = "g16"`.

---

### Task 8a: `oneOf` and distinct values

**Spec:** "Filters from the header" (the value-set operator; "Distinct values" with every bullet); "Unique values"; invariants 22, 23; Tests rows "`oneOf`", "Distinct values"; gates D3, D5, D6; timings D1–D4.

**Files:**
- Modify: `include/morph/table/filter.hpp` (`FilterEntry::oneOf`, `CompiledFilter::Op::OneOf`, `ValueSet`, `FilterCounters`, `detail::readEntry`, `compileEntry`, `CompiledFilter::matches`)
- Modify: `include/morph/table/query.hpp` (`QueryLimits::{maxSetValues, maxQuerySetValues, maxQuerySetBytes}`, `DateBucket`, `DistinctRequest`, `DistinctValue`, `DistinctValues`, `distinctValues`, `TableQuery::distinct`, `PageRows::distinct`, `Page<Row>::distinct`, `validate`, `apply`, `detail::entryTooLong`)
- Modify: `include/morph/table/engine.hpp` (`Engine::distinctValues`)
- Modify: `include/morph/table/description.hpp` (`GenericPage::distinct`, its writer and reader)
- Create: `tests/table/test_table_distinct.cpp`; modify `tests/table/test_table_filter.cpp`, `tests/table/test_table_query.cpp`, `tests/table/bench_table.cpp`, `tests/CMakeLists.txt` (add `table/test_table_distinct.cpp` to `morph_table_tests`)

**Interfaces:**
- Consumes: Task 3's `Match`, `CellState::Unloaded`, `CompiledFilter::evaluate`, `PageRows::undecided`; Task 1's `GenericPage`.
- Produces:
  ```cpp
  // filter.hpp
  struct FilterEntry { /* existing members, then: */ std::optional<std::vector<std::string>> oneOf; };
  enum class CompiledFilter::Op : std::uint8_t { Eq, …, NotEmpty, OneOf };   // OneOf appended last
  struct FilterCounters { std::uint64_t setLookups = 0; std::uint64_t cellReads = 0; std::uint64_t augmentations = 0; };
  class ValueSet;   // the members of one oneOf, parsed for one column; one lookup per cell
  // CompiledFilter::Entry gains: std::shared_ptr<ValueSet const> set;   (set for OneOf only)
  static Match CompiledFilter::evaluate(Column const&, KeyColumn const&, std::uint32_t row, FilterCounters* counters);
  // query.hpp
  enum class DateBucket : std::uint8_t { Value, Year, Month, Day };      // JSON "value", "year", "month", "day"
  struct DistinctRequest { std::string column; std::string group; std::string search;
                           DateBucket bucket = DateBucket::Value; std::int64_t offset = 0; std::int64_t limit = 200; };
  struct DistinctValue { std::string text; std::string operand; std::int64_t count = 0; };
  struct DistinctValues { std::vector<DistinctValue> values; std::int64_t distinct = 0;
                          TotalKind distinctKind = TotalKind::Exact; std::int64_t blanks = 0;
                          std::int64_t invalid = 0; bool partial = false; };
  [[nodiscard]] std::expected<DistinctValues, TableError>
  distinctValues(DataSource const& source, TableQuery const& query, DistinctRequest const& request,
                 ApplyOptions const& options = {});
  // TableQuery::distinct, PageRows::distinct, Page<Row>::distinct, GenericPage::distinct:
  std::optional<DistinctRequest> distinct;   // on TableQuery
  std::optional<DistinctValues> distinct;    // on PageRows, Page<Row>, GenericPage
  // engine.hpp
  [[nodiscard]] std::expected<DistinctValues, TableError> Engine::distinctValues(DistinctRequest const& request) const;
  ```

- [ ] **Step 1: Write the failing `oneOf` tests** in `tests/table/test_table_filter.cpp`. Add helpers `oneOf(std::vector<std::string>)` beside `eq(…)`, and, to the header comment's mutation list, "`ValueSet` matching nothing: `oneOf matches what its eq entries match` fails".

```cpp
FilterEntry oneOf(std::vector<std::string> v) { return FilterEntry{.oneOf = std::move(v)}; }

TEST_CASE("table: oneOf matches what its eq entries match", "[table][filter]") {
    Filtered const text{.source = tabletest::column(ColumnKind::Text,
                            {std::string{"Alpha"}, std::string{"beta"}, std::string{"Gamma"}, Cell{},
                             std::string{"alpha"}}),
                        .services = {}};
    CHECK(text.by(on("c", {oneOf({"alpha", "BETA"})})) == Rows{0, 1, 4});
    CHECK(text.by(on("c", {oneOf({"alpha", "BETA"})})) == text.by(on("c", {eq("alpha"), eq("BETA")})));
    CHECK(text.by(on("c", {}, Combine::Any, {oneOf({"alpha", "BETA"})})) == Rows{2, 3});
    CHECK(text.by(on("c", {oneOf({"alpha", "BETA"}), isEmpty()})) == Rows{0, 1, 3, 4});
    FilterEntry exact = oneOf({"alpha"});
    exact.caseSensitive = true;
    CHECK(text.by(on("c", {exact})) == Rows{4});

    Filtered const ints{.source = tabletest::column(ColumnKind::Integer,
                            {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}, std::string{"x"}}),
                        .services = {}};
    CHECK(ints.by(on("c", {oneOf({"2", "1.5"})})) == Rows{1});
    CHECK(ints.by(on("c", {oneOf({"2", "zz"})})).error().code == TableErrorCode::InvalidValue);
}

TEST_CASE("table: oneOf is unknown on an unloaded cell", "[table][filter]") {
    auto const source = tabletest::column(ColumnKind::Text, {std::string{"Alpha"}, Cell{Unloaded{}}, std::string{"b"}});
    auto const page = apply(*source, TableQuery{.sort = {}, .filters = on("c", {oneOf({"alpha"})}), .page = {}});
    REQUIRE(page);
    CHECK(page->rows == std::vector<std::size_t>{0});
    CHECK(page->undecided == 1);
    CHECK(page->totalKind == TotalKind::AtLeast);
}

TEST_CASE("table: an empty or repeating oneOf", "[table][filter]") {   // Review Focus 1
    Filtered const text{.source = tabletest::column(ColumnKind::Text,
                            {std::string{"Alpha"}, std::string{"beta"}, Cell{}}), .services = {}};
    CHECK(text.by(on("c", {oneOf({})})) == Rows{});
    CHECK(text.by(on("c", {}, Combine::Any, {oneOf({})})) == Rows{0, 1, 2});
    CHECK(text.by(on("c", {oneOf({"alpha", "ALPHA"})})) == text.by(on("c", {oneOf({"alpha"})})));
}

TEST_CASE("table: a oneOf of any size is one lookup a cell", "[table][filter]") {   // gate D5
    // 100,000 rows of "v<n % 20000>"; a oneOf of the 10,000 texts "v0".."v9999".
    // For every row r: CompiledFilter::evaluate(compiled.columns()[0], keys, r, &counters).
    CHECK(counters.setLookups == 100'000);
    CHECK(passed == 50'000);
}

TEST_CASE("table: oneOf is the OR of its eq entries over random tables", "[table][filter]") {   // invariant 23
    // std::mt19937 seed 23; 200 tables of 50 rows over kinds Text, Integer, Decimal, Date, Bool;
    // cells drawn from 8 values plus empty and one invalid text; a oneOf of 1..6 drawn values.
    CHECK(f.by(on("c", {oneOf(values)})) == f.by(on("c", eqEntries(values))));          // include, combine any
    CHECK(f.by(on("c", {}, Combine::Any, {oneOf(values)})) ==
          f.by(on("c", {}, Combine::Any, eqEntries(values))));                            // exclude
}
```

(`Filtered`, `on`, `eq`, `isEmpty` are the file's existing helpers; the D5 case builds key columns as `Filtered::by` does and keeps the pointers to call `evaluate` itself.)

- [ ] **Step 2: Write the failing limit tests** in `tests/table/test_table_query.cpp`:

```cpp
TEST_CASE("table query: value sets are bounded per entry and per query", "[table][query]") {
    // texts(n, bytes): n distinct strings of exactly `bytes` bytes.
    auto const columns = std::vector{tabletest::col("a", ColumnKind::Text), tabletest::col("b", ColumnKind::Text),
                                     tabletest::col("c", ColumnKind::Text)};
    auto query = [](std::vector<std::pair<std::string, std::vector<std::string>>> sets) { /* one oneOf include per column */ };
    CHECK(validate(query({{"a", texts(10'000, 16)}}), columns));
    CHECK(validate(query({{"a", texts(10'001, 16)}}), columns).error().code == TableErrorCode::LimitExceeded);
    CHECK(validate(query({{"a", texts(10'001, 16)}}), columns).error().column == "a");
    CHECK(validate(query({{"a", texts(10'000, 16)}, {"b", texts(10'000, 16)}}), columns));
    CHECK(validate(query({{"a", texts(10'000, 16)}, {"b", texts(10'000, 16)}, {"c", texts(1, 16)}}), columns)
              .error().code == TableErrorCode::LimitExceeded);
    CHECK(validate(query({{"a", texts(9'000, 120)}}), columns).error().code == TableErrorCode::LimitExceeded); // 1,080,000 B
    CHECK(validate(query({{"a", texts(1, 257)}}), columns).error().code == TableErrorCode::LimitExceeded);     // maxTextLength
}

TEST_CASE("table query: a 10,000-value set fits the wire bound", "[table][query]") {   // gate D6
    auto const q = /* one oneOf of 10,000 distinct 16-byte texts on column "a" */;
    std::string json;
    REQUIRE_FALSE(glz::write_json(q, json));
    CHECK(json.size() <= 250'000);
    CHECK(json.size() < QueryLimits{}.maxQuerySetBytes);
    CHECK(validate(q, columns));
}
```

- [ ] **Step 3: Write the failing distinct-value tests** in `tests/table/test_table_distinct.cpp`. Header comment lists the mutations: "counting over the view (the column's own filter kept): `counts are those of the rows the other filters leave` fails"; "grouping text by raw text instead of its fold: `values are grouped by the key` fails". The shared source:

```cpp
// status (Text), city (Text), qty (Integer); keys 0..5
// 0: "open",   "Berlin", 1
// 1: "Open",   "Paris",  2
// 2: "closed", "Berlin", 3
// 3: empty,    "Berlin", 4
// 4: "open",   "Paris",  "x"   (invalid integer)
// 5: "hold",   "Rome",   5

TEST_CASE("distinct: counts are those of the rows the other filters leave", "[table][distinct]") {
    TableQuery q;
    q.filters.columns["city"] = ColumnFilter{.combine = Combine::Any, .include = {oneOf({"Berlin", "Paris"})}, .exclude = {}};
    q.filters.columns["status"] = ColumnFilter{.combine = Combine::Any, .include = {eq("closed")}, .exclude = {}};
    auto const d = distinctValues(*source, q, DistinctRequest{.column = "status"});
    REQUIRE(d);
    REQUIRE(d->values.size() == 2);
    CHECK(d->values[0].text == "closed");  CHECK(d->values[0].operand == "closed"); CHECK(d->values[0].count == 1);
    CHECK(d->values[1].text == "open");    CHECK(d->values[1].operand == "open");   CHECK(d->values[1].count == 3);
    CHECK(d->distinct == 2);  CHECK(d->distinctKind == TotalKind::Exact);
    CHECK(d->blanks == 1);    CHECK(d->invalid == 0);  CHECK_FALSE(d->partial);
    // invariant 22: the sum is the row count of the other filters
    CHECK(1 + 3 + d->blanks + d->invalid == 5);
}

TEST_CASE("distinct: invalid cells are counted and listed under no value", "[table][distinct]") {
    TableQuery q;
    q.filters.columns["city"] = ColumnFilter{.combine = Combine::Any, .include = {eq("Paris")}, .exclude = {}};
    auto const d = distinctValues(*source, q, DistinctRequest{.column = "qty"});
    REQUIRE(d);
    REQUIRE(d->values.size() == 1);
    CHECK(d->values[0].text == "2");  CHECK(d->values[0].operand == "2");  CHECK(d->values[0].count == 1);
    CHECK(d->invalid == 1);
}

TEST_CASE("distinct: an unloaded cell or an undecided row sets partial", "[table][distinct]") {
    // the same source with row 0's status Unloaded{}, then (second source) row 1's city Unloaded{}
    CHECK(withUnloadedStatus->partial);        CHECK(withUnloadedStatus->values[1].count == 2);   // "open": rows 1, 4
    CHECK(withUndecidedRow->partial);          CHECK(withUndecidedRow->values[1].count == 2);     // row 1 undecided by city
}

TEST_CASE("distinct: search, paging and the cap", "[table][distinct]") {
    CHECK(names(distinctValues(*source, {}, {.column = "status", .search = "OP"})) == std::vector<std::string>{"open"});
    auto const page = distinctValues(*source, {}, {.column = "status", .offset = 1, .limit = 1});
    CHECK(names(page) == std::vector<std::string>{"hold"});   // ascending: closed, hold, open
    CHECK(page->distinct == 3);
    ApplyOptions capped;
    capped.limits.totalCap = 2;
    auto const atCap = distinctValues(*source, {}, {.column = "status"}, capped);
    CHECK(atCap->distinct == 2);  CHECK(atCap->distinctKind == TotalKind::AtLeast);  CHECK(atCap->values.size() == 3);
    ApplyOptions small;
    small.limits.maxLimit = 2;
    CHECK(distinctValues(*source, {}, {.column = "status", .limit = 500}, small)->values.size() == 2);
}

TEST_CASE("distinct: search on numbers and accents, and an offset past the end", "[table][distinct]") {  // Review Focus 2
    // decimal column [11.5, 1.5, 2]; text column ["école", "Ecole", "x"] with tabletest's accent-folding collator
    CHECK(names(distinctValues(*decimals, {}, {.column = "c", .search = "1.5"})) == std::vector<std::string>{"1.5", "11.5"});
    CHECK(names(distinctValues(*texts, {}, {.column = "c", .search = "É"}, foldingOptions)).size() == 1); // one folded value
    auto const past = distinctValues(*decimals, {}, {.column = "c", .offset = 10});
    CHECK(past->values.empty());  CHECK(past->distinct == 3);
}

TEST_CASE("distinct: dates by year, month and day", "[table][distinct]") {
    // Date column (days) for 2025-12-31, 2026-01-15, 2026-01-20, 2026-02-01
    auto const years = distinctValues(*dates, {}, {.column = "d", .bucket = DateBucket::Year});
    CHECK(years->values[0].text == "2025");  CHECK(years->values[0].operand == "2025-01-01..2025-12-31");  CHECK(years->values[0].count == 1);
    CHECK(years->values[1].text == "2026");  CHECK(years->values[1].operand == "2026-01-01..2026-12-31");  CHECK(years->values[1].count == 3);
    auto const months = distinctValues(*dates, {}, {.column = "d", .bucket = DateBucket::Month});
    CHECK(names(months) == std::vector<std::string>{"2025-12", "2026-01", "2026-02"});
    CHECK(months->values[1].operand == "2026-01-01..2026-01-31");  CHECK(months->values[1].count == 2);
    auto const days = distinctValues(*dates, {}, {.column = "d", .bucket = DateBucket::Day});
    CHECK(days->values[1].text == "2026-01-15");  CHECK(days->values[1].operand == "2026-01-15..2026-01-15");
    // A DateTime column: Day bucket of 2026-01-15T10:00:00Z has operand "2026-01-15T00:00:00Z..2026-01-15T23:59:59Z".
    CHECK(distinctValues(*source, {}, {.column = "status", .bucket = DateBucket::Year}).error().code == TableErrorCode::InvalidSpec);
}

TEST_CASE("distinct: requests are validated as queries are", "[table][distinct]") {
    CHECK(distinctValues(*source, {}, {.column = "status", .group = "g"}).error().code == TableErrorCode::InvalidSpec);
    CHECK(distinctValues(*source, {}, {}).error().code == TableErrorCode::InvalidSpec);
    CHECK(distinctValues(*source, {}, {.column = "nope"}).error().code == TableErrorCode::UnknownColumn);
    ApplyOptions hidden;
    hidden.mayRead = [](std::string_view id) { return id != "status"; };
    CHECK(distinctValues(*source, {}, {.column = "status"}, hidden).error().code == TableErrorCode::ColumnNotReadable);
    CHECK(distinctValues(*source, {}, {.column = "status", .search = std::string(257, 'a')}).error().code == TableErrorCode::LimitExceeded);
    CHECK(distinctValues(*source, {}, {.column = "status", .offset = 10'000'001}).error().code == TableErrorCode::LimitExceeded);
    CHECK(distinctValues(*source, {}, {.column = "status", .offset = -1}).error().code == TableErrorCode::LimitExceeded);
    TableQuery keyed;
    keyed.keys = std::vector<RowId>{std::int64_t{1}};
    keyed.distinct = DistinctRequest{.column = "status"};
    CHECK(validate(keyed, source->columns()).error().code == TableErrorCode::InvalidSpec);
}

TEST_CASE("distinct: the column's level is an order level of the request", "[table][distinct]") {
    // the shared source with status declared at level "slow" (tabletest::col then .level = "slow")
    TableQuery q;
    q.distinct = DistinctRequest{.column = "status"};
    q.columns = std::vector<std::string>{"city"};
    auto const needed = levelsNeeded(q, source->columns(), {});
    CHECK(needed.order == std::vector<std::string>{"slow"});   // Mutation: levelsNeeded ignoring query.distinct
}

TEST_CASE("distinct: server mode answers through apply, client mode through the engine", "[table][distinct]") {
    TableQuery q;   // the city filter of the first case
    q.distinct = DistinctRequest{.column = "status"};
    auto const page = apply(*source, q);
    REQUIRE(page);
    CHECK(page->rows.empty());
    REQUIRE(page->distinct);
    CHECK(*page->distinct == *distinctValues(*source, q, *q.distinct));   // DistinctValues and DistinctValue are ==-comparable
    Engine engine{source};
    REQUIRE(engine.setFilter(q.filters));
    CHECK(*engine.distinctValues(*q.distinct) == *distinctValues(*source, q, *q.distinct));
}

TEST_CASE("distinct: invariant 22 over random tables", "[table][distinct]") {
    // std::mt19937 seed 22; 100 tables of 300 rows, columns c0 (Text), c1 (Integer), c2 (Text);
    // random filters on all three; for column c0 with partial == false:
    CHECK(sumOfCounts + d->blanks + d->invalid == *apply(*source, queryWithoutC0Filter)->total);
}
```

Also in `test_table_description.cpp`: `"generic page: a distinct reply round-trips"` — a `GenericPage` with no rows and `distinct` holding the first case's values reads back equal.

- [ ] **Step 4: Run the new cases; they fail to compile** (`oneOf`, `distinctValues`, `DistinctRequest` undeclared).

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile errors naming `oneOf` and `distinctValues`.

- [ ] **Step 5: Implement the value-set operator** in `filter.hpp`:
  - `FilterEntry::oneOf` per the Interfaces block, Doxygen as the spec's block ("Matches a cell whose value equals any of these. Each is parsed for the column's kind, as `eq`'s operand is.").
  - `readEntry` counts `oneOf` as one operator (`Op::OneOf`); `operatorDefined(kind, OneOf)` is `operatorDefined(kind, Eq)`.
  - `class ValueSet` holds the parsed members for one column kind: `std::unordered_set<std::int64_t>` (Date, DateTime, Bool), `std::unordered_set<math::Rational, detail::RationalHash>` (Integer, Key, Decimal, Quantity; hash of the canonical `numerator`/`denominator`), `std::unordered_set<double>` (Number), and a transparent-hash `std::unordered_set<std::string>` of folded texts, or raw texts when `caseSensitive` (Text). Public: `[[nodiscard]] bool contains(KeyColumn const& keys, std::uint32_t row, FilterCounters* counters) const` (one probe; increments `counters->setLookups` when non-null) and `[[nodiscard]] std::size_t size() const noexcept`. Members are parsed by the existing `parseOperand`; one that does not parse is `InvalidValue` naming the column, as `eq` is. An Integer/Key column drops a member that is not a whole number at compile time (it can never match, as `eq "1.5"` cannot).
  - `matches` for `OneOf`: `Unloaded` → handled by Task 3's `evaluate` (`Unknown`); `Empty`/`Invalid` → `false`; `Valid` → `entry.set->contains(…)`.
  - The existing `evaluate(Column const&, KeyColumn const&, std::uint32_t)` gains a trailing `FilterCounters* counters = nullptr`, passed down to `matches`.

- [ ] **Step 6: Implement the limits** in `query.hpp`: `QueryLimits::maxSetValues = 10'000`, `maxQuerySetValues = 20'000`, `maxQuerySetBytes = std::size_t{1} << 20U`. `detail::entryTooLong` checks every `oneOf` member against `maxTextLength`. `detail::checkFilters` sums, over every `oneOf` of `include` and `exclude` of every column and group, the member count and the members' byte sizes; one entry over `maxSetValues` is `LimitExceeded` naming its column (a group's: empty), the sums over `maxQuerySetValues` or `maxQuerySetBytes` are `LimitExceeded` with an empty column.

- [ ] **Step 7: Implement distinct values** in `query.hpp` (`DateBucket` and its `glz::meta` "value", "year", "month", "day"; the three structs with `operator== = default`; `TableQuery::distinct`, `PageRows::distinct`, `Page<Row>::distinct`). `distinctValues` follows the spec's bullets; the choices they leave open:
  - **Validation** (also run by `validate` on `query.distinct`): exactly one of `column`, `group` is non-empty, else `InvalidSpec`; `group` non-empty is `Unavailable` ("pooled values need column groups") until Task 8b; the column passes `detail::checkColumn`; `search.size() > maxTextLength`, `offset < 0`, `offset > maxOffset`, `limit < 0` are `LimitExceeded`; `limit` is clamped to `maxLimit`; a `bucket` other than `Value` on a kind other than `Date`/`DateTime` is `InvalidSpec`; a `Custom` column is `UnsupportedOperator` (it has no values to tick); `query.keys` and `query.distinct` both set is `InvalidSpec`.
  - **Rows:** compile `query.filters` with `filters.columns[request.column]` erased; run it as `apply` does (a `FilterRun` over every source row). `Yes` rows are candidates; any `Unknown` row sets `partial`.
  - **Grouping key:** the column's `KeyColumn` (built for the column alone): Text by `folded`, the exact/int/real key otherwise; a bucket groups `Date` days or `DateTime` seconds (UTC) by the bucket's start. `Empty` → `blanks`, `Invalid` → `invalid`, `Unloaded` → `partial`, not counted.
  - **A value's `text` and `operand`:** from its first candidate row in source order. Text: the raw text (both). Integer/Key/Decimal/Quantity: `units::detail::formatRationalDecimal` of the key (both). Number: `std::format("{}", key)` (both). Bool: `"true"`/`"false"`. Date `Value`: the cell's own string when the source gave text, else ISO `YYYY-MM-DD` (both). DateTime `Value`: the cell's own string, else `YYYY-MM-DDTHH:MM:SSZ`. Buckets: `text` is `YYYY`, `YYYY-MM` or `YYYY-MM-DD`; `operand` is the bucket's first and last instant joined by `..` (`2026-01-01..2026-01-31`; DateTime ends `T00:00:00Z` and `T23:59:59Z`), which the menu splits into a `between`.
  - **Search:** `collator.fold(search)` is a substring of the value's folded `text`, folded once per distinct value, never per row.
  - **Order and paging:** values sort by `compareKeys` (ascending) on their first rows, ties by first row; `distinct` is the count after search, reported as `min(count, totalCap)` with `AtLeast` when `count > totalCap`; then `offset`/`limit` slice `values`.
  - `levelsNeeded` (Task 3) adds, when `query.distinct` names a `column`, that column's level to `order`: the values list reads it for every candidate row.
  - `apply` with `query.distinct` set returns `PageRows{.rows = {}, .distinct = distinctValues(…)}` (same options). The typed `apply` copies `distinct` into `Page<Row>`. `detail::answerQuery` (Task 2) takes the same branch before `computeView`, so a `Table.<id>` request with `distinct` answers a `GenericPage` with no rows and `distinct` set. `Page<Row>::openMembers()` already names `"distinct"` (Task 7).
  - `GenericPage::distinct` is written and read as the JSON member `"distinct"` with glaze's default shape of `DistinctValues`.

- [ ] **Step 8: Implement `Engine::distinctValues(request) const`** in `engine.hpp`: validation as above with default `QueryLimits`; the engine's current `filter()` spec with the column's entry erased; the engine's current snapshot; key columns the engine holds for that snapshot reused, a missing one built on the calling thread and not stored.

- [ ] **Step 9: Run the new cases and the target**

Run: `ctest --test-dir build/cl-debug -R "oneOf|distinct|value sets|wire bound" --output-on-failure`, then `ctest --test-dir build/cl-debug -R "table" --output-on-failure`
Expected: all pass.

- [ ] **Step 10: Mutations.** (a) `ValueSet::contains` returning `false`: `"table: oneOf matches what its eq entries match"` fails. (b) `distinctValues` keeping the column's own filter: `"distinct: counts are those of the rows the other filters leave"` fails (`closed` missing). (c) grouping text by `raw` instead of `folded`: the same case fails (`open` count 2, an `Open` value appears). Paste each failing line into the commit body; revert.

- [ ] **Step 11: Timings D1–D4.** Add `TEST_CASE("table benchmark: distinct values and value sets", "[.benchmark][table][distinct]")` to `bench_table.cpp`, generated with `std::mt19937{7}`: 100,000 rows, a text column of 50,000 distinct values; D1 first 200 with keys built; D2 with search `"abc"`; D3 with filters on two other columns, one of them `Unloaded` in 30% of rows — the case asserts `partial` before printing; D4 compile a 10,000-value `oneOf` and filter the 100,000 rows. Run on `cl-release` (command in Global Constraints) and write the medians and spreads into table.md "Benchmarks: configuring" rows D1–D4 (replacing "(P)" budgets by the recorded baseline plus its spread, as the Performance section says).

- [ ] **Step 12: Docs build** (new public symbols) — command in Global Constraints; expected: exits 0.

- [ ] **Step 13: Commit** `feat(table): value sets and distinct values` (body: the three mutations' failing lines).

---

### Task 8b: Column groups with quantifiers

**Spec:** "Column groups" (declaring, the filter, the truth tables, deciding `Together`, unloaded cells, the menu's pooled values list); "Unloaded cells in filters" (group rule); invariant 24 and invariant 1 for groups; Tests row "Column groups"; Tests row "Distinct values" (a group's "(Blanks)"); gates G5, G7; timings G1–G4, G6, G7; Neighbouring specs row `../forms/views.md` (`x-group`, step 3).

**Files:**
- Modify: `include/morph/table/data_source.hpp` (`ColumnGroupInfo` moved here, `DataSource::groups()`, `VectorSource::setGroups`, `detail::checkGroups`)
- Modify: `include/morph/table/filter.hpp` (`GroupMode`, `ColumnCount`, `GroupFilter::{group, mode, match, exclude}`, `CompiledFilter::Group`, `compileGroup`, `compileFilter`'s groups parameter, `CompiledFilter::evaluateGroup` with quantifiers and counters, glaze metas)
- Modify: `include/morph/table/query.hpp` (`checkFilters` for groups, `apply` passing `source.groups()`, pooled `distinctValues`)
- Modify: `include/morph/table/engine.hpp` (`compile` passing `_source->groups()`; `fitToColumns` keeping a declared group's filter)
- Modify: `include/morph/table/description.hpp` (remove `ColumnGroupInfo`'s definition; it now comes from `data_source.hpp`)
- Modify: `include/morph/forms/forms.hpp` (`mergeSchemaExtras` writes `x-group`), `include/morph/forms/views.hpp` (`deriveColumns` copies `x-group`)
- Modify: `tests/table/wide_table_model.{hpp,cpp}` (declared groups `g3`, `g16`)
- Test: `tests/table/test_table_filter.cpp`, `tests/table/test_table_distinct.cpp`, `tests/table/test_table_columns.cpp`, `tests/table/bench_table.cpp`
- Docs: `docs/spec/forms/views.md` (`x-group` on a schema property, copied into the `v-columns` entry)

**Interfaces:**
- Consumes: 8a's `ValueSet`, `FilterCounters`, `distinctValues`; Task 3's `Match`, `evaluate(Column const&, …)`.
- Produces:
  ```cpp
  // data_source.hpp (ColumnGroupInfo as Task 1 defined it: id, label, columns)
  [[nodiscard]] virtual std::span<ColumnGroupInfo const> DataSource::groups() const { return {}; }
  void VectorSource::setGroups(std::vector<ColumnGroupInfo> groups);
  // filter.hpp
  enum class GroupMode : std::uint8_t { Columns, Together };                 // JSON "columns", "together"
  struct ColumnCount { enum class Kind : std::uint8_t { Any, All, AtLeast, Exactly } kind = Kind::Any; std::int64_t n = 1; };
  struct GroupFilter { std::string group; std::vector<std::string> columns; GroupMode mode = GroupMode::Columns;
                       ColumnCount match; Combine combine = Combine::Any;
                       std::vector<FilterEntry> include; std::vector<FilterEntry> exclude; };
  [[nodiscard]] std::expected<CompiledFilter, TableError>
  compileFilter(FilterSpec const& spec, std::span<ColumnInfo const> columns, Services const& services,
                std::span<ColumnGroupInfo const> groups = {});                // and compileFilterGuarded likewise
  static Match CompiledFilter::evaluateGroup(Group const& group, std::span<KeyColumn const* const> keys,
                                             std::uint32_t row, FilterCounters* counters = nullptr);   // Task 3's, extended
  ```
  `ColumnCount` JSON is `"any"`, `"all"`, `{"atLeast": n}` or `{"exactly": n}` (custom glaze read/write).

- [ ] **Step 1: Write the failing truth-table test** in `test_table_filter.cpp`. Helper `Match groupMatch(VectorSource const&, GroupFilter, std::uint32_t row)` compiles `FilterSpec{.groups = {g}}` with `source.groups()`, builds the key columns as `Filtered::by` does, and returns `CompiledFilter::evaluateGroup(compiled->groups()[0], pointers, row)`. Header mutation line: "`Together` evaluated as `atLeast` with n = entries: the truth-table case fails on row F, F, X".

```cpp
// companyA, companyB, companyC (Text), declared group {"company", "Company", {companyA, companyB, companyC}}
// 0: F, S, X   1: F, F, X   2: S, F, F   3: F, X, Y   4: X, Y, Z
TEST_CASE("groups: the spec's truth table", "[table][filter][groups]") {
    auto const by = [&](GroupFilter g) { return f.by(FilterSpec{.columns = {}, .groups = {std::move(g)}}); };
    CHECK(by({.group = "company", .include = {eq("F")}}) == Rows{0, 1, 2, 3});
    CHECK(by({.group = "company", .mode = GroupMode::Together, .include = {eq("F"), eq("S")}}) == Rows{0, 2});
    CHECK(by({.group = "company", .include = {oneOf({"F", "S", "T"})}}) == Rows{0, 1, 2, 3});
    CHECK(by({.group = "company", .match = {.kind = ColumnCount::Kind::AtLeast, .n = 2}, .include = {oneOf({"F", "S"})}}) == Rows{0, 1, 2});
    CHECK(by({.group = "company", .match = {.kind = ColumnCount::Kind::Exactly, .n = 1}, .include = {eq("F")}}) == Rows{0, 3});
    CHECK(by({.group = "company", .match = {.kind = ColumnCount::Kind::All, .n = 1}, .include = {oneOf({"F", "S"})}}) == Rows{2});
    CHECK(by({.columns = {"companyA", "companyB", "companyC"}, .include = {eq("F")}}) ==
          by({.group = "company", .include = {eq("F")}}));                   // the plain group is `any`
    CHECK(by({.group = "company", .include = {}, .exclude = {eq("X")}}) == Rows{2});   // "none of the companies is X"
    std::vector<FilterEntry> seventeen;                                       // only the 17th entry can match
    for (int i = 1; i <= 16; ++i) seventeen.push_back(eq("A" + std::to_string(i)));
    seventeen.push_back(eq("F"));
    CHECK(by({.group = "company", .include = seventeen}) == Rows{0, 1, 2, 3});   // Mutation: a 16-bit entry mask
    // dates: "two of the three are after 2026-05-01" over three Date columns is atLeast 2 with a gt entry
}

TEST_CASE("groups: three-valued over unloaded cells", "[table][filter][groups]") {
    // Rows of three Text cells, U = Cell{Unloaded{}}; each CHECK is groupMatch(source, filter, row) == expected.
    struct Case { GroupFilter filter; std::array<char const*, 3> cells; Match expected; };
    // AtLeast 2, eq F:   {F,F,U} Yes   {X,X,U} No    {F,X,U} Unknown
    // Any, eq F:         {F,U,U} Yes   {X,X,U} Unknown   {X,X,X} No
    // All, eq F:         {F,F,U} Unknown   {F,X,U} No
    // Exactly 1, eq F:   {F,F,U} No    {F,X,U} Unknown   {X,X,U} Unknown   {F,X,X} Yes   {X,X,X} No
    // Together F,S:      {F,S,U} Yes   {F,F,U} Unknown   {U,U,X} Unknown   {X,X,U} No
    // include {}, exclude eq X:  {F,X,U} No   {F,S,U} Unknown   {F,S,F} Yes
    // isEmpty, Any:      {X,X,U} Unknown   (an unloaded cell is not empty)
}

TEST_CASE("groups: duplicate columns, exactly zero and too many together entries", "[table][filter][groups]") {  // Review Focus 3
    CHECK(f.by(FilterSpec{.columns = {}, .groups = {{.columns = {"companyA", "companyA"}, .include = {eq("F")}}}})
              .error().code == TableErrorCode::InvalidSpec);
    CHECK(by({.group = "company", .match = {.kind = ColumnCount::Kind::Exactly, .n = 0}, .include = {eq("F")}}) == Rows{4});
    CHECK(f.by(FilterSpec{.columns = {}, .groups = {{.group = "company", .mode = GroupMode::Together,
                                                     .include = {eq("A"), eq("B"), eq("C"), eq("D")}}}})
              .error().code == TableErrorCode::InvalidSpec);
    CHECK(f.by(FilterSpec{.columns = {}, .groups = {{.group = "company", .mode = GroupMode::Together, .include = {eq("A")}}}})
              .error().code == TableErrorCode::InvalidSpec);                   // fewer than 2 entries
    CHECK(f.by(FilterSpec{.columns = {}, .groups = {{.group = "company", .match = {.kind = ColumnCount::Kind::AtLeast, .n = 4},
                                                     .include = {eq("F")}}}}).error().code == TableErrorCode::InvalidSpec);
    CHECK(f.by(FilterSpec{.columns = {}, .groups = {{.group = "company", .columns = {"companyA", "companyB"},
                                                     .include = {eq("F")}}}}).error().code == TableErrorCode::InvalidSpec);
    CHECK(f.by(FilterSpec{.columns = {}, .groups = {{.group = "nope", .include = {eq("F")}}}}).error().code ==
          TableErrorCode::UnknownColumn);
}

TEST_CASE("groups: a declared group is checked where the columns are read", "[table][filter][groups]") {
    // setGroups with: one column; 17 columns; mixed kinds (Text + Integer); a column in two groups.
    // apply(*source, {}) on each is TableErrorCode::SourceContract; a well-formed declaration applies.
}

TEST_CASE("groups: the JSON shape reads and writes", "[table][filter][groups]") {
    auto const json = R"({"group":"company","mode":"together","match":{"atLeast":2},"combine":"all","include":[{"eq":"F"},{"eq":"S"}],"exclude":[{"eq":"X"}]})";
    GroupFilter g;
    REQUIRE_FALSE(glz::read_json(g, std::string{json}));
    CHECK(g.mode == GroupMode::Together);  CHECK(g.match.kind == ColumnCount::Kind::AtLeast);  CHECK(g.match.n == 2);
    for (auto const* m : {R"("any")", R"("all")", R"({"atLeast":3})", R"({"exactly":0})"}) { /* read then write gives m back */ }
}

TEST_CASE("groups: invariant 24 over random tables", "[table][filter][groups]") {
    // std::mt19937 seed 24; 300 tables of 40 rows, k = 2..5 Text columns over {A..E, empty};
    // random mode, match (n in 0..k), combine, 1..k include entries (eq, oneOf, ne, startsWith).
    // Oracle: Columns mode counts columns whose cell satisfies the entries one by one (CompiledFilter::matches);
    // Together tries every injective assignment of entries to columns.
    CHECK(f.by(spec) == oracleRows);
}

TEST_CASE("groups: invariant 1 over random completions", "[table][filter][groups]") {      // gate G7's oracle
    // std::mt19937 seed 1; 200 tables of 30 rows, k = 3, about a third of the cells Unloaded;
    // for each row, every completion of its unloaded cells over {A..E, empty}:
    // evaluate == Yes  =>  the oracle passes every completion; == No  =>  it passes none;
    // == Unknown  =>  it passes some and fails some.
}

TEST_CASE("groups: a row costs k lookups and at most one augmentation an entry", "[table][filter][groups]") {  // gate G5
    // 1,000 rows, a group of 16 Text columns, every cell valid: "v<0..31>" from std::mt19937{5}; no exclude entries.
    // FilterCounters over every row.
    CHECK(absentSetCounters.setLookups == 16 * 1'000);       // `any`, oneOf {"zz1", "zz2"}: no row holds it, no early stop
    CHECK(heldSetCounters.setLookups >= 1'000);              // `any`, oneOf {"v0", "v1"}: stops at the first match
    CHECK(heldSetCounters.setLookups < 16 * 1'000);
    CHECK(togetherCounters.setLookups == 16 * 1'000);        // Together, 16 eq entries "v0".."v15": reads every column
    // per row, a fresh FilterCounters: a row whose cells leave some entry unmatched has 0 augmentations,
    // every other row at most 16.
    CHECK(maxAugmentationsOnUncoveredRows == 0);
    CHECK(maxAugmentationsOnCoveredRows <= 16);
}
```

- [ ] **Step 2: Write the failing pooled-values test** in `test_table_distinct.cpp`:

```cpp
TEST_CASE("distinct: a group's values are pooled", "[table][distinct][groups]") {
    // the truth-table source plus row 5: empty, F, U(nloaded); and a column filter companyA notEmpty is NOT set
    auto const d = distinctValues(*source, {}, DistinctRequest{.group = "company"});
    // F counted once per row that has it in any column
    CHECK(countOf(d, "F") == 5);   // rows 0, 1, 2, 3, 5
    CHECK(countOf(d, "S") == 2);   // rows 0, 2
    CHECK(d->blanks == 1);         // row 5: the rows an isEmpty entry under any admits
    CHECK(d->partial);             // row 5's unloaded cell
    TableQuery own;                // the group's own filter is removed, others stay
    own.filters.groups.push_back({.group = "company", .include = {eq("Z")}});
    CHECK(countOf(distinctValues(*source, own, {.group = "company"}), "F") == 5);
    CHECK(distinctValues(*source, {}, {.group = "nope"}).error().code == TableErrorCode::UnknownColumn);
}
```

- [ ] **Step 3: Write the failing `x-group` test** in `test_table_columns.cpp`: a row type `GroupRow{std::string companyA, companyB, companyC, name;}` with `static constexpr auto columnGroups() noexcept` returning one `{"company", {"companyA","companyB","companyC"}}` (`table::ColumnGroup{std::string_view id; std::span<std::string_view const> columns;}`, declared in `data_source.hpp`); `"derived columns carry x-group"`: the schema property of `companyB` holds `"x-group": "company"`, `deriveColumns` copies it into `companyB`'s entry, `name` has none. Mutation: the copy list in `deriveColumns` without `"x-group"`.

- [ ] **Step 4: Run; the new cases fail to compile** (`GroupMode`, `ColumnCount`, `setGroups` undeclared).

- [ ] **Step 5: Implement declarations and compile.**
  - Move `ColumnGroupInfo` (unchanged members, `operator== = default` and Doxygen) from `description.hpp` to `data_source.hpp`; add `struct ColumnGroup` (the typed row's constexpr declaration), `DataSource::groups()`, `VectorSource::setGroups`, and `[[nodiscard]] std::optional<TableError> detail::checkGroups(std::span<ColumnInfo const>, std::span<ColumnGroupInfo const>)`: each group has 2 to 16 columns, all present and of one kind, and no column is in two groups; otherwise `SourceContract` naming the group's id. `apply`, `distinctValues` and `Engine::setFilter` run it before compiling.
  - `GroupFilter` gains members in the order of the Interfaces block (existing designated initializers `.columns, .combine, .include` stay valid). `glz::meta` for `GroupMode` and the custom read/write for `ColumnCount`.
  - `compileGroup(GroupFilter const&, columns, services, groups)`: exactly one of `group` and `columns` is non-empty, else `InvalidSpec`; `group` resolves through `groups` (`UnknownColumn` naming it when absent); a column named twice is `InvalidSpec`; `Together` takes 2 to k include entries, else `InvalidSpec`; `AtLeast`/`Exactly` take `0 <= n <= k`, else `InvalidSpec`. `CompiledFilter::Group` keeps `k` (the named column count), `mode`, `match`, and per column the include and exclude entries that compiled for it; a column on which `combine: all` cannot compile every include entry, or `any` none, never satisfies (it counts as a known *no*). An include or exclude entry that compiles on none of the columns is the error the existing rule returns. A group with no include and no exclude entry compiles to nothing.
  - Per column, the compile builds a **value index**: one hash map from a cell key to the bit mask (`std::uint32_t`: `Columns` mode takes up to `maxEntries` = 32 entries; only `Together` is capped at 16) of the include entries that are `eq` or `oneOf` on it (folded text unless `caseSensitive`), and the list of the other include entries with their bit. `exclude` is compiled the same way into its own index.
  - `checkFilters`: a group's `include + exclude` count against `maxEntries`, every exclude entry against `maxTextLength`; `group` and `columns` resolve and pass `checkColumn` (a declared group through `source.groups()`, which `validate` receives as a new trailing `std::span<ColumnGroupInfo const> groups = {}` parameter).

- [ ] **Step 6: Extend `CompiledFilter::evaluateGroup(Group const&, keys, row, counters)`** with this algorithm (the spec fixes it; G5 counts it):

```text
mask(c):  cell = keys[c] at row;  counters->cellReads += 1
          Unloaded            -> unknown column
          Empty / Invalid     -> entries matching by the column-filter rules (isEmpty / notEmpty only)
          Valid               -> index lookup (counters->setLookups += 1) | each condition entry tested

empty include: the include part is Yes, and neither `mode` nor `match` is consulted (an exclude-only group
   compiles in either mode); only the exclude part below decides.

Columns mode (each column known-satisfies when  combine any: mask != 0;  all: mask == every compiled include bit
and the column compiled every include entry): read columns in order, keeping y (satisfying), q (unknown),
r (columns not yet read); stop as soon as decided:
   any      Yes y>=1          No y+q+r == 0
   all      Yes y == k        No y+q+r < k
   atLeast  Yes y>=n          No y+q+r < n
   exactly  Yes y==n, q==0, r==0     No y>n  or  y+q+r < n
   otherwise, after the last column: Unknown

Together mode (E = include entries, full = (1<<E)-1):
   read every column; covered = OR of known masks; unknownCols = set of unknown columns
   if covered != full and unknownCols empty            -> No   (no augmentation)
   known pass: Kuhn matching over known masks; for e in 0..E-1:
        counters->augmentations += 1; if !augment(e) { fail; break }
   all augmented                                        -> Yes
   unknownCols empty                                    -> No
   wildcard pass: the same matching with mask(c) = full for every unknown column
        (not counted)                                   -> Unknown if it covers every entry, else No
   augment(e): for each column c with bit e in mask(c), not visited: visit c;
               if owner[c] is none or augment(owner[c]): owner[c] = e; return true
               return false

exclude (both modes):  any known cell matching any exclude entry -> No;  otherwise any unknown column -> Unknown;
   otherwise Yes.  A group with exclude entries reads every column: once the include part is decided its reading
   stops, and the columns it did not read are still read for the exclude index (one exclude-index lookup per valid
   cell, counted in setLookups like the include index's).  Empty and invalid cells do no index lookup.
group result: No if include-part or exclude-part is No; else Unknown if either is Unknown; else Yes.
```

  `FilterRun`'s group stage and `evaluateRow` already call `evaluateGroup` (Task 3); they pass no counters.

- [ ] **Step 7: Engine and pooled values.** `Engine::compile` passes `_source->groups()`; `fitToColumns` keeps a `GroupFilter` whose `group` the source still declares and drops one whose `group` it no longer declares, then erases groups whose `columns` and `group` are both empty. `distinctValues` with `request.group`: the rows passing every filter with every `GroupFilter` whose `group == request.group` removed; per candidate row, the set of distinct keys over the group's loaded valid cells (a row counts once per value); `blanks` counts rows with at least one empty cell, `invalid` rows with at least one invalid cell; an unloaded cell sets `partial` and the row's loaded cells still count. A value's text and operand come from the first row and column (in group order) that holds it. Remove 8a's `Unavailable` branch. `levelsNeeded` adds the level of every column of `request.group` to `order`; test `"distinct: a group's levels are order levels of the request"` in `test_table_distinct.cpp` (group `company` with `companyB` at level `"slow"`: `levelsNeeded(q with distinct{.group = "company"}, …).order == {"slow"}`; mutation: the group's columns left out).

- [ ] **Step 8: `x-group`.** `mergeSchemaExtras` writes `"x-group": "<id>"` on each property a row's `columnGroups()` names; `deriveColumns` adds `"x-group"` to the keys it copies. `docs/spec/forms/views.md`: one line under the copied keys, and the `v-columns` row's key list gains `"x-group"?`.

- [ ] **Step 9: Fixture.** `bench::WideTableSource::groups()` returns, from `layout()`'s `group` attributes, `{"g3", "Group of 3", {t004, t005, t006}}` and `{"g16", "Group of 16", {t011 ... t026}}` (`wideColumns(30)` keeps only `t004`...`t006`, so the 30-column source declares `g3` alone).

- [ ] **Step 10: Run** `ctest --test-dir build/cl-debug -R "groups|distinct|x-group|filter" --output-on-failure`; expected: all pass, and the existing group cases (`"columns combine by AND, and a group matches on any of its columns"`, `"an all-group uses only the columns that evaluate every entry"`) unchanged and passing.

- [ ] **Step 11: Mutations.** (a) `Together` decided as `atLeast` with n = entry count: the truth table fails on row `F, F, X`. (b) `exactly` deciding `Yes` while `q > 0`: the three-valued case `{F,X,U}` fails. (c) the known pass reading unknown columns as matching every entry: the three-valued case `{F,F,U}` reports `Yes`. (d) skipping the covered-mask check: G5 fails (augmentations on rows that cannot match). Paste each; revert.

- [ ] **Step 12: Timings G1–G4, G6, G7.** `TEST_CASE("table benchmark: column groups", "[.benchmark][table][groups]")` on 100,000 rows generated with `std::mt19937{7}` (a group of 3 and one of 16 Text columns, keys built): G1 `any` with a 2-value `oneOf`; G2 with a 1,000-value `oneOf`; G3 `atLeast 2` and `exactly 1`; G4 `together` 2 entries over 3 columns and 16 over 16; G6 pooled distinct values, first 200; G7 G1 and G4 with a third of the cells `Unloaded` — the case first asserts the undecided count equals the oracle's from Step 1's invariant-1 test on a 1,000-row slice. Write the figures into table.md "Benchmarks: column groups".

- [ ] **Step 13: Docs build; commit** `feat(table): column groups with quantifiers`.

---

### Task 8c: Values with units

**Spec:** "Values with units" (all subsections); "Rows on the wire" (the `Measure` row of the cell table); invariant 25; Tests rows "Values with units", "Unit numbers across registries", "The generic page" (measure half); Failure modes row "A level cache read under a registry of another lineage"; gate M5; timings M1–M4.

**Files:**
- Modify: `include/morph/table/data_source.hpp` (`UnitInfo`, `UnitId`, `kUnknownUnit`, `UnitRegistry`, `UnitRegistryBuilder`, `addUnitsOf<E>`, `Measure`, `Cell`'s last alternative, `ColumnKind::Measure`, `ColumnInfo::dimensions`, `Services::units`, `displayText`, `unitsFor`)
- Modify: `include/morph/table/sort.hpp` (`KeyColumn` for `Measure`, `compareKeys`)
- Modify: `include/morph/table/filter.hpp` (operands with units, `ValueSet` of measures)
- Modify: `include/morph/table/query.hpp` (`MeasureValue`, `kindOf`, `toCell`, `RowsSource`'s registry, typed `apply`, `distinctValues` on measures, `DistinctValue::{dimension, unit}`, the `SourceContract` check)
- Modify: `include/morph/table/description.hpp` (`TableDescription::units` after `levels`; the generic page's `Measure` cell; `registryOf`), `include/morph/core/registry.hpp` (`DescribeTable<F>::invoke` fills `units`)
- Modify: `include/morph/table/level_cache.hpp` (`LevelCacheOptions::units`, the lineage check)
- Modify: `include/morph/forms/forms.hpp` (`x-measure` on a `MeasureValue` property), `include/morph/forms/views.hpp` (`inferColumnKind` → `"measure"`)
- Create: `include/morph/ui/measure.hpp` (`ui::TypedMeasure`, `ui::parseMeasure`, `ui::measureOperand`); `tests/table/test_table_measure.cpp` (in `morph_table_tests`; add to `tests/CMakeLists.txt`)
- Modify: `tests/table/test_table_description.cpp`, `tests/table/bench_table.cpp`, `tests/table/wide_table_model.{hpp,cpp}` (its 10 measure columns become `ColumnKind::Measure` over the fixture registry below)

**Interfaces:**
- Consumes: 8a's `ValueSet`, `distinctValues`; Task 3's `Unloaded`, `SourceContract`; Task 1's generic page; Task 5's `CellLevelCache`, `LevelCacheOptions`, `LevelCacheStats`.
- Produces:
  ```cpp
  struct UnitInfo { std::string id; std::string symbol; std::string dimension; math::Rational toBase; std::uint32_t decimals = 0; };
  using UnitId = std::uint32_t;
  inline constexpr UnitId kUnknownUnit = std::numeric_limits<UnitId>::max();   // never a live unit
  class UnitRegistry {
  public:
      [[nodiscard]] std::optional<UnitId> find(std::string_view id) const;    // counts findCalls()
      [[nodiscard]] UnitInfo const& info(UnitId unit) const;                  // precondition: contains(unit)
      [[nodiscard]] bool contains(UnitId unit) const noexcept;                // false for a tombstone or kUnknownUnit
      [[nodiscard]] std::span<std::string const> dimensions() const;
      [[nodiscard]] std::span<UnitId const> unitsOf(std::string_view dimension) const;
      [[nodiscard]] std::uint64_t lineage() const noexcept;
      [[nodiscard]] std::uint64_t findCalls() const noexcept;                 // relaxed counter, for gate M5
  };
  class UnitRegistryBuilder {
  public:
      UnitRegistryBuilder();                                                  // seeds {"", "", "ratio", 1, 6}
      explicit UnitRegistryBuilder(std::shared_ptr<UnitRegistry const> previous);
      std::expected<UnitId, TableError> add(UnitInfo unit);
      bool remove(std::string_view id);
      [[nodiscard]] std::shared_ptr<UnitRegistry const> build() &&;
  };
  template <units::UnitEnum E>
  std::expected<void, TableError> addUnitsOf(UnitRegistryBuilder& builder, std::span<E const> units,
                                             std::string_view (*dimensionOf)(E));
  struct Measure { math::Rational amount; UnitId unit = 0; };
  using Cell = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string, bool, QuantityCell,
                            Unloaded, Measure>;
  // ColumnKind::Measure appended after Custom, JSON "measure"; ColumnInfo::dimensions (std::vector<std::string>, last member)
  // Services::units: std::shared_ptr<UnitRegistry const>
  [[nodiscard]] std::vector<UnitInfo> unitsFor(UnitRegistry const& registry, std::span<ColumnInfo const> columns);
  // query.hpp
  struct MeasureValue { math::Rational amount; std::string unit; };          // JSON {"num","den","unit"}; schema x-measure
  struct DistinctValue { /* 8a members */ std::string dimension; std::string unit; };
  // description.hpp
  [[nodiscard]] std::expected<std::shared_ptr<UnitRegistry const>, TableError> registryOf(TableDescription const& description);
  // writeGenericPage / readGenericPage gain a trailing `UnitRegistry const* units = nullptr`
  // level_cache.hpp
  // LevelCacheOptions::units: std::function<std::shared_ptr<UnitRegistry const>()>   (empty: no units)
  // ui/measure.hpp
  namespace morph::ui {
  struct TypedMeasure { math::Rational amount; std::string unit; };          // unit: the unit's id
  [[nodiscard]] std::optional<TypedMeasure> parseMeasure(std::string_view text, std::span<table::UnitInfo const> units,
                                                         LocaleFacts const& locale, std::string_view chosen = {});
  [[nodiscard]] std::string measureOperand(TypedMeasure const& measure);   // "0.3 kg"; "0.003" for the ratio unit
  }
  ```

The test registry used below (built once in the file): `""` (seeded), `kg` (`"kg"`, mass, 1, 3), `g` (`"g"`, mass, 1/1000, 0), `percent` (`"%"`, ratio, 1/100, 2). Helper `m(std::string_view amount, std::string_view unit) -> Cell` is `Measure{dec(amount), *registry->find(unit)}`. Columns: `col("w", ColumnKind::Measure)` with `dimensions = {"mass", "ratio"}` unless a case says otherwise; `ApplyOptions::services.units = registry`.

- [ ] **Step 1: Write the failing order and filter tests** in `test_table_measure.cpp` (header mutation line: "a key that ignores the unit's factor: `300 g sorts between 0.2 kg and 0.5 kg` fails"):

```cpp
// rows: 0: 0.5 kg   1: 0.3 %   2: 300 g   3: 0.2 kg   4: empty   5: 0.2 %
TEST_CASE("measure: 300 g sorts between 0.2 kg and 0.5 kg in both directions", "[table][measure]") {
    CHECK(sorted(SortDirection::Ascending)  == std::vector<std::size_t>{3, 2, 0, 5, 1, 4});
    CHECK(sorted(SortDirection::Descending) == std::vector<std::size_t>{0, 2, 3, 1, 5, 4});   // groups stay, amounts reverse
}

TEST_CASE("measure: filters compare base amounts within a dimension", "[table][measure]") {
    CHECK(rowsOf(gt("250 g")) == Rows{0, 2});             // drops 0.2 kg and both percents
    CHECK(rowsOf(eq("300 g")) == rowsOf(eq("0.3 kg")));
    CHECK(rowsOf(eq("0.3 kg")) == Rows{2});                // 300 g; not 0.3 %: they are not equal
    CHECK(rowsOf(ne("250 g")) == Rows{0, 1, 2, 3, 5});    // other dimensions included; empty never matches
    CHECK(rowsOf(eq("0.003")) == Rows{1});                 // no unit: a ratio, equal to 0.3 %
    CHECK(rowsOf(oneOf({"300 g", "0.2 percent"})) == Rows{2, 5});
    CHECK(rowsOf(between("0.25 kg", "600 g")) == Rows{0, 2});
    CHECK(error(eq("250 lb")).code == TableErrorCode::InvalidValue);   CHECK(error(eq("250 lb")).column == "w");
    CHECK(error(between("1 kg", "50 percent")).code == TableErrorCode::InvalidValue);
    CHECK(errorOnMassOnly(eq("0.003")).code == TableErrorCode::InvalidValue);   // dimensions = {"mass"}
}

TEST_CASE("measure: invalid cells and the registry contract", "[table][measure]") {
    // Measure{1, kUnknownUnit}; a ratio cell in a mass-only column: both Invalid, sort with empty after valid
    CHECK(sortedMassOnly(SortDirection::Ascending).back() == /* the empty row */);
    ApplyOptions none;   // services.units null
    CHECK(apply(*source, sortBy("w"), none).error().code == TableErrorCode::SourceContract);
}

TEST_CASE("measure: negative and zero amounts", "[table][measure]") {   // Review Focus 4
    // mass+ratio column: -300 g, -0.2 kg, 0 g, 0 kg, -0.5 %
    CHECK(sortedAsc == std::vector<std::size_t>{0, 1, 2, 3, 4});            // -300 g < -0.2 kg < 0 g = 0 kg (by row), then ratio
    CHECK(sortedDesc == std::vector<std::size_t>{2, 3, 1, 0, 4});           // mass reversed, ties by source row, ratio last
    CHECK(distinctCount("0 g") == 2);                                       // 0 g and 0 kg one value
    CHECK(errorOnMassOnly(eq("0")).code == TableErrorCode::InvalidValue);
}

TEST_CASE("measure: 0.3 kg and 300 g are one distinct value", "[table][measure]") {
    // rows: 0.3 kg, 300 g, 0.3 %
    REQUIRE(d->values.size() == 2);
    CHECK(d->values[0].count == 2);  CHECK(d->values[0].dimension == "mass");  CHECK(d->values[0].unit == "kg");
    CHECK(d->values[0].text == "0.3 kg");  CHECK(d->values[0].operand == "0.3 kg");
    CHECK(d->values[1].dimension == "ratio");  CHECK(d->values[1].text == "0.3 %");  CHECK(d->values[1].operand == "0.3 percent");
}

TEST_CASE("measure: invariant 25 over random amounts", "[table][measure]") {
    // std::mt19937 seed 25; 500 rows of random amounts in kg, g, percent, ""; sort ascending and descending;
    // within a dimension the order equals the order by amount * toBase (exact Rational), ties by source row;
    // eq on any one row's operand matches exactly the rows of equal base amount and equal dimension.
}

TEST_CASE("measure: keys look units up by number only", "[table][measure]") {   // gate M5
    auto const before = registry->findCalls();
    // build the Measure KeyColumn over 100,000 cells (KeyColumn::forColumn + buildKeys)
    CHECK(registry->findCalls() == before);
}
```

- [ ] **Step 2: Write the failing registry tests:**

```cpp
TEST_CASE("units: a unit id is a token", "[table][measure]") {
    UnitRegistryBuilder b;
    for (std::string_view bad : {"k g", "1kg", "-kg", "+kg", ".kg", ",kg", "", "abcdefghijklmnopqrstuvwxyz0123456"})   // last: 33 bytes
        CHECK(b.add({.id = std::string{bad}, .symbol = "x", .dimension = "mass", .toBase = dec("1"), .decimals = 0}).error().code
              == TableErrorCode::InvalidSpec);
    CHECK(b.add(kg));  CHECK_FALSE(b.add(kg));                                    // a second unit of one id
    CHECK(b.add({.id = "z", .symbol = "z", .dimension = "mass", .toBase = dec("0"), .decimals = 0}).error().code == TableErrorCode::InvalidSpec);
    auto const r = std::move(b).build();
    CHECK(r->find("") == UnitId{0});   CHECK(r->info(0).dimension == "ratio");
    CHECK(std::ranges::equal(r->dimensions(), std::vector<std::string>{"ratio", "mass"}));
}

TEST_CASE("units: numbers survive a successor and tombstones stay", "[table][measure]") {
    auto const first = /* "", kg, g, percent */;
    UnitRegistryBuilder next{first};
    CHECK(next.remove("g"));
    auto const mg = next.add({.id = "mg", .symbol = "mg", .dimension = "mass", .toBase = dec("0.000001"), .decimals = 0});
    auto const second = std::move(next).build();
    CHECK(second->find("kg") == first->find("kg"));
    CHECK_FALSE(second->find("g"));
    CHECK_FALSE(second->contains(*first->find("g")));
    CHECK(*mg == 4);                                                    // after "", kg, g, percent; g's 2 never reused
    CHECK(second->lineage() == first->lineage());
    CHECK(std::move(UnitRegistryBuilder{}).build()->lineage() != first->lineage());
    CHECK(std::ranges::equal(second->unitsOf("mass"), std::vector<UnitId>{*first->find("kg"), *mg}));   // largest factor first
    // a Measure{1, g's number} under `second` is an invalid cell
}

TEST_CASE("units: a level cache keeps values under a successor and drops them under another lineage", "[table][measure]") {
    // CellLevelCache over one Measure column, LevelCacheOptions::units returning `current`;
    // load key 1 (Measure 300 g), snapshot reads it, stats().loaderCalls == 1.
    current = successor;                 // same lineage
    CHECK(read(1) == LevelRead::Loaded);   CHECK(cache.stats().loaderCalls == 1);
    current = fresh;                     // another lineage
    CHECK(read(1) == LevelRead::Unloaded); CHECK(cache.stats().invalidatedKeys >= 1);
}

TEST_CASE("units: addUnitsOf reads a unit enum", "[table][measure]") {
    // tests' own enum TmUnit { kg, g, percent } with UnitTraits (meta + relations g->kg 1/1000);
    // dimensionOf maps kg, g -> "mass", percent -> "ratio".
    // registry holds kg (toBase 1, decimals from meta), g (1/1000); percent is refused (InvalidSpec):
    // a ratio unit must reach the dimension's base "" through relations, and TmUnit has none to it.
}
```

  `addUnitsOf` rule pinned by this test: within a dimension the base is the dimension's existing base unit (`""` for ratio) or else the first enumerator of `units` in that dimension; every other unit's `toBase` is the product of `UnitRelation::fromTo` along a relation path to the base (searched in both directions, the reverse taking the reciprocal); a unit with no path is `InvalidSpec`; `symbol` is `UnitMeta::display`, `decimals` `UnitMeta::defaultDecimals`.

- [ ] **Step 3: Write the failing wire, typed-row and parser tests.** In `test_table_description.cpp`: `"generic page: a measure round-trips"` — a page of one Measure column holding `Measure{3/10, kg}` writes `[3, 10, "kg"]`, reads back equal through `registryOf(description)`; reading with a null registry is `SourceContract`; an unknown id in the JSON reads as `Measure{…, kUnknownUnit}` (an invalid cell). In `test_table_measure.cpp`:

```cpp
TEST_CASE("measure: typed rows carry their unit by id", "[table][measure]") {
    struct MRow { std::int64_t id; MeasureValue weight; };   // file-scope in the real test, for glaze
    // typed apply with services.units: column "weight" is ColumnKind::Measure; sort ascending by weight over
    // {0.5 kg, 300 g, "lb"(unknown)} gives 300 g, 0.5 kg, then the unknown unit as invalid.
    // schemaJson<MRow>() property weight has "x-measure": true; deriveColumns gives kind "measure".
}

TEST_CASE("measure: what a user types", "[table][measure]") {
    auto const en = ui::localeFacts("en");  auto const de = ui::localeFacts("de");
    CHECK(ui::parseMeasure("0.3kg", units, en) == ui::TypedMeasure{dec("0.3"), "kg"});
    CHECK(ui::parseMeasure("0.3 kg", units, en) == ui::TypedMeasure{dec("0.3"), "kg"});
    CHECK(ui::parseMeasure("300 g", units, en) == ui::TypedMeasure{dec("300"), "g"});
    CHECK(ui::parseMeasure("0.3 KG", units, en) == ui::TypedMeasure{dec("0.3"), "kg"});     // then without case
    CHECK(ui::parseMeasure("5 %", units, en) == ui::TypedMeasure{dec("5"), "percent"});    // by symbol
    CHECK(ui::parseMeasure("0,3 kg", units, de) == ui::TypedMeasure{dec("0.3"), "kg"});
    CHECK_FALSE(ui::parseMeasure("0.3 kg", units, de));                                      // '.' is de's group separator
    CHECK(ui::parseMeasure("12", units, en, "g") == ui::TypedMeasure{dec("12"), "g"});
    CHECK(ui::parseMeasure("12", units, en) == ui::TypedMeasure{dec("12"), ""});
    CHECK(ui::parseMeasure("-1.5 g", units, en) == ui::TypedMeasure{dec("-1.5"), "g"});
    CHECK_FALSE(ui::parseMeasure("0.3 lb", units, en));
    CHECK(ui::measureOperand({dec("0.3"), "kg"}) == "0.3 kg");
    CHECK(ui::measureOperand({dec("0.003"), ""}) == "0.003");
}
```

- [ ] **Step 4: Run; the new cases fail to compile** (`UnitRegistryBuilder`, `Measure`, `ColumnKind::Measure` undeclared).

- [ ] **Step 5: Registry and cell** (`data_source.hpp`): the Interfaces block; `static_assert(sizeof(Measure) <= sizeof(QuantityCell))` beside `Cell`. `UnitRegistry` stores units in a vector indexed by `UnitId` (a tombstone slot is kept), a `std::unordered_map<std::string, UnitId>` for `find`, dimensions in first-registration order (kept from `previous`), and per dimension its unit ids sorted by `toBase` descending. `lineage()` is drawn from a process-wide `std::atomic<std::uint64_t>` when a builder starts from nothing and copied from `previous` otherwise. The builder refuses (`InvalidSpec`): an id of 0 or more than 32 bytes, holding white space, or beginning with a digit, `+`, `-`, `.` or `,`; an id already live; `toBase <= 0`; an empty `dimension`. The ratio unit `""` is seeded only by the default constructor and cannot be removed. `displayText(Measure)` is the amount's decimal text (`formatRationalDecimal`); a unit's symbol is the display layer's. `unitsFor` returns the live units of every dimension a `Measure` column of `columns` allows (all dimensions for an empty `dimensions`), in registry order. `ColumnKind::Measure` is appended after `Custom` with JSON `"measure"`.

- [ ] **Step 6: Keys and order** (`sort.hpp`). `KeyColumn` gains `std::vector<std::uint32_t> places` (per row) and, built once in `forColumn` from `services.units`, `std::vector<std::int32_t> unitPlaces` and `std::vector<math::Rational> unitFactors` indexed by `UnitId` (`-1` for a tombstone or a dimension the column does not allow; the place is the dimension's index in `info.dimensions`, or in `registry.dimensions()` when that is empty). Reading a cell into a `Measure` column: `Measure` → place and `checkedMul(amount, factor)` into `exact` (overflow or `-1` → `Invalid`); `Rational`/`int64` → the ratio unit `""`; `std::string` → parsed as an operand (Step 7's grammar); anything else `Invalid`. `compareKeys` for `Measure`, after the rank rule:

```text
if places[l] != places[r]:  return places[l] < places[r] ? -1 : 1        // direction not applied
result = sign(exact[l] <=> exact[r]);  return dir == Descending ? -result : result
```

- [ ] **Step 7: Filters** (`filter.hpp`). `operatorDefined(Measure, op)` is the numeric kinds' rule. An operand on a `Measure` column is `<decimal>` or `<decimal> <unit id>` (one space; the decimal by `parseDecimal`); no id is the ratio unit `""`. The unit must be live, its dimension allowed by the column, and `checkedMul` must fit; otherwise `InvalidValue` naming the column. `CompiledFilter::Entry` gains `std::array<std::uint32_t, 2> places`; `between`'s two places must be equal (`InvalidValue`). `matches` on a valid `Measure` cell: `Ne` → `places != entry.places[0] || exact != operand`; every other comparison → `false` when the places differ, else `compare` on `exact`. `ValueSet` holds `(place, base amount)` pairs for a `Measure` column.

- [ ] **Step 8: Typed rows, distinct, wire, level cache, parser.**
  - `query.hpp`: `MeasureValue` with `glz::meta` writing `{"num","den","unit"}`; `kindOf<MeasureValue>()` is `Measure`; the cell readers become `Cell (*)(Row const&, UnitRegistry const*)` and `toCell(MeasureValue)` resolves the id (`find`), giving `kUnknownUnit` when absent; `RowsSource` takes `std::shared_ptr<UnitRegistry const> units` as its last constructor parameter and the typed `apply` passes `options.services.units`. `apply`, `distinctValues` and `Engine::setSort`/`setFilter` return `SourceContract` when a column they sort, filter or list values of is `Measure` and `services.units` is null; `writeGenericPage` does when a column it writes is `Measure` and its `units` is null. `distinctValues` on a `Measure` column groups by `(place, base amount)`; a value's `text` is `<amount> <symbol>` of its first row (no space and no symbol for `""`), `operand` `<amount> <unit id>`, `dimension` and `unit` that row's.
  - `description.hpp`: `TableDescription::units` (`std::vector<UnitInfo>`, after `levels`, so `describe`'s revision covers it); `DescribeTable<F>::invoke` (Task 2) sets `units = unitsFor(*served.services.units, columnInfos(description))` before `describe` when a column is `Measure` and the model left `units` empty, and `detail::answerQuery` passes `served.services.units.get()` to `writeGenericPage`. `registryOf(description)` builds a registry from `description.units` (`""` already seeded and skipped). The page writer writes `Measure` as `[num, den, "<id>"]` through `units`; the reader resolves the id through `units`; a `Measure` column with `units == nullptr` is `SourceContract`.
  - `level_cache.hpp`: the cache records `units()->lineage()` with each stored value set; `snapshot()` first compares the current registry's lineage with the recorded one and, when they differ, runs `invalidateAll()` (counted in `invalidatedKeys`).
  - `forms.hpp`: a `MeasureValue` property gets `"x-measure": true`. `views.hpp`: `inferColumnKind` returns `"measure"` for an `x-measure` property.
  - `ui/measure.hpp`: `parseMeasure` reads an optional `-`, digits, at most one `locale.numeric.decimalSeparator` followed by digits, optional spaces, then a unit: an exact match of a unit's `symbol` or `id` first, then the same compared ASCII-case-insensitively, first in `units` order; no unit text takes `chosen`. Anything else (a group separator included) is `std::nullopt`. `measureOperand` writes `formatRationalDecimal(amount)`, then `" " + unit` unless `unit` is empty.

- [ ] **Step 9: Fixture.** `bench::WideTableSource` owns a registry of three dimensions and ten units — mass `kg` 1, `g` 1/1000, `mg` 1/1'000'000, `t` 1000; volume `l` 1, `ml` 1/1000, `m3` 1000; ratio `""` 1, `percent` 1/100, `permille` 1/1000 — exposes it as `std::shared_ptr<UnitRegistry const> units() const`, and `m000`...`m009` become `ColumnKind::Measure` (role `Measure`, `dimensions` empty) holding `Measure` cells drawn over those units from the same `cellBits(seed, id, row)`; `"wide table: the column mix is the spec's"` keeps counting them as the 10 measures. The call sites that now need the registry (each passes `source.units()` / `.get()`):
  - `bench::WideTableModel`'s three `ServedTable`s `wide`, `small`, `scoped` (Part A, Task 2): `services.units = source->units()`, so `DescribeTable` fills `units` and `answerQuery` writes measures;
  - Part A's `"generic page"` cases that write a `WideTableSource{1'000, 30, 7}` page with an empty column list (the 30 layout holds `m000`): `writeGenericPage(…, source.units().get())`;
  - `tests/table/bench_table.cpp`: the `[.benchmark][wire]` cases (W1, W2, W7) and every `apply`/`Engine` over a `WideTableSource` whose query sorts, filters or writes an `m` column;
  - the rest found by `grep -rn "WideTableSource\|WideTableModel" tests` — each `ApplyOptions`/`EngineOptions`/`ServedTable` built over the fixture gets `services.units = source.units()`, whether or not it touches an `m` column today, so a later query cannot hit `SourceContract`.

- [ ] **Step 10: Run** `cmake --build --preset cl-debug --target morph_table_tests morph_tests`, then `ctest --test-dir build/cl-debug -R "measure|units|generic page|wide table|over the bridge" --output-on-failure`, then `-R "table|views"`; expected: all pass. `morph_net_tests` (the wide model over a socket) is POSIX-only: run `-R "push"` on `linux-everything` before merging.

- [ ] **Step 11: Mutations.** (a) `KeyColumn` storing `amount` instead of `amount * factor`: `"300 g sorts between…"` fails. (b) `compareKeys` negating the place comparison under `Descending`: the same case's descending line fails. (c) `ne` returning `false` across dimensions: `"filters compare base amounts…"` fails on `ne("250 g")`. (d) `forColumn` dropped and `find(info(unit).id)` per cell: M5 fails. (e) the level cache skipping the lineage check: the level-cache case fails. Paste each; revert.

- [ ] **Step 12: Timings M1–M4.** `TEST_CASE("table benchmark: values with units", "[.benchmark][table][measure]")`: 100,000 rows from `std::mt19937{7}`, one `Measure` column over the fixture registry (three dimensions, ten units) and a `Quantity` column of the same amounts converted by factor; M1 keys and sort of each; M2 `gt "250 g"` against the quantity column's `gt "0.25"`; M3 compile 1,000 operands with units, and `ui::parseMeasure` of 1,000 typed texts; M4 `distinctValues` on the measure column against a decimal column. Write the figures into table.md "Benchmarks: values with units".

- [ ] **Step 13: Docs build; commit** `feat(table): values with units`.

---

### Task 8d: Column layout — widths, pinning and the fitting rule

**Spec:** "Columns: order, visibility, pinning, width" (the `ColumnLayout` paragraph and "A layout survives a changed table"); "Configuring a table" (the `layout` value's shape); "Column groups" (a user-formed group in the layout, fitted with it); "Display" (`"units"` in the layout); invariant 26 (engine half); Tests row "Column layout" (engine half; gate D8 is Task 10's).

**Files:**
- Modify: `include/morph/table/engine.hpp` (`ColumnLayoutValue`, `ColumnLayout`)
- Modify: `tests/table/test_table_columns.cpp` (new cases), `tests/table/test_table_engine.cpp` (the existing `rebuild` expectation)

**Interfaces:**
- Consumes: `ColumnGroupInfo` (Task 1's, in `description.hpp`; 8b moves it unchanged to `data_source.hpp`, which `description.hpp` includes, so `engine.hpp` includes `description.hpp` if 8d lands first and `data_source.hpp` after 8b).
- Produces:
  ```cpp
  /// The stored shape of a layout: {"order", "hidden", "pinned", "widths", "groups", "units"}.
  struct ColumnLayoutValue {
      std::vector<std::string> order;
      std::vector<std::string> hidden;                                      // in display order
      std::size_t pinned = 0;
      std::map<std::string, int> widths;                                    // column id -> width in units, > 0
      std::vector<ColumnGroupInfo> groups;                                  // user-formed groups
      std::map<std::string, std::map<std::string, std::string>> units;      // column -> dimension -> unit id
      [[nodiscard]] bool operator==(ColumnLayoutValue const&) const = default;
  };
  class ColumnLayout {
  public:
      explicit ColumnLayout(std::vector<std::string> ids, std::vector<std::string> declaredHidden = {});
      // existing: order(), visible(), setVisible(), move(), reset()
      bool setWidth(std::string const& columnId, std::optional<int> width);   // false: unknown id or width <= 0
      [[nodiscard]] std::map<std::string, int> const& widths() const noexcept;
      void setPinned(std::size_t n);                                          // clamped to visible().size()
      [[nodiscard]] std::size_t pinned() const noexcept;                      // the first n of visible()
      [[nodiscard]] ColumnLayoutValue value() const;
      void apply(ColumnLayoutValue const& value);                             // the fitting rule
      void rebuild(std::vector<std::string> ids, std::vector<std::string> declaredHidden = {});
  };
  ```

- [ ] **Step 1: Write the failing tests** in `test_table_columns.cpp` (header mutation lines: "`apply` ignoring `hidden`: `a stored layout is fitted to the table` fails"; "`rebuild` appending a new column last: `a new column takes its declared place` fails"):

```cpp
TEST_CASE("column layout: a stored layout is fitted to the table", "[table][layout]") {
    ColumnLayout layout{{"id", "name", "a", "new", "b"}};
    layout.apply(ColumnLayoutValue{
        .order = {"b", "ghost", "name", "id", "a"}, .hidden = {"ghost", "a"}, .pinned = 9,
        .widths = {{"name", 24}, {"ghost", 10}},
        .groups = {{.id = "g1", .label = "Company", .columns = {"a", "ghost", "b"}},
                   {.id = "g2", .label = "X", .columns = {"ghost", "name"}}},
        .units = {{"a", {{"mass", "g"}}}, {"ghost", {{"mass", "kg"}}}}});
    auto const v = layout.value();
    CHECK(v.order == std::vector<std::string>{"b", "name", "id", "a", "new"});   // "new" after its declared predecessor "a"
    CHECK(v.hidden == std::vector<std::string>{"a"});
    CHECK(layout.visible() == std::vector<std::string>{"b", "name", "id", "new"});
    CHECK(v.pinned == 4);
    CHECK(v.widths == std::map<std::string, int>{{"name", 24}});
    REQUIRE(v.groups.size() == 1);
    CHECK(v.groups[0].columns == std::vector<std::string>{"a", "b"});
    CHECK(v.units == std::map<std::string, std::map<std::string, std::string>>{{"a", {{"mass", "g"}}}});
}

TEST_CASE("column layout: every column hidden shows the first declared column", "[table][layout]") {
    ColumnLayout layout{{"id", "name"}};
    layout.apply(ColumnLayoutValue{.order = {"name", "id"}, .hidden = {"name", "id"}});
    CHECK(layout.visible() == std::vector<std::string>{"id"});
}

TEST_CASE("column layout: a new column takes its declared place", "[table][layout]") {
    ColumnLayout layout{{"new0", "new1", "id", "name", "new2"}};
    layout.apply(ColumnLayoutValue{.order = {"name", "id"}});
    CHECK(std::vector<std::string>(layout.order().begin(), layout.order().end()) ==
          std::vector<std::string>{"new0", "new1", "name", "new2", "id"});      // new0 first (no placed predecessor),
                                                                                  // new1 after new0, new2 after name
    ColumnLayout grown{{"a", "b", "c"}};
    grown.move("c", 0);
    grown.rebuild({"a", "b", "x", "c"});
    CHECK(std::vector<std::string>(grown.order().begin(), grown.order().end()) ==
          std::vector<std::string>{"c", "a", "b", "x"});
}

TEST_CASE("column layout: widths and pinning", "[table][layout]") {
    ColumnLayout layout{{"a", "b", "c"}, {"c"}};
    CHECK(layout.visible() == std::vector<std::string>{"a", "b"});           // declared hidden
    CHECK(layout.setWidth("a", 30));
    CHECK_FALSE(layout.setWidth("zzz", 30));
    CHECK_FALSE(layout.setWidth("b", 0));
    CHECK(layout.widths() == std::map<std::string, int>{{"a", 30}});
    CHECK(layout.setWidth("a", std::nullopt));
    CHECK(layout.widths().empty());
    layout.setPinned(5);
    CHECK(layout.pinned() == 2);
    CHECK(layout.setVisible("b", false));
    CHECK(layout.pinned() == 1);                                              // clamped as visibility changes
    layout.setWidth("a", 12);
    layout.reset();
    CHECK(layout.visible() == std::vector<std::string>{"a", "b"});
    CHECK(layout.widths().empty());
    CHECK(layout.pinned() == 0);
}

TEST_CASE("column layout: the stored shape round-trips as JSON", "[table][layout]") {
    std::string json;
    REQUIRE_FALSE(glz::write_json(fitted.value(), json));
    ColumnLayoutValue back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back == fitted.value());
    CHECK_FALSE(glz::read_json(back, std::string{R"({"order":["a"],"hidden":[],"pinned":1,"widths":{"a":24}})"}));  // groups, units absent
}

TEST_CASE("column layout: a stored layout that repeats itself", "[table][layout]") {   // Review Focus 5
    ColumnLayout layout{{"a", "b", "c"}};
    layout.apply(ColumnLayoutValue{.order = {"b", "a", "b", "c"}, .pinned = 40,
                                   .widths = {{"a", 0}, {"b", -5}, {"c", 8}},
                                   .groups = {{.id = "g", .label = "G", .columns = {"a", "a", "c"}}}});
    CHECK(layout.value().order == std::vector<std::string>{"b", "a", "c"});
    CHECK(layout.value().widths == std::map<std::string, int>{{"c", 8}});
    CHECK(layout.pinned() == 3);
    CHECK(layout.value().groups[0].columns == std::vector<std::string>{"a", "c"});
}
```

- [ ] **Step 2: Update** `"table: a column layout keeps one column visible and resets"` in `test_table_engine.cpp`: after `rebuild({"b", "c", "d"})` the order is `{"c", "d", "b"}` (d after its declared predecessor c) and `visible()` is `{"c", "d"}`.

- [ ] **Step 3: Run; the new cases fail to compile** (`ColumnLayoutValue` undeclared), and the updated engine case fails on the order line.

- [ ] **Step 4: Implement** in `engine.hpp`. `glz::meta` is not needed for `ColumnLayoutValue` (reflection), and reading leaves an absent member at its default. One function fits every input; `apply(value)` runs it with the current declared ids, `rebuild(ids, declaredHidden)` replaces the declared ids and declared hidden set and runs it on `value()`, `reset()` sets order to the declared ids, hidden to the declared hidden set, clears widths and `pinned`, and keeps `groups` and `units`:

```text
fit(value, D = declared ids):
  order  = ids of value.order that are in D, first occurrence only
  for d in D, in declared order, if d not in order:
      p = nearest id before d in D that is in order
      insert d right after p, or at the front when there is none
  hidden = ids of value.hidden in D, listed in `order`'s order       (an unmentioned column is shown)
  widths = entries of value.widths with id in D and width > 0
  groups = each group with its columns filtered to D, duplicates removed; groups with < 2 columns dropped
  units  = entries of value.units with column id in D
  if D is not empty and no column of order is visible: show D[0]
  pinned = min(value.pinned, number of visible columns)
```

  `setVisible` and `rebuild` re-clamp `pinned`; `pinned()` counts the first visible columns of `order`.

- [ ] **Step 5: Run** `ctest --test-dir build/cl-debug -R "column layout" --output-on-failure`; expected: all pass, the two existing engine layout cases included.

- [ ] **Step 6: Mutations.** (a) `fit` taking `hidden` as empty: `"a stored layout is fitted to the table"` fails. (b) the new-column loop appending at the end: `"a new column takes its declared place"` fails. (c) `pinned` not clamped: `"widths and pinning"` fails at `pinned() == 2`. Paste each; revert.

- [ ] **Step 7: Docs build; commit** `feat(table): widths, pinning and fitting in the column layout`.

---

## Self-review

- **Spec coverage.** Value-set operator (8a Steps 1, 5, 6); its limits and wire bound D6 (8a Step 2); distinct values — which rows, which values, unloaded, order and bound, dates, levels (the column is read for every candidate, through `apply`'s key building), client mode, server mode, validation, caching (8a Steps 3, 7, 8); column groups — declaring, user-formed groups (fitted in 8d), the filter and its JSON, both truth tables, `Together`, excludes, the pooled values list, `x-group` (8b); values with units — registry, lineage, tombstones, unit-id rule, ratio, wire, typed rows, order, filters, `parseMeasure`, values list (8c); display units per dimension are stored by 8d and drawn by Task 15; the layout (8d). The menu halves, D7, D8 and D9 are Tasks 10 and 12, as the roadmap assigns. Neighbouring specs for step 3: `../forms/views.md` (8b Step 8); `engine.md`'s fitting-rule row is step 4, not here.
- **Type consistency.** `FilterCounters` is introduced in 8a and used by 8b; `ValueSet` in 8a, extended to measures in 8c; `DistinctValue` gains `dimension`/`unit` in 8c only; `ColumnGroupInfo` moves to `data_source.hpp` in 8b and is consumed by 8d; `GroupFilter`'s member order keeps the existing designated initializers valid.
- **Proportion.** Code blocks are tests and signatures; the only bodies are the group evaluation, the measure comparison and the fitting rule, which the spec fixes and the tests alone would not determine.
