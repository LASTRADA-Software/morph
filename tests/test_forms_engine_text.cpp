// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <compare>
#include <morph/forms/engine/detail/text.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

namespace engine = morph::forms::detail::engine;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

TEST_CASE("engine text: quote matches JSON.stringify byte for byte", "[forms-engine][text]") {
    CHECK(engine::quote("plain") == R"("plain")");
    CHECK(engine::quote("a\"b\\c") == R"("a\"b\\c")");
    CHECK(engine::quote("tab\tnl\ncr\r") == R"("tab\tnl\ncr\r")");
    CHECK(engine::quote(std::string{"\x01"}) == R"("\u0001")");
    CHECK(engine::quote("kg/m\xC2\xB3") == "\"kg/m\xC2\xB3\"");  // non-ASCII passes through
}

TEST_CASE("engine text: numbers keep every digit, and a double is written in fixed notation", "[forms-engine][text]") {
    auto const dom = engine::parseJson(R"({"big":18446744073709551615,"neg":-9223372036854775808,"small":1e-7,)"
                                       R"("max":1.7976931348623157e+308,"half":0.5,"text":"x"})");
    REQUIRE(dom.has_value());
    CHECK(engine::numberText(*engine::member(*dom, "big")) == "18446744073709551615");
    CHECK(engine::numberText(*engine::member(*dom, "neg")) == "-9223372036854775808");
    CHECK(engine::numberText(*engine::member(*dom, "small")) == "0.0000001");
    CHECK(engine::numberText(*engine::member(*dom, "half")) == "0.5");
    CHECK(engine::numberText(*engine::member(*dom, "max"))->size() == 309);
    CHECK_FALSE(engine::numberText(*engine::member(*dom, "text")).has_value());
    CHECK(engine::toJson(*engine::member(*dom, "big")) == "18446744073709551615");
    CHECK(engine::displayText(*engine::member(*dom, "text")) == "x");
    CHECK_FALSE(engine::parseJson("not json").has_value());
}

TEST_CASE("engine text: compareDecimal orders canonical decimals at any magnitude", "[forms-engine][text]") {
    using std::strong_ordering;
    CHECK(engine::compareDecimal("9223372036854775808", "9223372036854775807") == strong_ordering::greater);
    CHECK(engine::compareDecimal("-9223372036854775809", "-9223372036854775808") == strong_ordering::less);
    CHECK(engine::compareDecimal("0009223372036854775807", "9223372036854775807") == strong_ordering::equal);
    CHECK(engine::compareDecimal("0.25", "0.5") == strong_ordering::less);
    CHECK(engine::compareDecimal("2.50", "2.5") == strong_ordering::equal);
    CHECK(engine::compareDecimal("-0", "0") == strong_ordering::equal);
    CHECK(engine::compareDecimal("-40.5", "-41") == strong_ordering::greater);
}

TEST_CASE("engine text: digit multiplication is exact", "[forms-engine][text]") {
    CHECK(engine::multiplyDigits("123", "45") == "5535");
    CHECK(engine::multiplyDigits("0", "999") == "0");
    CHECK(engine::multiplyDigits("9223372036854775807", "1000") == "9223372036854775807000");
}

TEST_CASE("engine text: a Rational compares exactly against a decimal", "[forms-engine][text]") {
    Rational const third{Numerator{1}, Denominator{3}, DecimalPlaces{2}};
    CHECK(engine::compareRationalToDecimal(third, "0.3333") == std::strong_ordering::greater);
    CHECK(engine::compareRationalToDecimal(third, "0.3334") == std::strong_ordering::less);
    Rational const big{Numerator{9223372036854775807}, Denominator{1}, DecimalPlaces{0}};
    CHECK(engine::compareRationalToDecimal(big, std::string(309, '9')) == std::strong_ordering::less);
    CHECK(engine::compareRationalToDecimal(-big, "-100000000000000000000") == std::strong_ordering::greater);
}

TEST_CASE("engine text: decimalText rounds half away from zero at the requested places", "[forms-engine][text]") {
    CHECK(engine::decimalText(Rational{Numerator{5}, Denominator{4}, DecimalPlaces{2}}, 2) == "1.25");
    CHECK(engine::decimalText(Rational{Numerator{-1}, Denominator{3}, DecimalPlaces{2}}, 2) == "-0.33");
    CHECK(engine::decimalText(Rational{Numerator{245050}, Denominator{100}, DecimalPlaces{2}}, 2) == "2450.50");
    CHECK(engine::decimalText(Rational{Numerator{15}, Denominator{10}, DecimalPlaces{1}}, 1) == "1.5");
    CHECK(engine::decimalText(Rational{Numerator{-1}, Denominator{1000}, DecimalPlaces{3}}, 2) == "0.00");
    CHECK(engine::decimalText(Rational{Numerator{7}, Denominator{1}, DecimalPlaces{0}}, 0) == "7");
}

TEST_CASE("engine text: wall-clock and instant conversion", "[forms-engine][text]") {
    using namespace std::chrono_literals;
    auto const wall = engine::parseWallClock("2026-07-05T14:30");
    REQUIRE(wall.has_value());
    CHECK(engine::formatUtc(*wall) == "2026-07-05T14:30:00Z");
    CHECK(engine::formatWallClock(*wall + 2h) == "2026-07-05T16:30:00");
    CHECK(engine::parseWallClock("2026-07-05T14:30:15").has_value());
    CHECK_FALSE(engine::parseWallClock("not-a-date").has_value());
    CHECK_FALSE(engine::parseWallClock("2026-02-30T10:00").has_value());
    CHECK_FALSE(engine::parseWallClock("2026-07-05T24:00").has_value());
    auto const zoned = engine::parseInstant("2026-01-01T00:30:00+01:00");
    REQUIRE(zoned.has_value());
    CHECK(engine::formatWallClock(*zoned) == "2025-12-31T23:30:00");
    CHECK(engine::parseInstant("2026-07-20T09:00:00Z").has_value());
    CHECK(engine::parseInstant("2026-07-20T09:00:00.123Z").has_value());
    CHECK(engine::parseInstant("2026-07-20T09:00:00").has_value());
    CHECK(engine::isIsoDate("2026-02-28"));
    CHECK_FALSE(engine::isIsoDate("2026-02-30"));
    CHECK_FALSE(engine::isIsoDate("2026-2-3"));
}
