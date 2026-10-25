// SPDX-License-Identifier: Apache-2.0
//
// Rational's checked arithmetic: report overflow instead of committing it.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <morph/core/logger.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using morph::math::checkedAdd;
using morph::math::checkedDiv;
using morph::math::checkedMul;
using morph::math::checkedSub;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::math::RationalError;

namespace {

constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] Rational whole(std::int64_t value) {
    return Rational{Numerator{value}, Denominator{1}, DecimalPlaces{2}};
}

}  // namespace

TEST_CASE("Overflow predicates answer from the operands, never by overflowing", "[rational][checked]") {
    using morph::math::detail::addOverflows;
    using morph::math::detail::mulOverflows;
    using morph::math::detail::subOverflows;

    CHECK_FALSE(addOverflows(1, 2));
    CHECK(addOverflows(kMax, 1));
    CHECK(addOverflows(kMin, -1));
    CHECK_FALSE(addOverflows(kMax, -1));

    CHECK_FALSE(subOverflows(2, 1));
    CHECK(subOverflows(kMin, 1));
    CHECK(subOverflows(kMax, -1));
    CHECK_FALSE(subOverflows(kMin, -1));

    CHECK_FALSE(mulOverflows(0, kMin));
    CHECK_FALSE(mulOverflows(kMin, 0));
    CHECK_FALSE(mulOverflows(3, 3));
    CHECK(mulOverflows(kMax, 2));
    CHECK(mulOverflows(2, kMax));
    CHECK(mulOverflows(-4000000000LL, -4000000000LL));  // both negative
    CHECK(mulOverflows(2, kMin / 2 - 1));               // positive x negative
    CHECK(mulOverflows(kMin / 2 - 1, 2));               // negative x positive
    // -1 is the factor that overflows without either operand being large.
    CHECK(mulOverflows(-1, kMin));
    CHECK(mulOverflows(kMin, -1));
    CHECK_FALSE(mulOverflows(-1, kMax));
}

TEST_CASE("checkedAdd returns the same value operator+ would, when it fits", "[rational][checked]") {
    const auto sum = checkedAdd(whole(2), whole(3));
    REQUIRE(sum.has_value());
    CHECK(*sum == whole(5));
    CHECK(*sum == whole(2) + whole(3));
}

TEST_CASE("checkedAdd reports overflow instead of committing it", "[rational][checked]") {
    const auto sum = checkedAdd(whole(kMax), whole(1));
    REQUIRE_FALSE(sum.has_value());
    CHECK(sum.error() == RationalError::Overflow);
}

TEST_CASE("checkedAdd catches an overflowing cross-term whose result would have fit", "[rational][checked]") {
    // The case a caller cannot detect by inspecting the answer: the final sum
    // is small, but reaching it requires an intermediate that does not fit.
    // 1/kMax + 1/(kMax-1) has a tiny value and an unrepresentable denominator.
    const Rational lhs{Numerator{1}, Denominator{kMax}, DecimalPlaces{2}};
    const Rational rhs{Numerator{1}, Denominator{kMax - 1}, DecimalPlaces{2}};

    const auto sum = checkedAdd(lhs, rhs);
    REQUIRE_FALSE(sum.has_value());
    CHECK(sum.error() == RationalError::Overflow);
}

// Coverage: rational.hpp:845 and rational.hpp:859, addWouldOverflow's and
// subWouldOverflow's first two disjuncts (the numerator-side cross-term
// overflow checks). Every existing overflow test above only ever triggers
// the final addOverflows/subOverflows check (849/863) or, for addition, the
// denominator-product clause -- never a numerator whose *rescaled* product
// alone overflows. These two constructions isolate each clause: a large
// numerator on one side against a small, non-unit denominator on the other
// (so rightScaled/leftScaled == 3), with nothing else overflowing.
TEST_CASE("checkedAdd/checkedSub catch a numerator whose rescaled product alone overflows", "[rational][checked]") {
    // *this's numerator, rescaled by rightScaled == 3, overflows (845/859's
    // first disjunct: mulOverflows(numerator, rightScaled)).
    const Rational lhs{Numerator{kMax}, Denominator{1}, DecimalPlaces{0}};
    const Rational rhs{Numerator{1}, Denominator{3}, DecimalPlaces{0}};

    const auto addResult = checkedAdd(lhs, rhs);
    REQUIRE_FALSE(addResult.has_value());
    CHECK(addResult.error() == RationalError::Overflow);
    const auto subResult = checkedSub(lhs, rhs);
    REQUIRE_FALSE(subResult.has_value());
    CHECK(subResult.error() == RationalError::Overflow);

    // Mirrored: rhs's numerator, rescaled by leftScaled == 3, overflows
    // (845/859's second disjunct: mulOverflows(rhs.numerator, leftScaled)).
    const Rational lhsSmall{Numerator{1}, Denominator{3}, DecimalPlaces{0}};
    const Rational rhsLarge{Numerator{kMax}, Denominator{1}, DecimalPlaces{0}};

    const auto addResult2 = checkedAdd(lhsSmall, rhsLarge);
    REQUIRE_FALSE(addResult2.has_value());
    CHECK(addResult2.error() == RationalError::Overflow);
    const auto subResult2 = checkedSub(lhsSmall, rhsLarge);
    REQUIRE_FALSE(subResult2.has_value());
    CHECK(subResult2.error() == RationalError::Overflow);
}

// Coverage: rational.hpp:860, subWouldOverflow's third disjunct (the
// denominator-product overflow check) -- covered for addWouldOverflow
// already, but never observed true for subWouldOverflow. Numerators of 1 on
// both sides make the first two disjuncts false (1 * anything representable
// never overflows), isolating the denominator product.
TEST_CASE("checkedSub catches a denominator-product overflow that checkedAdd's own third clause already covers",
          "[rational][checked]") {
    const Rational lhs{Numerator{1}, Denominator{4'000'000'000LL}, DecimalPlaces{0}};
    const Rational rhs{Numerator{1}, Denominator{4'000'000'001LL}, DecimalPlaces{0}};

    const auto subResult = checkedSub(lhs, rhs);
    REQUIRE_FALSE(subResult.has_value());
    CHECK(subResult.error() == RationalError::Overflow);
}

// Coverage: rational.hpp:882, mulWouldOverflow's denominator-side product
// check. The numerator-side product (881) is exercised elsewhere; unit
// numerators on both sides keep cross-cancellation trivial (crossDivisorOne
// == crossDivisorTwo == 1) so the denominator product alone overflows.
TEST_CASE("checkedMul catches a denominator-side product overflow after trivial cross-cancellation",
          "[rational][checked]") {
    const Rational lhs{Numerator{1}, Denominator{4'000'000'000LL}, DecimalPlaces{0}};
    const Rational rhs{Numerator{1}, Denominator{4'000'000'000LL}, DecimalPlaces{0}};

    const auto product = checkedMul(lhs, rhs);
    REQUIRE_FALSE(product.has_value());
    CHECK(product.error() == RationalError::Overflow);
}

TEST_CASE("checkedSub mirrors checkedAdd", "[rational][checked]") {
    const auto ok = checkedSub(whole(5), whole(3));
    REQUIRE(ok.has_value());
    CHECK(*ok == whole(2));

    // kMin + 1 (== -INT64_MAX), not kMin: constructing a Rational whose
    // numerator is INT64_MIN is itself undefined behaviour -- canonicalise()
    // negates the numerator unguarded, and -INT64_MIN is not representable.
    // setWire guards that case on the wire path; the public constructor does
    // not. Tracked separately; this test stays inside the representable range
    // so it measures checkedSub rather than that.
    //
    // -INT64_MAX - 2 is the first difference that genuinely does not fit
    // (-INT64_MAX - 1 is exactly INT64_MIN, which still does).
    const auto bad = checkedSub(whole(kMin + 1), whole(2));
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == RationalError::Overflow);
}

TEST_CASE("checkedMul checks the cross-cancelled factors, not the raw operands", "[rational][checked]") {
    // kMax/2 * 2/1 cross-cancels to kMax/1 and multiplies fine. Checking the
    // raw operands would have rejected it.
    const Rational lhs{Numerator{kMax}, Denominator{2}, DecimalPlaces{2}};
    const Rational rhs{Numerator{2}, Denominator{1}, DecimalPlaces{2}};
    const auto product = checkedMul(lhs, rhs);
    REQUIRE(product.has_value());
    CHECK(product->numerator == kMax);

    const auto bad = checkedMul(whole(kMax), whole(3));
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == RationalError::Overflow);
}

TEST_CASE("checkedMul handles a zero operand without dividing by a zero gcd", "[rational][checked]") {
    const auto product = checkedMul(whole(0), whole(kMax));
    REQUIRE(product.has_value());
    CHECK(product->numerator == 0);
}

TEST_CASE("checkedDiv reports the overflow dividedBy reports as success", "[rational][checked]") {
    // Dividing INT64_MAX by 1/1000000 has an exact quotient of
    // 9223372036854775807000000, which does not fit. dividedBy multiplies by
    // the reciprocal through the saturating path, clamps to INT64_MAX/1, and
    // hands back a *successful* expected -- so the caller who checks it is
    // told the division worked. checkedDiv is the form that says otherwise.
    const morph::log::ScopedLoggerOverride quiet{[](morph::log::LogLevel, std::string_view) {},
                                                 morph::log::LogLevel::error};

    const Rational dividend{Numerator{kMax}, Denominator{1}, DecimalPlaces{0}};
    const Rational divisor{Numerator{1}, Denominator{1'000'000}, DecimalPlaces{6}};

    // The behaviour that stays: dividedBy still saturates, on purpose.
    const auto saturated = dividend.dividedBy(divisor);
    REQUIRE(saturated.has_value());
    CHECK(saturated->numerator == kMax);
    CHECK(saturated->denominator == 1);
    CHECK((dividend / divisor).has_value());

    // The behaviour that is new: the same operands, reported.
    const auto reported = checkedDiv(dividend, divisor);
    REQUIRE_FALSE(reported.has_value());
    CHECK(reported.error() == RationalError::Overflow);
}

TEST_CASE("checkedDiv agrees with operator/ on quotients that fit", "[rational][checked]") {
    const Rational six{Numerator{6}, Denominator{1}, DecimalPlaces{2}};
    const Rational three{Numerator{3}, Denominator{1}, DecimalPlaces{2}};

    const auto quotient = checkedDiv(six, three);
    REQUIRE(quotient.has_value());
    CHECK(*quotient == whole(2));
    CHECK(*quotient == *(six / three));

    // A negative divisor keeps the sign on the numerator, as reciprocal does.
    const auto negative = checkedDiv(six, whole(-3));
    REQUIRE(negative.has_value());
    CHECK(negative->numerator == -2);
    CHECK(negative->denominator == 1);

    // Usable in a constant expression, like the rest of the family.
    static_assert(checkedDiv(Rational{Numerator{6}, Denominator{1}, DecimalPlaces{2}},
                             Rational{Numerator{3}, Denominator{1}, DecimalPlaces{2}})
                      ->numerator == 2);
}

TEST_CASE("checkedDiv reports a zero divisor through the same channel", "[rational][checked]") {
    // Both failure modes reach the caller as one error type: the point of the
    // helper is that a caller need not check the expected *and* separately
    // inspect the value for a clamp.
    const auto byZero = checkedDiv(whole(1), whole(0));
    REQUIRE_FALSE(byZero.has_value());
    CHECK(byZero.error() == RationalError::DivisionByZero);
}

TEST_CASE("checkedDiv checks the cross-cancelled factors, not the raw operands", "[rational][checked]") {
    // kMax/2 divided by 1/2 cross-cancels to kMax/1 and divides fine, so the
    // predicate must be the one operator*= uses -- checking raw operands would
    // reject this pair.
    const Rational lhs{Numerator{kMax}, Denominator{2}, DecimalPlaces{2}};
    const Rational half{Numerator{1}, Denominator{2}, DecimalPlaces{2}};

    const auto quotient = checkedDiv(lhs, half);
    REQUIRE(quotient.has_value());
    CHECK(quotient->numerator == kMax);
    CHECK(quotient->denominator == 1);
}

TEST_CASE("A saturating division names its own site and its own remedy", "[rational][checked][saturate]") {
    // dividedBy saturates through operator*='s arithmetic, and a log that says
    // so names a function the division caller never
    // called, and offering checkedAdd/checkedSub/checkedMul, none of which is
    // a division. Each site now names itself and the one helper that helps.
    std::vector<std::string> logged;
    const morph::log::ScopedLoggerOverride capture{
        [&logged](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::error};

    const Rational dividend{Numerator{kMax}, Denominator{1}, DecimalPlaces{0}};
    const Rational divisor{Numerator{1}, Denominator{1'000'000}, DecimalPlaces{6}};
    (void)dividend.dividedBy(divisor);

    REQUIRE(logged.size() == 1);
    CHECK(logged.front().find("dividedBy") != std::string::npos);
    CHECK(logged.front().find("checkedDiv") != std::string::npos);
    CHECK(logged.front().find("operator*=") == std::string::npos);
    CHECK(logged.front().find("checkedAdd") == std::string::npos);
    CHECK(logged.front().find("checkedMul") == std::string::npos);

    // The three operators keep their own attribution, each naming the one
    // remedy that applies to it.
    logged.clear();
    (void)(whole(kMax) + whole(1));
    (void)(whole(kMin + 1) - whole(2));
    (void)(whole(kMax) * whole(3));
    REQUIRE(logged.size() == 3);
    CHECK(logged[0].find("operator+=") != std::string::npos);
    CHECK(logged[0].find("checkedAdd") != std::string::npos);
    CHECK(logged[1].find("operator-=") != std::string::npos);
    CHECK(logged[1].find("checkedSub") != std::string::npos);
    CHECK(logged[2].find("operator*=") != std::string::npos);
    CHECK(logged[2].find("checkedMul") != std::string::npos);
}

TEST_CASE("Summing at ledger magnitudes reports the boundary rather than crossing it", "[rational][checked]") {
    // The issue's own scenario: dp=2 legs of 10^9 minor units. The boundary is
    // kMax / 10^9 + 1 rows; this walks up to it without ever performing the
    // overflowing addition.
    constexpr std::int64_t leg = 1'000'000'000;
    auto running = Rational{Numerator{kMax - leg}, Denominator{1}, DecimalPlaces{2}};

    const auto stillFits = checkedAdd(running, whole(leg));
    REQUIRE(stillFits.has_value());

    running = *stillFits;
    const auto overflows = checkedAdd(running, whole(1));
    REQUIRE_FALSE(overflows.has_value());
    CHECK(overflows.error() == RationalError::Overflow);
}

// ---------------------------------------------------------------------------
// The saturating operators. Before these, every case below was undefined
// behaviour rather than a wrong-but-defined answer.
// ---------------------------------------------------------------------------

TEST_CASE("operator+ saturates and logs instead of overflowing", "[rational][checked][saturate]") {
    std::vector<std::string> logged;
    const morph::log::ScopedLoggerOverride capture{
        [&logged](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::error};

    const auto sum = whole(kMax) + whole(1);

    CHECK(sum.numerator == kMax);
    CHECK(sum.denominator == 1);
    REQUIRE_FALSE(logged.empty());
    CHECK(logged.front().find("operator+=") != std::string::npos);
}

TEST_CASE("Saturation carries the sign of the true result", "[rational][checked][saturate]") {
    const morph::log::ScopedLoggerOverride quiet{[](morph::log::LogLevel, std::string_view) {},
                                                 morph::log::LogLevel::error};

    CHECK((whole(kMax) + whole(1)).numerator == kMax);
    CHECK((whole(kMin + 1) - whole(2)).numerator == -kMax);
    CHECK((whole(kMax) * whole(3)).numerator == kMax);
    // A negative product must not saturate positive.
    CHECK((whole(kMax) * whole(-3)).numerator == -kMax);
    CHECK((whole(-kMax) * whole(3)).numerator == -kMax);
}

TEST_CASE("An intermediate-only overflow saturates toward the true sign", "[rational][checked][saturate]") {
    const morph::log::ScopedLoggerOverride quiet{[](morph::log::LogLevel, std::string_view) {},
                                                 morph::log::LogLevel::error};

    // 1/kMax + 1/(kMax-1): a tiny *positive* value whose common denominator is
    // unrepresentable. The cross-terms overflow while the result would not,
    // so the saturation direction cannot come from the operands' magnitudes --
    // it comes from exact comparison.
    const Rational lhs{Numerator{1}, Denominator{kMax}, DecimalPlaces{2}};
    const Rational rhs{Numerator{1}, Denominator{kMax - 1}, DecimalPlaces{2}};

    const auto sum = lhs + rhs;
    CHECK(sum.numerator == kMax);  // positive, as the true value is

    const Rational negLhs{Numerator{-1}, Denominator{kMax}, DecimalPlaces{2}};
    const Rational negRhs{Numerator{-1}, Denominator{kMax - 1}, DecimalPlaces{2}};
    CHECK((negLhs + negRhs).numerator == -kMax);
}

TEST_CASE("A numerator of INT64_MIN is clamped, not undefined", "[rational][checked][saturate]") {
    std::vector<std::string> logged;
    const morph::log::ScopedLoggerOverride capture{
        [&logged](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::error};

    // Constructing this was undefined behaviour before: canonicalise() negated
    // the numerator, and -INT64_MIN is not representable.
    const Rational direct{Numerator{kMin}, Denominator{1}, DecimalPlaces{2}};
    CHECK(direct.numerator == -kMax);

    // And arithmetic can land on it exactly, without anyone naming it:
    // -INT64_MAX - 1 is a legal subtraction whose result is INT64_MIN.
    const auto landedOn = whole(-kMax) - whole(1);
    CHECK(landedOn.numerator == -kMax);

    REQUIRE_FALSE(logged.empty());
}

// Coverage: rational.hpp:908/910/911. Every existing clamp test (including
// the one just above) triggers via numerator == INT64_MIN; denominator ==
// INT64_MIN had never been exercised. The public constructor documents
// wantedDenominator as "may be negative or zero on input" and assigns it
// verbatim before canonicalising, so this reaches the clamp directly.
TEST_CASE("A denominator of INT64_MIN is clamped, not undefined", "[rational][checked][saturate]") {
    std::vector<std::string> logged;
    const morph::log::ScopedLoggerOverride capture{
        [&logged](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::error};

    const Rational direct{Numerator{1}, Denominator{kMin}, DecimalPlaces{2}};
    // -INT64_MIN clamps to -INT64_MAX, then the sign-normalisation a few
    // lines below (denominator < 0) flips both components' signs.
    CHECK(direct.denominator == kMax);
    CHECK(direct.numerator == -1);

    REQUIRE_FALSE(logged.empty());
}

TEST_CASE("Saturating operators do not disturb ordinary arithmetic", "[rational][checked][saturate]") {
    CHECK((whole(2) + whole(3)) == whole(5));
    CHECK((whole(5) - whole(3)) == whole(2));
    CHECK((whole(6) * whole(7)) == whole(42));

    const Rational third{Numerator{1}, Denominator{3}, DecimalPlaces{2}};
    const Rational sixth{Numerator{1}, Denominator{6}, DecimalPlaces{2}};
    const auto half = third + sixth;
    CHECK(half.numerator == 1);
    CHECK(half.denominator == 2);
}

TEST_CASE("checked* still report rather than saturate, for callers that must not absorb it",
          "[rational][checked][saturate]") {
    // The division of labour: operators stay usable and defined, checked*
    // stays exact-or-nothing.
    const auto reported = checkedAdd(whole(kMax), whole(1));
    REQUIRE_FALSE(reported.has_value());
    CHECK(reported.error() == RationalError::Overflow);

    const morph::log::ScopedLoggerOverride quiet{[](morph::log::LogLevel, std::string_view) {},
                                                 morph::log::LogLevel::error};
    CHECK((whole(kMax) + whole(1)).numerator == kMax);
}

TEST_CASE("Saturating arithmetic is noexcept even when the log sink throws", "[rational][checked][saturate]") {
    // morph::log offers no noexcept guarantee: a user-installed
    // sink may throw, and detail::log's own scoped_lock may throw
    // std::system_error. An arithmetic operator must not start failing because
    // logging failed, so the reporters swallow. Without that, these operators
    // could not carry noexcept -- and a throw from one would terminate.
    static_assert(noexcept(std::declval<Rational&>() += std::declval<const Rational&>()));
    static_assert(noexcept(std::declval<Rational&>() -= std::declval<const Rational&>()));
    static_assert(noexcept(std::declval<Rational&>() *= std::declval<const Rational&>()));

    const morph::log::ScopedLoggerOverride hostile{
        [](morph::log::LogLevel, std::string_view) { throw std::runtime_error{"sink failed"}; },
        morph::log::LogLevel::error};

    // Each of these overflows, so each reaches the (throwing) sink.
    CHECK_NOTHROW([] { return whole(kMax) + whole(1); }());
    CHECK_NOTHROW([] { return whole(kMin + 1) - whole(2); }());
    CHECK_NOTHROW([] { return whole(kMax) * whole(3); }());
    // And the INT64_MIN clamp path in canonicalise().
    CHECK_NOTHROW([] { return Rational{Numerator{kMin}, Denominator{1}, DecimalPlaces{2}}; }());

    // Still saturated correctly despite the sink failing.
    CHECK((whole(kMax) + whole(1)).numerator == kMax);
}

// Mutation: drop mulWouldOverflow's zero-divisor guard (0/0, which only a hand-written field can make, divides by
// zero).
TEST_CASE("mulWouldOverflow answers false for a 0/0 operand built by writing its fields", "[rational][checked]") {
    Rational broken{};
    broken.numerator = 0;
    broken.denominator = 0;
    const Rational zero{Numerator{0}, Denominator{1}, DecimalPlaces{0}};
    CHECK_FALSE(broken.mulWouldOverflow(broken));
    CHECK_FALSE(zero.mulWouldOverflow(broken));
}

namespace {

using morph::math::detail::U128;

/// A deterministic 64-bit sequence (splitmix64), so a failure names a reproducible case.
struct Sequence {
    std::uint64_t state;
    std::uint64_t next() {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
        return mixed ^ (mixed >> 31U);
    }
    /// A 128-bit value whose width varies, so carries, borrows and both division paths are all reached.
    U128 wide() {
        std::uint64_t const shape = next() % 4U;
        std::uint64_t const high = next();
        std::uint64_t const low = next();
        if (shape == 0) {
            return U128{.hi = 0, .lo = low};
        }
        if (shape == 1) {
            return U128{.hi = high >> (next() % 64U), .lo = low};
        }
        if (shape == 2) {
            return U128{.hi = 0, .lo = low >> (next() % 64U)};
        }
        return U128{.hi = high, .lo = low};
    }
};

}  // namespace

TEST_CASE("The 128-bit integer adds, subtracts, multiplies and divides exactly", "[rational][checked][u128]") {
    using morph::math::detail::checkedAddU128;
    using morph::math::detail::checkedMulU128;
    using morph::math::detail::checkedSubU128;
    using morph::math::detail::divModU128;
    using morph::math::detail::mulU64;
    constexpr auto kAll = std::numeric_limits<std::uint64_t>::max();
    constexpr U128 kTop{.hi = kAll, .lo = kAll};

    CHECK(mulU64(kAll, kAll) == U128{.hi = kAll - 1, .lo = 1});
    CHECK(checkedAddU128(U128{.hi = 0, .lo = kAll}, U128{.hi = 0, .lo = 1}) == U128{.hi = 1, .lo = 0});
    CHECK_FALSE(checkedAddU128(kTop, U128{.hi = 0, .lo = 1}).has_value());
    CHECK_FALSE(checkedAddU128(U128{.hi = kAll, .lo = 0}, U128{.hi = 1, .lo = 0}).has_value());
    CHECK(checkedSubU128(U128{.hi = 1, .lo = 0}, U128{.hi = 0, .lo = 1}) == U128{.hi = 0, .lo = kAll});
    CHECK_FALSE(checkedSubU128(U128{.hi = 0, .lo = 1}, U128{.hi = 0, .lo = 2}).has_value());
    CHECK(checkedMulU128(U128{.hi = 1, .lo = 0}, U128{.hi = 0, .lo = kAll}) == U128{.hi = kAll, .lo = 0});
    CHECK_FALSE(checkedMulU128(U128{.hi = 1, .lo = 0}, U128{.hi = 1, .lo = 0}).has_value());
    CHECK_FALSE(checkedMulU128(U128{.hi = 2, .lo = 0}, U128{.hi = 0, .lo = kAll}).has_value());
    CHECK_FALSE(divModU128(kTop, U128{}).has_value());
    // A divisor past 2^127 makes the shifted remainder drop its top bit: the wrapping subtraction still lands exactly.
    auto const large = divModU128(kTop, U128{.hi = 0x8000000000000000ULL, .lo = 1});
    REQUIRE(large.has_value());
    CHECK(large->quotient == U128{.hi = 0, .lo = 1});
    CHECK(large->remainder == U128{.hi = 0x7fffffffffffffffULL, .lo = kAll - 1});
    CHECK(U128{.hi = 1, .lo = 0} > U128{.hi = 0, .lo = kAll});
}

#ifdef __SIZEOF_INT128__
// Mutation: drop the carry in checkedAddU128, or the carried-bit branch in divModU128's loop -> the native
// comparison fails on the first case that needs it.
TEST_CASE("The 128-bit integer agrees with unsigned __int128", "[rational][checked][u128]") {
    using morph::math::detail::checkedAddU128;
    using morph::math::detail::checkedMulU128;
    using morph::math::detail::checkedSubU128;
    using morph::math::detail::divModU128;
    __extension__ using Native = unsigned __int128;
    auto const native = [](U128 value) { return (static_cast<Native>(value.hi) << 64U) | value.lo; };
    auto const limbs = [](Native value) {
        return U128{.hi = static_cast<std::uint64_t>(value >> 64U), .lo = static_cast<std::uint64_t>(value)};
    };
    Sequence sequence{.state = 20261025};
    for (int round = 0; round < 20000; ++round) {
        U128 const lhs = sequence.wide();
        U128 const rhs = sequence.wide();
        CAPTURE(lhs.hi, lhs.lo, rhs.hi, rhs.lo);
        Native sum = 0;
        bool const sumOverflows = __builtin_add_overflow(native(lhs), native(rhs), &sum);
        auto const added = checkedAddU128(lhs, rhs);
        CHECK(added.has_value() == !sumOverflows);
        if (added && !sumOverflows) {
            CHECK(*added == limbs(sum));
        }
        Native product = 0;
        bool const productOverflows = __builtin_mul_overflow(native(lhs), native(rhs), &product);
        auto const multiplied = checkedMulU128(lhs, rhs);
        CHECK(multiplied.has_value() == !productOverflows);
        if (multiplied && !productOverflows) {
            CHECK(*multiplied == limbs(product));
        }
        auto const subtracted = checkedSubU128(lhs, rhs);
        CHECK(subtracted.has_value() == (native(lhs) >= native(rhs)));
        if (subtracted) {
            CHECK(*subtracted == limbs(native(lhs) - native(rhs)));
        }
        CHECK((lhs < rhs) == (native(lhs) < native(rhs)));
        if (native(rhs) != 0) {
            auto const divided = divModU128(lhs, rhs);
            REQUIRE(divided.has_value());
            CHECK(divided->quotient == limbs(native(lhs) / native(rhs)));
            CHECK(divided->remainder == limbs(native(lhs) % native(rhs)));
        }
    }
}
#endif

// Mutation: round through the saturating roundToDecimalPlaces in checkedRoundToDecimalPlaces -> `10/3` at 18 places
// is clamped and `28/3` is not empty.
TEST_CASE("checkedRoundToDecimalPlaces rounds as roundToDecimalPlaces does, and reports what does not fit",
          "[rational][checked][round]") {
    using morph::math::checkedRoundToDecimalPlaces;
    using morph::math::RoundingMode;
    using morph::math::roundToDecimalPlaces;
    std::vector<std::pair<Rational, std::uint32_t>> const fitting = {
        {Rational{Numerator{1}, Denominator{8}, DecimalPlaces{3}}, 2},
        {Rational{Numerator{-1}, Denominator{8}, DecimalPlaces{3}}, 2},
        {Rational{Numerator{27}, Denominator{200}, DecimalPlaces{3}}, 2},
        {Rational{Numerator{-27}, Denominator{200}, DecimalPlaces{3}}, 2},
        {Rational{Numerator{5}, Denominator{2}, DecimalPlaces{1}}, 0},
        {Rational{Numerator{-5}, Denominator{2}, DecimalPlaces{1}}, 0},
        {Rational{Numerator{1}, Denominator{3}, DecimalPlaces{1}}, 3},
        {Rational{Numerator{-2}, Denominator{3}, DecimalPlaces{1}}, 3},
        {Rational{Numerator{2469}, Denominator{2000}, DecimalPlaces{4}}, 3},
        {Rational{Numerator{1}, Denominator{3}, DecimalPlaces{1}}, 18},
        {Rational{Numerator{9'000'000'000'000'000'000}, Denominator{1}, DecimalPlaces{0}}, 18},
        {Rational{Numerator{-7}, Denominator{1}, DecimalPlaces{0}}, 2},
    };
    for (auto const& [value, places] : fitting) {
        for (RoundingMode const mode : {RoundingMode::HalfEven, RoundingMode::HalfAwayFromZero}) {
            CAPTURE(value.numerator, value.denominator, places, mode == RoundingMode::HalfEven);
            auto const checked = checkedRoundToDecimalPlaces(value, DecimalPlaces{places}, mode);
            REQUIRE(checked.has_value());
            auto const saturating = roundToDecimalPlaces(value, DecimalPlaces{places}, mode);
            CHECK(*checked == saturating);
            CHECK(checked->decimalPlaces == saturating.decimalPlaces);
        }
    }
    // The product passes int64 on the way, but the result fits.
    auto const tenThirds = checkedRoundToDecimalPlaces(Rational{Numerator{10}, Denominator{3}, DecimalPlaces{1}},
                                                       DecimalPlaces{18}, RoundingMode::HalfEven);
    REQUIRE(tenThirds.has_value());
    CHECK(*tenThirds ==
          Rational{Numerator{3'333'333'333'333'333'333}, Denominator{1'000'000'000'000'000'000}, DecimalPlaces{18}});
    // 9.333333333333333333 has a numerator past int64.
    CHECK_FALSE(checkedRoundToDecimalPlaces(Rational{Numerator{28}, Denominator{3}, DecimalPlaces{18}},
                                            DecimalPlaces{18}, RoundingMode::HalfEven)
                    .has_value());
    CHECK(checkedRoundToDecimalPlaces(Rational{Numerator{1}, Denominator{8}, DecimalPlaces{3}}, DecimalPlaces{99},
                                      RoundingMode::HalfEven)
              ->decimalPlaces == DecimalPlaces{18});
}
