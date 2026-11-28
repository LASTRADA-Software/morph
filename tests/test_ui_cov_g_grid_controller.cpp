// SPDX-License-Identifier: Apache-2.0

// A grid's controller asked what it cannot do: typing with nothing focused or on a cell a span covers, focusing a
// covered cell, and focusing a place whose row has no key yet.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::at;
using morph::test::grid::GridFixture;
namespace ui = morph::ui;

}  // namespace

TEST_CASE("covg grid controller: typing with nothing focused writes nothing", "[covg][ui][grid]") {
    GridFixture f{2};
    f.grid->type("x");
    CHECK(f.json("specimens[0].label").empty());
    CHECK_FALSE(f.grid->editing().has_value());
}

TEST_CASE("covg grid controller: a cell a span covers cannot be focused or typed in", "[covg][ui][grid]") {
    GridFixture f{3};
    f.grid->focusCell(at(1, "mass"));
    f.source->setSpans([](morph::forms::RowState const& row) {
        std::vector<ui::CellSpan> spans;
        if (row.key() == 1) {
            spans.push_back({.column = "mass", .rows = 2, .columns = 2});
        }
        return spans;
    });
    auto const before = f.grid->focus();
    f.grid->focusCell(at(2, "mass"));  // the span of (1, mass) covers it
    CHECK(f.grid->focus() == before);
    f.grid->focusCell(at(1, "volume"));
    CHECK(f.grid->focus() == before);
    f.grid->type("9");  // into the cell that spans, not into those it covers
    CHECK_FALSE(f.json("specimens[0].mass").empty());
    CHECK(f.json("specimens[1].mass").empty());
}

TEST_CASE("covg grid controller: a place with no key yet is named by its row once the row is there",
          "[covg][ui][grid]") {
    GridFixture f{3};
    f.grid->focusPlace(ui::GridFocus{.key = std::nullopt, .viewRow = 1, .column = "label"});
    std::optional<ui::GridFocus> const place = f.grid->place();
    REQUIRE(place.has_value());
    CHECK(place->column == "label");
    REQUIRE(place->key.has_value());
    CHECK(*place->key == ui::Key{std::int64_t{2}});
}
