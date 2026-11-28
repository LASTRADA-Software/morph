// SPDX-License-Identifier: Apache-2.0

// The 128-bit helpers and the rounding of `Rational`, at the edges where a shift crosses the word, a root is a tie
// and a result no longer fits an `int64`.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <limits>
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
        {.value = {.hi = 0xF0F0, .lo = 0x1234}, .count = 0, .expected = {.hi = 0xF0F0, .lo = 0x1234}},
        {.value = {.hi = 0x1, .lo = 0x0}, .count = 1, .expected = {.hi = 0, .lo = 0x8000000000000000ULL}},
        {.value = {.hi = 0xFF, .lo = 0xFF00}, .count = 8, .expected = {.hi = 0, .lo = 0xFF000000000000FFULL}},
        {.value = {.hi = 0xABCD, .lo = 0x1}, .count = 64, .expected = {.hi = 0, .lo = 0xABCD}},
        {.value = {.hi = 0xABCD, .lo = 0x1}, .count = 70, .expected = {.hi = 0, .lo = 0xABCD >> 6}},
        {.value = {.hi = 0xABCD, .lo = 0x1}, .count = 127, .expected = {.hi = 0, .lo = 0}},
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
        {.what = "division by zero",
         .numerator = {.hi = 0, .lo = 9},
         .denominator = {.hi = 0, .lo = 0},
         .mode = half,
         .expected = std::nullopt},
        {.what = "2.25 is 1.5, a tie that goes up from an odd root",
         .numerator = {.hi = 0, .lo = 9},
         .denominator = {.hi = 0, .lo = 4},
         .mode = even,
         .expected = 2},
        {.what = "2.25 half away",
         .numerator = {.hi = 0, .lo = 9},
         .denominator = {.hi = 0, .lo = 4},
         .mode = half,
         .expected = 2},
        {.what = "6.25 is 2.5, a tie that stays on an even root",
         .numerator = {.hi = 0, .lo = 25},
         .denominator = {.hi = 0, .lo = 4},
         .mode = even,
         .expected = 2},
        {.what = "6.25 half away",
         .numerator = {.hi = 0, .lo = 25},
         .denominator = {.hi = 0, .lo = 4},
         .mode = half,
         .expected = 3},
        {.what = "a quotient whose root is 2^63",
         .numerator = {.hi = 0x4000000000000000ULL, .lo = 0},
         .denominator = {.hi = 0, .lo = 1},
         .mode = half,
         .expected = std::nullopt},
        {.what = "a root of int64 max that would round up",
         .numerator = {.hi = 0x3fffffffffffffffULL, .lo = 0x8000000000000001ULL},
         .denominator = {.hi = 0, .lo = 1},
         .mode = half,
         .expected = std::nullopt},
        {.what = "a root of int64 max that stays",
         .numerator = {.hi = 0x3fffffffffffffffULL, .lo = 0x0000000000000001ULL},
         .denominator = {.hi = 0, .lo = 1},
         .mode = half,
         .expected = kMax},
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

namespace {

using Expected = std::expected<math::Rational, math::RationalError>;

math::Rational const kOne{math::Numerator{1}, math::Denominator{1}, math::DecimalPlaces{2}};

template <typename Op>
void mixedOperandsFail(Op op) {
    constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInf = std::numeric_limits<double>::infinity();
    Expected const failed = std::unexpected(math::RationalError::Overflow);
    // A float that is not a number, on either side, names it.
    CHECK(op(kOne, kNan) == Expected{std::unexpected(math::RationalError::NotFinite)});
    CHECK(op(kNan, kOne) == Expected{std::unexpected(math::RationalError::NotFinite)});
    CHECK(op(kOne, kInf) == Expected{std::unexpected(math::RationalError::NotFinite)});
    // A float beyond what the precision holds overflows.
    CHECK(op(kOne, 1e30) == Expected{std::unexpected(math::RationalError::Overflow)});
    CHECK(op(-1e30, kOne) == Expected{std::unexpected(math::RationalError::Overflow)});
    // An expected that holds an error passes it on, whichever side it is on.
    CHECK(op(failed, kOne) == failed);
    CHECK(op(kOne, failed) == failed);
    CHECK(op(failed, 1.5) == failed);
    CHECK(op(1.5, failed) == failed);
    CHECK(op(Expected{std::unexpected(math::RationalError::NotFinite)}, failed) ==
          Expected{std::unexpected(math::RationalError::NotFinite)});
    // One that holds a value works as the value does.
    CHECK(op(Expected{kOne}, 1.0).has_value());
    CHECK(op(1.0, Expected{kOne}).has_value());
}

}  // namespace

TEST_CASE("cov h rational: a float operand that does not lift, or an expected that failed, fails the operator",
          "[covh][rational]") {
    mixedOperandsFail([](auto const& lhs, auto const& rhs) { return lhs + rhs; });
    mixedOperandsFail([](auto const& lhs, auto const& rhs) { return lhs - rhs; });
    mixedOperandsFail([](auto const& lhs, auto const& rhs) { return lhs * rhs; });
    mixedOperandsFail([](auto const& lhs, auto const& rhs) { return lhs / rhs; });
}
