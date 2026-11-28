// SPDX-License-Identifier: Apache-2.0
//
// morph::table's unloaded cell in the engine (docs/spec/table/table.md, "The unloaded cell", "Semantics of an
// unloaded cell" and "Data levels"): a base-level column that holds one is a source contract error, the cell reads
// back as itself, an edit of it is refused on the cell, and a selection of the view leaves undecided rows out.
// Levels: `apply` reads only the columns it sorts and filters, a typed page names its unloaded rows and the order
// levels it awaits, a row's levels name its own columns, a table without levels is whole, and an arrival of a level
// is one repair that runs no filter over the rows it did not decide.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <morph/table/edits.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <morph/table/selection.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"
#include "table/wide_table_model.hpp"

using namespace morph::table;
using tabletest::viewKeys;

namespace {

// Column `c` holding 3, 1 and 2: row 1 holds `Unloaded` when `unloaded`, at level `level`.
std::shared_ptr<VectorSource> threeRows(std::string level, bool unloaded) {
    return tabletest::makeSource(
        {tabletest::levelCol("c", ColumnKind::Integer, std::move(level))},
        {{Cell{std::int64_t{3}}}, {unloaded ? Cell{Unloaded{}} : Cell{std::int64_t{1}}}, {Cell{std::int64_t{2}}}});
}

Selection multiple(bool pruneOnFilter) {
    auto selection = Selection::create(
        {.mode = SelectionMode::Multiple, .pruneOnFilter = pruneOnFilter, .tableMode = TableMode::Client});
    REQUIRE(selection.has_value());
    return std::move(*selection);
}

}  // namespace

// Mutation: key building without the base-level check (setSort succeeds, and the handler is not called).
TEST_CASE("table: a base-level unloaded cell is a source contract error", "[table][unloaded]") {
    SortChain const byC{SortKey{.column = "c"}};

    SECTION("apply returns it") {
        auto const page = apply(*threeRows("", true), TableQuery{.sort = byC});
        REQUIRE_FALSE(page.has_value());
        CHECK(page.error().code == TableErrorCode::SourceContract);
        CHECK(page.error().column == "c");
    }

    SECTION("a synchronous engine returns it from the call that asked") {
        Engine engine{threeRows("", true)};
        auto const before = viewKeys(engine);
        auto const sorted = engine.setSort(byC);
        REQUIRE_FALSE(sorted.has_value());
        CHECK(sorted.error().code == TableErrorCode::SourceContract);
        CHECK(sorted.error().column == "c");
        CHECK(engine.sort().empty());
        CHECK(viewKeys(engine) == before);
    }

    SECTION("an update no call asked for goes to onError") {
        auto const loaded = threeRows("", false);
        Engine shown{loaded};
        REQUIRE(shown.setSort(byC));
        auto const before = viewKeys(shown);
        std::vector<TableError> errors;
        shown.onError([&](TableError const& error) { errors.push_back(error); });
        loaded->updateRow(1, {Cell{Unloaded{}}});
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == TableErrorCode::SourceContract);
        CHECK(errors[0].column == "c");
        CHECK(viewKeys(shown) == before);
    }

    SECTION("the same column at a level sorts the unloaded row last") {
        Engine slow{threeRows("slow", true)};
        REQUIRE(slow.setSort(byC));
        CHECK(viewKeys(slow) == std::vector<RowId>{2, 0, 1});
    }
}

// Mutation: displayText returning "…" for an unloaded cell.
TEST_CASE("table: cellAt returns the unloaded cell", "[table][unloaded]") {
    Engine const engine{tabletest::slowColumn(ColumnKind::Text, {Cell{Unloaded{}}})};
    CHECK(std::holds_alternative<Unloaded>(engine.cellAt(0, 0)));
    CHECK(displayText(engine.cellAt(0, 0)).empty());
}

// Mutation: CellEdits::commit not looking at the cell (the commit runs and the cell has no error).
TEST_CASE("table: an edit of an unloaded cell is refused on the cell", "[table][unloaded][edits]") {
    auto const source = tabletest::makeSource(
        {tabletest::levelCol("c", ColumnKind::Integer, "slow"), tabletest::col("d", ColumnKind::Text)},
        {{Cell{Unloaded{}}, Cell{std::string{"x"}}}});
    Engine engine{source};
    std::vector<EditRequest> ran;
    CellEdits edits{*source, [&](EditRequest const& request, EditDone const&) { ran.push_back(request); }, &engine};
    edits.commit({.row = RowId{0}, .column = "c", .value = Cell{std::int64_t{5}}});
    CHECK(ran.empty());
    CHECK(edits.error(RowId{0}, "c") == "not loaded yet");
    CHECK_FALSE(edits.stale(RowId{0}, "c"));
    edits.commit({.row = RowId{0}, .column = "d", .value = Cell{std::string{"y"}}});
    REQUIRE(ran.size() == 1);
    CHECK(ran[0].column == "d");
    CHECK(edits.error(RowId{0}, "c") == "not loaded yet");
}

// Mutation: selectAll selecting the view and the undecided rows.
TEST_CASE("table: selectAll does not select undecided rows", "[table][unloaded][selection]") {
    auto const source = threeRows("slow", true);
    Engine engine{source};
    REQUIRE(engine.setFilter(FilterSpec{.columns = {{"c", ColumnFilter{.include = {FilterEntry{.gt = "0"}}}}}}));
    REQUIRE(engine.undecidedRowCount() == 1);
    auto selection = multiple(false);
    REQUIRE(selection.selectAll(engine));
    CHECK(selection.contains(RowId{0}));
    CHECK(selection.contains(RowId{2}));
    CHECK_FALSE(selection.contains(RowId{1}));
}

// Mutation: Selection::viewChanged keeping a selected row that is in the undecided set.
TEST_CASE("table: a selected row that becomes undecided is pruned", "[table][unloaded][selection]") {
    auto const source = threeRows("slow", false);
    Engine engine{source};
    REQUIRE(engine.setFilter(FilterSpec{.columns = {{"c", ColumnFilter{.include = {FilterEntry{.gt = "0"}}}}}}));
    auto selection = multiple(true);
    REQUIRE(selection.select(RowId{1}));
    engine.onViewChange([&](ViewChange const&) { selection.viewChanged(engine); });
    source->updateRow(1, {Cell{Unloaded{}}});
    CHECK(engine.undecidedRowCount() == 1);
    CHECK_FALSE(selection.contains(RowId{1}));
}

// ── Data levels on typed rows, pages and the wide fixture ───────────────────

namespace {

using tabletest::LevelRow;

RowKey<LevelRow> levelKey() {
    return [](LevelRow const& row) { return RowId{row.id}; };
}

// Refuses "stock" for odd ids.
LevelLoaded<LevelRow> stockOnEvenIds() {
    return [](LevelRow const& row, std::string_view level) { return level != "stock" || row.id % 2 == 0; };
}

std::vector<std::int64_t> idsOf(std::vector<LevelRow> const& rows) {
    std::vector<std::int64_t> out;
    out.reserve(rows.size());
    for (auto const& row : rows) {
        out.push_back(row.id);
    }
    return out;
}

// A source that logs the column of every cell it hands out, by `cell` or `readColumn`.
class RecordingSnapshot final : public RowSnapshot {
public:
    RecordingSnapshot(std::shared_ptr<RowSnapshot const> inner, std::set<std::size_t>& log)
        : _inner{std::move(inner)}, _log{&log} {}
    [[nodiscard]] std::size_t rowCount() const override { return _inner->rowCount(); }
    [[nodiscard]] RowId rowId(std::size_t row) const override { return _inner->rowId(row); }
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        _log->insert(column);
        return _inner->cell(row, column);
    }
    void readColumn(std::size_t column, ColumnSink& sink, std::size_t first, std::size_t last) const override {
        _log->insert(column);
        _inner->readColumn(column, sink, first, last);
    }

private:
    std::shared_ptr<RowSnapshot const> _inner;
    std::set<std::size_t>* _log;
};

class RecordingSource final : public DataSource {
public:
    explicit RecordingSource(std::shared_ptr<DataSource> inner) : _inner{std::move(inner)} {}
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _inner->columns(); }
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override {
        return std::make_shared<RecordingSnapshot const>(_inner->snapshot(), _log);
    }
    void subscribe(ChangeListener& /*listener*/) override {}
    void unsubscribe(ChangeListener& /*listener*/) override {}
    [[nodiscard]] std::set<std::size_t> const& log() const noexcept { return _log; }

private:
    std::shared_ptr<DataSource> _inner;
    mutable std::set<std::size_t> _log;
};

// The columns of @p source at level "recipe".
std::vector<std::string> recipeColumns(DataSource const& source) {
    std::vector<std::string> out;
    for (auto const& column : source.columns()) {
        if (column.level == "recipe") {
            out.push_back(column.id);
        }
    }
    return out;
}

std::size_t columnIndex(DataSource const& source, std::string_view id) {
    auto const found = detail::findColumn(source.columns(), id);
    REQUIRE(found.has_value());
    return *found;
}

// Whether any row of @p column holds `Unloaded`.
bool anyUnloaded(DataSource const& source, std::string_view column) {
    auto const snapshot = source.snapshot();
    auto const index = columnIndex(source, column);
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        if (std::holds_alternative<Unloaded>(snapshot->cell(row, index))) {
            return true;
        }
    }
    return false;
}

// Every row of the view @p query gives over @p source, by a fresh `apply`: the oracle.
PageRows wholeView(DataSource const& source, TableQuery query) {
    query.page = PageRequest{.offset = 0, .limit = std::numeric_limits<std::int64_t>::max()};
    ApplyOptions options;
    options.services.units = bench::wideUnits();
    options.limits.maxLimit = std::numeric_limits<std::int64_t>::max();
    options.limits.totalCap = std::numeric_limits<std::int64_t>::max();
    auto page = apply(source, query, options);
    REQUIRE(page.has_value());
    return std::move(*page);
}

// The 30-column wide table of 10,000 rows, its recipe columns loaded for @p loadedFraction of the rows.
std::shared_ptr<bench::UnloadedSource> wideUnloaded(double loadedFraction) {
    auto wide = std::make_shared<bench::WideTableSource>(
        bench::WideTableOptions{.rows = 10'000, .columns = 30, .seed = 7, .loadDelay = {}});
    auto const columns = recipeColumns(*wide);
    return bench::withUnloaded(std::move(wide), columns, "recipe", loadedFraction, 7);
}

}  // namespace

// A row whose levels name a member it does not have. Reflected by glaze, so not in an anonymous namespace.
namespace tunloaded {

struct NoSuchColumnRow {
    std::int64_t id = 0;
    static constexpr std::array<ColumnLevel, 1> columnLevels() noexcept {
        return {{{.column = "nope", .level = "stock"}}};
    }
};

// A row that declares no levels.
struct PlainRow {
    std::int64_t id = 0;
    std::int64_t qty = 0;
};

}  // namespace tunloaded

// Mutation: apply building keys for every column.
TEST_CASE("table: apply reads only the sorted and filtered columns", "[table][unloaded][levels]") {
    auto wide = std::make_shared<bench::WideTableSource>(
        bench::WideTableOptions{.rows = 1000, .columns = 300, .seed = 7, .loadDelay = {}});
    auto const columns = recipeColumns(*wide);
    REQUIRE(columns.size() == 20);
    auto const unloaded = bench::withUnloaded(std::move(wide), columns, "recipe", 0.7, 7);
    REQUIRE(anyUnloaded(*unloaded, "d003"));
    REQUIRE(anyUnloaded(*unloaded, "t005"));
    RecordingSource const recording{unloaded};
    TableQuery const query{
        .sort = {SortKey{.column = "d003"}},
        .filters = FilterSpec{.columns = {{"t005", ColumnFilter{.include = {FilterEntry{.contains = "a"}}}}}}};
    ApplyOptions options;
    options.services.units = bench::wideUnits();
    auto const page = apply(recording, query, options);
    REQUIRE(page.has_value());
    CHECK(page->undecided > 0);
    CHECK(recording.log() == std::set<std::size_t>{columnIndex(recording, "d003"), columnIndex(recording, "t005")});
}

// Mutation: the typed apply dropping unloadedRows.
TEST_CASE("table: a typed page names exactly its unloaded rows and levels", "[table][unloaded][levels]") {
    auto const rows = tabletest::levelRows(10);
    using V = std::vector<std::string>;
    auto const all = apply<LevelRow>(rows, levelKey(), TableQuery{}, {}, {}, {}, stockOnEvenIds());
    REQUIRE(all.has_value());
    CHECK(idsOf(all->rows) == std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK(all->unloadedRows == std::vector<UnloadedRow>{{.index = 1, .levels = V{"stock"}, .failed = {}},
                                                        {.index = 3, .levels = V{"stock"}, .failed = {}},
                                                        {.index = 5, .levels = V{"stock"}, .failed = {}},
                                                        {.index = 7, .levels = V{"stock"}, .failed = {}},
                                                        {.index = 9, .levels = V{"stock"}, .failed = {}}});
    CHECK(all->awaiting.empty());  // nothing sorts or filters on qty

    auto const named =
        apply<LevelRow>(rows, levelKey(), TableQuery{.columns = V{"id", "name"}}, {}, {}, {}, stockOnEvenIds());
    REQUIRE(named.has_value());
    CHECK(named->unloadedRows.empty());  // the level is not needed

    // Sorted on the level: the refused rows go last, and the level is awaited.
    // Mutation: awaiting left empty when an order level is refused.
    auto const sorted =
        apply<LevelRow>(rows, levelKey(), TableQuery{.sort = {SortKey{.column = "qty"}}, .columns = V{"id"}}, {}, {},
                        {}, stockOnEvenIds());
    REQUIRE(sorted.has_value());
    CHECK(idsOf(sorted->rows) == std::vector<std::int64_t>{0, 2, 4, 6, 8, 1, 3, 5, 7, 9});
    CHECK(sorted->awaiting == V{"stock"});
    REQUIRE(sorted->unloadedRows.size() == 5);
    CHECK(sorted->unloadedRows.front().index == 5);
    CHECK(sorted->unloadedRows.back().index == 9);
}

// Mutation: a columnLevels() entry naming no column ignored.
TEST_CASE("table: a column level naming no column is refused where columns are reflected",
          "[table][unloaded][levels]") {
    using tunloaded::NoSuchColumnRow;
    auto const rows = std::make_shared<std::vector<NoSuchColumnRow> const>();
    auto const key = [](NoSuchColumnRow const& row) { return RowId{row.id}; };
    try {
        RowsSource<NoSuchColumnRow> const source{rows, key};
        FAIL("RowsSource accepted a level naming no column");
    } catch (std::invalid_argument const& error) {
        CHECK(std::string_view{error.what()}.contains("nope"));
    }
}

// Mutation: awaiting filled with every level of the columns when `loaded` is empty.
TEST_CASE("table: a typed table without levels is whole", "[table][unloaded][levels]") {
    TableQuery const query{
        .sort = {SortKey{.column = "qty", .dir = SortDirection::Descending}},
        .filters = FilterSpec{.columns = {{"qty", ColumnFilter{.include = {FilterEntry{.gt = "20"}}}}}},
        .page = PageRequest{.offset = 1, .limit = 3}};

    SECTION("no LevelLoaded and no columnLevels()") {
        using tunloaded::PlainRow;
        std::vector<PlainRow> rows;
        for (std::int64_t i = 0; i < 10; ++i) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
            rows.push_back(PlainRow{.id = i, .qty = 10 * i});
        }
        auto const page = apply<PlainRow>(rows, [](PlainRow const& row) { return RowId{row.id}; }, query);
        REQUIRE(page.has_value());
        std::vector<std::int64_t> ids;
        for (auto const& row : page->rows) {
            ids.push_back(row.id);
        }
        CHECK(ids == std::vector<std::int64_t>{8, 7, 6});
        CHECK(page->total == 7);
        CHECK(page->offset == 1);
        CHECK(page->totalKind == TotalKind::Exact);
        CHECK(page->undecided == 0);
        CHECK(page->unloadedRows.empty());
        CHECK(page->awaiting.empty());
        CHECK(page->failed.empty());
    }

    SECTION("columnLevels() and no LevelLoaded: every level is loaded") {
        auto const page = apply<LevelRow>(tabletest::levelRows(10), levelKey(), query);
        REQUIRE(page.has_value());
        CHECK(idsOf(page->rows) == std::vector<std::int64_t>{8, 7, 6});
        CHECK(page->total == 7);
        CHECK(page->totalKind == TotalKind::Exact);
        CHECK(page->undecided == 0);
        CHECK(page->unloadedRows.empty());
        CHECK(page->awaiting.empty());
        CHECK(page->failed.empty());
    }
}

// Mutation: UnloadedSource::load notifying Reset (a full sort instead of a repair).
TEST_CASE("table: an arrival is one repair", "[table][unloaded][levels]") {
    auto const source = wideUnloaded(0.7);
    REQUIRE(anyUnloaded(*source, "d000"));
    Engine engine{source};
    TableQuery const query{.sort = {SortKey{.column = "d000"}}};
    REQUIRE(engine.setSort(query.sort));
    for (std::size_t const count : {std::size_t{200}, std::size_t{1000}}) {
        auto const waiting = source->unloadedRows();
        REQUIRE(waiting.size() > count);
        std::vector<std::size_t> const arriving(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(count));
        auto const before = engine.stats();
        source->load(arriving);
        CHECK(engine.stats().repairs == before.repairs + 1);
        CHECK(engine.stats().fullSorts == before.fullSorts);
        auto const oracle = wholeView(*source, query);
        std::vector<std::size_t> place(source->snapshot()->rowCount());
        for (std::size_t i = 0; i < oracle.rows.size(); ++i) {
            place[oracle.rows[i]] = i;
        }
        auto const snapshot = source->snapshot();
        std::size_t misplaced = 0;
        for (auto const row : arriving) {
            misplaced += engine.viewRowOfKey(snapshot->rowId(row)) == place[row] ? 0U : 1U;
        }
        CHECK(misplaced == 0);
    }
}

// Mutation: a repair that constructs a FilterRun over previousUndecided.
TEST_CASE("table: deciding rows runs no filter over all rows", "[table][unloaded][levels]") {
    auto const source = wideUnloaded(0.5);
    Engine engine{source};
    FilterSpec const filter{.columns = {{"d000", ColumnFilter{.include = {FilterEntry{.gt = "0"}}}}}};
    REQUIRE(engine.setFilter(filter));
    REQUIRE(engine.undecidedRowCount() > 4000);
    auto const waiting = source->unloadedRows();
    std::vector<std::size_t> const arriving(waiting.begin(), waiting.begin() + 1000);
    auto const before = engine.stats();
    source->load(arriving);
    CHECK(engine.stats().repairs == before.repairs + 1);
    CHECK(engine.stats().filterRuns == before.filterRuns);
    auto const oracle = wholeView(*source, TableQuery{.filters = filter});
    std::set<std::size_t> const shown(oracle.rows.begin(), oracle.rows.end());
    auto const snapshot = source->snapshot();
    std::size_t wrong = 0;
    for (auto const row : arriving) {
        bool const inView = engine.viewRowOfKey(snapshot->rowId(row)).has_value();
        wrong += inView == shown.contains(row) ? 0U : 1U;
    }
    CHECK(wrong == 0);
    CHECK(engine.undecidedRowCount() == static_cast<std::size_t>(oracle.undecided));
}
