// SPDX-License-Identifier: Apache-2.0

// A model table's grid: the text of a cell by the kind of the value it holds, and the draft an editor opens with when
// the value does not fit the field.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/table/view_change.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <vector>

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
    CHECK(opened("day", Value{std::string{"junk"}}).empty());                // not a YYYY-MM-DD text
    CHECK(opened("day", Value{std::string{"2026-13-40"}}).empty());          // not a date
    CHECK(opened("day", Value{std::string{"2026-11-20"}}) == "2026-11-20");  // read as a day, shown as one
    CHECK(opened("at", Value{std::string{"junk"}}).empty());                 // not an instant
    auto const weight = [](std::string unit) {
        return Value{morph::ui::Quantity{.value = morph::table::parseDecimal("2").value(), .unit = std::move(unit)}};
    };
    CHECK(opened("weight", weight("m")).empty());      // a unit of another dimension
    CHECK(opened("weight", weight("kg")) == "2.000");  // the field's own unit
}

namespace {

using morph::table::ViewOp;

ViewOp op(ViewOp::Kind kind, std::size_t first, std::size_t count, std::size_t to = 0) {
    return ViewOp{.kind = kind, .first = first, .count = count, .to = to};
}

// The view indices the grid reads after the model's rows announce @p ops.
std::vector<std::int64_t> readAfter(std::vector<ViewOp> ops) {
    ModelGridFixture const f{{1, 2, 3, 4, 5, 6}};
    f.rows->readIndices.clear();
    f.rows->announceChange(std::move(ops));
    std::vector<std::int64_t> read = f.rows->readIndices;
    std::ranges::sort(read);
    return read;
}

}  // namespace

TEST_CASE("cov model grid: the rows a change brings are found through the operations that follow", "[ui][grid][cov]") {
    using Kind = ViewOp::Kind;
    using Indices = std::vector<std::int64_t>;
    CHECK(readAfter({op(Kind::Inserted, 1, 2)}) == Indices{1, 2});
    CHECK(readAfter({op(Kind::Changed, 4, 2)}) == Indices{4, 5});
    // A later removal takes a brought row with it, and moves the ones after it down.
    CHECK(readAfter({op(Kind::Inserted, 1, 2), op(Kind::Removed, 2, 1)}) == Indices{1});
    CHECK(readAfter({op(Kind::Inserted, 3, 2), op(Kind::Removed, 0, 1)}) == Indices{2, 3});
    CHECK(readAfter({op(Kind::Inserted, 1, 1), op(Kind::Removed, 3, 1)}) == Indices{1});  // one before it stays
    // A later insertion moves the brought rows from its place on up.
    CHECK(readAfter({op(Kind::Changed, 2, 1), op(Kind::Inserted, 0, 2)}) == Indices{0, 1, 4});
    CHECK(readAfter({op(Kind::Changed, 2, 1), op(Kind::Inserted, 5, 1)}) == Indices{2, 5});
    // A row changed twice is read once.
    CHECK(readAfter({op(Kind::Changed, 2, 2), op(Kind::Changed, 3, 2)}) == Indices{2, 3, 4});
    // A moved row takes its place; the rows between shift by one.
    CHECK(readAfter({op(Kind::Changed, 1, 1), op(Kind::Moved, 1, 1, 4)}) == Indices{4});
    CHECK(readAfter({op(Kind::Changed, 3, 1), op(Kind::Moved, 1, 1, 4)}) == Indices{2});
    CHECK(readAfter({op(Kind::Changed, 3, 1), op(Kind::Moved, 5, 1, 1)}) == Indices{4});
    CHECK(readAfter({op(Kind::Changed, 0, 1), op(Kind::Moved, 5, 1, 3)}) == Indices{0});
    CHECK(readAfter({op(Kind::Changed, 5, 1), op(Kind::Moved, 1, 1, 3)}) == Indices{5});
    // A reset forgets what came before it.
    CHECK(readAfter({op(Kind::Changed, 1, 2), op(Kind::Reset, 0, 0), op(Kind::Changed, 5, 1)}) == Indices{5});
}

TEST_CASE("cov model grid: cells held for a row follow it when the row's key changes", "[r2][ui][grid][cov]") {
    ModelGridFixture f{{1, 2}, morph::ui::EditMode::Row};
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    f.grid->press("Tab");
    f.grid->type("b");
    f.grid->press("Tab");
    f.rows->rekey(row(1), row(9));
    f.source->moveKey(key(1), key(9));
    f.grid->focusCell(at(2, "label"));  // focus leaves the row: its held cells are sent
    REQUIRE(f.commits.sent.size() == 1);
    CHECK(f.commits.sent.at(0).row == row(9));
    REQUIRE(f.commits.sent.at(0).cells.size() == 2);
    CHECK(f.commits.sent.at(0).cells.at(0).json == R"("a")");
    CHECK(f.commits.sent.at(0).cells.at(1).json == R"("b")");
}

TEST_CASE("cov model grid: a cell dropped for a busy row is resent under the row's new key", "[r2][ui][grid][cov]") {
    ModelGridFixture f{{1, 2}, morph::ui::EditMode::Row};
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    f.grid->press("Tab");
    f.rows->rowStates.busyKeys(7, {row(1)});  // a document mutation's edit of row 1
    f.source->touch();
    f.grid->focusCell(at(2, "label"));
    CHECK(f.source->resends(key(1), "label"));
    f.rows->rekey(row(1), row(9));
    f.source->moveKey(key(1), key(9));
    CHECK(f.source->resends(key(9), "label"));
    CHECK_FALSE(f.source->resends(key(1), "label"));
}
