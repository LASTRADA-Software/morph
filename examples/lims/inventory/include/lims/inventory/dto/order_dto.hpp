// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/inventory/core/types.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Suppliers and purchase orders: an approved requisition becomes one order to one supplier, priced in the supplier's
/// currency, as the wire sees them.

namespace lims {

/// @brief Registers a supplier of a company, with the currency it invoices in.
struct CreateSupplier {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company.
    CompanyId company;
    /// @brief The supplier's name, unique within the company.
    std::string name;
    /// @brief The currency every order to this supplier is priced in.
    CurrencyCode currency = CurrencyCode::EUR;

    /// @brief Whether the supplier is well-formed.
    /// @return `true` when a company and a name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue() && !name.empty() && name.size() <= 128; }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief A supplier.
struct SupplierView {
    /// @brief Its id.
    SupplierId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The supplier's name.
    std::string name;
    /// @brief The currency every order to it is priced in.
    CurrencyCode currency = CurrencyCode::EUR;
};

/// @brief Lists a company's suppliers.
struct ListSuppliers {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A list of suppliers, by id.
struct ListSuppliersResult {
    /// @brief The suppliers.
    std::vector<SupplierView> suppliers;
};

/// @brief The price of one requisition line, per stock unit, in the supplier's currency.
struct OrderLinePrice {
    /// @brief The requisition line.
    RequisitionLineId line;
    /// @brief The price of one stock unit of the line's item.
    Amount unitPrice;
};

/// @brief Orders an approved requisition from a supplier, pricing every one of its lines.
struct CreatePurchaseOrder {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The approved requisition.
    RequisitionId requisition;
    /// @brief The supplier, of the requisition's company; the order is in its currency.
    SupplierId supplier;
    /// @brief One price for each line of the requisition.
    std::vector<OrderLinePrice> prices;

    /// @brief Whether the order is well-formed.
    /// @return `true` when a requisition, a supplier and a price for each line named are given; whether the lines
    ///         named are the requisition's is the model's to judge.
    [[nodiscard]] bool validate() const noexcept {
        if (!requisition.hasValue() || !supplier.hasValue()) {
            return false;
        }
        for (OrderLinePrice const& price : prices) {
            if (!price.line.hasValue() || !price.unitPrice.hasValue()) {
                return false;
            }
        }
        return true;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One line of a purchase order.
struct OrderLineView {
    /// @brief Its id.
    OrderLineId id;
    /// @brief The item.
    ItemId item;
    /// @brief The item's code.
    std::string itemCode;
    /// @brief How much was ordered, in @p unit.
    Measure ordered;
    /// @brief How much has been received, in @p unit; zero until a receipt.
    Measure received;
    /// @brief The item's unit.
    StockUnit unit = StockUnit::piece;
    /// @brief The price of one @p unit, in the order's currency.
    Amount unitPrice;
    /// @brief The quantity times the unit price, rounded half-up once to the currency's decimals.
    Amount lineTotal;
};

/// @brief A purchase order.
struct PurchaseOrderView {
    /// @brief Its id.
    PurchaseOrderId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The site that raised the requisition.
    SiteId site;
    /// @brief The supplier.
    SupplierId supplier;
    /// @brief The supplier's name.
    std::string supplierName;
    /// @brief The requisition it was made from.
    RequisitionId requisition;
    /// @brief The currency of every amount on it: the supplier's.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief Where it stands.
    OrderState state = OrderState::Open;
    /// @brief Its lines, by id.
    std::vector<OrderLineView> lines;
    /// @brief The lines' exact sum.
    Amount total;
    /// @brief Who placed it.
    std::string orderedBy;
    /// @brief When it was placed.
    ::morph::time::Timestamp orderedAt;
};

/// @brief Reads a purchase order.
struct GetPurchaseOrder {
    /// @brief The order.
    PurchaseOrderId orderId;

    /// @brief Whether an order is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return orderId.hasValue(); }
};

/// @brief Lists a company's purchase orders.
struct ListPurchaseOrders {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A list of purchase orders, by id.
struct ListPurchaseOrdersResult {
    /// @brief The orders.
    std::vector<PurchaseOrderView> orders;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::SupplierView, "Inventory_Supplier")
BRIDGE_REGISTER_RECORD(lims::PurchaseOrderView, "Inventory_Order")
