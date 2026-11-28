// SPDX-License-Identifier: Apache-2.0

// The edges of how a table column shows its cells (`ui::formatCell` and the helpers under `detail::cells`): digit
// splitting, rounding carries, unreadable and overflowing values, units, dates and value labels.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
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
namespace cells = morph::ui::detail::cells;

morph::math::Rational dec(std::string_view text) { return morph::table::parseDecimal(text).value(); }
TableColumn column(ColumnKind kind, morph::table::ColumnDisplay display = {}) {
    return TableColumn{.info = {.id = "v", .kind = kind}, .label = "V", .display = std::move(display)};
}
morph::math::Rational huge() {
    return morph::math::Rational{std::numeric_limits<std::int64_t>::max(), morph::math::DecimalPlaces{2}};
}

struct Units {
    std::shared_ptr<morph::table::UnitRegistry const> registry;
    morph::table::UnitId kg = 0;
    morph::table::UnitId g = 0;
    morph::table::UnitId meter = 0;
    morph::table::UnitId big = 0;
};
Units makeUnits() {
    morph::table::UnitRegistryBuilder builder;
    REQUIRE(builder.add({.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}));
    REQUIRE(builder.add({.id = "g", .symbol = "g", .dimension = "mass", .toBase = dec("0.001"), .decimals = 0}));
    REQUIRE(builder.add({.id = "m", .symbol = "", .dimension = "length", .toBase = dec("1"), .decimals = 1}));
    REQUIRE(builder.add({.id = "big", .symbol = "B", .dimension = "mass", .toBase = huge(), .decimals = 0}));
    Units out;
    out.registry = std::move(builder).build();
    out.kg = *out.registry->find("kg");
    out.g = *out.registry->find("g");
    out.meter = *out.registry->find("m");
    out.big = *out.registry->find("big");
    return out;
}
}  // namespace

TEST_CASE("cov cell format: digitsOf splits a sign, whole and fraction and refuses malformed text",
          "[ui][cell_format][cov]") {
    auto const negative = cells::digitsOf("-12.50");
    REQUIRE(negative.has_value());
    CHECK(negative->negative);
    CHECK(negative->whole == "12");
    CHECK(negative->fraction == "50");
    auto const plain = cells::digitsOf("7");
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->negative);
    CHECK(plain->fraction.empty());
    CHECK_FALSE(cells::digitsOf("").has_value());
    CHECK_FALSE(cells::digitsOf(".5").has_value());
    CHECK_FALSE(cells::digitsOf("1.").has_value());
    CHECK_FALSE(cells::digitsOf("1.2x").has_value());
    CHECK_FALSE(cells::digitsOf("x1").has_value());
    CHECK_FALSE(cells::digitsOf("-").has_value());
}

TEST_CASE("cov cell format: textOf drops the sign of a zero and keeps it for anything else",
          "[ui][cell_format][cov]") {
    CHECK(cells::textOf({.negative = true, .whole = "0", .fraction = "00"}) == "0.00");
    CHECK(cells::textOf({.negative = true, .whole = "0", .fraction = "01"}) == "-0.01");
    CHECK(cells::textOf({.negative = false, .whole = "5", .fraction = ""}) == "5");
    CHECK(cells::textOf({.negative = true, .whole = "5", .fraction = ""}) == "-5");
}

TEST_CASE("cov cell format: roundText rounds half cases, carries and pads or trims", "[ui][cell_format][cov]") {
    auto const round = [](std::string_view text, std::size_t places, Rounding mode, bool pad) {
        return cells::roundText(*cells::digitsOf(text), places, mode, pad);
    };
    // A half with nothing after it goes to the even digit, or up under HalfUp.
    CHECK(round("2.5", 0, Rounding::HalfEven, true) == "2");
    CHECK(round("3.5", 0, Rounding::HalfEven, true) == "4");
    CHECK(round("2.5", 0, Rounding::HalfUp, true) == "3");
    CHECK(round("0.125", 2, Rounding::HalfEven, true) == "0.12");
    CHECK(round("0.135", 2, Rounding::HalfEven, true) == "0.14");
    // A half followed by a non-zero digit is above the half and rounds up in every mode.
    CHECK(round("2.51", 0, Rounding::HalfEven, true) == "3");
    CHECK(round("2.49", 0, Rounding::HalfUp, true) == "2");
    CHECK(round("2.6", 0, Rounding::HalfEven, true) == "3");
    // A carry runs through nines, and out of the whole part.
    CHECK(round("9.99", 1, Rounding::HalfUp, true) == "10.0");
    CHECK(round("99.9", 0, Rounding::HalfUp, true) == "100");
    CHECK(round("1.999", 2, Rounding::HalfUp, true) == "2.00");
    // Padding or trimming applies when nothing was dropped.
    CHECK(round("1.5", 3, Rounding::HalfEven, true) == "1.500");
    CHECK(round("1.500", 3, Rounding::HalfEven, false) == "1.5");
    CHECK(round("1.000", 3, Rounding::HalfEven, false) == "1");
    CHECK(round("-0.001", 2, Rounding::HalfEven, true) == "0.00");
}

TEST_CASE("cov cell format: exactText writes a negative rational and long fractions", "[ui][cell_format][cov]") {
    CHECK(cells::exactText(dec("-12.5"), 3) == "-12.500");
    CHECK(cells::exactText(morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{3},
                                                 morph::math::DecimalPlaces{4}},
                           4) == "0.3333");
    CHECK(cells::exactText(dec("7"), 0) == "7");
}

TEST_CASE("cov cell format: doubleText refuses a value with no fixed text", "[ui][cell_format][cov]") {
    CHECK_FALSE(cells::doubleText(std::nan("")).has_value());
    CHECK_FALSE(cells::doubleText(std::numeric_limits<double>::infinity()).has_value());
    CHECK(cells::doubleText(0.5) == "0.5");
    CHECK(cells::doubleText(-1e21) == "-1000000000000000000000");
}

TEST_CASE("cov cell format: readText reads text by the column's kind", "[ui][cell_format][cov]") {
    CHECK(cells::readText("12", ColumnKind::Integer) == Cell{std::int64_t{12}});
    CHECK_FALSE(cells::readText("12x", ColumnKind::Integer).has_value());
    CHECK_FALSE(cells::readText("", ColumnKind::Integer).has_value());
    CHECK(cells::readText("7", ColumnKind::Key) == Cell{std::int64_t{7}});
    CHECK(cells::readText("abc", ColumnKind::Key) == Cell{std::string{"abc"}});
    CHECK(cells::readText("", ColumnKind::Key) == Cell{std::string{}});
    CHECK(cells::readText("1.5", ColumnKind::Decimal) == Cell{dec("1.5")});
    CHECK_FALSE(cells::readText("x", ColumnKind::Decimal).has_value());
    CHECK(cells::readText("1.5", ColumnKind::Quantity) == Cell{dec("1.5")});
    CHECK(cells::readText("2.5", ColumnKind::Number) == Cell{dec("2.5")});
    auto const real = cells::readText("1e+21", ColumnKind::Number);
    REQUIRE(real.has_value());
    CHECK(std::holds_alternative<double>(*real));
    CHECK_FALSE(cells::readText("zzz", ColumnKind::Number).has_value());
    CHECK(cells::readText("true", ColumnKind::Bool) == Cell{true});
    CHECK(cells::readText("false", ColumnKind::Bool) == Cell{false});
    CHECK_FALSE(cells::readText("maybe", ColumnKind::Bool).has_value());
    CHECK(cells::readText("x", ColumnKind::Text) == Cell{std::string{"x"}});
    CHECK(cells::readText("x", ColumnKind::Date) == Cell{std::string{"x"}});
    CHECK(cells::readText("x", ColumnKind::Custom) == Cell{std::string{"x"}});
    CHECK(cells::readText("x", ColumnKind::Measure) == Cell{std::string{"x"}});
    // A cell that is not text passes through, and text is read by the column's kind.
    CHECK(cells::normalized(Cell{true}, ColumnKind::Integer) == Cell{true});
    CHECK(cells::normalized(Cell{std::string{"4"}}, ColumnKind::Integer) == Cell{std::int64_t{4}});
    CHECK_FALSE(cells::normalized(Cell{std::string{"x"}}, ColumnKind::Integer).has_value());
}

TEST_CASE("cov cell format: numberText follows the display for each numeric cell", "[ui][cell_format][cov]") {
    // A rational shows its own places without `decimals`, rounds half away from zero in that case.
    CHECK(cells::numberText(Cell{dec("1.50")}, {}) == "1.50");
    CHECK(cells::numberText(Cell{dec("-2.5")}, {.decimals = 0, .rounding = Rounding::HalfUp}) == "-3");
    CHECK(cells::numberText(Cell{dec("2.5")}, {.decimals = 0, .rounding = Rounding::HalfEven}) == "2");
    // Without padding the zeros after the point go.
    CHECK(cells::numberText(Cell{dec("1.5")}, {.decimals = 3, .padPlaces = false}) == "1.5");
    CHECK(cells::numberText(Cell{dec("1.5")}, {.decimals = 3}) == "1.500");
    // An integer without `decimals` is its own digits.
    CHECK(cells::numberText(Cell{std::int64_t{-12}}, {}) == "-12");
    CHECK(cells::numberText(Cell{std::int64_t{-12}}, {.decimals = 1}) == "-12.0");
    // A double reads from its fixed text.
    CHECK(cells::numberText(Cell{0.25}, {.decimals = 1, .rounding = Rounding::HalfUp}) == "0.3");
    CHECK(cells::numberText(Cell{0.25}, {}) == "0.25");
    CHECK_FALSE(cells::numberText(Cell{std::nan("")}, {}).has_value());
    // A quantity shows its canonical amount.
    auto const grams = morph::table::QuantityCell{.amount = dec("250"), .toCanonical = dec("0.001")};
    CHECK(cells::numberText(Cell{grams}, {.decimals = 2}) == "0.25");
    auto const overflowing = morph::table::QuantityCell{.amount = huge(), .toCanonical = dec("1000")};
    CHECK_FALSE(cells::numberText(Cell{overflowing}, {}).has_value());
    // A rational too large to round to the asked places has no text.
    // Text, booleans and empty cells are not numbers.
    CHECK_FALSE(cells::numberText(Cell{std::string{"1"}}, {}).has_value());
    CHECK_FALSE(cells::numberText(Cell{true}, {}).has_value());
}

TEST_CASE("cov cell format: a measure converts to the chosen unit of its dimension only", "[ui][cell_format][cov]") {
    auto const units = makeUnits();
    morph::ui::DisplayUnits const plain{.units = units.registry.get(), .byDimension = {}};
    using morph::table::Measure;
    // No registry, or a unit the registry does not hold: nothing to show.
    CHECK_FALSE(cells::measureText(Measure{.amount = dec("1"), .unit = units.kg}, {}).has_value());
    CHECK_FALSE(
        cells::measureText(Measure{.amount = dec("1"), .unit = morph::table::kUnknownUnit}, plain).has_value());
    auto const own = cells::measureText(Measure{.amount = dec("1.2345"), .unit = units.kg}, plain);
    REQUIRE(own.has_value());
    CHECK(own->first == "1.234");
    CHECK(own->second == "kg");
    // A dimension with a choice of the unit the cell is already in shows the cell as it is.
    morph::ui::DisplayUnits const same{.units = units.registry.get(), .byDimension = {{"mass", "kg"}}};
    CHECK(cells::measureText(Measure{.amount = dec("2"), .unit = units.kg}, same)->first == "2.000");
    // A chosen unit the registry does not know leaves the cell in its own unit.
    morph::ui::DisplayUnits const unknown{.units = units.registry.get(), .byDimension = {{"mass", "stone"}}};
    CHECK(cells::measureText(Measure{.amount = dec("5"), .unit = units.g}, unknown)->second == "g");
    // A chosen unit of another dimension leaves the cell in its own unit.
    morph::ui::DisplayUnits const crossed{.units = units.registry.get(), .byDimension = {{"mass", "m"}}};
    CHECK(cells::measureText(Measure{.amount = dec("5"), .unit = units.g}, crossed)->second == "g");
    // A conversion that overflows has no text.
    morph::ui::DisplayUnits const toBig{.units = units.registry.get(), .byDimension = {{"mass", "big"}}};
    CHECK(cells::measureText(Measure{.amount = dec("5"), .unit = units.kg}, toBig).has_value());
}

TEST_CASE("cov cell format: dateText falls back to ISO and shows an unspellable value as its number",
          "[ui][cell_format][cov]") {
    morph::ui::FormatContext const bare;  // no patterns: ISO
    auto const date = column(ColumnKind::Date);
    auto const stamp = column(ColumnKind::DateTime);
    CHECK(cells::dateText(0, date, bare) == "1970-01-01");
    CHECK(cells::dateText(3'600, stamp, bare) == "1970-01-01 01:00");
    // Outside the four-digit years a date is its number.
    CHECK(cells::dateText(2'932'897, date, bare) == "2932897");
    CHECK(cells::dateText(-719'529, date, bare) == "-719529");
    CHECK(cells::dateText(2'932'896, date, bare) == "9999-12-31");
    // A date-time a day past either end is out of range in every zone.
    constexpr std::int64_t kDay = 86'400;
    CHECK(cells::dateText((2'932'896 + 2) * kDay, stamp, bare) == std::to_string((2'932'896 + 2) * kDay));
    CHECK(cells::dateText((-719'528 - 2) * kDay, stamp, bare) == std::to_string((-719'528 - 2) * kDay));
    // Inside that margin the zone decides: the last second of 9999 shifted east leaves the range.
    auto east = bare;
    east.utcOffsetMinutes = 120;
    std::int64_t const lastSecond = (2'932'896 + 1) * kDay - 1;
    CHECK(cells::dateText(lastSecond, stamp, bare) == "9999-12-31 23:59");
    CHECK(cells::dateText(lastSecond, stamp, east) == std::to_string(lastSecond));
    auto west = bare;
    west.utcOffsetMinutes = -120;
    std::int64_t const firstSecond = -719'528 * kDay;
    CHECK(cells::dateText(firstSecond, stamp, west) == std::to_string(firstSecond));
    CHECK(cells::dateText(0, stamp, east) == "1970-01-01 02:00");
    // The column's own pattern beats the session's.
    CHECK(cells::dateText(20'000, column(ColumnKind::Date, {.date = "dd-MMM-yyyy"}), formatFor("en")) ==
          "04-Oct-2024");
    // A pattern that writes nothing falls back to ISO.
    CHECK(cells::dateText(20'000, column(ColumnKind::Date, {.date = "dd.MM.yy"}), formatFor("en")) == "2024-10-04");
    CHECK(cells::dateText(20'000, date, formatFor("de")) == "04.10.2024");
}

TEST_CASE("cov cell format: sameOperand compares decimals by value and everything else by text",
          "[ui][cell_format][cov]") {
    CHECK(cells::sameOperand("1.0", "1", ColumnKind::Decimal));
    CHECK(cells::sameOperand("2.50", "2.5", ColumnKind::Number));
    CHECK_FALSE(cells::sameOperand("1.0", "1.1", ColumnKind::Decimal));
    CHECK_FALSE(cells::sameOperand("1.0", "1", ColumnKind::Text));
    CHECK(cells::sameOperand("a", "a", ColumnKind::Decimal));
    CHECK_FALSE(cells::sameOperand("a", "1", ColumnKind::Decimal));
    CHECK_FALSE(cells::sameOperand("1", "a", ColumnKind::Number));
}

TEST_CASE("cov cell format: cellLook marks unreadable, unitless and overflowing cells", "[ui][cell_format][cov]") {
    using morph::ui::CellLook;
    using morph::ui::cellLook;
    auto const units = makeUnits();
    morph::ui::DisplayUnits const shown{.units = units.registry.get(), .byDimension = {}};
    auto const amount = column(ColumnKind::Measure);
    CHECK(cellLook(Cell{morph::table::Measure{.amount = dec("1"), .unit = units.kg}}, amount, shown) ==
          CellLook::Text);
    CHECK(cellLook(Cell{morph::table::Measure{.amount = dec("1"), .unit = units.kg}}, amount) == CellLook::Invalid);
    // A number a column cannot spell is invalid; a date column's number is not asked for text.
    CHECK(cellLook(Cell{std::nan("")}, column(ColumnKind::Number)) == CellLook::Invalid);
    CHECK(cellLook(Cell{std::int64_t{20'000}}, column(ColumnKind::Date)) == CellLook::Text);
    CHECK(cellLook(Cell{std::int64_t{20'000}}, column(ColumnKind::DateTime)) == CellLook::Text);
    CHECK(cellLook(Cell{std::string{"abc"}}, column(ColumnKind::Text)) == CellLook::Text);
}

TEST_CASE("cov cell format: operandOf writes each cell kind as the text a filter takes", "[ui][cell_format][cov]") {
    using morph::ui::operandOf;
    auto const units = makeUnits();
    morph::ui::DisplayUnits const shown{.units = units.registry.get(), .byDimension = {}};
    CHECK(operandOf(Cell{std::int64_t{-4}}, column(ColumnKind::Integer)) == "-4");
    CHECK(operandOf(Cell{dec("1.50")}, column(ColumnKind::Decimal)) == "1.5");
    CHECK(operandOf(Cell{0.25}, column(ColumnKind::Number)) == "0.25");
    CHECK(operandOf(Cell{false}, column(ColumnKind::Bool)) == "false");
    CHECK(operandOf(Cell{std::string{"abc"}}, column(ColumnKind::Text)) == "abc");
    auto const grams = morph::table::QuantityCell{.amount = dec("250"), .toCanonical = dec("0.001")};
    CHECK(operandOf(Cell{grams}, column(ColumnKind::Quantity)) == "0.25");
    auto const overflowing = morph::table::QuantityCell{.amount = huge(), .toCanonical = dec("1000")};
    CHECK_FALSE(operandOf(Cell{overflowing}, column(ColumnKind::Quantity)).has_value());
    CHECK(operandOf(Cell{morph::table::Measure{.amount = dec("2.5"), .unit = units.kg}}, column(ColumnKind::Measure),
                    shown) == "2.5 kg");
    CHECK_FALSE(operandOf(Cell{std::string{"abc"}}, column(ColumnKind::Integer)).has_value());
}

TEST_CASE("cov cell format: cellOfOperand reads an operand by the column's kind and keeps text it cannot read",
          "[ui][cell_format][cov]") {
    using morph::ui::cellOfOperand;
    auto const units = makeUnits();
    morph::ui::DisplayUnits const shown{.units = units.registry.get(), .byDimension = {}};
    CHECK(cellOfOperand("12", column(ColumnKind::Integer)) == Cell{std::int64_t{12}});
    CHECK(cellOfOperand("12x", column(ColumnKind::Integer)) == Cell{std::string{"12x"}});
    CHECK(cellOfOperand("", column(ColumnKind::Key)) == Cell{std::string{}});
    CHECK(cellOfOperand("20000", column(ColumnKind::Date)) == Cell{std::int64_t{20'000}});
    CHECK(cellOfOperand("2024-10-04", column(ColumnKind::DateTime)) == Cell{std::string{"2024-10-04"}});
    CHECK(cellOfOperand("1.5", column(ColumnKind::Decimal)) == Cell{dec("1.5")});
    CHECK(cellOfOperand("x", column(ColumnKind::Quantity)) == Cell{std::string{"x"}});
    CHECK(cellOfOperand("0.5", column(ColumnKind::Number)) == Cell{0.5});
    CHECK(cellOfOperand("n/a", column(ColumnKind::Number)) == Cell{std::string{"n/a"}});
    CHECK(cellOfOperand("true", column(ColumnKind::Bool)) == Cell{true});
    CHECK(cellOfOperand("yes", column(ColumnKind::Bool)) == Cell{std::string{"yes"}});
    CHECK(cellOfOperand("abc", column(ColumnKind::Custom)) == Cell{std::string{"abc"}});
    CHECK(cellOfOperand("2.5 kg", column(ColumnKind::Measure), shown) ==
          Cell{morph::table::Measure{.amount = dec("2.5"), .unit = units.kg}});
    CHECK(cellOfOperand("2.5 stone", column(ColumnKind::Measure), shown) ==
          Cell{morph::table::Measure{.amount = dec("2.5"), .unit = morph::table::kUnknownUnit}});
    CHECK(cellOfOperand("2.5 kg", column(ColumnKind::Measure)) ==
          Cell{morph::table::Measure{.amount = dec("2.5"), .unit = morph::table::kUnknownUnit}});
    CHECK(cellOfOperand("heavy kg", column(ColumnKind::Measure), shown) == Cell{std::string{"heavy kg"}});
}

TEST_CASE("cov cell format: formatCell labels, groups, dates, measures and unspellable numbers",
          "[ui][cell_format][cov]") {
    using morph::ui::formatCell;
    auto const units = makeUnits();
    morph::ui::DisplayUnits const shown{.units = units.registry.get(), .byDimension = {}};
    auto const en = formatFor("en");
    // A label matches a decimal by value.
    auto const labelled = column(ColumnKind::Decimal, {.labels = {{.operand = "1", .text = "one"}}});
    CHECK(formatCell(Cell{dec("1.00")}, labelled, en) == "one");
    CHECK(formatCell(Cell{dec("1.01")}, labelled, en) == "1.01");
    // An empty cell shows the column's text, an unreadable one nothing.
    CHECK(formatCell(Cell{std::monostate{}}, column(ColumnKind::Integer, {.empty = "n/a"}), en) == "n/a");
    CHECK(formatCell(Cell{std::string{"abc"}}, column(ColumnKind::Integer, {.empty = "n/a"}), en).empty());
    // A date-time shows in the session's zone.
    auto zoned = en;
    zoned.utcOffsetMinutes = 90;
    CHECK(formatCell(Cell{std::int64_t{0}}, column(ColumnKind::DateTime), zoned) == "1970-01-01 01:30");
    // A measure with a symbol is "amount symbol"; one without is the amount only.
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("1.5"), .unit = units.kg}}, column(ColumnKind::Measure),
                     en, shown) == "1.500 kg");
    CHECK(formatCell(Cell{morph::table::Measure{.amount = dec("1.5"), .unit = units.meter}},
                     column(ColumnKind::Measure), en, shown) == "1.5");
    // A text cell in a text column is its own text; a boolean is its display text.
    CHECK(formatCell(Cell{std::string{"abc"}}, column(ColumnKind::Text), en) == "abc");
    CHECK(formatCell(Cell{true}, column(ColumnKind::Bool), en) == "true");
}

TEST_CASE("cov cell format: distinctTexts keeps a date bucket's server text and the places of values that collide",
          "[ui][cell_format][cov]") {
    std::vector<morph::table::DistinctValue> const buckets{
        {.text = "2024", .operand = "2024-01-01..2024-12-31", .count = 3},
        {.text = "2025", .operand = "2025-01-01..2025-12-31", .count = 1},
    };
    CHECK(morph::ui::distinctTexts(buckets, column(ColumnKind::Date), formatFor("en")) ==
          std::vector<std::string>{"2024", "2025"});
    std::vector<morph::table::DistinctValue> const alike{
        {.text = {}, .operand = "0.124", .count = 1},
        {.text = {}, .operand = "0.123", .count = 1},
        {.text = "range", .operand = "a..b", .count = 1},
    };
    auto const texts = morph::ui::distinctTexts(alike, column(ColumnKind::Decimal, {.decimals = 2}), formatFor("en"));
    CHECK(texts == std::vector<std::string>{"0.124", "0.123", "range"});
}
