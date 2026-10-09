// SPDX-License-Identifier: Apache-2.0
//
// Calling back into morph::table, or destroying its objects, from their own
// callbacks (spec 7 §8): a source's listeners, an engine's handlers, and a
// CellEdits' commit and patch. The rule under test: each object calls out
// last, or checks that it is still alive before it goes on.
//
// Each case names the mutation it was seen to fail on. The cases that
// destroy an object from its own callback fail as a heap-use-after-free under
// the clang-asan preset; a plain build may pass them by luck.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/table/edits.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

// Reflected by glaze, so not in an anonymous namespace.
namespace treent {
struct IdRow {
    std::int64_t id = 0;
};
}  // namespace treent

using namespace morph::table;
using tabletest::col;
using tabletest::drain;
using tabletest::ManualExecutor;

namespace {

RowId key(std::int64_t value) { return RowId{value}; }

std::shared_ptr<VectorSource> integers(std::size_t rows) {
    std::vector<std::vector<Cell>> cells;
    cells.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        cells.push_back({Cell{static_cast<std::int64_t>((i * 7919) % rows)}});
    }
    return tabletest::makeSource({col("n", ColumnKind::Integer)}, std::move(cells));
}

struct CountingListener final : ChangeListener {
    std::function<void()> onChange;
    int calls = 0;
    void rowsChanged(RowChange const& /*change*/) override {
        ++calls;
        if (onChange) {
            onChange();
        }
    }
};

}  // namespace

// Mutation: detail::Listeners::notify calling every listener of its copy
// without looking it up again: the unsubscribed listener is called.
TEST_CASE("table: a listener unsubscribed during a notification is not called", "[table][reentrancy]") {
    auto const source = integers(3);
    CountingListener first;
    CountingListener second;
    source->subscribe(first);
    source->subscribe(second);
    first.onChange = [&] { source->unsubscribe(second); };
    source->updateRow(0, {Cell{std::int64_t{9}}});
    CHECK(first.calls == 1);
    CHECK(second.calls == 0);
    source->unsubscribe(first);
}

// Mutation: as above; the destroyed engine is called through the copy.
TEST_CASE("table: an engine destroyed by another listener's notification is not called", "[table][reentrancy]") {
    auto const source = integers(3);
    Engine master{source};
    auto detail = std::make_unique<Engine>(source);
    master.onViewChange([&](ViewChange const&) { detail.reset(); });
    REQUIRE(master.setSort({{"n", SortDirection::Ascending}}).has_value());
    source->updateRow(0, {Cell{std::int64_t{100}}});
    CHECK(detail == nullptr);

    using Row = treent::IdRow;
    auto const rows =
        std::make_shared<RowsSource<Row>>(std::make_shared<std::vector<Row> const>(std::vector<Row>{{1}, {2}}),
                                          [](Row const& row) { return RowId{row.id}; });
    Engine first{rows};
    auto second = std::make_unique<Engine>(rows);
    first.onViewChange([&](ViewChange const&) { second.reset(); });
    rows->setRows(std::make_shared<std::vector<Row> const>(std::vector<Row>{{2}, {3}}));
    CHECK(second == nullptr);
}

// Mutation: Engine::finish recording the owner step and ending pending()
// after publishing (the order before the fix): it writes the destroyed engine.
TEST_CASE("table: an engine may be destroyed from its own view-change handler", "[table][reentrancy]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = integers(3000);
    bool const withWorker = GENERATE(false, true);
    auto engine =
        std::make_unique<Engine>(source, EngineOptions{.owner = &owner, .worker = withWorker ? &worker : nullptr});
    engine->onViewChange([&](ViewChange const&) { engine.reset(); });
    REQUIRE(engine->setSort({{"n", SortDirection::Ascending}}).has_value());
    drain(owner, &worker);
    CHECK(engine == nullptr);
}

// Mutation: Engine::start calling setPending(true) before it builds the job's
// input: the handler destroyed the engine, and start goes on with it.
TEST_CASE("table: an engine may be destroyed from its own pending handler", "[table][reentrancy]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = integers(3000);
    auto engine = std::make_unique<Engine>(source, EngineOptions{.owner = &owner, .worker = &worker});
    bool const atEnd = GENERATE(false, true);
    engine->onPending([&](bool pending) {
        if (pending != atEnd) {
            engine.reset();
        }
    });
    REQUIRE(engine->setSort({{"n", SortDirection::Ascending}}).has_value());
    drain(owner, &worker);
    CHECK(engine == nullptr);
}

// Mutation: Engine::finish ending pending() after publishing whatever happened
// in the handler (no `!_running` check): the job the handler started runs
// with pending() false.
TEST_CASE("table: a job started from the view-change handler keeps pending true", "[table][reentrancy]") {
    ManualExecutor owner;
    ManualExecutor worker;
    auto const source = integers(3000);
    Engine engine{source, EngineOptions{.owner = &owner, .worker = &worker}};
    std::vector<bool> pendingSeen;
    engine.onPending([&](bool pending) { pendingSeen.push_back(pending); });
    bool refiltered = false;
    bool pendingAfterHandler = false;
    engine.onViewChange([&](ViewChange const&) {
        if (!refiltered) {
            refiltered = true;
            FilterSpec spec;
            spec.columns["n"].include.push_back(FilterEntry{.ge = "100"});
            REQUIRE(engine.setFilter(spec).has_value());
        }
    });
    REQUIRE(engine.setSort({{"n", SortDirection::Ascending}}).has_value());
    while (owner.runAll() + worker.runAll() > 0) {
        if (refiltered && engine.viewRowCount() == 3000) {
            pendingAfterHandler = engine.pending();
        }
    }
    CHECK(pendingAfterHandler);
    CHECK(pendingSeen == std::vector<bool>{true, false});
    CHECK(engine.viewRowCount() == 2900);
}

namespace {

struct EditSetup {
    std::shared_ptr<VectorSource> source = integers(5);
    Engine engine{source};
    std::vector<EditRequest> requests;
    std::vector<EditDone> pending;
    std::unique_ptr<CellEdits> edits;

    explicit EditSetup(EditCommit commit = {}) {
        if (!commit) {
            commit = [this](EditRequest const& request, EditDone done) {
                requests.push_back(request);
                pending.push_back(std::move(done));
            };
        }
        edits = std::make_unique<CellEdits>(*source, std::move(commit), &engine);
    }
};

}  // namespace

// Mutation: CellEdits::start handing the commit a reference into its queue
// (`auto const& request = ...front()`): a commit that settles at once pops
// that element, and reading the request afterwards reads freed memory.
TEST_CASE("table: a commit may read its request after settling it at once", "[table][reentrancy]") {
    std::vector<std::string> seen;
    EditSetup setup{[&](EditRequest const& request, EditDone done) {
        done(std::vector<Cell>{Cell{std::int64_t{42}}});
        seen.push_back(request.column + " of a long enough name to live on the heap");
    }};
    setup.edits->commit(EditRequest{.row = key(1), .column = "n", .value = Cell{std::int64_t{42}}});
    setup.edits->commit(EditRequest{.row = key(1), .column = "n", .value = Cell{std::int64_t{43}}});
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == "n of a long enough name to live on the heap");
    CHECK(setup.edits->unsettled() == 0);
}

// Mutation: CellEdits::settle deciding whether the row has more edits after
// it patched the row (the order before the fix): the edit committed from the
// view-change handler is started twice.
TEST_CASE("table: an edit committed while its row is patched starts once", "[table][reentrancy]") {
    EditSetup setup;
    bool again = true;
    setup.engine.onViewChange([&](ViewChange const&) {
        if (again) {
            again = false;
            setup.edits->commit(EditRequest{.row = key(1), .column = "n", .value = Cell{std::int64_t{7}}});
        }
    });
    setup.edits->commit(EditRequest{.row = key(1), .column = "n", .value = Cell{std::int64_t{6}}});
    REQUIRE(setup.pending.size() == 1);
    setup.pending[0](std::vector<Cell>{Cell{std::int64_t{100}}});  // moves the row: a change is published
    CHECK(setup.requests.size() == 2);
    CHECK(setup.edits->unsettled() == 1);
    CHECK(setup.edits->stale(key(1), "n"));
}

// Mutation: CellEdits::settle going on after the patch without checking that
// it is still alive: it erases from the destroyed queues. And with the row's
// last edit not remembered while it is patched (`_ending`), the destructor
// does not end it, and the row stays held as edited.
TEST_CASE("table: cell edits may be destroyed while their row is patched", "[table][reentrancy]") {
    auto const source = integers(5);
    Engine engine{source, EngineOptions{.reorder = ReorderPolicy::Deferred,
                                        .scheduleSettle = [](std::chrono::milliseconds, std::function<void()>) {}}};
    std::vector<EditDone> pending;
    auto edits = std::make_unique<CellEdits>(
        *source, [&](EditRequest const&, EditDone done) { pending.push_back(std::move(done)); }, &engine);
    engine.onViewChange([&](ViewChange const&) { edits.reset(); });
    edits->commit(EditRequest{.row = key(1), .column = "n", .value = Cell{std::int64_t{6}}});
    REQUIRE(pending.size() == 1);
    pending[0](std::vector<Cell>{Cell{std::int64_t{100}}});
    CHECK(edits == nullptr);
    engine.settleNow();  // releases every held row not being edited
    CHECK(engine.heldRows().empty());
}

// Mutation: ~CellEdits defaulted (no endEdit for rows in flight): the row
// stays held, and every later repair holds it again.
TEST_CASE("table: destroying cell edits ends the edits they had in flight", "[table][reentrancy]") {
    auto const source = integers(5);
    Engine engine{source, EngineOptions{.reorder = ReorderPolicy::Deferred,
                                        .scheduleSettle = [](std::chrono::milliseconds, std::function<void()>) {}}};
    REQUIRE(engine.setSort({{"n", SortDirection::Ascending}}).has_value());
    std::vector<EditDone> pending;
    auto edits = std::make_unique<CellEdits>(
        *source, [&](EditRequest const&, EditDone done) { pending.push_back(std::move(done)); }, &engine);
    edits->commit(EditRequest{.row = key(2), .column = "n", .value = Cell{std::int64_t{1000}}});
    source->updateRow(2, {Cell{std::int64_t{1000}}});  // a push lands while the edit is in flight
    REQUIRE(engine.heldRows().size() == 1);

    edits.reset();
    CHECK(engine.heldRows().empty());
    CHECK(engine.rowIdAt(4) == key(2));
    pending.at(0)(std::vector<Cell>{Cell{std::int64_t{1000}}});  // ignored
    source->updateRow(0, {Cell{std::int64_t{-1}}});
    CHECK(engine.heldRows() == std::vector<RowId>{key(0)});
}
