// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <morph/core/records.hpp>
#include <morph/table/query.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/inventory/core/types.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/dto/shell_dto.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// A company's inventory settings, its items, locations and lots, the stock on hand and the movements that changed
/// it, as the wire sees them.

namespace lims {

/// @brief Sets how a company keeps stock: the ledger accounts its stock postings use and how long a requisition may
///        wait for approval. Setting twice replaces the earlier settings.
///
/// The account codes are names in the company's chart, kept as text: inventory does not read the ledger's tables, and
/// the ledger refuses a posting that names a code its chart lacks.
struct SetInventorySettings {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company.
    CompanyId company;
    /// @brief The account stock on hand is valued on.
    std::string stockAccount;
    /// @brief The account goods received and not yet invoiced are owed on.
    std::string grniAccount;
    /// @brief The account consumed stock is expensed to.
    std::string consumptionAccount;
    /// @brief The account a rounding difference is posted to.
    std::string roundingAccount;
    /// @brief How many hours an approval may wait before it escalates, 1 to 8760.
    Count approvalHours;

    /// @brief Whether the settings are well-formed.
    /// @return `true` when a company, four account codes and the approval hours are named. The range of the hours is
    /// the model's to judge.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && isAccountCode(stockAccount) && isAccountCode(grniAccount) &&
               isAccountCode(consumptionAccount) && isAccountCode(roundingAccount) && approvalHours.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief A company's inventory settings.
struct InventorySettingsView {
    /// @brief The company; the settings' key.
    CompanyId company;
    /// @brief The account stock on hand is valued on.
    std::string stockAccount;
    /// @brief The account goods received and not yet invoiced are owed on.
    std::string grniAccount;
    /// @brief The account consumed stock is expensed to.
    std::string consumptionAccount;
    /// @brief The account a rounding difference is posted to.
    std::string roundingAccount;
    /// @brief How many hours an approval may wait.
    Count approvalHours;
};

/// @brief Reads a company's inventory settings.
struct GetInventorySettings {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief Creates an item a company keeps in stock. Its unit is chosen here and never changes.
struct CreateItem {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company.
    CompanyId company;
    /// @brief The item's code, unique within the company.
    std::string code;
    /// @brief The item's name.
    std::string name;
    /// @brief The unit every quantity of it is held in.
    StockUnit unit = StockUnit::piece;

    /// @brief Whether the item is well-formed.
    /// @return `true` when a company, a code of 1 to 32 bytes and a name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && !code.empty() && code.size() <= 32 && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief An item.
struct ItemView {
    /// @brief Its id.
    ItemId id;
    /// @brief The company.
    CompanyId company;
    /// @brief Its code.
    std::string code;
    /// @brief Its name.
    std::string name;
    /// @brief The unit every quantity of it is held in.
    StockUnit unit = StockUnit::piece;
};

/// @brief Lists a company's items.
struct ListItems {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A company's items, by code.
struct ListItemsResult {
    /// @brief The items.
    std::vector<ItemView> items;
};

/// @brief Creates a place at a site where stock is kept.
struct CreateLocation {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The site.
    SiteId site;
    /// @brief The location's name, unique within the site.
    std::string name;

    /// @brief Whether the location is well-formed.
    /// @return `true` when a site and a name of 1 to 64 bytes are named.
    [[nodiscard]] bool validate() const noexcept { return site.hasValue() && !name.empty() && name.size() <= 64; }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief A location.
struct LocationView {
    /// @brief Its id.
    LocationId id;
    /// @brief The company of its site.
    CompanyId company;
    /// @brief The site.
    SiteId site;
    /// @brief Its name.
    std::string name;
};

/// @brief Lists a company's locations.
struct ListLocations {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A company's locations, by site then name.
struct ListLocationsResult {
    /// @brief The locations.
    std::vector<LocationView> locations;
};

/// @brief Records a lot of stock a company held before lims. It posts nothing: the opening balance is the
///        accountant's own entry.
///
/// The quantity may be in any unit of the item's dimension; it is stored in the item's unit.
struct RecordOpeningLot {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The item.
    ItemId item;
    /// @brief Where the lot is kept.
    LocationId location;
    /// @brief The supplier's lot number, unique within the item.
    std::string lotNumber;
    /// @brief When the lot expires.
    ::morph::time::Timestamp expiresAt;
    /// @brief How much, in @p unit.
    Measure quantity;
    /// @brief The unit @p quantity is in.
    StockUnit unit = StockUnit::piece;
    /// @brief The cost per stock unit, in the company's currency.
    Amount unitCost;

    /// @brief Whether the lot is well-formed.
    /// @return `true` when an item, a location, a lot number of 1 to 32 bytes, an expiry, a quantity and a cost are
    /// named.
    [[nodiscard]] bool validate() const noexcept {
        return item.hasValue() && location.hasValue() && !lotNumber.empty() && lotNumber.size() <= 32 &&
               expiresAt.hasValue() && quantity.hasValue() && unitCost.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief A lot with the stock on hand and the part of it reserved.
struct LotStockView {
    /// @brief Its id; the lot's key.
    LotId lot;
    /// @brief The item.
    ItemId item;
    /// @brief The item's code.
    std::string itemCode;
    /// @brief The supplier's lot number.
    std::string lotNumber;
    /// @brief Where it is kept.
    LocationId location;
    /// @brief The site of its location.
    SiteId site;
    /// @brief The company.
    CompanyId company;
    /// @brief When it expires.
    ::morph::time::Timestamp expiresAt;
    /// @brief The stock on hand, in @p unit.
    Measure onHand;
    /// @brief The part of it reserved, in @p unit.
    Measure reserved;
    /// @brief The item's unit.
    StockUnit unit = StockUnit::piece;
    /// @brief The cost per stock unit.
    Amount unitCost;
    /// @brief The currency of @p unitCost.
    CurrencyCode currency = CurrencyCode::EUR;
};

/// @brief Lists the lots of a company, optionally of one item.
struct StockOnHand {
    /// @brief The company.
    CompanyId company;
    /// @brief The item; empty for every item.
    ItemId item;

    /// @brief The item is optional.
    static constexpr std::array optionalFields{std::string_view{"item"}};

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A company's lots, by expiry then id.
struct StockOnHandResult {
    /// @brief The lots.
    std::vector<LotStockView> lots;
};

/// @brief One change of a lot's stock on hand.
struct StockMovementView {
    /// @brief Its id.
    MovementId id;
    /// @brief The item.
    ItemId item;
    /// @brief The lot; empty for a shortfall.
    LotId lot;
    /// @brief Why it happened.
    MovementKind kind = MovementKind::Opening;
    /// @brief The change in @p unit: positive in, negative out.
    Measure quantity;
    /// @brief The item's unit.
    StockUnit unit = StockUnit::piece;
    /// @brief When.
    ::morph::time::Timestamp at;
    /// @brief Who.
    std::string by;
    /// @brief What caused it.
    std::string source;
};

/// @brief Lists the movements of an item through the table engine's query.
struct ListStockMovements {
    /// @brief The item.
    ItemId item;
    /// @brief The sort, filters and page.
    ::morph::table::TableQuery query;

    /// @brief Whether an item is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return item.hasValue(); }
};

/// @brief Finds the lots of a company by the start of their item's code or of their lot number.
struct SearchStock {
    /// @brief The text typed.
    std::string query;
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::InventorySettingsView, "Inventory_Settings",
                       ::morph::record::key<&lims::InventorySettingsView::company>)
BRIDGE_REGISTER_RECORD(lims::ItemView, "Inventory_Item")
BRIDGE_REGISTER_RECORD(lims::LocationView, "Inventory_Location")
BRIDGE_REGISTER_RECORD(lims::LotStockView, "Inventory_Lot", ::morph::record::key<&lims::LotStockView::lot>)
BRIDGE_REGISTER_RECORD(lims::StockMovementView, "Inventory_Movement")
BRIDGE_REGISTER_RECORD(lims::SearchHits<lims::LotStockView>, "Inventory_StockHits", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::LotStockView, lims::RoleAssignmentView>)
