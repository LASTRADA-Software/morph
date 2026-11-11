// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

/// @file
/// Inventory's tables, prefixed `inventory_`. Companies and sites are another module's rows: their ids are stored as
/// plain columns, since no module reads another's tables.

namespace lims::db {

/// @brief One company's inventory settings.
struct InventorySettingsRecord {
    static constexpr std::string_view TableName = "inventory_settings";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"stock_account"}> stockAccount;              // 2
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"grni_account"}> grniAccount;                // 3
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"consumption_account"}> consumptionAccount;  // 4
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"rounding_account"}> roundingAccount;        // 5
    Light::Field<std::int64_t, Light::SqlRealName{"approval_hours"}> approvalHours{0};                     // 6
};

/// @brief One item a company keeps in stock.
struct ItemRecord {
    static constexpr std::string_view TableName = "inventory_items";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"code"}> code;                               // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 3
    /// @brief `StockUnit` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"unit"}> unit{0};  // 4
};

/// @brief One place at a site where stock is kept.
struct LocationRecord {
    static constexpr std::string_view TableName = "inventory_locations";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 2
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"name"}> name;                               // 3
};

/// @brief One lot of an item at a location, with the stock on hand.
struct LotRecord {
    static constexpr std::string_view TableName = "inventory_lots";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"item_id"}> itemId{0};                                   // 1
    Light::Field<std::int64_t, Light::SqlRealName{"location_id"}> locationId{0};                           // 2
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 3
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 4
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"lot_number"}> lotNumber;                    // 5
    /// @brief When the lot expires, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"expires_at"}> expiresAt{0};  // 6
    /// @brief The stock on hand, in the item's unit.
    Light::Field<std::int64_t, Light::SqlRealName{"on_hand_num"}> onHandNum{0};  // 7
    Light::Field<std::int64_t, Light::SqlRealName{"on_hand_den"}> onHandDen{1};  // 8
    /// @brief The cost per stock unit, in the company's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"unit_cost_num"}> unitCostNum{0};  // 9
    Light::Field<std::int64_t, Light::SqlRealName{"unit_cost_den"}> unitCostDen{1};  // 10
    /// @brief Written first in every reservation, so contenders for one lot are ordered by a conditional update.
    Light::Field<std::int64_t, Light::SqlRealName{"reservation_version"}> reservationVersion{0};  // 11
};

/// @brief One change of a lot's stock on hand.
struct MovementRecord {
    static constexpr std::string_view TableName = "inventory_movements";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"item_id"}> itemId{0};                                   // 2
    /// @brief The lot; empty for a shortfall, which names none.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"lot_id"}> lotId;  // 3
    /// @brief `MovementKind` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind{0};  // 4
    /// @brief The change in the item's unit: positive in, negative out.
    Light::Field<std::int64_t, Light::SqlRealName{"quantity_num"}> quantityNum{0};  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"quantity_den"}> quantityDen{1};  // 6
    /// @brief When, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"at"}> at{0};           // 7
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"by"}> by;  // 8
    /// @brief What caused it: `opening`, `receipt:<id>` or the consuming event's id.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source"}> source;  // 9
};

}  // namespace lims::db
