// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/models/stock_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr std::int64_t kMinApprovalHours = 1;
// A year: the deadline `now + hours × 3,600,000` ms cannot overflow.
constexpr std::int64_t kMaxApprovalHours = 8760;

/// @brief Throws `NotFound` unless @p company exists.
/// @param where The action, for the message.
/// @param company The company.
void requireCompany(std::string_view where, CompanyId company) {
    if (!OrgModel::findCompany(company).has_value()) {
        throw NotFound{std::string{where} + ": company " + std::to_string(*company) + " does not exist"};
    }
}

/// @brief Runs an insert that a lookup guards, answering the loss of a race on the unique index as the conflict.
///
/// A concurrent call that inserted the same key makes the index refuse this one, and this call's transaction is rolled
/// back. The winner's row is there now, so the answer is the same Conflict a later call gets. A failure that is not
/// that race finds no such row and surfaces as it is.
/// @param insert One attempt, in its own transaction.
/// @param exists Whether a row holds the key now.
/// @param conflict The refusal for a key that is taken.
/// @return What @p insert answers.
template <typename Insert, typename Exists, typename MakeConflict>
auto guardedInsert(Insert const& insert, Exists const& exists, MakeConflict const& conflict) {
    try {
        return insert();
    } catch (Lightweight::SqlException const&) {
        if (exists()) {
            throw conflict();
        }
        throw;
    }
}

[[nodiscard]] InventorySettingsView toView(db::InventorySettingsRecord const& row) {
    return InventorySettingsView{
        .company = CompanyId{row.companyId.Value()},
        .stockAccount = std::string{row.stockAccount.Value().ToStringView()},
        .grniAccount = std::string{row.grniAccount.Value().ToStringView()},
        .consumptionAccount = std::string{row.consumptionAccount.Value().ToStringView()},
        .roundingAccount = std::string{row.roundingAccount.Value().ToStringView()},
        .approvalHours = Count{Rational{Numerator{row.approvalHours.Value()}, Denominator{1}, DecimalPlaces{0}}},
    };
}

[[nodiscard]] ItemView toView(db::ItemRecord const& row) {
    return ItemView{
        .id = ItemId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .code = std::string{row.code.Value().ToStringView()},
        .name = std::string{row.name.Value().ToStringView()},
        .unit = inventory::stockUnitOf(row),
    };
}

[[nodiscard]] LocationView toView(db::LocationRecord const& row) {
    return LocationView{
        .id = LocationId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .site = SiteId{row.siteId.Value()},
        .name = std::string{row.name.Value().ToStringView()},
    };
}

[[nodiscard]] std::optional<db::InventorySettingsRecord> settingsRow(Lightweight::DataMapper& mapper,
                                                                     CompanyId company) {
    auto rows = mapper.Query<db::InventorySettingsRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::InventorySettingsRecord::companyId>, "=", *company)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

[[nodiscard]] std::optional<db::ItemRecord> itemByCode(Lightweight::DataMapper& mapper, CompanyId company,
                                                       std::string const& code) {
    auto rows = mapper.Query<db::ItemRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ItemRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::ItemRecord::code>, "=", code)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

[[nodiscard]] std::optional<db::LocationRecord> locationByName(Lightweight::DataMapper& mapper, SiteId site,
                                                               std::string const& name) {
    auto rows = mapper.Query<db::LocationRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LocationRecord::siteId>, "=", *site)
                    .Where(::Lightweight::FieldNameOf<&db::LocationRecord::name>, "=", name)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

/// @brief Refuses an approval-hours count that is not a whole number in range.
/// @param action The settings.
/// @throws ValidationError if it is not.
void requireHoursInRange(SetInventorySettings const& action) {
    Rational const& hours = *action.approvalHours;
    if (!hours.isInteger() || hours.numerator < kMinApprovalHours || hours.numerator > kMaxApprovalHours) {
        throw ValidationError{"SetInventorySettings: the approval hours must be a whole number from 1 to 8760"};
    }
}

/// @brief One attempt at storing settings, in its own transaction.
/// @param action The settings.
/// @return The stored settings.
/// @throws Lightweight::SqlException if the write fails.
[[nodiscard]] InventorySettingsView storeSettingsOnce(SetInventorySettings const& action) {
    // The lookup runs on a connection of its own, outside the write's transaction: a read inside it would hold a
    // lock that blocks a concurrent writer's commit.
    auto const existing = [&] {
        Lightweight::DataMapper lookup;
        return settingsRow(lookup, action.company);
    }();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::InventorySettingsRecord row = existing.value_or(db::InventorySettingsRecord{});
    row.companyId = *action.company;
    row.stockAccount = Lightweight::SqlAnsiString<16>{action.stockAccount};
    row.grniAccount = Lightweight::SqlAnsiString<16>{action.grniAccount};
    row.consumptionAccount = Lightweight::SqlAnsiString<16>{action.consumptionAccount};
    row.roundingAccount = Lightweight::SqlAnsiString<16>{action.roundingAccount};
    row.approvalHours = (*action.approvalHours).numerator;
    if (existing.has_value()) {
        mapper.Update(row);
    } else {
        mapper.Create(row);
    }
    sqlTxn.Commit();
    return toView(row);
}

/// @brief Stores a company's settings, replacing any it had.
/// @param action The settings, already validated and authorized.
/// @return The stored settings.
/// @throws Lightweight::SqlException if the write fails for another reason than a concurrent first call.
[[nodiscard]] InventorySettingsView storeSettings(SetInventorySettings const& action) {
    try {
        return storeSettingsOnce(action);
    } catch (Lightweight::SqlException const&) {
        // A concurrent first call for the company inserted its row between this call's lookup and its insert, and
        // this call's transaction is rolled back. Now the row is there, so this call replaces it, as if it had come
        // after. A failure that is not that race finds no row and surfaces as it is.
        Lightweight::DataMapper mapper;
        if (settingsRow(mapper, action.company).has_value()) {
            return storeSettingsOnce(action);
        }
        throw;
    }
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
InventorySettingsView StockModel::execute(const SetInventorySettings& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "SetInventorySettings: a company, four account codes of 1 to 16 upper-case letters and digits and the "
            "approval hours are required"};
    }
    requireHoursInRange(action);
    requireCompany("SetInventorySettings", action.company);
    return storeSettings(action);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
InventorySettingsView StockModel::execute(const GetInventorySettings& action) {
    if (!action.validate()) {
        throw ValidationError{"GetInventorySettings: a company is required"};
    }
    std::optional<db::InventorySettingsRecord> row;
    if (OrgModel::callerSees(action.company)) {
        Lightweight::DataMapper mapper;
        row = settingsRow(mapper, action.company);
    }
    if (!row.has_value()) {
        throw NotFound{"GetInventorySettings: company " + std::to_string(*action.company) +
                       " has no inventory settings"};
    }
    return toView(*row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ItemView StockModel::execute(const CreateItem& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "CreateItem: a company, a code of 1 to 32 bytes and a name of 1 to 128 bytes are required"};
    }
    requireCompany("CreateItem", action.company);
    auto const conflict = [&] {
        return Conflict{"CreateItem: company " + std::to_string(*action.company) + " has an item " + action.code +
                        " already"};
    };
    return guardedInsert(
        [&] {
            // The lookup runs on a mapper of its own, outside the write's transaction below.
            if (std::invoke([&] {
                    Lightweight::DataMapper lookup;
                    return itemByCode(lookup, action.company, action.code).has_value();
                })) {
                throw conflict();
            }
            Lightweight::DataMapper mapper;
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            db::ItemRecord row;
            row.companyId = *action.company;
            row.code = Lightweight::SqlAnsiString<32>{action.code};
            row.name = Lightweight::SqlAnsiString<128>{action.name};
            row.unit = static_cast<int>(action.unit);
            mapper.Create(row);
            sqlTxn.Commit();
            return toView(row);
        },
        [&] {
            Lightweight::DataMapper mapper;
            return itemByCode(mapper, action.company, action.code).has_value();
        },
        conflict);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListItemsResult StockModel::execute(const ListItems& action) {
    if (!action.validate()) {
        throw ValidationError{"ListItems: a company is required"};
    }
    ListItemsResult result;
    if (!OrgModel::callerSees(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::ItemRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::ItemRecord::companyId>, "=", *action.company)
                               .All()) {
        result.items.push_back(toView(row));
    }
    std::ranges::sort(result.items, {}, [](ItemView const& view) { return view.code; });
    return result;
}

std::optional<InventorySettingsView> StockModel::settingsOf(CompanyId company) {
    Lightweight::DataMapper mapper;
    auto const row = settingsRow(mapper, company);
    return row.has_value() ? std::optional{toView(*row)} : std::nullopt;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
LocationView StockModel::execute(const CreateLocation& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateLocation: a site and a name of 1 to 64 bytes are required"};
    }
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"CreateLocation: site " + std::to_string(*action.site) + " does not exist"};
    }
    auto const conflict = [&] {
        return Conflict{"CreateLocation: site " + std::to_string(*action.site) + " has a location " + action.name +
                        " already"};
    };
    return guardedInsert(
        [&] {
            // The lookup runs on a mapper of its own, outside the write's transaction below.
            if (std::invoke([&] {
                    Lightweight::DataMapper lookup;
                    return locationByName(lookup, action.site, action.name).has_value();
                })) {
                throw conflict();
            }
            Lightweight::DataMapper mapper;
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            db::LocationRecord row;
            row.companyId = *site->companyId;
            row.siteId = *action.site;
            row.name = Lightweight::SqlAnsiString<64>{action.name};
            mapper.Create(row);
            sqlTxn.Commit();
            return toView(row);
        },
        [&] {
            Lightweight::DataMapper mapper;
            return locationByName(mapper, action.site, action.name).has_value();
        },
        conflict);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListLocationsResult StockModel::execute(const ListLocations& action) {
    if (!action.validate()) {
        throw ValidationError{"ListLocations: a company is required"};
    }
    ListLocationsResult result;
    if (!OrgModel::callerSees(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::LocationRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::LocationRecord::companyId>, "=", *action.company)
                               .All()) {
        result.locations.push_back(toView(row));
    }
    std::ranges::sort(result.locations, {},
                      [](LocationView const& view) { return std::tuple{*view.site, view.name}; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
LotStockView StockModel::execute(const RecordOpeningLot& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "RecordOpeningLot: an item, a location, a lot number, an expiry, a quantity and a cost "
            "are required"};
    }
    // Reads on a mapper of their own: the transaction below opens with its first write.
    Lightweight::DataMapper reader;
    auto const item = inventory::findItem(reader, *action.item);
    if (!item.has_value()) {
        throw NotFound{"RecordOpeningLot: item " + std::to_string(*action.item) + " does not exist"};
    }
    auto const location = inventory::findLocation(reader, *action.location);
    if (!location.has_value() || location->companyId.Value() != item->companyId.Value()) {
        throw NotFound{"RecordOpeningLot: location " + std::to_string(*action.location) + " is not one of " +
                       std::string{item->code.Value().ToStringView()} + "'s company"};
    }
    auto const unit = inventory::stockUnitOf(*item);
    auto const quantity = convertStock(*action.quantity, action.unit, unit);
    if (quantity.numerator <= 0) {
        throw ValidationError{"RecordOpeningLot: the quantity must be positive"};
    }
    auto const company = OrgModel::findCompany(CompanyId{item->companyId.Value()});
    if (!company.has_value()) {
        throw NotFound{"RecordOpeningLot: company " + std::to_string(item->companyId.Value()) + " does not exist"};
    }
    if ((*action.unitCost).isNegative()) {
        throw ValidationError{"RecordOpeningLot: the unit cost must not be negative"};
    }
    if (!fitsCurrency(*action.unitCost, company->baseCurrency)) {
        throw ValidationError{"RecordOpeningLot: the unit cost is not a whole number of the company's minor units"};
    }
    std::string const principal = ::morph::session::current()->principal;
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const lot = inventory::createLot(mapper, "RecordOpeningLot", *item, *location, action.lotNumber,
                                          action.expiresAt, *action.unitCost);
    static_cast<void>(inventory::moveLot(mapper, lot, MovementKind::Opening, quantity, "opening", principal));
    sqlTxn.Commit();
    return inventory::lotView(mapper, inventory::lotRecord(mapper, static_cast<std::int64_t>(lot.id.Value())));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
StockOnHandResult StockModel::execute(const StockOnHand& action) {
    if (!action.validate()) {
        throw ValidationError{"StockOnHand: a company is required"};
    }
    StockOnHandResult result;
    if (!OrgModel::callerSees(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    std::vector<db::LotRecord> rows;
    for (auto const& row : mapper.Query<db::LotRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::LotRecord::companyId>, "=", *action.company)
                               .All()) {
        if (!action.item.hasValue() || row.itemId.Value() == *action.item) {
            rows.push_back(row);
        }
    }
    std::ranges::sort(rows, {},
                      [](db::LotRecord const& lot) { return std::tuple{lot.expiresAt.Value(), lot.id.Value()}; });
    for (auto const& row : rows) {
        result.lots.push_back(inventory::lotView(mapper, row));
    }
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ReservationView StockModel::execute(const GetSampleReservation& action) {
    if (!action.validate()) {
        throw ValidationError{"GetSampleReservation: a sample is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = inventory::liveReservationsOfSample(mapper, *action.sample);
    std::erase_if(rows, [](db::ReservationRecord const& row) {
        return !OrgModel::callerSees(CompanyId{row.companyId.Value()});
    });
    return inventory::reservationView(mapper, *action.sample, rows);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::morph::table::Page<StockMovementView> StockModel::execute(const ListStockMovements& action) {
    if (!action.validate()) {
        throw ValidationError{"ListStockMovements: an item is required"};
    }
    Lightweight::DataMapper mapper;
    auto const item = inventory::findItem(mapper, *action.item);
    std::vector<StockMovementView> rows;
    if (item.has_value() && OrgModel::callerSees(CompanyId{item->companyId.Value()})) {
        auto const unit = inventory::stockUnitOf(*item);
        for (auto const& row : mapper.Query<db::MovementRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::MovementRecord::itemId>, "=", *action.item)
                                   .All()) {
            rows.push_back(inventory::movementView(row, unit));
        }
    }
    std::ranges::sort(rows, {}, [](StockMovementView const& movement) { return *movement.id; });
    auto page = ::morph::table::apply<StockMovementView>(
        rows, [](StockMovementView const& movement) { return ::morph::table::RowId{*movement.id}; }, action.query);
    if (!page) {
        throw ValidationError{"ListStockMovements: " + page.error().message};
    }
    return std::move(*page);
}

}  // namespace lims
