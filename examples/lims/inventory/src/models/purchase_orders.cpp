// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return Whether a role of theirs reaches it.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

/// @brief The supplier of @p company called @p name.
/// @param mapper The mapper to read through.
/// @param company The company.
/// @param name The supplier's name.
/// @return The row, or nothing.
[[nodiscard]] std::optional<db::SupplierRecord> supplierByName(Lightweight::DataMapper& mapper, CompanyId company,
                                                               std::string const& name) {
    auto rows = mapper.Query<db::SupplierRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::SupplierRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::SupplierRecord::name>, "=", name)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

/// @brief One requisition line with the price the caller gave it and the total that price makes.
struct PricedLine {
    /// @brief The requisition line.
    RequisitionLineView const* line;
    /// @brief The price of one stock unit.
    Rational unitPrice;
    /// @brief The line total, rounded to the currency.
    Rational total;
};

/// @brief Prices every line of @p requisition once, in @p currency.
/// @param requisition The requisition.
/// @param prices The prices given.
/// @param currency The supplier's currency.
/// @return The lines in the requisition's order, with their totals.
/// @throws ValidationError if a line is priced twice, not at all or is not the requisition's, or a price is not
///         positive or not a whole number of the currency's minor units.
/// @throws OutOfRange if a line total does not fit.
[[nodiscard]] std::vector<PricedLine> priceLines(RequisitionView const& requisition,
                                                 std::vector<OrderLinePrice> const& prices, CurrencyCode currency) {
    std::map<std::int64_t, Rational> byLine;
    for (OrderLinePrice const& price : prices) {
        bool const known = std::ranges::any_of(requisition.lines,
                                               [&](RequisitionLineView const& line) { return line.id == price.line; });
        if (!known) {
            throw ValidationError{"CreatePurchaseOrder: line " + std::to_string(*price.line) +
                                  " is not a line of requisition " + std::to_string(*requisition.id)};
        }
        if (!byLine.emplace(*price.line, *price.unitPrice).second) {
            throw ValidationError{"CreatePurchaseOrder: line " + std::to_string(*price.line) + " is priced twice"};
        }
    }
    std::vector<PricedLine> priced;
    for (RequisitionLineView const& line : requisition.lines) {
        auto const found = byLine.find(*line.id);
        if (found == byLine.end()) {
            throw ValidationError{"CreatePurchaseOrder: line " + std::to_string(*line.id) + " has no unit price"};
        }
        Rational const& unitPrice = found->second;
        if (unitPrice.numerator <= 0) {
            throw ValidationError{"CreatePurchaseOrder: the unit price of line " + std::to_string(*line.id) +
                                  " must be positive"};
        }
        if (!fitsCurrency(unitPrice, currency)) {
            throw ValidationError{"CreatePurchaseOrder: a unit price of " +
                                  ::morph::units::toDecimalString(Amount{unitPrice}) + " is not a whole number of " +
                                  std::string{currencyName(currency)}};
        }
        priced.push_back(PricedLine{
            .line = &line, .unitPrice = unitPrice, .total = orderLineTotal(*line.quantity, unitPrice, currency)});
    }
    return priced;
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
SupplierView ProcurementModel::execute(const CreateSupplier& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateSupplier: a company and a name of 1 to 128 bytes are required"};
    }
    if (!OrgModel::findCompany(action.company).has_value()) {
        throw NotFound{"CreateSupplier: company " + std::to_string(*action.company) + " does not exist"};
    }
    auto const conflict = [&] {
        return Conflict{"CreateSupplier: company " + std::to_string(*action.company) + " has a supplier " +
                        action.name + " already"};
    };
    // The lookups run on a mapper of their own, outside the write's transaction.
    {
        Lightweight::DataMapper lookup;
        if (supplierByName(lookup, action.company, action.name).has_value()) {
            throw conflict();
        }
    }
    try {
        Lightweight::DataMapper mapper;
        inventoryFaultPoint("supplier.beforeInsert", mapper.Connection());
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        db::SupplierRecord row;
        row.companyId = *action.company;
        row.name = Lightweight::SqlAnsiString<128>{action.name};
        row.currency = static_cast<int>(action.currency);
        mapper.Create(row);
        sqlTxn.Commit();
        return inventory::supplierView(row);
    } catch (Lightweight::SqlException const&) {
        // The unique index refused the second of two creates that raced past the lookup; the winner's row is there
        // now. A failure that is not that race finds no such row and surfaces as it is.
        Lightweight::DataMapper lookup;
        if (supplierByName(lookup, action.company, action.name).has_value()) {
            throw conflict();
        }
        throw;
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListSuppliersResult ProcurementModel::execute(const ListSuppliers& action) {
    if (!action.validate()) {
        throw ValidationError{"ListSuppliers: a company is required"};
    }
    ListSuppliersResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::SupplierRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::SupplierRecord::companyId>, "=", *action.company)
                    .All();
    std::ranges::sort(rows, {}, [](db::SupplierRecord const& row) { return row.id.Value(); });
    for (auto const& row : rows) {
        result.suppliers.push_back(inventory::supplierView(row));
    }
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PurchaseOrderView ProcurementModel::execute(const CreatePurchaseOrder& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreatePurchaseOrder: a requisition, a supplier and a price for each line are required"};
    }
    requireRoleSomewhere<CreatePurchaseOrder>();
    auto const requisitionRow = inventory::visibleRequisition(action.requisition, "CreatePurchaseOrder");
    // The reads run on a mapper of their own: the transaction below opens with its first write.
    Lightweight::DataMapper reader;
    auto const supplier = inventory::findSupplier(reader, *action.supplier);
    if (!supplier.has_value() || !visible(CompanyId{supplier->companyId.Value()})) {
        throw NotFound{"CreatePurchaseOrder: supplier " + std::to_string(*action.supplier) + " does not exist"};
    }
    if (supplier->companyId.Value() != requisitionRow.companyId.Value()) {
        throw ValidationError{"CreatePurchaseOrder: supplier " + std::to_string(*action.supplier) +
                              " is not one of requisition " + std::to_string(*action.requisition) + "'s company"};
    }
    // Judged again by the handler so a call that reaches it without the guard is refused too, and just before the
    // write so a role changed since the guard ran is seen. The conditional update below on Approved binds what was
    // judged to what is written.
    requireAuthorized(action);
    if (requisitionRow.state.Value() != static_cast<int>(RequisitionState::Approved)) {
        throw IllegalTransition{"CreatePurchaseOrder: only an approved requisition is ordered"};
    }
    auto const requisition = inventory::requisitionView(reader, *action.requisition);
    auto const supplierRow = inventory::supplierView(*supplier);
    CurrencyCode const currency = supplierRow.currency;
    auto const priced = priceLines(requisition, action.prices, currency);
    ExactSum sum;
    for (PricedLine const& line : priced) {
        if (!sum.add(line.total)) {
            throw OutOfRange{"CreatePurchaseOrder: the order's total does not fit"};
        }
    }
    Rational const total = sum.total();
    std::int64_t const requisitionId = *action.requisition;
    try {
        Lightweight::DataMapper mapper;
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        // The first write: a requisition is ordered once, whoever else read it as approved.
        if (!inventory::moveRequisition(
                mapper, requisitionId, RequisitionState::Approved,
                [](auto& query) { query.Set("state", static_cast<int>(RequisitionState::Ordered)); }, [](auto&) {})) {
            throw Conflict{"CreatePurchaseOrder: requisition " + std::to_string(requisitionId) +
                           " was ordered meanwhile"};
        }
        db::PurchaseOrderRecord order;
        order.companyId = requisitionRow.companyId.Value();
        order.siteId = requisitionRow.siteId.Value();
        order.supplierId = *action.supplier;
        order.requisitionId = requisitionId;
        order.currency = static_cast<int>(currency);
        order.state = static_cast<int>(OrderState::Open);
        order.totalNum = total.numerator;
        order.totalDen = total.denominator;
        order.orderedBy = Lightweight::SqlAnsiString<64>{currentPrincipal()};
        order.orderedAt = nowMillis();
        mapper.Create(order);
        auto const orderId = static_cast<std::int64_t>(order.id.Value());
        for (PricedLine const& pricedLine : priced) {
            db::OrderLineRecord line;
            line.orderId = orderId;
            line.itemId = *pricedLine.line->item;
            line.orderedNum = (*pricedLine.line->quantity).numerator;
            line.orderedDen = (*pricedLine.line->quantity).denominator;
            line.receivedNum = 0;
            line.receivedDen = 1;
            line.unitPriceNum = pricedLine.unitPrice.numerator;
            line.unitPriceDen = pricedLine.unitPrice.denominator;
            line.lineTotalNum = pricedLine.total.numerator;
            line.lineTotalDen = pricedLine.total.denominator;
            mapper.Create(line);
        }
        auto link = mapper.Connection().Query(db::RequisitionRecord::TableName).Update();
        link.Set("order_id", orderId);
        (void)link.Where("id", "=", requisitionId).Where("state", "=", static_cast<int>(RequisitionState::Ordered));
        if (inventory::changedRows(mapper, link.ToSql()) != 1U) {
            throw Conflict{"CreatePurchaseOrder: requisition " + std::to_string(requisitionId) + " changed meanwhile"};
        }
        sqlTxn.Commit();
        return inventory::orderView(mapper, orderId);
    } catch (Lightweight::SqlException const&) {
        // The unique index on the requisition refused the second of two orders that raced past the state read, or the
        // database was busy under the winner's write; either way the requisition is ordered now. A failure that is
        // not that race finds it still approved and surfaces as it is.
        Lightweight::DataMapper reread;
        inventoryFaultPoint("order.afterRollback", reread.Connection());
        auto const now = inventory::findRequisition(reread, requisitionId);
        if (now.has_value() && now->state.Value() == static_cast<int>(RequisitionState::Ordered)) {
            throw Conflict{"CreatePurchaseOrder: requisition " + std::to_string(requisitionId) +
                           " was ordered meanwhile"};
        }
        throw;
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PurchaseOrderView ProcurementModel::execute(const GetPurchaseOrder& action) {
    if (!action.validate()) {
        throw ValidationError{"GetPurchaseOrder: an order is required"};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::PurchaseOrderRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::PurchaseOrderRecord::id>, "=", *action.orderId)
                          .All();
    if (rows.empty() || !visible(CompanyId{rows.front().companyId.Value()})) {
        throw NotFound{"GetPurchaseOrder: order " + std::to_string(*action.orderId) + " does not exist"};
    }
    return inventory::orderView(mapper, *action.orderId);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListPurchaseOrdersResult ProcurementModel::execute(const ListPurchaseOrders& action) {
    if (!action.validate()) {
        throw ValidationError{"ListPurchaseOrders: a company is required"};
    }
    ListPurchaseOrdersResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::PurchaseOrderRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PurchaseOrderRecord::companyId>, "=", *action.company)
                    .All();
    std::ranges::sort(rows, {}, [](db::PurchaseOrderRecord const& row) { return row.id.Value(); });
    for (auto const& row : rows) {
        result.orders.push_back(inventory::orderView(mapper, static_cast<std::int64_t>(row.id.Value())));
    }
    return result;
}

}  // namespace lims
