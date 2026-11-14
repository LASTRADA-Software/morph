// SPDX-License-Identifier: Apache-2.0
//
// Money: one money unit whose currency is a field beside it, and the half-up rounding every poster uses.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <morph/util/rational.hpp>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/money.hpp"

namespace {

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

/// @brief @p numerator / @p denominator at @p places.
/// @param numerator The numerator.
/// @param denominator The denominator.
/// @param places The decimal places.
/// @return The value.
[[nodiscard]] Rational ratio(std::int64_t numerator, std::int64_t denominator, std::uint32_t places) {
    return Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{places}};
}

}  // namespace

TEST_CASE("money: a currency's decimals, JPY's being none", "[lims][money]") {
    CHECK(lims::decimalsOf(lims::CurrencyCode::EUR) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::USD) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::GBP) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::CHF) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::JPY) == 0U);
    CHECK(lims::currencyName(lims::CurrencyCode::JPY) == "JPY");
}

// Mutation: round half-even in roundToCurrency (1.005 EUR becomes 1.00, 100.5 JPY becomes 100).
TEST_CASE("money: a poster rounds half-up to the currency's decimals", "[lims][money]") {
    CHECK(lims::roundToCurrency(ratio(1005, 1000, 3), lims::CurrencyCode::EUR) == ratio(101, 100, 2));
    CHECK(lims::roundToCurrency(ratio(1004, 1000, 3), lims::CurrencyCode::EUR) == ratio(100, 100, 2));
    CHECK(lims::roundToCurrency(ratio(1005, 10, 1), lims::CurrencyCode::JPY) == ratio(101, 1, 0));
    CHECK(lims::roundToCurrency(ratio(1004, 10, 1), lims::CurrencyCode::JPY) == ratio(100, 1, 0));
    // Equality ignores the decimal-places tag, so the tag is asserted on its own.
    CHECK(lims::roundToCurrency(ratio(1005, 10, 1), lims::CurrencyCode::JPY).getDecimalPlaces() == DecimalPlaces{0});
    CHECK(lims::roundToCurrency(ratio(1005, 1000, 3), lims::CurrencyCode::EUR).getDecimalPlaces() == DecimalPlaces{2});
    // Away from zero below it: a negative half rounds to the larger magnitude.
    CHECK(lims::roundToCurrency(ratio(-1005, 1000, 3), lims::CurrencyCode::EUR) == ratio(-101, 100, 2));
    // A value that already fits is unchanged.
    CHECK(lims::roundToCurrency(ratio(1234, 100, 2), lims::CurrencyCode::EUR) == ratio(1234, 100, 2));
}

TEST_CASE("money: a value fits a currency when it is a whole number of its minor units", "[lims][money]") {
    CHECK(lims::fitsCurrency(ratio(12, 1, 0), lims::CurrencyCode::JPY));
    CHECK_FALSE(lims::fitsCurrency(ratio(25, 2, 1), lims::CurrencyCode::JPY));
    CHECK(lims::fitsCurrency(ratio(1234, 100, 2), lims::CurrencyCode::EUR));
    CHECK_FALSE(lims::fitsCurrency(ratio(12345, 1000, 3), lims::CurrencyCode::EUR));
    CHECK_FALSE(lims::fitsCurrency(ratio(1, 3, 2), lims::CurrencyCode::EUR));
}

// Mutation: let ExactSum::add use operator+ (the over-range add saturates and reports success).
TEST_CASE("money: an exact sum refuses a total it cannot hold, and keeps the last one it could", "[lims][money]") {
    lims::ExactSum sum;
    CHECK(sum.add(ratio(150, 100, 2)));
    CHECK(sum.add(ratio(250, 100, 2)));
    CHECK(sum.total() == ratio(400, 100, 2));

    lims::ExactSum huge;
    auto const near = std::numeric_limits<std::int64_t>::max() / 2 + 1;
    CHECK(huge.add(ratio(near, 1, 0)));
    CHECK_FALSE(huge.add(ratio(near, 1, 0)));
    CHECK(huge.total() == ratio(near, 1, 0));
}

TEST_CASE("money: rounding a value whose rounded numerator does not fit throws OutOfRange", "[lims][money]") {
    auto const tooBig = ratio(std::numeric_limits<std::int64_t>::max(), 3, 0);
    CHECK_THROWS_AS(lims::roundToCurrency(tooBig, lims::CurrencyCode::EUR), lims::OutOfRange);
}

TEST_CASE("money: the money units name themselves for the schema", "[lims][money]") {
    CHECK(lims::Amount::unitMeta().id == "money");
    CHECK(lims::Amount::declaredDecimals == 2U);
    CHECK(lims::Rate::unitMeta().id == "rate");
    CHECK(lims::Rate::declaredDecimals == 6U);
    CHECK(lims::Count::unitMeta().id == "count");
    CHECK(lims::Count::declaredDecimals == 0U);
}
