// SPDX-License-Identifier: Apache-2.0
//
// morph::table::Engine's pending work (spec 7 §7, §8): rows updated, rows
// owed a Changed, the cached sort order, held rows and the columns, as
// requests overtake each other and jobs are dropped or fail.
//
// Each case names the mutation it was seen to fail on.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/table/edits.hpp>
#include <morph/table/engine.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::col;
using tabletest::ContentModel;
using tabletest::drain;
using tabletest::FollowingModel;
using tabletest::ManualExecutor;
using tabletest::viewKeys;

namespace {

constexpr auto kAsc = SortDirection::Ascending;
constexpr auto kDesc = SortDirection::Descending;

std::vector<RowId> keys(std::initializer_list<std::int64_t> values) {
    std::vector<RowId> out;
    for (auto const value : values) {
        out.emplace_back(value);
    }
    return out;
}

RowId key(std::int64_t value) { return RowId{value}; }

// Rows 0..n-1 with n = i * 10, k = i, name = `name`: sorted by n, the order is the key order.
std::shared_ptr<VectorSource> numbered(std::size_t rows, std::string const& name = "old") {
    std::vector<std::vector<Cell>> cells;
    cells.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        auto const value = static_cast<std::int64_t>(i);
        cells.push_back({Cell{value * 10}, Cell{value}, Cell{name}});
    }
    return tabletest::makeSource(
        {col("n", ColumnKind::Integer), col("k", ColumnKind::Integer), col("name", ColumnKind::Text)},
        std::move(cells));
}

std::vector<Cell> numberedRow(std::int64_t value, std::string name) {
    return {Cell{value * 10}, Cell{value}, Cell{std::move(name)}};
}

FilterSpec nAtLeast(std::string const& bound) {
    FilterSpec spec;
    spec.columns["n"].include.push_back(FilterEntry{.ge = bound});
    return spec;
}

bool held(Engine const& engine, std::int64_t value) {
    auto const rows = engine.heldRows();
    return std::ranges::find(rows, key(value)) != rows.end();
}

}  // namespace

// Mutation: Engine::makeInput clearing the rows notified but not yet taken in
// (`_notifiedRows.clear()`): row 1's update is lost and it stays in the view.
TEST_CASE("table: a sort set between an update and its flush keeps the update", "[table][pending]") {
    ManualExecutor owner;
    auto const source = tabletest::makeSource({col("a", ColumnKind::Integer), col("b", ColumnKind::Text)},
                                              {{Cell{std::int64_t{1}}, Cell{std::string{"ax"}}},
                                               {Cell{std::int64_t{2}}, Cell{std::string{"bx"}}},
                                               {Cell{std::int64_t{3}}, Cell{std::string{"cx"}}},
                                               {Cell{std::int64_t{4}}, Cell{std::string{"d"}}},
                                               {Cell{std::int64_t{5}}, Cell{std::string{"ex"}}}});
    Engine engine{source, EngineOptions{.owner = &owner}};
    FollowingModel model{engine};
    FilterSpec containsX;
    containsX.columns["b"].include.push_back(FilterEntry{.contains = std::string{"x"}});
    REQUIRE(engine.setFilter(containsX).has_value());
    drain(owner);

    source->updateRow(1, {Cell{std::int64_t{2}}, Cell{std::string{"nope"}}});  // its flush is posted
    REQUIRE(engine.setSort({{"a", kDesc}}).has_value());                       // computes before that flush
    source->updateRow(3, {Cell{std::int64_t{4}}, Cell{std::string{"dx"}}});
    drain(owner);

    CHECK(viewKeys(engine) == keys({4, 3, 2, 0}));
    CHECK(model.check());
}

// Mutation: Engine::install keeping a cached sort order of older rows (no
// `_sortCache = {}` when the result made none): a cache from before row 0's
// update is repaired with row 2 only, and the view is 2 0 1.
TEST_CASE("table: a cached order from older rows is never repaired as current", "[table][pending]") {
    auto const source = tabletest::column(ColumnKind::Integer,
                                          {Cell{std::int64_t{0}}, Cell{std::int64_t{1}}, Cell{std::int64_t{2}}}, "a");
    Engine engine{source};
    REQUIRE(engine.setSort({{"a", kAsc}}).has_value());  // caches the full order
    FilterSpec everyRow;
    everyRow.columns["a"].include.push_back(FilterEntry{.ge = "0"});
    REQUIRE(engine.setFilter(everyRow).has_value());
    REQUIRE(engine.setSort({{"a", kDesc}}).has_value());
    source->updateRow(0, {Cell{std::int64_t{10}}});  // repaired under desc: the asc cache is not touched
    REQUIRE(engine.setSort({{"a", kAsc}}).has_value());
    source->updateRow(2, {Cell{std::int64_t{3}}});
    REQUIRE(engine.setFilter(FilterSpec{}).has_value());

    CHECK(viewKeys(engine) == keys({1, 2, 0}));
    Engine reference{source};
    REQUIRE(reference.setSort({{"a", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}

// Mutation: Engine::giveBack dropping the dirty rows of a dropped job: the
// updated rows are never reported Changed, and the renderer keeps old text.
TEST_CASE("table: rows owed a Changed survive a dropped job", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);
    ContentModel model{engine, 2};

    SECTION("an update flushed while a filter job runs") {
        REQUIRE(engine.setFilter(nAtLeast("10")).has_value());
        source->updateRow(7, numberedRow(7, "new"));
        owner.runAll();  // the flush, while the filter job is on the worker
    }
    SECTION("a repair superseded by a filter") {
        source->updateRow(100, numberedRow(100, "new"));
        owner.runAll();  // the flush starts the repair
        REQUIRE(engine.setFilter(nAtLeast("10")).has_value());
    }
    SECTION("a reset superseded by a filter") {
        std::vector<RowId> ids;
        std::vector<std::vector<Cell>> rows;
        for (std::int64_t i = 0; i < 3000; ++i) {
            ids.emplace_back(i);
            rows.push_back(numberedRow(i, "reset"));
        }
        source->setRows(std::move(ids), std::move(rows));
        owner.runAll();  // the flush starts the recompute
        REQUIRE(engine.setFilter(nAtLeast("10")).has_value());
    }
    drain(owner, &worker);
    CHECK_FALSE(engine.pending());
    CHECK(model.stale() == 0);
}

// Mutation: Engine::schedule stopping a running job for updates alone
// (`supersedes` true when `_dirty` is not empty): the repair is dropped, and
// the stats show it.
TEST_CASE("table: an update waits for the running repair rather than dropping it", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);

    source->updateRow(100, numberedRow(100, "a"));
    owner.runAll();  // the repair starts
    source->updateRow(200, {Cell{std::int64_t{-5}}, Cell{std::int64_t{200}}, Cell{std::string{"b"}}});
    owner.runAll();  // its flush finds the repair running
    drain(owner, &worker);

    CHECK(engine.stats().dropped == 0);
    CHECK(engine.stats().repairs == 2);
    CHECK(engine.rowIdAt(0) == key(200));
    CHECK(model.check());
}

// Mutation: Engine::relayout publishing while a job runs (no `_running`
// check): the job's change was computed against the view before the release,
// and replaying it diverges.
TEST_CASE("table: holds released while a job runs are laid out with its result", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source,
                  EngineOptions{.owner = &owner,
                                .worker = &worker,
                                .reorder = ReorderPolicy::Deferred,
                                .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);
    source->updateRow(0, numberedRow(99'999, "last"));  // sorts last; held at the top
    drain(owner, &worker);
    REQUIRE(held(engine, 0));
    REQUIRE(engine.rowIdAt(0) == key(0));

    REQUIRE(engine.setFilter(nAtLeast("10")).has_value());
    REQUIRE(engine.pending());
    engine.settleNow();  // releases row 0 while the filter job runs
    CHECK(model.check());
    drain(owner, &worker);
    CHECK_FALSE(engine.pending());
    CHECK(model.check());
    CHECK(engine.rowIdAt(engine.viewRowCount() - 1) == key(0));
}

// Mutation: Engine::canRepair returning false once a job is running, so the
// updates that waited on it recompute and release every hold: the row being
// edited jumps to its sorted place.
TEST_CASE("table: a row being edited stays held through updates that wait on a job", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source,
                  EngineOptions{.owner = &owner,
                                .worker = &worker,
                                .reorder = ReorderPolicy::Deferred,
                                .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);
    engine.beginEdit(key(500));
    source->updateRow(500, numberedRow(999'999, "edited"));
    drain(owner, &worker);
    REQUIRE(engine.viewRowOfKey(key(500)) == 500U);

    source->updateRow(10, numberedRow(10, "other"));
    owner.runAll();  // a repair starts
    source->updateRow(20, numberedRow(20, "other"));
    owner.runAll();  // its flush finds the repair running
    drain(owner, &worker);

    CHECK(engine.viewRowOfKey(key(500)) == 500U);
    CHECK(held(engine, 500));
    CHECK(model.check());
}

// Mutation: Engine::install clearing every ended-edit token at each result
// (`_releaseOnRepair.clear()`): an unrelated repair that completes between the
// commit's patch and its flush uses the token up, and the patch then holds
// the committed row a second time.
TEST_CASE("table: an ended edit moves its row once even past another repair", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source,
                  EngineOptions{.owner = &owner,
                                .worker = &worker,
                                .reorder = ReorderPolicy::Deferred,
                                .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}}};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);
    std::vector<EditDone> commits;
    CellEdits edits{*source, [&](EditRequest const&, EditDone done) { commits.push_back(std::move(done)); }, &engine};
    edits.commit(EditRequest{.row = key(5), .column = "n", .value = Cell{std::int64_t{999'999}}});

    source->updateRow(100, numberedRow(100, "unrelated"));  // does not move
    owner.runAll();                                         // its repair starts
    worker.runAll();                                        // and finishes; the result waits on the owner
    REQUIRE(commits.size() == 1);
    commits[0](numberedRow(99'999, "committed"));
    drain(owner, &worker);

    CHECK(engine.viewRowOfKey(key(5)) == engine.viewRowCount() - 1);
    CHECK_FALSE(held(engine, 5));
}

// Mutation: the same token outliving the view that took in its update (no
// erase against `_viewChangeSeq`): with no owner the commit's patch is applied
// before the edit ends, and the token then keeps a later update from holding.
TEST_CASE("table: an ended edit's token does not outlive its update", "[table][pending]") {
    auto const source = numbered(5);
    Engine engine{source,
                  EngineOptions{.reorder = ReorderPolicy::Deferred,
                                .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}}};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    std::vector<EditDone> commits;
    CellEdits edits{*source, [&](EditRequest const&, EditDone done) { commits.push_back(std::move(done)); }, &engine};
    edits.commit(EditRequest{.row = key(1), .column = "n", .value = Cell{std::int64_t{35}}});
    REQUIRE(commits.size() == 1);
    commits[0](numberedRow(3, "committed"));  // n = 30: between rows 2 and 3
    REQUIRE(viewKeys(engine) == keys({0, 2, 1, 3, 4}));

    source->updateRow(1, numberedRow(-1, "pushed"));  // would sort first
    CHECK(viewKeys(engine) == keys({0, 2, 1, 3, 4}));
    CHECK(held(engine, 1));
}

// Mutation: Engine::columns() returning the requested columns (`_columns`)
// rather than the applied view's: they change at the flush while cellAt still
// reads the old rows, and an index past the old columns reads out of bounds.
TEST_CASE("table: a column-changing reset shows its columns with its view", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    std::vector<std::vector<Cell>> cells;
    cells.reserve(3000);
    for (std::int64_t i = 0; i < 3000; ++i) {
        cells.push_back({Cell{i}});
    }
    auto const source = tabletest::makeSource({col("a", ColumnKind::Integer)}, std::move(cells));
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel model{engine};

    std::vector<RowId> ids;
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t i = 0; i < 3000; ++i) {
        ids.emplace_back(i);
        rows.push_back({Cell{"b" + std::to_string(i)}, Cell{i}, Cell{true}});
    }
    source->setRows(std::move(ids), std::move(rows),
                    std::vector<ColumnInfo>{col("b", ColumnKind::Text), col("a", ColumnKind::Integer),
                                            col("c", ColumnKind::Bool)});
    owner.runAll();  // the flush starts the recompute
    REQUIRE(engine.pending());
    REQUIRE(engine.columns().size() == 1);
    CHECK(engine.columns()[0].id == "a");
    auto const beforeCell = engine.cellAt(5, 0);
    auto const* const before = std::get_if<std::int64_t>(&beforeCell);
    REQUIRE(before != nullptr);
    CHECK(*before == 5);
    CHECK(std::holds_alternative<std::monostate>(engine.cellAt(5, 2)));

    drain(owner, &worker);
    CHECK_FALSE(engine.pending());
    REQUIRE(engine.columns().size() == 3);
    CHECK(engine.columns()[1].id == "a");
    auto const afterCell = engine.cellAt(5, 1);
    auto const* const after = std::get_if<std::int64_t>(&afterCell);
    REQUIRE(after != nullptr);
    CHECK(*after == 5);
    CHECK(model.last().isReset());  // row operations cannot say the columns changed
    CHECK(model.check());
}

namespace {

// Orders cells by their integer value, and throws when `armed` is set.
CellComparator throwingComparator(std::shared_ptr<bool const> armed) {
    return [armed = std::move(armed)](Cell const& left, Cell const& right) -> std::strong_ordering {
        if (*armed) {
            throw std::runtime_error{"comparator down"};
        }
        return std::get<std::int64_t>(left) <=> std::get<std::int64_t>(right);
    };
}

std::shared_ptr<VectorSource> customColumn(std::size_t rows) {
    std::vector<std::vector<Cell>> cells;
    cells.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        cells.push_back({Cell{static_cast<std::int64_t>(rows - i)}});
    }
    return tabletest::makeSource({col("c", ColumnKind::Custom, "byValue")}, std::move(cells));
}

}  // namespace

// Mutation: ViewJob::step without its try/catch: the exception escapes setSort
// after the sort chain was replaced.
TEST_CASE("table: a service that throws inside the call fails the sort and keeps the old one", "[table][pending]") {
    auto const armed = std::make_shared<bool>(true);
    EngineOptions options;
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    Engine engine{customColumn(10), options};
    auto const result = engine.setSort({{.column = "c", .dir = kAsc}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == TableErrorCode::ServiceFailed);
    CHECK(engine.sort().empty());
    CHECK(viewKeys(engine) == keys({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));

    *armed = false;
    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}}).has_value());
    CHECK(viewKeys(engine) == keys({9, 8, 7, 6, 5, 4, 3, 2, 1, 0}));
}

// Mutation: Engine::finish ignoring `job->failure()`: the half-built result is
// applied, or (before the job caught) pending() stayed true for good.
TEST_CASE("table: a service that throws on the worker reports through onError", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const armed = std::make_shared<bool>(true);
    EngineOptions options{.owner = &owner, .worker = &worker};
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    Engine engine{customColumn(3000), options};
    std::vector<TableError> errors;
    engine.onError([&](TableError const& error) { errors.push_back(error); });
    std::vector<bool> pendingSeen;
    engine.onPending([&](bool pending) { pendingSeen.push_back(pending); });

    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}}).has_value());
    REQUIRE(engine.pending());
    drain(owner, &worker);
    CHECK_FALSE(engine.pending());
    CHECK(pendingSeen == std::vector<bool>{true, false});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == TableErrorCode::ServiceFailed);
    CHECK(engine.sort().empty());
    CHECK(engine.rowIdAt(0) == key(0));
}
