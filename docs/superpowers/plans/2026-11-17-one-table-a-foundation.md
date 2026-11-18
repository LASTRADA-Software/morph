# The One Table, Part A: the Fixture, the Description and the Generic Page — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land table.md Delivery order steps 0, 0a and 1: the wide-table fixture and its baselines, the removal of paging in a database, and the generic table contract (a table as a run-time description, served by any model through `BRIDGE_REGISTER_TABLE`, answered as a generic page on `tableWork`).

**Architecture:** The fixture is a column-store `DataSource` generated from a seed, in a static test library every test executable links. Paging in a database leaves the library and the lims examples, which read their rows and answer with `apply`. A new header `include/morph/table/description.hpp` holds the description, the generic page and its codec, and the bridge-facing types; `BRIDGE_REGISTER_TABLE` registers two actions and one keyed record per table, and the result walk learns to find a table's keys inside `ChangedRows<fn>`.

**Tech Stack:** C++23, glaze, Catch2, CMake presets `cl-debug` / `cl-release` (MSVC), Emscripten 4.0.7 + Node for the WebAssembly table tests, Qt 6 Quick offscreen for the frontend baseline, POSIX `morph::net` for the socket tests.

**Spec:** `docs/spec/table/table.md`, sections "Performance" (the fixture, the data, how figures are taken, and the baseline rows of step 0), "A table is a description", "A model's tables" (with "The table's record"), "Typed rows are a convenience", "Rows on the wire", "A table on the server", "Alternatives considered" (the "Paging in the database" item), the rows of "Tests" and "Invariants" named in each task, the step-0a and step-1 rows of "Neighbouring specs", and "Delivery order" steps 0, 0a, 1. Roadmap: `docs/superpowers/plans/2026-11-17-one-table.md` (its Global Constraints, standard task loop and task numbers apply unchanged).

Part A of 7; index: 2026-11-17-one-table.md

## Global Constraints

Everything in the roadmap's **Global Constraints** applies. In addition, for this part:

- The fixture has no row type anywhere: its description is built from `WideTableOptions`, and a reply is the generic page written from the arrays (table.md, "The fixture").
- "The same seed gives the same table on every machine": no `std::uniform_*_distribution`, no `std::random_device`, no clock in the generator; cells come only from the integer hash this plan pins.
- "A benchmark is recorded from the step that lands what it measures. No baseline is taken of a thing that does not exist."
- Timings are never CI gates; counts are, and run as ordinary tests. Benchmarks are tagged `"[.benchmark]"` and first assert that the path they time was reached.
- Timings are taken on Release, median and 10th–90th percentile over at least seven runs at 100,000 rows, with the machine's load recorded beside them (`engine.md`, "Measurements").
- `QueryLimits::maxColumns = 512` (roadmap Global Constraints).
- A `TableDescription`'s `revision` is filled by the framework from its canonical JSON; a model leaves it empty.
- The action ids a table registers are `Table.<id>` and `Table.<id>.describe`; its record is `<model type id>.<table id>`.

## Implementer models

Per the user's standing instructions: Task 1 is fully specified (Haiku). Tasks 0, 0a and 2 need investigation across layers (the Emscripten build, the examples' data access, the dispatcher's handler call) and go to Opus. Every review goes to Opus.

## Review Focus

1. **A `columns` list that is not clean.** Duplicates, ids out of the description's order, an id the description lacks next to valid ones, an empty list: the page names each valid column once, in the description's order; an unknown id is `UnknownColumn`; an empty list is a page of keys. Test in Task 1: `"table query: a columns list is answered in the description's order, once each"`.
2. **Numbers JSON cannot hold.** `NaN`, `+inf` and `-inf` in a `Number` column; `INT64_MIN`/`INT64_MAX` integers; a `Rational` whose scaled numerator would overflow: each crosses the wire and reads back as the same cell, so the client keys it as the server did. Test in Task 1: `"generic page: numbers at the edges cross the wire unchanged"`.
3. **A reply that is malformed.** A row whose length differs from `columns`, `keys` and `rows` of different lengths, a `columns` entry the description lacks, a cell `[1, 0]` or `{"x": 1}`: the structural faults refuse the page with a `TableError`; a cell fault keeps the page and makes that one cell invalid. Test in Task 1: `"generic page: a malformed reply is refused and a malformed cell is invalid"`.
4. **The same seed on two machines.** A fixture that agrees with itself on one machine and differs between MSVC and Clang passes every same-process test. Test in Task 0: `"wide table: seed 7 has the pinned digest"`, run in the Windows and Linux CI legs and under Node.
5. **A model whose table function misbehaves.** It throws, or returns a source without a column its description declares: the action replies with an error, the next request to the same model succeeds, and no reply holds a partial page. Test in Task 2: `"table action: a failing table function is an error reply and the model stays usable"`.

None of the roadmap's Review Focus items falls in this part.

---

### Task 0: The fixture and the baseline (table.md step 0)

**Files:**
- Create: `tests/table/wide_table_model.hpp`, `tests/table/wide_table_model.cpp` (static library `morph_wide_table_model`), `tests/table/test_wide_table.cpp` (in `morph_table_tests`), `tests/qt_quick/bench_qt_quick_table.cpp` (in `morph_qt_quick_tests`), `tests/qt_quick/wide_rows_document.hpp` (the `rows` document F2–F5 and B5 both mount), `tests/qt_quick/wasm/wide_rows_baseline.cpp` and `tests/qt_quick/wasm/CMakeLists.txt` (B5's browser harness)
- Modify: `include/morph/table/data_source.hpp:71-76` (`QuantityCell` gains `[[nodiscard]] bool operator==(QuantityCell const&) const = default;` with its Doxygen, so `Cell == Cell` compiles; index decision 38), `tests/table/table_fixtures.hpp` (`tabletest::sameCell`)
- Modify: `CMakeLists.txt:1252` (an `if(EMSCRIPTEN AND MORPH_BUILD_QT_QUICK AND MORPH_BUILD_TESTS)` branch adding `tests/qt_quick/wasm`), `tests/CMakeLists.txt:882-909` (the library; `morph_table_tests` links it and `test_wide_table.cpp`; `morph_tests` links it; an `if(EMSCRIPTEN)` branch that builds only `morph_wide_table_model` and `morph_table_tests`), `tests/net/CMakeLists.txt:13-17`, `tests/qt_quick/CMakeLists.txt` (link and the bench file), `tests/table/bench_table.cpp`, `tests/bench_interpreter.cpp`, `tests/table/test_table_engine.cpp:210` (tag `[threads]`), `.github/workflows/ci.yml` (new job `table-wasm`), `docs/spec/table/table.md` (a "Recorded baselines" table at the end of "Performance", before "## Invariants")

**Interfaces:**
- Consumes: `table::DataSource`, `RowSnapshot::readColumn`, `ColumnSink`, `table::apply`, `Engine` (all existing, `include/morph/table/`).
- Produces (namespace `bench`, header `tests/table/wide_table_model.hpp`):

```cpp
enum class WideRole : std::uint8_t { Key, Integer, Decimal, Quantity, Measure, Text, LongText, Date, DateTime, Bool };

struct WideColumn {
    morph::table::ColumnInfo info;   // id and kind; Measure-role columns are ColumnKind::Decimal until Task 8c
    WideRole role;
    bool sparse = false;             // 10% of its cells empty
    std::string level;               // "recipe" or empty; used from Task 2 on (ColumnInfo::level lands in Task 3)
    std::string group;               // "g3", "g16" or empty
};

struct WideTableOptions {
    std::int64_t rows = 100'000;
    int columns = 300;               // 300 or 30; anything else throws std::invalid_argument
    std::uint64_t seed = 1;
    std::chrono::microseconds loadDelay{0};   // read by the slow level from Task 5 on
};

[[nodiscard]] std::uint64_t cellBits(std::uint64_t seed, std::string_view column, std::int64_t row) noexcept;
[[nodiscard]] std::vector<WideColumn> wideColumns(int columns);
[[nodiscard]] std::size_t approximateCellBytes(morph::table::Cell const& cell) noexcept;

class WideTableSource final : public morph::table::DataSource {
public:
    explicit WideTableSource(WideTableOptions options);
    std::span<morph::table::ColumnInfo const> columns() const override;
    std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override;
    void subscribe(morph::table::ChangeListener& listener) override;
    void unsubscribe(morph::table::ChangeListener& listener) override;
    [[nodiscard]] std::vector<WideColumn> const& layout() const noexcept;
    [[nodiscard]] WideTableOptions const& options() const noexcept;
    [[nodiscard]] std::size_t residentBytes() const noexcept;      // bytes of the column arrays, capacity included
    void setCell(std::int64_t row, std::string_view column, morph::table::Cell value);   // copy-on-write of that one column; notifies Updated
    void regenerate(std::uint64_t seed);                                                 // notifies Reset
    // snapshot(), setCell() and regenerate() hold one mutex around reading or swapping the snapshot pointer; listeners are
    // notified after the lock is released. The source is shared by every model instance's strand (Part B's fifty clients).
};
```

- Produces (namespace `tabletest`, `tests/table/table_fixtures.hpp`): `[[nodiscard]] inline bool sameCell(morph::table::Cell const& a, morph::table::Cell const& b)`, true when both hold the same alternative and equal values, and, for a `Rational`, equal `decimalPlaces` too (for a `QuantityCell`, of `amount` and of `toCanonical`). `Rational::operator==` compares the value only (`util/rational.hpp:692`), so `Cell == Cell` means "the same value" and `sameCell` means "the same cell, places included"; a test that pins places uses `sameCell`.

**The layout** (`wideColumns(300)`, in declared order; position `p` counts from 0):

| Ids | Role, kind | Count |
|---|---|---|
| `id` | `Key` role, `ColumnKind::Integer`, the row key, value `row + 1`, never empty | 1 |
| `i001`…`i099` | Integer | 99 |
| `d000`…`d059` | Decimal; `d000`…`d009` have `level = "recipe"` | 60 |
| `q000`…`q029` | Quantity, canonical unit gram | 30 |
| `m000`…`m009` | Measure role, `ColumnKind::Decimal` | 10 |
| `t000` | LongText, `ColumnKind::Text` | 1 |
| `t001`…`t069` | Text; `t001`…`t010` have `level = "recipe"`; `t004`…`t006` have `group = "g3"`; `t011`…`t026` have `group = "g16"` | 69 |
| `a000`…`a009` | Date | 10 |
| `s000`…`s009` | DateTime | 10 |
| `b000`…`b009` | Bool | 10 |

`sparse` is true exactly when `p % 12 == 11` (25 columns). `wideColumns(30)` keeps, in the same order, `id`, `i001`…`i009`, `d000`…`d005`, `q000`…`q002`, `m000`, `t000`…`t006`, `a000`, `s000`, `b000`, each with every attribute it has in the 300 layout, `sparse` included, so a column shared by `wide` and `small` has the same cells in both (table.md, "The fixture": the two tables share the rows they have in common).

**The generator.** A cell depends only on `(seed, column id, row)`, so `wide` and `small` agree on the rows and columns they share:

```cpp
// splitmix64: z += 0x9E3779B97F4A7C15; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
//             z = (z ^ (z >> 27)) * 0x94D049BB133111EB; return z ^ (z >> 31);
cellBits(seed, column, row) = splitmix64(splitmix64(seed ^ fnv1a64(column)) + static_cast<std::uint64_t>(row));
```

`fnv1a64` is `morph::model::detail::fnv1a64` (`include/morph/core/payload_schema.hpp:213`). With `b = cellBits(...)`: a sparse column's cell is empty when `b % 10 == 0`; otherwise by role —
Integer `static_cast<std::int64_t>(b % 2'000'001) - 1'000'000`;
Decimal `Rational{Numerator{static_cast<std::int64_t>(b % 20'000'001) - 10'000'000}, Denominator{100}, DecimalPlaces{2}}`;
Quantity with `n = b % 10'000'001`: when `(b >> 32) & 1` a `QuantityCell{Rational{n, 1'000'000, 6} /* kg */, Rational{1000, 1, 0}}`, otherwise `Rational{n, 1000, 3}` (grams);
Measure role `Rational{static_cast<std::int64_t>(b % 1'000'001), 100, 2}`;
Text: length `4 + b % 57` (LongText: `200 + b % 1801`) characters, each the entry `syllables[splitmix64(b + i) % 32]` of the fixed table `{"a","e","i","o","u","b","c","d","f","g","k","l","m","n","p","r","s","t","v","z","É","é","Ä","ä","Ö","ø","Ü","ç","ß","Z","K","M"}`, truncated to the length in UTF-8 code points;
Date `static_cast<std::int64_t>(b % 20'000)`; DateTime `static_cast<std::int64_t>(b % 1'700'000'000)`; Bool `(b & 1) != 0`.

**The column store.** One typed array per column (`std::vector<std::int64_t>` for integer, date, date-time, decimal numerators at a fixed denominator and quantity numerators; one `std::uint8_t` unit flag for quantities and one for booleans; texts as one `std::string` pool with `std::uint32_t` offsets; a `std::vector<bool>` empty mask for sparse columns), generated once in the constructor. The snapshot holds `std::shared_ptr` to each column's arrays; `readColumn` converts into a local buffer of 256 cells and hands each run to the sink in one `cells` call.

- [ ] **Step 1: Write the failing tests** in `tests/table/test_wide_table.cpp`

```cpp
TEST_CASE("wide table: the same seed gives the same cells", "[table][wide]") {
    bench::WideTableSource const a{{.rows = 1'000, .columns = 300, .seed = 7}};
    bench::WideTableSource const b{{.rows = 1'000, .columns = 300, .seed = 7}};
    auto const sa = a.snapshot(); auto const sb = b.snapshot();
    REQUIRE(sa->rowCount() == 1'000);
    for (std::size_t r = 0; r < 1'000; ++r) {
        REQUIRE(sa->rowId(r) == sb->rowId(r));
        for (std::size_t c = 0; c < 300; ++c) { REQUIRE(tabletest::sameCell(sa->cell(r, c), sb->cell(r, c))); }   // Mutation: seed from std::random_device
    }
    bench::WideTableSource const other{{.rows = 1'000, .columns = 300, .seed = 8}};
    CHECK_FALSE(other.snapshot()->cell(0, 1) == sa->cell(0, 1));   // Mutation: generator ignoring the seed
}

TEST_CASE("wide table: the column mix is the spec's", "[table][wide]") {
    auto const roles = [](int n) { std::map<bench::WideRole, int> count; for (auto const& c : bench::wideColumns(n)) ++count[c.role]; return count; };
    auto const wide = roles(300);
    CHECK(wide.at(bench::WideRole::Key) + wide.at(bench::WideRole::Integer) == 100);
    CHECK(wide.at(bench::WideRole::Decimal) == 60);
    CHECK(wide.at(bench::WideRole::Quantity) == 30);
    CHECK(wide.at(bench::WideRole::Measure) == 10);
    CHECK(wide.at(bench::WideRole::Text) + wide.at(bench::WideRole::LongText) == 70);
    CHECK(wide.at(bench::WideRole::LongText) == 1);
    CHECK(wide.at(bench::WideRole::Date) + wide.at(bench::WideRole::DateTime) == 20);
    CHECK(wide.at(bench::WideRole::Bool) == 10);
    auto const columns = bench::wideColumns(300);
    CHECK(std::ranges::count_if(columns, &bench::WideColumn::sparse) == 25);
    CHECK(std::ranges::count(columns, std::string{"recipe"}, &bench::WideColumn::level) == 20);
    CHECK(std::ranges::count(columns, std::string{"g3"}, &bench::WideColumn::group) == 3);
    CHECK(std::ranges::count(columns, std::string{"g16"}, &bench::WideColumn::group) == 16);
    CHECK(bench::wideColumns(30).size() == 30);
    // One column in twelve has 10% empty cells: 1,000 rows of a sparse column hold between 50 and 150 empties.
    bench::WideTableSource const source{{.rows = 1'000, .columns = 300, .seed = 7}};
    auto const snap = source.snapshot();
    auto const empties = [&](std::size_t c) { int n = 0; for (std::size_t r = 0; r < 1'000; ++r) n += std::holds_alternative<std::monostate>(snap->cell(r, c)); return n; };
    CHECK(empties(11) > 50); CHECK(empties(11) < 150);   // position 11 is sparse
    CHECK(empties(0) == 0);                                // `id` is never empty
    // Mutation: sparse every 11th column instead of every 12th.
}

TEST_CASE("wide table: shared rows agree between 300 and 30 columns", "[table][wide]") {
    bench::WideTableSource const wide{{.rows = 2'000, .columns = 300, .seed = 7}};
    bench::WideTableSource const small{{.rows = 1'000, .columns = 30, .seed = 7}};
    auto const ws = wide.snapshot(); auto const ss = small.snapshot();
    auto const wideIds = wide.columns();
    int empties = 0;
    for (std::size_t c = 0; c < small.columns().size(); ++c) {
        auto const at = std::ranges::find(wideIds, small.columns()[c].id, &morph::table::ColumnInfo::id);
        REQUIRE(at != wideIds.end());
        auto const w = static_cast<std::size_t>(at - wideIds.begin());
        CHECK(small.layout()[c].sparse == wide.layout()[w].sparse);
        for (std::size_t r = 0; r < 1'000; ++r) {
            REQUIRE(tabletest::sameCell(ss->cell(r, c), ws->cell(r, w)));
            empties += std::holds_alternative<std::monostate>(ss->cell(r, c)) ? 1 : 0;
        }
    }
    CHECK(empties > 0);   // `t003` is sparse in both layouts, so empty cells were compared
    // Mutation: wideColumns(30) recomputing `sparse` from the 30 layout's positions.
    // Mutation: hashing the column's position instead of its id.
}

TEST_CASE("table fixtures: sameCell tells places apart and operator== does not", "[table][wide]") {
    using morph::math::Rational; using morph::math::Numerator; using morph::math::Denominator; using morph::math::DecimalPlaces;
    Cell const two{Rational{Numerator{5}, Denominator{2}, DecimalPlaces{2}}};
    Cell const four{Rational{Numerator{5}, Denominator{2}, DecimalPlaces{4}}};
    CHECK(two == four);
    CHECK_FALSE(tabletest::sameCell(two, four));
    CHECK(tabletest::sameCell(two, two));
    Cell const kg{QuantityCell{std::get<Rational>(two), Rational{Numerator{1000}, Denominator{1}, DecimalPlaces{0}}}};
    Cell const g{QuantityCell{std::get<Rational>(two), Rational{Numerator{1}, Denominator{1}, DecimalPlaces{0}}}};
    CHECK(kg == kg);
    CHECK_FALSE(kg == g);                                   // Mutation: QuantityCell::operator== comparing `amount` only
    CHECK_FALSE(tabletest::sameCell(two, Cell{std::int64_t{2}}));   // Mutation: sameCell delegating to operator==
}

TEST_CASE("wide table: seed 7 has the pinned digest", "[table][wide]") {
    // FNV-1a 64 over displayText(cell) of rows 0..999, columns 0..299, each followed by '\x1f'.
    CHECK(bench::digestForTesting(bench::WideTableSource{{.rows = 1'000, .columns = 300, .seed = 7}}) == kPinnedDigest);
    // kPinnedDigest is computed once by the implementer on MSVC and checked unchanged by the Linux leg and Node.
    // Mutation: a std::uniform_int_distribution in the generator (passes on one standard library, fails on another).
}

TEST_CASE("wide table: apply reads only the sorted and filtered columns", "[table][wide]") {
    // Gate S3. A RecordingSource wraps a WideTableSource{1'000, 300, 7}; its snapshot counts cells handed out per column
    // through both cell() and readColumn().
    // Query: sort {i001 asc}, filter t001 contains "a", page {0, 200}.
    // CHECK: counts[i001] == 1'000, counts[t001] == 1'000, and the other 298 counts are 0.
    // Mutation: apply listing every column in JobInput::needed.
}

TEST_CASE("wide table: an edit changes one cell and copies one column", "[table][wide]") {
    // setCell(5, "i001", Cell{std::int64_t{42}}): the new snapshot's cell(5, i001) is 42; the snapshot taken before
    // still reads the old value; a subscribed listener got one RowChange{Updated, {5}}.
    // Mutation: setCell writing into the shared array (the old snapshot changes too).
}
```

`bench::digestForTesting(WideTableSource const&) -> std::uint64_t` is declared in the fixture header for this test.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset cl-debug && cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile failure, `wide_table_model.hpp` not found (add `table/test_wide_table.cpp` to `morph_table_tests` in `tests/CMakeLists.txt` first, so the build sees it).

- [ ] **Step 3: Implement the fixture** in `tests/table/wide_table_model.{hpp,cpp}` with the interface, layout and generator above, `QuantityCell::operator==` in `data_source.hpp`, `tabletest::sameCell` in `table_fixtures.hpp`, and add to `tests/CMakeLists.txt`:

```cmake
add_library(morph_wide_table_model STATIC table/wide_table_model.cpp)
target_link_libraries(morph_wide_table_model PUBLIC morph::morph)
target_include_directories(morph_wide_table_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
apply_warnings(morph_wide_table_model)
```

`morph_table_tests`, `morph_tests`, `morph_net_tests` and `morph_qt_quick_tests` link it as `"$<LINK_LIBRARY:WHOLE_ARCHIVE,morph_wide_table_model>"` (the ladder's precedent, `cmake/morph_add_rung.cmake:209`), so the registrars Task 2 adds are never dropped. Tag the case at `tests/table/test_table_engine.cpp:210` `[threads]`.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset cl-debug --target morph_table_tests && ctest --test-dir build/cl-debug -R "wide table" --output-on-failure`
Expected: 6 tests pass, and `ctest --test-dir build/cl-debug -R "table fixtures" --output-on-failure` 1 more; then the rest of the target: `ctest --test-dir build/cl-debug -R "^table" --output-on-failure` all pass.

- [ ] **Step 5: Build the table tests for WebAssembly and run them under Node**

In `tests/CMakeLists.txt`, at the top after the pinned-facts block: under `if(EMSCRIPTEN)`, define `morph_wide_table_model` and `morph_table_tests` exactly as above, register no `catch_discover_tests`, and `return()`. Emscripten's default heap is a fixed 16 MiB and no target in this repository sets another, so the branch also gives the executable a heap that grows:

```cmake
target_link_options(morph_table_tests PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
```

That holds the 100,000 × 300 column store B2 runs S1 and S2 over. S8's `VectorSource` of the same rows (near 1.7 GB of cells, and its keys beside them) is native only: its case is compiled out under `__EMSCRIPTEN__`. Add to `.github/workflows/ci.yml` a job:

```yaml
  table-wasm:
    name: Table tests (WebAssembly, Node)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: mymindstorm/setup-emsdk@v14
        with: { version: 4.0.7 }
      - name: Configure
        run: emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_EXAMPLES=OFF
      - name: Build
        run: cmake --build build-wasm --target morph_table_tests
      - name: Run
        run: node build-wasm/tests/morph_table_tests.js "~[threads]"
```

Run locally (with emsdk active): the same three commands.
Expected: `All tests passed` with the digest case among them. If a header of `morph::morph` does not build under Emscripten, the fix belongs to this commit (AGENTS.md: build configuration is fixed in the change that needs it); a defect in `include/morph` found this way is filed.

- [ ] **Step 6: Write the baseline benchmarks**

Each case is `"[.benchmark][baseline]"`, prints Markdown rows as `bench_table.cpp`'s `printRow` does, and first `REQUIRE`s the guard in the last column.

| Id | File, case name | What it times or counts | Guard |
|---|---|---|---|
| S1 | `bench_table.cpp`, `"table baseline: S1 S2 S7 over the wide table"` | `apply`, sort `d000` asc, page {0, 200}, 100,000 × 300 | the page has 200 rows and the view 100,000 |
| S2 | same case | `apply`, filter `t001` contains `"ka"` | the view is neither empty nor all rows |
| S7 | same case | ten consecutive `apply` calls of S1's query, offsets 0, 200, … 1,800: total and per call | each page's first key differs |
| S8 | `bench_table.cpp`, `"table baseline: S8 resident bytes"` | `WideTableSource::residentBytes()` at 100,000 × 300, and the sum of `approximateCellBytes` over the same rows held as a `VectorSource` | the `VectorSource` has 100,000 rows |
| B1 | `bench_table.cpp`, `"table baseline: B1 client mode with no worker"` | an `Engine` over `WideTableSource{rows, 30}` with an owner and no worker, at 10,000, 20,000, 100,000 rows: sort on `a000` asc, `d000` desc, `i001` asc, then filter `t001` contains `"ka"`; request to result, and the longest owner step | the view's first row changed after the sort |
| B2 | the S1, S2 and B1 cases and W2's decode (Task 1) | run under Node from the `table-wasm` build: `node build-wasm/tests/morph_table_tests.js "[.benchmark][baseline]"`; recorded as a factor over native | as the native case |
| W7 | `bench_table.cpp`, `"table baseline: W7 as a typed page"` | bytes and encode time of a page of 200 rows × 300 columns written as row objects keyed by column id, each cell as glaze writes the typed member (`Rational` as its `{"num","den","dp"}` object) | the page holds 60,000 cells |
| U1 | `tests/bench_interpreter.cpp`, `"interpreter baseline: U1 U2 a rows table of 100,000"` | a `ui/1` document whose `table` reads state `items` (100,000 rows of `id`, `name` from `t001`, `qty` from `i001`) and binds `sort` to state `order`; a button sets `order` to `[{"column":"name","direction":"asc"}]`; click to settled | the first rendered row's `name` changed |
| U2 | same case | the sorted table, a button replacing one row's `name` in `items`; click to settled | that row moved |
| U3 | `bench_interpreter.cpp`, `"interpreter baseline: U3 folds per sort"` | the U1 sort with a counting collator installed where the interpreter takes its collator; folds performed | the count is above 100,000 |
| U4 | `bench_interpreter.cpp`, `"interpreter baseline: U4 memory of a rows table"` | 10,000 × 300 rows as document values, sorted on one column: `approximateCellBytes` sum against the document's value bytes | the table mounted with 10,000 rows |
| U5 | `bench_interpreter.cpp`, `"interpreter baseline: U5 a collection of 20,000"` | a forms-engine collection over a `ListRows` reply of 20,000 rows, as `tests/test_forms_engine_collection.cpp` builds one: mount, sort, one row edited | the collection holds 20,000 rows |
| F2–F5 | `tests/qt_quick/bench_qt_quick_table.cpp`, `"qt quick baseline: F2 to F5 on a rows table of 2,000 x 300"` | a `rows` table of `WideTableSource{2'000, 300}` cells in a 1,280 × 800 offscreen window: widgets alive (F2), frame time of vertical (20 rows/s, 2,000 rows/s, thumb drag) and horizontal (10 and 100 columns/s) scrolls timed as table.md "How a frame is timed" says for CI (F3, F4), memory per realized row (F5) | the table realized at least 30 rows |
| B5 | `tests/qt_quick/wasm/wide_rows_baseline.cpp` | a Qt WebAssembly executable (`tests/qt_quick/wasm/CMakeLists.txt`, reached from the root branch below) that mounts F2's table, scrolls one screen and prints mount time and the scroll's frame times to the console; run locally in a headless browser with `emrun --browser chrome --browser_args=--headless=new` | its console shows 2,000 rows mounted |

- [ ] **Step 7: Record the baselines**

Run on a Release build, machine load recorded:
`cmake --preset cl-release && cmake --build --preset cl-release --target morph_table_tests morph_tests`
`build/cl-release/tests/morph_table_tests.exe "[.benchmark][baseline]"` and `build/cl-release/tests/morph_tests.exe "[.benchmark][baseline]"`;
`cmake --preset cl-qt-release -DMORPH_BUILD_QT_QUICK=ON && cmake --build --preset cl-qt-release --target morph_qt_quick_tests`, then with `QT_QPA_PLATFORM=offscreen`: `build/cl-qt-release/tests/qt_quick/morph_qt_quick_tests.exe "[.benchmark][baseline]"`;
B2 under Node as in the table above. B5: in the root `CMakeLists.txt`, beside `:1252` (which adds `tests/qt_quick` only when `NOT EMSCRIPTEN`), add `if(EMSCRIPTEN AND MORPH_BUILD_QT_QUICK AND MORPH_BUILD_TESTS) add_subdirectory(tests/qt_quick/wasm) endif()`; configure with the form of `.github/workflows/wasm-demo.yml:114-119`: `"$WASM/bin/qt-cmake" -S . -B build-wasm-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH="$HOST" -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON`, build `cmake --build build-wasm-qt --target wide_rows_baseline`, and run it with `emrun` as the table says. (The `if(EMSCRIPTEN)` branch of `tests/CMakeLists.txt` returns before anything else, so this build compiles only `morph_table_tests`, its fixture and this harness.)
The B5 harness, so that nothing in it is left to choose:

- `tests/qt_quick/wide_rows_document.hpp`: `namespace bench { [[nodiscard]] std::string wideRowsDocument(WideTableSource const& source); }`, the `ui/1` document text of one `table` whose `rows` are the source's cells as literal row objects keyed by column id and whose `columns` are its 300 ids. `bench_qt_quick_table.cpp` (F2–F5) and the harness both mount this text, so B5 measures the table F2 measures.
- `tests/qt_quick/wasm/CMakeLists.txt`:

```cmake
qt_add_executable(wide_rows_baseline wide_rows_baseline.cpp)
target_link_libraries(wide_rows_baseline PRIVATE morph::qt_quick morph_wide_table_model)
target_include_directories(wide_rows_baseline PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
target_link_options(wide_rows_baseline PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
apply_warnings(wide_rows_baseline)
```

- `wide_rows_baseline.cpp`: a `main` that owns the `QGuiApplication` and a 1,280 × 800 window as `tests/qt_quick/test_qt_quick_main.cpp` and `qt_quick_harness.hpp` set one up, mounts `wideRowsDocument(WideTableSource{{.rows = 2'000, .columns = 300, .seed = 7}})`, and on the first frame after the mount prints `mounted rows=2000 ms=<mount time>`; it then scrolls the table down by one viewport over one second and prints `frame ms=<t>` per frame and `done` at the end. It links no Catch2: its guard is the `mounted rows=2000` line.

Expected: every case prints its rows and none fails its guard. Add to table.md, at the end of "Performance":

```markdown
### Recorded baselines

| Id | Step | Figure | Machine, load | Date |
|---|---|---|---|---|
```

one row per figure (S1, S2, S7, S8, B1 ×3 native, B2 for S1, S2, B1 ×3 and W2 under Node, B5, U1–U5, W7, F2–F5), the figure as median and p10–p90.

- [ ] **Step 8: Mutations, then commit**

Apply each mutation named in Step 1, see its test fail, revert; paste the failing lines into the commit body.

```bash
git add include/morph/table/data_source.hpp tests/table/table_fixtures.hpp \
        tests/table/wide_table_model.hpp tests/table/wide_table_model.cpp tests/table/test_wide_table.cpp \
        tests/table/bench_table.cpp tests/table/test_table_engine.cpp tests/bench_interpreter.cpp \
        tests/qt_quick/bench_qt_quick_table.cpp tests/qt_quick/wide_rows_document.hpp tests/qt_quick/wasm tests/qt_quick/CMakeLists.txt \
        CMakeLists.txt tests/CMakeLists.txt tests/net/CMakeLists.txt .github/workflows/ci.yml docs/spec/table/table.md
git commit -m "test(table): wide-table fixture and baselines"
```

---

### Task 0a: Remove paging in a database (table.md step 0a)

**Files:**
- Delete: `include/morph/table/plan_conformance.hpp`, `tests/table/test_table_database_pages.cpp`, `examples/lims/org/include/lims/org/core/plan_sql.hpp`, `docs/spec/table/database_pages.md`
- Modify: `include/morph/table/query.hpp:1-16` (file comment: "or with a database query that honours the same semantics" goes), `:252-268` (`escapeLikePattern` goes), `:354-592` (the "Query plans" section: `PlanUse`, `FilterOp`, `PlannedOperand`, `PlannedSortKey`, `PlannedCondition`, `PlannedColumnFilter`, `PlannedGroup`, `QueryPlan`, `detail::plannedOperand`, `detail::plannedCondition`, `detail::plannedColumnFilter`, `detail::untranslatable`, `planQuery`); no `QueryLimits` member is used only by plans, so `QueryLimits` is unchanged
- Modify: `include/morph/table/filter.hpp:645` (the comment becomes "The one compile step `apply` and `Engine` share, so a throwing service is an error value in both."), `CMakeLists.txt:722`, `tests/CMakeLists.txt:36`, `tests/table/test_table_query.cpp` (receives the moved cases; loses `"table: LIKE patterns keep % and _ ordinary"`)
- Modify: `docs/spec/table/engine.md:52-53` (the `query.hpp` row loses `escapeLikePattern`, `QueryPlan`, `planQuery`; the `plan_conformance.hpp` row goes), `:300-306` and `:311-312` (see Step 5), `docs/spec/README.md:138-139`
- Modify: `examples/lims/lab/include/lims/lab/models/sample_model.hpp:96-110`, `examples/lims/lab/src/models/sample_model.cpp:280-390`, `examples/lims/ledger/include/lims/ledger/models/ledger_model.hpp:166-191`, `examples/lims/ledger/src/models/ledger_entries.cpp:535-580`, `examples/lims/tests/test_sample_pages.cpp`, `examples/lims/tests/test_ledger_entries.cpp:1440-1785`, `examples/lims/README.md:1795-1846`

**Interfaces:**
- Consumes: `table::apply(std::vector<Row> const&, RowKey<Row>, TableQuery const&, ApplyOptions const&)` (`query.hpp:891`), `validate` (`query.hpp:228`).
- Produces: `lims::SampleModel::pageSamples(std::vector<SiteId> const& sites, ::morph::table::TableQuery const& query) -> ::morph::table::Page<SampleView>` and `lims::LedgerModel::pageEntries(CompanyId company, ::morph::table::TableQuery const& query) -> ::morph::table::Page<JournalEntryView>`, each throwing `ValidationError{"<Action>: " + error.message}` on a `TableError`. `canTranslateSampleColumn` and `canTranslateEntryColumn` go.

**Test inventory.** `tests/table/test_table_database_pages.cpp` holds 14 cases:

| Case | Fate |
|---|---|
| `"table: a total is exact or a lower bound, and a lower bound does not end the window"` | Moved to `test_table_query.cpp`, tag `[table][query]`, with its mutation line ("`PageWindow` stopping at an `AtLeast` total") |
| `"table: a total kind goes over the wire by name"` | Moved, tag `[table][query]` |
| `"table: a plan is refused as apply refuses"` | Replaced by `"table: apply refuses what validate and compile refuse, by code"` (below); its other queries are already pinned by `"table: an untrusted query is bounded"` and `"table: a column the principal may not read is refused"` |
| `"table: a throwing collator is a ServiceFailed error from planQuery"` | Deleted: `"table: a collator that throws while the filter compiles is a ServiceFailed error from apply"` (`test_table_query.cpp:457`) pins the shared step |
| `"table: a model that follows the plan answers as apply does"`, `"…the conformance helper reports a model that disagrees with apply"`, `"…a model refuses what it cannot translate with Unavailable"`, `"…a plan carries the clamped page, the cap and operands typed by their column"`, `"…the standard corpus filters every column kind with operands that parse"`, `"…an exact total above the cap must equal the true count"`, `"…the corpus holds a translation to non-integral operands on integer columns"`, `"…the corpus writes date operands the date parser reads"`, `"…the corpus holds a translation to the floor of a negative instant"`, `"…the helper reports a column whose queries every side refused"` | Deleted with the file (they test plans, the corpus or the helper) |

In the lims tests:

| File, case | Fate |
|---|---|
| `test_sample_pages.cpp`, `"lab: a page of samples from the database equals the engine's page over the same rows, for every sort and filter the list offers"` | Deleted (SQL translation against `apply`) |
| `test_sample_pages.cpp`, `"lab: a plan the statement cannot translate is refused"` | Deleted |
| `test_sample_pages.cpp`, `"lab: the sample list answers through the action with an exact total"` | Kept unchanged: it pins scope (bob's total is his sites' rows) and the unknown-column refusal |
| `test_ledger_entries.cpp`, the four `"ledger: a page of entries from the database equals the engine's page … (part N of 4)"` | Deleted, with `checkEntryPagesAgainstEngine`, `kCorpusParts`, `dateQueries`, `groupQueries` |
| `test_ledger_entries.cpp`, `"ledger: the entry page refuses the columns the database cannot compare, and nothing else"` | Deleted |
| `test_ledger_entries.cpp`, `"ledger: text operands that look like SQL or like patterns are only data"` | Deleted |
| `test_ledger_entries.cpp`, `"ledger: a page of entries is the caller's, with an exact total from the database"` | Kept; renamed `"ledger: a page of entries is the caller's, with an exact total"`; it pins scope (dave gets no row) |

A helper left with no caller (`journalRowsFromLedger`, `visibleViews`, the sample `dateQueries`, anything in `lims_test_support.hpp` only these cases used) is deleted; the compiler's unused-function warning finds them.

- [ ] **Step 1: Move the cases and write the replacement** in `tests/table/test_table_query.cpp`

```cpp
TEST_CASE("table: apply refuses what validate and compile refuse, by code", "[table][query]") {
    auto const source = tabletest::makeSource({tabletest::col("id", ColumnKind::Key), tabletest::col("n", ColumnKind::Integer)},
                                              {{Cell{std::int64_t{1}}, Cell{std::int64_t{5}}}});
    auto const code = [&](TableQuery const& q) { auto r = apply(*source, q); REQUIRE_FALSE(r.has_value()); return std::pair{r.error().code, r.error().column}; };
    TableQuery far; far.page.offset = 10'000'001;
    CHECK(code(far).first == TableErrorCode::LimitExceeded);
    CHECK(code(withFilter("n", FilterEntry{.contains = "1"})) == std::pair{TableErrorCode::UnsupportedOperator, std::string{"n"}});
    CHECK(code(withFilter("n", FilterEntry{.eq = "1", .ne = "2"})) == std::pair{TableErrorCode::InvalidSpec, std::string{"n"}});
    TableQuery group; group.filters.groups.push_back(GroupFilter{.columns = {"n"}, .combine = Combine::Any, .include = {FilterEntry{.contains = "x"}}});
    CHECK(code(group) == std::pair{TableErrorCode::UnsupportedOperator, std::string{"n"}});
    // Mutation: apply skipping validate (the offset case answers an empty page).
}
```

(`withFilter` is the one-column helper the moved cases bring with them; keep a single definition.)

- [ ] **Step 2: Delete** the four files listed, remove `include/morph/table/plan_conformance.hpp` from the `FILE_SET` at `CMakeLists.txt:722` and `table/test_table_database_pages.cpp` from `tests/CMakeLists.txt:36`, remove the query-plans section and `escapeLikePattern` from `query.hpp`, and delete `"table: LIKE patterns keep % and _ ordinary"`.

- [ ] **Step 3: Rewrite the lims list actions** to read their rows and answer with `apply`

- `SampleModel::pageSamples(sites, query)`: one `mapper.Query<db::SampleRecord>().WhereIn(::Lightweight::FieldNameOf<&db::SampleRecord::siteId>, siteIds).All()` (the `WhereIn` form `examples/bank/src/models/statement_model.cpp:62` uses), each record through `toView`, then `::morph::table::apply<SampleView>(rows, [](SampleView const& s) { return RowId{*s.id}; }, query)` (the row type is named: a lambda does not deduce `Row` through `RowKey<Row>`, a `std::function`). An empty site list reads nothing (`WHERE IN ()` is not issued). `execute(ListSamples)` calls it with `OrgModel::visibleSites(currentPrincipal())`; `samplesWhere` builds its `TableQuery` as now and calls it with `callerSitesAt(company)`.
- `LedgerModel::pageEntries(company, query)`: the entries `execute(ListEntries)` already reads (`ledger_entries.cpp:520-532`, one query on `companyId` and `toView(mapper, loadWritten(…))` per entry), then `apply<JournalEntryView>`; the read is one helper both actions share. `execute(ListEntriesPage)` validates the query against the journal columns first (an invalid query is refused for every caller, as before), then keeps its visibility check and empty page.
- Doc comments on both say what they now do: "reads those rows (the sites' samples, the company's entries) and runs the query over them with `table::apply`; the total is the count up to `QueryLimits::totalCap` and omitted above it". A refusal names the action the caller sent (`samplesWhere` passes `CountSamplesToVerify` or `SearchSamples`), and `pageSamples` validates before it reads.
- `examples/lims/tests/test_ledger_scale.cpp`'s 10^6-entry `ListEntriesPage` case asserts no total (it is above `totalCap`), not an exact one.
- `plan_sql.hpp`'s includes leave `sample_model.cpp` and `ledger_entries.cpp`.

- [ ] **Step 4: Prove scope is still pinned**

Mutation: make `pageSamples` ignore `sites` (read every sample) and `execute(ListEntriesPage)` skip its visibility check, one at a time.
Expected: `"lab: the sample list answers through the action with an exact total"` fails on the first (`page.total` is all samples), `"ledger: a page of entries is the caller's, with an exact total"` on the second (dave gets rows). If either passes, add `"ledger entries: a principal never receives a row outside its scope"` asserting the failing condition, and run the mutation again.

- [ ] **Step 5: Update the documentation**

- `engine.md:300-306` becomes: "A model whose rows live in a database reads them as a `DataSource`, whole or lazily by column, and the server sorts, filters, pages and counts them with `apply`; `Page::totalKind` says whether a total is the count or a lower bound (`AtLeast`). `compileFilterGuarded` is the one compile step `apply` and `Engine` share, so a collator or date parser that throws is `ServiceFailed` in both." The `escapeLikePattern` bullet (`:311-312`) goes.
- `docs/spec/README.md:138-139`: the `database_pages.md` entry goes.
- `examples/lims/README.md:1795-1819`: replace the database-page section with one paragraph (the trial-balance paragraph after it stays and keeps its 10^7-line measurement): the sample and ledger lists read the caller's rows with one query and answer with `table::apply`, so their order is the engine's on every surface. The two findings that describe `plan_sql.hpp` and the conformance corpus (`:1830-1847`) go.

- [ ] **Step 6: Verify nothing names the removed code**

Run: `git grep -n -E "planQuery|QueryPlan|PlanUse|plan_conformance|standardCorpus|conformanceFailures|database_pages|escapeLikePattern|plan_sql|canTranslate" -- include src tests examples docs/spec ':!docs/spec/table/table.md'`
Expected: no output. (`table.md` names the removals in its API, Neighbouring-specs and Delivery-order rows, which stay.)

- [ ] **Step 7: Run the suites**

Run: `cmake --build --preset cl-debug --target morph_table_tests && ctest --test-dir build/cl-debug -R "^table" --output-on-failure`
Run: `cmake --preset cl-qt-debug -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=lims && cmake --build build/cl-qt-debug --target ladder_lims_tests -j 6 && ctest --test-dir build/cl-qt-debug -L ladder-lims --output-on-failure`
(The ladder needs `MORPH_BUILD_QT=ON`, so `cl-debug` refuses it at configure. Where Qt is not on the preset's path, add `-DCMAKE_PREFIX_PATH=<Qt>/lib/cmake`; where the SQLite ODBC driver is registered under another name than `SQLite3`, set `ODBC_CONNECTION_STRING` to name it.)
Run the docs build of the roadmap's Global Constraints.
Expected: all green; the docs build prints no warning.

- [ ] **Step 8: Commit**

```bash
git add -A include/morph/table tests/table tests/CMakeLists.txt CMakeLists.txt examples/lims docs/spec
git commit -m "refactor(table): remove paging in a database"
```

---

### Task 1: The description and the generic page (table.md step 1, first half)

**Files:**
- Create: `include/morph/table/description.hpp` (add to the `FILE_SET` beside `query.hpp`, `CMakeLists.txt:723`), `tests/table/test_table_description.cpp` (in `morph_table_tests`)
- Modify: `include/morph/table/query.hpp:67-78` (`TableQuery::columns`), `:102-121` (`QueryLimits::maxColumns`), `:228-250` (`validate` checks `columns` and normalises it; `detail::orderedColumns` beside `detail::checkColumn`), `tests/table/wide_table_model.{hpp,cpp}` (`bench::wideDescription`), `tests/table/bench_table.cpp` (W1, W2, W5, W6, W7)

**Interfaces:**
- Consumes: `PageRows` (`query.hpp:282`), `TotalKind`, `DataSource`, `ColumnInfo`, `Cell`, `RowId`, `TableError` (existing); `morph::model::detail::fnv1a64`, `hex64` (`core/payload_schema.hpp`); `bench::WideTableSource` (Task 0).
- Produces, in `namespace morph::table`:

```cpp
// query.hpp
struct TableQuery { std::vector<SortKey> sort; FilterSpec filters; PageRequest page;
                    std::optional<std::vector<std::string>> columns; /* absent: every column */ };
struct QueryLimits { /* … */ std::size_t maxColumns = 512; };
namespace detail {
/// `asked` in the order of `columns`, each id once; an id `columns` lacks is dropped (validate has refused it first).
[[nodiscard]] std::vector<std::string> orderedColumns(std::span<ColumnInfo const> columns,
                                                      std::vector<std::string> const& asked);
}

// tests/table/wide_table_model.hpp, namespace bench
/// The description of `wideColumns(columns)`: `id` as given, `label` = `id`, each column's label its id, `group` and
/// `groups` (`g3`, `g16`, labels their ids) from the layout, key `"id"`, `levels` empty, `revision` empty.
[[nodiscard]] morph::table::TableDescription wideDescription(std::string id, int columns);

// description.hpp
struct TableColumn { ColumnInfo info; std::string label; std::string group;
                     bool sortable = true; bool filterable = true; bool shown = true;
                     bool operator==(TableColumn const&) const = default; };
struct ColumnGroupInfo { std::string id; std::string label; std::vector<std::string> columns;
                         bool operator==(ColumnGroupInfo const&) const = default; };
struct TableCapabilities { bool keys = true; bool distinct = true; std::uint32_t maxSortKeys = 8;
                           bool operator==(TableCapabilities const&) const = default; };
struct TableDescription { std::string id; std::string label; std::vector<TableColumn> columns; std::string key;
                          std::vector<ColumnGroupInfo> groups; std::vector<std::string> levels;
                          TableCapabilities capabilities; std::string revision;
                          bool operator==(TableDescription const&) const = default; };
[[nodiscard]] TableDescription describe(TableDescription description);
[[nodiscard]] std::vector<ColumnInfo> columnInfos(TableDescription const& description);

using GenericRow = std::vector<Cell>;
struct GenericPage { std::string revision; std::vector<std::string> columns; std::vector<RowId> keys;
                     std::vector<GenericRow> rows; std::optional<std::int64_t> total;
                     TotalKind totalKind = TotalKind::Exact; std::int64_t offset = 0; };
[[nodiscard]] GenericPage writeGenericPage(PageRows const& page, DataSource const& source,
                                           std::span<std::string const> columns);
[[nodiscard]] std::expected<GenericPage, TableError> readGenericPage(std::string_view json,
                                                                     TableDescription const& description);
namespace detail {
void appendCellJson(std::string& out, Cell const& cell);
[[nodiscard]] Cell cellFromJson(std::string_view json, std::optional<ColumnKind> kind);
}
```

Members later tasks insert, at the place table.md's block puts them: `TableColumn::{display, tone, tooltip}` (Task 15), `TableColumn::edit` (Task 24), `TableDescription::bands` after `groups` (Task 22), `units` after `levels` (Task 8c), `rowTone`, `rowLock` after `units` (Task 15); the generic page's sidecar members (Task 4). `TableQuery::keys` is Task 4's.

**Pinned rules.**
- `describe` returns the description with `revision = hex64(fnv1a64(glz::write_json(d with revision cleared)))`: 16 lowercase hex digits. The canonical JSON is glaze's default write of the struct, members in declaration order.
- The page's JSON members, in this order: `revision`, `columns`, `keys`, `rows`, `total` (absent when not counted), `totalKind` (`"exact"`/`"atLeast"`), `offset`. `glz::meta<GenericPage>` maps `rows` through a `glz::custom` pair that writes each cell with `appendCellJson` and reads each with `cellFromJson(json, std::nullopt)`; `readGenericPage` re-reads the cells with their column's kind.
- A cell is written by its alternative (table.md, "Rows on the wire"): `null`; an integer; a finite `double` as glaze writes it, with `.0` appended when that text has no `.`, `e` or `E` (so `2.0` is `2.0` and `-0.0` is `-0.0`, never the integer token `2`), a non-finite one as the string `"nan"`, `"inf"` or `"-inf"`; a string; `true`/`false`; a `Rational` with `p = decimalPlaces` as `[numerator × (10^p / denominator), 10^p]` when `10^p` is a multiple of the denominator and the product fits `int64`, otherwise `[numerator, denominator]`; a `QuantityCell` as `[n, d, [fn, fd]]` with amount and factor written by the `Rational` rule, and as the plain `Rational` `[n, d]` when its factor is 1.
- `cellFromJson` reads by shape: `null` → empty; a number whose token has no `.`, `e` or `E` → `int64` (a `double` when `kind` is `Number`, or when it does not fit `int64`); any other number → `double`, whatever the kind, so a `double` the server held in an `Integer` or `Decimal` column, which it keyed invalid (`sort.hpp`'s `readInteger` and `readExact` take no `double`), is a `double` and invalid on the client too; a string → `std::string` (in a `Number` column, `"nan"`, `"inf"`, `"-inf"` → that `double`); a boolean → `bool`; `[n, d]` with `d > 0` → `Rational{Numerator{n}, Denominator{d}, DecimalPlaces{k}}` where `k` is such that `d == 10^k` (`k ≤ 18`), otherwise `kMaxDecimalPlaces`; `[n, d, [fn, fd]]` → `QuantityCell`; anything else (an object, `[n, 0]`, a negative denominator, a third element of another shape, another length) → the `std::string` of its JSON text, which no kind but `Text` reads as valid. Read this way, the client's cell keys exactly as the server's did, so a cell the server keyed as invalid is invalid on the client.
- `readGenericPage` refuses with `TableError{.code = InvalidValue}` (naming the column where there is one) when the JSON is not a page, a `columns` entry is not in the description (`UnknownColumn`), a row's length differs from `columns`, or `keys` and `rows` differ in length.
- `writeGenericPage` writes `columns` as given (the caller orders and deduplicates them), `keys[i] = snapshot->rowId(page.rows[i])`, the cells of those columns only, `total` and `offset` from `page`; `revision` is left for the caller.
- `validate` checks `query.columns` when present, and normalises it (table.md, "Data levels": "`validate` bounds `columns` by `QueryLimits::maxColumns` (512), puts it in the description's order and removes duplicates"): more than `maxColumns` entries, counted as received, is `LimitExceeded`; each entry through `detail::checkColumn` (`UnknownColumn`, `ColumnNotReadable`); the query it returns holds `detail::orderedColumns(columns, *query.columns)`. An absent list stays absent and an empty one stays empty. Every caller of `validate` or `apply` therefore sees the list in the order of the columns it validated against, each id once.

- [ ] **Step 1: Write the failing tests** in `tests/table/test_table_description.cpp`

```cpp
TEST_CASE("description: equal descriptions have equal revisions", "[table][description]") {
    TableDescription base{.id = "designs", .label = "Designs",
                          .columns = {TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
                                      TableColumn{.info = {.id = "name", .kind = ColumnKind::Text}, .label = "Name"}},
                          .key = "id"};
    auto const a = describe(base); auto const b = describe(base);
    CHECK(a.revision.size() == 16);
    CHECK(a.revision == b.revision);
    CHECK(describe(a).revision == a.revision);                     // Mutation: hashing with the old revision in
    auto relabelled = base; relabelled.columns[1].label = "Title";
    CHECK(describe(relabelled).revision != a.revision);            // Mutation: hashing the columns' ids only
    auto hidden = base; hidden.columns[1].shown = false;
    CHECK(describe(hidden).revision != a.revision);
}

TEST_CASE("generic page: every cell kind round-trips", "[table][description]") {
    // Description: id Integer, name Text, net Decimal, share Decimal, dose Quantity, ratio Number, flag Bool, on Date.
    // Rows (keys 101, 102):
    //   101: 101, "Alpha", 12.50 (places 2), 0.003 (places 3), QuantityCell{5 (places 0), 1/1000 (places 3)}, 0.1, true, 19'000
    //   102: 102, "Beta", empty, empty, Rational 250 (places 0), 2.5, false, empty
    // writeGenericPage over a VectorSource of these rows, PageRows{.rows = {0, 1}, .total = 9093, .offset = 0},
    // columns {"id","name","net","share","dose"}, revision set to "9f2c"; glz::write_json gives exactly:
    //   {"revision":"9f2c","columns":["id","name","net","share","dose"],"keys":[101,102],
    //    "rows":[[101,"Alpha",[1250,100],[3,1000],[5,1,[1,1000]]],[102,"Beta",null,null,[250,1]]],
    //    "total":9093,"totalKind":"exact","offset":0}
    // readGenericPage(that JSON, description) gives back: keys, columns, total, offset equal; tabletest::sameCell(read,
    // written) for every cell (so a Rational's decimalPlaces and a QuantityCell's amount and toCanonical are compared).
    // The same page with all eight columns: `ratio` reads as double 0.1 and 2.5, `flag` as bool, `on` as int64 19'000.
    // Mutation: writing a Rational as glaze's {"num","den","dp"} object.
    // Mutation: reading [1250, 100] with DecimalPlaces{kMaxDecimalPlaces} (operator== still holds; sameCell fails).
}

TEST_CASE("generic page: a cell that does not read as its column's kind is invalid", "[table][description]") {
    // readGenericPage of a one-column Integer page whose rows are ["abc"], [2.5], [[1,0]], [{"x":1}], [2.0], [7]:
    // buildKeys over a VectorSource of the read rows gives states {Invalid, Invalid, Invalid, Invalid, Invalid, Valid}.
    // The same values in a Text column: the last five keys are Valid (a Text column has no invalid cell).
    // Server and client agree: a VectorSource of one Integer column holding Cell{2.0} keys Invalid; its page through
    // writeGenericPage and glz::write_json holds the token 2.0; read back, the cell holds a double and keys Invalid.
    // The same for a Decimal column. In a Number column 2.0 writes 2.0 and reads as double 2.0, Valid.
    // Mutation: cellFromJson parsing "abc"-like strings for the column's kind on read (2.5 read as Integer 2).
    // Mutation: writing a double as glaze does (2.0 becomes 2, an int64 the client keys Valid).
}

TEST_CASE("generic page: numbers at the edges cross the wire unchanged", "[table][description]") {   // Review Focus 2
    // Number column: NaN, +inf, -inf, -0.0, 1e308 → written as "nan", "inf", "-inf", -0.0, glaze's 1E308 → read back equal
    //   (NaN by std::isnan). Integer column: INT64_MIN, INT64_MAX. Decimal column: Rational{INT64_MAX, 3, places 2}
    //   (10^2 is not a multiple of 3, so written [INT64_MAX, 3]) and Rational{INT64_MAX / 10, 1, places 18}
    //   (the scaled numerator overflows, so written [n, 1]) → read back equal by value.
    // Mutation: writing a non-finite double with glaze's default (the page fails to write, or reads back empty).
}

TEST_CASE("generic page: a malformed reply is refused and a malformed cell is invalid", "[table][description]") {   // Review Focus 3
    // Refused, code and column:
    //   a row of 2 cells under 3 columns → InvalidValue; keys [1,2] with one row → InvalidValue;
    //   "columns":["nope"] → UnknownColumn, column "nope"; "not json" → InvalidValue.
    // Kept, one cell invalid: an Integer cell [1,0] in an otherwise valid page reads, and that cell keys Invalid.
    // Mutation: readGenericPage padding a short row with empty cells.
}

TEST_CASE("table query: columns round-trip and are bounded", "[table][query]") {
    // glz::write_json(TableQuery{.columns = std::vector<std::string>{"a","b"}}) contains "columns":["a","b"] and reads back
    // equal; R"({"sort":[],"filters":{},"page":{"offset":0,"limit":10}})" reads with columns == std::nullopt.
    // validate against columns {id, a, b} with mayRead refusing "b":
    //   columns {"zz"} → UnknownColumn "zz"; {"b"} → ColumnNotReadable "b"; 513 entries → LimitExceeded (513 copies of
    //   "a" too: the bound counts entries as received); {} → accepted, and the returned query's columns are present and empty.
    // Mutation: validate ignoring `columns`.
}

TEST_CASE("table query: a columns list is answered in the description's order, once each", "[table][query]") {   // Review Focus 1
    // infos = columnInfos(bench::wideDescription("small", 30)):
    //   validate(TableQuery{.columns = std::vector<std::string>{"t000","id","t000","i001"}}, infos)->columns
    //     == std::vector<std::string>{"id","i001","t000"};
    //   validate(TableQuery{}, infos)->columns == std::nullopt (absent stays absent: every column);
    //   validate with columns {"t000","zz"} → UnknownColumn "zz" (an unknown id is refused, not dropped);
    //   apply(WideTableSource{1'000, 30, 7}, the first query) succeeds: apply validates through the same step.
    //   detail::orderedColumns(infos, {"i001","nope","id"}) == {"id","i001"}.
    // writeGenericPage of rows 0..199 of WideTableSource{1'000, 30, 7} with an empty column list gives 200 keys and
    //   200 rows of zero cells.
    // Mutation: validate returning `columns` as received (the query's order, the duplicate kept).
}

TEST_CASE("generic page: a page names only the asked columns", "[table][description]") {   // gate W1
    // WideTableSource{1'000, 300, 7}; PageRows of rows 0..199; 40 columns: positions 100..139.
    // The JSON's "columns" has 40 entries; every row array has 40 cells; no other column id appears in "columns".
    // Mutation: writeGenericPage ignoring `columns` (writes all 300).
}

TEST_CASE("generic page: a whole page fits an envelope", "[table][description]") {   // gate W7
    // The same 200 rows with all 300 columns: every row holds 300 cells (60,000 in all), and
    // glz::write_json(page)->size() + 4'096 < morph::wire::kMaxEnvelopeBytes; the size is also above 600'000, so the
    // page measured is the wide one (ten bytes a cell is a floor no 300-column page is under).
    // Mutation: writeGenericPage writing the first 40 columns whatever it is given (the row length and the floor fail).
}

TEST_CASE("description: a 300-column description round-trips", "[table][description]") {   // W6's codec
    // d = bench::wideDescription("wide", 300) with d.levels = {"recipe"} set by the test (the fixture serves none in
    // this part); through describe, glz::write_json and glz::read_json: equal to the original, with 300 columns,
    // groups g3 (3 columns) and g16 (16), and the 16-digit revision.
    // Mutation: a glz::meta for TableDescription that leaves out `groups` (the read description has none).
}
```

`detail::orderedColumns` is declared in `query.hpp` beside `detail::checkColumn`; `bench::wideDescription` in the fixture header.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile failure, `morph/table/description.hpp` not found.

- [ ] **Step 3: Implement** `TableQuery::columns`, `QueryLimits::maxColumns`, `detail::orderedColumns`, `validate`'s check and normalisation in `query.hpp`, `bench::wideDescription` in the fixture, and every symbol listed in Produces in `description.hpp`, with the pinned rules above. Every public symbol gets complete Doxygen.

- [ ] **Step 4: Run them to see them pass**

Run: `ctest --test-dir build/cl-debug -R "description|generic page|table query" --output-on-failure`
Expected: the 10 tests of `test_table_description.cpp` pass (the pattern also matches `morph_tests` cases whose names contain "description", when that executable is built); then `ctest --test-dir build/cl-debug -R "^table" --output-on-failure`, all pass.

- [ ] **Step 5: Write the wire benchmarks** in `bench_table.cpp`, `"table benchmark: the generic page"`, `"[.benchmark][wire]"`: W1 (bytes of the 200 × 40 page in an `ok` envelope), W2 (encode from the column store, and `readGenericPage`), W5 (the same 200 × 30 page as generic rows and as a typed `Page<WideRow30>`, where `WideRow30` is a 30-member struct in the bench file mirroring `wideColumns(30)`), W6 (bytes, encode and decode of the 300-column description), W7 (the 200 × 300 page: bytes, encode, decode). Each first `REQUIRE`s its page's cell count. Run on `cl-release` and add rows W1, W2, W5, W6, W7 to "Recorded baselines".

- [ ] **Step 6: Mutations, docs build, commit**

Apply each mutation named in Step 1; paste the failing lines into the commit body. Run the docs build (Global Constraints).

```bash
git add include/morph/table/description.hpp include/morph/table/query.hpp CMakeLists.txt \
        tests/table/wide_table_model.hpp tests/table/wide_table_model.cpp \
        tests/table/test_table_description.cpp tests/table/bench_table.cpp tests/CMakeLists.txt docs/spec/table/table.md
git commit -m "feat(table): table descriptions and the generic page"
```

---

### Task 2: Tables of a model on the bridge (table.md step 1, second half)

**Files:**
- Modify: `include/morph/table/description.hpp` (the bridge-facing types below)
- Modify: `include/morph/core/detail/result_walker.hpp:139-165` (a holder reports its record at a member), `include/morph/core/records.hpp:43-58` (`RecordInfo::table`) and a `registerTableRecordOnce`, `include/morph/core/effects.hpp:113-127` (`ActionRecord::opaque`), `:410-418` (`sameRegistration` compares `table`), `:520-527` (a table record may be keyed and derived), `:543-560` (an opaque slot), `:73-89` (`updatesTable`, `changesTable`)
- Modify: `include/morph/core/registry.hpp:841,967,1030,1039` and `include/morph/core/bridge.hpp:612,625,2245,2434` (call `invokeExecute`), and `BRIDGE_REGISTER_TABLE` after `BRIDGE_REGISTER_ACTION_SOURCE` (`registry.hpp:2206`)
- Create: `include/morph/table/runtime.hpp` (`TableRuntime`, `currentTableRuntime`, `TableRuntimeScope`; it includes only `core/executor.hpp`, so `core/remote.hpp` includes it without reaching the table headers; add it to the `FILE_SET` beside `query.hpp`); define the thread-local behind `currentTableRuntime()` in `src/ui/process_state.cpp` beside `tlsCurrent()` (`:144`), as a `MORPH_API` function so a shared build has one copy
- Modify: `include/morph/core/remote.hpp:310-367` (`ServerConfig::tableWork`), `:2861` and `:2875-2885` (`startRemote` and `startTaskRemote` install a `table::TableRuntimeScope` beside `ScopedContext`, with `work` = `tableWork` or, when null, the server's worker pool)
- Modify: `include/morph/forms/forms.hpp` (`forEachNamedMember` walks a type glaze writes as one of its members, its `glz::meta` value a member pointer, through that member: `QueryTable`'s JSON is its `TableQuery`'s, and the action's schema extras are built by that walk)
- Modify: `tests/table/wide_table_model.{hpp,cpp}` (`bench::WideTableModel`), `tests/table/test_table_description.cpp`, `tests/table/bench_table.cpp` (Step 6)
- Create: `tests/net/test_push_tables.cpp` (in `morph_net_tests`, `tests/net/CMakeLists.txt:1-12`)
- Docs: `docs/spec/core/registry.md` (a section `### BRIDGE_REGISTER_TABLE(M, ID, FN, ...)` after `### BRIDGE_REGISTER_ACTION(M, A, NAME, ...)`, `:999`, and a paragraph on `invoke` under `### ActionTraits<Action>`, `:217`), `docs/spec/core/backend.md` (`tableWork` in the `ServerConfig` table, `:2491`, and one bullet in "Fan-out", `:1221`, saying a table action's reply is built on `tableWork` and its fan-out is the record machinery's)

**Interfaces:**
- Consumes: Task 1's `TableDescription`, `describe`, `GenericPage`, `writeGenericPage`, `columnInfos`, `bench::wideDescription`; `validate` (which orders and deduplicates `columns`), `apply`, `RowsSource<Row>`, `detail::reflectedColumns<Row>`; `effects::updates<R>`, `changes<R>`; `session::current()`; `forms::detail::findFieldMeta<Row>`, `forms::detail::inferTitle`; `morph::async::DelayAwaiter` as the model for an awaiter that resumes on the strand.
- Produces, `namespace morph::table`:

```cpp
class LevelSource;                                  // defined by Task 5 as a class
struct TableContext { session::Context const& session; };
struct ServedTable {
    TableDescription description;
    std::shared_ptr<DataSource const> source;
    std::vector<std::shared_ptr<LevelSource>> levels;   // empty until Task 5 gives it meaning
    std::function<bool(std::string_view column)> mayRead;
    std::string scope;
    Services services;
    QueryLimits limits;
};
struct ChangedRow { RowId key; };                    // Task 17c adds `from`
template <auto Function> struct ChangedRows {        // Task 17c adds `failed`
    std::vector<ChangedRow> rows;
    using heldRecord = TableOf<Function>;
    static constexpr std::string_view heldMember() noexcept { return "rows"; }
};
template <auto Function> struct TableTraits { static constexpr bool registered = false; };   // specialised by the macro
template <auto Function> struct TableOf { static_assert(TableTraits<Function>::registered, "not a registered table"); };
template <typename Row, typename Fn> struct Typed { Fn function; };
template <typename Row, typename Fn> [[nodiscard]] constexpr Typed<Row, Fn> typed(Fn function) noexcept;
struct rowScoped {};
template <typename... Records> struct derivedFrom {};
[[nodiscard]] std::shared_ptr<DataSource const> scoped(std::shared_ptr<DataSource const> source, std::vector<RowId> keys);
template <auto Function> struct QueryTable { TableQuery query; /* JSON is the TableQuery's: glz::meta value = &QueryTable::query */
    template <typename Model> static ::core::async::Task<GenericPage> invoke(Model& model, QueryTable& action); };
template <auto Function> struct DescribeTable {
    template <typename Model> static TableDescription invoke(Model& model, DescribeTable& action); };
namespace detail {
struct AnswerContext {};                              // Task 6 adds `ViewCache* views`, `std::string_view record`, `bool rowScoped` (index decisions 5, 48)
[[nodiscard]] std::expected<GenericPage, TableError> answerQuery(ServedTable const& served, TableQuery const& query,
                                                                 AnswerContext const& context = {});   // served.source is pinned
[[nodiscard]] ServedTable pinned(ServedTable served);   // on the owner's thread: source → PinnedSource of one snapshot
template <auto Function, typename Model>
[[nodiscard]] ServedTable serve(Model& model, TableContext const& context);   // the one way to a table's ServedTable
}
// include/morph/table/runtime.hpp
struct TableRuntime { exec::IExecutor* work = nullptr; };   // Task 6 adds `ViewCache* views`
[[nodiscard]] MORPH_API TableRuntime const* currentTableRuntime() noexcept;   // null outside a RemoteServer dispatch
class TableRuntimeScope {                             // installs `runtime` for the scope, restores the previous one
public: explicit TableRuntimeScope(TableRuntime const& runtime) noexcept; ~TableRuntimeScope();
        TableRuntimeScope(TableRuntimeScope const&) = delete; TableRuntimeScope& operator=(TableRuntimeScope const&) = delete; };
}
namespace bench { inline constexpr std::string_view kWideModelType = "WideTableModel"; }   // tests/table/wide_table_model.hpp
namespace morph::effects {
template <auto Function> using updatesTable = updates<::morph::table::TableOf<Function>>;
template <auto Function> using changesTable = changes<::morph::table::TableOf<Function>>;
}
namespace morph::model::detail {
template <typename Model, typename Action> decltype(auto) invokeExecute(Model& model, Action& action);
// `Action::invoke(model, action)` when the action declares one, `model.execute(action)` otherwise.
}
// ServerConfig: std::shared_ptr<::morph::exec::IExecutor> tableWork;  // null: the server's worker pool
// effects::ActionRecord: bool opaque = false;  // held at no path; `paths` is empty
// record::RecordInfo:    bool table = false;   // registered by BRIDGE_REGISTER_TABLE
```

`BRIDGE_REGISTER_TABLE(M, ID, FN, ...)`, `ID` a string literal, `FN` a member function pointer or a `table::typed<Row>(…)` expression, options `table::rowScoped` and `table::derivedFrom<…>`. It requires `BRIDGE_REGISTER_MODEL(M, …)` before it and `<morph/table/description.hpp>` in the translation unit, and emits:
1. `template <> struct morph::table::TableTraits<KEY> { using model = M; using row = R; static constexpr bool registered = true; static constexpr std::string_view id = ID; static constexpr auto serve = FN; }`, where `KEY` is the member function pointer: `FN` itself, or `FN.function` for `typed<Row>(&M::fn)`, and `R` is `Row` for a typed table and `void` otherwise (`table::detail::tableKey(FN)` and `table::detail::TableRow<decltype(FN)>` compute them). Every type below is keyed on `KEY`, so `TableOf<&M::fn>`, `ChangedRows<&M::fn>` and `updatesTable<&M::fn>` name a typed table as they name a generic one; `QueryTable<KEY>::invoke` reads `TableTraits<KEY>::serve` to call the function;
2. `ActionTraits` and registrars for `QueryTable<KEY>` under `"Table." ID` (result `GenericPage`) and `DescribeTable<KEY>` under `"Table." ID ".describe"` (result `TableDescription`), as `BRIDGE_DETAIL_ACTION_TRAITS_BODY` does for an action with an explicit result, both with `::morph::model::Loggable::No`: they read, and a page fetch is not written to an action log (the default of `BRIDGE_REGISTER_ACTION` is `Loggable::Yes`, `registry.hpp:2122`; the tree's read actions pass `No`, `ui/preferences.hpp:316`);
3. `registerTableRecordOnce<TableOf<KEY>, Options…>(ModelTraits<M>::typeId() + "." ID)`: a `RecordInfo` with `keyField = "key"`, `table = true`, `privateKeys` from `rowScoped`, `derivedFrom` from `derivedFrom<…>`;
4. one more `ResultSlot{typeid(TableOf<KEY>), "", .opaque = true}` on `"Table." ID`'s result slots (`ResultSlot` gains `bool opaque = false`), which `resolve` turns into `ActionRecord{.record, .keyField, .paths = {}, .opaque = true}` so the table's action holds its record for watch admission (`remote.hpp:1794`) and names no path.

**Pinned behaviour.**
- **A table id is unique in the process, not only in its model.** The action id is `"Table." ID` with no model in it, and the effects registries are keyed by action id: a second action type under an id is listed in `actionResultConflicts()` (`registerActionResultOnce`, `registry.hpp:1626-1648`) and `effects::resolve` refuses it (`effects.hpp:522`), which fails every effects lookup of the process. Two models that each register a table `"rows"` are therefore refused when the registries resolve, with "action `Table.rows` is registered by two action types; give each its own id". table.md states the same rule ("A table id is unique in the process", under "A model's tables"), so the spec is not edited here; check with `grep -n "unique in the process" docs/spec/table/table.md`. The macro's Doxygen and `registry.md` state the process-wide rule, and a test below pins the refusal.
- The walk (`result_walker::visit`): a type `T` with `T::heldRecord` and `T::heldMember()` adds `ResultSlot{typeid(typename T::heldRecord), path.empty() ? heldMember() : path + "." + heldMember()}` beside its own slot.
- `resolve` does not report "derived from other records but has a key" for a record with `table == true`: a source's change reaches it as a whole-record change, as `addDerivedChanges` already sends.
- `QueryTable<F>::invoke`: on the strand, build `TableContext` from `session::current()` (a static empty `Context` when null) and call `detail::serve<KEY>(model, context)`; read `currentTableRuntime()` before the first suspension and build `AnswerContext{}` from it; then, still on the strand, replace `served.source` with a `detail::PinnedSource` (the one `snapshot()` taken now and a copy of `columns()`), because a `DataSource` is owner-affine and only its snapshot may cross threads (`data_source.hpp:728`; table.md "the framework takes the source's snapshot, and then on the server's table executor…"); then run `answerQuery(pinnedServed, action.query, context)` as one work item posted to `runtime->work` and resume with its result through an awaiter built like `DelayAwaiter`, so the coroutine's own steps stay on the strand and the sort does not. With no runtime installed or a null `work` (`LocalBackend`), `answerQuery` runs inline. A `TableError` becomes `model::ActionRefusal{"table." + <the code's JSON name>, {{"column", error.column}}, error.message}`.
- `answerQuery`: `validate(query, columnInfos(served.description), served.limits, served.mayRead)`; the page's columns are that validated query's `columns` (in the description's order, each once, by Task 1's `validate`), kept from this first validation, since `apply` validates again against the source's columns, whose order may differ; `served.source` is a `detail::PinnedSource` (a `DataSource` holding one snapshot and a copy of the columns, whose `snapshot()` returns that snapshot and whose `subscribe` does nothing); `answerQuery` never takes a snapshot of a live source, and both `apply` (with `ApplyOptions{limits, services, mayRead}`) and `writeGenericPage` read the pinned one; a caller that passes a live source (the tests) gets it pinned by `detail::pinned(ServedTable)`, called on the caller's thread; with `query.columns` absent, the page's columns are the description's minus those `served.mayRead` refuses (invariant 14); `page.revision = served.description.revision`. A description column the source lacks is `TableError{UnknownColumn, column, "the table's source has no column <id>"}`.
- `DescribeTable<F>::invoke` returns `describe(served.description)` (so a model never fills `revision`); it runs on the strand and touches neither the source nor a snapshot. The function result is described once per call; a model whose description does not vary returns the same object each time.
- `detail::serve<Function>(model, context)` is the only path from a model to its `ServedTable`; `QueryTable::invoke` and `DescribeTable::invoke` call it on the strand, and Task 17a's `resolve` will too. For a plain table it calls `(model.*Function)(context)`; for a typed table (`TableTraits<Function>::row` not `void`) it calls the typed function and builds the description and the `RowsSource` as the next bullet says.
- `typed<Row>(fn)`: `fn` is `std::shared_ptr<std::vector<Row> const> (Model::*)(TableContext const&)`. The served description's columns are `detail::reflectedColumns<Row>()` in member order, labels `findFieldMeta<Row>(id)->label` or `inferTitle(id)`, key the `keyField` of the record registered for `Row`; the source is `RowsSource<Row>` keyed by that member.
- `scoped(source, keys)`: a `DataSource` whose snapshot maps its rows, in the shared source's order, onto the shared source's rows whose key is in `keys` (keys the source lacks are skipped); it copies no cell; when the shared source's snapshot pointer changes, the next `snapshot()` rebuilds the row map under a mutex.
- `bench::WideTableModel` (registered `"WideTableModel"`), process-wide state set by `static void configure(WideTableOptions wide, WideTableOptions small)` (defaults `{100'000, 300, 1}` and `{1'000, 30, 1}`). `configure` may be called at any time, from any test case: it replaces both options under the state's mutex and drops both sources; each source is built lazily on the first table call that needs it after that. A request already holding a source keeps it.
  - `wide(TableContext const&)`, `small(…)`: the description is Task 1's `bench::wideDescription("wide", 300)` and `("small", 30)` (so `levels` is empty in this part; Part B's Task 5 sets it when the slow level lands), key `id`, one shared `WideTableSource` each, `Services{}`.
  - `scoped(…)`: `small`'s source; the description is `small`'s without `d000`…`d005` for every principal except `"extended"` (all 30) and `"restricted"` (also without `t000`…`t006`); `mayRead` is membership in the caller's description.
  - Actions: `EditWideRow{std::int64_t row; std::string column; std::int64_t value;}` sets the cell in `wide`, and in `small` when the row and column are there, and returns `EditWideRowResult{ChangedRows<&WideTableModel::wide> wide; ChangedRows<&WideTableModel::small> small;}` with the changed row's key in each it changed; effects `updatesTable` on `wide` and `small`. `ReplaceWideRows{std::uint64_t seed;}` regenerates both; effects `changesTable` on both.

- [ ] **Step 1: Write the failing local tests** in `tests/table/test_table_description.cpp`

```cpp
TEST_CASE("table registration: a model with no row type serves a table", "[table][registration]") {
    // Spec row "A table from a description", server half. WideTableModel::configure({2'000, 300, 7}, {1'000, 30, 7}).
    // served = WideTableModel{}.small(ctx); answerQuery(served, {sort {i001 asc}, filter t001 contains "a", page {0, 10}}):
    //   10 keys, equal to the keys of apply over a fresh WideTableSource{1'000, 30, 7} with the same query;
    //   columns == the 30 ids in layout order; revision == describe(served.description).revision.
    // Mutation: answerQuery ignoring query.sort.
}

TEST_CASE("table registration: two tables of one model are two records and two actions", "[table][registration]") {
    // effects::recordNamed("WideTableModel.wide")->keyField == "key" and ->table; same for ".small" and ".scoped".
    // effects::recordsOf("Table.wide") == {ActionRecord{.record = "WideTableModel.wide", .keyField = "key", .paths = {}, .opaque = true}}.
    // effects::actionsHolding("WideTableModel.wide") == {"EditWideRow", "Table.wide"}; ("…small") == {"EditWideRow", "Table.small"}.
    // effects::effectsOf("EditWideRow") == {{Updates, "WideTableModel.wide"}, {Updates, "WideTableModel.small"}}.
    // effects::effectsOf("ReplaceWideRows") == {{Changes, "WideTableModel.wide"}, {Changes, "WideTableModel.small"}}.
    // Mutation: one record for the model (the record keyed on M instead of FN): the second registration conflicts.
}

TEST_CASE("changed rows: the walk reports the table at its rows member", "[table][registration]") {
    // walkResult<bench::EditWideRowResult>() contains {typeid(TableOf<&WideTableModel::wide>), "wide.rows"} and
    // {typeid(TableOf<&WideTableModel::small>), "small.rows"}.
    // effects::deriveChanges("EditWideRow", R"({"wide":{"rows":[{"key":5}]},"small":{"rows":[{"key":5}]}})", ranOn)
    //   names record "WideTableModel.wide" with keys [5] and "WideTableModel.small" with keys [5].
    // Mutation: the walker without the heldRecord rule (the registry then refuses EditWideRow's updates).
}

TEST_CASE("changed rows: updatesTable without its ChangedRows is refused when the registries resolve", "[table][registration]") {
    // resolveForTesting(records = {the "WideTableModel.wide" RecordInfo}, slots = {"Bad": walkResult<NoTableResult>()},
    //   declared = {"Bad": {updates<TableOf<&WideTableModel::wide>>}}) throws EffectsRegistrationError whose problems
    //   contain "action `Bad` declares updates on record `WideTableModel.wide`, but its result holds no `WideTableModel.wide`".
    // NoTableResult is a test struct {std::vector<int> ids;}.
    // The same call with slots = {"Bad": walkResult<bench::EditWideRowResult>()} resolves, and its actionRecords for
    // "Bad" name "WideTableModel.wide" at path "wide.rows".
    // Mutation: the walker without the heldRecord rule (the second call throws the same problem).
}

TEST_CASE("table registration: one table id in two models is refused when the registries resolve", "[table][registration]") {
    // Test models UqA and UqB each have a function `rows`. Neither is registered with the macro: a real second
    // "Table.rows" would make the process registry of this executable invalid for every other case.
    // static_assert(!std::is_same_v<QueryTable<&UqA::rows>, QueryTable<&UqB::rows>>): the two tables' query actions
    // are two action types, which under the one id "Table.rows" is what registerActionResultOnce lists as a conflict.
    // resolveForTesting(records = {}, slots = {"Table.rows": ActionResult{typeid(QueryTable<&UqA::rows>), {}}},
    //   declared = {}, slotConflicts = {"Table.rows"}) throws EffectsRegistrationError whose problems contain
    //   "action `Table.rows` is registered by two action types; give each its own id".
    // The process registry holds no conflict: model::detail::actionResultConflicts() is empty, with WideTableModel's
    // three tables and this file's test models registered (their ids are all different).
    // Mutation: QueryTable keyed on the table's id text instead of its function (the static_assert fails: two models'
    // tables would share one action type and a second table "rows" would silently answer with the first's rows).
}

TEST_CASE("table registration: rowScoped and derivedFrom are the record's", "[table][registration]") {
    // Test model RtModel registers table "mine" with table::rowScoped and "theirs" with table::derivedFrom<RtSource>
    // (RtSource a registered record; RtTouch an action declaring changes<RtSource>).
    // recordNamed("RtModel.mine")->privateKeys; recordNamed("RtModel.theirs")->derivedFrom == {typeid(RtSource)} and
    // keyField == "key"; deriveChanges("RtTouch", "{}", ranOn) names "RtModel.theirs" with no keys.
    // Mutation: keeping "a derived record is keyless" for tables (the process registry fails to resolve).
}

TEST_CASE("columns by session: two principals get two descriptions", "[table][registration]") {
    // Spec row "Columns by session", server half. describe(scoped(ctx "alice")) has 24 columns, "extended" 30,
    // "restricted" 17; their three revisions differ. answerQuery for "restricted" with columns absent names its 17;
    // with columns {"d000"} it is UnknownColumn "d000"; for "alice" a page's revision is alice's description's.
    // A ServedTable whose description lists i001 but whose mayRead refuses it: with columns absent, the page's columns
    // lack i001 and no row holds its cell. Mutation: answerQuery not filtering the absent-columns list by mayRead.
    // Mutation: answerQuery validating against the source's columns instead of the description's.
}

TEST_CASE("typed: a typed table writes the page the generic source writes", "[table][registration]") {
    // Spec row "The generic page", typed half. TypedRow{std::int64_t id; std::string name; Rational price;}, registered
    // as a record keyed `id`; TypedModel::rows returns 3 rows; table "typedRows" registered with typed<TypedRow>(&TypedModel::rows).
    // The served description: columns id (Integer), name (Text), price (Decimal), labels "Id", "Name", "Price", key "id".
    // glz::write_json of answerQuery(typed served, q) == of answerQuery(a ServedTable over a VectorSource of the same
    //   rows and the same description, q), for q = {sort {price desc}, page {0, 2}}.
    // detail::serve<&TypedModel::rows>(model, ctx).description, through describe(), equals what DescribeTable<&TypedModel::rows>
    //   returns through the bridge. Mutation: DescribeTable building the typed description its own way and dropping the labels.
    // Mutation: the typed layer reading cells in glaze's key order while the description lists reflected order differently.
}

TEST_CASE("scoped: a scope's source reads the shared rows of its keys", "[table][registration]") {
    // base VectorSource keys 1..5; scoped(base, {4, 2, 9}): rowCount 2, rowId(0) == 2, rowId(1) == 4, cell(1, c) == base's
    // row of key 4; after base->updateRow(index of 4, new cells), scoped->snapshot()->cell(1, c) is the new value.
    // Mutation: scoped copying the rows when built (the update is not seen).
}

TEST_CASE("table action: a failing table function is an error reply and the model stays usable", "[table][registration]") {   // Review Focus 5
    // FlakyModel's table "flaky" throws std::runtime_error on its first call and on the second returns a description
    // naming column "ghost" its source lacks; the third call is well formed.
    // Through Bridge + LocalBackend (as tests/test_coroutine_model.cpp:513 drives a Task handler): the first
    // QueryTable<&FlakyModel::flaky> fails with the thrown message; the second fails with ActionRefusal code
    // "table.unknownColumn" and arg column "ghost"; the third succeeds with its rows.
    // Mutation: answerQuery writing the page column by column without checking the source first (a partial page).
}
```

- [ ] **Step 2: Write the failing socket tests** in `tests/net/test_push_tables.cpp`, built on the server-and-two-clients harness of `tests/net/test_push_two_clients.cpp`, with a test model `PtModel` serving tables `first` and `second` over small `VectorSource`s, actions `EditFirst` (→ `ChangedRows<&PtModel::first>`, `updatesTable` first), `EditSecond` (→ `ChangedRows<&PtModel::second>`, `updatesTable` second) and `EditBoth` (→ `{ChangedRows<&PtModel::first> first; ChangedRows<&PtModel::second> second;}`, `updatesTable` on both), and an authoriser that refuses principal `bob` `Table.second`, `EditSecond` and `EditBoth`. A watch of a record is admitted when the principal may run any action holding it (`remote.hpp:1794-1800`), and `EditSecond` and `EditBoth` hold `PtModel.second`, so refusing `Table.second` alone would still admit bob:

```cpp
TEST_CASE("tables: a principal allowed one table's action is refused the other", "[net][tables]") {
    // bob (refused Table.second, EditSecond and EditBoth, the only actions holding PtModel.second): watch {PtModel.first} admitted,
    // watch {PtModel.second} in refusedRecords; executing Table.second is refused.
    // alice: both admitted.  Mutation: one record for the model (bob is admitted to both or neither).
}
TEST_CASE("tables: updatesTable on one table marks only its watchers", "[net][tables]") {
    // c1 watches PtModel.first, c2 PtModel.second. EditFirst changing key 3: c1 receives a change of PtModel.first
    // with keys [3]; c2 receives nothing within 3 × coalesceWindow. Then EditSecond changing key 4: c2 receives
    // PtModel.second with keys [4]; c1 receives nothing.
    // Mutation: ChangedRows<F>::heldRecord naming the model's first registered table whatever F is (the registries
    // refuse EditSecond's updatesTable, or c1 is told of key 4).
}
TEST_CASE("tables: an action changing two tables sends each table its keys", "[net][tables]") {
    // EditBoth changing first key 1, second key 2: c1 gets PtModel.first [1]; c2 gets PtModel.second [2].
    // Mutation: the walk reporting a holder's record at the holder's own path instead of at its `rows` member (no key
    // is found at "first" or "second", so each watcher gets a change without keys).
}
TEST_CASE("tables: a page's columns are the caller's", "[net][tables]") {
    // Spec row "Columns by session" over the socket: WideTableModel's Table.scoped.describe for alice and restricted
    // differ; each one's Table.scoped page lists its own columns and carries its own description's revision.
    // Mutation: QueryTable::invoke building TableContext from an empty session instead of session::current() (both
    // principals get the same 24 columns).
}
TEST_CASE("tables: a table's reply is built on tableWork", "[net][tables]") {
    // ServerConfig::tableWork = a ThreadPoolExecutor{2} wrapped to record its threads' ids. PtModel.first's source is a
    // recording source whose snapshot records the thread of every readColumn call. A Table.first query with a sort:
    // every recorded read is on a tableWork thread, none on the thread that ran PtModel::first.
    // Mutation: RemoteServer not installing the TableRuntimeScope (answerQuery runs inline on the strand).
}
```

- [ ] **Step 3: Run them to see them fail**

Run: `cmake --build --preset cl-debug --target morph_table_tests`
Expected: compile failure, `ServedTable` undeclared.
Socket tests (POSIX only; on Windows in WSL or the Linux CI leg): `cmake --preset linux-everything && cmake --build --preset linux-everything --target morph_net_tests`
Expected: compile failure, `BRIDGE_REGISTER_TABLE` undefined.

- [ ] **Step 4: Implement** every symbol of Produces and the pinned behaviour, in the files listed; register `bench::WideTableModel` with its three tables and two actions in `wide_table_model.hpp`, below the class, and define its members in `wide_table_model.cpp`. The registrations go in the header because a translation unit that names `TableOf<&WideTableModel::wide>` (the walk over `EditWideRowResult` in the tests does) must see `TableTraits`' specialisation, or `TableOf`'s `static_assert` fires there. Every public symbol gets complete Doxygen.

- [ ] **Step 5: Run them to see them pass**

Run: `ctest --test-dir build/cl-debug -R "table registration|changed rows|columns by session|typed:|scoped:|table action" --output-on-failure`
Expected: 10 tests pass. Then the suites the dispatcher change touches: `cmake --build --preset cl-debug --target morph_tests morph_table_tests && ctest --test-dir build/cl-debug --output-on-failure -j 8`, all pass.
Run: `ctest --test-dir build/linux-everything -L net -R "tables:" --output-on-failure`
Expected: 5 tests pass; `ctest --test-dir build/linux-everything -L net` all pass.

- [ ] **Step 6: Record W1 and W2 over the bridge**

Extend `"table benchmark: the generic page"` with the `Table.wide` reply of 200 rows × 40 named columns through `LocalBackend` (W1's `ok` envelope bytes, W2's encode on the server side); first `REQUIRE` the reply's 200 keys. Add the rows to "Recorded baselines".

- [ ] **Step 7: Neighbouring specs, mutations, docs build, commit**

Write `registry.md`'s `BRIDGE_REGISTER_TABLE` section (what the macro emits, its options, the record's name and key, `TableOf`, the walk's rule for a holder's `heldRecord`, the opaque holding, that a table id is unique in the process and what happens when it is not, that both table actions are `Loggable::No`, and the admission rule: a principal may watch a table's record when it may run any action holding it, the table's own `Table.<id>` or an action whose result holds its `ChangedRows`, so refusing a table means refusing all of them) and the `invoke` paragraph; `backend.md`'s `tableWork` row and Fan-out bullet. Nothing about `openMembers`, `partialMember` or `announce` (steps 2c, 2d). Apply each mutation; paste the failing lines into the commit body; run the docs build.

```bash
git add include/morph/table/description.hpp include/morph/table/runtime.hpp include/morph/core src/ui/process_state.cpp CMakeLists.txt \
        tests/table tests/net docs/spec/core/registry.md docs/spec/core/backend.md docs/spec/table/table.md
git commit -m "feat(core): register tables of a model"
```

---

## Self-review

**Spec coverage.** "The fixture", "The data", "How figures are taken" → Task 0 (layout, generator, guards, Release medians). Step 0's records S1, S2, S7, S8, B1, B2, B5, U1–U5, W7, F2–F5 and gate S3 → Task 0 Steps 6–7 and the S3 test. "The table tests are built for WebAssembly and run under Node" → Task 0 Step 5. Step 0a and "Paging in the database", "Rows in a database are a source too", the step-0a Neighbouring-specs row (`engine.md`, `database_pages.md`) → Task 0a. "A table is a description" (the four structs, `revision`), "Rows on the wire" (every encoding, `columns`, `keys`, `revision`), `TableQuery::columns`, W1, W5, W6's codec, W7 → Task 1. "A model's tables" (`TableContext`, `ServedTable`, strand then `tableWork`, services, scope, `scoped`, the action table, the table's record, `TableOf`, options, `ChangedRows`, `updatesTable`/`changesTable`, two members for two tables, the opaque holding), "Typed rows are a convenience", "A table on the server" (one `DataSource` per table and scope, `mayRead` narrowing), invariant 14's `mayRead` half and invariant 15, the fixture as a registered model with three tables and no slow level, W2, the step-1 Neighbouring-specs rows (`registry.md`, `backend.md`) → Task 2. Left to other parts by design: `levels` meaning and level-cache sharing ("A table on the server", second bullet) → Task 5; `announce` → Task 6; the client's refetch on a new `revision` → Task 9; `ChangedRow::from`, `FailedRow` → Task 17c; `Table.<id>.resolve` → Task 17a; unloaded and measure cells in the generic page → Tasks 4 and 8c.

**Step scan.** Each test step names its cases and their assertions with the spec's values; the bodies written as comments state exact inputs and expected values. Code steps give signatures and the rules the tests do not determine (the cell codec, the generator, the hop to `tableWork`). The pinned digest is the one value computed during implementation, and the step says how.

**Type consistency.** `GenericPage`, `writeGenericPage`, `readGenericPage`, `TableColumn`, `TableDescription`, `ServedTable`, `ChangedRows<fn>`, `TableOf<fn>` match the roadmap's Interfaces; `TableQuery::columns` and `QueryLimits::maxColumns` land in Task 1, and `ServerConfig::tableWork`, `answerQuery(…, AnswerContext const& = {})` and `TableRuntime`/`currentTableRuntime`/`TableRuntimeScope` are as the index's "Cross-part decisions" 1, 2, 5 and 6 name them.

**Proportion.** About as long as the spec sections it implements; the code blocks are signatures, layouts and test assertions.
