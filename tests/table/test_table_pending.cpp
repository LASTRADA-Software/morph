// SPDX-License-Identifier: Apache-2.0
//
// morph::table::Engine's pending work (spec 7 §7, §8): rows updated, rows
// owed a Changed, the cached sort order, held rows and the columns, as
// requests overtake each other and jobs are dropped or fail.
//
// Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/table/edits.hpp>
#include <morph/table/engine.hpp>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// An exception that is not a std::exception ends the job like any other.
TEST_CASE("table: a service that throws a non-standard exception fails the sort", "[table][pending]") {
    auto const armed = std::make_shared<bool>(true);
    EngineOptions options;
    options.services.comparators.emplace("byValue", [armed](Cell const& left, Cell const& right) {
        if (*armed) {
            throw 42;  // NOLINT(hicpp-exception-baseclass) -- the case is an exception that is not a std::exception
        }
        return std::get<std::int64_t>(left) <=> std::get<std::int64_t>(right);
    });
    Engine engine{customColumn(10), options};
    auto const result = engine.setSort({{.column = "c", .dir = kAsc}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == TableErrorCode::ServiceFailed);
    CHECK(result.error().message.contains("unknown exception"));
    CHECK(engine.sort().empty());
}

namespace {

// Folds by lowering ASCII, and throws while `armed` is set: a std::exception,
// or anything else when `standard` is false.
class ThrowingCollator final : public TextCollator {
public:
    ThrowingCollator(std::shared_ptr<bool const> armed, bool standard)
        : _armed{std::move(armed)}, _standard{standard} {}
    [[nodiscard]] std::string sortKey(std::string_view text) const override { return fold(text); }
    [[nodiscard]] std::string fold(std::string_view text) const override {
        if (*_armed) {
            if (_standard) {
                throw std::runtime_error{"collator down"};
            }
            throw 7;  // NOLINT(hicpp-exception-baseclass) -- the case is an exception that is not a std::exception
        }
        std::string out{text};
        for (auto& ch : out) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return out;
    }

private:
    std::shared_ptr<bool const> _armed;
    bool _standard;
};

}  // namespace

// Mutation: Engine::compile without its try/catch: a collator that throws
// while a text filter's value is folded escapes setFilter.
TEST_CASE("table: a collator that throws while a filter compiles fails setFilter", "[table][pending]") {
    auto const standard = GENERATE(true, false);
    auto const armed = std::make_shared<bool>(false);
    EngineOptions options;
    options.services.collator = std::make_shared<ThrowingCollator>(armed, standard);
    Engine engine{tabletest::column(ColumnKind::Text, {std::string{"Ab"}, std::string{"cd"}}), options};
    *armed = true;
    FilterSpec spec;
    spec.columns["c"].include.push_back(FilterEntry{.contains = "A"});
    auto const result = engine.setFilter(spec);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == TableErrorCode::ServiceFailed);
    CHECK(result.error().message.contains(standard ? "collator down" : "unknown exception"));
    CHECK(engine.filter().empty());
    *armed = false;
    REQUIRE(engine.setFilter(spec).has_value());
    CHECK(viewKeys(engine) == keys({0}));
}

// Mutation: Engine::flushChanges dropping schedule()'s error: a repair that
// fails inside the change notification is reported nowhere.
TEST_CASE("table: a repair that fails inside the notification reports through onError", "[table][pending]") {
    auto const armed = std::make_shared<bool>(false);
    EngineOptions options;
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    auto const source = customColumn(10);
    Engine engine{source, options};
    std::vector<TableError> errors;
    engine.onError([&](TableError const& error) { errors.push_back(error); });
    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}}).has_value());
    auto const before = viewKeys(engine);

    *armed = true;
    source->updateRow(0, {Cell{std::int64_t{0}}});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == TableErrorCode::ServiceFailed);
    CHECK(viewKeys(engine) == before);
    CHECK(engine.sort() == SortChain{{.column = "c", .dir = kAsc}});

    // The failed repair handed its row back: the next one repairs both.
    *armed = false;
    source->updateRow(1, {Cell{std::int64_t{100}}});
    Engine reference{source, options};
    REQUIRE(reference.setSort({{.column = "c", .dir = kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
    CHECK(engine.rowIdAt(0) == key(0));
    CHECK(engine.rowIdAt(9) == key(1));
}

// Mutation: Engine::fitToColumns leaving group columns that vanished, or
// keeping a filter entry that no longer compiles for its column's new kind.
TEST_CASE("table: a reset trims group columns and drops a filter its columns no longer take", "[table][pending]") {
    auto const source =
        tabletest::makeSource({col("a", ColumnKind::Integer), col("b", ColumnKind::Text), col("c", ColumnKind::Text)},
                              {{Cell{std::int64_t{1}}, Cell{std::string{"x"}}, Cell{std::string{"-"}}},
                               {Cell{std::int64_t{2}}, Cell{std::string{"-"}}, Cell{std::string{"x"}}},
                               {Cell{std::int64_t{-3}}, Cell{std::string{"x"}}, Cell{std::string{"-"}}}});
    Engine engine{source};
    FilterSpec spec;
    spec.columns["a"].include.push_back(FilterEntry{.gt = "0"});
    spec.groups.push_back(
        GroupFilter{.columns = {"b", "c"}, .combine = {}, .include = {FilterEntry{.contains = "x"}}});
    REQUIRE(engine.setFilter(spec).has_value());
    CHECK(viewKeys(engine) == keys({0, 1}));

    auto const rows = [] {
        return std::vector<std::vector<Cell>>{{Cell{std::int64_t{1}}, Cell{std::string{"x"}}},
                                              {Cell{std::int64_t{2}}, Cell{std::string{"-"}}},
                                              {Cell{std::int64_t{-3}}, Cell{std::string{"x"}}}};
    };
    source->setRows(keys({0, 1, 2}), rows(),
                    std::vector<ColumnInfo>{col("a", ColumnKind::Integer), col("b", ColumnKind::Text)});
    REQUIRE(engine.filter().groups.size() == 1);
    CHECK(engine.filter().groups[0].columns == std::vector<std::string>{"b"});
    CHECK(viewKeys(engine) == keys({0}));

    // `a` becomes a Bool column, where `gt` is not defined: the filter goes.
    source->setRows(keys({0, 1, 2}), rows(),
                    std::vector<ColumnInfo>{col("a", ColumnKind::Bool), col("b", ColumnKind::Text)});
    CHECK(engine.filter().empty());
    CHECK(viewKeys(engine) == keys({0, 1, 2}));
}

// Mutation: Engine::revert not fitting the restored sort to columns that
// changed meanwhile: the sort keeps a key for a column that is gone.
TEST_CASE("table: a failed job after a column change restores a sort fitted to the new columns", "[table][pending]") {
    auto const armed = std::make_shared<bool>(false);
    EngineOptions options;
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    auto const source = tabletest::makeSource(
        {col("c", ColumnKind::Custom, "byValue"), col("d", ColumnKind::Integer)},
        {{Cell{std::int64_t{2}}, Cell{std::int64_t{0}}}, {Cell{std::int64_t{1}}, Cell{std::int64_t{0}}}});
    Engine engine{source, options};
    std::vector<TableError> errors;
    engine.onError([&](TableError const& error) { errors.push_back(error); });
    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}, {.column = "d", .dir = kAsc}}).has_value());

    *armed = true;
    source->setRows(keys({0, 1}),
                    {{Cell{std::int64_t{2}}, Cell{std::int64_t{0}}}, {Cell{std::int64_t{1}}, Cell{std::int64_t{0}}}},
                    std::vector<ColumnInfo>{col("c", ColumnKind::Custom, "byValue"), col("e", ColumnKind::Integer)});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == TableErrorCode::ServiceFailed);
    CHECK(engine.sort() == SortChain{{.column = "c", .dir = kAsc}});
    REQUIRE(engine.columns().size() == 2);
    CHECK(engine.columns()[1].id == "d");  // the view shown is still the old one

    *armed = false;
    REQUIRE(engine.setSort({{.column = "c", .dir = kDesc}}).has_value());
    CHECK(engine.columns()[1].id == "e");
    CHECK(viewKeys(engine) == keys({0, 1}));
}

// Mutation: Engine::ownerStep not reporting progress: a frame-budgeted job
// without a worker tells the sink nothing.
TEST_CASE("table: a progress sink hears from every owner step", "[table][pending]") {
    struct Recorder final : ProgressSink {
        std::vector<std::pair<std::size_t, std::size_t>> calls;
        void progress(std::size_t done, std::size_t total) override { calls.emplace_back(done, total); }
    };
    auto const recorder = std::make_shared<Recorder>();
    ManualExecutor owner;
    Services services;
    services.progress = recorder;
    Engine engine{
        tabletest::syntheticSource(3000, 61),
        EngineOptions{
            .owner = &owner, .worker = nullptr, .frameBudget = std::chrono::microseconds{1}, .services = services}};
    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());
    drain(owner);
    CHECK_FALSE(engine.pending());
    // One report per step; applying the result counts as one more owner step.
    REQUIRE(recorder->calls.size() + 1 == engine.stats().ownerSteps);
    REQUIRE(recorder->calls.size() > 1);
    CHECK(std::ranges::is_sorted(recorder->calls));
    CHECK(recorder->calls.back() == std::pair<std::size_t, std::size_t>{3, 3});
}

// Mutation: Engine::afterJob dropping the error of the work it started: a job
// dropped for a reset that shrank the table recomputes inside afterJob, and
// that recompute's failure is reported nowhere.
TEST_CASE("table: work started after a dropped job reports its failure", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const armed = std::make_shared<bool>(false);
    EngineOptions options{.owner = &owner, .worker = &worker};
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    auto const source = customColumn(3000);
    Engine engine{source, options};
    std::vector<TableError> errors;
    engine.onError([&](TableError const& error) { errors.push_back(error); });
    std::vector<bool> pendingSeen;
    engine.onPending([&](bool pending) { pendingSeen.push_back(pending); });

    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}}).has_value());  // a job on the worker
    std::vector<std::vector<Cell>> small;
    small.reserve(10);
    for (std::int64_t i = 0; i < 10; ++i) {
        small.push_back({Cell{10 - i}});
    }
    source->setRows(keys({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), std::move(small));
    owner.runAll();  // the flush stops the job: its rows are gone
    *armed = true;
    drain(owner, &worker);

    CHECK(engine.stats().dropped == 1);
    CHECK_FALSE(engine.pending());
    CHECK(pendingSeen == std::vector<bool>{true, false});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == TableErrorCode::ServiceFailed);
    CHECK(engine.sort().empty());
    CHECK(engine.sourceRowCount() == 3000);
}

// Mutation: Engine::afterJob laying out released holds only after a
// successful job: a release made while a job ran that then failed is never
// shown, and the released row stays at its old place.
TEST_CASE("table: holds released while a job runs are laid out when it fails", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const armed = std::make_shared<bool>(false);
    EngineOptions options{.owner = &owner,
                          .worker = &worker,
                          .reorder = ReorderPolicy::Deferred,
                          .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}};
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    std::vector<std::vector<Cell>> cells;
    cells.reserve(3000);
    for (std::int64_t i = 0; i < 3000; ++i) {
        cells.push_back({Cell{i * 10}, Cell{3000 - i}});
    }
    auto const source = tabletest::makeSource({col("n", ColumnKind::Integer), col("c", ColumnKind::Custom, "byValue")},
                                              std::move(cells));
    Engine engine{source, options};
    FollowingModel const model{engine};
    std::vector<TableError> errors;
    engine.onError([&](TableError const& error) { errors.push_back(error); });
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);
    source->updateRow(0, {Cell{std::int64_t{999'999}}, Cell{std::int64_t{0}}});
    drain(owner, &worker);
    REQUIRE(held(engine, 0));
    REQUIRE(engine.rowIdAt(0) == key(0));

    *armed = true;
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    REQUIRE(engine.pending());
    engine.settleNow();  // waits for the job
    CHECK(engine.rowIdAt(0) == key(0));
    drain(owner, &worker);

    REQUIRE(errors.size() == 1);
    CHECK_FALSE(engine.pending());
    CHECK(engine.sort() == SortChain{{"n", kAsc}});
    CHECK(engine.heldRows().empty());
    CHECK(engine.rowIdAt(2999) == key(0));
    CHECK(model.check());
}

// A release made while a job runs that is then superseded waits for the job
// that replaces it, and is laid out with that job's result.
TEST_CASE("table: holds released while a superseded job runs are laid out with the next job", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source,
                  EngineOptions{.owner = &owner,
                                .worker = &worker,
                                .reorder = ReorderPolicy::Deferred,
                                .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}}};
    FollowingModel const model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);
    source->updateRow(0, numberedRow(99'999, "last"));
    drain(owner, &worker);
    REQUIRE(held(engine, 0));

    REQUIRE(engine.setFilter(nAtLeast("10")).has_value());
    engine.settleNow();                                     // waits for the filter job
    REQUIRE(engine.setFilter(nAtLeast("20")).has_value());  // which this one supersedes
    drain(owner, &worker);
    CHECK_FALSE(engine.pending());
    CHECK(engine.stats().dropped == 1);
    CHECK(engine.heldRows().empty());
    CHECK(engine.rowIdAt(engine.viewRowCount() - 1) == key(0));
    CHECK(engine.rowIdAt(0) == key(2));
    CHECK(model.check());
}

// Mutation: Engine::reportError calling the error handler without checking it is
// set: the empty std::function throws std::bad_function_call.
TEST_CASE("table: a failure with no error handler still ends pending", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const armed = std::make_shared<bool>(true);
    EngineOptions options{.owner = &owner, .worker = &worker};
    options.services.comparators.emplace("byValue", throwingComparator(armed));
    Engine engine{customColumn(3000), options};
    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}}).has_value());
    drain(owner, &worker);
    CHECK_FALSE(engine.pending());
    CHECK(engine.sort().empty());
}

// Mutation: Engine::deliver arming the settle timer without a scheduler:
// calling the empty std::function throws std::bad_function_call.
TEST_CASE("table: deferred reorder without a settle scheduler waits for settleNow", "[table][pending]") {
    ManualExecutor owner;
    auto const source = tabletest::column(ColumnKind::Integer, {std::int64_t{10}, std::int64_t{20}, std::int64_t{30}});
    Engine engine{source, EngineOptions{.owner = &owner, .reorder = ReorderPolicy::Deferred}};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    source->updateRow(0, {Cell{std::int64_t{99}}});
    drain(owner);
    CHECK(viewKeys(engine) == keys({0, 1, 2}));
    CHECK(held(engine, 0));
    engine.settleNow();
    CHECK(viewKeys(engine) == keys({1, 2, 0}));
}

// Only rows the view showed are held: a row an update brings into the filter
// has no place to keep, and goes where the sort puts it.
TEST_CASE("table: a row an update brings into the view is placed, not held", "[table][pending]") {
    ManualExecutor owner;
    auto const source = tabletest::column(ColumnKind::Integer, {std::int64_t{10}, std::int64_t{20}, std::int64_t{30}});
    Engine engine{source,
                  EngineOptions{.owner = &owner,
                                .reorder = ReorderPolicy::Deferred,
                                .scheduleSettle = [](std::chrono::milliseconds, std::function<void()> const&) {}}};
    FollowingModel const model{engine};
    FilterSpec below25;
    below25.columns["c"].include.push_back(FilterEntry{.lt = "25"});
    REQUIRE(engine.setFilter(below25).has_value());
    REQUIRE(engine.setSort({{"c", kDesc}}).has_value());
    REQUIRE(viewKeys(engine) == keys({1, 0}));
    source->updateRow(2, {Cell{std::int64_t{5}}});
    drain(owner);
    CHECK(viewKeys(engine) == keys({1, 0, 2}));
    CHECK(engine.heldRows().empty());
    CHECK(model.check());
}

// A full job that inherits a dropped repair's rows patches the keys it has
// and builds the ones it lacks across owner steps; the patched keys serve a
// later sort without a rebuild.
TEST_CASE("table: a dropped repair's rows are patched once by the job that follows", "[table][pending]") {
    ManualExecutor owner;
    auto const source = tabletest::syntheticSource(20000, 71);
    Engine engine{source,
                  EngineOptions{.owner = &owner, .worker = nullptr, .frameBudget = std::chrono::microseconds{1}}};
    FollowingModel const model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner);
    std::mt19937 rng{72};  // NOLINT(cert-msc32-c,cert-msc51-cpp) -- a fixed seed keeps the case reproducible
    source->updateRow(5, tabletest::syntheticRow(5, rng));
    owner.runAll();                                         // the flush starts the repair
    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());  // and this stops it
    drain(owner);

    CHECK(engine.stats().dropped == 1);
    CHECK(engine.stats().keyBuilds == 2);  // n's keys were patched, not rebuilt
    CHECK(model.check());
    Engine byName{source};
    REQUIRE(byName.setSort({{"name", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(byName));

    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner);
    Engine byN{source};
    REQUIRE(byN.setSort({{"n", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(byN));
}

// Mutation: Engine::canRepair ignoring the epoch: an update absorbed after a
// structural change, while the view still shows the old rows, would be
// repaired into an order of row indices that no longer exist.
TEST_CASE("table: updates after an unapplied structural change recompute in full", "[table][pending]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = numbered(3000);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel const model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    drain(owner, &worker);

    source->updateRow(5, numberedRow(-5, "moved"));
    owner.runAll();  // the repair starts
    source->insertRow(0, key(100'000), numberedRow(1'000'000, "inserted"));
    owner.runAll();  // the flush stops the repair
    source->updateRow(20, numberedRow(-20, "moved"));
    owner.runAll();  // an update while the stopped job is still out
    drain(owner, &worker);

    CHECK_FALSE(engine.pending());
    CHECK(engine.stats().dropped == 1);
    CHECK(engine.stats().repairs == 0);
    CHECK(model.check());
    Engine reference{source};
    REQUIRE(reference.setSort({{"n", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
    CHECK(engine.rowIdAt(0) == key(19));
}
