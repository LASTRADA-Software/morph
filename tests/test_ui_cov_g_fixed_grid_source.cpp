// SPDX-License-Identifier: Apache-2.0

// A fixed grid's source asked about a row or a cell it does not hold, about a cell that is text and no field, and for
// the key of a record of its rows.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <morph/ui/form_rows.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::fieldCell;
using morph::test::grid::FixedGridFixture;
using morph::test::grid::textCell;
namespace ui = morph::ui;
using Parts = std::vector<std::variant<ui::FixedRow, ui::FixedBlock>>;

Parts limits() {
    Parts parts;
    parts.emplace_back(
        ui::FixedRow{.key = "density",
                     .label = "Density",
                     .cells = {{"min", fieldCell("limits.density.min")}, {"max", fieldCell("limits.density.max")}}});
    parts.emplace_back(ui::FixedRow{
        .key = "count", .label = "count", .cells = {{"min", fieldCell("limits.count")}, {"max", textCell("none")}}});
    return parts;
}

std::vector<ui::GridColumn> columns() {
    std::vector<ui::GridColumn> out;
    for (std::string const id : {"min", "max"}) {
        out.push_back(ui::GridColumn{.id = id, .label = id, .field = {}, .value = {}});
    }
    return out;
}

ui::Key rowKey(std::string const& text) { return ui::Key{text}; }

}  // namespace

TEST_CASE("covg fixed grid source: a row or a cell it does not hold is empty and takes nothing", "[covg][ui][grid]") {
    FixedGridFixture f{R"({"limits":{"count":3}})", limits(), columns()};
    ui::FixedGridSource& source = *f.source;
    CHECK(source.label(rowKey("density")) == "Density");
    CHECK(source.label(rowKey("nosuch")).empty());
    CHECK(source.text(rowKey("count"), "max") == "none");
    CHECK(source.text(rowKey("nosuch"), "max").empty());
    CHECK(source.text(rowKey("count"), "zzz").empty());   // a column the row gives no cell
    CHECK(source.draft(rowKey("count"), "max").empty());  // a text cell is no field
    CHECK_FALSE(source.error(rowKey("count"), "max").has_value());
    CHECK_FALSE(source.toggles(rowKey("count"), "max"));
    CHECK(source.readOnly(rowKey("count"), "max"));
    CHECK_FALSE(source.covered(rowKey("nosuch"), "max"));
    CHECK(source.spans(rowKey("nosuch")).empty());
    CHECK_FALSE(source.spanning());
    CHECK_FALSE(source.rowModified(rowKey("nosuch")));
    source.write(rowKey("count"), "max", "x");  // no field to write
    source.editorOpened(rowKey("count"), "max");
    CHECK(source.text(rowKey("count"), "max") == "none");
    ui::CellMarks const marks = source.marks(rowKey("count"), "max");
    CHECK_FALSE(marks.modified);
    CHECK_FALSE(marks.error);
    CHECK_FALSE(marks.stale);
    CHECK(source.editor(rowKey("nosuch"), "max") != nullptr);
}

TEST_CASE("covg fixed grid source: a record's key is its text key", "[covg][ui][grid]") {
    FixedGridFixture f{R"({"limits":{}})", limits(), columns()};
    auto const keyed = [&f](ui::Value member) {
        ui::ValueRecord record;
        if (!member.isNull()) {
            record.insert_or_assign(std::string{ui::kGridKeyField}, std::move(member));
        }
        return f.source->keyOf(ui::Value{std::move(record)});
    };
    CHECK(keyed(ui::Value{std::string{"count"}}) == std::optional<ui::Key>{rowKey("count")});
    CHECK_FALSE(keyed(ui::Value{std::int64_t{1}}).has_value());
    CHECK_FALSE(keyed(ui::Value{}).has_value());
}
