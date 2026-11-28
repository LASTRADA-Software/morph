// SPDX-License-Identifier: Apache-2.0

// A form grid's rows at their bounds and against keys that name nothing: a full grid adds no row, a removal of keys
// that are no rows removes none, an undo after another edit restores nothing, and a record with no key has none.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <vector>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::GridFixture;
namespace ui = morph::ui;

ui::Key rowKey(std::int64_t value) { return ui::Key{value}; }

}  // namespace

TEST_CASE("covg form grid rows: a grid at its most adds no row", "[covg][ui][grid]") {
    GridFixture f{5};
    auto const [least, most] = f.source->bounds();
    CHECK(least == 0);
    CHECK(most == std::optional<std::size_t>{5});
    CHECK_FALSE(f.source->appendRow().has_value());
    CHECK_FALSE(f.source->insertRow(1).has_value());
    CHECK(f.source->rowCount() == 5);
}

TEST_CASE("covg form grid rows: a row is inserted before the row at a place, or after the last", "[covg][ui][grid]") {
    GridFixture f{3};
    std::optional<ui::Key> const before = f.source->insertRow(1);
    REQUIRE(before.has_value());
    CHECK(f.source->rowCount() == 4);
    CHECK(f.source->positionOf(*before) == std::optional<std::size_t>{1});
    std::optional<ui::Key> const last = f.source->insertRow(40);  // no row at that place
    REQUIRE(last.has_value());
    CHECK(f.source->rowCount() == 5);
    CHECK(f.source->positionOf(*last) == std::optional<std::size_t>{4});
}

TEST_CASE("covg form grid rows: keys that are no rows remove none, and an undo after another edit restores none",
          "[covg][ui][grid]") {
    GridFixture f{3};
    std::vector<ui::Key> const none{ui::Key{std::string{"x"}}, rowKey(0), rowKey(-4)};
    CHECK_FALSE(f.source->removeRows(none));
    CHECK(f.source->rowCount() == 3);
    f.source->undoRemove();  // nothing was removed
    CHECK(f.source->rowCount() == 3);
    std::vector<ui::Key> const second{rowKey(2)};
    static_cast<void>(f.source->removeRows(second));
    REQUIRE(f.source->rowCount() == 2);
    REQUIRE(f.source->appendRow().has_value());  // another edit since the removal
    f.source->undoRemove();
    CHECK(f.source->rowCount() == 3);
}

TEST_CASE("covg form grid rows: a record with no key, a text key and an unknown cell have no marks",
          "[covg][ui][grid]") {
    GridFixture f{2};
    CHECK_FALSE(f.source->keyOf(ui::Value{ui::ValueRecord{}}).has_value());
    CHECK(f.source->keyOf(ui::Value{ui::ValueRecord{{std::string{ui::kGridKeyField}, ui::Value{std::int64_t{2}}}}}) ==
          std::optional<ui::Key>{rowKey(2)});
    CHECK_FALSE(f.source->rowModified(ui::Key{std::string{"x"}}));
    CHECK_FALSE(f.source->rowModified(rowKey(-1)));
    f.source->revertRow(ui::Key{std::string{"x"}});  // no row of that key
    f.source->revertCell(rowKey(99), "label");
    ui::CellMarks const marks = f.source->marks(rowKey(99), "label");
    CHECK_FALSE(marks.modified);
    CHECK_FALSE(marks.stale);
    CHECK_FALSE(marks.muted);
}
