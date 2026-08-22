// SPDX-License-Identifier: Apache-2.0
//
// Rational's checked arithmetic: report overflow instead of committing it.

#include <morph/util/rational.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using morph::math::checkedAdd;
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

TEST_CASE("Overflow predicates answer from the operands, never by overflowing",
          "[rational][checked]") {
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
    CHECK(mulOverflows(-4000000000LL, -4000000000LL));   // both negative
    CHECK(mulOverflows(2, kMin / 2 - 1));                // positive x negative
    CHECK(mulOverflows(kMin / 2 - 1, 2));                // negative x positive
    // -1 is the factor that overflows without either operand being large.
    CHECK(mulOverflows(-1, kMin));
    CHECK(mulOverflows(kMin, -1));
    CHECK_FALSE(mulOverflows(-1, kMax));
}

TEST_CASE("checkedAdd returns the same value operator+ would, when it fits",
          "[rational][checked]") {
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

TEST_CASE("checkedAdd catches an overflowing cross-term whose result would have fit",
          "[rational][checked]") {
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

TEST_CASE("checkedMul checks the cross-cancelled factors, not the raw operands",
          "[rational][checked]") {
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

TEST_CASE("checkedMul handles a zero operand without dividing by a zero gcd",
          "[rational][checked]") {
    const auto product = checkedMul(whole(0), whole(kMax));
    REQUIRE(product.has_value());
    CHECK(product->numerator == 0);
}

TEST_CASE("Summing at ledger magnitudes reports the boundary rather than crossing it",
          "[rational][checked]") {
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
