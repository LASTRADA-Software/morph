// SPDX-License-Identifier: Apache-2.0
//
// Inventory: items with a stock unit, locations at a site, lots with an expiry, and stock on hand per lot, changed
// only by movements that sum to it. A quantity in another unit of the item's dimension converts exactly.

#include <Lightweight/SqlConnection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <morph/core/action_guard.hpp>
#include <morph/table/query.hpp>
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

/// @brief A second company with its own site, location, item and settings, and `kim` a stock keeper there too.
struct OtherCompany {
    /// @brief The company.
    lims::CompanyId company;
    /// @brief Its site.
    lims::SiteId site;
    /// @brief Its location.
    lims::LocationId store;
    /// @brief Its item.
    lims::ItemId item;
};

/// @brief Makes the second company, as `sam`, who becomes its supervisor.
/// @return Its ids.
[[nodiscard]] OtherCompany otherCompany() {
    OtherCompany other;
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel org;
        other.company = org.execute(lims::CreateCompany{.name = "Osaka branch"}).id;
        other.site = org.execute(lims::CreateSite{.companyId = other.company, .name = "Osaka lab"}).id;
        org.execute(lims::AssignRole{
            .principal = "kim", .role = lims::LimsRole::StockKeeper, .company = other.company, .site = {}});
        lims::StockModel{}.execute(
            lims::SetInventorySettings{.company = other.company,
                                       .stockAccount = "3100",
                                       .grniAccount = "1600",
                                       .consumptionAccount = "5100",
                                       .roundingAccount = "6900",
                                       .approvalHours = lims::Count{Rational{24, DecimalPlaces{0}}}});
    }
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    other.item =
        stock
            .execute(lims::CreateItem{
                .company = other.company, .code = "OSA-1", .name = "Osaka item", .unit = lims::StockUnit::gram})
            .id;
    other.store = stock.execute(lims::CreateLocation{.site = other.site, .name = "Osaka store"}).id;
    return other;
}

/// @brief The settings action for the seeded company with @p hours approval hours.
/// @param hours The hours, as a rational.
/// @return The action.
[[nodiscard]] lims::SetInventorySettings settingsWith(Rational hours) {
    return lims::SetInventorySettings{.company = lims::test::seededCompany(),
                                      .stockAccount = "3100",
                                      .grniAccount = "1600",
                                      .consumptionAccount = "5100",
                                      .roundingAccount = "6900",
                                      .approvalHours = lims::Count{std::move(hours)}};
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

// Mutation: swap the `from` and `to` of the litre/millilitre relation in UnitTraits<StockUnit>::relations (0.25 L
// becomes 250000 mL in the compile-time conversion).
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

TEST_CASE("stock: a person without the stock keeper role cannot create an item, and one with no role sees no lots",
          "[lims][inventory][authorization]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    lims::StockModel stock;
    CHECK_THROWS_WITH(::morph::model::executeChecked(stock, lims::CreateItem{.company = lims::test::seededCompany(),
                                                                             .code = "X",
                                                                             .name = "X",
                                                                             .unit = lims::StockUnit::piece}),
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

// Mutation: drop the range check on the approval hours in SetInventorySettings (0 and 8761 hours are stored).
TEST_CASE("stock: settings are read back, replaced by a second set and refused out of range", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(setUp);
    ScopedPrincipal const sam{"sam"};
    lims::StockModel stock;
    auto const first = stock.execute(lims::GetInventorySettings{.company = lims::test::seededCompany()});
    CHECK(first.stockAccount == "3100");
    CHECK(*first.approvalHours == Rational{48, DecimalPlaces{0}});

    auto replacement = settingsWith(Rational{72, DecimalPlaces{0}});
    replacement.stockAccount = "3200";
    stock.execute(replacement);
    auto const second = stock.execute(lims::GetInventorySettings{.company = lims::test::seededCompany()});
    CHECK(second.stockAccount == "3200");
    CHECK(*second.approvalHours == Rational{72, DecimalPlaces{0}});

    CHECK_THROWS_AS(stock.execute(settingsWith(Rational{0, DecimalPlaces{0}})), lims::ValidationError);
    CHECK_THROWS_AS(stock.execute(settingsWith(Rational{8761, DecimalPlaces{0}})), lims::ValidationError);
    CHECK_THROWS_AS(stock.execute(settingsWith(Rational{Numerator{15}, Denominator{10}, DecimalPlaces{1}})),
                    lims::ValidationError);
    CHECK(*stock.execute(settingsWith(Rational{8760, DecimalPlaces{0}})).approvalHours ==
          Rational{8760, DecimalPlaces{0}});

    auto unknown = settingsWith(Rational{48, DecimalPlaces{0}});
    unknown.company = lims::CompanyId{999};
    CHECK_THROWS_AS(stock.execute(unknown), lims::NotFound);
    auto lowercase = settingsWith(Rational{48, DecimalPlaces{0}});
    lowercase.grniAccount = "grni";
    CHECK_THROWS_AS(stock.execute(lowercase), lims::ValidationError);
}

// Mutation: let GetInventorySettings answer a company the caller cannot see (ann reads Osaka's settings).
TEST_CASE("stock: settings of a company with none, or of one the caller cannot see, are not found",
          "[lims][inventory][authorization]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(setUp);
    auto const other = otherCompany();
    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    lims::StockModel stock;
    CHECK_THROWS_AS(stock.execute(lims::GetInventorySettings{.company = other.company}), lims::NotFound);
    CHECK_THROWS_AS(stock.execute(lims::GetInventorySettings{.company = lims::CompanyId{999}}), lims::NotFound);
    CHECK(stock.execute(lims::GetInventorySettings{.company = lims::test::seededCompany()}).stockAccount == "3100");
    ScopedPrincipal const kim{"kim"};
    CHECK(stock.execute(lims::GetInventorySettings{.company = other.company}).stockAccount == "3100");
}

// Mutation: drop the (company, code) lookup in CreateItem (the duplicate is stored, or surfaces as a raw SQL error).
TEST_CASE("stock: an item code is unique in its company, and items are listed by code per visible company",
          "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(setUp);
    auto const other = otherCompany();
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    CHECK_THROWS_AS(::morph::model::executeChecked(stock, lims::CreateItem{.company = lims::test::seededCompany(),
                                                                           .code = "NO3-R",
                                                                           .name = "Again",
                                                                           .unit = lims::StockUnit::piece}),
                    lims::Conflict);
    CHECK_THROWS_AS(::morph::model::executeChecked(stock, lims::CreateItem{.company = lims::CompanyId{999},
                                                                           .code = "X",
                                                                           .name = "X",
                                                                           .unit = lims::StockUnit::piece}),
                    lims::Forbidden);
    CHECK_THROWS_AS(
        stock.execute(lims::CreateItem{
            .company = lims::test::seededCompany(), .code = "", .name = "X", .unit = lims::StockUnit::piece}),
        lims::ValidationError);
    static_cast<void>(stock.execute(lims::CreateItem{
        .company = lims::test::seededCompany(), .code = "ACID", .name = "Acid", .unit = lims::StockUnit::kilogram}));
    auto const listed = stock.execute(lims::ListItems{.company = lims::test::seededCompany()});
    REQUIRE(listed.items.size() == 2);
    CHECK(listed.items.front().code == "ACID");
    CHECK(listed.items.back().code == "NO3-R");
    auto const osaka = stock.execute(lims::ListItems{.company = other.company});
    REQUIRE(osaka.items.size() == 1);
    CHECK(osaka.items.front().code == "OSA-1");

    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    CHECK(stock.execute(lims::ListItems{.company = other.company}).items.empty());
    CHECK(stock.execute(lims::ListItems{.company = lims::test::seededCompany()}).items.size() == 2);
}

// Mutation: drop the (site, name) lookup in CreateLocation, or the visibility test in ListLocations.
TEST_CASE("stock: a location name is unique at its site, and locations are listed per visible company",
          "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    auto const other = otherCompany();
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        stock, lims::CreateLocation{.site = lims::test::seededSite(), .name = "Cold room"}),
                    lims::Conflict);
    CHECK_THROWS_AS(
        ::morph::model::executeChecked(stock, lims::CreateLocation{.site = lims::SiteId{999}, .name = "Nowhere"}),
        lims::Forbidden);
    CHECK_THROWS_AS(stock.execute(lims::CreateLocation{.site = lims::test::seededSite(), .name = ""}),
                    lims::ValidationError);
    static_cast<void>(stock.execute(lims::CreateLocation{.site = lims::test::seededSite(), .name = "Annex"}));
    auto const listed = stock.execute(lims::ListLocations{.company = lims::test::seededCompany()});
    REQUIRE(listed.locations.size() == 2);
    CHECK(listed.locations.front().name == "Annex");
    CHECK(listed.locations.back().id == setUp.coldRoom);
    auto const osaka = stock.execute(lims::ListLocations{.company = other.company});
    REQUIRE(osaka.locations.size() == 1);
    CHECK(osaka.locations.front().id == other.store);

    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    CHECK(stock.execute(lims::ListLocations{.company = other.company}).locations.empty());
}

// Mutation: drop the same-company test on the location in RecordOpeningLot (a lot of a seeded item sits in Osaka's
// store), or the sign test on the unit cost.
TEST_CASE("stock: an opening lot is refused for an unknown item, another company's location or a bad cost",
          "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    auto const other = otherCompany();
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    auto const lotWith = [&](lims::ItemId item, lims::LocationId location, Rational cost) {
        return lims::RecordOpeningLot{.item = item,
                                      .location = location,
                                      .lotNumber = "L-1",
                                      .expiresAt = morph::time::Timestamp{at(2027, 3, 31)},
                                      .quantity = litres(1000),
                                      .unit = lims::StockUnit::litre,
                                      .unitCost = lims::Amount{std::move(cost)}};
    };
    Rational const fine{Numerator{1250}, Denominator{100}, DecimalPlaces{2}};
    CHECK_THROWS_AS(stock.execute(lotWith(lims::ItemId{999}, setUp.coldRoom, fine)), lims::NotFound);
    CHECK_THROWS_AS(stock.execute(lotWith(setUp.reagent, lims::LocationId{999}, fine)), lims::NotFound);
    CHECK_THROWS_AS(stock.execute(lotWith(setUp.reagent, other.store, fine)), lims::NotFound);
    CHECK_THROWS_WITH(stock.execute(lotWith(setUp.reagent, setUp.coldRoom,
                                            Rational{Numerator{-1250}, Denominator{100}, DecimalPlaces{2}})),
                      Catch::Matchers::ContainsSubstring("must not be negative"));
    CHECK_THROWS_WITH(stock.execute(lotWith(setUp.reagent, setUp.coldRoom,
                                            Rational{Numerator{12505}, Denominator{1000}, DecimalPlaces{3}})),
                      Catch::Matchers::ContainsSubstring("minor units"));
    CHECK(stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.empty());
    CHECK_NOTHROW(stock.execute(lotWith(setUp.reagent, setUp.coldRoom, fine)));
}

// Mutation: sort StockOnHand by id only, or ignore the item filter.
TEST_CASE("stock: stock on hand is ordered by expiry then id, filtered by item, and hidden from other companies",
          "[lims][inventory][authorization]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    auto const other = otherCompany();
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    auto const acid = stock
                          .execute(lims::CreateItem{.company = lims::test::seededCompany(),
                                                    .code = "ACID",
                                                    .name = "Acid",
                                                    .unit = lims::StockUnit::litre})
                          .id;
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-LATE", at(2027, 6, 30), litres(1000),
                                             lims::StockUnit::litre));
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-SOON", at(2027, 5, 31), litres(1000),
                                             lims::StockUnit::litre));
    static_cast<void>(
        lims::test::openingLot(acid, setUp.coldRoom, "A-SOON", at(2027, 5, 31), litres(1000), lims::StockUnit::litre));
    auto const all = stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}});
    REQUIRE(all.lots.size() == 3);
    CHECK(all.lots[0].lotNumber == "L-SOON");
    CHECK(all.lots[1].lotNumber == "A-SOON");
    CHECK(all.lots[2].lotNumber == "L-LATE");
    auto const reagentOnly =
        stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = setUp.reagent});
    REQUIRE(reagentOnly.lots.size() == 2);
    CHECK(reagentOnly.lots.front().lotNumber == "L-SOON");
    CHECK(reagentOnly.lots.back().lotNumber == "L-LATE");
    CHECK(stock.execute(lims::StockOnHand{.company = other.company, .item = {}}).lots.empty());

    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    CHECK(stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.size() == 3);
    CHECK(stock.execute(lims::StockOnHand{.company = other.company, .item = {}}).lots.empty());
}

// Mutation: drop the visibility test in ListStockMovements (ann pages the movements of Osaka's lot).
TEST_CASE("stock: movements are paged for a visible item only, and a refused query is a validation error",
          "[lims][inventory][authorization]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    auto const other = otherCompany();
    {
        ScopedPrincipal const kim{"kim"};
        static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                                 lims::StockUnit::litre));
        static_cast<void>(lims::StockModel{}.execute(
            lims::RecordOpeningLot{.item = other.item,
                                   .location = other.store,
                                   .lotNumber = "O-1",
                                   .expiresAt = morph::time::Timestamp{at(2027, 3, 31)},
                                   .quantity = millilitres(5),
                                   .unit = lims::StockUnit::gram,
                                   .unitCost = lims::Amount{Rational{1, DecimalPlaces{2}}}}));
    }
    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    lims::StockModel stock;
    CHECK(stock.execute(lims::ListStockMovements{.item = setUp.reagent, .query = {}}).rows.size() == 1);
    CHECK(stock.execute(lims::ListStockMovements{.item = other.item, .query = {}}).rows.empty());
    CHECK(stock.execute(lims::ListStockMovements{.item = lims::ItemId{999}, .query = {}}).rows.empty());
    morph::table::TableQuery refused;
    refused.page.offset = -1;
    CHECK_THROWS_AS(stock.execute(lims::ListStockMovements{.item = setUp.reagent, .query = refused}),
                    lims::ValidationError);
}
