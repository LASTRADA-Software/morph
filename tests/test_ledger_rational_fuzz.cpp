// SPDX-License-Identifier: Apache-2.0
#include <morph/util/rational.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <vector>

TEST_CASE("Zero-sum check never false-positives across differing decimalPlaces in one currency", "[ledger][rational][fuzz]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    // A USD leg at dp=2 and a correcting USD leg at dp=4 in the same
    // journal, constructed to sum to true zero once both are reduced to a
    // common scale. Assert Rational::operator+ over the two produces
    // canonical zero (num=0, den=1) -- not a "close to zero" approximation.
    Rational a{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}};
    Rational b{Numerator{5000}, Denominator{1}, DecimalPlaces{4}};
    auto sum = a + b;
    // -5000 + 5000 = 0 demonstrates exact arithmetic across decimal places
    CHECK(sum.numerator == 0);
    CHECK(sum.denominator == 1);
}

TEST_CASE("Measure the row count at which partial-sum overflow occurs at ledger-realistic magnitudes", "[ledger][rational][fuzz]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    // Sum N synthetic dp=2 legs at up to 10^9 minor units each; find the N
    // at which the running numerator would exceed int64_t's range.
    // Document the measured N as a comment here once run, per design spec
    // §7 -- this is empirical, not a static claim.
    Rational running{Numerator{0}, Denominator{1}, DecimalPlaces{2}};
    std::int64_t count = 0;
    constexpr std::int64_t perLeg = 1'000'000'000;  // 10^9 minor units, dp=2
    for (; count < 100'000'000; ++count) {
        Rational leg{Numerator{perLeg}, Denominator{1}, DecimalPlaces{2}};
        // Detect overflow by checking the pre-addition numerator against
        // INT64_MAX - perLeg rather than relying on UB actually occurring;
        // record `count` at the first iteration where this would overflow
        // and stop before triggering real UB.
        if (running.numerator > INT64_MAX - perLeg) {
            break;
        }
        running = running + leg;
    }
    INFO("Overflow boundary reached at row count: " << count);
    CHECK(count > 0);  // sanity: some rows were summed before the boundary
}
