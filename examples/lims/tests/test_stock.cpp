// SPDX-License-Identifier: Apache-2.0
//
// Inventory: items with a stock unit, locations at a site, lots with an expiry, and stock on hand per lot, changed
// only by movements that sum to it. A quantity in another unit of the item's dimension converts exactly.

#include <Lightweight/SqlConnection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/core/errors.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::millilitres;
using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief The rational @p numerator / @p denominator at six places.
/// @param numerator The numerator.
/// @param denominator The denominator.
/// @return It.
[[nodiscard]] Rational six(std::int64_t numerator, std::int64_t denominator) {
    return Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{6}};
}

}  // namespace

// Mutation: make convertStock multiply by the relation's ratio in the wrong direction (250 mL becomes 250000 L).
TEST_CASE("stock: a quantity in millilitres of an item stocked in litres converts exactly", "[lims][inventory]") {
    CHECK(lims::convertStock(six(250, 1), lims::StockUnit::millilitre, lims::StockUnit::litre) == six(1, 4));
    CHECK(lims::convertStock(six(1, 4), lims::StockUnit::litre, lims::StockUnit::millilitre) == six(250, 1));
    CHECK(lims::convertStock(six(3, 1), lims::StockUnit::gram, lims::StockUnit::kilogram) == six(3, 1000));
    CHECK(lims::convertStock(six(7, 1), lims::StockUnit::piece, lims::StockUnit::piece) == six(7, 1));
    CHECK_THROWS_AS(lims::convertStock(six(1, 1), lims::StockUnit::litre, lims::StockUnit::kilogram),
                    lims::ValidationError);
}

// Decision 42's evidence: Quantity converts the same pair exactly when both units are compile-time arguments, and an
// action's field cannot be such a Quantity because the item's unit is known only from its row.
TEST_CASE("stock: Quantity's own conversion agrees when both units are known at compile time", "[lims][inventory]") {
    morph::units::Quantity<lims::StockUnit::millilitre, 3> const received{Rational{250, DecimalPlaces{3}}};
    morph::units::Quantity<lims::StockUnit::litre, 3> const stocked = received;
    REQUIRE(stocked.hasValue());
    CHECK(*stocked == Rational{Numerator{1}, Denominator{4}, DecimalPlaces{3}});
}

// Mutation: skip the unit conversion in RecordOpeningLot (the lot holds 250 L instead of 0.25 L).
TEST_CASE("stock: an opening lot entered in millilitres is held in the item's unit", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    auto const lot = lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), millilitres(250),
                                            lims::StockUnit::millilitre);
    CHECK(lot.unit == lims::StockUnit::litre);
    CHECK(*lot.onHand == six(1, 4));
    ScopedPrincipal const kim{"kim"};
    auto const stock =
        lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}});
    REQUIRE(stock.lots.size() == 1);
    CHECK(stock.lots.front().lotNumber == "L-0042");
    CHECK(*stock.lots.front().reserved == six(0, 1));
}

// Mutation: write the movement row outside the lot's transaction, after the commit (a failure injected after the lot's
// write leaves stock on hand with no movement).
TEST_CASE("stock: a lot's movements always sum to its stock on hand", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    lims::setInventoryFaultHook([](std::string_view point, Lightweight::SqlConnection& /*connection*/) {
        if (point == "lot.afterLotWrite") {
            throw std::runtime_error{"injected"};
        }
    });
    CHECK_THROWS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0001", at(2027, 1, 31), litres(1000),
                                        lims::StockUnit::litre));
    lims::setInventoryFaultHook({});
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    CHECK(stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.empty());
    auto const lot = lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0001", at(2027, 1, 31), litres(1000),
                                            lims::StockUnit::litre);
    auto const moves = stock.execute(lims::ListStockMovements{.item = setUp.reagent, .query = {}});
    REQUIRE(moves.rows.size() == 1);
    CHECK(moves.rows.front().kind == lims::MovementKind::Opening);
    CHECK(*moves.rows.front().quantity == *lot.onHand);
}

TEST_CASE("stock: a lot number is unique per item, and a quantity must be positive", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    CHECK_THROWS_AS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                           lims::StockUnit::litre),
                    lims::Conflict);
    CHECK_THROWS_AS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0043", at(2027, 3, 31), litres(0),
                                           lims::StockUnit::litre),
                    lims::ValidationError);
    CHECK_THROWS_AS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0044", at(2027, 3, 31), litres(500),
                                           lims::StockUnit::kilogram),
                    lims::ValidationError);
}

TEST_CASE("stock: a person without the stock keeper role cannot create an item, and sees no other company's lots",
          "[lims][inventory][authorization]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    lims::StockModel stock;
    CHECK_THROWS_WITH(
        stock.execute(lims::CreateItem{
            .company = lims::test::seededCompany(), .code = "X", .name = "X", .unit = lims::StockUnit::piece}),
        Catch::Matchers::ContainsSubstring("does not hold the stock-keeper role"));
    ScopedPrincipal const stranger{"stranger"};
    CHECK(stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.empty());
}

TEST_CASE("stock: the inventory model across the backend matrix", "[lims][inventory][matrix]") {
    using morph::ladder::testkit::Mode;
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    auto const stock = lims::test::dispatchAs<lims::StockModel>(
        "kim", lims::StockOnHand{.company = lims::test::seededCompany(), .item = setUp.reagent}, mode);
    CHECK(stock.lots.size() == 1);
}

// Mutation: leave the new roles out of org's stored-role range (a stored stock-keeper assignment is refused on read).
TEST_CASE("stock: the three procurement roles are named, stored and read back", "[lims][inventory][authorization]") {
    CHECK(lims::roleName(lims::LimsRole::StockKeeper) == "stock-keeper");
    CHECK(lims::roleName(lims::LimsRole::Purchaser) == "purchaser");
    CHECK(lims::roleName(lims::LimsRole::PurchaseApprover) == "purchase-approver");
    CHECK(static_cast<int>(lims::LimsRole::StockKeeper) == 7);
    DbFixture fixture;
    lims::test::grant("pat", lims::LimsRole::Purchaser);
    lims::test::grant("pia", lims::LimsRole::PurchaseApprover);
    auto const patRoles = lims::OrgModel::rolesIn("pat", lims::Scope{lims::test::seededCompany()});
    REQUIRE(patRoles.size() == 1);
    CHECK(patRoles.front() == lims::LimsRole::Purchaser);
    auto const piaRoles = lims::OrgModel::rolesIn("pia", lims::Scope{lims::test::seededCompany()});
    REQUIRE(piaRoles.size() == 1);
    CHECK(piaRoles.front() == lims::LimsRole::PurchaseApprover);
}
