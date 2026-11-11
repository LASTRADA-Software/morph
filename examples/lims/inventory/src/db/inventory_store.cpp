// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/db/inventory_store.hpp"

#include <Lightweight/SqlStatement.hpp>
#include <string_view>

#include "lims/inventory/core/fault_hook.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims::inventory {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr int kMaxStockUnit = static_cast<int>(StockUnit::piece);

/// @brief The lot number a row stores, as text.
/// @param lot The row.
/// @return It.
[[nodiscard]] std::string numberOf(db::LotRecord const& lot) {
    return std::string{lot.lotNumber.Value().ToStringView()};
}

}  // namespace

std::size_t changedRows(Lightweight::DataMapper& mapper, std::string const& sql) {
    Lightweight::SqlStatement statement{mapper.Connection()};
    return static_cast<std::size_t>(statement.ExecuteDirect(sql).NumRowsAffected());
}

Rational moveLot(Lightweight::DataMapper& mapper, db::LotRecord const& lot, MovementKind kind,
                 Rational const& quantity, std::string const& source, std::string const& principal) {
    Rational const before{Numerator{lot.onHandNum.Value()}, Denominator{lot.onHandDen.Value()}, DecimalPlaces{6}};
    auto const after = ::morph::math::checkedAdd(before, quantity);
    if (!after.has_value()) {
        throw OutOfRange{"lot " + numberOf(lot) + ": stock on hand does not fit"};
    }
    if (after->numerator < 0) {
        throw ValidationError{"lot " + numberOf(lot) + " holds less than that"};
    }
    auto update = mapper.Connection().Query(db::LotRecord::TableName).Update();
    update.Set("on_hand_num", after->numerator).Set("on_hand_den", after->denominator);
    (void)update.Where("id", "=", static_cast<std::int64_t>(lot.id.Value()))
        .Where("on_hand_num", "=", lot.onHandNum.Value())
        .Where("on_hand_den", "=", lot.onHandDen.Value());
    if (changedRows(mapper, update.ToSql()) != 1U) {
        throw Conflict{"lot " + numberOf(lot) + " changed meanwhile"};
    }
    inventoryFaultPoint("lot.afterLotWrite", mapper.Connection());
    db::MovementRecord movement;
    movement.companyId = lot.companyId.Value();
    movement.itemId = lot.itemId.Value();
    movement.lotId = static_cast<std::int64_t>(lot.id.Value());
    movement.kind = static_cast<int>(kind);
    movement.quantityNum = quantity.numerator;
    movement.quantityDen = quantity.denominator;
    movement.at = nowMillis();
    movement.by = Lightweight::SqlAnsiString<64>{principal};
    movement.source = Lightweight::SqlAnsiString<128>{source};
    mapper.Create(movement);
    return *after;
}

LotStockView lotView(Lightweight::DataMapper& mapper, db::LotRecord const& lot) {
    auto const item = findItem(mapper, lot.itemId.Value());
    if (!item.has_value()) {
        throw NotFound{"lot " + numberOf(lot) + ": its item does not exist"};
    }
    auto const company = OrgModel::findCompany(CompanyId{lot.companyId.Value()});
    if (!company.has_value()) {
        throw NotFound{"lot " + numberOf(lot) + ": its company does not exist"};
    }
    // Nothing reserves stock yet, so the reserved quantity is zero.
    return LotStockView{
        .lot = LotId{static_cast<std::int64_t>(lot.id.Value())},
        .item = ItemId{lot.itemId.Value()},
        .itemCode = std::string{item->code.Value().ToStringView()},
        .lotNumber = numberOf(lot),
        .location = LocationId{lot.locationId.Value()},
        .site = SiteId{lot.siteId.Value()},
        .company = CompanyId{lot.companyId.Value()},
        .expiresAt = timestampFromMillis(lot.expiresAt.Value()),
        .onHand =
            Measure{Rational{Numerator{lot.onHandNum.Value()}, Denominator{lot.onHandDen.Value()}, DecimalPlaces{6}}},
        .reserved = Measure{Rational{Numerator{0}, Denominator{1}, DecimalPlaces{6}}},
        .unit = stockUnitOf(*item),
        .unitCost = Amount{Rational{Numerator{lot.unitCostNum.Value()}, Denominator{lot.unitCostDen.Value()},
                                    DecimalPlaces{decimalsOf(company->baseCurrency)}}},
        .currency = company->baseCurrency,
    };
}

std::optional<db::ItemRecord> findItem(Lightweight::DataMapper& mapper, std::int64_t item) {
    auto rows = mapper.Query<db::ItemRecord>().Where(::Lightweight::FieldNameOf<&db::ItemRecord::id>, "=", item).All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

std::optional<db::LocationRecord> findLocation(Lightweight::DataMapper& mapper, std::int64_t location) {
    auto rows = mapper.Query<db::LocationRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LocationRecord::id>, "=", location)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

StockUnit stockUnitOf(db::ItemRecord const& item) {
    int const unit = item.unit.Value();
    if (unit < 0 || unit > kMaxStockUnit) {
        throw ValidationError{"item " + std::to_string(item.id.Value()) + " stores a unit outside its range"};
    }
    return static_cast<StockUnit>(unit);
}

bool lotNumberTaken(std::int64_t item, std::string const& lotNumber) {
    Lightweight::DataMapper mapper;
    return !mapper.Query<db::LotRecord>()
                .Where(::Lightweight::FieldNameOf<&db::LotRecord::itemId>, "=", item)
                .Where(::Lightweight::FieldNameOf<&db::LotRecord::lotNumber>, "=", lotNumber)
                .All()
                .empty();
}

db::LotRecord lotRecord(Lightweight::DataMapper& mapper, std::int64_t lot) {
    auto rows = mapper.Query<db::LotRecord>().Where(::Lightweight::FieldNameOf<&db::LotRecord::id>, "=", lot).All();
    if (rows.empty()) {
        throw NotFound{"lot " + std::to_string(lot) + " does not exist"};
    }
    return rows.front();
}

}  // namespace lims::inventory
