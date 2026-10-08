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

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/table/edits.hpp>
#include <morph/table/selection.hpp>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::viewKeys;

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
