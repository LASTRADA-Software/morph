// SPDX-License-Identifier: Apache-2.0

#include <morph/rational.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <array>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

// Ported from LASTRADA JPMath/Rational_test.cpp — same coverage, adjusted for
// the morph::math namespace and the bundled DecimalPlaces strong type
// (`.value` instead of `unbox<...>`); wire-codec cases added at the end.

using morph::math::DecimalPlaces;
using morph::math::kMaxDecimalPlaces;
using morph::math::Rational;
using morph::math::RationalError;

// ---------------------------------------------------------------------------
// Precision shorthands — most tests use precision 9 (the historical default);
// a few use precision 3 for cross-precision and rounding checks.
// ---------------------------------------------------------------------------

inline constexpr DecimalPlaces dp1 { 1 };
inline constexpr DecimalPlaces dp2 { 2 };
inline constexpr DecimalPlaces dp3 { 3 };
inline constexpr DecimalPlaces dp6 { 6 };
inline constexpr DecimalPlaces dp9 { 9 };

// ---------------------------------------------------------------------------
// Catch2 stringification — print Rationals via std::formatter so failed
// assertions show "3/4" instead of an opaque struct dump.
// ---------------------------------------------------------------------------

namespace Catch
{

template <>
struct StringMaker<Rational>
{
    static std::string convert(Rational const& value) { return std::format("{}", value); }
};

} // namespace Catch

// ---------------------------------------------------------------------------
// Compile-time checks — every constexpr-able piece of the API must be
// evaluable at compile time. FromFloat is intentionally not constexpr.
// ---------------------------------------------------------------------------

namespace
{

// Layout / traits.
static_assert(std::is_trivially_copyable_v<Rational>);
static_assert(std::is_standard_layout_v<Rational>);

// Construction and canonicalisation.
static_assert(Rational {}.numerator == 0);
static_assert(Rational {}.denominator == 1);
static_assert(Rational { 7, dp9 }.numerator == 7);
static_assert(Rational { 7, dp9 }.denominator == 1);
static_assert(Rational { 2, 4, dp9 }.numerator == 1);
static_assert(Rational { 2, 4, dp9 }.denominator == 2);
static_assert(Rational { -1, -2, dp9 }.numerator == 1);
static_assert(Rational { -1, -2, dp9 }.denominator == 2);
static_assert(Rational { 3, -4, dp9 }.numerator == -3);
static_assert(Rational { 3, -4, dp9 }.denominator == 4);
static_assert(Rational { 5, 0, dp9 }.numerator == 5);
static_assert(Rational { 5, 0, dp9 }.denominator == 1);

// Precision is carried and queryable at compile time.
static_assert((Rational { 7, dp9 }.GetDecimalPlaces()).value == 9);
static_assert((Rational { 7, dp3 }.GetDecimalPlaces()).value == 3);

// Constants.
static_assert(Rational::Zero(dp9).IsZero());
static_assert(Rational::One(dp9).IsInteger());
static_assert(Rational::One(dp9).numerator == 1);
static_assert(Rational::One(dp9).denominator == 1);

// From factory.
static_assert(Rational::From(1, 2, dp9).has_value());
static_assert(!Rational::From(1, 0, dp9).has_value());
static_assert(Rational::From(1, 0, dp9).error() == RationalError::DivisionByZero);

// Arithmetic.
static_assert(Rational { 1, 3, dp9 } + Rational { 1, 3, dp9 } + Rational { 1, 3, dp9 }
              == Rational::One(dp9));
static_assert(Rational { 1, 10, dp9 } + Rational { 2, 10, dp9 } == Rational { 3, 10, dp9 });
static_assert(Rational { 5, 6, dp9 } - Rational { 1, 6, dp9 } == Rational { 2, 3, dp9 });
static_assert(Rational { 2, 3, dp9 } * Rational { 3, 5, dp9 } == Rational { 2, 5, dp9 });
static_assert((Rational { 1, 7, dp9 } * Rational { 7, 1, dp9 }) == Rational::One(dp9));

// Division returns expected.
static_assert((Rational { 1, 2, dp9 } / Rational { 1, 4, dp9 }).has_value());
static_assert(*(Rational { 1, 2, dp9 } / Rational { 1, 4, dp9 }) == Rational { 2, dp9 });
static_assert(!(Rational { 1, 2, dp9 } / Rational::Zero(dp9)).has_value());

// Reciprocal.
static_assert(Rational { 3, 4, dp9 }.Reciprocal().has_value());
static_assert(*Rational { 3, 4, dp9 }.Reciprocal() == Rational { 4, 3, dp9 });
static_assert(*Rational { -3, 4, dp9 }.Reciprocal() == Rational { -4, 3, dp9 });
static_assert(!Rational::Zero(dp9).Reciprocal().has_value());

// Comparison (value-only).
static_assert(Rational { 1, 2, dp9 } < Rational { 2, 3, dp9 });
static_assert(Rational { -1, 2, dp9 } < Rational { 0, dp9 });
static_assert(Rational { 2, 4, dp9 } == Rational { 1, 2, dp9 });
// Comparison ignores precision: same value, different precision -> equal.
static_assert(Rational { 1, 2, dp3 } == Rational { 1, 2, dp9 });

// Conversion helpers (free functions; ADL through Rational).
static_assert(trunc(Rational { 7, 2, dp9 }) == 3);
static_assert(trunc(Rational { -7, 2, dp9 }) == -3);
static_assert(floor(Rational { 7, 2, dp9 }) == 3);
static_assert(floor(Rational { -7, 2, dp9 }) == -4);
static_assert(ceil(Rational { 7, 2, dp9 }) == 4);
static_assert(ceil(Rational { -7, 2, dp9 }) == -3);

// Abs / unary minus.
static_assert((-Rational { 3, 4, dp9 }) == Rational { -3, 4, dp9 });
static_assert(abs(Rational { -3, 4, dp9 }) == Rational { 3, 4, dp9 });

// Plain arithmetic result type is Rational; division wraps in expected.
static_assert(std::same_as<decltype(Rational {} + Rational {}), Rational>);
static_assert(std::same_as<decltype(Rational {} - Rational {}), Rational>);
static_assert(std::same_as<decltype(Rational {} * Rational {}), Rational>);
static_assert(std::same_as<decltype(Rational {} / Rational {}),
                           std::expected<Rational, RationalError>>);

// Mixed-type result types.
static_assert(std::same_as<decltype(std::declval<Rational>() + 3.5),
                           std::expected<Rational, RationalError>>);
static_assert(std::same_as<decltype(std::declval<std::expected<Rational, RationalError>>()
                                    + std::declval<Rational>()),
                           std::expected<Rational, RationalError>>);
static_assert(std::same_as<decltype(std::declval<std::expected<Rational, RationalError>>()
                                    + std::declval<std::expected<Rational, RationalError>>()),
                           std::expected<Rational, RationalError>>);
// User's worked expression: a / b + c * 3.5
static_assert(std::same_as<decltype(std::declval<Rational>() / std::declval<Rational>()
                                    + std::declval<Rational>() * 3.5),
                           std::expected<Rational, RationalError>>);
static_assert(std::same_as<decltype(3.5 + std::declval<Rational>()),
                           std::expected<Rational, RationalError>>);

} // namespace

// ---------------------------------------------------------------------------
// Rational — construction and canonicalisation
// ---------------------------------------------------------------------------

TEST_CASE("Rational::TraitsAndLayout", "[rational]")
{
    CHECK(std::is_trivially_copyable_v<Rational>);
    CHECK(std::is_standard_layout_v<Rational>);
    Rational const defaulted {};
    CHECK(defaulted.numerator == 0);
    CHECK(defaulted.denominator == 1);
}

TEST_CASE("Rational::Construction", "[rational]")
{
    CHECK(Rational { 7, dp9 }.numerator == 7);
    CHECK(Rational { 7, dp9 }.denominator == 1);

    CHECK(Rational { 2, 4, dp9 }.numerator == 1);
    CHECK(Rational { 2, 4, dp9 }.denominator == 2);

    CHECK(Rational { -1, -2, dp9 }.numerator == 1);
    CHECK(Rational { -1, -2, dp9 }.denominator == 2);
    CHECK(Rational { 3, -4, dp9 }.numerator == -3);
    CHECK(Rational { 3, -4, dp9 }.denominator == 4);

    CHECK(Rational { 5, 0, dp9 }.numerator == 5);
    CHECK(Rational { 5, 0, dp9 }.denominator == 1);
}

TEST_CASE("Rational::PrecisionQuery", "[rational]")
{
    // GetDecimalPlaces is public API and returns the constructed precision.
    CHECK(Rational { 1, 2, dp9 }.GetDecimalPlaces() == dp9);
    CHECK(Rational { 1, 2, dp3 }.GetDecimalPlaces() == dp3);
    CHECK(Rational { 7, dp6 }.GetDecimalPlaces() == dp6);
    CHECK((Rational { 1, 2, dp9 }.GetDecimalPlaces()).value == 9);
}

TEST_CASE("Rational::PrecisionCap", "[rational]")
{
    CHECK(kMaxDecimalPlaces == 18);

    // Precision at the cap is accepted unchanged.
    CHECK(Rational { 1, 2, DecimalPlaces { 18 } }.GetDecimalPlaces() == DecimalPlaces { 18 });

    // In a release build (NDEBUG) an out-of-range precision is clamped into
    // [1, kMaxDecimalPlaces]. A debug build asserts before reaching here, so
    // these clamp checks only run when NDEBUG is defined.
#ifdef NDEBUG
    CHECK(Rational { 1, 2, DecimalPlaces { 0 } }.GetDecimalPlaces() == DecimalPlaces { 1 });
    CHECK(Rational { 1, 2, DecimalPlaces { 99 } }.GetDecimalPlaces() == DecimalPlaces { 18 });
#endif
}

TEST_CASE("Rational::StrongTypePrecision", "[rational]")
{
    // DecimalPlaces is a distinct strong type with an explicit constructor:
    // it is not implicitly convertible from a raw integer.
    static_assert(!std::is_convertible_v<std::uint32_t, DecimalPlaces>);
    static_assert(std::is_constructible_v<DecimalPlaces, std::uint32_t>);
    auto const precision = DecimalPlaces { 9 };
    CHECK((precision).value == 9);
}

TEST_CASE("Rational::DenominatorAlwaysPositive", "[rational]")
{
    auto const samples = std::array {
        Rational { 0, dp9 },      Rational { 7, dp9 },      Rational { -7, dp9 },
        Rational { 2, 4, dp9 },   Rational { -2, 4, dp9 },  Rational { 2, -4, dp9 },
        Rational { -2, -4, dp9 }, Rational { 5, 0, dp9 },   Rational::Zero(dp9),
        Rational::One(dp9),
    };
    for (auto const& value : samples)
        CHECK(value.denominator > 0);

    auto const left = Rational { 3, 5, dp9 };
    auto const right = Rational { -7, 11, dp9 };
    CHECK((left + right).denominator > 0);
    CHECK((left - right).denominator > 0);
    CHECK((left * right).denominator > 0);
    CHECK((*(left / right)).denominator > 0);

    Rational mutating { 1, 2, dp9 };
    mutating += right;
    CHECK(mutating.denominator > 0);
    mutating -= right;
    CHECK(mutating.denominator > 0);
    mutating *= right;
    CHECK(mutating.denominator > 0);

    CHECK((-left).denominator > 0);
    CHECK((*left.Reciprocal()).denominator > 0);
    CHECK(abs(left).denominator > 0);
    CHECK(abs(right).denominator > 0);
}

TEST_CASE("Rational::FromExpected", "[rational]")
{
    auto const valid = Rational::From(1, 2, dp9);
    REQUIRE(valid.has_value());
    CHECK(*valid == Rational { 1, 2, dp9 });

    auto const invalid = Rational::From(1, 0, dp9);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Constants", "[rational]")
{
    CHECK(Rational::Zero(dp9) == Rational { 0, dp9 });
    CHECK(Rational::One(dp9) == Rational { 1, dp9 });
    CHECK(Rational::Zero(dp9).IsZero());
    CHECK(Rational::One(dp9).IsInteger());
    CHECK(Rational::Zero(dp9).denominator == 1);
    CHECK(Rational::One(dp9).denominator == 1);
}

// ---------------------------------------------------------------------------
// Rational — FromFloat (decimal-scaled to the requested precision)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::FromFloat::Roundtrip", "[rational]")
{
    {
        auto const value = Rational::FromFloat(0.5, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { 1, 2, dp1 });
    }
    {
        auto const value = Rational::FromFloat(0.25, dp2);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { 1, 4, dp2 });
    }
    {
        auto const value = Rational::FromFloat(0.1, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { 1, 10, dp1 });
    }
    {
        auto const value = Rational::FromFloat(-1.75, dp2);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { -7, 4, dp2 });
    }
    {
        auto const value = Rational::FromFloat(0.0, dp6);
        REQUIRE(value.has_value());
        CHECK(*value == Rational::Zero(dp6));
    }
    {
        auto const value = Rational::FromFloat(0.5f, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { 1, 2, dp1 });
    }
    {
        auto const value = Rational::FromFloat(0.5L, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { 1, 2, dp1 });
    }

    auto const negative = Rational::FromFloat(-1.75, dp2);
    REQUIRE(negative.has_value());
    CHECK(negative->denominator > 0);
}

TEST_CASE("Rational::FromFloat::Precision", "[rational]")
{
    auto const oneThirdAt3 = Rational::FromFloat(1.0 / 3.0, dp3);
    REQUIRE(oneThirdAt3.has_value());
    CHECK(*oneThirdAt3 == Rational { 333, 1000, dp3 });

    auto const oneThirdAt6 = Rational::FromFloat(1.0 / 3.0, dp6);
    REQUIRE(oneThirdAt6.has_value());
    CHECK(*oneThirdAt6 == Rational { 333333, 1000000, dp6 });

    auto const half = Rational::FromFloat(0.5, dp6);
    REQUIRE(half.has_value());
    CHECK(*half == Rational { 1, 2, dp6 });
}

TEST_CASE("Rational::FromFloat::ErrorPaths", "[rational]")
{
    auto const nan = Rational::FromFloat(std::numeric_limits<double>::quiet_NaN(), dp6);
    REQUIRE_FALSE(nan.has_value());
    CHECK(nan.error() == RationalError::NotFinite);

    auto const positiveInfinity = Rational::FromFloat(std::numeric_limits<double>::infinity(), dp6);
    REQUIRE_FALSE(positiveInfinity.has_value());
    CHECK(positiveInfinity.error() == RationalError::NotFinite);

    auto const negativeInfinity =
        Rational::FromFloat(-std::numeric_limits<double>::infinity(), dp6);
    REQUIRE_FALSE(negativeInfinity.has_value());
    CHECK(negativeInfinity.error() == RationalError::NotFinite);

    // Overflow: 1e20 * 10^9 = 1e29, way past int64_t range.
    auto const overflow = Rational::FromFloat(1e20, dp9);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == RationalError::Overflow);
}

// ---------------------------------------------------------------------------
// Rational — exact arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("Rational::ExactArithmetic", "[rational]")
{
    CHECK(Rational { 1, 3, dp9 } + Rational { 1, 3, dp9 } + Rational { 1, 3, dp9 }
          == Rational::One(dp9));
    CHECK(Rational { 1, 10, dp9 } + Rational { 2, 10, dp9 } == Rational { 3, 10, dp9 });
    CHECK(Rational { 1, 7, dp9 } * Rational { 7, dp9 } == Rational::One(dp9));
    CHECK(Rational { 5, 6, dp9 } - Rational { 1, 6, dp9 } == Rational { 2, 3, dp9 });

    auto const tenth = Rational { 1, 10, dp9 };
    auto const fifth = Rational { 1, 5, dp9 };
    CHECK(tenth + fifth == Rational { 3, 10, dp9 });
}

TEST_CASE("Rational::Comparison", "[rational]")
{
    CHECK(Rational { 2, 4, dp9 } == Rational { 1, 2, dp9 });
    CHECK(Rational { 1, 2, dp9 } != Rational { 1, 3, dp9 });
    CHECK(Rational { 1, 3, dp9 } < Rational { 1, 2, dp9 });
    CHECK(Rational { -1, 2, dp9 } < Rational { 1, 100, dp9 });
    CHECK(Rational { 1, 2, dp9 } > Rational { 1, 3, dp9 });
    CHECK(Rational { 0, dp9 } == Rational::Zero(dp9));
    CHECK((Rational { 2, 4, dp9 } <=> Rational { 1, 2, dp9 }) == std::strong_ordering::equal);
    CHECK((Rational { 1, 3, dp9 } <=> Rational { 1, 2, dp9 }) == std::strong_ordering::less);

    // Precision-independent: same value at different precision compares equal.
    CHECK(Rational { 1, 2, dp3 } == Rational { 1, 2, dp9 });
    CHECK((Rational { 1, 2, dp3 } <=> Rational { 1, 2, dp9 }) == std::strong_ordering::equal);
}

TEST_CASE("Rational::DivisionByZero", "[rational]")
{
    auto const result = Rational { 1, 2, dp9 } / Rational::Zero(dp9);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::DivisionByZero);

    auto const named = Rational { 1, 2, dp9 }.DividedBy(Rational::Zero(dp9));
    REQUIRE_FALSE(named.has_value());
    CHECK(named.error() == RationalError::DivisionByZero);

    auto const chained = Rational::From(1, 2, dp9).and_then(
        [](Rational value) { return value.DividedBy(Rational::Zero(dp9)); });
    REQUIRE_FALSE(chained.has_value());
    CHECK(chained.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Reciprocal", "[rational]")
{
    auto const reciprocal = Rational { 3, 4, dp9 }.Reciprocal();
    REQUIRE(reciprocal.has_value());
    CHECK(*reciprocal == Rational { 4, 3, dp9 });

    auto const negativeReciprocal = Rational { -3, 4, dp9 }.Reciprocal();
    REQUIRE(negativeReciprocal.has_value());
    CHECK(*negativeReciprocal == Rational { -4, 3, dp9 });
    CHECK(negativeReciprocal->denominator > 0);

    auto const zeroReciprocal = Rational::Zero(dp9).Reciprocal();
    REQUIRE_FALSE(zeroReciprocal.has_value());
    CHECK(zeroReciprocal.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Conversions", "[rational]")
{
    CHECK(Rational { 1, 2, dp9 }.ToDouble() == Catch::Approx(0.5));
    CHECK(Rational { -3, 4, dp9 }.ToDouble() == Catch::Approx(-0.75));

    CHECK(trunc(Rational { 7, 2, dp9 }) == 3);
    CHECK(trunc(Rational { -7, 2, dp9 }) == -3);
    CHECK(trunc(Rational { 6, 2, dp9 }) == 3);

    CHECK(floor(Rational { 7, 2, dp9 }) == 3);
    CHECK(floor(Rational { -7, 2, dp9 }) == -4);
    CHECK(floor(Rational { 6, 2, dp9 }) == 3);

    CHECK(ceil(Rational { 7, 2, dp9 }) == 4);
    CHECK(ceil(Rational { -7, 2, dp9 }) == -3);
    CHECK(ceil(Rational { 6, 2, dp9 }) == 3);
}

TEST_CASE("Rational::OverflowReduction", "[rational]")
{
    auto constexpr large = std::int64_t { std::numeric_limits<std::int32_t>::max() };
    auto const left = Rational { large, 1, dp9 };
    auto const right = Rational { 1, large, dp9 };
    auto const sum = left + right;
    CHECK(sum.denominator == large);
    CHECK(sum.numerator == large * large + 1);

    auto const cancelling = Rational { large, 7, dp9 } * Rational { 7, large, dp9 };
    CHECK(cancelling == Rational::One(dp9));
}

// ---------------------------------------------------------------------------
// Rational — mixed-type expressions with expected propagation
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Mixed::ResultTypes", "[rational]")
{
    using Expected = std::expected<Rational, RationalError>;
    auto const oneHalf = Rational { 1, 2, dp9 };
    auto const oneQuarter = Rational { 1, 4, dp9 };
    auto const expectedHalf = Expected { oneHalf };

    CHECK(*(oneHalf + 0.25) == Rational { 3, 4, dp9 });
    CHECK(*(0.25 + oneHalf) == Rational { 3, 4, dp9 });
    CHECK(*(expectedHalf + oneQuarter) == Rational { 3, 4, dp9 });
    CHECK(*(oneHalf + expectedHalf) == Rational::One(dp9));
    CHECK(*(expectedHalf + expectedHalf) == Rational::One(dp9));
    CHECK(*(expectedHalf - oneQuarter) == Rational { 1, 4, dp9 });
    CHECK(*(oneHalf * 2.0) == Rational::One(dp9));
    CHECK(*(expectedHalf / 2.0) == Rational { 1, 4, dp9 });
}

TEST_CASE("Rational::Mixed::ExactValuesHappy", "[rational]")
{
    auto const sum = Rational { 1, 2, dp9 } + 0.25;
    REQUIRE(sum.has_value());
    CHECK(*sum == Rational { 3, 4, dp9 });

    auto const composed =
        Rational { 1, 2, dp9 } / Rational { 2, dp9 } + Rational { 1, 4, dp9 } * 2.0;
    REQUIRE(composed.has_value());
    CHECK(*composed == Rational { 3, 4, dp9 });

    auto const a = Rational { 7, 2, dp9 };
    auto const b = Rational { 2, dp9 };
    auto const c = Rational { 1, 2, dp9 };
    auto const expression = a / b + c * 3.5;
    REQUIRE(expression.has_value());
    CHECK(*expression == Rational { 7, 2, dp9 });
}

TEST_CASE("Rational::Mixed::ErrorPropagation::DivByZero", "[rational]")
{
    auto const result = Rational { 1, 2, dp9 } / Rational::Zero(dp9) + Rational { 3, 4, dp9 };
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::DivisionByZero);

    auto const otherSide = Rational { 1, 4, dp9 } + Rational { 1, dp9 } / Rational::Zero(dp9);
    REQUIRE_FALSE(otherSide.has_value());
    CHECK(otherSide.error() == RationalError::DivisionByZero);

    auto const product = (Rational { 1, dp9 } / Rational::Zero(dp9)) * Rational { 7, 8, dp9 };
    REQUIRE_FALSE(product.has_value());
    CHECK(product.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Mixed::ErrorPropagation::FloatNonFinite", "[rational]")
{
    auto const nan = std::numeric_limits<double>::quiet_NaN();
    auto const result = Rational::One(dp9) + nan;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::NotFinite);

    auto const positiveInfinity = std::numeric_limits<double>::infinity();
    auto const product = Rational { 1, 2, dp9 } * positiveInfinity;
    REQUIRE_FALSE(product.has_value());
    CHECK(product.error() == RationalError::NotFinite);

    auto const negativeInfinity = -std::numeric_limits<double>::infinity();
    auto const difference = Rational { 1, 2, dp9 } - negativeInfinity;
    REQUIRE_FALSE(difference.has_value());
    CHECK(difference.error() == RationalError::NotFinite);
}

TEST_CASE("Rational::Mixed::ErrorPropagation::FirstErrorWins", "[rational]")
{
    auto const nan = std::numeric_limits<double>::quiet_NaN();
    auto const result = (Rational::One(dp9) / Rational::Zero(dp9)) + nan;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::DivisionByZero);
}

// ---------------------------------------------------------------------------
// Rational — std::format support
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Formatter::Default", "[rational]")
{
    CHECK(std::format("{}", Rational { 3, 4, dp9 }) == "3/4");
    CHECK(std::format("{}", Rational { 7, 1, dp9 }) == "7");
    CHECK(std::format("{}", Rational { -3, 4, dp9 }) == "-3/4");
    CHECK(std::format("{}", Rational::Zero(dp9)) == "0");
    CHECK(std::format("{}", Rational::One(dp9)) == "1");
}

TEST_CASE("Rational::Formatter::DelegatesToDouble", "[rational]")
{
    CHECK(std::format("{:.3f}", Rational { 1, 3, dp9 }) == "0.333");
    CHECK(std::format("{:.6f}", Rational { 22, 7, dp9 }) == "3.142857");
    CHECK(std::format("{:+.2e}", Rational { -1, 4, dp9 }) == "-2.50e-01");

    auto const ratio = Rational { 1, 8, dp9 };
    CHECK(std::format("{:.4f}", ratio) == std::format("{:.4f}", ratio.ToDouble()));
    CHECK(std::format("{:10.4g}", ratio) == std::format("{:10.4g}", ratio.ToDouble()));
}

// ---------------------------------------------------------------------------
// Rational — trivial-copyability contract
// ---------------------------------------------------------------------------

TEST_CASE("Rational::TriviallyCopyable", "[rational]")
{
    auto const original = Rational { -3, 4, dp9 };
    Rational copy {};
    std::memcpy(&copy, &original, sizeof(Rational));
    CHECK(copy == original);
    CHECK(copy.numerator == -3);
    CHECK(copy.denominator == 4);
    CHECK(copy.GetDecimalPlaces() == dp9);
}

// ---------------------------------------------------------------------------
// Rational — ToDouble precision behaviour
// ---------------------------------------------------------------------------

TEST_CASE("Rational::ToDouble::PrecisionRoundtrip", "[rational]")
{
    auto const oneThird = *Rational::FromFloat(1.0 / 3.0, dp9);

    CHECK(oneThird.ToDouble() == Catch::Approx(0.333333333).epsilon(1e-12));
    CHECK(oneThird.ToDouble(3) == Catch::Approx(0.333).epsilon(1e-12));
    CHECK(oneThird.ToDouble(1) == Catch::Approx(0.3).epsilon(1e-12));
    CHECK(oneThird.ToDouble(6) == Catch::Approx(0.333333).epsilon(1e-12));
    CHECK(oneThird.ToDouble(9) == Catch::Approx(oneThird.ToDouble()).epsilon(1e-15));

    auto const negativeQuarter = Rational { -1, 4, dp9 };
    CHECK(negativeQuarter.ToDouble(2) == Catch::Approx(-0.25).epsilon(1e-12));
    CHECK(negativeQuarter.ToDouble(1) == Catch::Approx(-0.3).epsilon(1e-12));
}

TEST_CASE("Rational::CommonArithmetic::PiApproximation22Over7", "[rational]")
{
    auto const exactQuotient = Rational { 22, dp3 } / Rational { 7, dp3 };
    REQUIRE(exactQuotient.has_value());
    CHECK(*exactQuotient == Rational { 22, 7, dp3 });
    CHECK(exactQuotient->ToDouble() == Catch::Approx(3.143).epsilon(1e-12));
    CHECK(exactQuotient->ToDouble(3) == Catch::Approx(3.143).epsilon(1e-12));
    CHECK(std::format("{:.3f}", *exactQuotient) == "3.143");

    auto const dividend = Rational::FromFloat(22.0, dp3);
    auto const divisor = Rational::FromFloat(7.0, dp3);
    REQUIRE(dividend.has_value());
    REQUIRE(divisor.has_value());
    auto const liftedQuotient = *dividend / *divisor;
    REQUIRE(liftedQuotient.has_value());
    CHECK(*liftedQuotient == Rational { 22, 7, dp3 });
    CHECK(liftedQuotient->ToDouble() == Catch::Approx(3.143).epsilon(1e-12));

    auto const mixedQuotient = Rational { 22, dp3 } / 7.0;
    REQUIRE(mixedQuotient.has_value());
    CHECK(*mixedQuotient == Rational { 22, 7, dp3 });
    CHECK(mixedQuotient->ToDouble() == Catch::Approx(3.143).epsilon(1e-12));
}

TEST_CASE("Rational::FreeFunctions::AbsCeilFloorTrunc", "[rational]")
{
    CHECK(abs(Rational { -3, 4, dp9 }) == Rational { 3, 4, dp9 });
    CHECK(abs(Rational { 3, 4, dp9 }) == Rational { 3, 4, dp9 });
    CHECK(abs(Rational::Zero(dp9)) == Rational::Zero(dp9));

    CHECK(ceil(Rational { 7, 2, dp9 }) == 4);
    CHECK(floor(Rational { 7, 2, dp9 }) == 3);
    CHECK(trunc(Rational { 7, 2, dp9 }) == 3);

    using std::abs;
    using std::ceil;
    using std::floor;
    using std::trunc;
    CHECK(abs(-2.5) == Catch::Approx(2.5));
    CHECK(abs(Rational { -3, 4, dp9 }) == Rational { 3, 4, dp9 });
    CHECK(ceil(2.1) == Catch::Approx(3.0));
    CHECK(ceil(Rational { 7, 2, dp9 }) == 4);
}

// ---------------------------------------------------------------------------
// Rational — cross-precision arithmetic (now runtime max-propagation)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::CrossPrecision::MaxPropagates", "[rational]")
{
    // Result precision is the max of the two operands' precisions.
    CHECK((Rational { 1, 2, dp3 } + Rational { 1, 4, dp9 }).GetDecimalPlaces() == dp9);
    CHECK((Rational { 1, 2, dp9 } + Rational { 1, 4, dp3 }).GetDecimalPlaces() == dp9);
    CHECK((Rational { 1, 2, dp3 } + Rational { 1, 4, dp3 }).GetDecimalPlaces() == dp3);
    CHECK((Rational { 1, 2, dp3 } - Rational { 1, 4, dp9 }).GetDecimalPlaces() == dp9);
    CHECK((Rational { 1, 2, dp3 } * Rational { 1, 4, dp9 }).GetDecimalPlaces() == dp9);

    auto const quotient = Rational { 3, 4, dp3 } / Rational { 1, 2, dp9 };
    REQUIRE(quotient.has_value());
    CHECK(quotient->GetDecimalPlaces() == dp9);
}

TEST_CASE("Rational::CrossPrecision::ValueCorrectness", "[rational]")
{
    auto const sum = Rational { 1, 2, dp3 } + Rational { 1, 4, dp9 };
    CHECK(sum == Rational { 3, 4, dp9 });

    auto const difference = Rational { 2, 3, dp9 } - Rational { 1, 3, dp3 };
    CHECK(difference == Rational { 1, 3, dp9 });

    auto const product = Rational { 2, 3, dp3 } * Rational { 3, 2, dp9 };
    CHECK(product == Rational::One(dp9));

    auto const quotient = Rational { 3, 4, dp3 } / Rational { 1, 2, dp9 };
    REQUIRE(quotient.has_value());
    CHECK(*quotient == Rational { 3, 2, dp9 });
}

TEST_CASE("Rational::Mixed::FloatInheritsRationalPrecision", "[rational]")
{
    static_assert(std::same_as<decltype(Rational {} + 3.5),
                               std::expected<Rational, RationalError>>);
    static_assert(std::same_as<decltype(3.5 + Rational {}),
                               std::expected<Rational, RationalError>>);

    // 1/2 (precision 3) + 0.25 (lifted at precision 3) = 3/4 at precision 3.
    auto const lifted = Rational { 1, 2, dp3 } + 0.25;
    REQUIRE(lifted.has_value());
    CHECK(lifted->GetDecimalPlaces() == dp3);
    CHECK(*lifted == Rational { 3, 4, dp3 });

    // Float lifts to precision 9 when the Rational operand carries 9.
    auto const liftedAt9 = Rational { 1, 2, dp9 } + 0.25;
    REQUIRE(liftedAt9.has_value());
    CHECK(liftedAt9->GetDecimalPlaces() == dp9);
}

// ---------------------------------------------------------------------------
// Rational — Glaze wire codec (morph addition)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Wire::RoundTrip", "[rational]")
{
    auto const original = Rational { 617, 50, dp2 };
    auto const json = glz::write_json(original);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"num":617,"den":50,"dp":2})");

    Rational restored {};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored == original);
    CHECK(restored.GetDecimalPlaces() == dp2);
}

TEST_CASE("Rational::Wire::CanonicalisesOnRead", "[rational]")
{
    // A non-canonical payload (1234/100) must land reduced: the codec goes
    // through the canonicalising constructor, so invariants always hold.
    Rational value {};
    REQUIRE_FALSE(glz::read_json(value, R"({"num":1234,"den":100,"dp":2})"));
    CHECK(value.numerator == 617);
    CHECK(value.denominator == 50);
    CHECK(value == Rational { 1234, 100, dp2 });

    Rational negative {};
    REQUIRE_FALSE(glz::read_json(negative, R"({"num":3,"den":-4,"dp":1})"));
    CHECK(negative.numerator == -3);
    CHECK(negative.denominator == 4);
}

TEST_CASE("Rational::Wire::HostileInputClamps", "[rational]")
{
    // Zero denominator and out-of-range precision are clamped, not asserted:
    // wire input is untrusted.
    Rational hostile {};
    REQUIRE_FALSE(glz::read_json(hostile, R"({"num":5,"den":0,"dp":99})"));
    CHECK(hostile.denominator == 1);
    CHECK(hostile.numerator == 5);
    CHECK(hostile.GetDecimalPlaces() == DecimalPlaces { kMaxDecimalPlaces });

    Rational zeroPrecision {};
    REQUIRE_FALSE(glz::read_json(zeroPrecision, R"({"num":1,"den":2,"dp":0})"));
    CHECK(zeroPrecision.GetDecimalPlaces() == dp1);
}

TEST_CASE("Rational::Wire::MissingFieldsUseWireDefaults", "[rational]")
{
    // Absent keys fall back to Wire's member defaults (0/1 at precision 1).
    Rational value { 9, 4, dp9 };
    REQUIRE_FALSE(glz::read_json(value, R"({})"));
    CHECK(value == Rational::Zero(dp1));
    CHECK(value.GetDecimalPlaces() == dp1);
}

TEST_CASE("Rational::Wire::NullableComposition", "[rational]")
{
    std::optional<Rational> maybe {};
    REQUIRE_FALSE(glz::read_json(maybe, "null"));
    CHECK_FALSE(maybe.has_value());

    REQUIRE_FALSE(glz::read_json(maybe, R"({"num":1,"den":3,"dp":9})"));
    REQUIRE(maybe.has_value());
    CHECK(*maybe == Rational { 1, 3, dp9 });

    auto const written = glz::write_json(maybe);
    REQUIRE(written.has_value());
    CHECK(*written == R"({"num":1,"den":3,"dp":9})");
}
