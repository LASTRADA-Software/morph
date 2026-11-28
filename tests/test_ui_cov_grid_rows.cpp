// SPDX-License-Identifier: Apache-2.0

// A grid's rows: what inserting, appending and deleting do to the focus and the selection, and the bounds that refuse
// them.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <optional>
#include <string>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::at;
using morph::test::grid::GridFixture;
using namespace morph;

// Rows of at most two, and at least one.
std::string boundedSchema() {
    std::string schema{morph::test::grid::kSpecimens};
    auto const replaceOnce = [&schema](std::string const& from, std::string const& to) {
        REQUIRE(schema.find(from) != std::string::npos);
        schema.replace(schema.find(from), from.size(), to);
    };
    replaceOnce(R"("minItems":0,"maxItems":5)", R"("minItems":1,"maxItems":2)");
    return schema;
}

}  // namespace

TEST_CASE("cov grid rows: inserting needs a focused row and goes above or below it", "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->insertRow(true);  // no focus: nowhere to insert
    CHECK(f.form->field("specimens").rows().size() == 2);
    f.grid->focusCell(at(1, "note"));
    f.grid->type("first");
    f.grid->press("Enter");
    f.grid->focusCell(at(1, "note"));
    f.grid->insertRow(true);
    REQUIRE(f.form->field("specimens").rows().size() == 3);
    CHECK(f.form->field("specimens[0].note").draft() == "first");
    CHECK(f.form->field("specimens[1].note").draft().empty());  // the new row sits below the focused one
    f.grid->focusCell(at(1, "note"));
    f.grid->insertRow(false);
    REQUIRE(f.form->field("specimens").rows().size() == 4);
    CHECK(f.form->field("specimens[0].note").draft().empty());  // and above it
    CHECK(f.form->field("specimens[1].note").draft() == "first");
}

TEST_CASE("cov grid rows: a grid at its most refuses a row, and one at its fewest refuses a deletion",
          "[ui][grid][cov]") {
    GridFixture f{2, ui::GridOptions{}, boundedSchema()};
    ui::RowOps const full = f.grid->rowOps();
    CHECK_FALSE(full.insert);
    CHECK(full.remove);
    CHECK(full.why == "A grid holds at most 2 rows");
    f.grid->focusCell(at(1, "note"));
    f.grid->insertRow(true);
    CHECK(f.form->field("specimens").rows().size() == 2);
    f.grid->addRow();
    CHECK(f.form->field("specimens").rows().size() == 2);
    f.grid->deleteRows();
    REQUIRE(f.form->field("specimens").rows().size() == 1);
    ui::RowOps const least = f.grid->rowOps();
    CHECK(least.insert);
    CHECK_FALSE(least.remove);
    CHECK(least.why == "A grid holds at least 1 rows");
    f.grid->focusCell(at(2, "note"));
    f.grid->deleteRows();
    CHECK(f.form->field("specimens").rows().size() == 1);
}

TEST_CASE("cov grid rows: fixed rows are neither inserted, appended nor deleted", "[ui][grid][cov]") {
    GridFixture f{2, ui::GridOptions{.rowsFixed = true}};
    f.grid->focusCell(at(1, "note"));
    f.grid->insertRow(true);
    f.grid->addRow();
    f.grid->deleteRows();
    CHECK(f.form->field("specimens").rows().size() == 2);
    ui::RowOps const ops = f.grid->rowOps();
    CHECK_FALSE(ops.insert);
    CHECK_FALSE(ops.remove);
    CHECK(ops.why.empty());
}

TEST_CASE("cov grid rows: deleting the last row moves focus to the one before, and the only row leaves none",
          "[ui][grid][cov]") {
    GridFixture f{3};
    f.grid->focusCell(at(3, "note"));
    f.grid->type("kept");
    f.grid->press("Enter");
    f.grid->focusCell(at(3, "note"));
    f.grid->deleteRows();
    CHECK(f.form->field("specimens").rows().size() == 2);
    CHECK(f.grid->focus() == at(2, "note"));
    CHECK(f.grid->message() == "Deleted 1 row");
    f.grid->deleteRows();
    f.grid->deleteRows();
    CHECK(f.form->field("specimens").rows().empty());
    CHECK_FALSE(f.grid->focus().has_value());
    f.grid->deleteRows();  // nothing to delete
    CHECK(f.form->field("specimens").rows().empty());
}

TEST_CASE("cov grid rows: deleting the rows of a selection drops the selection and names the count",
          "[ui][grid][cov]") {
    GridFixture f{4};
    for (std::int64_t const key : {2, 3}) {
        f.grid->focusCell(at(key, "note"));
        f.grid->type("kept");
        f.grid->press("Enter");
    }
    f.grid->select(ui::CellRange{.anchor = at(2, "label"), .focus = at(3, "note")});
    f.grid->deleteRows();
    CHECK(f.form->field("specimens").rows().size() == 2);
    CHECK_FALSE(f.grid->selection().has_value());
    CHECK(f.grid->message() == "Deleted 2 rows");
    f.grid->undo();
    CHECK(f.form->field("specimens").rows().size() == 4);
}

TEST_CASE("cov grid rows: enter and tab past the last row append one when the grid appends", "[ui][grid][cov]") {
    GridFixture f{1, ui::GridOptions{.appendRows = true}};
    f.grid->focusCell(at(1, "note"));
    f.grid->type("a");
    f.grid->press("Enter");
    REQUIRE(f.form->field("specimens").rows().size() == 2);
    CHECK(f.grid->focus() == at(2, "note"));  // the same column, in the new row
    f.grid->focusCell(at(2, "note"));
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::Handled);
    CHECK(f.form->field("specimens").rows().size() == 3);
    CHECK(f.grid->focus().has_value());
}

TEST_CASE("cov grid rows: enter past the last row appends nothing in a grid that does not append", "[ui][grid][cov]") {
    GridFixture f{1};
    f.grid->focusCell(at(1, "note"));
    f.grid->type("a");
    f.grid->press("Enter");
    CHECK(f.form->field("specimens").rows().size() == 1);
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);
}
