// SPDX-License-Identifier: Apache-2.0
//
// Rational's checked arithmetic: report overflow instead of committing it.

#include <catch2/catch_test_macros.hpp>
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
    // morph#206. Dividing INT64_MAX by 1/1000000 has an exact quotient of
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
    // morph#206: dividedBy saturates through operator*='s arithmetic, and the
    // log used to say so -- naming a function the division caller never
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
    // morph::log offers no noexcept guarantee (morph#158): a user-installed
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
