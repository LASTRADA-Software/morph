// SPDX-License-Identifier: Apache-2.0
//
// morph::table's unloaded cell in the engine (docs/spec/table/table.md, "The unloaded cell" and "Semantics of an
// unloaded cell"): a base-level column that holds one is a source contract error, the cell reads back as itself,
// an edit of it is refused on the cell, and a selection of the view leaves undecided rows out.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/table/edits.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <morph/table/selection.hpp>
#include <string>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"

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
    Engine engine{tabletest::slowColumn(ColumnKind::Text, {Cell{Unloaded{}}})};
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
    CellEdits edits{*source, [&](EditRequest const& request, EditDone) { ran.push_back(request); }, &engine};
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
