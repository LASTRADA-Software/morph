// examples/ledger/tests/test_ledger_units.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/units.hpp"

#include <morph/util/rational.hpp>

#include <catch2/catch_test_macros.hpp>

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
