// SPDX-License-Identifier: Apache-2.0

// A model table's grid: the text of a cell by the kind of the value it holds, and the draft an editor opens with when
// the value does not fit the field.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <morph/ui/value.hpp>
#include <string>

#include "grid_test_support.hpp"
#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace table_model_support;
using morph::test::grid::at;
using morph::test::grid::ModelGridFixture;
using morph::ui::Value;

}  // namespace

TEST_CASE("cov model grid: a cell shows the value it holds as its kind", "[ui][grid][cov]") {
    ModelGridFixture f{{1}};
    auto const shown = [&f](Value value) {
        morph::test::grid::setField(*f.rows, 1, "note", std::move(value));
        f.source->touch();
        return f.source->text(key(1), "note");
    };
    CHECK(shown(Value{true}) == "true");
    CHECK(shown(Value{std::int64_t{7}}) == "7");
    CHECK(shown(Value{morph::ui::Decimal{morph::table::parseDecimal("1.5").value()}}) == "1.5");
    CHECK(shown(Value{morph::ui::Quantity{.value = morph::table::parseDecimal("2").value(), .unit = "g"}}) == "2");
    CHECK(shown(Value{std::string{"text"}}) == "text");
    CHECK(shown(Value{morph::ui::ValueList{}}).empty());  // a list is no cell
    CHECK(shown(Value{}).empty());
}

TEST_CASE("cov model grid: an editor opens blank on a value its field cannot read", "[ui][grid][cov]") {
    // A fresh grid for each value: an editor keeps the value it first loaded for a row.
    auto const opened = [](std::string const& column, Value value) {
        ModelGridFixture f{{1}};
        morph::test::grid::setField(*f.rows, 1, column, std::move(value));
        f.source->touch();
        f.grid->focusCell(at(1, column));
        f.grid->press("F2");
        std::string draft = f.source->draft(key(1), column);
        f.grid->press("Escape");
        CHECK(f.commits.sent.empty());
        return draft;
    };
    CHECK(opened("day", Value{std::string{"junk"}}).empty());        // not a YYYY-MM-DD text
    CHECK(opened("day", Value{std::string{"2026-13-40"}}).empty());  // not a date
    CHECK(opened("day", Value{std::string{"2026-11-20"}}) == "2026-11-20");  // read as a day, shown as one
    CHECK(opened("at", Value{std::string{"junk"}}).empty());           // not an instant
    auto const weight = [](std::string unit) {
        return Value{morph::ui::Quantity{.value = morph::table::parseDecimal("2").value(), .unit = std::move(unit)}};
    };
    CHECK(opened("weight", weight("m")).empty());          // a unit of another dimension
    CHECK(opened("weight", weight("kg")) == "2.000");  // the field's own unit
}
