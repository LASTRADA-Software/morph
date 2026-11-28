// SPDX-License-Identifier: Apache-2.0
//
// The text a filter operand or a typed cell is parsed from: whole integers, reals and a measure's amount and unit.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <morph/table/sort.hpp>
#include <optional>
#include <string_view>

using namespace morph::table;

TEST_CASE("an integer is the whole text or nothing", "[covc][table][parse]") {
    CHECK(detail::parseInteger("42") == 42);
    CHECK(detail::parseInteger("-7") == -7);
    CHECK_FALSE(detail::parseInteger(""));
    CHECK_FALSE(detail::parseInteger("4 2"));
    CHECK_FALSE(detail::parseInteger("4.0"));
    CHECK_FALSE(detail::parseInteger("x"));
    CHECK_FALSE(detail::parseInteger("99999999999999999999"));
}

TEST_CASE("a real is the whole text, never NaN, by either reader", "[covc][table][parse]") {
    for (auto const parse : {&detail::parseReal, &detail::parseRealClassic}) {
        CHECK(parse("1.5") == 1.5);
        CHECK(parse("-2e3") == -2000.0);
        CHECK_FALSE(parse(""));
        CHECK_FALSE(parse("1.5x"));
        CHECK_FALSE(parse("abc"));
        CHECK_FALSE(parse("nan"));
        CHECK_FALSE(parse(" 1"));
    }
    CHECK_FALSE(detail::parseRealClassic("+1"));
    auto const inf = detail::parseReal("inf");
    REQUIRE(inf.has_value());
    CHECK(std::isinf(*inf));
}

TEST_CASE("a measure operand is a decimal with at most one unit id", "[covc][table][parse]") {
    auto const bare = detail::splitMeasureOperand("250");
    REQUIRE(bare);
    CHECK(bare->first.numerator == 250);
    CHECK(bare->second.empty());
    auto const named = detail::splitMeasureOperand("0.3 kg");
    REQUIRE(named);
    CHECK(named->second == "kg");
    CHECK_FALSE(detail::splitMeasureOperand("x kg"));
    CHECK_FALSE(detail::splitMeasureOperand("1 "));
    CHECK_FALSE(detail::splitMeasureOperand("1 k g"));
    CHECK_FALSE(detail::splitMeasureOperand(""));
}

TEST_CASE("a dimension's place follows the column's list, else the registry's", "[covc][table][parse]") {
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(UnitInfo{.id = "kg",
                                 .symbol = "kg",
                                 .dimension = "mass",
                                 .toBase = morph::math::Rational{1, morph::math::DecimalPlaces{0}},
                                 .decimals = 0}));
    REQUIRE(builder.add(UnitInfo{.id = "m",
                                 .symbol = "m",
                                 .dimension = "length",
                                 .toBase = morph::math::Rational{1, morph::math::DecimalPlaces{0}},
                                 .decimals = 0}));
    auto const registry = std::move(builder).build();
    ColumnInfo listed;
    listed.dimensions = {"length", "mass"};
    CHECK(detail::dimensionPlace(listed, *registry, "mass") == 1);
    CHECK(detail::dimensionPlace(listed, *registry, "time") == -1);
    ColumnInfo const open;
    CHECK(detail::dimensionPlace(open, *registry, "mass") >= 0);
    CHECK(detail::dimensionPlace(open, *registry, "time") == -1);
}
