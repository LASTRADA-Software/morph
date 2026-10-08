// SPDX-License-Identifier: Apache-2.0
//
// morph::table::Engine and source changes (spec 7 §7): lazy repair of
// updates, structural changes, column changes and deferred reorder.
//
// Mutations these tests were seen to fail on:
//  - Engine::flushChanges always taking the full path (never Request::Repair):
//    "a thousand updates in one turn repair once, without a sort" fails.
//  - Engine::rowsChanged flushing synchronously instead of posting once per
//    owner turn: the same test fails (one repair per update).
//  - Engine::apply ignoring holds under Deferred: "deferred reorder holds an
//    updated row until the view settles" fails.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/executor.hpp>
#include <morph/table/engine.hpp>
#include <random>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::FollowingModel;
using tabletest::viewKeys;

namespace {

constexpr auto kAsc = SortDirection::Ascending;

std::vector<RowId> ids(std::initializer_list<std::int64_t> keys) {
    std::vector<RowId> out;
    for (auto const key : keys) {
        out.emplace_back(key);
    }
    return out;
}

// A settle scheduler the test fires by hand.
struct ManualSettle {
    std::vector<std::function<void()>> pending;
    std::function<void(std::chrono::milliseconds, std::function<void()>)> scheduler() {
        return [this](std::chrono::milliseconds, std::function<void()> fire) { pending.push_back(std::move(fire)); };
    }
    void fireAll() {
        auto const fires = std::move(pending);
        pending.clear();
        for (auto const& fire : fires) {
            fire();
        }
    }
};

}  // namespace

TEST_CASE("table: a thousand updates in one turn repair once, without a sort", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    auto source = tabletest::syntheticSource(5000, 11);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = nullptr}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"price", kAsc}}).has_value());
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));
    REQUIRE(engine.stats().fullSorts == 1);

    std::mt19937 rng{12};
    for (int i = 0; i < 1000; ++i) {
        auto const row = rng() % 5000;
        source->updateRow(row, tabletest::syntheticRow(static_cast<std::int64_t>(row), rng));
    }
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending() && engine.stats().repairs > 0; }));
    owner.drain();
    CHECK(engine.stats().repairs == 1);
    CHECK(engine.stats().fullSorts == 1);
    CHECK(model.check());

    Engine reference{source};
    REQUIRE(reference.setSort({{"price", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}

TEST_CASE("table: an updated row that moves is one move and one change", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    auto source = tabletest::column(ColumnKind::Integer,
                                    {std::int64_t{10}, std::int64_t{20}, std::int64_t{30}, std::int64_t{40}});
    Engine engine{source, EngineOptions{.owner = &owner}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    source->updateRow(0, {std::int64_t{35}});
    owner.drain();
    CHECK(viewKeys(engine) == ids({1, 2, 0, 3}));
    REQUIRE(model.last.ops.size() == 2);
    CHECK(model.last.ops[0].kind == ViewOp::Kind::Moved);
    CHECK(model.last.ops[1] == ViewOp{.kind = ViewOp::Kind::Changed, .first = 2, .count = 1, .to = 0});
    CHECK(model.check());
}

TEST_CASE("table: an update can take a row out of the filter or bring it in", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    auto source = tabletest::column(ColumnKind::Integer, {std::int64_t{1}, std::int64_t{5}, std::int64_t{9}});
    Engine engine{source, EngineOptions{.owner = &owner}};
    FollowingModel model{engine};
    FilterSpec spec;
    spec.columns.emplace("c",
                         ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.gt = "3"}}, .exclude = {}});
    REQUIRE(engine.setFilter(spec).has_value());
    CHECK(viewKeys(engine) == ids({1, 2}));
    source->updateRow(1, {std::int64_t{0}});
    source->updateRow(0, {std::int64_t{7}});
    owner.drain();
    CHECK(viewKeys(engine) == ids({0, 2}));
    CHECK(model.check());
}

TEST_CASE("table: inserted and removed rows recompute the view by key", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    auto source = tabletest::syntheticSource(4000, 13);
    Engine engine{source, EngineOptions{.owner = &owner}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));
    std::mt19937 rng{14};
    source->insertRow(10, RowId{std::int64_t{100000}}, tabletest::syntheticRow(100000, rng));
    source->removeRow(3000);
    source->removeRow(5);
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending() && engine.sourceRowCount() == 3999; }));
    CHECK(model.check());
    CHECK(model.resets == 0);
    Engine reference{source};
    REQUIRE(reference.setSort({{"name", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}

TEST_CASE("table: a reset keeps the sort and filter entries whose columns remain", "[table][changes]") {
    auto source =
        tabletest::makeSource({tabletest::col("a", ColumnKind::Integer), tabletest::col("b", ColumnKind::Text)},
                              {{std::int64_t{2}, std::string{"x"}}, {std::int64_t{1}, std::string{"y"}}});
    Engine engine{source};
    REQUIRE(engine.setSort({{"b", kAsc}, {"a", kAsc}}).has_value());
    FilterSpec spec;
    spec.columns.emplace("a",
                         ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.gt = "0"}}, .exclude = {}});
    spec.columns.emplace(
        "b", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.notEmpty = true}}, .exclude = {}});
    REQUIRE(engine.setFilter(spec).has_value());

    source->setRows({RowId{std::int64_t{0}}, RowId{std::int64_t{1}}, RowId{std::int64_t{2}}},
                    {{std::int64_t{3}}, {std::int64_t{-1}}, {std::int64_t{1}}},
                    std::vector<ColumnInfo>{tabletest::col("a", ColumnKind::Integer)});
    CHECK(engine.sort() == SortChain{{"a", kAsc}});
    CHECK(engine.filter().columns.size() == 1);
    CHECK(engine.filter().columns.contains("a"));
    CHECK(engine.columns().size() == 1);
    CHECK(viewKeys(engine) == ids({2, 0}));
}

TEST_CASE("table: deferred reorder holds an updated row until the view settles", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    ManualSettle settle;
    auto source = tabletest::column(ColumnKind::Integer, {std::int64_t{10}, std::int64_t{20}, std::int64_t{30}});
    Engine engine{
        source,
        EngineOptions{.owner = &owner, .reorder = ReorderPolicy::Deferred, .scheduleSettle = settle.scheduler()}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());

    source->updateRow(0, {std::int64_t{99}});
    owner.drain();
    CHECK(viewKeys(engine) == ids({0, 1, 2}));  // held in place
    CHECK(engine.heldRows().size() == 1);
    REQUIRE(model.last.ops.size() == 1);
    CHECK(model.last.ops[0].kind == ViewOp::Kind::Changed);

    // Another update before the interval ends restarts it: the first timer is stale.
    source->updateRow(1, {std::int64_t{98}});
    owner.drain();
    REQUIRE(settle.pending.size() == 2);
    auto const stale = settle.pending.front();
    stale();
    CHECK(viewKeys(engine) == ids({0, 1, 2}));

    settle.pending.erase(settle.pending.begin());
    settle.fireAll();
    CHECK(viewKeys(engine) == ids({2, 1, 0}));
    CHECK(engine.heldRows().empty());
    CHECK(model.check());
}

TEST_CASE("table: a row being edited keeps its place until its edit ends", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    ManualSettle settle;
    auto source = tabletest::column(ColumnKind::Integer, {std::int64_t{10}, std::int64_t{20}, std::int64_t{30}});
    Engine engine{
        source,
        EngineOptions{.owner = &owner, .reorder = ReorderPolicy::Deferred, .scheduleSettle = settle.scheduler()}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    engine.beginEdit(RowId{std::int64_t{0}});
    source->updateRow(0, {std::int64_t{25}});
    owner.drain();
    settle.fireAll();  // the interval passes, but the row is still being edited
    CHECK(viewKeys(engine) == ids({0, 1, 2}));
    engine.endEdit(RowId{std::int64_t{0}});
    CHECK(viewKeys(engine) == ids({1, 0, 2}));
    REQUIRE(model.last.ops.size() == 1);
    CHECK(model.last.ops[0].kind == ViewOp::Kind::Moved);
    CHECK(model.check());
}

TEST_CASE("table: immediate reorder moves an updated row at once", "[table][changes]") {
    auto source = tabletest::column(ColumnKind::Integer, {std::int64_t{10}, std::int64_t{20}, std::int64_t{30}});
    Engine engine{source};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    engine.beginEdit(RowId{std::int64_t{0}});
    source->updateRow(0, {std::int64_t{99}});
    CHECK(viewKeys(engine) == ids({1, 2, 0}));
}

TEST_CASE("table: updates during a worker job recompute once the job ends", "[table][changes]") {
    morph::exec::MainThreadExecutor owner;
    tabletest::ManualExecutor worker;
    auto source = tabletest::syntheticSource(3000, 15);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    std::mt19937 rng{16};
    source->updateRow(7, tabletest::syntheticRow(7, rng));
    owner.drain();    // the flush finds a job running: it queues a recompute
    worker.runAll();  // the first job was stopped
    owner.drain();
    worker.runAll();
    owner.drain();
    CHECK_FALSE(engine.pending());
    CHECK(model.check());
    Engine reference{source};
    REQUIRE(reference.setSort({{"n", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}
