// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <utility>

#include "clock.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
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

}  // namespace lims::test
