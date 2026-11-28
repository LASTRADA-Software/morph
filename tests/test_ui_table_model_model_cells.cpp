// SPDX-License-Identifier: Apache-2.0

// A model-backed table's cells, selection and field errors.
//
// Part of the client table model tests; see docs/spec/table/table.md.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "grid_test_support.hpp"
#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace table = morph::table;

using namespace table_model_support;

using morph::ui::Value;
using morph::ui::testing::keyedJson;
using morph::ui::testing::RowsFixture;
using namespace std::chrono_literals;
using morph::test::grid::at;
using morph::test::grid::committedOk;
using morph::test::grid::ModelGridFixture;

// Mutation: onRowsAbsent called with every key the fetch named (row 2, which came back, is reported too).

TEST_CASE("table rows: a keyed refetch that returns no row reports it absent", "[ui][table][edit]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    std::vector<std::vector<table::RowId>> absent;
    rows->onRowsAbsent(
        [&absent](std::span<table::RowId const> keys) { absent.emplace_back(keys.begin(), keys.end()); });
    h.change({2, 3});
    REQUIRE(rows->stats().rowsFetches == 1);
    h.replyPage(keyedJson({{0, 0}, {2, 2}}, 300, h.columns));  // row 3 is gone
    REQUIRE(absent.size() == 1);
    CHECK(absent.at(0) == std::vector<table::RowId>{table::RowId{std::int64_t{3}}});
}

// Mutation: the busy test left out of ModelGridSource::readOnly (row 3 takes the edit).

TEST_CASE("model table: read-only is one rule", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2, 3, 4, 5}};
    f.rows->view.at(1).unloaded = {"counts"};  // row 2's count is not loaded
    f.rows->rowStates.busyKeys(7, {row(3)});   // another action's run
    morph::test::grid::setField(*f.rows, 4, "lockedBy", Value{std::string{"Being edited by another user"}});
    morph::test::grid::setField(*f.rows, 5, "countFrozen", Value{true});
    for (std::int64_t const k : {2, 3, 4, 5}) {
        CHECK(f.source->readOnly(key(k), "count"));
    }
    CHECK_FALSE(f.source->readOnly(key(1), "count"));
    CHECK(f.source->readOnly(key(1), "note"));  // no edit action
    f.grid->focusCell(at(2, "count"));          // arrows and clicks reach read-only cells
    CHECK(f.grid->focus() == at(2, "count"));
    f.grid->type("7");
    CHECK(f.grid->message() == "This value is not loaded yet");
    f.grid->focusCell(at(3, "count"));
    f.grid->type("7");
    CHECK(f.grid->message() == "This row is being saved");
    f.grid->focusCell(at(4, "count"));
    f.grid->type("7");
    CHECK(f.grid->message() == "Being edited by another user");
    CHECK(f.commits.sent.empty());
    f.grid->focusCell(at(1, "count"));
    f.grid->type("3");
    f.grid->press("Enter");
    CHECK(f.commits.sent.size() == 1);
    CHECK(f.grid->focus() == at(1, "count"));  // Enter down passes rows 2-5, all read-only in count: it stays
    f.permitted = {"Lims_EditSpecimenRow"};    // count's action refused
    CHECK(f.source->readOnly(key(1), "count"));
}

// Mutation: the commit sent as a run of the session (marking row 1 busy in its states: the second cell refuses).

TEST_CASE("model table: the table's own edits do not make its row busy", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}};
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    f.grid->press("Enter");
    REQUIRE(f.commits.sent.size() == 1);
    REQUIRE(f.commits.sent.at(0).cells.size() == 1);
    CHECK(f.commits.sent.at(0).cells.at(0).json == R"("a")");
    CHECK(f.commits.sent.at(0).cells.at(0).member == "label");
    CHECK_FALSE(f.rows->rowStates.isBusy(row(1)));
    f.grid->focusCell(at(1, "remark"));
    f.grid->type("b");
    CHECK(f.grid->message().empty());
    f.grid->press("Enter");
    CHECK(f.commits.sent.size() == 1);  // queued behind the first
    CHECK(f.source->marks(key(1), "remark").stale);
    f.commits.done.at(0)(committedOk());
    CHECK(f.commits.sent.size() == 2);
    f.rows->rowStates.busyKeys(7, {row(2)});  // another action's run on row 2
    f.grid->focusCell(at(2, "label"));
    f.grid->type("c");
    CHECK(f.grid->message() == "This row is being saved");
    CHECK(f.commits.sent.size() == 2);
}
