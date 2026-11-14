// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <tuple>
#include <utility>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/dto/order_dto.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"

/// @file
/// What every inventory suite starts from: settings for the seeded company, a reagent stocked in litres, a cold room
/// at the seeded site, and `kim`, a stock keeper there.

namespace lims::test {

/// @brief @p thousandths / 1000 of the carrier unit: `litres(250)` is 0.250.
/// @param thousandths The amount in thousandths.
/// @return The measure, at six places.
[[nodiscard]] inline Measure litres(std::int64_t thousandths) {
    return Measure{::morph::math::Rational{::morph::math::Numerator{thousandths}, ::morph::math::Denominator{1000},
                                           ::morph::math::DecimalPlaces{6}}};
}

/// @brief A whole number of the carrier unit: `millilitres(250)` is 250.
/// @param whole The amount.
/// @return The measure, at six places.
[[nodiscard]] inline Measure millilitres(std::int64_t whole) {
    return Measure{::morph::math::Rational{whole, ::morph::math::DecimalPlaces{6}}};
}

/// @brief What `stockSetUp` made.
struct StockSetUp {
    /// @brief The nitrate reagent, stocked in litres.
    ItemId reagent;
    /// @brief The cold room at the seeded site.
    LocationId coldRoom;
};

/// @brief Inventory settings for the seeded company (stock 3100, goods received not invoiced 1600, consumption 5100,
///        rounding 6900, 48 approval hours), the nitrate reagent `NO3-R` stocked in litres, and the cold room at the
///        seeded site. `sam` is supervisor and `kim` stock keeper at the seeded company.
/// @return The ids.
inline StockSetUp stockSetUp() {
    grant("sam", LimsRole::Supervisor);
    grant("kim", LimsRole::StockKeeper);
    {
        ScopedPrincipal const sam{"sam"};
        StockModel{}.execute(SetInventorySettings{
            .company = seededCompany(),
            .stockAccount = "3100",
            .grniAccount = "1600",
            .consumptionAccount = "5100",
            .roundingAccount = "6900",
            .approvalHours = Count{::morph::math::Rational{48, ::morph::math::DecimalPlaces{0}}}});
    }
    ScopedPrincipal const kim{"kim"};
    StockModel stock;
    auto const item = stock.execute(
        CreateItem{.company = seededCompany(), .code = "NO3-R", .name = "Nitrate reagent", .unit = StockUnit::litre});
    auto const room = stock.execute(CreateLocation{.site = seededSite(), .name = "Cold room"});
    return StockSetUp{.reagent = item.id, .coldRoom = room.id};
}

/// @brief An opening lot recorded by `kim`, at 12.50 per stock unit in the company's currency.
/// @param item The item.
/// @param location The location.
/// @param lotNumber The supplier's lot number.
/// @param expires When it expires.
/// @param quantity How much.
/// @param unit The unit @p quantity is in.
/// @return The lot as it stands.
inline LotStockView openingLot(ItemId item, LocationId location, std::string lotNumber,
                               ::morph::time::DateTime expires, Measure quantity, StockUnit unit) {
    ScopedPrincipal const kim{"kim"};
    return StockModel{}.execute(RecordOpeningLot{
        .item = item,
        .location = location,
        .lotNumber = std::move(lotNumber),
        .expiresAt = ::morph::time::Timestamp{expires},
        .quantity = std::move(quantity),
        .unit = unit,
        .unitCost = Amount{::morph::math::Rational{::morph::math::Numerator{1250}, ::morph::math::Denominator{100},
                                                   ::morph::math::DecimalPlaces{2}}}});
}

/// @brief What `requisitionSetUp` made.
struct RequisitionSetUp {
    /// @brief The stock `stockSetUp` made.
    StockSetUp stock;
    /// @brief A draft requisition by `rita` at the seeded site for 2 L of the reagent, estimated at 180.00.
    RequisitionView draft;
};

/// @brief `stockSetUp`, purchase approvers and their limits for requisitions, and a draft by `rita`, an analyst at the
///        seeded site. Limits for the purchase approver role at the seeded company: `anna` 5000.00 and `tom` 10.00,
///        approvers at the seeded site; `julia` 50000.00, approver at the company. Set at 09:00 on 10 November 2026.
/// @return The ids and the draft.
inline RequisitionSetUp requisitionSetUp() {
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    RequisitionSetUp setUp{.stock = stockSetUp(), .draft = {}};
    {
        ScopedPrincipal const sam{"sam"};
        OrgModel org;
        auto const limit = [&org](std::string principal, std::int64_t cents) {
            org.execute(SetValueLimit{.principal = std::move(principal),
                                      .company = seededCompany(),
                                      .limit = Amount{::morph::math::Rational{::morph::math::Numerator{cents},
                                                                              ::morph::math::Denominator{100},
                                                                              ::morph::math::DecimalPlaces{2}}},
                                      .role = LimsRole::PurchaseApprover});
        };
        limit("anna", 500000);
        limit("tom", 1000);
        limit("julia", 5000000);
    }
    grant("anna", LimsRole::PurchaseApprover, Scope{seededSite()});
    grant("tom", LimsRole::PurchaseApprover, Scope{seededSite()});
    grant("julia", LimsRole::PurchaseApprover);
    grant("rita", LimsRole::Analyst, Scope{seededSite()});
    ScopedPrincipal const rita{"rita"};
    ProcurementModel procurement;
    auto const draft = procurement.execute(
        CreateRequisition{.company = seededCompany(), .site = seededSite(), .reason = "Nitrate reagent for November"});
    setUp.draft = procurement.execute(AddRequisitionLine{
        .requisition = draft.id,
        .item = setUp.stock.reagent,
        .quantity = litres(2000),
        .unit = StockUnit::litre,
        .estimate = Amount{::morph::math::Rational{::morph::math::Numerator{18000}, ::morph::math::Denominator{100},
                                                   ::morph::math::DecimalPlaces{2}}}});
    return setUp;
}

/// @brief What `approvedRequisition` made.
struct ApprovedRequisition {
    /// @brief The requisition, approved.
    RequisitionView requisition;
    /// @brief The stock `stockSetUp` made.
    StockSetUp stock;
};

/// @brief What `openOrder` made.
struct OpenOrder {
    /// @brief The order, open.
    PurchaseOrderView order;
    /// @brief The stock `stockSetUp` made.
    StockSetUp stock;
};

/// @brief `requisitionSetUp`, submitted by rita and approved by anna at 10:00 on 10 November 2026; `paul` is a
///        purchaser at the seeded company.
/// @return The approved requisition and the stock.
inline ApprovedRequisition approvedRequisition() {
    auto const setUp = requisitionSetUp();
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    {
        ScopedPrincipal const rita{"rita"};
        static_cast<void>(ProcurementModel{}.execute(SubmitRequisition{.requisitionId = setUp.draft.id}));
    }
    grant("paul", LimsRole::Purchaser);
    ScopedPrincipal const anna{"anna"};
    return ApprovedRequisition{
        .requisition = ProcurementModel{}.execute(ApproveRequisition{.requisitionId = setUp.draft.id}),
        .stock = setUp.stock};
}

/// @brief An order by `paul` for the approved requisition's 2 L, from "Merck" in @p supplierCurrency, at 85.00 a litre
///        (8500 yen in JPY).
/// @param supplierCurrency The supplier's currency.
/// @return The open order and the stock.
inline OpenOrder openOrder(CurrencyCode supplierCurrency = CurrencyCode::EUR) {
    auto const approved = approvedRequisition();
    auto const& requisition = approved.requisition;
    ScopedPrincipal const paul{"paul"};
    ProcurementModel procurement;
    auto const supplier =
        procurement.execute(CreateSupplier{.company = seededCompany(), .name = "Merck", .currency = supplierCurrency});
    // 85.00 a litre is 8500/100; in yen, 8500/1.
    constexpr std::int64_t price = 8500;
    std::uint32_t const places = decimalsOf(supplierCurrency);
    std::int64_t const denominator = places == 0U ? 1 : 100;
    auto order = procurement.execute(CreatePurchaseOrder{
        .requisition = requisition.id,
        .supplier = supplier.id,
        .prices = {OrderLinePrice{.line = requisition.lines.front().id,
                                  .unitPrice = Amount{::morph::math::Rational{
                                      ::morph::math::Numerator{price}, ::morph::math::Denominator{denominator},
                                      ::morph::math::DecimalPlaces{places}}}}}});
    return OpenOrder{.order = std::move(order), .stock = approved.stock};
}

/// @brief `books()`, plus the stock (3100, asset), goods received not invoiced (1600, liability) and consumption
///        (5100, expense) accounts in the seeded company's chart, made by `acc`, an accountant there; and a CHF to EUR
///        rate of 0.95 from 1 November 2026. The rounding account 6900 is `books()`'s.
inline void inventoryBooks() {
    books();
    ScopedPrincipal const acc{"acc"};
    LedgerModel ledger;
    for (auto const& [code, name, type] : {std::tuple{"3100", "Stock", AccountType::Asset},
                                           std::tuple{"1600", "Goods received not invoiced", AccountType::Liability},
                                           std::tuple{"5100", "Reagent consumption", AccountType::Expense}}) {
        static_cast<void>(
            ledger.execute(CreateAccount{.company = seededCompany(), .code = code, .name = name, .type = type}));
    }
    static_cast<void>(ledger.execute(SetExchangeRate{
        .company = seededCompany(),
        .from = CurrencyCode::CHF,
        .to = CurrencyCode::EUR,
        .validFrom = ::morph::time::Timestamp{at(2026, 11, 1, 0)},
        .rate = Rate{::morph::math::Rational{::morph::math::Numerator{95}, ::morph::math::Denominator{100},
                                             ::morph::math::DecimalPlaces{6}}}}));
}

/// @brief What `consumptionSetUp` made.
struct ConsumptionSetUp {
    /// @brief The stock.
    StockSetUp stock;
    /// @brief The lots: `L-EARLY` (0.040 L, expiring 31 January 2027) and `L-LATE` (1 L, expiring 30 June 2027).
    LotStockView early;
    /// @brief The later lot.
    LotStockView late;
};

/// @brief `stockSetUp`, lots of the reagent in the cold room, and a recipe: each analysis of id 1 to 20 uses 25 mL of
///        the reagent. The events principal holds the service role at the seeded company.
/// @param withLateLot Whether `L-LATE` is recorded; without it, `L-EARLY` is the last lot.
/// @return The ids; `late` is empty without the late lot.
inline ConsumptionSetUp consumptionSetUp(bool withLateLot = true) {
    ConsumptionSetUp setUp{.stock = stockSetUp(), .early = {}, .late = {}};
    setUp.early = openingLot(setUp.stock.reagent, setUp.stock.coldRoom, "L-EARLY", at(2027, 1, 31), litres(40),
                             StockUnit::litre);
    if (withLateLot) {
        setUp.late = openingLot(setUp.stock.reagent, setUp.stock.coldRoom, "L-LATE", at(2027, 6, 30), litres(1000),
                                StockUnit::litre);
    }
    ScopedPrincipal const kim{"kim"};
    StockModel stock;
    for (std::int64_t analysis = 1; analysis <= 20; ++analysis) {
        static_cast<void>(stock.execute(SetRecipe{.company = seededCompany(),
                                                  .analysis = AnalysisId{analysis},
                                                  .item = setUp.stock.reagent,
                                                  .quantity = millilitres(25),
                                                  .unit = StockUnit::millilitre}));
    }
    grantService(std::string{app::kEventsPrincipal});
    return setUp;
}

/// @brief A server, the posting principal's client on it and the ledger port installed in a mode, as `PostingRig`
///        gives billing: goods receipt posts through the same port.
using ReceiptRig = PostingRig;

}  // namespace lims::test
