// SPDX-License-Identifier: Apache-2.0

#include <morph/util/rational.hpp>

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
using morph::math::Denominator;
using morph::math::kMaxDecimalPlaces;
using morph::math::Numerator;
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
// evaluable at compile time. fromFloat is intentionally not constexpr.
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
static_assert(Rational { Numerator{2}, Denominator{4}, dp9 }.numerator == 1);
static_assert(Rational { Numerator{2}, Denominator{4}, dp9 }.denominator == 2);
static_assert(Rational { Numerator{-1}, Denominator{-2}, dp9 }.numerator == 1);
static_assert(Rational { Numerator{-1}, Denominator{-2}, dp9 }.denominator == 2);
static_assert(Rational { Numerator{3}, Denominator{-4}, dp9 }.numerator == -3);
static_assert(Rational { Numerator{3}, Denominator{-4}, dp9 }.denominator == 4);
static_assert(Rational { Numerator{5}, Denominator{0}, dp9 }.numerator == 5);
static_assert(Rational { Numerator{5}, Denominator{0}, dp9 }.denominator == 1);

// Precision is carried and queryable at compile time.
static_assert((Rational { 7, dp9 }.getDecimalPlaces()).value == 9);
static_assert((Rational { 7, dp3 }.getDecimalPlaces()).value == 3);

// Constants.
static_assert(Rational::zero(dp9).isZero());
static_assert(Rational::one(dp9).isInteger());
static_assert(Rational::one(dp9).numerator == 1);
static_assert(Rational::one(dp9).denominator == 1);

// From factory.
static_assert(Rational::from(Numerator{1}, Denominator{2}, dp9).has_value());
static_assert(!Rational::from(Numerator{1}, Denominator{0}, dp9).has_value());
static_assert(Rational::from(Numerator{1}, Denominator{0}, dp9).error() == RationalError::DivisionByZero);

// Arithmetic.
static_assert(Rational { Numerator{1}, Denominator{3}, dp9 } + Rational { Numerator{1}, Denominator{3}, dp9 } + Rational { Numerator{1}, Denominator{3}, dp9 }
              == Rational::one(dp9));
static_assert(Rational { Numerator{1}, Denominator{10}, dp9 } + Rational { Numerator{2}, Denominator{10}, dp9 } == Rational { Numerator{3}, Denominator{10}, dp9 });
static_assert(Rational { Numerator{5}, Denominator{6}, dp9 } - Rational { Numerator{1}, Denominator{6}, dp9 } == Rational { Numerator{2}, Denominator{3}, dp9 });
static_assert(Rational { Numerator{2}, Denominator{3}, dp9 } * Rational { Numerator{3}, Denominator{5}, dp9 } == Rational { Numerator{2}, Denominator{5}, dp9 });
static_assert((Rational { Numerator{1}, Denominator{7}, dp9 } * Rational { Numerator{7}, Denominator{1}, dp9 }) == Rational::one(dp9));

// Division returns expected.
static_assert((Rational { Numerator{1}, Denominator{2}, dp9 } / Rational { Numerator{1}, Denominator{4}, dp9 }).has_value());
static_assert(*(Rational { Numerator{1}, Denominator{2}, dp9 } / Rational { Numerator{1}, Denominator{4}, dp9 }) == Rational { 2, dp9 });
static_assert(!(Rational { Numerator{1}, Denominator{2}, dp9 } / Rational::zero(dp9)).has_value());

// reciprocal.
static_assert(Rational { Numerator{3}, Denominator{4}, dp9 }.reciprocal().has_value());
static_assert(*Rational { Numerator{3}, Denominator{4}, dp9 }.reciprocal() == Rational { Numerator{4}, Denominator{3}, dp9 });
static_assert(*Rational { Numerator{-3}, Denominator{4}, dp9 }.reciprocal() == Rational { Numerator{-4}, Denominator{3}, dp9 });
static_assert(!Rational::zero(dp9).reciprocal().has_value());

// Comparison (value-only).
static_assert(Rational { Numerator{1}, Denominator{2}, dp9 } < Rational { Numerator{2}, Denominator{3}, dp9 });
static_assert(Rational { Numerator{-1}, Denominator{2}, dp9 } < Rational { 0, dp9 });
static_assert(Rational { Numerator{2}, Denominator{4}, dp9 } == Rational { Numerator{1}, Denominator{2}, dp9 });
// Comparison ignores precision: same value, different precision -> equal.
static_assert(Rational { Numerator{1}, Denominator{2}, dp3 } == Rational { Numerator{1}, Denominator{2}, dp9 });

// Conversion helpers (free functions; ADL through Rational).
static_assert(trunc(Rational { Numerator{7}, Denominator{2}, dp9 }) == 3);
static_assert(trunc(Rational { Numerator{-7}, Denominator{2}, dp9 }) == -3);
static_assert(floor(Rational { Numerator{7}, Denominator{2}, dp9 }) == 3);
static_assert(floor(Rational { Numerator{-7}, Denominator{2}, dp9 }) == -4);
static_assert(ceil(Rational { Numerator{7}, Denominator{2}, dp9 }) == 4);
static_assert(ceil(Rational { Numerator{-7}, Denominator{2}, dp9 }) == -3);

// Abs / unary minus.
static_assert((-Rational { Numerator{3}, Denominator{4}, dp9 }) == Rational { Numerator{-3}, Denominator{4}, dp9 });
static_assert(abs(Rational { Numerator{-3}, Denominator{4}, dp9 }) == Rational { Numerator{3}, Denominator{4}, dp9 });

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

    CHECK(Rational { Numerator{2}, Denominator{4}, dp9 }.numerator == 1);
    CHECK(Rational { Numerator{2}, Denominator{4}, dp9 }.denominator == 2);

    CHECK(Rational { Numerator{-1}, Denominator{-2}, dp9 }.numerator == 1);
    CHECK(Rational { Numerator{-1}, Denominator{-2}, dp9 }.denominator == 2);
    CHECK(Rational { Numerator{3}, Denominator{-4}, dp9 }.numerator == -3);
    CHECK(Rational { Numerator{3}, Denominator{-4}, dp9 }.denominator == 4);

    CHECK(Rational { Numerator{5}, Denominator{0}, dp9 }.numerator == 5);
    CHECK(Rational { Numerator{5}, Denominator{0}, dp9 }.denominator == 1);
}

TEST_CASE("Rational::PrecisionQuery", "[rational]")
{
    // getDecimalPlaces is public API and returns the constructed precision.
    CHECK(Rational { Numerator{1}, Denominator{2}, dp9 }.getDecimalPlaces() == dp9);
    CHECK(Rational { Numerator{1}, Denominator{2}, dp3 }.getDecimalPlaces() == dp3);
    CHECK(Rational { 7, dp6 }.getDecimalPlaces() == dp6);
    CHECK((Rational { Numerator{1}, Denominator{2}, dp9 }.getDecimalPlaces()).value == 9);
}

TEST_CASE("Rational::PrecisionCap", "[rational]")
{
    CHECK(kMaxDecimalPlaces == 18);

    // Precision at the cap is accepted unchanged.
    CHECK(Rational { Numerator{1}, Denominator{2}, DecimalPlaces { 18 } }.getDecimalPlaces() == DecimalPlaces { 18 });

    // Zero decimal places is a legal precision (zero-decimal currencies like
    // JPY/KRW): the floor is 0, not 1.
    CHECK(Rational { Numerator{1}, Denominator{2}, DecimalPlaces { 0 } }.getDecimalPlaces() == DecimalPlaces { 0 });

    // In a release build (NDEBUG) an out-of-range precision is clamped into
    // [0, kMaxDecimalPlaces]. A debug build asserts before reaching here, so
    // this clamp check only runs when NDEBUG is defined.
#ifdef NDEBUG
    CHECK(Rational { Numerator{1}, Denominator{2}, DecimalPlaces { 99 } }.getDecimalPlaces() == DecimalPlaces { 18 });
#endif
}

TEST_CASE("Rational::ZeroDecimalPlaces", "[rational]")
{
    // DecimalPlaces{0} is a fully legal precision: a whole-number quantity
    // (e.g. a JPY/KRW amount) carries no fractional digit at all.
    DecimalPlaces const dp0 { 0 };
    Rational const whole { 1200, dp0 };
    CHECK(whole.getDecimalPlaces() == dp0);
    CHECK(whole.numerator == 1200);
    CHECK(whole.denominator == 1);
    CHECK(std::format("{}", whole) == "1200");

    // toDouble at 0 requested places rounds to the nearest integer.
    CHECK(Rational { Numerator{7}, Denominator{2}, dp0 }.toDouble(0) == Catch::Approx(4.0));

    // Arithmetic between a dp0 and a wider-precision operand still
    // max-propagates, same as any other pair.
    auto const sum = whole + Rational { Numerator{1}, Denominator{4}, dp2 };
    CHECK(sum.getDecimalPlaces() == dp2);

    // Wire round-trip at dp == 0.
    Rational restored {};
    REQUIRE_FALSE(glz::read_json(restored, R"({"num":1200,"den":1,"dp":0})"));
    CHECK(restored.getDecimalPlaces() == dp0);
    auto const written = glz::write_json(restored);
    REQUIRE(written.has_value());
    CHECK(*written == R"({"num":1200,"den":1,"dp":0})");
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Rational::DenominatorAlwaysPositive", "[rational]")
{
    auto const samples = std::array {
        Rational { 0, dp9 },      Rational { 7, dp9 },      Rational { -7, dp9 },
        Rational { Numerator{2}, Denominator{4}, dp9 },   Rational { Numerator{-2}, Denominator{4}, dp9 },  Rational { Numerator{2}, Denominator{-4}, dp9 },
        Rational { Numerator{-2}, Denominator{-4}, dp9 }, Rational { Numerator{5}, Denominator{0}, dp9 },   Rational::zero(dp9),
        Rational::one(dp9),
    };
    for (auto const& value : samples) {
        CHECK(value.denominator > 0);
    }

    auto const left = Rational { Numerator{3}, Denominator{5}, dp9 };
    auto const right = Rational { Numerator{-7}, Denominator{11}, dp9 };
    CHECK((left + right).denominator > 0);
    CHECK((left - right).denominator > 0);
    CHECK((left * right).denominator > 0);
    CHECK((*(left / right)).denominator > 0);

    Rational mutating { Numerator{1}, Denominator{2}, dp9 };
    mutating += right;
    CHECK(mutating.denominator > 0);
    mutating -= right;
    CHECK(mutating.denominator > 0);
    mutating *= right;
    CHECK(mutating.denominator > 0);

    CHECK((-left).denominator > 0);
    CHECK((*left.reciprocal()).denominator > 0);
    CHECK(abs(left).denominator > 0);
    CHECK(abs(right).denominator > 0);
}

TEST_CASE("Rational::FromExpected", "[rational]")
{
    auto const valid = Rational::from(Numerator{1}, Denominator{2}, dp9);
    REQUIRE(valid.has_value());
    CHECK(*valid == Rational { Numerator{1}, Denominator{2}, dp9 });

    auto const invalid = Rational::from(Numerator{1}, Denominator{0}, dp9);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Constants", "[rational]")
{
    CHECK(Rational::zero(dp9) == Rational { 0, dp9 });
    CHECK(Rational::one(dp9) == Rational { 1, dp9 });
    CHECK(Rational::zero(dp9).isZero());
    CHECK(Rational::one(dp9).isInteger());
    CHECK(Rational::zero(dp9).denominator == 1);
    CHECK(Rational::one(dp9).denominator == 1);
}

// ---------------------------------------------------------------------------
// Rational — fromFloat (decimal-scaled to the requested precision)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::fromFloat::Roundtrip", "[rational]")
{
    {
        auto const value = Rational::fromFloat(0.5, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { Numerator{1}, Denominator{2}, dp1 });
    }
    {
        auto const value = Rational::fromFloat(0.25, dp2);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { Numerator{1}, Denominator{4}, dp2 });
    }
    {
        auto const value = Rational::fromFloat(0.1, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { Numerator{1}, Denominator{10}, dp1 });
    }
    {
        auto const value = Rational::fromFloat(-1.75, dp2);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { Numerator{-7}, Denominator{4}, dp2 });
    }
    {
        auto const value = Rational::fromFloat(0.0, dp6);
        REQUIRE(value.has_value());
        CHECK(*value == Rational::zero(dp6));
    }
    {
        auto const value = Rational::fromFloat(0.5F, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { Numerator{1}, Denominator{2}, dp1 });
    }
    {
        auto const value = Rational::fromFloat(0.5L, dp1);
        REQUIRE(value.has_value());
        CHECK(*value == Rational { Numerator{1}, Denominator{2}, dp1 });
    }

    auto const negative = Rational::fromFloat(-1.75, dp2);
    REQUIRE(negative.has_value());
    CHECK(negative->denominator > 0);
}

TEST_CASE("Rational::fromFloat::Precision", "[rational]")
{
    auto const oneThirdAt3 = Rational::fromFloat(1.0 / 3.0, dp3);
    REQUIRE(oneThirdAt3.has_value());
    CHECK(*oneThirdAt3 == Rational { Numerator{333}, Denominator{1000}, dp3 });

    auto const oneThirdAt6 = Rational::fromFloat(1.0 / 3.0, dp6);
    REQUIRE(oneThirdAt6.has_value());
    CHECK(*oneThirdAt6 == Rational { Numerator{333333}, Denominator{1000000}, dp6 });

    auto const half = Rational::fromFloat(0.5, dp6);
    REQUIRE(half.has_value());
    CHECK(*half == Rational { Numerator{1}, Denominator{2}, dp6 });
}

TEST_CASE("Rational::fromFloat::ErrorPaths", "[rational]")
{
    auto const nan = Rational::fromFloat(std::numeric_limits<double>::quiet_NaN(), dp6);
    REQUIRE_FALSE(nan.has_value());
    CHECK(nan.error() == RationalError::NotFinite);

    auto const positiveInfinity = Rational::fromFloat(std::numeric_limits<double>::infinity(), dp6);
    REQUIRE_FALSE(positiveInfinity.has_value());
    CHECK(positiveInfinity.error() == RationalError::NotFinite);

    auto const negativeInfinity =
        Rational::fromFloat(-std::numeric_limits<double>::infinity(), dp6);
    REQUIRE_FALSE(negativeInfinity.has_value());
    CHECK(negativeInfinity.error() == RationalError::NotFinite);

    // Overflow: 1e20 * 10^9 = 1e29, way past int64_t range.
    auto const overflow = Rational::fromFloat(1e20, dp9);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == RationalError::Overflow);

    // Near the 2^63 boundary: just under scales fine, just over is rejected
    // (the guard compares against 2^63 exactly, so this holds even where
    // long double == double and casting INT64_MAX would round up).
    auto const justUnder = Rational::fromFloat(4.0e17, dp1);  // scaled 4e18 < 2^63
    REQUIRE(justUnder.has_value());
    auto const justOver = Rational::fromFloat(1.0e18, dp1);  // scaled 1e19 > 2^63
    REQUIRE_FALSE(justOver.has_value());
    CHECK(justOver.error() == RationalError::Overflow);

    // The llround half-ulp window: on x86's 80-bit long double this scales to
    // exactly 2^63 - 0.5, which llround would round UP to 2^63 (wrapping to a
    // poisoned INT64_MIN numerator). Must be rejected, not returned. Where
    // long double == double the literal rounds past the bound and is rejected
    // by the plain 2^63 check, so the assertion holds on every platform.
    auto const halfUlp = Rational::fromFloat(922337203685477580.75L, dp1);
    REQUIRE_FALSE(halfUlp.has_value());
    CHECK(halfUlp.error() == RationalError::Overflow);
}

TEST_CASE("Rational::fromFloat::NegativeOverflow", "[rational]")
{
    // The overflow guard is `scaled >= 2^63 - 0.5 || scaled < -2^63`: every
    // case above only ever drives the *positive* disjunct. Exercise the
    // negative one directly with a large-magnitude negative value whose
    // scaled magnitude passes -2^63 the same way 1e20 passes +2^63 above.
    auto const negativeOverflow = Rational::fromFloat(-1e20, dp9);
    REQUIRE_FALSE(negativeOverflow.has_value());
    CHECK(negativeOverflow.error() == RationalError::Overflow);

    // Near the negative boundary: just under (in magnitude) scales fine,
    // just past -2^63 is rejected. INT64_MIN itself (== -2^63 exactly) is a
    // valid llround result and must NOT be rejected by this guard.
    auto const justUnderNegative = Rational::fromFloat(-4.0e17, dp1);  // scaled -4e18, > -2^63
    REQUIRE(justUnderNegative.has_value());
    auto const justPastNegative = Rational::fromFloat(-1.0e18, dp1);  // scaled -1e19, < -2^63
    REQUIRE_FALSE(justPastNegative.has_value());
    CHECK(justPastNegative.error() == RationalError::Overflow);
}

// ---------------------------------------------------------------------------
// Rational — exact arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("Rational::ExactArithmetic", "[rational]")
{
    CHECK(Rational { Numerator{1}, Denominator{3}, dp9 } + Rational { Numerator{1}, Denominator{3}, dp9 } + Rational { Numerator{1}, Denominator{3}, dp9 }
          == Rational::one(dp9));
    CHECK(Rational { Numerator{1}, Denominator{10}, dp9 } + Rational { Numerator{2}, Denominator{10}, dp9 } == Rational { Numerator{3}, Denominator{10}, dp9 });
    CHECK(Rational { Numerator{1}, Denominator{7}, dp9 } * Rational { 7, dp9 } == Rational::one(dp9));
    CHECK(Rational { Numerator{5}, Denominator{6}, dp9 } - Rational { Numerator{1}, Denominator{6}, dp9 } == Rational { Numerator{2}, Denominator{3}, dp9 });
    // Negative left operand exercises the sign handling in the cross-cancel.
    CHECK(Rational { Numerator{-2}, Denominator{3}, dp9 } * Rational { Numerator{3}, Denominator{2}, dp9 } == -Rational::one(dp9));

    auto const tenth = Rational { Numerator{1}, Denominator{10}, dp9 };
    auto const fifth = Rational { Numerator{1}, Denominator{5}, dp9 };
    CHECK(tenth + fifth == Rational { Numerator{3}, Denominator{10}, dp9 });
}

TEST_CASE("Rational::Comparison", "[rational]")
{
    CHECK(Rational { Numerator{2}, Denominator{4}, dp9 } == Rational { Numerator{1}, Denominator{2}, dp9 });
    CHECK(Rational { Numerator{1}, Denominator{2}, dp9 } != Rational { Numerator{1}, Denominator{3}, dp9 });
    CHECK(Rational { Numerator{1}, Denominator{3}, dp9 } < Rational { Numerator{1}, Denominator{2}, dp9 });
    CHECK(Rational { Numerator{-1}, Denominator{2}, dp9 } < Rational { Numerator{1}, Denominator{100}, dp9 });
    CHECK(Rational { Numerator{1}, Denominator{2}, dp9 } > Rational { Numerator{1}, Denominator{3}, dp9 });
    CHECK(Rational { 0, dp9 } == Rational::zero(dp9));
    CHECK((Rational { Numerator{2}, Denominator{4}, dp9 } <=> Rational { Numerator{1}, Denominator{2}, dp9 }) == std::strong_ordering::equal);
    CHECK((Rational { Numerator{1}, Denominator{3}, dp9 } <=> Rational { Numerator{1}, Denominator{2}, dp9 }) == std::strong_ordering::less);

    // Precision-independent: same value at different precision compares equal.
    CHECK(Rational { Numerator{1}, Denominator{2}, dp3 } == Rational { Numerator{1}, Denominator{2}, dp9 });
    CHECK((Rational { Numerator{1}, Denominator{2}, dp3 } <=> Rational { Numerator{1}, Denominator{2}, dp9 }) == std::strong_ordering::equal);
}

TEST_CASE("Rational::DivisionByZero", "[rational]")
{
    auto const result = Rational { Numerator{1}, Denominator{2}, dp9 } / Rational::zero(dp9);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::DivisionByZero);

    auto const named = Rational { Numerator{1}, Denominator{2}, dp9 }.dividedBy(Rational::zero(dp9));
    REQUIRE_FALSE(named.has_value());
    CHECK(named.error() == RationalError::DivisionByZero);

    auto const chained = Rational::from(Numerator{1}, Denominator{2}, dp9).and_then(
        [](Rational value) { return value.dividedBy(Rational::zero(dp9)); });
    REQUIRE_FALSE(chained.has_value());
    CHECK(chained.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::reciprocal", "[rational]")
{
    auto const reciprocal = Rational { Numerator{3}, Denominator{4}, dp9 }.reciprocal();
    REQUIRE(reciprocal.has_value());
    CHECK(*reciprocal == Rational { Numerator{4}, Denominator{3}, dp9 });

    auto const negativeReciprocal = Rational { Numerator{-3}, Denominator{4}, dp9 }.reciprocal();
    REQUIRE(negativeReciprocal.has_value());
    CHECK(*negativeReciprocal == Rational { Numerator{-4}, Denominator{3}, dp9 });
    CHECK(negativeReciprocal->denominator > 0);

    auto const zeroReciprocal = Rational::zero(dp9).reciprocal();
    REQUIRE_FALSE(zeroReciprocal.has_value());
    CHECK(zeroReciprocal.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Conversions", "[rational]")
{
    CHECK(Rational { Numerator{1}, Denominator{2}, dp9 }.toDouble() == Catch::Approx(0.5));
    CHECK(Rational { Numerator{-3}, Denominator{4}, dp9 }.toDouble() == Catch::Approx(-0.75));

    CHECK(trunc(Rational { Numerator{7}, Denominator{2}, dp9 }) == 3);
    CHECK(trunc(Rational { Numerator{-7}, Denominator{2}, dp9 }) == -3);
    CHECK(trunc(Rational { Numerator{6}, Denominator{2}, dp9 }) == 3);

    CHECK(floor(Rational { Numerator{7}, Denominator{2}, dp9 }) == 3);
    CHECK(floor(Rational { Numerator{-7}, Denominator{2}, dp9 }) == -4);
    CHECK(floor(Rational { Numerator{6}, Denominator{2}, dp9 }) == 3);

    CHECK(ceil(Rational { Numerator{7}, Denominator{2}, dp9 }) == 4);
    CHECK(ceil(Rational { Numerator{-7}, Denominator{2}, dp9 }) == -3);
    CHECK(ceil(Rational { Numerator{6}, Denominator{2}, dp9 }) == 3);
}

TEST_CASE("Rational::OverflowReduction", "[rational]")
{
    auto constexpr large = std::int64_t { std::numeric_limits<std::int32_t>::max() };
    auto const left = Rational { Numerator{large}, Denominator{1}, dp9 };
    auto const right = Rational { Numerator{1}, Denominator{large}, dp9 };
    auto const sum = left + right;
    CHECK(sum.denominator == large);
    CHECK(sum.numerator == (large * large) + 1);

    auto const cancelling = Rational { Numerator{large}, Denominator{7}, dp9 } * Rational { Numerator{7}, Denominator{large}, dp9 };
    CHECK(cancelling == Rational::one(dp9));
}

// ---------------------------------------------------------------------------
// Rational — mixed-type expressions with expected propagation
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Mixed::ResultTypes", "[rational]")
{
    using Expected = std::expected<Rational, RationalError>;
    auto const oneHalf = Rational { Numerator{1}, Denominator{2}, dp9 };
    auto const oneQuarter = Rational { Numerator{1}, Denominator{4}, dp9 };
    auto const expectedHalf = Expected { oneHalf };

    CHECK(*(oneHalf + 0.25) == Rational { Numerator{3}, Denominator{4}, dp9 });
    CHECK(*(0.25 + oneHalf) == Rational { Numerator{3}, Denominator{4}, dp9 });
    CHECK(*(expectedHalf + oneQuarter) == Rational { Numerator{3}, Denominator{4}, dp9 });
    CHECK(*(oneHalf + expectedHalf) == Rational::one(dp9));
    CHECK(*(expectedHalf + expectedHalf) == Rational::one(dp9));
    CHECK(*(expectedHalf - oneQuarter) == Rational { Numerator{1}, Denominator{4}, dp9 });
    CHECK(*(oneHalf * 2.0) == Rational::one(dp9));
    CHECK(*(expectedHalf / 2.0) == Rational { Numerator{1}, Denominator{4}, dp9 });
}

TEST_CASE("Rational::Mixed::ExactValuesHappy", "[rational]")
{
    auto const sum = Rational { Numerator{1}, Denominator{2}, dp9 } + 0.25;
    REQUIRE(sum.has_value());
    CHECK(*sum == Rational { Numerator{3}, Denominator{4}, dp9 });

    auto const composed =
        Rational { Numerator{1}, Denominator{2}, dp9 } / Rational { 2, dp9 } + Rational { Numerator{1}, Denominator{4}, dp9 } * 2.0;
    REQUIRE(composed.has_value());
    CHECK(*composed == Rational { Numerator{3}, Denominator{4}, dp9 });

    auto const sevenHalves = Rational { Numerator{7}, Denominator{2}, dp9 };
    auto const two = Rational { 2, dp9 };
    auto const oneHalf = Rational { Numerator{1}, Denominator{2}, dp9 };
    auto const expression = sevenHalves / two + oneHalf * 3.5;
    REQUIRE(expression.has_value());
    CHECK(*expression == Rational { Numerator{7}, Denominator{2}, dp9 });
}

TEST_CASE("Rational::Mixed::ErrorPropagation::DivByZero", "[rational]")
{
    auto const result = Rational { Numerator{1}, Denominator{2}, dp9 } / Rational::zero(dp9) + Rational { Numerator{3}, Denominator{4}, dp9 };
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::DivisionByZero);

    auto const otherSide = Rational { Numerator{1}, Denominator{4}, dp9 } + Rational { 1, dp9 } / Rational::zero(dp9);
    REQUIRE_FALSE(otherSide.has_value());
    CHECK(otherSide.error() == RationalError::DivisionByZero);

    auto const product = (Rational { 1, dp9 } / Rational::zero(dp9)) * Rational { Numerator{7}, Denominator{8}, dp9 };
    REQUIRE_FALSE(product.has_value());
    CHECK(product.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Mixed::ErrorPropagation::FloatNonFinite", "[rational]")
{
    auto const nan = std::numeric_limits<double>::quiet_NaN();
    auto const result = Rational::one(dp9) + nan;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::NotFinite);

    auto const positiveInfinity = std::numeric_limits<double>::infinity();
    auto const product = Rational { Numerator{1}, Denominator{2}, dp9 } * positiveInfinity;
    REQUIRE_FALSE(product.has_value());
    CHECK(product.error() == RationalError::NotFinite);

    auto const negativeInfinity = -std::numeric_limits<double>::infinity();
    auto const difference = Rational { Numerator{1}, Denominator{2}, dp9 } - negativeInfinity;
    REQUIRE_FALSE(difference.has_value());
    CHECK(difference.error() == RationalError::NotFinite);
}

TEST_CASE("Rational::Mixed::ErrorPropagation::FirstErrorWins", "[rational]")
{
    auto const nan = std::numeric_limits<double>::quiet_NaN();
    auto const result = (Rational::one(dp9) / Rational::zero(dp9)) + nan;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == RationalError::DivisionByZero);
}

// ---------------------------------------------------------------------------
// Rational — std::format support
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Formatter::Default", "[rational]")
{
    CHECK(std::format("{}", Rational { Numerator{3}, Denominator{4}, dp9 }) == "3/4");
    CHECK(std::format("{}", Rational { Numerator{7}, Denominator{1}, dp9 }) == "7");
    CHECK(std::format("{}", Rational { Numerator{-3}, Denominator{4}, dp9 }) == "-3/4");
    CHECK(std::format("{}", Rational::zero(dp9)) == "0");
    CHECK(std::format("{}", Rational::one(dp9)) == "1");
}

TEST_CASE("Rational::Formatter::DelegatesToDouble", "[rational]")
{
    CHECK(std::format("{:.3f}", Rational { Numerator{1}, Denominator{3}, dp9 }) == "0.333");
    CHECK(std::format("{:.6f}", Rational { Numerator{22}, Denominator{7}, dp9 }) == "3.142857");
    CHECK(std::format("{:+.2e}", Rational { Numerator{-1}, Denominator{4}, dp9 }) == "-2.50e-01");

    auto const ratio = Rational { Numerator{1}, Denominator{8}, dp9 };
    CHECK(std::format("{:.4f}", ratio) == std::format("{:.4f}", ratio.toDouble()));
    CHECK(std::format("{:10.4g}", ratio) == std::format("{:10.4g}", ratio.toDouble()));
}

// ---------------------------------------------------------------------------
// Rational — trivial-copyability contract
// ---------------------------------------------------------------------------

TEST_CASE("Rational::TriviallyCopyable", "[rational]")
{
    auto const original = Rational { Numerator{-3}, Denominator{4}, dp9 };
    Rational copy {};
    std::memcpy(&copy, &original, sizeof(Rational));
    CHECK(copy == original);
    CHECK(copy.numerator == -3);
    CHECK(copy.denominator == 4);
    CHECK(copy.getDecimalPlaces() == dp9);
}

// ---------------------------------------------------------------------------
// Rational — toDouble precision behaviour
// ---------------------------------------------------------------------------

TEST_CASE("Rational::toDouble::PrecisionRoundtrip", "[rational]")
{
    auto const oneThird = *Rational::fromFloat(1.0 / 3.0, dp9);

    CHECK(oneThird.toDouble() == Catch::Approx(0.333333333).epsilon(1e-12));
    CHECK(oneThird.toDouble(3) == Catch::Approx(0.333).epsilon(1e-12));
    CHECK(oneThird.toDouble(1) == Catch::Approx(0.3).epsilon(1e-12));
    CHECK(oneThird.toDouble(6) == Catch::Approx(0.333333).epsilon(1e-12));
    CHECK(oneThird.toDouble(9) == Catch::Approx(oneThird.toDouble()).epsilon(1e-15));

    auto const negativeQuarter = Rational { Numerator{-1}, Denominator{4}, dp9 };
    CHECK(negativeQuarter.toDouble(2) == Catch::Approx(-0.25).epsilon(1e-12));
    CHECK(negativeQuarter.toDouble(1) == Catch::Approx(-0.3).epsilon(1e-12));
}

TEST_CASE("Rational::CommonArithmetic::PiApproximation22Over7", "[rational]")
{
    auto const exactQuotient = Rational { 22, dp3 } / Rational { 7, dp3 };
    REQUIRE(exactQuotient.has_value());
    CHECK(*exactQuotient == Rational { Numerator{22}, Denominator{7}, dp3 });
    CHECK(exactQuotient->toDouble() == Catch::Approx(3.143).epsilon(1e-12));
    CHECK(exactQuotient->toDouble(3) == Catch::Approx(3.143).epsilon(1e-12));
    CHECK(std::format("{:.3f}", *exactQuotient) == "3.143");

    auto const dividend = Rational::fromFloat(22.0, dp3);
    auto const divisor = Rational::fromFloat(7.0, dp3);
    REQUIRE(dividend.has_value());
    REQUIRE(divisor.has_value());
    auto const liftedQuotient = *dividend / *divisor;
    REQUIRE(liftedQuotient.has_value());
    CHECK(*liftedQuotient == Rational { Numerator{22}, Denominator{7}, dp3 });
    CHECK(liftedQuotient->toDouble() == Catch::Approx(3.143).epsilon(1e-12));

    auto const mixedQuotient = Rational { 22, dp3 } / 7.0;
    REQUIRE(mixedQuotient.has_value());
    CHECK(*mixedQuotient == Rational { Numerator{22}, Denominator{7}, dp3 });
    CHECK(mixedQuotient->toDouble() == Catch::Approx(3.143).epsilon(1e-12));
}

TEST_CASE("Rational::FreeFunctions::AbsCeilFloorTrunc", "[rational]")
{
    CHECK(abs(Rational { Numerator{-3}, Denominator{4}, dp9 }) == Rational { Numerator{3}, Denominator{4}, dp9 });
    CHECK(abs(Rational { Numerator{3}, Denominator{4}, dp9 }) == Rational { Numerator{3}, Denominator{4}, dp9 });
    CHECK(abs(Rational::zero(dp9)) == Rational::zero(dp9));

    CHECK(ceil(Rational { Numerator{7}, Denominator{2}, dp9 }) == 4);
    CHECK(floor(Rational { Numerator{7}, Denominator{2}, dp9 }) == 3);
    CHECK(trunc(Rational { Numerator{7}, Denominator{2}, dp9 }) == 3);

    using std::abs;
    using std::ceil;
    using std::floor;
    using std::trunc;
    CHECK(abs(-2.5) == Catch::Approx(2.5));
    CHECK(abs(Rational { Numerator{-3}, Denominator{4}, dp9 }) == Rational { Numerator{3}, Denominator{4}, dp9 });
    CHECK(ceil(2.1) == Catch::Approx(3.0));
    CHECK(ceil(Rational { Numerator{7}, Denominator{2}, dp9 }) == 4);
}

// ---------------------------------------------------------------------------
// Rational — cross-precision arithmetic (now runtime max-propagation)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::CrossPrecision::MaxPropagates", "[rational]")
{
    // Result precision is the max of the two operands' precisions.
    CHECK((Rational { Numerator{1}, Denominator{2}, dp3 } + Rational { Numerator{1}, Denominator{4}, dp9 }).getDecimalPlaces() == dp9);
    CHECK((Rational { Numerator{1}, Denominator{2}, dp9 } + Rational { Numerator{1}, Denominator{4}, dp3 }).getDecimalPlaces() == dp9);
    CHECK((Rational { Numerator{1}, Denominator{2}, dp3 } + Rational { Numerator{1}, Denominator{4}, dp3 }).getDecimalPlaces() == dp3);
    CHECK((Rational { Numerator{1}, Denominator{2}, dp3 } - Rational { Numerator{1}, Denominator{4}, dp9 }).getDecimalPlaces() == dp9);
    CHECK((Rational { Numerator{1}, Denominator{2}, dp3 } * Rational { Numerator{1}, Denominator{4}, dp9 }).getDecimalPlaces() == dp9);

    auto const quotient = Rational { Numerator{3}, Denominator{4}, dp3 } / Rational { Numerator{1}, Denominator{2}, dp9 };
    REQUIRE(quotient.has_value());
    CHECK(quotient->getDecimalPlaces() == dp9);
}

TEST_CASE("Rational::CrossPrecision::ValueCorrectness", "[rational]")
{
    auto const sum = Rational { Numerator{1}, Denominator{2}, dp3 } + Rational { Numerator{1}, Denominator{4}, dp9 };
    CHECK(sum == Rational { Numerator{3}, Denominator{4}, dp9 });

    auto const difference = Rational { Numerator{2}, Denominator{3}, dp9 } - Rational { Numerator{1}, Denominator{3}, dp3 };
    CHECK(difference == Rational { Numerator{1}, Denominator{3}, dp9 });

    auto const product = Rational { Numerator{2}, Denominator{3}, dp3 } * Rational { Numerator{3}, Denominator{2}, dp9 };
    CHECK(product == Rational::one(dp9));

    auto const quotient = Rational { Numerator{3}, Denominator{4}, dp3 } / Rational { Numerator{1}, Denominator{2}, dp9 };
    REQUIRE(quotient.has_value());
    CHECK(*quotient == Rational { Numerator{3}, Denominator{2}, dp9 });
}

TEST_CASE("Rational::Mixed::FloatInheritsRationalPrecision", "[rational]")
{
    static_assert(std::same_as<decltype(Rational {} + 3.5),
                               std::expected<Rational, RationalError>>);
    static_assert(std::same_as<decltype(3.5 + Rational {}),
                               std::expected<Rational, RationalError>>);

    // 1/2 (precision 3) + 0.25 (lifted at precision 3) = 3/4 at precision 3.
    auto const lifted = Rational { Numerator{1}, Denominator{2}, dp3 } + 0.25;
    REQUIRE(lifted.has_value());
    CHECK(lifted->getDecimalPlaces() == dp3);
    CHECK(*lifted == Rational { Numerator{3}, Denominator{4}, dp3 });

    // Float lifts to precision 9 when the Rational operand carries 9.
    auto const liftedAt9 = Rational { Numerator{1}, Denominator{2}, dp9 } + 0.25;
    REQUIRE(liftedAt9.has_value());
    CHECK(liftedAt9->getDecimalPlaces() == dp9);
}

// ---------------------------------------------------------------------------
// Rational — predicates, rounding fallback, and exact-comparison extremes
// (morph additions)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Predicates::isNegative", "[rational]")
{
    CHECK(Rational { Numerator{-1}, Denominator{2}, dp9 }.isNegative());
    CHECK(Rational { Numerator{1}, Denominator{-2}, dp9 }.isNegative());
    CHECK_FALSE(Rational { Numerator{1}, Denominator{2}, dp9 }.isNegative());
    CHECK_FALSE(Rational::zero(dp9).isNegative());
}

TEST_CASE("Rational::toDouble::OverlargePrecisionFallsBackUnrounded", "[rational]")
{
    // Requested precision beyond 18 cannot be scaled in int64; the conversion
    // falls back to the unrounded quotient.
    auto const oneThird = Rational { Numerator{1}, Denominator{3}, dp9 };
    CHECK(oneThird.toDouble(19) == Catch::Approx(1.0 / 3.0).epsilon(1e-15));
    CHECK(oneThird.toDouble(99) == Catch::Approx(1.0 / 3.0).epsilon(1e-15));
}

TEST_CASE("Rational::Mixed::ErrorPropagation::EveryOperatorBothSides", "[rational]")
{
    auto const poisoned = Rational::one(dp9) / Rational::zero(dp9);
    REQUIRE_FALSE(poisoned.has_value());

    auto const leftMinus = poisoned - Rational::one(dp9);
    REQUIRE_FALSE(leftMinus.has_value());
    CHECK(leftMinus.error() == RationalError::DivisionByZero);

    auto const leftDiv = poisoned / Rational::one(dp9);
    REQUIRE_FALSE(leftDiv.has_value());
    CHECK(leftDiv.error() == RationalError::DivisionByZero);

    auto const rightDiv = Rational::one(dp9) / poisoned;
    REQUIRE_FALSE(rightDiv.has_value());
    CHECK(rightDiv.error() == RationalError::DivisionByZero);

    auto const rightMinus = Rational::one(dp9) - poisoned;
    REQUIRE_FALSE(rightMinus.has_value());
    CHECK(rightMinus.error() == RationalError::DivisionByZero);
}

TEST_CASE("Rational::Comparison::ExactAtInt64Extremes", "[rational]")
{
    // The cross products of these two differ by exactly 1 at ~2^124 — a
    // long-double comparison would round both to the same value and call
    // them equal. The 128-bit comparison must not.
    auto constexpr big = std::int64_t { 1 } << 62;
    auto const left = Rational { Numerator{big + 1}, Denominator{big}, dp9 };    // 1 + 1/2^62
    auto const right = Rational { Numerator{big}, Denominator{big - 1}, dp9 };   // 1 + 1/(2^62-1)

    CHECK(left != right);
    CHECK(left < right);
    CHECK((left <=> right) == std::strong_ordering::less);
    CHECK((right <=> left) == std::strong_ordering::greater);

    // The negative mirror flips the ordering (both directions)...
    CHECK(-left > -right);
    CHECK(((-left) <=> (-right)) == std::strong_ordering::greater);
    CHECK(((-right) <=> (-left)) == std::strong_ordering::less);
    // ...and equal values stay equal through the sign-flipped path.
    CHECK(((-left) <=> (-left)) == std::strong_ordering::equal);

    // Canonical zeros compare equal regardless of precision.
    CHECK((Rational::zero(dp3) <=> Rational::zero(dp9)) == std::strong_ordering::equal);
}

// ---------------------------------------------------------------------------
// Rational — Glaze wire codec (morph addition)
// ---------------------------------------------------------------------------

TEST_CASE("Rational::Wire::Int64MinComponentsClamp", "[rational]")
{
    // INT64_MIN cannot be negated; the codec clamps it to -INT64_MAX so the
    // canonicalising constructor (and later arithmetic) stays defined.
    Rational numeratorCase {};
    REQUIRE_FALSE(glz::read_json(numeratorCase, R"({"num":-9223372036854775808,"den":2,"dp":1})"));
    CHECK(numeratorCase.denominator > 0);
    CHECK(numeratorCase == Rational { Numerator{-std::numeric_limits<std::int64_t>::max()}, Denominator{2}, dp1 });

    Rational denominatorCase {};
    REQUIRE_FALSE(glz::read_json(denominatorCase, R"({"num":3,"den":-9223372036854775808,"dp":1})"));
    CHECK(denominatorCase.denominator > 0);
    CHECK(denominatorCase.isNegative());
}

TEST_CASE("Rational::Wire::RoundTrip", "[rational]")
{
    auto const original = Rational { Numerator{617}, Denominator{50}, dp2 };
    auto const json = glz::write_json(original);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"num":617,"den":50,"dp":2})");

    Rational restored {};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored == original);
    CHECK(restored.getDecimalPlaces() == dp2);
}

TEST_CASE("Rational::Wire::CanonicalisesOnRead", "[rational]")
{
    // A non-canonical payload (1234/100) must land reduced: the codec goes
    // through the canonicalising constructor, so invariants always hold.
    Rational value {};
    REQUIRE_FALSE(glz::read_json(value, R"({"num":1234,"den":100,"dp":2})"));
    CHECK(value.numerator == 617);
    CHECK(value.denominator == 50);
    CHECK(value == Rational { Numerator{1234}, Denominator{100}, dp2 });

    Rational negative {};
    REQUIRE_FALSE(glz::read_json(negative, R"({"num":3,"den":-4,"dp":1})"));
    CHECK(negative.numerator == -3);
    CHECK(negative.denominator == 4);
}

TEST_CASE("Rational::Wire::HostileInputClamps", "[rational]")
{
    // Zero denominator and out-of-range (too high) precision are clamped, not
    // asserted: wire input is untrusted.
    Rational hostile {};
    REQUIRE_FALSE(glz::read_json(hostile, R"({"num":5,"den":0,"dp":99})"));
    CHECK(hostile.denominator == 1);
    CHECK(hostile.numerator == 5);
    CHECK(hostile.getDecimalPlaces() == DecimalPlaces { kMaxDecimalPlaces });

    // dp:0 is a legal precision (the floor is 0, not 1): it is honored as-is,
    // not clamped up.
    Rational zeroPrecision {};
    REQUIRE_FALSE(glz::read_json(zeroPrecision, R"({"num":1,"den":2,"dp":0})"));
    CHECK(zeroPrecision.getDecimalPlaces() == DecimalPlaces { 0 });
}

TEST_CASE("Rational::Wire::MissingFieldsUseWireDefaults", "[rational]")
{
    // Absent keys fall back to Wire's member defaults (0/1 at precision 1).
    Rational value { Numerator{9}, Denominator{4}, dp9 };
    REQUIRE_FALSE(glz::read_json(value, R"({})"));
    CHECK(value == Rational::zero(dp1));
    CHECK(value.getDecimalPlaces() == dp1);
}

TEST_CASE("Rational::Wire::NullableComposition", "[rational]")
{
    std::optional<Rational> maybe {};
    REQUIRE_FALSE(glz::read_json(maybe, "null"));
    CHECK_FALSE(maybe.has_value());

    REQUIRE_FALSE(glz::read_json(maybe, R"({"num":1,"den":3,"dp":9})"));
    CHECK(maybe == std::optional<Rational> { Rational { Numerator{1}, Denominator{3}, dp9 } });

    auto const written = glz::write_json(maybe);
    REQUIRE(written.has_value());
    CHECK(*written == R"({"num":1,"den":3,"dp":9})");
}
