// SPDX-License-Identifier: Apache-2.0
//
// morph::table::Engine execution (spec 7 §3, §7, §8): the view on and off the
// owner, supersession, pending, the small-table path and the sort cache.
//
// Mutations these tests were seen to fail on:
//  - Engine::finish applying a completed result although a newer request
//    was queued (drop the `_queued ||` condition): "a sort superseded after
//    its job finished is still dropped" fails.
//  - Engine::start not calling setPending(true): "pending brackets
//    asynchronous work" and the supersession tests fail.
//  - Engine::makeInput never passing the cached sorted order: "a filter
//    change filters the cached sorted order" fails.
//  - ViewJob skipping the filter when a cached order exists: "sort and
//    filter map view rows and source rows both ways" and the cache test fail.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/executor.hpp>
#include <morph/table/engine.hpp>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::FollowingModel;
using tabletest::ManualExecutor;
using tabletest::viewKeys;

namespace {

constexpr auto kAsc = SortDirection::Ascending;
constexpr auto kDesc = SortDirection::Descending;

FilterSpec filterOn(std::string column, FilterEntry entry) {
    FilterSpec spec;
    spec.columns.emplace(std::move(column),
                         ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    return spec;
}

// The scenarios the equivalence test runs: (sort, filter).
std::vector<std::pair<SortChain, FilterSpec>> scenarios() {
    FilterSpec group;
    group.groups.push_back(
        GroupFilter{.columns = {"name", "n"}, .combine = Combine::Any, .include = {FilterEntry{.contains = "ta"}}});
    return {
        {{{.column = "n", .dir = kAsc}}, {}},
        {{{.column = "name", .dir = kDesc}}, {}},
        {{{.column = "on", .dir = kAsc}, {.column = "price", .dir = kDesc}, {.column = "mass", .dir = kAsc}}, {}},
        {{{.column = "mass", .dir = kAsc}}, filterOn("price", FilterEntry{.ge = "250.5"})},
        {{}, filterOn("name", FilterEntry{.contains = "ETA"})},
        {{{.column = "price", .dir = kAsc}}, group},
        {{{.column = "n", .dir = kDesc}},
         filterOn("on", FilterEntry{.between = std::array<std::string, 2>{"2023-06-01", "2024-12-31"}})},
    };
}

}  // namespace

TEST_CASE("table: the initial view is every row in source order", "[table][engine]") {
    auto const source = tabletest::column(ColumnKind::Integer, {std::int64_t{3}, std::int64_t{1}, std::int64_t{2}});
    Engine const engine{source};
    CHECK(engine.viewRowCount() == 3);
    CHECK(engine.sourceRowCount() == 3);
    CHECK(engine.sourceRowOf(1) == 1U);
    CHECK(engine.sourceRowOf(3) == std::nullopt);
    CHECK_FALSE(engine.pending());
}

TEST_CASE("table: sort and filter map view rows and source rows both ways", "[table][engine]") {
    auto const source = tabletest::column(ColumnKind::Integer,
                                          {std::int64_t{30}, std::int64_t{10}, std::int64_t{20}, std::int64_t{40}});
    Engine engine{source};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    CHECK(engine.sourceRowOf(0) == 1U);
    CHECK(engine.viewRowOf(0) == 2U);
    CHECK(std::get<std::int64_t>(engine.cellAt(3, 0)) == 40);
    REQUIRE(engine.setFilter(filterOn("c", FilterEntry{.lt = "35"})).has_value());
    CHECK(engine.viewRowCount() == 3);
    CHECK(engine.viewRowOf(3) == std::nullopt);
    CHECK(engine.viewRowOfKey(RowId{std::int64_t{0}}) == 2U);
    CHECK(engine.viewRowOfKey(RowId{std::int64_t{3}}) == std::nullopt);
    CHECK(model.check());
    CHECK(model.changes() == 2);
}

TEST_CASE("table: setSort and setFilter refuse what they cannot apply and keep the old state", "[table][engine]") {
    auto const source = tabletest::column(ColumnKind::Integer, {std::int64_t{1}});
    Engine engine{source};
    REQUIRE(engine.setSort({{"c", kDesc}}).has_value());
    auto const badSort = engine.setSort({{.column = "nope", .dir = kAsc}});
    REQUIRE_FALSE(badSort.has_value());
    CHECK(badSort.error().code == TableErrorCode::UnknownColumn);
    CHECK(engine.sort() == SortChain{{"c", kDesc}});
    auto const badFilter = engine.setFilter(filterOn("c", FilterEntry{.gt = "x"}));
    REQUIRE_FALSE(badFilter.has_value());
    CHECK(badFilter.error().column == "c");
    CHECK(engine.filter().empty());
}

TEST_CASE("table: a small table computes on the owner in one step", "[table][engine]") {
    morph::exec::MainThreadExecutor owner;
    ManualExecutor worker;
    auto const source = tabletest::syntheticSource(500, 1);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    std::vector<bool> pendings;
    engine.onPending([&](bool value) { pendings.push_back(value); });
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    CHECK_FALSE(engine.pending());
    CHECK(pendings.empty());
    CHECK(worker.queued() == 0);
    CHECK_FALSE(owner.runOnce());
    CHECK(engine.stats().ownerSteps == 1);
    CHECK(engine.stats().fullSorts == 1);
}

TEST_CASE("table: pending brackets asynchronous work and the old view stays meanwhile", "[table][engine]") {
    morph::exec::MainThreadExecutor owner;
    ManualExecutor worker;
    auto const source = tabletest::syntheticSource(3000, 2);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel model{engine};
    std::vector<bool> pendings;
    engine.onPending([&](bool value) { pendings.push_back(value); });
    auto const before = viewKeys(engine);

    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());
    CHECK(engine.pending());
    CHECK(pendings == std::vector<bool>{true});
    CHECK(viewKeys(engine) == before);
    CHECK(model.changes() == 0);

    CHECK(worker.runAll() == 1);
    CHECK(engine.pending());  // the result is applied on the owner, not on the worker
    owner.drain();
    CHECK_FALSE(engine.pending());
    CHECK(pendings == std::vector<bool>{true, false});
    CHECK(model.changes() == 1);
    CHECK(model.check());
}

TEST_CASE("table: a superseded sort stops and its result is dropped", "[table][engine]") {
    morph::exec::MainThreadExecutor owner;
    ManualExecutor worker;
    auto const source = tabletest::syntheticSource(3000, 3);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel model{engine};

    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    REQUIRE(engine.setSort({{"price", kDesc}}).has_value());  // supersedes the first
    CHECK(worker.queued() == 1);                              // one job at a time
    worker.runAll();                                          // the first job sees its stop request
    owner.drain();                                            // drops it and starts the second
    CHECK(engine.stats().dropped == 1);
    CHECK(model.changes() == 0);
    CHECK(engine.pending());
    CHECK(worker.runAll() == 1);
    owner.drain();
    CHECK_FALSE(engine.pending());
    CHECK(model.changes() == 1);
    CHECK(model.check());

    Engine reference{source};
    REQUIRE(reference.setSort({{"price", kDesc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}

TEST_CASE("table: a sort superseded after its job finished is still dropped", "[table][engine]") {
    morph::exec::MainThreadExecutor owner;
    ManualExecutor worker;
    auto const source = tabletest::syntheticSource(3000, 31);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    FollowingModel model{engine};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    worker.runAll();  // the job completes; its result waits on the owner
    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());
    owner.drain();
    CHECK(engine.stats().dropped == 1);
    CHECK(model.changes() == 0);
    worker.runAll();
    owner.drain();
    CHECK(model.changes() == 1);
    CHECK(model.check());
    Engine reference{source};
    REQUIRE(reference.setSort({{"name", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}

TEST_CASE("table: an engine destroyed with a job in flight drops the job's result", "[table][engine]") {
    morph::exec::MainThreadExecutor owner;
    ManualExecutor worker;
    auto const source = tabletest::syntheticSource(3000, 4);
    std::size_t changes = 0;
    {
        Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
        engine.onViewChange([&](ViewChange const&) { ++changes; });
        REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    }
    worker.runAll();
    owner.drain();
    CHECK(changes == 0);
}

TEST_CASE("table: results are identical on the owner, in owner steps and on a worker", "[table][engine]") {
    auto const source = tabletest::syntheticSource(6000, 5);
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{2};
    for (auto const& [sort, filter] : scenarios()) {
        Engine oneStep{source};
        Engine stepped{source,
                       EngineOptions{.owner = &owner, .worker = nullptr, .frameBudget = std::chrono::microseconds{1}}};
        Engine threaded{source, EngineOptions{.owner = &owner, .worker = &pool}};
        for (Engine* engine : {&oneStep, &stepped, &threaded}) {
            REQUIRE(engine->setSort(sort).has_value());
            REQUIRE(engine->setFilter(filter).has_value());
        }
        REQUIRE(tabletest::pumpUntil(owner, [&] { return !stepped.pending() && !threaded.pending(); }));
        auto const expected = viewKeys(oneStep);
        CHECK(viewKeys(stepped) == expected);
        CHECK(viewKeys(threaded) == expected);
        CHECK(stepped.stats().ownerSteps > 1);
    }
}

TEST_CASE("table: owner steps yield to the frame budget", "[table][engine]") {
    morph::exec::MainThreadExecutor owner;
    auto const source = tabletest::syntheticSource(20000, 6);
    Engine engine{source,
                  EngineOptions{.owner = &owner, .worker = nullptr, .frameBudget = std::chrono::microseconds{1}}};
    REQUIRE(engine.setSort({{"name", kAsc}, {"price", kDesc}}).has_value());
    CHECK(engine.stats().ownerSteps == 0);
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));
    // A 1 us budget yields at every chunk boundary: two key columns of ten
    // 2,048-row chunks each, then the sort passes. How long a real step takes
    // is what the [.benchmark] run measures in a Release build.
    CHECK(engine.stats().ownerSteps > 20);
}

TEST_CASE("table: a filter change filters the cached sorted order without re-sorting", "[table][engine]") {
    auto const source = tabletest::syntheticSource(3000, 7);
    Engine engine{source};
    REQUIRE(engine.setSort({{"price", kAsc}}).has_value());
    CHECK(engine.stats().fullSorts == 1);
    REQUIRE(engine.setFilter(filterOn("n", FilterEntry{.gt = "0"})).has_value());
    {
        // Filtering the cached order gives what filtering first and sorting the survivors gives.
        Engine filterFirst{source};
        REQUIRE(filterFirst.setFilter(filterOn("n", FilterEntry{.gt = "0"})).has_value());
        REQUIRE(filterFirst.setSort({{"price", kAsc}}).has_value());
        CHECK(filterFirst.stats().subsetSorts == 1);
        CHECK(viewKeys(engine) == viewKeys(filterFirst));
    }
    REQUIRE(engine.setFilter(filterOn("n", FilterEntry{.lt = "0"})).has_value());
    REQUIRE(engine.setFilter({}).has_value());
    CHECK(engine.stats().fullSorts == 1);
    CHECK(engine.stats().subsetSorts == 0);

    // A sort change with a filter active sorts only the survivors.
    REQUIRE(engine.setFilter(filterOn("n", FilterEntry{.gt = "400"})).has_value());
    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());
    CHECK(engine.stats().fullSorts == 1);
    CHECK(engine.stats().subsetSorts == 1);

    Engine reference{source};
    REQUIRE(reference.setFilter(filterOn("n", FilterEntry{.gt = "400"})).has_value());
    REQUIRE(reference.setSort({{"name", kAsc}}).has_value());
    CHECK(viewKeys(engine) == viewKeys(reference));
}

TEST_CASE("table: keys are built once per column and reused", "[table][engine]") {
    auto const source = tabletest::syntheticSource(1000, 8);
    Engine engine{source};
    REQUIRE(engine.setSort({{"price", kAsc}}).has_value());
    REQUIRE(engine.setSort({{"price", kDesc}}).has_value());
    REQUIRE(engine.setFilter(filterOn("price", FilterEntry{.gt = "1"})).has_value());
    CHECK(engine.stats().keyBuilds == 1);
    REQUIRE(engine.setSort({{"name", kAsc}}).has_value());
    CHECK(engine.stats().keyBuilds == 2);
}

TEST_CASE("table: a progress sink hears from a worker job on the owner", "[table][engine]") {
    struct Recorder final : ProgressSink {
        std::vector<std::pair<std::size_t, std::size_t>> calls;
        void progress(std::size_t done, std::size_t total) override { calls.emplace_back(done, total); }
    };
    auto const recorder = std::make_shared<Recorder>();
    morph::exec::MainThreadExecutor owner;
    ManualExecutor worker;
    Services services;
    services.progress = recorder;
    Engine engine{tabletest::syntheticSource(3000, 9),
                  EngineOptions{.owner = &owner, .worker = &worker, .services = services}};
    REQUIRE(engine.setSort({{"n", kAsc}}).has_value());
    worker.runAll();
    CHECK(recorder->calls.empty());
    owner.drain();
    // One call per phase (keys, order, diff). Mutation: ViewJob::run not
    // calling its phase listener, so a worker job reports nothing.
    REQUIRE(recorder->calls.size() == 3);
    CHECK(recorder->calls[0] == std::pair<std::size_t, std::size_t>{1, 3});
    CHECK(recorder->calls.back().first == recorder->calls.back().second);
}

TEST_CASE("table: a column layout keeps one column visible and resets", "[table][engine]") {
    ColumnLayout layout{{"a", "b", "c"}};
    CHECK(layout.setVisible("a", false));
    CHECK(layout.setVisible("b", false));
    CHECK_FALSE(layout.setVisible("c", false));
    CHECK(layout.visible() == std::vector<std::string>{"c"});
    CHECK_FALSE(layout.setVisible("zzz", true));
    CHECK(layout.move("c", 0));
    CHECK(layout.order()[0] == "c");
    layout.reset();
    CHECK(layout.visible() == std::vector<std::string>{"a", "b", "c"});

    CHECK(layout.setVisible("b", false));
    CHECK(layout.move("c", 0));
    layout.rebuild({"b", "c", "d"});
    CHECK(std::vector<std::string>(layout.order().begin(), layout.order().end()) ==
          std::vector<std::string>{"c", "b", "d"});
    CHECK(layout.visible() == std::vector<std::string>{"c", "d"});
}
