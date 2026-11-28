// SPDX-License-Identifier: Apache-2.0

// The 128-bit helpers and the rounding of `Rational`, at the edges where a shift crosses the word, a root is a tie
// and a result no longer fits an `int64`.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/util/rational.hpp>
#include <optional>
#include <vector>

namespace {

namespace math = morph::math;
using math::detail::U128;

constexpr std::int64_t kMax = INT64_MAX;

}  // namespace

TEST_CASE("cov h rational: a shift right moves bits across the word", "[covh][rational]") {
    struct Case {
        U128 value;
        std::uint32_t count;
        U128 expected;
    };
    std::vector<Case> const cases{
        {{.hi = 0xF0F0, .lo = 0x1234}, 0, {.hi = 0xF0F0, .lo = 0x1234}},
        {{.hi = 0x1, .lo = 0x0}, 1, {.hi = 0, .lo = 0x8000000000000000ULL}},
        {{.hi = 0xFF, .lo = 0xFF00}, 8, {.hi = 0, .lo = 0xFF000000000000FFULL}},
        {{.hi = 0xABCD, .lo = 0x1}, 64, {.hi = 0, .lo = 0xABCD}},
        {{.hi = 0xABCD, .lo = 0x1}, 70, {.hi = 0, .lo = 0xABCD >> 6}},
        {{.hi = 0xABCD, .lo = 0x1}, 127, {.hi = 0, .lo = 0}},
    };
    for (auto const& item : cases) {
        INFO("shift by " << item.count);
        auto const shifted = math::detail::shiftRightU128(item.value, item.count);
        CHECK(shifted.hi == item.expected.hi);
        CHECK(shifted.lo == item.expected.lo);
    }
}

TEST_CASE("cov h rational: a root of a fraction is rounded by its tie rule and refused beyond int64",
          "[covh][rational]") {
    struct Case {
        char const* what;
        U128 numerator;
        U128 denominator;
        math::RoundingMode mode;
        std::optional<std::int64_t> expected;
    };
    constexpr auto half = math::RoundingMode::HalfAwayFromZero;
    constexpr auto even = math::RoundingMode::HalfEven;
    std::vector<Case> const cases{
        {"division by zero", {.hi = 0, .lo = 9}, {.hi = 0, .lo = 0}, half, std::nullopt},
        {"2.25 is 1.5, a tie that goes up from an odd root", {.hi = 0, .lo = 9}, {.hi = 0, .lo = 4}, even, 2},
        {"2.25 half away", {.hi = 0, .lo = 9}, {.hi = 0, .lo = 4}, half, 2},
        {"6.25 is 2.5, a tie that stays on an even root", {.hi = 0, .lo = 25}, {.hi = 0, .lo = 4}, even, 2},
        {"6.25 half away", {.hi = 0, .lo = 25}, {.hi = 0, .lo = 4}, half, 3},
        {"a quotient whose root is 2^63", {.hi = 0x4000000000000000ULL, .lo = 0}, {.hi = 0, .lo = 1}, half,
         std::nullopt},
        {"a root of int64 max that would round up", {.hi = 0x3fffffffffffffffULL, .lo = 0x8000000000000001ULL},
         {.hi = 0, .lo = 1}, half, std::nullopt},
        {"a root of int64 max that stays", {.hi = 0x3fffffffffffffffULL, .lo = 0x0000000000000001ULL},
         {.hi = 0, .lo = 1}, half, kMax},
    };
    for (auto const& item : cases) {
        INFO(item.what);
        auto const root = math::detail::roundSqrtOfFraction(item.numerator, item.denominator, 0, item.mode);
        REQUIRE(root.has_value() == item.expected.has_value());
        if (root) {
            CHECK(root->numerator == *item.expected);
            CHECK(root->denominator == 1);
        }
    }
}

TEST_CASE("cov h rational: a rounding whose numerator does not fit is declined", "[covh][rational]") {
    math::Rational const third{math::Numerator{28}, math::Denominator{3}, math::DecimalPlaces{0}};
    CHECK_FALSE(math::checkedRoundToDecimalPlaces(third, math::DecimalPlaces{18}, math::RoundingMode::HalfEven));
    auto const fits = math::checkedRoundToDecimalPlaces(third, math::DecimalPlaces{2}, math::RoundingMode::HalfEven);
    REQUIRE(fits.has_value());
    CHECK(fits->numerator == 933);
    CHECK(fits->denominator == 100);
}
