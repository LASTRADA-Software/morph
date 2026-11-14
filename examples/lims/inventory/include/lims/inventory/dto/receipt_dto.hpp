// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "lims/inventory/core/types.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// Goods receipts: a delivery against an open purchase order, as the wire sees it. Each receipt line becomes a lot.

namespace lims {

/// @brief One line of a delivery: a new lot of the goods an order line asked for.
struct ReceiptLineInput {
    /// @brief The order line the goods are delivered against.
    OrderLineId orderLine;
    /// @brief The supplier's lot number, 1 to 32 bytes; new for the item.
    std::string lotNumber;
    /// @brief When the lot expires.
    ::morph::time::Timestamp expiresAt;
    /// @brief How much arrived, in @p unit.
    Measure quantity;
    /// @brief The unit @p quantity is in: any unit of the item's dimension.
    StockUnit unit = StockUnit::piece;
};

/// @brief Takes delivery against an open order: creates the lots, moves the order's lines, numbers the receipt and has
///        the ledger record its value against goods received not invoiced.
struct ReceiveGoods {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The order.
    PurchaseOrderId order;
    /// @brief Where the lots are kept: a location at the order's site.
    LocationId location;
    /// @brief The receipt's date: it picks the exchange rate, the ledger's period and the number's fiscal year.
    ::morph::time::Timestamp date;
    /// @brief The lines; one order line may arrive as several lots.
    std::vector<ReceiptLineInput> lines;

    /// @brief Whether the delivery is well-formed.
    /// @return `true` when an order, a location, a date and at least one line are named, and each line names an order
    ///         line, a lot number of 1 to 32 bytes, an expiry and a quantity, and no lot number is named twice.
    [[nodiscard]] bool validate() const noexcept {
        if (!order.hasValue() || !location.hasValue() || !date.hasValue() || lines.empty()) {
            return false;
        }
        std::unordered_set<std::string> numbers;
        for (ReceiptLineInput const& line : lines) {
            if (!line.orderLine.hasValue() || line.lotNumber.empty() || line.lotNumber.size() > 32 ||
                !line.expiresAt.hasValue() || !line.quantity.hasValue() || !numbers.insert(line.lotNumber).second) {
                return false;
            }
        }
        return true;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief A goods receipt.
struct ReceiptView {
    /// @brief Its id.
    ReceiptId id;
    /// @brief Its number, `GR-<fiscal year>-<six digits>`: gap-free per company and fiscal year.
    std::string number;
    /// @brief The order it was received against.
    PurchaseOrderId order;
    /// @brief The company.
    CompanyId company;
    /// @brief The order's currency: the supplier's.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The lines' exact sum, in @p currency.
    Amount value;
    /// @brief The value in the company's currency, as the ledger carries it against goods received not invoiced.
    Amount companyValue;
    /// @brief The lots it created, in the order of its lines.
    std::vector<LotId> lots;
    /// @brief The ledger's entry.
    JournalEntryId entry;
    /// @brief The receipt's date.
    ::morph::time::Timestamp date;
    /// @brief Who received the goods.
    std::string receivedBy;
};

/// @brief Reads a goods receipt.
struct GetReceipt {
    /// @brief The receipt.
    ReceiptId receiptId;

    /// @brief Whether a receipt is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return receiptId.hasValue(); }
};

/// @brief Lists a company's goods receipts.
struct ListReceipts {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A list of goods receipts, by id.
struct ListReceiptsResult {
    /// @brief The receipts.
    std::vector<ReceiptView> receipts;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::ReceiptView, "Inventory_Receipt")
