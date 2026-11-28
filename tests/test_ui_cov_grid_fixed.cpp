// SPDX-License-Identifier: Apache-2.0

// A fixed grid's rows as the source keeps them: what it refuses to be built from, the rows it reverts, and the cells
// it counts.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <morph/table/data_source.hpp>
#include <morph/ui/grid.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::fieldCell;
using morph::test::grid::FixedGridFixture;
using morph::test::grid::textCell;
using namespace morph;
using Parts = std::vector<std::variant<ui::FixedRow, ui::FixedBlock>>;

ui::FixedRow fixedRow(std::string const& key, std::map<std::string, ui::FixedCell, std::less<>> cells) {
    return ui::FixedRow{.key = key, .label = key, .cells = std::move(cells)};
}

std::vector<ui::GridColumn> columns(std::vector<std::string> const& ids) {
    std::vector<ui::GridColumn> out;
    for (std::string const& id : ids) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): a fixture list built once
        out.push_back(ui::GridColumn{.id = id, .label = id, .field = {}, .value = {}});
    }
    return out;
}

Parts limits() {
    return {fixedRow("density", {{"min", fieldCell("limits.density.min")}, {"max", fieldCell("limits.density.max")}}),
            fixedRow("count", {{"min", fieldCell("limits.count")}, {"max", textCell("none")}})};
}

ui::CellRef cell(std::string row, std::string column) {
    return ui::CellRef{.key = ui::Key{std::move(row)}, .column = std::move(column)};
}

}  // namespace

TEST_CASE("cov fixed source: a row with no key, or a key given twice, is refused", "[ui][grid][fixed][cov]") {
    using Catch::Matchers::ContainsSubstring;
    Parts empty = limits();
    empty.emplace_back(fixedRow("", {{"min", textCell("x")}}));
    CHECK_THROWS_WITH((FixedGridFixture{R"({"limits":{}})", std::move(empty), columns({"min", "max"})}),
                      ContainsSubstring("has no key"));
    Parts twice = limits();
    twice.emplace_back(fixedRow("density", {{"min", textCell("x")}}));
    CHECK_THROWS_WITH((FixedGridFixture{R"({"limits":{}})", std::move(twice), columns({"min", "max"})}),
                      ContainsSubstring("`density` is given twice"));
}

TEST_CASE("cov fixed source: opening an editor offers the field's options, and a row reverts to what it loaded",
          "[ui][grid][fixed][cov]") {
    FixedGridFixture f{R"({"limits":{"count":3}})", limits(), columns({"min", "max"})};
    f.grid->focusCell(cell("count", "min"));
    f.grid->type("9");
    f.grid->press("Enter");
    f.grid->focusCell(cell("density", "min"));
    f.grid->type("1,5");
    f.grid->press("Enter");
    CHECK(f.source->modifiedRows() == 2);
    CHECK(f.source->draft(ui::Key{std::string{"count"}}, "min") == "9");
    f.source->revertCell(ui::Key{std::string{"count"}}, "min");
    CHECK(f.source->draft(ui::Key{std::string{"count"}}, "min") == "3");
    CHECK(f.source->modifiedRows() == 1);
    f.source->revertRow(ui::Key{std::string{"density"}});
    CHECK(f.source->draft(ui::Key{std::string{"density"}}, "min").empty());
    CHECK(f.source->modifiedRows() == 0);
    f.source->revertRow(ui::Key{std::string{"no such row"}});  // nothing to revert
    CHECK(f.source->modifiedRows() == 0);
}

TEST_CASE("cov fixed source: a cell the form refuses counts as an error", "[ui][grid][fixed][cov]") {
    FixedGridFixture f{R"({"limits":{}})", limits(), columns({"min", "max"})};
    CHECK(f.source->errorCount() == 0);
    f.grid->focusCell(cell("density", "min"));
    f.grid->type("1,2345");  // three places are the most the field takes
    f.grid->press("Enter");
    CHECK(f.source->errorCount() == 1);
    f.grid->focusCell(cell("density", "min"));
    f.grid->type("1,2");
    f.grid->press("Enter");
    CHECK(f.source->errorCount() == 0);
}

TEST_CASE("cov grid rows: a range of keys outside the view is refused, and one inside is listed", "[ui][grid][cov]") {
    morph::test::grid::GridFixture f{3};
    ui::GridRows& rows = f.source->view();
    std::optional<std::expected<std::vector<table::RowId>, table::TableError>> got;
    rows.keysOf(1, 2).thenDetached([&got](auto const& result) { got = result; });
    f.h.settle();
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    CHECK((*got)->size() == 2);
    got.reset();
    rows.keysOf(2, 5).thenDetached([&got](auto const& result) { got = result; });
    f.h.settle();
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->has_value());
    CHECK(got->error().code == table::TableErrorCode::InvalidSpec);
    got.reset();
    rows.keysOf(-1, 1).thenDetached([&got](auto const& result) { got = result; });
    f.h.settle();
    REQUIRE(got.has_value());
    CHECK_FALSE(got->has_value());
}

TEST_CASE("cov fixed source: a part that names no form, no field or no collection is refused",
          "[ui][grid][fixed][cov]") {
    using Catch::Matchers::ContainsSubstring;
    auto const refusal = [](Parts parts, bool withForm) {
        FixedGridFixture f{R"({"limits":{}})", limits(), columns({"min", "max"})};
        try {
            ui::FixedGridSource const source{f.h.rt, withForm ? f.form.get() : nullptr, std::move(parts),
                                             columns({"min", "max"}), ui::FormatContext{}};
        } catch (std::invalid_argument const& failure) {
            return std::string{failure.what()};
        }
        return std::string{};
    };
    auto const block = [](std::string forEach) {
        Parts parts;
        parts.emplace_back(
            ui::FixedBlock{.forEach = std::move(forEach), .rows = {fixedRow("mass", {{"min", fieldCell("mass")}})}});
        return parts;
    };
    CHECK_THAT(refusal(block("specimens"), false), ContainsSubstring("there is no form"));
    CHECK_THAT(refusal(block("nothing"), true), ContainsSubstring("`nothing` is not a field of the form"));
    CHECK_THAT(refusal(block("limits"), true), ContainsSubstring("`limits` is not a collection"));
    Parts slash;
    slash.emplace_back(fixedRow("a/b", {{"min", textCell("x")}}));
    CHECK_THAT(refusal(std::move(slash), true), ContainsSubstring("holds a `/`"));
    Parts noForm;
    noForm.emplace_back(fixedRow("density", {{"min", fieldCell("limits.density.min")}}));
    CHECK_THAT(refusal(std::move(noForm), false), ContainsSubstring("edits a field, and there is no form"));
    Parts unknown;
    unknown.emplace_back(fixedRow("density", {{"min", fieldCell("limits.nosuch")}}));
    CHECK_THAT(refusal(std::move(unknown), true), ContainsSubstring("`limits.nosuch` is not a field of the form"));
    Parts fine;
    fine.emplace_back(fixedRow("density", {{"min", textCell("x")}}));
    CHECK(refusal(std::move(fine), false).empty());  // a text cell needs no form
}
