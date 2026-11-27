// SPDX-License-Identifier: Apache-2.0

// The grid's keys that take a rectangle, a toggle or a whole column of the grid: Space on a flag, Ctrl+A, Shift and
// the arrows, fill down, and the commands a grid with no cells to act on refuses.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("cov grid keys: the chords a grid claims follow whether an editor is open", "[ui][grid][cov]") {
    GridFixture f{2};
    std::vector<std::string> const idle = f.grid->keyChords();
    CHECK(std::ranges::contains(idle, std::string{"Ctrl+A"}));
    CHECK(std::ranges::contains(idle, std::string{"Shift+Right"}));
    CHECK(std::ranges::contains(idle, std::string{"F2"}));
    f.grid->focusCell(at(1, "note"));
    f.grid->type("x");
    CHECK(f.grid->keyChords() == std::vector<std::string>{"Enter", "Tab", "Shift+Tab", "Escape", "Ctrl+Enter"});
}

TEST_CASE("cov grid keys: activating a read-only cell reports it and opens no editor", "[ui][grid][cov]") {
    std::optional<ui::Key> activated;
    GridFixture f{2, ui::GridOptions{.onActivate = [&activated](ui::Key key) { activated = std::move(key); }}};
    f.grid->activate(at(2, "density"));  // computed, so read-only
    CHECK(activated == ui::Key{std::int64_t{2}});
    CHECK_FALSE(f.grid->editing().has_value());
    f.grid->activate(at(1, "note"));
    CHECK(f.grid->editing() == at(1, "note"));
    CHECK(activated == ui::Key{std::int64_t{2}});  // an editable cell reports nothing
}

TEST_CASE("cov grid keys: a flag opens no editor, and one click on it flips it", "[ui][grid][cov]") {
    GridFixture f{1, ui::GridOptions{}, specimensWithChoices()};
    f.grid->activate(at(1, "checked"));
    CHECK_FALSE(f.grid->editing().has_value());
    CHECK(f.json("specimens[0].checked").empty());
    f.grid->type("x");  // typing into a flag does nothing
    CHECK_FALSE(f.grid->editing().has_value());
    f.grid->click(at(1, "checked"));
    CHECK(f.json("specimens[0].checked") == "true");
    f.grid->click(at(1, "note"));  // a click elsewhere only moves focus
    CHECK(f.json("specimens[0].checked") == "true");
    CHECK(f.grid->focus() == at(1, "note"));
}

TEST_CASE("cov grid keys: a read-only cell refuses typing with a notice", "[ui][grid][cov]") {
    GridFixture f{1};
    f.grid->focusCell(at(1, "limit"));
    f.grid->type("5");
    CHECK_FALSE(f.grid->editing().has_value());
    CHECK(f.grid->message() == "This cell is read-only");
}

TEST_CASE("cov grid keys: a cell's menu reverts the cell and the row and names the row commands", "[ui][grid][cov]") {
    GridFixture f{3};
    ui::Key const first{std::int64_t{1}};
    std::vector<ui::MenuItem> entries = f.grid->cellEntries(first, "note");
    REQUIRE(entries.size() == 6);
    CHECK(entries.at(0).label.constant() == "Revert cell");
    CHECK(entries.at(1).label.constant() == "Revert row");
    CHECK(entries.at(2).separator);
    CHECK(entries.at(3).label.constant() == "Insert row above");
    CHECK(entries.at(4).label.constant() == "Insert row below");
    CHECK(entries.at(5).label.constant() == "Delete row");
    CHECK_FALSE(entries.at(0).enabled.evaluate());  // nothing modified yet
    CHECK_FALSE(entries.at(1).enabled.evaluate());
    f.grid->focusCell(at(1, "note"));
    f.grid->type("changed");
    f.grid->press("Enter");
    entries = f.grid->cellEntries(first, "note");
    CHECK(entries.at(0).enabled.evaluate());
    CHECK(entries.at(1).enabled.evaluate());
    entries.at(0).onSelect();
    CHECK(f.form->field("specimens[0].note").draft().empty());
    f.grid->focusCell(at(1, "note"));
    f.grid->type("again");
    f.grid->press("Enter");
    f.grid->cellEntries(first, "note").at(1).onSelect();
    CHECK(f.form->field("specimens[0].note").draft().empty());
    f.grid->cellEntries(first, "note").at(3).onSelect();  // above the first row
    CHECK(f.form->field("specimens").rows().size() == 4);
}

TEST_CASE("cov grid keys: the row menu deletes the selected rows when the row is among them", "[ui][grid][cov]") {
    GridFixture f{4};
    f.grid->select(ui::CellRange{.anchor = at(2, "note"), .focus = at(3, "note")});
    std::vector<ui::MenuItem> inside = f.grid->rowEntries(ui::Key{std::int64_t{2}});
    REQUIRE(inside.size() == 3);
    CHECK(inside.at(2).label.constant() == "Delete rows");
    std::vector<ui::MenuItem> outside = f.grid->rowEntries(ui::Key{std::int64_t{4}});
    CHECK(outside.at(2).label.constant() == "Delete row");  // a row outside the selection is the only one
    outside.at(2).onSelect();
    CHECK(f.form->field("specimens").rows().size() == 3);
    inside.at(2).onSelect();
    CHECK(f.form->field("specimens").rows().size() == 1);
}

TEST_CASE("cov grid keys: the row menu of fixed rows is empty and a full grid disables insertion", "[ui][grid][cov]") {
    GridFixture fixed{2, ui::GridOptions{.rowsFixed = true}};
    CHECK(fixed.grid->rowEntries(ui::Key{std::int64_t{1}}).empty());
    std::string schema{morph::test::grid::kSpecimens};
    schema.replace(schema.find(R"("maxItems":5)"), std::string_view{R"("maxItems":5)"}.size(), R"("maxItems":2)");
    GridFixture full{2, ui::GridOptions{}, schema};
    std::vector<ui::MenuItem> const entries = full.grid->rowEntries(ui::Key{std::int64_t{1}});
    REQUIRE(entries.size() == 3);
    CHECK_FALSE(entries.at(0).enabled.evaluate());
    CHECK(entries.at(0).description.evaluate() == "A grid holds at most 2 rows");
    CHECK(entries.at(2).enabled.evaluate());
}

TEST_CASE("cov grid keys: the status line's entries undo a deletion and ignore a name they do not know",
          "[ui][grid][cov]") {
    GridFixture f{2};
    f.grid->focusCell(at(2, "note"));
    f.grid->type("kept");
    f.grid->press("Enter");
    f.grid->focusCell(at(2, "note"));
    f.grid->deleteRows();
    REQUIRE(f.form->field("specimens").rows().size() == 1);
    f.grid->chooseEntry("nothing");
    CHECK(f.form->field("specimens").rows().size() == 1);
    f.grid->chooseEntry("undo");
    CHECK(f.form->field("specimens").rows().size() == 2);
}

TEST_CASE("cov grid walk: tab walks down a column in a column walk and skips cells it may not edit",
          "[r2][ui][grid][cov]") {
    GridFixture f{2, ui::GridOptions{.walk = ui::Walk::Columns}};
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::Handled);  // no focus: the first editable cell
    CHECK(f.grid->focus() == at(1, "label"));
    f.grid->press("Tab");
    CHECK(f.grid->focus() == at(2, "label"));
    f.grid->press("Tab");
    CHECK(f.grid->focus() == at(1, "mass"));
    f.grid->press("Shift+Tab");
    CHECK(f.grid->focus() == at(2, "label"));
    f.grid->focusCell(at(2, "volume"));
    f.grid->press("Tab");
    CHECK(f.grid->focus() == at(1, "note"));  // density and limit are read-only
    f.grid->press("Tab");
    CHECK(f.grid->focus() == at(2, "note"));
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);  // past the last cell
    f.grid->focusCell(at(1, "label"));
    CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::FocusPrevious);  // before the first
}

TEST_CASE("cov grid walk: shift tab with no focus starts at the last editable cell", "[r2][ui][grid][cov]") {
    GridFixture f{2};
    CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::Handled);
    CHECK(f.grid->focus() == at(2, "note"));
}

TEST_CASE("cov grid walk: a grid with no rows walks nowhere", "[r2][ui][grid][cov]") {
    GridFixture f{0};
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);
    CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::FocusPrevious);
}

TEST_CASE("cov grid walk: enter moves right, down or nowhere by the option, over cells it may edit",
          "[r2][ui][grid][cov]") {
    {
        GridFixture f{2, ui::GridOptions{.enterMoves = ui::EnterMoves::Right}};
        f.grid->focusCell(at(1, "volume"));
        f.grid->type("1");
        f.grid->press("Enter");
        CHECK(f.grid->focus() == at(1, "note"));  // density and limit are read-only
        f.grid->type("n");
        f.grid->press("Enter");
        CHECK(f.grid->focus() == at(1, "note"));  // the last column: nowhere to go
        CHECK(f.form->field("specimens[0].note").draft() == "n");
    }
    {
        GridFixture f{2, ui::GridOptions{.enterMoves = ui::EnterMoves::None}};
        f.grid->focusCell(at(1, "mass"));
        f.grid->type("2");
        f.grid->press("Enter");
        CHECK(f.grid->focus() == at(1, "mass"));
        CHECK_FALSE(f.grid->editing().has_value());
        CHECK(f.form->field("specimens[0].mass").draft() == "2");
    }
    {
        GridFixture f{2};
        f.grid->focusCell(at(1, "density"));
        f.grid->press("Enter");  // read-only: no editor
        CHECK_FALSE(f.grid->editing().has_value());
        f.grid->focusCell(at(2, "mass"));
        f.grid->type("3");
        f.grid->press("Enter");
        CHECK(f.grid->focus() == at(2, "mass"));  // the last row, and the grid does not append
    }
}
