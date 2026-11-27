// SPDX-License-Identifier: Apache-2.0

// The grid's keys that take a rectangle, a toggle or a whole column of the grid: Space on a flag, Ctrl+A, Shift and
// the arrows, fill down, and the commands a grid with no cells to act on refuses.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <optional>
#include <string>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::at;
using morph::test::grid::GridFixture;
using morph::test::grid::specimensWithChoices;
using namespace morph;

}  // namespace

TEST_CASE("cov grid keys: space toggles a flag and leaves other cells alone", "[ui][grid][cov]") {
    GridFixture f{2, ui::GridOptions{}, specimensWithChoices()};
    f.grid->focusCell(at(1, "checked"));
    CHECK(f.grid->press("Space") == ui::KeyOutcome::Handled);
    CHECK(f.json("specimens[0].checked") == "true");
    CHECK(f.grid->press("Space") == ui::KeyOutcome::Handled);
    CHECK(f.json("specimens[0].checked") == "false");
    f.grid->focusCell(at(2, "note"));
    CHECK(f.grid->press("Space") == ui::KeyOutcome::Handled);
    CHECK(f.json("specimens[1].note").empty());
    CHECK_FALSE(f.grid->editing().has_value());
}

TEST_CASE("cov grid keys: control a selects every cell", "[ui][grid][cov]") {
    GridFixture f{3};
    f.grid->focusCell(at(2, "mass"));
    CHECK(f.grid->press("Ctrl+A") == ui::KeyOutcome::Handled);
    std::optional<ui::CellRange> const range = f.grid->selection();
    REQUIRE(range.has_value());
    CHECK(range->anchor == at(1, "label"));
    CHECK(range->focus == at(3, "note"));
}

TEST_CASE("cov grid keys: control a on a grid with no rows selects nothing", "[ui][grid][cov]") {
    GridFixture f{0};
    CHECK(f.grid->press("Ctrl+A") == ui::KeyOutcome::Handled);
    CHECK_FALSE(f.grid->selection().has_value());
}

TEST_CASE("cov grid keys: shift and the arrows grow the selection in each direction", "[ui][grid][cov]") {
    GridFixture f{3};
    f.grid->focusCell(at(2, "mass"));
    f.grid->press("Shift+Down");
    REQUIRE(f.grid->selection().has_value());
    CHECK(f.grid->selection()->focus == at(3, "mass"));
    f.grid->press("Shift+Up");
    CHECK(f.grid->selection()->focus == at(2, "mass"));
    f.grid->press("Shift+Right");
    CHECK(f.grid->selection()->focus == at(2, "volume"));
    f.grid->press("Shift+Left");
    CHECK(f.grid->selection()->focus == at(2, "mass"));
}

TEST_CASE("cov grid keys: control d copies the top cell down the selection", "[ui][grid][cov]") {
    GridFixture f{3};
    f.grid->focusCell(at(1, "note"));
    f.grid->type("same");
    f.grid->press("Enter");
    f.grid->select(ui::CellRange{.anchor = at(1, "note"), .focus = at(3, "note")});
    CHECK(f.grid->press("Ctrl+D") == ui::KeyOutcome::Handled);
    CHECK(f.json("specimens[1].note") == R"("same")");
    CHECK(f.json("specimens[2].note") == R"("same")");
}

TEST_CASE("cov grid keys: control d and delete with no selection leave the rows as they are", "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->fillDown();
    f.grid->clear();
    CHECK(f.json("specimens[0].note").empty());
    CHECK(f.json("specimens[1].note").empty());
}

TEST_CASE("cov grid keys: a key the grid does not know is ignored", "[ui][grid][cov]") {
    GridFixture f{1};
    f.grid->focusCell(at(1, "note"));
    CHECK(f.grid->press("Ctrl+Q") == ui::KeyOutcome::Ignored);
}
