// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/render/locale_format.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string_view>

namespace {

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::render::NumericLocale;
using morph::render::parseDecimal;

constexpr NumericLocale kCLocale{};
constexpr NumericLocale kGerman{.decimalSeparator = ",", .groupSeparator = "."};

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{0}};
}

}  // namespace

TEST_CASE("parseDecimal: plain text is the exact value it spells", "[render][parse-decimal]") {
    auto const value = parseDecimal("2650.5", kCLocale);
    REQUIRE(value.has_value());
    CHECK(*value == exact(26505, 10));
    CHECK(value->decimalPlaces.value == 1);
    CHECK(parseDecimal("-0.25", kCLocale) == exact(-1, 4));
    CHECK(parseDecimal("007.50", kCLocale) == exact(75, 10));
    CHECK(parseDecimal("0", kCLocale) == exact(0, 1));
}

TEST_CASE("parseDecimal: locale grouping, separators and signs are read through the control edge",
          "[render][parse-decimal]") {
    CHECK(parseDecimal("1.050,25", kGerman) == exact(105025, 100));
    CHECK(parseDecimal("+1.050,25", kGerman) == exact(105025, 100));
    CHECK(parseDecimal("1.5", kGerman) == std::nullopt);  // a group that is too short, not 15
    NumericLocale const minusSign{.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = "\xE2\x88\x92"};
    CHECK(parseDecimal("\xE2\x88\x92"
                       "5",
                       minusSign) == exact(-5, 1));       // U+2212
    NumericLocale const arabic{.zeroDigit = "\xD9\xA0"};  // U+0660
    CHECK(parseDecimal("\xD9\xA5", arabic) == exact(5, 1));
}

TEST_CASE("parseDecimal: a declared precision rejects more places and tags the value", "[render][parse-decimal]") {
    CHECK(parseDecimal("12.55", kCLocale, 1U) == std::nullopt);
    auto const typed = parseDecimal("12.5", kCLocale, 1U);
    REQUIRE(typed.has_value());
    CHECK(*typed == exact(125, 10));
    CHECK(typed->decimalPlaces.value == 1);
    auto const whole = parseDecimal("12", kCLocale, 3U);
    REQUIRE(whole.has_value());
    CHECK(whole->decimalPlaces.value == 3);
    auto const wide = parseDecimal("1.5", kCLocale, 30U);
    REQUIRE(wide.has_value());
    CHECK(wide->decimalPlaces.value == morph::math::kMaxDecimalPlaces);
}

TEST_CASE("parseDecimal: malformed shapes are refused rather than guessed", "[render][parse-decimal]") {
    for (std::string_view const text : {"", "-", "5.", ".5", "-.5", "1e5", "abc", "1-2", "3,5", " 5", "5 ", "--1"}) {
        INFO("accepted '" << text << "'");
        CHECK_FALSE(parseDecimal(text, kCLocale).has_value());
    }
}

TEST_CASE("parseDecimal: a value outside the int64 envelope is refused", "[render][parse-decimal]") {
    CHECK(parseDecimal("9223372036854775807", kCLocale) == exact(9223372036854775807, 1));
    CHECK(parseDecimal("-9223372036854775807", kCLocale) == exact(-9223372036854775807, 1));
    CHECK_FALSE(parseDecimal("9223372036854775808", kCLocale).has_value());
    CHECK_FALSE(parseDecimal("92233720368547758.08", kCLocale).has_value());
    CHECK_FALSE(parseDecimal("0.1234567890123456789", kCLocale).has_value());  // 19 places
    CHECK(parseDecimal("0.123456789012345678", kCLocale).has_value());         // 18 places
}
