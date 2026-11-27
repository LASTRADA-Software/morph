// SPDX-License-Identifier: Apache-2.0

// Paste, fill down and clear over a grid of a form: where a paste starts, what it counts as dropped, and the rows it
// appends.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <string>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::at;
using morph::test::grid::GridFixture;
using namespace morph;

}  // namespace

TEST_CASE("cov grid paste: a paste of more values than the grid takes writes nothing", "[ui][grid][cov]") {
    GridFixture f{3, ui::GridOptions{.maxPasteCells = 3}};
    f.grid->focusCell(at(1, "label"));
    ui::PasteResult const result = f.grid->paste("a\tb\nc\td");
    CHECK(result.written == 0);
    CHECK(f.form->field("specimens[0].label").draft().empty());
    CHECK(f.grid->message() == "A paste holds at most 3 values");
}

TEST_CASE("cov grid paste: with no focus a paste starts at the corner of the selection", "[ui][grid][cov]") {
    GridFixture f{3};
    f.grid->select(ui::CellRange{.anchor = at(3, "note"), .focus = at(2, "label")});
    ui::PasteResult const result = f.grid->paste("x");
    CHECK(result.written == 1);
    CHECK(f.form->field("specimens[1].label").draft() == "x");  // the top left of the rectangle
    CHECK(f.form->field("specimens[2].note").draft().empty());
}

TEST_CASE("cov grid paste: with neither focus nor selection a paste starts at the first cell", "[ui][grid][cov]") {
    GridFixture f{2};
    ui::PasteResult const result = f.grid->paste("x\ty");
    CHECK(result.written == 2);
    CHECK(f.form->field("specimens[0].label").draft() == "x");
    CHECK(f.form->field("specimens[0].mass").draft() == "y");
}

TEST_CASE("cov grid paste: lines past the last row and cells past the last column are counted and dropped",
          "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->focusCell(at(2, "volume"));
    ui::PasteResult const result = f.grid->paste("1\t2\t3\t4\t5\n6\t7");
    CHECK(result.written == 2);  // volume and note; density and limit are read-only
    CHECK(result.readOnly == 2);
    CHECK(result.pastColumns == 1);  // the fifth cell of the first line
    CHECK(result.pastRows == 2);     // the second line has no row to land on
    CHECK(f.grid->message() == ui::pasteMessage(result));
    CHECK(f.form->field("specimens[1].note").draft() == "4");
    CHECK(f.form->field("specimens").rows().size() == 2);
}

TEST_CASE("cov grid paste: a grid that appends takes the lines past the last row as new rows up to its bound",
          "[ui][grid][cov]") {
    std::string schema{morph::test::grid::kSpecimens};
    schema.replace(schema.find(R"("maxItems":5)"), std::string_view{R"("maxItems":5)"}.size(), R"("maxItems":3)");
    GridFixture f{2, ui::GridOptions{.appendRows = true}, schema};
    f.grid->focusCell(at(2, "label"));
    ui::PasteResult const result = f.grid->paste("a\nb\nc");
    CHECK(result.written == 2);   // row 2 and the one appended
    CHECK(result.pastRows == 1);  // the third row would pass the bound
    REQUIRE(f.form->field("specimens").rows().size() == 3);
    CHECK(f.form->field("specimens[2].label").draft() == "b");
}

TEST_CASE("cov grid fill: fill down copies the top cell of each column down and skips cells it may not write",
          "[ui][grid][cov]") {
    GridFixture f{3};
    f.grid->focusCell(at(1, "label"));
    f.grid->paste("top\t1\t2");
    f.grid->select(ui::CellRange{.anchor = at(1, "label"), .focus = at(3, "volume")});
    f.grid->fillDown();
    for (std::string const path : {"specimens[1]", "specimens[2]"}) {
        CHECK(f.form->field(path + ".label").draft() == "top");
        CHECK(f.form->field(path + ".mass").draft() == "1");
        CHECK(f.form->field(path + ".volume").draft() == "2");
    }
    f.grid->undo();
    f.grid->select(ui::CellRange{.anchor = at(1, "label"), .focus = at(1, "volume")});
    f.grid->fillDown();  // one row: nothing to fill
    CHECK(f.form->field("specimens[2].label").draft() == "top");
}

TEST_CASE("cov grid fill: a fill that changes nothing leaves the rows alone", "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->select(ui::CellRange{.anchor = at(1, "label"), .focus = at(2, "label")});
    f.grid->fillDown();  // both blank already
    CHECK(f.form->field("specimens[1].label").draft().empty());
}

TEST_CASE("cov grid clear: clearing a selection blanks the cells it may write", "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "label"));
    f.grid->paste("a\t1\t2\n b\t3\t4");
    f.grid->select(ui::CellRange{.anchor = at(1, "label"), .focus = at(2, "volume")});
    f.grid->clear();
    for (std::string const path : {"specimens[0]", "specimens[1]"}) {
        CHECK(f.form->field(path + ".label").draft().empty());
        CHECK(f.form->field(path + ".mass").draft().empty());
        CHECK(f.form->field(path + ".volume").draft().empty());
    }
    f.grid->clear();  // nothing left to clear
    CHECK(f.form->field("specimens[0].label").draft().empty());
}

TEST_CASE("cov grid clear: with no selection the focused cell alone is cleared", "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "label"));
    f.grid->paste("keep\n\nlost");
    f.grid->focusCell(at(2, "label"));
    f.grid->clear();
    CHECK(f.form->field("specimens[0].label").draft() == "keep");
    CHECK(f.form->field("specimens[1].label").draft().empty());
}
