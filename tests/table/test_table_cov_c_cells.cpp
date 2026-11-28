// SPDX-License-Identifier: Apache-2.0
//
// The cell codec of morph::table descriptions: escaping, rational pairs, the cursor, and reading a cell back
// from the JSON text of its shape.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <morph/table/description.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

using morph::table::Cell;
using morph::table::ColumnKind;
using morph::table::Measure;
using morph::table::QuantityCell;
using morph::table::Unloaded;
namespace tdetail = morph::table::detail;
namespace math = morph::math;

math::Rational ratio(std::int64_t numerator, std::int64_t denominator, std::uint32_t places) {
    return math::Rational{math::Numerator{numerator}, math::Denominator{denominator}, math::DecimalPlaces{places}};
}

std::string cellText(Cell const& cell, std::map<morph::table::UnitId, std::string> const& units = {}) {
    std::string out;
    tdetail::appendCellJson(out, cell, units);
    return out;
}

}  // namespace

TEST_CASE("a string is escaped as JSON, character by character", "[covc][table][cells]") {
    std::string out;
    tdetail::appendJsonString(out, "q\"b\\s\bf\fn\nr\rt\t\x01z\x1f/");
    CHECK(out == "\"q\\\"b\\\\s\\bf\\fn\\nr\\rt\\t\\u0001z\\u001f/\"");
    out.clear();
    tdetail::appendJsonString(out, "");
    CHECK(out == "\"\"");
}

TEST_CASE("a rational is written scaled to its places when that is exact and fits", "[covc][table][cells]") {
    auto const written = [](math::Rational const& value) {
        std::string out;
        tdetail::appendRational(out, value);
        return out;
    };
    CHECK(written(ratio(5, 1, 2)) == "[500,100]");
    CHECK(written(ratio(1, 2, 2)) == "[50,100]");
    CHECK(written(ratio(5, 1, 0)) == "[5,1]");
    CHECK(written(ratio(1, 3, 2)) == "[1,3]");
    CHECK(written(ratio(std::numeric_limits<std::int64_t>::max(), 1, 2)) == "[9223372036854775807,1]");
    auto lowest = ratio(1, 1, 2);
    lowest.numerator = std::numeric_limits<std::int64_t>::min();
    CHECK(written(lowest) == "[-9223372036854775808,1]");
    CHECK(written(ratio(-7, 1, 2)) == "[-700,100]");
    auto zero = ratio(1, 1, 2);
    zero.denominator = 0;
    CHECK(written(zero) == "[1,0]");
}

TEST_CASE("a double is written so it never reads back as an integer", "[covc][table][cells]") {
    auto const written = [](double value) {
        std::string out;
        tdetail::appendDouble(out, value);
        return out;
    };
    CHECK(written(std::numeric_limits<double>::quiet_NaN()) == "\"nan\"");
    CHECK(written(std::numeric_limits<double>::infinity()) == "\"inf\"");
    CHECK(written(-std::numeric_limits<double>::infinity()) == "\"-inf\"");
    CHECK(written(2.0) == "2.0");
    CHECK(written(1.5) == "1.5");
    CHECK(written(1e20).find_first_of(".eE") != std::string::npos);
}

TEST_CASE("every cell alternative is written as its JSON", "[covc][table][cells]") {
    CHECK(cellText(Cell{}) == "null");
    CHECK(cellText(Cell{Unloaded{}}) == "null");
    CHECK(cellText(Cell{std::int64_t{-4}}) == "-4");
    CHECK(cellText(Cell{2.0}) == "2.0");
    CHECK(cellText(Cell{std::string{"a\"b"}}) == "\"a\\\"b\"");
    CHECK(cellText(Cell{true}) == "true");
    CHECK(cellText(Cell{false}) == "false");
    CHECK(cellText(Cell{ratio(3, 2, 1)}) == "[15,10]");
    std::map<morph::table::UnitId, std::string> const units{{7, "kg"}};
    CHECK(cellText(Cell{Measure{.amount = ratio(3, 2, 1), .unit = 7}}, units) == "[15,10,\"kg\"]");
    CHECK(cellText(Cell{Measure{.amount = ratio(3, 2, 1), .unit = 8}}, units) == "[15,10,null]");
    CHECK(cellText(Cell{QuantityCell{.amount = ratio(3, 2, 1), .toCanonical = ratio(1, 1, 0)}}) == "[15,10]");
    CHECK(cellText(Cell{QuantityCell{.amount = ratio(3, 2, 1), .toCanonical = ratio(1000, 1, 0)}}) ==
          "[15,10,[1000,1]]");
    CHECK(cellText(Cell{QuantityCell{.amount = ratio(3, 2, 1), .toCanonical = ratio(1, 2, 1)}}) == "[15,10,[5,10]]");
}

TEST_CASE("a cursor reads whitespace, tokens and integers", "[covc][table][cells]") {
    SECTION("whitespace of every kind is skipped") {
        tdetail::CellCursor cursor{" \t\n\r x"};
        CHECK(cursor.rest() == "x");
        CHECK_FALSE(cursor.atEnd());
        CHECK(cursor.eat('x'));
        CHECK(cursor.atEnd());
        CHECK_FALSE(cursor.eat('x'));
    }
    SECTION("eat leaves a mismatch in place") {
        tdetail::CellCursor cursor{"  ab"};
        CHECK_FALSE(cursor.eat('b'));
        CHECK(cursor.eat('a'));
        CHECK(cursor.rest() == "b");
    }
    SECTION("a number token has its fraction and exponent only when they are complete") {
        using tdetail::CellCursor;
        CHECK(CellCursor::numberToken("12abc") == "12");
        CHECK(CellCursor::numberToken("-3.25,") == "-3.25");
        CHECK(CellCursor::numberToken("1e5") == "1e5");
        CHECK(CellCursor::numberToken("1E+5x") == "1E+5");
        CHECK(CellCursor::numberToken("1e-5") == "1e-5");
        CHECK(CellCursor::numberToken("-").empty());
        CHECK(CellCursor::numberToken("x").empty());
        CHECK(CellCursor::numberToken("").empty());
        CHECK(CellCursor::numberToken("1.").empty());
        CHECK(CellCursor::numberToken("1.x").empty());
        CHECK(CellCursor::numberToken("1e").empty());
        CHECK(CellCursor::numberToken("1e+").empty());
        CHECK(CellCursor::numberToken("1ex").empty());
    }
    SECTION("an integer token is consumed only when it is one that fits") {
        auto const integerOf = [](std::string_view text) { return tdetail::CellCursor{text}.integer(); };
        CHECK(integerOf("  42,") == 42);
        CHECK(integerOf("-9") == -9);
        CHECK_FALSE(integerOf("1.5"));
        CHECK_FALSE(integerOf("1e3"));
        CHECK_FALSE(integerOf(""));
        CHECK_FALSE(integerOf("-"));
        CHECK_FALSE(integerOf("99999999999999999999"));
        tdetail::CellCursor cursor{"x"};
        CHECK_FALSE(cursor.integer());
        CHECK(cursor.rest() == "x");
    }
}

TEST_CASE("a pair reads two integers with a positive denominator", "[covc][table][cells]") {
    auto const pairOf = [](std::string_view text) {
        tdetail::CellCursor cursor{text};
        return tdetail::readPair(cursor);
    };
    auto const good = pairOf("5,100");
    REQUIRE(good);
    CHECK(good->numerator == 1);
    CHECK(good->denominator == 20);
    CHECK(good->decimalPlaces.value == 2);
    CHECK(pairOf("3, 7")->decimalPlaces.value == math::kMaxDecimalPlaces);
    CHECK_FALSE(pairOf("x,1"));
    CHECK_FALSE(pairOf("1 2"));
    CHECK_FALSE(pairOf("1,"));
    CHECK_FALSE(pairOf("1,x"));
    CHECK_FALSE(pairOf("1,0"));
    CHECK_FALSE(pairOf("1,-5"));
}

TEST_CASE("an exact cell reads as a rational or a quantity and refuses any other array", "[covc][table][cells]") {
    auto const read = [](std::string_view text) { return tdetail::readExactCell(text); };
    auto const plain = read("[1,10]");
    REQUIRE(plain);
    CHECK(std::holds_alternative<math::Rational>(*plain));
    auto const quantity = read(" [ 1 , 10 , [ 2 , 1 ] ] ");
    REQUIRE(quantity);
    auto const* const cell = std::get_if<QuantityCell>(&*quantity);
    REQUIRE(cell != nullptr);
    CHECK(cell->toCanonical.numerator == 2);
    for (std::string_view const bad : {"1,10]", "[x,10]", "[1,10] x", "[1,10;", "[1,10,x]", "[1,10,[2,0]]",
                                       "[1,10,[2,1]", "[1,10,[2,1]] y", "[1,10,[2,1]x", "[1,10,[2 1]]"}) {
        CAPTURE(bad);
        CHECK_FALSE(read(bad));
    }
}

TEST_CASE("a measure reads its amount and its unit id, null for no unit", "[covc][table][cells]") {
    auto const read = [](std::string_view text) { return tdetail::readMeasureCell(text); };
    auto const named = read("[15,10, \"kg\" ]");
    REQUIRE(named);
    CHECK(named->first.numerator == 3);
    CHECK(named->first.denominator == 2);
    CHECK(named->second == std::optional<std::string>{"kg"});
    auto const unitless = read("[15,10,null]");
    REQUIRE(unitless);
    CHECK_FALSE(unitless->second);
    for (std::string_view const bad : {"x", "[bad", "[1,10]", "[1,10,]", "[1,10,\"m\"", "[1,10,   ", "[1,10,m]",
                                       "[1,10,\"open]", "[1,0,\"m\"]", "[1 2,\"m\"]"}) {
        CAPTURE(bad);
        CHECK_FALSE(read(bad));
    }
}

TEST_CASE("the non-finite strings read as their doubles", "[covc][table][cells]") {
    CHECK(std::isnan(*tdetail::nonFinite("nan")));
    CHECK(*tdetail::nonFinite("inf") == std::numeric_limits<double>::infinity());
    CHECK(*tdetail::nonFinite("-inf") == -std::numeric_limits<double>::infinity());
    CHECK_FALSE(tdetail::nonFinite("Inf"));
    CHECK_FALSE(tdetail::nonFinite(""));
}

TEST_CASE("a cell is read from the shape of its JSON text", "[covc][table][cells]") {
    auto const read = [](std::string_view text, std::optional<ColumnKind> kind = std::nullopt) {
        return tdetail::cellFromJson(text, kind);
    };
    SECTION("blank text is itself") {
        CHECK(read("") == Cell{std::string{}});
        CHECK(read("  ") == Cell{std::string{"  "}});
    }
    SECTION("literals") {
        CHECK(read(" null ") == Cell{});
        CHECK(read("true") == Cell{true});
        CHECK(read("false\n") == Cell{false});
    }
    SECTION("strings, with the non-finite spellings only in a number column") {
        CHECK(read("\"abc\"") == Cell{std::string{"abc"}});
        CHECK(read("\"nan\"") == Cell{std::string{"nan"}});
        auto const nan = read("\"nan\"", ColumnKind::Number);
        REQUIRE(std::holds_alternative<double>(nan));
        CHECK(std::isnan(std::get<double>(nan)));
        CHECK(read("\"-inf\"", ColumnKind::Number) == Cell{-std::numeric_limits<double>::infinity()});
        CHECK(read("\"xyz\"", ColumnKind::Number) == Cell{std::string{"xyz"}});
        CHECK(read("\"abc") == Cell{std::string{"\"abc"}});
    }
    SECTION("arrays") {
        CHECK(std::holds_alternative<math::Rational>(read("[1,10]")));
        CHECK(read("[1,2,3]") == Cell{std::string{"[1,2,3]"}});
    }
    SECTION("numbers") {
        CHECK(read("42") == Cell{std::int64_t{42}});
        CHECK(read("-5") == Cell{std::int64_t{-5}});
        CHECK(read("42", ColumnKind::Number) == Cell{42.0});
        CHECK(read("1.5") == Cell{1.5});
        CHECK(read("1e3") == Cell{1000.0});
        CHECK(read("99999999999999999999") == Cell{1e20});
        CHECK(read("1e999") == Cell{std::string{"1e999"}});
        CHECK(read("-") == Cell{std::string{"-"}});
        CHECK(read("abc") == Cell{std::string{"abc"}});
        CHECK(read("12x") == Cell{std::string{"12x"}});
    }
}
