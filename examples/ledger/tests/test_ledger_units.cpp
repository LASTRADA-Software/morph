// examples/ledger/tests/test_ledger_units.cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <morph/util/rational.hpp>

#include "ledger/core/money.hpp"
#include "ledger/core/units.hpp"

TEST_CASE("USD default decimals is 2", "[ledger][units]") {
    const auto meta = ledger::UnitTraits<ledger::Currency>::meta(ledger::Currency::USD);
    CHECK(meta.defaultDecimals == 2);
}

TEST_CASE("JPY default decimals is 0 -- no floor of 1", "[ledger][units]") {
    const auto meta = ledger::UnitTraits<ledger::Currency>::meta(ledger::Currency::JPY);
    CHECK(meta.defaultDecimals == 0);
}

TEST_CASE("KRW default decimals is 0", "[ledger][units]") {
    const auto meta = ledger::UnitTraits<ledger::Currency>::meta(ledger::Currency::KRW);
    CHECK(meta.defaultDecimals == 0);
}

TEST_CASE("A JPY-denominated Quantity round-trips as a whole number", "[ledger][units]") {
    using JpyQuantity = morph::units::Quantity<ledger::Currency::JPY, 0>;
    auto amount = JpyQuantity{morph::math::Rational{morph::math::Numerator{1500}, morph::math::Denominator{1},
                                                    morph::math::DecimalPlaces{0}}};
    REQUIRE(amount.payload.has_value());
    CHECK(amount.payload->decimalPlaces == morph::math::DecimalPlaces{0});
}

TEST_CASE("currencyDecimalPlaces names each currency's own scale", "[ledger][units][money]") {
    CHECK(ledger::currencyDecimalPlaces(ledger::Currency::USD) == 2);
    CHECK(ledger::currencyDecimalPlaces(ledger::Currency::EUR) == 2);
    CHECK(ledger::currencyDecimalPlaces(ledger::Currency::JPY) == 0);
    CHECK(ledger::currencyDecimalPlaces(ledger::Currency::KRW) == 0);
}

TEST_CASE("restateMinorUnits widens, narrows exactly, and refuses the rest", "[ledger][units][money]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    const auto at = [](std::int64_t numerator, std::uint32_t places) {
        return Rational{Numerator{numerator}, Denominator{1}, DecimalPlaces{places}};
    };

    // Same scale in, same value out -- the common case, and the one every
    // stored row already satisfies.
    auto identity = ledger::restateMinorUnits(at(450, 2), 2);
    REQUIRE(identity.has_value());
    CHECK(identity->numerator == 450);
    CHECK(identity->decimalPlaces == DecimalPlaces{2});

    // Widening: "$4.5" at dp 1 is 450 cents.
    auto widened = ledger::restateMinorUnits(at(45, 1), 2);
    REQUIRE(widened.has_value());
    CHECK(widened->numerator == 450);
    CHECK(widened->denominator == 1);
    CHECK(widened->decimalPlaces == DecimalPlaces{2});

    // Sign survives widening.
    auto negative = ledger::restateMinorUnits(at(-45, 1), 2);
    REQUIRE(negative.has_value());
    CHECK(negative->numerator == -450);

    // Narrowing when nothing is lost: $4.50 written at dp 4 is 450 cents.
    auto narrowed = ledger::restateMinorUnits(at(45000, 4), 2);
    REQUIRE(narrowed.has_value());
    CHECK(narrowed->numerator == 450);
    CHECK(narrowed->decimalPlaces == DecimalPlaces{2});

    // Narrowing to a zero-decimal currency.
    auto toWholeUnits = ledger::restateMinorUnits(at(150000, 2), 0);
    REQUIRE(toWholeUnits.has_value());
    CHECK(toWholeUnits->numerator == 1500);
    CHECK(toWholeUnits->decimalPlaces == DecimalPlaces{0});

    // Zero restates to zero at the target scale, not to a stray precision.
    auto zero = ledger::restateMinorUnits(at(0, 0), 2);
    REQUIRE(zero.has_value());
    CHECK(zero->numerator == 0);
    CHECK(zero->decimalPlaces == DecimalPlaces{2});

    // Refused: narrowing would drop a non-zero digit ($4.505 in USD).
    CHECK_FALSE(ledger::restateMinorUnits(at(4505, 3), 2).has_value());

    // Refused: not a whole number of minor units at all.
    CHECK_FALSE(ledger::restateMinorUnits(Rational{Numerator{9}, Denominator{2}, DecimalPlaces{2}}, 2).has_value());

    // Refused: widening this far overflows int64 rather than saturating
    // silently -- `checkedMul`, not `operator*`.
    CHECK_FALSE(ledger::restateMinorUnits(at(9'000'000'000'000'000'000LL, 0), 2).has_value());
}

TEST_CASE("formatMoney renders exact decimal text without a float", "[ledger][units][money]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    const auto at = [](std::int64_t numerator, std::uint32_t places) {
        return Rational{Numerator{numerator}, Denominator{1}, DecimalPlaces{places}};
    };

    CHECK(ledger::formatMoney(ledger::Currency::USD, at(450, 2)) == "4.5");
    CHECK(ledger::formatMoney(ledger::Currency::USD, at(-4500, 2)) == "-45");
    CHECK(ledger::formatMoney(ledger::Currency::USD, at(451, 2)) == "4.51");
    CHECK(ledger::formatMoney(ledger::Currency::USD, at(0, 2)) == "0");
    CHECK(ledger::formatMoney(ledger::Currency::EUR, at(4523, 2)) == "45.23");

    // Zero-decimal currencies render as whole numbers, with no invented
    // fractional part.
    CHECK(ledger::formatMoney(ledger::Currency::JPY, at(1500, 0)) == "1500");
    CHECK(ledger::formatMoney(ledger::Currency::KRW, at(-1500, 0)) == "-1500");

    // An amount off the currency's own scale is restated first, so the two
    // spellings of ¥1500 render identically.
    CHECK(ledger::formatMoney(ledger::Currency::JPY, at(150000, 2)) == "1500");

    // Past 2^53, where the double division the QML views used to do drifts
    // and this does not: 9007199254740993 cents is 90071992547409.93.
    CHECK(ledger::formatMoney(ledger::Currency::USD, at(9007199254740993LL, 2)) == "90071992547409.93");
}
