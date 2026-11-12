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

/// @brief One requisition: a request to buy, raised at a site and approved by a purchase approver.
struct RequisitionRecord {
    static constexpr std::string_view TableName = "inventory_requisitions";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"reason"}> reason;                          // 3
    /// @brief `RequisitionState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                                 // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"requested_by"}> requestedBy;  // 5
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"submitted_by"}> submittedBy;  // 6
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"approved_by"}> approvedBy;    // 7
    /// @brief When it was raised, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};  // 8
    /// @brief When it was submitted, in epoch milliseconds; 0 until then.
    Light::Field<std::int64_t, Light::SqlRealName{"submitted_at"}> submittedAt{0};  // 9
    /// @brief When its approval escalates to the company, in epoch milliseconds; 0 until it is submitted.
    Light::Field<std::int64_t, Light::SqlRealName{"deadline_at"}> deadlineAt{0};  // 10
    /// @brief The sum of its lines' estimates, in the company's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"total_num"}> totalNum{0};  // 11
    Light::Field<std::int64_t, Light::SqlRealName{"total_den"}> totalDen{1};  // 12
    /// @brief `CurrencyCode` of the company's base currency when the requisition was raised.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 13
    /// @brief The purchase order it became; empty until ordered.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"order_id"}> orderId;  // 14
};

/// @brief One line of a requisition: an item, a quantity in the item's unit and an estimated total.
struct RequisitionLineRecord {
    static constexpr std::string_view TableName = "inventory_requisition_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"requisition_id"}> requisitionId{0};                     // 1
    Light::Field<std::int64_t, Light::SqlRealName{"item_id"}> itemId{0};                                   // 2
    /// @brief The quantity, in the item's unit.
    Light::Field<std::int64_t, Light::SqlRealName{"quantity_num"}> quantityNum{0};  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"quantity_den"}> quantityDen{1};  // 4
    /// @brief The estimated line total, in the company's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"estimate_num"}> estimateNum{0};  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"estimate_den"}> estimateDen{1};  // 6
};

/// @brief One supplier of a company, with the currency its orders are priced in.
struct SupplierRecord {
    static constexpr std::string_view TableName = "inventory_suppliers";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 3
};

/// @brief One purchase order: an approved requisition, ordered from a supplier in its currency.
struct PurchaseOrderRecord {
    static constexpr std::string_view TableName = "inventory_orders";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 2
    Light::Field<std::int64_t, Light::SqlRealName{"supplier_id"}> supplierId{0};                           // 3
    Light::Field<std::int64_t, Light::SqlRealName{"requisition_id"}> requisitionId{0};                     // 4
    /// @brief `CurrencyCode` of the supplier when the order was placed.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 5
    /// @brief `OrderState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};  // 6
    /// @brief The sum of its lines' totals, in the order's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"total_num"}> totalNum{0};             // 7
    Light::Field<std::int64_t, Light::SqlRealName{"total_den"}> totalDen{1};             // 8
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"ordered_by"}> orderedBy;  // 9
    /// @brief When it was placed, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"ordered_at"}> orderedAt{0};  // 10
};

/// @brief One line of a purchase order: an item, a quantity in the item's unit and a price per unit.
struct OrderLineRecord {
    static constexpr std::string_view TableName = "inventory_order_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"order_id"}> orderId{0};                                 // 1
    Light::Field<std::int64_t, Light::SqlRealName{"item_id"}> itemId{0};                                   // 2
    /// @brief The quantity ordered, in the item's unit.
    Light::Field<std::int64_t, Light::SqlRealName{"ordered_num"}> orderedNum{0};  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"ordered_den"}> orderedDen{1};  // 4
    /// @brief The quantity received, in the item's unit; zero until a receipt.
    Light::Field<std::int64_t, Light::SqlRealName{"received_num"}> receivedNum{0};  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"received_den"}> receivedDen{1};  // 6
    /// @brief The price of one stock unit, in the order's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"unit_price_num"}> unitPriceNum{0};  // 7
    Light::Field<std::int64_t, Light::SqlRealName{"unit_price_den"}> unitPriceDen{1};  // 8
    /// @brief The line total, in the order's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"line_total_num"}> lineTotalNum{0};  // 9
    Light::Field<std::int64_t, Light::SqlRealName{"line_total_den"}> lineTotalDen{1};  // 10
};

/// @brief One goods receipt: a delivery against a purchase order, with its number and the ledger's entry.
struct ReceiptRecord {
    static constexpr std::string_view TableName = "inventory_receipts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"order_id"}> orderId{0};                                 // 2
    /// @brief `GR-<fiscal year>-<six digits>`, unique within the company.
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"number"}> number;  // 3
    /// @brief `CurrencyCode` of the order.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 4
    /// @brief The value in the order's currency.
    Light::Field<std::int64_t, Light::SqlRealName{"value_num"}> valueNum{0};  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"value_den"}> valueDen{1};  // 6
    /// @brief The value in the company's currency, as the ledger carries it.
    Light::Field<std::int64_t, Light::SqlRealName{"company_value_num"}> companyValueNum{0};  // 7
    Light::Field<std::int64_t, Light::SqlRealName{"company_value_den"}> companyValueDen{1};  // 8
    /// @brief The ledger's journal entry.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"entry_id"}> entryId;  // 9
    /// @brief The receipt's date, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"received_at"}> receivedAt{0};           // 10
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"received_by"}> receivedBy;  // 11
};

/// @brief One lot a receipt created, and the order line it was delivered against.
struct ReceiptLotRecord {
    static constexpr std::string_view TableName = "inventory_receipt_lots";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"receipt_id"}> receiptId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"lot_id"}> lotId{0};                                     // 2
    Light::Field<std::int64_t, Light::SqlRealName{"order_line_id"}> orderLineId{0};                        // 3
};

/// @brief The next receipt number of a company's fiscal year.
struct ReceiptNumberRecord {
    static constexpr std::string_view TableName = "inventory_receipt_numbers";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"fiscal_year"}> fiscalYear{0};                           // 2
    /// @brief The number the next receipt takes.
    Light::Field<std::int64_t, Light::SqlRealName{"next"}> next{1};  // 3
};

}  // namespace lims::db
