// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 8, the orders half of its second instance: a purchase order is in its supplier's currency, which only the
// supplier's row names; whole yen are kept by the model, and the served form still advertises the type's two places.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <morph/core/action_guard.hpp>
#include <morph/core/effects.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief Clears the inventory fault hook when a case ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setInventoryFaultHook({}); }
};

/// @brief A unit price of @p cents hundredths.
/// @param cents The price in hundredths.
/// @return The amount, at two places.
lims::Amount price(std::int64_t cents) {
    return lims::Amount{Rational{Numerator{cents}, Denominator{100}, DecimalPlaces{2}}};
}

/// @brief Runs @p sql on a connection of its own, committed.
/// @param sql The statement.
void runOnOther(std::string const& sql) {
    Lightweight::SqlConnection other;
    Lightweight::SqlStatement statement{other};
    static_cast<void>(statement.ExecuteDirect(sql));
}

/// @brief The second company, with `omar` a purchaser there and able to see nothing of the seeded one.
/// @return The second company.
lims::CompanyId osakaWithPurchaser() {
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Rheinlab Japan", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    org.execute(
        lims::AssignRole{.principal = "omar", .role = lims::LimsRole::Purchaser, .company = company.id, .site = {}});
    return company.id;
}

}  // namespace

// Mutation: multiply with Rational's operator* in orderLineTotal instead of checkedMul (the over-range case answers a
// saturated total instead of OutOfRange).
TEST_CASE("purchase orders: a line total is quantity times unit price, rounded half-up once",
          "[lims][inventory][order]") {
    Rational const twoAndAThird{Numerator{7}, Denominator{3}, DecimalPlaces{6}};
    Rational const unitPrice{Numerator{1001}, Denominator{100}, DecimalPlaces{2}};
    // 7/3 x 10.01 = 23.356666... -> 23.36 EUR, -> 23 JPY at a yen price of 10.
    CHECK(lims::orderLineTotal(twoAndAThird, unitPrice, lims::CurrencyCode::EUR) ==
          Rational{Numerator{2336}, Denominator{100}, DecimalPlaces{2}});
    CHECK(lims::orderLineTotal(twoAndAThird, Rational{10, DecimalPlaces{0}}, lims::CurrencyCode::JPY) ==
          Rational{23, DecimalPlaces{0}});
    Rational const huge{4'000'000'000'000'000'000, DecimalPlaces{0}};
    CHECK_THROWS_AS(lims::orderLineTotal(huge, Rational{3, DecimalPlaces{0}}, lims::CurrencyCode::EUR),
                    lims::OutOfRange);
}

// Mutation: drop the conditional update of the requisition's state from CreatePurchaseOrder (a second order of the
// same requisition succeeds).
TEST_CASE("purchase orders: an approved requisition is ordered once, in the supplier's currency",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const order = lims::test::openOrder(lims::CurrencyCode::CHF).order;
    CHECK(order.currency == lims::CurrencyCode::CHF);
    CHECK(order.state == lims::OrderState::Open);
    CHECK(*order.total == Rational{Numerator{17000}, Denominator{100}, DecimalPlaces{2}});
    CHECK(order.supplierName == "Merck");
    CHECK(order.orderedBy == "paul");
    CHECK(order.site == lims::test::seededSite());
    REQUIRE(order.lines.size() == 1U);
    CHECK(order.lines.front().itemCode == "NO3-R");
    CHECK(order.lines.front().unit == lims::StockUnit::litre);
    CHECK(*order.lines.front().ordered == Rational{Numerator{2}, Denominator{1}, DecimalPlaces{6}});
    CHECK(*order.lines.front().received == Rational{Numerator{0}, Denominator{1}, DecimalPlaces{6}});
    CHECK(*order.lines.front().unitPrice == Rational{Numerator{8500}, Denominator{100}, DecimalPlaces{2}});
    CHECK(*order.lines.front().lineTotal == *order.total);
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const requisition = procurement.execute(lims::GetRequisition{.requisitionId = order.requisition});
    CHECK(requisition.state == lims::RequisitionState::Ordered);
    CHECK(requisition.order == order.id);
    CHECK_THROWS_AS(procurement.execute(lims::CreatePurchaseOrder{
                        .requisition = order.requisition, .supplier = order.supplier, .prices = {}}),
                    lims::IllegalTransition);
    CHECK(procurement.execute(lims::ListPurchaseOrders{.company = lims::test::seededCompany()}).orders.size() == 1U);
}

// Hypothesis 8, orders half. Mutation: remove the fitsCurrency check on a unit price (a fraction of a yen is stored).
TEST_CASE("Hypothesis 8: a yen order keeps whole yen by the model, not by the type", "[lims][inventory][order][h8]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Kanto", .currency = lims::CurrencyCode::JPY});
    CHECK_THROWS_WITH(
        procurement.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = supplier.id,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(85050)}}}),
        Catch::Matchers::ContainsSubstring("a unit price of 850.5 is not a whole number of JPY"));
    // A price past six significant digits is printed exactly, with no exponent.
    CHECK_THROWS_WITH(
        procurement.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = supplier.id,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(123456750)}}}),
        Catch::Matchers::ContainsSubstring("a unit price of 1234567.5 is not"));
    // The type still says two places: the served form of a yen order line advertises them.
    auto const& schema = morph::forms::schemaJson<lims::CreatePurchaseOrder>();
    CHECK_THAT(schema, Catch::Matchers::ContainsSubstring(R"("x-decimalPlaces":2)"));
    // Nothing was written by the refusal: the requisition is still approved.
    CHECK(procurement.execute(lims::GetRequisition{.requisitionId = requisition.id}).state ==
          lims::RequisitionState::Approved);
}

// Mutation: remove the positivity check on a unit price (a zero price is stored); drop the duplicate check (a line
// priced twice takes the first price); drop the missing-line check (an order of an unpriced line is stored).
TEST_CASE("purchase orders: every requisition line is priced once, with a positive price",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    auto const line = requisition.lines.front().id;
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::EUR});
    auto const order = [&](std::vector<lims::OrderLinePrice> prices) {
        return procurement.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id, .supplier = supplier.id, .prices = std::move(prices)});
    };
    CHECK_THROWS_WITH(order({}), Catch::Matchers::ContainsSubstring("has no unit price"));
    CHECK_THROWS_WITH(order({{line, price(8500)}, {line, price(9000)}}),
                      Catch::Matchers::ContainsSubstring("is priced twice"));
    CHECK_THROWS_WITH(order({{lims::RequisitionLineId{9999}, price(8500)}}),
                      Catch::Matchers::ContainsSubstring("is not a line of requisition"));
    CHECK_THROWS_WITH(order({{line, price(0)}}), Catch::Matchers::ContainsSubstring("must be positive"));
    CHECK_THROWS_WITH(order({{line, price(-100)}}), Catch::Matchers::ContainsSubstring("must be positive"));
    CHECK_THROWS_AS(order({{line, lims::Amount{}}}), lims::ValidationError);
    CHECK(procurement.execute(lims::ListPurchaseOrders{.company = lims::test::seededCompany()}).orders.empty());
    CHECK(order({{line, price(8500)}}).state == lims::OrderState::Open);
}

// Mutation: multiply the quantity by the price without the checked product (the one-line case saturates).
TEST_CASE("purchase orders: a total that does not fit is refused, and nothing is written",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::JPY});
    // 2 L at 5e18 yen a litre is 1e19 yen, past the int64 range.
    auto const huge = lims::Amount{Rational{5'000'000'000'000'000'000, DecimalPlaces{0}}};
    CHECK_THROWS_AS(procurement.execute(lims::CreatePurchaseOrder{
                        .requisition = requisition.id,
                        .supplier = supplier.id,
                        .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = huge}}}),
                    lims::OutOfRange);
    CHECK(procurement.execute(lims::GetRequisition{.requisitionId = requisition.id}).state ==
          lims::RequisitionState::Approved);
}

// Mutation: ignore the result of ExactSum::add in CreatePurchaseOrder (two lines of 9.2e18 yen each store a wrong
// total, a wrapped one).
TEST_CASE("purchase orders: lines whose sum does not fit are refused although each fits", "[lims][inventory][order]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    lims::ProcurementModel procurement;
    lims::RequisitionView requisition;
    {
        ScopedPrincipal const rita{"rita"};
        static_cast<void>(procurement.execute(lims::AddRequisitionLine{.requisition = setUp.draft.id,
                                                                       .item = setUp.stock.reagent,
                                                                       .quantity = lims::test::litres(1000),
                                                                       .unit = lims::StockUnit::litre,
                                                                       .estimate = price(100)}));
        static_cast<void>(procurement.execute(lims::SubmitRequisition{.requisitionId = setUp.draft.id}));
    }
    lims::test::grant("paul", lims::LimsRole::Purchaser);
    {
        morph::ladder::ScopedClockOverride const clock{lims::test::at(2026, 11, 10, 10)};
        ScopedPrincipal const anna{"anna"};
        requisition = procurement.execute(lims::ApproveRequisition{.requisitionId = setUp.draft.id});
    }
    REQUIRE(requisition.lines.size() == 2U);
    ScopedPrincipal const paul{"paul"};
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Kanto", .currency = lims::CurrencyCode::JPY});
    // 2 L at 4.6e18 yen is 9.2e18, and 1 L at 4.6e18 is 4.6e18: each fits an int64, their sum does not.
    auto const huge = lims::Amount{Rational{4'600'000'000'000'000'000, DecimalPlaces{0}}};
    CHECK_THROWS_WITH(procurement.execute(lims::CreatePurchaseOrder{
                          .requisition = requisition.id,
                          .supplier = supplier.id,
                          .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = huge},
                                     lims::OrderLinePrice{.line = requisition.lines.back().id, .unitPrice = huge}}}),
                      Catch::Matchers::ContainsSubstring("the order's total does not fit"));
    CHECK(procurement.execute(lims::GetRequisition{.requisitionId = requisition.id}).state ==
          lims::RequisitionState::Approved);
}

// Mutation: drop the Approved check from CreatePurchaseOrder (a draft requisition is ordered).
TEST_CASE("purchase orders: only an approved requisition is ordered", "[lims][inventory][order]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    lims::test::grant("paul", lims::LimsRole::Purchaser);
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::EUR});
    CHECK_THROWS_WITH(
        procurement.execute(lims::CreatePurchaseOrder{
            .requisition = setUp.draft.id,
            .supplier = supplier.id,
            .prices = {lims::OrderLinePrice{.line = setUp.draft.lines.front().id, .unitPrice = price(8500)}}}),
        Catch::Matchers::ContainsSubstring("only an approved requisition is ordered"));
}

// Mutation: drop the conditional update's state from the requisition move (the loser of a race orders a requisition
// that another order already took).
TEST_CASE("purchase orders: an order that lost the race for its requisition is a Conflict",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::EUR});
    FaultHookGuard const hookGuard;
    lims::setInventoryFaultHook([id = *requisition.id](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "requisition.beforeStateChange") {
            runOnOther("UPDATE inventory_requisitions SET state = 3 WHERE id = " + std::to_string(id));
        }
    });
    CHECK_THROWS_AS(
        procurement.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = supplier.id,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(8500)}}}),
        lims::Conflict);
    CHECK(procurement.execute(lims::ListPurchaseOrders{.company = lims::test::seededCompany()}).orders.empty());
}

// Mutation: drop the unique index on the requisition from the migration (the second order is stored beside the first);
// turn the catch's re-read into a plain rethrow (the loser sees the database's error, not a Conflict).
TEST_CASE("purchase orders: the index settles two orders that raced past the state read", "[lims][inventory][order]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::EUR});
    auto const place = [&] {
        return procurement.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = supplier.id,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(8500)}}});
    };
    FaultHookGuard const hookGuard;
    auto const companyId = std::to_string(*lims::test::seededCompany());
    auto const siteId = std::to_string(*lims::test::seededSite());
    // A rival order row for the requisition lands before this order's insert, and the rival's requisition update
    // commits once this transaction has rolled back.
    lims::setInventoryFaultHook(
        [id = *requisition.id, &supplier, &companyId, &siteId](std::string_view point, Lightweight::SqlConnection&) {
            if (point == "requisition.beforeStateChange") {
                runOnOther(
                    "INSERT INTO inventory_orders (company_id, site_id, supplier_id, requisition_id, currency, state, "
                    "total_num, total_den, ordered_by, ordered_at) VALUES (" +
                    companyId + ", " + siteId + ", " + std::to_string(*supplier.id) + ", " + std::to_string(id) +
                    ", 0, 0, 100, 1, 'rival', 0)");
            } else if (point == "order.afterRollback") {
                runOnOther("UPDATE inventory_requisitions SET state = 3 WHERE id = " + std::to_string(id));
            }
        });
    CHECK_THROWS_AS(place(), lims::Conflict);
    // A refusal that is not that race surfaces as itself: the same failure with the requisition still approved.
    runOnOther("UPDATE inventory_requisitions SET state = 2 WHERE id = " + std::to_string(*requisition.id));
    lims::setInventoryFaultHook(
        [id = *requisition.id, &supplier, &companyId, &siteId](std::string_view point, Lightweight::SqlConnection&) {
            if (point == "requisition.beforeStateChange") {
                runOnOther("DELETE FROM inventory_orders WHERE requisition_id = " + std::to_string(id));
                runOnOther(
                    "INSERT INTO inventory_orders (company_id, site_id, supplier_id, requisition_id, currency, state, "
                    "total_num, total_den, ordered_by, ordered_at) VALUES (" +
                    companyId + ", " + siteId + ", " + std::to_string(*supplier.id) + ", " + std::to_string(id) +
                    ", 0, 0, 100, 1, 'rival', 0)");
            }
        });
    CHECK_THROWS_AS(place(), Lightweight::SqlException);
    CHECK(procurement.execute(lims::GetRequisition{.requisitionId = requisition.id}).state ==
          lims::RequisitionState::Approved);
}

// Mutation: drop the unique index on (company, name) from the migration (the Racer insert is stored beside the
// supplier the hook made); turn the catch's re-read into a plain rethrow (the loser sees the database's error, not a
// Conflict). The lookup before the insert is a fast path whose absence the catch hides: the duplicate Merck is
// answered with a Conflict either way.
TEST_CASE("suppliers: a name is taken once per company, and listed by id", "[lims][inventory][order]") {
    DbFixture fixture;
    lims::test::stockSetUp();
    lims::test::grant("paul", lims::LimsRole::Purchaser);
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const first = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::EUR});
    auto const second = procurement.execute(lims::CreateSupplier{
        .company = lims::test::seededCompany(), .name = "Kanto", .currency = lims::CurrencyCode::JPY});
    CHECK(first.currency == lims::CurrencyCode::EUR);
    CHECK(second.currency == lims::CurrencyCode::JPY);
    CHECK_THROWS_AS(procurement.execute(lims::CreateSupplier{
                        .company = lims::test::seededCompany(), .name = "Merck", .currency = lims::CurrencyCode::USD}),
                    lims::Conflict);
    CHECK_THROWS_AS(procurement.execute(lims::CreateSupplier{.company = lims::test::seededCompany(), .name = ""}),
                    lims::ValidationError);
    CHECK_THROWS_AS(procurement.execute(lims::CreateSupplier{.company = lims::CompanyId{9999}, .name = "Nobody"}),
                    lims::NotFound);
    auto const listed = procurement.execute(lims::ListSuppliers{.company = lims::test::seededCompany()}).suppliers;
    REQUIRE(listed.size() == 2U);
    CHECK(listed.front().id == first.id);
    CHECK(listed.back().name == "Kanto");
    CHECK_THROWS_AS(procurement.execute(lims::ListSuppliers{}), lims::ValidationError);
    // The index refuses the second of two creates that raced past the lookup.
    FaultHookGuard const hookGuard;
    lims::setInventoryFaultHook(
        [company = std::to_string(*lims::test::seededCompany())](std::string_view point, Lightweight::SqlConnection&) {
            if (point == "supplier.beforeInsert") {
                runOnOther("INSERT INTO inventory_suppliers (company_id, name, currency) VALUES (" + company +
                           ", 'Racer', 0)");
            }
        });
    CHECK_THROWS_AS(procurement.execute(lims::CreateSupplier{.company = lims::test::seededCompany(), .name = "Racer"}),
                    lims::Conflict);
}

// Mutation: let a supplier of another company through (the supplier-company comparison).
TEST_CASE("purchase orders: the supplier is the requisition's company's, and another company's reads as absent",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    auto const osaka = osakaWithPurchaser();
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "paul", .role = lims::LimsRole::Purchaser, .company = osaka, .site = {}});
    }
    lims::SupplierView osakaSupplier;
    {
        ScopedPrincipal const omar{"omar"};
        osakaSupplier = lims::ProcurementModel{}.execute(
            lims::CreateSupplier{.company = osaka, .name = "Kanto", .currency = lims::CurrencyCode::JPY});
    }
    auto const order = [&](lims::SupplierId supplier) {
        return lims::ProcurementModel{}.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = supplier,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(8500)}}});
    };
    {
        ScopedPrincipal const paul{"paul"};
        CHECK_THROWS_WITH(order(osakaSupplier.id), Catch::Matchers::ContainsSubstring("is not one of requisition"));
        CHECK_THROWS_AS(order(lims::SupplierId{9999}), lims::NotFound);
    }
    ScopedPrincipal const omar{"omar"};
    // omar sees nothing of the seeded company: its requisition and its orders read as absent.
    CHECK_THROWS_AS(order(osakaSupplier.id), lims::NotFound);
    CHECK(lims::ProcurementModel{}
              .execute(lims::ListPurchaseOrders{.company = lims::test::seededCompany()})
              .orders.empty());
    CHECK(lims::ProcurementModel{}
              .execute(lims::ListSuppliers{.company = lims::test::seededCompany()})
              .suppliers.empty());
}

// Mutation: remove the visibility test on the supplier (paul, who sees only the seeded company, finds omar's Osaka
// supplier and is told it is another company's instead of that it does not exist).
TEST_CASE("purchase orders: a supplier of a company the caller cannot see reads as absent",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    auto const osaka = osakaWithPurchaser();
    lims::SupplierView osakaSupplier;
    {
        ScopedPrincipal const omar{"omar"};
        osakaSupplier = lims::ProcurementModel{}.execute(
            lims::CreateSupplier{.company = osaka, .name = "Kanto", .currency = lims::CurrencyCode::JPY});
    }
    ScopedPrincipal const paul{"paul"};
    CHECK_THROWS_WITH(
        lims::ProcurementModel{}.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = osakaSupplier.id,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(8500)}}}),
        Catch::Matchers::ContainsSubstring("does not exist"));
}

// Mutation: make GetPurchaseOrder skip the visibility test (omar reads the seeded company's order).
TEST_CASE("purchase orders: an order is read by those who see its company", "[lims][inventory][order]") {
    DbFixture fixture;
    auto const placed = lims::test::openOrder().order;
    osakaWithPurchaser();
    {
        ScopedPrincipal const paul{"paul"};
        lims::ProcurementModel procurement;
        CHECK(procurement.execute(lims::GetPurchaseOrder{.orderId = placed.id}).id == placed.id);
        CHECK_THROWS_AS(procurement.execute(lims::GetPurchaseOrder{}), lims::ValidationError);
        CHECK_THROWS_AS(procurement.execute(lims::GetPurchaseOrder{.orderId = lims::PurchaseOrderId{9999}}),
                        lims::NotFound);
        CHECK_THROWS_AS(procurement.execute(lims::ListPurchaseOrders{}), lims::ValidationError);
    }
    ScopedPrincipal const omar{"omar"};
    CHECK_THROWS_AS(lims::ProcurementModel{}.execute(lims::GetPurchaseOrder{.orderId = placed.id}), lims::NotFound);
}

// Mutation: declare Authorization<CreatePurchaseOrder> with the Supervisor role (paul, a purchaser, is refused); make
// its scopeOf answer an empty company (paul is refused); drop `guard` from CreateSupplier (rita, an analyst, creates
// one through the dispatcher path).
TEST_CASE("purchase orders: suppliers and orders need a purchaser at the company, through the guard",
          "[lims][inventory][order][authorization]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    lims::ProcurementModel procurement;
    auto const supply = [&](std::string const& principal, std::string name) {
        ScopedPrincipal const who{principal};
        return ::morph::model::executeChecked(
            procurement, lims::CreateSupplier{.company = lims::test::seededCompany(), .name = std::move(name)});
    };
    CHECK_THROWS_AS(supply("rita", "Rita Ltd"), lims::Forbidden);
    CHECK_THROWS_AS(supply("sven", "Sven Ltd"), lims::Forbidden);
    auto const supplier = supply("paul", "Merck");
    auto const order = [&](std::string const& principal) {
        ScopedPrincipal const who{principal};
        return ::morph::model::executeChecked(
            procurement,
            lims::CreatePurchaseOrder{
                .requisition = requisition.id,
                .supplier = supplier.id,
                .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = price(8500)}}});
    };
    CHECK_THROWS_AS(order("rita"), lims::Forbidden);
    CHECK_THROWS_AS(order("sven"), lims::Forbidden);
    // anna approved it and holds a purchase-approver role only: ordering is a purchaser's.
    CHECK_THROWS_AS(order("anna"), lims::Forbidden);
    CHECK(order("paul").state == lims::OrderState::Open);
    // The handler judges the rule too, for a call that reaches it without the guard.
    ScopedPrincipal const rita{"rita"};
    CHECK_THROWS_AS(procurement.execute(lims::CreatePurchaseOrder{
                        .requisition = requisition.id, .supplier = supplier.id, .prices = {}}),
                    lims::Forbidden);
}

// Mutation: drop the RequisitionView effect from CreatePurchaseOrder's registration (a requisition list that holds the
// requisition is not told it was ordered); drop the SupplierView effect from CreateSupplier.
TEST_CASE("purchase orders: ordering declares that it changes the order and the requisition it moved",
          "[lims][inventory][order]") {
    auto const records = [](std::string_view actionId) {
        std::vector<std::string> names;
        auto const found = morph::effects::effectsOf(actionId);
        REQUIRE(found.has_value());
        for (auto const& effect : found.value_or(decltype(found)::value_type{})) {
            CHECK(effect.kind == morph::effects::Kind::Changes);
            names.push_back(std::string{effect.record});
        }
        std::ranges::sort(names);
        return names;
    };
    CHECK(records("CreatePurchaseOrder") == std::vector<std::string>{"Inventory_Order", "Inventory_Requisition"});
    CHECK(records("CreateSupplier") == std::vector<std::string>{"Inventory_Supplier"});
}

// The nested price list of CreatePurchaseOrder crosses each backend, and the model's refusal comes back through it.
TEST_CASE("purchase orders: suppliers and orders across the backend matrix", "[lims][inventory][order][matrix]") {
    using morph::ladder::testkit::Mode;
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    auto const supplier = lims::test::dispatchAs<lims::ProcurementModel>(
        "paul",
        lims::CreateSupplier{
            .company = lims::test::seededCompany(), .name = "Kanto", .currency = lims::CurrencyCode::JPY},
        mode);
    CHECK(supplier.currency == lims::CurrencyCode::JPY);
    auto const placeAt = [&](lims::Amount unitPrice) {
        return lims::test::dispatchAs<lims::ProcurementModel>(
            "paul",
            lims::CreatePurchaseOrder{
                .requisition = requisition.id,
                .supplier = supplier.id,
                .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id, .unitPrice = unitPrice}}},
            mode);
    };
    CHECK_THROWS_WITH(placeAt(price(85050)), Catch::Matchers::ContainsSubstring("whole number of JPY"));
    auto const order = placeAt(lims::Amount{Rational{8500, DecimalPlaces{0}}});
    CHECK(order.state == lims::OrderState::Open);
    CHECK(order.supplier == supplier.id);
    REQUIRE(order.lines.size() == 1U);
    CHECK(*order.lines.front().unitPrice == Rational{8500, DecimalPlaces{0}});
}
