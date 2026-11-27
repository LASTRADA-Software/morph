// SPDX-License-Identifier: Apache-2.0
//
// morph::table selection, activation and cell edits (spec 7 §10, §11).
//
// Mutations these tests were seen to fail on:
//  - Selection::viewChanged pruning without pruneOnFilter: "selection
//    survives sort, filter and refetch" fails. (Activation cannot select by
//    construction: Selection::activate is static and reaches no selection.)
//  - CellEdits::commit starting every edit at once instead of after the row's
//    earlier edits: "edits to one row run in order" fails.
//  - CellEdits::settle not patching the row on success: "a commit runs the
//    mutation and patches the row" fails.
//  - Engine holding a dirty row whose edit just ended (drop the
//    _releaseOnRepair check): "under deferred reorder, a committed edit moves
//    its row when the commit settles" fails.
//  - Each "view selection:" case names, after it, the mutation it was seen to
//    fail on.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <iterator>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/table/selection.hpp>
#include <morph/table/view_cache.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::viewKeys;

namespace seltest {

// Ten rows, keys 0..9: status "open" for 0..5, "closed" for 6..9; mass i/10.
struct SelectionModel {
    std::shared_ptr<VectorSource> source = tabletest::makeSource(
        {tabletest::col("status", ColumnKind::Text), tabletest::col("mass", ColumnKind::Decimal)}, [] {
            std::vector<std::vector<Cell>> rows;
            for (int i = 0; i < 10; ++i) {
                // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
                rows.push_back(
                    {Cell{std::string{i < 6 ? "open" : "closed"}}, Cell{tabletest::dec(std::to_string(i) + ".0")}});
            }
            return rows;
        }());
    QueryLimits limits{};
    // The caller's rows when set, through table::scoped; every row otherwise.
    std::optional<std::vector<RowId>> scope;
    [[nodiscard]] ServedTable rows(TableContext const& /*context*/) {  // non-const, as the spec's table functions
        std::shared_ptr<DataSource const> served = source;
        if (scope) {
            served = scoped(source, *scope);
        }
        return ServedTable{.description = {},
                           .source = served,
                           .levels = {},
                           .mayRead = {},
                           .scope = {},
                           .services = {},
                           .limits = limits};
    }
};

// A row-scoped table whose function names no scope: what the registration says it must.
struct UnscopedModel {
    SelectionModel inner;
    [[nodiscard]] ServedTable rows(TableContext const& context) { return inner.rows(context); }
};

}  // namespace seltest

BRIDGE_REGISTER_MODEL(seltest::SelectionModel, "seltest::SelectionModel")
BRIDGE_REGISTER_TABLE(seltest::SelectionModel, "rows", &seltest::SelectionModel::rows)
BRIDGE_REGISTER_MODEL(seltest::UnscopedModel, "seltest::UnscopedModel")
BRIDGE_REGISTER_TABLE(seltest::UnscopedModel, "unscopedRows", &seltest::UnscopedModel::rows, morph::table::rowScoped)

namespace {

constexpr auto kAsc = SortDirection::Ascending;

RowId key(std::int64_t value) { return RowId{value}; }

std::shared_ptr<VectorSource> fiveRows() {
    return tabletest::column(ColumnKind::Integer, {std::int64_t{50}, std::int64_t{10}, std::int64_t{40},
                                                   std::int64_t{20}, std::int64_t{30}});
}

Selection make(SelectionOptions options) {
    auto selection = Selection::create(options);
    REQUIRE(selection.has_value());
    return std::move(*selection);
}

using seltest::SelectionModel;

FilterSpec open() { return FilterSpec{.columns = {{"status", ColumnFilter{.include = {{.eq = "open"}}}}}}; }

// TableContext holds a reference: one caller context outlives every call in the file.
TableContext const& caller() {
    static morph::session::Context const session{};
    static TableContext const context{.session = session};
    return context;
}

// resolve with no TableRuntime settles on its own inline owner before it returns: `then` runs at once.
template <typename T>
T settledNow(morph::async::Completion<T>& completion) {
    std::optional<T> out;
    completion.then([&out](T const& value) { out = value; });
    REQUIRE(out.has_value());
    return std::move(*out);
}

// The code of the error a settled completion holds; a value fails the case.
template <typename T>
TableErrorCode errorOf(morph::async::Completion<T>& completion) {
    auto const settled = settledNow(completion);
    REQUIRE_FALSE(settled.has_value());
    return settled.error().code;
}

std::string revisionOf(SelectionModel& model, FilterSpec const& filters, std::vector<RowId> retained = {}) {
    return detail::membership(model.rows(caller()), filters, retained, nullptr).value().digest.text();
}

std::vector<RowId> keysOf(std::initializer_list<std::int64_t> keys) {
    std::vector<RowId> out;
    for (auto const k : keys) {
        out.emplace_back(k);
    }
    return out;
}

}  // namespace

TEST_CASE("table: selection survives sort, filter and refetch", "[table][selection]") {
    auto const source = fiveRows();
    Engine engine{source};
    auto selection = make({.mode = SelectionMode::Multiple, .pruneOnFilter = false, .tableMode = TableMode::Client});
    CHECK(selection.select(key(0)));
    CHECK(selection.select(key(3)));
    selection.setActiveKey(key(3));

    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    selection.viewChanged(engine);
    FilterSpec spec;
    spec.columns.emplace("c",
                         ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.lt = "45"}}, .exclude = {}});
    REQUIRE(engine.setFilter(spec).has_value());
    selection.viewChanged(engine);
    CHECK(selection.contains(key(0)));  // filtered out, still selected
    CHECK(engine.viewRowOfKey(key(0)) == std::nullopt);
    CHECK(engine.viewRowOfKey(key(3)) == 1U);

    source->setRows({key(3), key(0), key(9)}, {{std::int64_t{1}}, {std::int64_t{2}}, {std::int64_t{3}}});
    selection.viewChanged(engine);
    CHECK(selection.keys() == std::vector<RowId>{key(0), key(3)});
    CHECK(selection.activeKey() == key(3));
    CHECK(engine.viewRowOfKey(key(3)) == 0U);
    auto const shown = counts(engine, selection);
    CHECK(shown.viewCount == 3);
    CHECK(shown.sourceCount == 3);
    CHECK(shown.selectedCount == 2);
}

TEST_CASE("table: pruneOnFilter drops selected rows that leave the view", "[table][selection]") {
    auto const source = fiveRows();
    Engine engine{source};
    auto selection = make({.mode = SelectionMode::Multiple, .pruneOnFilter = true, .tableMode = TableMode::Client});
    REQUIRE(selection.selectAll(engine).has_value());
    CHECK(selection.selectedCount() == 5);
    FilterSpec spec;
    spec.columns.emplace("c",
                         ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.ge = "30"}}, .exclude = {}});
    REQUIRE(engine.setFilter(spec).has_value());
    selection.viewChanged(engine);
    CHECK(selection.keys() == std::vector<RowId>{key(0), key(2), key(4)});
}

TEST_CASE("table: single and none modes", "[table][selection]") {
    auto single = make({.mode = SelectionMode::Single, .pruneOnFilter = false, .tableMode = TableMode::Client});
    CHECK(single.select(key(1)));
    CHECK(single.select(key(2)));
    CHECK(single.keys() == std::vector<RowId>{key(2)});
    CHECK_FALSE(single.toggle(key(2)));
    CHECK(single.selectedCount() == 0);
    Engine const engine{fiveRows()};
    CHECK(single.selectAll(engine).error().code == TableErrorCode::Unavailable);

    auto none = make({});
    CHECK_FALSE(none.select(key(1)));
    CHECK(none.selectedCount() == 0);
}

// Mutation: Selection::select taking its key by reference: in single mode it
// clears the storage the key refers to before it inserts it.
TEST_CASE("table: selecting the selected key again keeps it", "[table][selection]") {
    auto selection = Selection::create(SelectionOptions{.mode = SelectionMode::Single}).value();
    RowId const longKey{std::string{"a key long enough to be stored on the heap, not inline"}};
    REQUIRE(selection.select(longKey));
    REQUIRE(selection.select(selection.keys().front()));
    REQUIRE(selection.keys().size() == 1);
    CHECK(selection.keys().front() == longKey);
    CHECK(selection.contains(longKey));

    auto multiple = Selection::create(SelectionOptions{.mode = SelectionMode::Multiple}).value();
    multiple.select(RowId{std::string{"first of several long keys stored on the heap"}});
    multiple.select(RowId{std::string{"second of several long keys stored on the heap"}});
    multiple.select(RowId{std::string{"third of several long keys stored on the heap"}});
    REQUIRE(multiple.deselect(multiple.keys().front()));
    CHECK(multiple.keys().size() == 2);
    CHECK(multiple.selectedCount() == 2);
}

TEST_CASE("table: server mode has neither select-all nor pruning", "[table][selection]") {
    auto const pruning =
        Selection::create({.mode = SelectionMode::Multiple, .pruneOnFilter = true, .tableMode = TableMode::Server});
    REQUIRE_FALSE(pruning.has_value());
    CHECK(pruning.error().code == TableErrorCode::Unavailable);
    auto selection = make({.mode = SelectionMode::Multiple, .pruneOnFilter = false, .tableMode = TableMode::Server});
    Engine const engine{fiveRows()};
    CHECK(selection.selectAll(engine).error().code == TableErrorCode::Unavailable);
    CHECK(selection.select(key(1234)));  // a key outside any fetched page
    CHECK(selection.contains(key(1234)));
}

TEST_CASE("table: activation does not change the selection", "[table][selection]") {
    Engine engine{fiveRows()};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    auto selection = make({.mode = SelectionMode::Single, .pruneOnFilter = false, .tableMode = TableMode::Client});
    selection.select(key(0));
    std::vector<RowId> activated;
    CHECK(Selection::activate(engine, 0, [&](RowId const& id) { activated.push_back(id); }));
    CHECK_FALSE(Selection::activate(engine, 5, [&](RowId const& id) { activated.push_back(id); }));
    CHECK(activated == std::vector<RowId>{key(1)});
    CHECK(selection.keys() == std::vector<RowId>{key(0)});
}

TEST_CASE("table: a commit runs the mutation and patches the row", "[table][edits]") {
    auto const source = fiveRows();
    Engine engine{source};
    REQUIRE(engine.setSort({{"c", kAsc}}).has_value());
    std::vector<EditRequest> ran;
    std::vector<EditDone> pending;
    CellEdits edits{*source,
                    [&](EditRequest const& request, EditDone done) {
                        ran.push_back(request);
                        pending.push_back(std::move(done));
                    },
                    &engine};
    edits.commit(EditRequest{.row = key(1), .column = "c", .value = std::int64_t{60}});
    REQUIRE(ran.size() == 1);
    CHECK(edits.stale(key(1), "c"));
    CHECK(viewKeys(engine).front() == key(1));
    pending.front()(std::vector<Cell>{std::int64_t{60}});
    CHECK_FALSE(edits.stale(key(1), "c"));
    CHECK(edits.unsettled() == 0);
    CHECK(viewKeys(engine).back() == key(1));
    CHECK(std::get<std::int64_t>(engine.cellAt(4, 0)) == 60);
}

TEST_CASE("table: a field error attaches to its cell until the next edit succeeds", "[table][edits]") {
    auto const source = fiveRows();
    std::vector<EditDone> pending;
    CellEdits edits{*source, [&](EditRequest const&, EditDone done) { pending.push_back(std::move(done)); }};
    edits.commit(EditRequest{.row = key(2), .column = "c", .value = std::string{"lots"}});
    pending.back()(std::unexpected(FieldError{.column = "c", .message = "not a number"}));
    CHECK(edits.error(key(2), "c") == "not a number");
    CHECK_FALSE(edits.error(key(1), "c").has_value());
    CHECK(std::get<std::int64_t>(source->snapshot()->cell(2, 0)) == 40);
    edits.commit(EditRequest{.row = key(2), .column = "c", .value = std::int64_t{41}});
    pending.back()(std::vector<Cell>{std::int64_t{41}});
    CHECK_FALSE(edits.error(key(2), "c").has_value());
    CHECK(std::get<std::int64_t>(source->snapshot()->cell(2, 0)) == 41);
}

TEST_CASE("table: edits to one row run in order, other rows run alongside", "[table][edits]") {
    auto const source = fiveRows();
    std::vector<std::pair<EditRequest, EditDone>> calls;
    CellEdits edits{*source,
                    [&](EditRequest const& request, EditDone done) { calls.emplace_back(request, std::move(done)); }};
    edits.commit(EditRequest{.row = key(0), .column = "c", .value = std::int64_t{1}});
    edits.commit(EditRequest{.row = key(0), .column = "c", .value = std::int64_t{2}});
    edits.commit(EditRequest{.row = key(4), .column = "c", .value = std::int64_t{3}});
    REQUIRE(calls.size() == 2);  // the second edit of row 0 waits
    CHECK(std::get<std::int64_t>(calls[0].first.value) == 1);
    CHECK(calls[1].first.row == key(4));
    CHECK(edits.unsettled() == 3);
    calls[0].second(std::vector<Cell>{std::int64_t{1}});
    REQUIRE(calls.size() == 3);
    CHECK(std::get<std::int64_t>(calls[2].first.value) == 2);
    calls[2].second(std::vector<Cell>{std::int64_t{2}});
    calls[1].second(std::vector<Cell>{std::int64_t{3}});
    CHECK(std::get<std::int64_t>(source->snapshot()->cell(0, 0)) == 2);
    CHECK(edits.unsettled() == 0);
}

TEST_CASE("table: under deferred reorder, a committed edit moves its row when the commit settles", "[table][edits]") {
    morph::exec::MainThreadExecutor owner;
    std::vector<std::function<void()>> settles;
    auto const source = fiveRows();
    Engine engine{source, EngineOptions{.owner = &owner,
                                        .reorder = ReorderPolicy::Deferred,
                                        .scheduleSettle = [&](std::chrono::milliseconds, std::function<void()> fire) {
                                            settles.push_back(std::move(fire));
                                        }}};
    REQUIRE(engine.setSort({{.column = "c", .dir = kAsc}}).has_value());
    REQUIRE(viewKeys(engine).front() == key(1));  // 10 sorts first
    std::vector<EditDone> pending;
    CellEdits edits{*source, [&](EditRequest const&, EditDone done) { pending.push_back(std::move(done)); }, &engine};
    edits.commit(EditRequest{.row = key(1), .column = "c", .value = std::int64_t{60}});
    REQUIRE(pending.size() == 1);
    pending.front()(std::vector<Cell>{std::int64_t{60}});
    owner.drain();
    // The row moved to the end as the commit settled, with no settle timer fired.
    CHECK(viewKeys(engine).back() == key(1));
    CHECK(engine.heldRows().empty());
}

TEST_CASE("table: a completion after the committer is gone is ignored", "[table][edits]") {
    auto const source = fiveRows();
    EditDone late;
    {
        CellEdits edits{*source, [&](EditRequest const&, EditDone done) { late = std::move(done); }};
        edits.commit(EditRequest{.row = key(0), .column = "c", .value = std::int64_t{7}});
    }
    late(std::vector<Cell>{std::int64_t{7}});
    CHECK(std::get<std::int64_t>(source->snapshot()->cell(0, 0)) == 50);
}

TEST_CASE("table: multiple selection toggles, ignores repeats and clears", "[table][selection]") {
    SelectionOptions const options{
        .mode = SelectionMode::Multiple, .pruneOnFilter = true, .tableMode = TableMode::Client};
    auto selection = make(options);
    CHECK(selection.options().pruneOnFilter);
    CHECK(selection.select(key(1)));
    CHECK(selection.select(key(1)));  // already selected: kept once
    CHECK(selection.keys() == std::vector<RowId>{key(1)});
    CHECK(selection.toggle(key(2)));
    CHECK(selection.keys() == std::vector<RowId>{key(1), key(2)});
    CHECK_FALSE(selection.deselect(key(3)));
    CHECK(selection.selectedCount() == 2);
    selection.clear();
    CHECK(selection.selectedCount() == 0);
    CHECK(selection.keys().empty());
    CHECK_FALSE(selection.contains(key(1)));

    Engine const engine{fiveRows()};
    CHECK(Selection::activate(engine, 0, {}));  // no receiver: the row is still activatable
}

TEST_CASE("table: a completion called twice settles its edit once", "[table][edits]") {
    auto const source = fiveRows();
    std::vector<EditDone> pending;
    int patches = 0;
    struct Counting final : RowPatcher {
        VectorSource* source = nullptr;
        int* patches = nullptr;
        bool patchRow(RowId const& row, std::vector<Cell> cells) override {
            ++*patches;
            return source->patchRow(row, std::move(cells));
        }
    } patcher;
    patcher.source = source.get();
    patcher.patches = &patches;
    CellEdits edits{patcher, [&](EditRequest const&, EditDone done) { pending.push_back(std::move(done)); }};
    edits.commit(EditRequest{.row = key(0), .column = "c", .value = std::int64_t{1}});
    REQUIRE(pending.size() == 1);
    auto const done = pending.front();
    done(std::vector<Cell>{std::int64_t{1}});
    done(std::vector<Cell>{std::int64_t{2}});
    CHECK(patches == 1);
    CHECK(std::get<std::int64_t>(source->snapshot()->cell(0, 0)) == 1);
}

TEST_CASE("table: a field error without a column attaches to the edited cell", "[table][edits]") {
    auto const source = fiveRows();
    std::vector<EditDone> pending;
    CellEdits edits{*source, [&](EditRequest const&, EditDone done) { pending.push_back(std::move(done)); }};
    edits.commit(EditRequest{.row = key(3), .column = "c", .value = std::string{"x"}});
    REQUIRE(pending.size() == 1);
    pending.front()(std::unexpected(FieldError{.column = {}, .message = "refused"}));
    CHECK(edits.error(key(3), "c") == "refused");
    CHECK_FALSE(edits.error(key(3), "").has_value());
}

TEST_CASE("view selection: the view revision is membership", "[table][selection][revision]") {
    SelectionModel model;
    std::string const before = revisionOf(model, open());
    CHECK(before.size() == 48);
    model.source->patchRow(RowId{std::int64_t{2}}, {Cell{std::string{"open"}}, Cell{tabletest::dec("9.5")}});
    CHECK(revisionOf(model, open()) == before);  // a cell changed, membership did not
    TableQuery const ascending{.sort = {{.column = "mass", .dir = SortDirection::Ascending}}, .filters = open()};
    TableQuery const descending{.sort = {{.column = "mass", .dir = SortDirection::Descending}}, .filters = open()};
    CHECK(apply(*model.source, ascending).value().viewRevision == before);
    CHECK(apply(*model.source, descending).value().viewRevision == before);  // the sort is not membership
    model.source->patchRow(RowId{std::int64_t{7}}, {Cell{std::string{"open"}}, Cell{tabletest::dec("0.7")}});
    CHECK(revisionOf(model, open()) != before);  // a row came in
    CHECK(revisionOf(model, open(), {RowId{std::int64_t{8}}}) != revisionOf(model, open()));
    // One row out and another in: as many rows as before, and not the same ones.
    std::string const seven = revisionOf(model, open());
    model.source->patchRow(RowId{std::int64_t{2}}, {Cell{std::string{"closed"}}, Cell{tabletest::dec("9.5")}});
    CHECK(revisionOf(model, open()) != before);  // 0,1,3,4,5,7 against 0..5: six rows each, and different
    CHECK(revisionOf(model, open()) != seven);
}
// Mutation: the revision taken from the snapshot's version, which changes it on the first edit.

TEST_CASE("view selection: the digest of a set of keys", "[table][selection][revision]") {
    MembershipDigest forward;
    forward.add(RowId{std::int64_t{0}});
    forward.add(RowId{std::int64_t{1}});
    // count, sum of the FNV-1a hashes of "i:0" and "i:1", sum of their splitmix64 mixes; each 16 hex digits.
    CHECK(forward.text() == "00000000000000025614773256aab02f9ad26e76a98ec3f4");
    MembershipDigest backward;
    backward.add(RowId{std::int64_t{1}});
    backward.add(RowId{std::int64_t{0}});
    CHECK(backward.text() == forward.text());  // a set: the order keys are added in does not matter
    MembershipDigest swapped;
    swapped.add(RowId{std::int64_t{0}});
    swapped.add(RowId{std::int64_t{2}});
    CHECK(swapped.text() != forward.text());  // as many keys, other ones
}
// Mutation: the digest of the count alone, which passes the pinned value only by its first 16 digits.

TEST_CASE("view selection: a row-scoped table with no scope is refused", "[table][selection][resolve]") {
    seltest::UnscopedModel model;
    RowSelection<&seltest::UnscopedModel::rows> const selection{.keys = keysOf({1}), .view = {}, .except = {}};
    auto resolved = resolve(model, selection, caller(), SelectionPolicy::Current);
    CHECK(errorOf(resolved) == TableErrorCode::SourceContract);
}
// Mutation: no scope check in resolve, which resolves one caller's selection against rows no scope names.

TEST_CASE("view selection: resolve under Exact is the view less except, in source order",
          "[table][selection][resolve]") {
    SelectionModel model;
    RowSelection<&SelectionModel::rows> const selection{
        .keys = {},
        .view = {{.filters = open(), .retained = {}, .revision = revisionOf(model, open())}},
        .except = {RowId{std::int64_t{3}}}};
    auto exact = resolve(model, selection, caller(), SelectionPolicy::Exact);
    auto const rows = settledNow(exact).value();
    CHECK(rows.keys ==
          std::vector<RowId>{std::int64_t{0}, std::int64_t{1}, std::int64_t{2}, std::int64_t{4}, std::int64_t{5}});
    CHECK(rows.dropped == 0);

    model.source->patchRow(RowId{std::int64_t{7}}, {Cell{std::string{"open"}}, Cell{tabletest::dec("0.7")}});
    auto changed = resolve(model, selection, caller(), SelectionPolicy::Exact);
    CHECK(errorOf(changed) == TableErrorCode::ViewChanged);
    auto current = resolve(model, selection, caller(), SelectionPolicy::Current);
    CHECK(settledNow(current).value().keys == std::vector<RowId>{std::int64_t{0}, std::int64_t{1}, std::int64_t{2},
                                                                 std::int64_t{4}, std::int64_t{5}, std::int64_t{7}});
}
// Mutation: `viewRevision` that ignores the source snapshot (a digest of the filter alone), which resolves the
// edited view under Exact.

TEST_CASE("view selection: a key selection resolves to the keys the caller sees", "[table][selection][resolve]") {
    SelectionModel model;
    // The caller's source is rows 0..4 only, through table::scoped.
    model.scope = keysOf({0, 1, 2, 3, 4});
    RowSelection<&SelectionModel::rows> const selection{.keys = keysOf({3, 1, 8}), .view = {}, .except = {}};
    auto resolved = resolve(model, selection, caller(), SelectionPolicy::Exact);
    auto const rows = settledNow(resolved).value();
    CHECK(rows.keys == keysOf({1, 3}));  // source order
    CHECK(rows.dropped == 1);
}
// Mutation: dropped keys left in the reply, which returns 8.

TEST_CASE("view selection: a view over maxSelectedRows is LimitExceeded", "[table][selection][resolve]") {
    CHECK(QueryLimits{}.maxSelectedRows == 1'000'000);
    SelectionModel model;
    model.limits.maxSelectedRows = 5;  // the open view has 6 rows
    RowSelection<&SelectionModel::rows> const selection{
        .keys = {}, .view = {{.filters = open(), .retained = {}, .revision = {}}}, .except = keysOf({3})};
    auto over = resolve(model, selection, caller(), SelectionPolicy::Current);
    // The bound is on the view, 6 rows, not on what is left of it, 5.
    CHECK(errorOf(over) == TableErrorCode::LimitExceeded);
    model.limits.maxSelectedRows = 6;
    auto within = resolve(model, selection, caller(), SelectionPolicy::Current);
    CHECK(settledNow(within).value().keys == keysOf({0, 1, 2, 4, 5}));
}
// Mutation: the bound compared against the view less `except` (5 rows), which resolves the first case.

TEST_CASE("view selection: retained rows are in the membership", "[table][selection][resolve]") {
    SelectionModel model;
    RowSelection<&SelectionModel::rows> const selection{
        .keys = {},
        .view = {{.filters = open(), .retained = keysOf({8}), .revision = revisionOf(model, open(), keysOf({8}))}},
        .except = {}};
    auto exact = resolve(model, selection, caller(), SelectionPolicy::Exact);
    CHECK(settledNow(exact).value().keys == keysOf({0, 1, 2, 3, 4, 5, 8}));

    // Retained {42}, a key the source lacks, adds nothing.
    CHECK(revisionOf(model, open(), keysOf({42})) == revisionOf(model, open()));
    RowSelection<&SelectionModel::rows> const absent{
        .keys = {},
        .view = {{.filters = open(), .retained = keysOf({42}), .revision = revisionOf(model, open(), keysOf({42}))}},
        .except = {}};
    auto none = resolve(model, absent, caller(), SelectionPolicy::Exact);
    CHECK(settledNow(none).value().keys == keysOf({0, 1, 2, 3, 4, 5}));
}
// Mutation: retained rows left out of `membership`, which refuses with ViewChanged.

TEST_CASE("view selection: its JSON", "[table][selection]") {
    RowSelection<&SelectionModel::rows> keys;
    REQUIRE_FALSE(glz::read_json(keys, std::string_view{R"({"keys":[1,"a"]})"}));
    CHECK(keys.keys == std::vector<RowId>{std::int64_t{1}, std::string{"a"}});
    CHECK_FALSE(keys.view.has_value());

    std::string const text = R"({"view":{"filters":{"columns":{"status":{"include":[{"eq":"open"}]}}},)"
                             R"("retained":[8],"revision":"r"},"except":[3]})";
    RowSelection<&SelectionModel::rows> view;
    REQUIRE_FALSE(glz::read_json(view, text));
    REQUIRE(view.view.has_value());
    CHECK(view.view->filters == open());
    CHECK(glz::write_json(view).value_or("") == text);  // byte for byte
    CHECK(glz::write_json(SelectionPolicy::Exact).value_or("") == R"("exact")");
    CHECK(glz::write_json(SelectionPolicy::Current).value_or("") == R"("current")");
}
// Mutation: `SelectionPolicy` without glaze meta, which writes 0 and 1.

// ── Rows kept in view ─────────────────────────────────────────────────────────────────────────────────────────

namespace {

// The keys of an `apply` page, in its order.
std::vector<RowId> pageKeys(VectorSource const& source, PageRows const& page) {
    std::vector<RowId> out;
    for (auto const row : page.rows) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(source.snapshot()->rowId(row));
    }
    return out;
}

// The model's rows with a description of its two columns, pinned: what `answerQuery` reads.
ServedTable describedRows(SelectionModel& model) {
    auto served = model.rows(caller());
    TableDescription description;
    description.id = "rows";
    description.columns = {TableColumn{.info = tabletest::col("status", ColumnKind::Text), .label = "Status"},
                           TableColumn{.info = tabletest::col("mass", ColumnKind::Decimal), .label = "Mass"}};
    served.description = describe(std::move(description));
    return detail::pinned(std::move(served));
}

}  // namespace

TEST_CASE("retained rows: they stay in the view, listed in unmatched", "[table][retain]") {
    SelectionModel model;
    TableQuery const query{.sort = {{.column = "mass", .dir = SortDirection::Descending}},
                           .filters = open(),
                           .retain = {RowId{std::int64_t{1}}, RowId{std::int64_t{7}}, RowId{std::int64_t{8}}}};
    for (std::int64_t key : {1, 7, 8}) {  // the action closed row 1; rows 7 and 8 were closed already
        model.source->patchRow(RowId{key},
                               {Cell{std::string{"closed"}}, Cell{tabletest::dec(std::to_string(key) + ".0")}});
    }
    auto const page = apply(*model.source, query).value();
    std::vector<RowId> keys;
    for (auto const row : page.rows) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `keys` is a fixture list built once; sizing it first would not change what the case checks
        keys.push_back(model.source->snapshot()->rowId(row));
    }
    CHECK(keys == std::vector<RowId>{std::int64_t{8}, std::int64_t{7}, std::int64_t{5}, std::int64_t{4},
                                     std::int64_t{3}, std::int64_t{2}, std::int64_t{1}, std::int64_t{0}});
    CHECK(page.unmatched == std::vector<std::int64_t>{0, 1, 6});
    CHECK(page.total == 8);
    TableQuery const plain{.sort = query.sort, .filters = open()};
    CHECK(page.viewRevision != apply(*model.source, plain).value().viewRevision);
}
// Mutation: retained rows appended after the base view instead of merged by the sort's keys.

TEST_CASE("retained rows: the shared view entry is not changed by one request's rows", "[table][retain]") {
    SelectionModel model;
    auto const served = describedRows(model);
    ViewCache views;
    detail::AnswerContext const context{.views = &views, .record = "seltest::SelectionModel.rows", .rowScoped = false};
    TableQuery const retaining{
        .sort = {{.column = "mass", .dir = SortDirection::Ascending}}, .filters = open(), .retain = keysOf({7})};
    auto const first = detail::answerQuery(served, retaining, context);
    REQUIRE(first.has_value());
    CHECK(first->keys == keysOf({0, 1, 2, 3, 4, 5, 7}));
    CHECK(first->unmatched == std::vector<std::int64_t>{6});
    TableQuery const plain{.sort = retaining.sort, .filters = open()};
    auto const second = detail::answerQuery(served, plain, context);
    REQUIRE(second.has_value());
    CHECK(second->keys == keysOf({0, 1, 2, 3, 4, 5}));
    CHECK(second->total == 6);
    CHECK(second->unmatched.empty());
    CHECK(views.stats().computations == 1);
    // A keyed query of the plain view does not find 7 either.
    TableQuery keyed = plain;
    keyed.keys = keysOf({7, 2});
    auto const third = detail::answerQuery(served, keyed, context);
    REQUIRE(third.has_value());
    CHECK(third->keys == keysOf({2}));
}
// Mutation: the merged view written into the shared view cache entry, which serves one session's retained rows to
// another.

TEST_CASE("retained rows: retaining adds, never removes", "[table][retain][property]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 20261120 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 random{20261120};
    for (int table = 0; table < 200; ++table) {
        std::uniform_int_distribution<int> rowsOf{0, 40};
        std::uniform_int_distribution<int> valueOf{0, 9};
        int const count = rowsOf(random);
        std::vector<std::vector<Cell>> rows;
        for (int row = 0; row < count; ++row) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
            rows.push_back({Cell{std::int64_t{valueOf(random)}}, Cell{std::string{valueOf(random) < 5 ? "x" : "y"}}});
        }
        auto const source = tabletest::makeSource(
            {tabletest::col("a", ColumnKind::Integer), tabletest::col("b", ColumnKind::Text)}, std::move(rows));
        TableQuery plain;
        bool const ascending = valueOf(random) < 5;
        plain.sort = {{.column = "a", .dir = ascending ? SortDirection::Ascending : SortDirection::Descending}};
        plain.page.limit = 1000;
        if (valueOf(random) < 7) {
            plain.filters.columns["a"].include.push_back(FilterEntry{.ge = std::to_string(valueOf(random))});
        }
        if (valueOf(random) < 5) {
            plain.filters.columns["b"].include.push_back(FilterEntry{.eq = "x"});
        }
        TableQuery retaining = plain;
        std::uniform_int_distribution<int> retainedOf{0, 20};
        std::uniform_int_distribution<std::int64_t> keyOf{0, count + 5};
        for (int at = retainedOf(random); at > 0; --at) {
            retaining.retain.emplace_back(keyOf(random));
        }
        auto const without = apply(*source, plain).value();
        auto const with = apply(*source, retaining).value();
        auto const before = pageKeys(*source, without);
        auto const after = pageKeys(*source, with);
        std::set<RowId> const beforeSet(before.begin(), before.end());
        std::set<RowId> const afterSet(after.begin(), after.end());
        // The view with R holds every row of the view without it.
        CHECK(std::ranges::includes(afterSet, beforeSet));
        // What it adds is R's rows the source has and the view without R lacks.
        std::set<RowId> added;
        std::ranges::set_difference(afterSet, beforeSet, std::inserter(added, added.end()));
        std::set<RowId> expected;
        for (auto const& key : retaining.retain) {
            if (std::get<std::int64_t>(key) < count && !beforeSet.contains(key)) {
                expected.insert(key);
            }
        }
        CHECK(added == expected);
        CHECK(after.size() == afterSet.size());  // no row twice
        CHECK(with.total == static_cast<std::int64_t>(after.size()));
        // `unmatched` names exactly the rows of R the filter rejects: with no unloaded cell, those it left out.
        std::set<RowId> unmatched;
        for (auto const at : with.unmatched) {
            unmatched.insert(after.at(static_cast<std::size_t>(at)));
        }
        CHECK(unmatched == expected);
        // The order is the sort's over every row of the view, ties to the lower source row.
        auto const snapshot = source->snapshot();
        auto const aOf = [&snapshot](std::size_t row) { return std::get<std::int64_t>(snapshot->cell(row, 0)); };
        for (std::size_t at = 1; at < with.rows.size(); ++at) {
            auto const left = aOf(with.rows[at - 1]);
            auto const right = aOf(with.rows[at]);
            CHECK((left == right ? with.rows[at - 1] < with.rows[at] : (ascending ? left < right : left > right)));
        }
    }
}
// Mutation: a retained row counted twice when the filter passes it too, which breaks the second equation.

TEST_CASE("retained rows: the bound and the wire", "[table][retain]") {
    CHECK(QueryLimits{}.maxRetained == 10'000);
    auto const source = SelectionModel{}.source;
    TableQuery over;
    for (std::int64_t key = 0; key < 10'001; ++key) {
        over.retain.emplace_back(key);
    }
    auto const refused = validate(over, source->columns());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::LimitExceeded);
    over.retain.pop_back();
    CHECK(validate(over, source->columns()).has_value());

    GenericPage page;
    page.keys = keysOf({1, 2});
    page.unmatched = {0, 6};
    auto const text = glz::write_json(page).value_or("");
    CHECK(text.contains(R"("unmatched":[0,6])"));
    GenericPage read;
    REQUIRE_FALSE(glz::read_json(read, text));
    CHECK(read.unmatched == std::vector<std::int64_t>{0, 6});
    GenericPage plain;
    REQUIRE_FALSE(glz::read_json(plain, std::string_view{R"({"revision":"r","columns":[],"keys":[],"rows":[]})"}));
    CHECK(plain.unmatched.empty());
    // A query that retains nothing is written without `retain`; one that does carries it.
    CHECK(!glz::write_json(TableQuery{}).value_or("").contains("retain"));
    TableQuery retaining;
    retaining.retain = keysOf({3});
    auto const query = glz::write_json(retaining).value_or("");
    CHECK(query.contains(R"("retain":[3])"));
    TableQuery back;
    REQUIRE_FALSE(glz::read_json(back, query));
    CHECK(back.retain == keysOf({3}));
}
// Mutation: no bound, which accepts 10'001.

TEST_CASE("retained rows: a client-mode engine does the same", "[table][retain]") {
    SelectionModel model;
    Engine engine{model.source};
    REQUIRE(engine.setFilter(open()).has_value());
    REQUIRE(engine.setSort({{.column = "mass", .dir = SortDirection::Descending}}).has_value());
    for (std::int64_t key : {1, 7, 8}) {
        model.source->patchRow(RowId{key},
                               {Cell{std::string{"closed"}}, Cell{tabletest::dec(std::to_string(key) + ".0")}});
    }
    REQUIRE(engine.retain(keysOf({1, 7, 8})).has_value());
    CHECK(viewKeys(engine) == keysOf({8, 7, 5, 4, 3, 2, 1, 0}));
    std::vector<bool> unmatched;
    for (std::size_t row = 0; row < engine.viewRowCount(); ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `unmatched` is a fixture list built once; sizing it first would not change what the case checks
        unmatched.push_back(engine.isUnmatched(row));
    }
    CHECK(unmatched == std::vector<bool>{true, true, false, false, false, false, true, false});
    REQUIRE(engine.setFilter(open()).has_value());
    CHECK(engine.retained().empty());
    CHECK(viewKeys(engine) == keysOf({5, 4, 3, 2, 0}));
}
// Mutation: Engine::retain ignored by the filter run, which leaves 1, 7 and 8 out.

TEST_CASE("retained rows: a selection of a view with retained rows resolves under Exact", "[table][retain][resolve]") {
    // The revision a page with `retain` carries is the one `resolve` computes for the same retained rows, so the
    // client's selection of such a view, which names them, is not refused as changed.
    SelectionModel model;
    model.source->patchRow(RowId{std::int64_t{1}}, {Cell{std::string{"closed"}}, Cell{tabletest::dec("1.0")}});
    TableQuery const query{
        .sort = {{.column = "mass", .dir = SortDirection::Descending}}, .filters = open(), .retain = keysOf({1, 8})};
    auto const page = apply(*model.source, query).value();
    RowSelection<&SelectionModel::rows> const selection{
        .keys = {},
        .view = {{.filters = open(), .retained = keysOf({1, 8}), .revision = page.viewRevision}},
        .except = keysOf({3})};
    auto exact = resolve(model, selection, caller(), SelectionPolicy::Exact);
    CHECK(settledNow(exact).value().keys == keysOf({0, 1, 2, 4, 5, 8}));
}
// Mutation: the merged digest left without the retained keys, which refuses with ViewChanged.

TEST_CASE("retained rows: one the filter cannot decide is in the view, not undecided", "[table][retain]") {
    // Column c is at level "slow"; row 2's cell is not loaded, so `c = x` can neither admit nor reject it.
    auto const source = tabletest::makeSource(
        {tabletest::levelCol("c", ColumnKind::Text, "slow")},
        {{Cell{std::string{"x"}}}, {Cell{std::string{"y"}}}, {Cell{Unloaded{}}}, {Cell{std::string{"x"}}}});
    TableQuery plain;
    plain.filters.columns["c"].include.push_back(FilterEntry{.eq = "x"});
    auto const without = apply(*source, plain).value();
    REQUIRE(pageKeys(*source, without) == keysOf({0, 3}));
    REQUIRE(without.undecided == 1);
    TableQuery retaining = plain;
    retaining.retain = keysOf({2, 1});
    auto const with = apply(*source, retaining).value();
    CHECK(pageKeys(*source, with) == keysOf({0, 1, 2, 3}));
    CHECK(with.undecided == 0);
    CHECK(with.totalKind == TotalKind::Exact);
    CHECK(with.unmatched == std::vector<std::int64_t>{1});  // row 1 is rejected; row 2 is undecided, not rejected
}

TEST_CASE("retained rows: a keyed query sees the merged view", "[table][retain]") {
    SelectionModel model;
    TableQuery query{
        .sort = {{.column = "mass", .dir = SortDirection::Descending}}, .filters = open(), .retain = keysOf({7})};
    query.keys = keysOf({7, 2});
    auto const page = apply(*model.source, query).value();
    CHECK(pageKeys(*model.source, page) == keysOf({7, 2}));
    CHECK(page.indices == std::vector<std::int64_t>{0, 4});  // 7, then 5, 4, 3, 2
    CHECK(page.unmatched == std::vector<std::int64_t>{0});
    CHECK(page.total == 7);
}
