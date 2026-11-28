// SPDX-License-Identifier: Apache-2.0

// A model-backed table's cells: how they lower and what an open editor holds.
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
using namespace std::chrono_literals;
using morph::test::grid::at;
using morph::test::grid::committedOk;
using morph::test::grid::ModelGridFixture;
using morph::test::grid::refusedWith;
using morph::test::grid::specimenRecord;

// Mutation: drafts held by the window (the decorator dropping a row's drafts with the row: row 5's draft is gone when
// it comes back).

TEST_CASE("model table: cells of a model's table", "[ui][table][edit]") {
    SECTION("cell mode") {
        ModelGridFixture f{{1, 2, 3, 4, 5}};
        f.grid->focusCell(at(1, "count"));
        f.grid->type("9");
        f.grid->press("Enter");
        REQUIRE(f.commits.sent.size() == 1);
        CHECK(f.commits.sent.at(0).row == row(1));
        REQUIRE(f.commits.sent.at(0).cells.size() == 1);
        CHECK(f.commits.sent.at(0).cells.at(0).column == "count");
        CHECK(f.commits.sent.at(0).cells.at(0).json == "9");
        CHECK(f.source->marks(key(1), "count").stale);
        f.commits.done.at(0)(committedOk());  // ChangedRows named row 1
        CHECK(f.source->marks(key(1), "count").modified);
        CHECK_FALSE(f.source->marks(key(1), "count").stale);
        CHECK(f.source->rowModified(key(1)));
        f.rows->refetch(row(1), specimenRecord(1, 7));  // the row as the server has it
        CHECK_FALSE(f.source->marks(key(1), "count").modified);
        CHECK(f.source->text(key(1), "count") == "7");
        CHECK(f.source->draft(key(1), "count") == "7");  // an editor opens on it, not on "9"
    }
    SECTION("refusal") {
        ModelGridFixture f{{1, 2, 3, 4, 5}};
        f.grid->focusCell(at(2, "count"));
        f.grid->type("9");
        f.grid->press("Enter");
        REQUIRE(f.commits.sent.size() == 1);
        morph::table::EditCell const& cell = f.commits.sent.at(0).cells.at(0);
        CHECK((cell.column == "count" && cell.json == "9" && cell.member == "value"));
        f.commits.done.at(0)(refusedWith({{.column = "value", .message = "Row 2 is closed"}}));
        CHECK(f.source->error(key(2), "count") == "Row 2 is closed");
        CHECK(f.source->draft(key(2), "count") == "9");
        CHECK(f.source->marks(key(2), "count").error);
        f.grid->focusCell(at(3, "count"));
        f.grid->type("11");
        f.grid->press("Enter");
        CHECK(f.commits.sent.size() == 1);  // the field's own maximum refuses it
        CHECK(f.source->error(key(3), "count").has_value());
        CHECK(f.source->draft(key(3), "count") == "11");
    }
    SECTION("scroll and from") {
        ModelGridFixture f{{1, 2, 3, 4, 5}};
        f.grid->focusCell(at(5, "count"));
        f.grid->type("abc");
        f.grid->leave();
        CHECK(f.edits.errorCount() == 1);
        CHECK(f.commits.sent.empty());
        morph::test::grid::FakeRows::Row dropped = f.rows->drop(row(5));
        f.rows->restore(std::move(dropped), 4);
        CHECK(f.source->draft(key(5), "count") == "abc");
        CHECK(f.source->error(key(5), "count").has_value());
        f.source->moveKey(key(5), key(9));
        CHECK(f.edits.draft(row(9), "count") == "abc");
        CHECK(f.edits.error(row(9), "count").has_value());
        CHECK_FALSE(f.edits.draft(row(5), "count").has_value());
        CHECK(f.rows->moved == std::vector<std::pair<table::RowId, table::RowId>>{{row(5), row(9)}});
    }
    SECTION("row mode") {
        ModelGridFixture f{{1, 2}, morph::ui::EditMode::Row};
        f.grid->focusCell(at(1, "label"));
        f.grid->type("a");
        f.grid->press("Tab");
        REQUIRE(f.grid->focus() == at(1, "remark"));
        f.grid->type("b");
        f.grid->press("Enter");
        CHECK(f.commits.sent.empty());
        f.grid->focusCell(at(2, "label"));
        REQUIRE(f.commits.sent.size() == 1);
        CHECK(f.commits.sent.at(0).row == row(1));
        REQUIRE(f.commits.sent.at(0).cells.size() == 2);
        morph::table::EditCell const& label = f.commits.sent.at(0).cells.at(0);
        morph::table::EditCell const& remark = f.commits.sent.at(0).cells.at(1);
        CHECK((label.column == "label" && label.json == R"("a")" && label.member == "label"));
        CHECK((remark.column == "remark" && remark.json == R"("b")" && remark.member == "remarkText"));
        f.commits.done.at(0)(refusedWith({{.column = "remarkText", .message = "too long"}}));
        CHECK(f.source->error(key(1), "remark") == "too long");
        CHECK_FALSE(f.source->error(key(1), "label").has_value());
        CHECK(f.grid->focus() == at(1, "remark"));  // focus back on the refused cell
        f.grid->focusCell(at(2, "label"));
        f.grid->type("c");
        f.grid->press("Ctrl+Enter");
        REQUIRE(f.commits.sent.size() == 2);
        CHECK(f.commits.sent.at(1).row == row(2));
        CHECK(f.grid->focus() == at(2, "label"));
    }
}

// Mutations: send() not checking read-only again (the held cell is sent into a row another run is saving); the
// dropped cell keeping its draft with no error (nothing shows the refusal); `resends` answering false (committing the
// refused draft unchanged sends nothing).

TEST_CASE("model table: a held cell whose row became busy is refused at the cell", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}, morph::ui::EditMode::Row};
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    f.grid->press("Tab");
    f.rows->rowStates.busyKeys(7, {row(1)});  // a document mutation's edit of row 1
    f.source->touch();
    f.grid->focusCell(at(2, "label"));  // focus leaves row 1
    CHECK(f.commits.sent.empty());
    CHECK(f.source->draft(key(1), "label") == "a");                        // the draft stays
    CHECK(f.source->error(key(1), "label") == "This row is being saved");  // with the refusal at the cell
    CHECK(f.source->marks(key(1), "label").error);
    CHECK(f.edits.errorCount() == 1);
    f.grid->focusCell(at(1, "label"));
    CHECK(f.grid->message() == "This row is being saved");  // and in the status line
    // The row is free again: committing the cell, unchanged, sends its draft when focus leaves the row.
    f.rows->rowStates.settle(7);
    f.source->touch();
    f.grid->press("F2");
    f.grid->press("Enter");
    f.grid->focusCell(at(2, "label"));
    REQUIRE(f.commits.sent.size() == 1);
    CHECK(f.commits.sent.at(0).row == row(1));
    CHECK(f.commits.sent.at(0).cells.at(0).json == R"("a")");
    CHECK(f.edits.errorCount() == 0);
}

// Mutation: rowsAbsent leaving the row's held cells (they are sent when focus leaves the row).

TEST_CASE("model table: a held cell of a row gone from the view is not sent", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}, morph::ui::EditMode::Row};
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    f.grid->press("Tab");
    std::vector<table::RowId> const gone{row(1)};
    f.source->rowsAbsent(gone);
    f.grid->focusCell(at(2, "label"));
    CHECK(f.commits.sent.empty());
}

// Mutation: the record's value loaded into an epoch field as it is (a `YYYY-MM-DD` text, a timestamp), which the field
// reads as blank.

TEST_CASE("model table: an editor opens holding the cell's value, whatever its kind", "[ui][table][edit]") {
    ModelGridFixture f{{1}};
    auto const opened = [&f](std::string const& column) {
        f.grid->focusCell(at(1, column));
        f.grid->press("F2");
        std::string draft = f.source->draft(key(1), column);
        f.grid->press("Escape");
        return draft;
    };
    std::string const sampled = opened("sampled");
    std::string const taken = opened("taken");
    CHECK_FALSE(sampled.empty());
    CHECK_FALSE(taken.empty());
    CHECK(opened("day") == sampled);  // an EpochDate field of a Date cell
    CHECK(opened("at") == taken);     // an EpochDateTime field of a DateTime cell
    CHECK(opened("mass") == "12.50");
    CHECK(opened("weight") == "0.200");  // 200 g, in the field's kg
    CHECK(f.commits.sent.empty());
}

// Mutation: ModelGridSource::moveKey not telling the controller (focus stays on key 5). Where a focused row the window
// no longer holds is, and which row takes focus when it leaves, are the mount's: the TableHarness cases below.

TEST_CASE("model table: focus follows its row's key", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2, 3, 4, 5}};
    f.grid->focusCell(at(5, "count"));
    f.source->moveKey(key(5), key(9));
    f.rows->rekey(row(5), row(9));
    CHECK(f.grid->focus() == at(9, "count"));
    CHECK(f.grid->place() == morph::ui::GridFocus{key(9), 4, "count"});
}

// Mutation: the foreign-unit fact left out of ModelGridSource's facts (the 0.3 % cell opens blank in the kg field and
// takes "5", sent as 5 kg).

TEST_CASE("model table: a measure its field's unit cannot hold is read-only", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}};
    morph::test::grid::setField(
        *f.rows, 2, "weight",
        Value{morph::ui::Quantity{.value = table::parseDecimal("0.3").value(), .unit = "percent"}});
    CHECK_FALSE(f.source->readOnly(key(1), "weight"));  // 200 g converts into kg
    CHECK(f.source->readOnly(key(2), "weight"));
    CHECK(f.source->marks(key(2), "weight").readOnly);
    f.grid->focusCell(at(2, "weight"));
    f.grid->type("5");
    CHECK(f.grid->message() == "This value's unit cannot be edited here");
    f.grid->press("Enter");
    CHECK(f.commits.sent.empty());
}

// Mutation: draft() loading the column's session for another row while an editor of the column is open (the open
// editor's field is reloaded with row 2's value).

TEST_CASE("model table: reading another cell of the column leaves an open editor as it is", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}};
    f.grid->focusCell(at(1, "count"));
    f.grid->type("5");
    morph::ui::testing::RecordingBackend backend;
    morph::ui::Mounted const view{f.h.rt, backend,
                                  f.source->editor(key(1), "count")};  // the open editor, as the mount builds it
    f.h.settle();
    int const input = backend.find("TextInput", "text", "5").value_or(0);
    REQUIRE(input != 0);
    CHECK(f.source->draft(key(2), "count") == "2");
    f.h.settle();
    CHECK(backend.prop(input, "text") == "5");  // the editor still shows its own cell
    CHECK(f.source->draft(key(1), "count") == "5");
    CHECK(f.grid->editing() == at(1, "count"));
}
