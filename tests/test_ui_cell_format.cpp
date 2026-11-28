// SPDX-License-Identifier: Apache-2.0

// How a table column shows its cells: `ui::formatCell` and its companions (docs/spec/table/table.md, "How a column
// shows its cells" and "Values with units").
//
// Each case names the mutation it was seen to fail on.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/cell_format.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ui_table_support.hpp"

namespace {
using morph::table::Cell;
using morph::table::ColumnKind;
using morph::table::Rounding;
using morph::table::TableColumn;
using morph::ui::testing::formatFor;

morph::math::Rational dec(std::string_view text) { return morph::table::parseDecimal(text).value(); }
TableColumn numeric(ColumnKind kind, morph::table::ColumnDisplay display) {
    return TableColumn{.info = {.id = "v", .kind = kind}, .label = "V", .display = std::move(display)};
}
// formatFor fills the separators and patterns from the locale table. A default-constructed FormatContext has no
// group separator, so `{}` would show 12500 for a grouped column.
morph::ui::FormatContext en() { return formatFor("en"); }  // "," groups, "." decimal, ISO dates
morph::ui::FormatContext de() { return formatFor("de"); }  // "." groups, "," decimal, dd.MM.yyyy
}  // namespace

// Mutation: `rounding` ignored (always HalfEven), which shows "0.12" under HalfUp.
TEST_CASE("format cell: a column's places and rounding", "[ui][cell_format]") {
    using morph::ui::formatCell;
    CHECK(formatCell(Cell{dec("0.125")}, numeric(ColumnKind::Decimal, {.decimals = 2}), en()) == "0.12");
    CHECK(formatCell(Cell{dec("0.125")}, numeric(ColumnKind::Decimal, {.decimals = 2, .rounding = Rounding::HalfUp}),
                     en()) == "0.13");
    CHECK(formatCell(Cell{dec("1.5")}, numeric(ColumnKind::Decimal, {.decimals = 2}), en()) == "1.50");
    CHECK(formatCell(Cell{dec("1.5")}, numeric(ColumnKind::Decimal, {.decimals = 2, .padPlaces = false}), en()) ==
          "1.5");
    CHECK(formatCell(Cell{dec("2")}, numeric(ColumnKind::Decimal, {.decimals = 2, .padPlaces = false}), en()) == "2");
    CHECK(formatCell(Cell{std::int64_t{5}}, numeric(ColumnKind::Integer, {.decimals = 2}), en()) == "5.00");
    CHECK(formatCell(Cell{dec("12.50")}, numeric(ColumnKind::Decimal, {}), en()) == "12.50");  // own places
    CHECK(formatCell(Cell{0.1}, numeric(ColumnKind::Number, {}), en()) == "0.1");              // shortest
    CHECK(formatCell(Cell{std::int64_t{12500}}, numeric(ColumnKind::Integer, {.grouping = true}), en()) == "12,500");
    CHECK(formatCell(Cell{std::int64_t{12500}}, numeric(ColumnKind::Integer, {}), en()) == "12500");
}

// Mutation: a zero value shown as `empty`, which gives "–" for 0.
TEST_CASE("format cell: negative halves and empty text", "[ui][cell_format][review]") {
    using morph::ui::formatCell;
    auto const halfUp = numeric(ColumnKind::Decimal, {.decimals = 2, .rounding = Rounding::HalfUp, .empty = "–"});
    auto const halfEven = numeric(ColumnKind::Decimal, {.decimals = 2, .empty = "–"});
    CHECK(formatCell(Cell{dec("-0.125")}, halfUp, en()) == "-0.13");
    CHECK(formatCell(Cell{dec("-0.125")}, halfEven, en()) == "-0.12");
    CHECK(formatCell(Cell{dec("-0.001")}, halfEven, en()) == "0.00");  // zero shows no sign
    CHECK(formatCell(Cell{std::int64_t{0}}, numeric(ColumnKind::Integer, {.empty = "–"}), en()) == "0");
    CHECK(formatCell(Cell{std::monostate{}}, halfEven, en()) == "–");
    CHECK(formatCell(Cell{std::monostate{}}, numeric(ColumnKind::Decimal, {}), en()).empty());
    CHECK(formatCell(Cell{dec("-12500.5")}, numeric(ColumnKind::Decimal, {.decimals = 2, .grouping = true}), de()) ==
          "-12.500,50");
    CHECK(formatCell(Cell{dec("-12500.5")}, numeric(ColumnKind::Decimal, {.decimals = 2}), de()) == "-12500,50");
}

// Mutation: std::format("{:.2f}") on the binary value, which gives "1.00" for 1.005.
TEST_CASE("format cell: a double rounds from its shortest decimal text", "[ui][cell_format][review]") {
    using morph::ui::formatCell;
    auto const halfUp = numeric(ColumnKind::Number, {.decimals = 2, .rounding = Rounding::HalfUp});
    CHECK(formatCell(Cell{1.005}, halfUp, en()) == "1.01");
    CHECK(formatCell(Cell{2.675}, halfUp, en()) == "2.68");
    CHECK(formatCell(Cell{0.125}, numeric(ColumnKind::Number, {.decimals = 2}), en()) == "0.12");
    CHECK(formatCell(Cell{1e21}, numeric(ColumnKind::Number, {}), en()) == "1000000000000000000000");
}

// Mutation: a Measure shown at the column's `decimals` instead of its unit's, which gives "0.3 kg".
TEST_CASE("format cell: labels, dates, quantities and measures", "[ui][cell_format]") {
    using morph::ui::formatCell;
    auto const available = TableColumn{.info = {.id = "ok", .kind = ColumnKind::Bool},
                                       .display = {.labels = {{.operand = "true", .text = "available"},
                                                              {.operand = "false", .text = "not available"}}}};
    CHECK(formatCell(Cell{true}, available, en()) == "available");
    CHECK(formatCell(Cell{false}, available, en()) == "not available");
    auto const coded = numeric(ColumnKind::Integer, {.labels = {{.operand = "3", .text = "three"}}});
    CHECK(formatCell(Cell{std::int64_t{3}}, coded, en()) == "three");
    CHECK(formatCell(Cell{std::int64_t{4}}, coded, en()) == "4");

    auto const date = TableColumn{.info = {.id = "d", .kind = ColumnKind::Date}};
    CHECK(formatCell(Cell{std::int64_t{20'000}}, date, en()) == "2024-10-04");
    CHECK(formatCell(Cell{std::int64_t{20'000}}, date, de()) == "04.10.2024");
    auto const patterned =
        TableColumn{.info = {.id = "d", .kind = ColumnKind::Date}, .display = {.date = "dd-MMM-yyyy"}};
    CHECK(formatCell(Cell{std::int64_t{20'000}}, patterned, de()) == "04-Oct-2024");

    auto const grams = morph::table::QuantityCell{.amount = dec("250"), .toCanonical = dec("0.001")};
    CHECK(formatCell(Cell{grams}, numeric(ColumnKind::Quantity, {.decimals = 3}), en()) == "0.250");

    morph::table::UnitRegistryBuilder builder;
    REQUIRE(builder.add({.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}));
    REQUIRE(builder.add({.id = "g", .symbol = "g", .dimension = "mass", .toBase = dec("0.001"), .decimals = 0}));
    auto const registry = std::move(builder).build();
    morph::ui::DisplayUnits const units{.units = registry.get(), .byDimension = {}};
    auto const amount = TableColumn{.info = {.id = "amount", .kind = ColumnKind::Measure}};
    auto const kg = *registry->find("kg");
    auto const g = *registry->find("g");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("0.3"), .unit = kg}}, amount, en(), units) ==
          "0.300 kg");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("200"), .unit = g}}, amount, en(), units) == "200 g");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("1.2345"), .unit = kg}}, amount, en(), units) ==
          "1.234 kg");
    morph::ui::DisplayUnits const inKg{.units = registry.get(), .byDimension = {{"mass", "kg"}}};
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("200"), .unit = g}}, amount, en(), inKg) == "0.200 kg");
}

// Mutation: `empty` shown for an unloaded cell, which gives "–".
TEST_CASE("format cell: empty, unloaded and invalid differ", "[ui][cell_format]") {
    using morph::ui::CellLook;
    using morph::ui::cellLook;
    auto const integer = numeric(ColumnKind::Integer, {.empty = "–"});
    CHECK(cellLook(Cell{std::monostate{}}, integer) == CellLook::Empty);
    CHECK(cellLook(Cell{morph::table::Unloaded{}}, integer) == CellLook::Unloaded);
    CHECK(morph::ui::formatCell(Cell{morph::table::Unloaded{}}, integer, en()).empty());
    CHECK(cellLook(Cell{std::string{"abc"}}, integer) == CellLook::Invalid);
    CHECK(morph::ui::formatCell(Cell{std::string{"abc"}}, integer, en()).empty());
    CHECK(cellLook(Cell{std::string{"12"}}, integer) == CellLook::Text);
    CHECK(morph::ui::formatCell(Cell{std::string{"12"}}, numeric(ColumnKind::Integer, {.decimals = 1}), en()) ==
          "12.0");
    auto const amount = TableColumn{.info = {.id = "amount", .kind = ColumnKind::Measure}};
    CHECK(cellLook(Cell{morph::table::Measure{.amount = dec("1"), .unit = morph::table::kUnknownUnit}}, amount) ==
          CellLook::Invalid);
}

// Mutation: the values list formatting every entry by `decimals`, which shows "0.12" twice.
TEST_CASE("format cell: values that show alike keep their places in the values list", "[ui][cell_format]") {
    std::vector<morph::table::DistinctValue> const values{
        {.text = {}, .operand = "0.124", .count = 1},
        {.text = {}, .operand = "0.123", .count = 2},
        {.text = {}, .operand = "0.5", .count = 1},
    };
    CHECK(morph::ui::distinctTexts(values, numeric(ColumnKind::Decimal, {.decimals = 2}), en()) ==
          std::vector<std::string>{"0.124", "0.123", "0.50"});
    CHECK(morph::ui::operandOf(Cell{dec("0.125")}, numeric(ColumnKind::Decimal, {.decimals = 2})) == "0.125");
    CHECK(morph::ui::operandOf(Cell{true}, TableColumn{.info = {.id = "b", .kind = ColumnKind::Bool}}) == "true");
    CHECK_FALSE(morph::ui::operandOf(Cell{std::monostate{}}, numeric(ColumnKind::Integer, {})).has_value());
    CHECK_FALSE(morph::ui::operandOf(Cell{morph::table::Unloaded{}}, numeric(ColumnKind::Integer, {})).has_value());
    CHECK(morph::ui::headerText("Mass", "kg") == "Mass [kg]");
    CHECK(morph::ui::headerText("Mass", "") == "Mass");
    CHECK(morph::ui::headerAccessibleName("Mass", "kg") == "Mass, kg");
}
