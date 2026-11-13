// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/dto/order_dto.hpp"
#include "lims/inventory/dto/receipt_dto.hpp"
#include "lims/inventory/dto/requisition_dto.hpp"
#include "lims/inventory/dto/reservation_dto.hpp"
#include "lims/inventory/dto/stock_dto.hpp"

/// @file
/// Inventory's ORM-level reads and writes shared by its models. Inventory-internal: no model header includes it, so a
/// consumer of a model does not pull in the ORM.

namespace lims::inventory {

/// @brief Executes @p sql, a statement a query builder made, and answers how many rows it changed.
///
/// Every transition in inventory is a conditional update: the caller `Where`s the id and every column it read, and a
/// count other than one is a `Conflict`, so two callers acting on one read cannot both succeed.
/// @param mapper The caller's mapper, inside its transaction.
/// @param sql The statement.
/// @return The rows it changed.
[[nodiscard]] std::size_t changedRows(Lightweight::DataMapper& mapper, std::string const& sql);

/// @brief Moves requisition @p id from @p from by a conditional update, and counts the rows it changed.
///
/// A fault point sits before the write, so a test makes a concurrent action win it.
/// @tparam Set Callable adding the columns to change to the update's builder.
/// @tparam Guard Callable adding further columns the caller read and requires unchanged.
/// @param mapper The caller's mapper, inside its transaction.
/// @param id The requisition.
/// @param from The state the caller read.
/// @param set Adds the columns to change.
/// @param guard Adds the further conditions.
/// @return Whether exactly one row matched.
template <typename Set, typename Guard>
[[nodiscard]] bool moveRequisition(Lightweight::DataMapper& mapper, std::int64_t id, RequisitionState from,
                                   Set const& set, Guard const& guard) {
    inventoryFaultPoint("requisition.beforeStateChange", mapper.Connection());
    auto query = mapper.Connection().Query(db::RequisitionRecord::TableName).Update();
    set(query);
    (void)query.Where("id", "=", id).Where("state", "=", static_cast<int>(from));
    guard(query);
    return changedRows(mapper, query.ToSql()) == 1U;
}

/// @brief Writes one movement and moves its lot's stock on hand by it, through the caller's transaction.
///
/// The lot's update is conditional on the stock on hand the caller read, so a concurrent movement on the same lot
/// makes this one a `Conflict` rather than a lost update.
/// @param mapper The caller's mapper, inside its transaction.
/// @param lot The lot as the caller read it.
/// @param kind The movement's kind.
/// @param quantity The change in the item's unit: positive in, negative out.
/// @param source What caused it.
/// @param principal Who.
/// @param eventId The event that caused it, when one did; the movement is found by it afterwards.
/// @return The lot's new stock on hand.
/// @throws Conflict if the lot changed since it was read.
/// @throws ValidationError if the movement would take the lot below zero.
/// @throws OutOfRange if the new stock on hand does not fit.
::morph::math::Rational moveLot(Lightweight::DataMapper& mapper, db::LotRecord const& lot, MovementKind kind,
                                ::morph::math::Rational const& quantity, std::string const& source,
                                std::string const& principal, std::optional<std::string> const& eventId = {});

/// @brief The view of a movement.
/// @param row The movement's row.
/// @param unit The unit of the movement's item.
/// @return The view.
/// @throws ValidationError if the row stores a kind outside `MovementKind`.
[[nodiscard]] StockMovementView movementView(db::MovementRecord const& row, StockUnit unit);

/// @brief The live reservations on lot @p lot: those whose expiry has not passed.
/// @param mapper The mapper to read through.
/// @param lot The lot.
/// @return The rows, oldest first.
[[nodiscard]] std::vector<db::ReservationRecord> liveReservationsOn(Lightweight::DataMapper& mapper, std::int64_t lot);

/// @brief The live reservations of sample @p sample.
/// @param mapper The mapper to read through.
/// @param sample The sample.
/// @return The rows, oldest first.
[[nodiscard]] std::vector<db::ReservationRecord> liveReservationsOfSample(Lightweight::DataMapper& mapper,
                                                                          std::int64_t sample);

/// @brief The quantity of lot @p lot held by live reservations of samples other than @p exceptSample.
/// @param mapper The mapper to read through.
/// @param lot The lot.
/// @param exceptSample A sample whose reservations do not count; 0 for none.
/// @return The quantity held, in the item's unit.
/// @throws OutOfRange if the sum does not fit.
[[nodiscard]] ::morph::math::Rational reservedOn(Lightweight::DataMapper& mapper, std::int64_t lot,
                                                 std::int64_t exceptSample);

/// @brief What of lot @p lot is free to hold or take for @p exceptSample: its stock on hand less every live
///        reservation of another sample.
/// @param mapper The mapper to read through.
/// @param lot The lot's row.
/// @param exceptSample The sample the stock is for; its own reservations do not count against it. 0 for none.
/// @return The available quantity, in the item's unit.
/// @throws OutOfRange if a sum does not fit.
[[nodiscard]] ::morph::math::Rational availableOf(Lightweight::DataMapper& mapper, db::LotRecord const& lot,
                                                  std::int64_t exceptSample);

/// @brief The view of the reservation @p rows hold for sample @p sample.
/// @param mapper The mapper to read through.
/// @param sample The sample.
/// @param rows Its live reservation rows; none gives a view with no holder.
/// @return The view.
/// @throws NotFound if a row's lot or item does not exist.
/// @throws ValidationError if an item stores a unit outside its enumeration.
[[nodiscard]] ReservationView reservationView(Lightweight::DataMapper& mapper, std::int64_t sample,
                                              std::vector<db::ReservationRecord> const& rows);

/// @brief The view of a lot, with its live reserved quantity.
/// @param mapper The mapper to read through.
/// @param lot The lot's row.
/// @return The view.
/// @throws ValidationError if a column holds a value outside its enumeration.
/// @throws NotFound if the lot's item or company does not exist.
[[nodiscard]] LotStockView lotView(Lightweight::DataMapper& mapper, db::LotRecord const& lot);

/// @brief The row of item @p item.
/// @param mapper The mapper to read through.
/// @param item The item.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::ItemRecord> findItem(Lightweight::DataMapper& mapper, std::int64_t item);

/// @brief The row of location @p location.
/// @param mapper The mapper to read through.
/// @param location The location.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::LocationRecord> findLocation(Lightweight::DataMapper& mapper, std::int64_t location);

/// @brief The unit an item row stores.
/// @param item The row.
/// @return The unit.
/// @throws ValidationError if the row stores a value outside `StockUnit`.
[[nodiscard]] StockUnit stockUnitOf(db::ItemRecord const& item);

/// @brief Whether a lot of @p item already carries @p lotNumber, read on a mapper of its own.
/// @param item The item.
/// @param lotNumber The supplier's lot number.
/// @return `true` when one does.
[[nodiscard]] bool lotNumberTaken(std::int64_t item, std::string const& lotNumber);

/// @brief Creates a lot of @p item at @p location holding nothing yet, through the caller's transaction.
///
/// The unique index on (item, lot number) refuses a second lot of the number; any other failure finds no such lot and
/// surfaces as it is.
/// @param mapper The caller's mapper, inside its transaction.
/// @param action The action's name, for the refusal.
/// @param item The item's row.
/// @param location The location's row.
/// @param lotNumber The supplier's lot number.
/// @param expiresAt When the lot expires.
/// @param unitCost The cost per stock unit, in the company's currency.
/// @return The lot's row, with its id.
/// @throws Conflict if a lot of the item carries the number already.
[[nodiscard]] db::LotRecord createLot(Lightweight::DataMapper& mapper, char const* action, db::ItemRecord const& item,
                                      db::LocationRecord const& location, std::string const& lotNumber,
                                      ::morph::time::Timestamp const& expiresAt,
                                      ::morph::math::Rational const& unitCost);

/// @brief The view of receipt @p receipt, with the lots it created.
/// @param mapper The mapper to read through.
/// @param receipt The receipt.
/// @return The view.
/// @throws NotFound if there is no such receipt or its company does not exist.
/// @throws ValidationError if a column holds a value outside its enumeration.
[[nodiscard]] ReceiptView receiptView(Lightweight::DataMapper& mapper, std::int64_t receipt);

/// @brief The row of lot @p lot.
/// @param mapper The mapper to read through.
/// @param lot The lot.
/// @return It.
/// @throws NotFound if there is no such lot.
[[nodiscard]] db::LotRecord lotRecord(Lightweight::DataMapper& mapper, std::int64_t lot);

/// @brief The row of requisition @p requisition.
/// @param mapper The mapper to read through.
/// @param requisition The requisition.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::RequisitionRecord> findRequisition(Lightweight::DataMapper& mapper,
                                                                   std::int64_t requisition);

/// @brief The row of requisition @p requisition, read on a mapper of its own so the caller's transaction opens with
///        its write.
/// @param requisition The requisition.
/// @param action The action's name, for the refusal.
/// @return The row.
/// @throws NotFound if there is no such requisition, or it is of a company the caller cannot see; the words are the
///         same for both.
[[nodiscard]] db::RequisitionRecord visibleRequisition(RequisitionId requisition, char const* action);

/// @brief The row of supplier @p supplier.
/// @param mapper The mapper to read through.
/// @param supplier The supplier.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::SupplierRecord> findSupplier(Lightweight::DataMapper& mapper, std::int64_t supplier);

/// @brief The row of the purchase order made from requisition @p requisition.
/// @param mapper The mapper to read through.
/// @param requisition The requisition.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::PurchaseOrderRecord> findOrderOf(Lightweight::DataMapper& mapper,
                                                                 std::int64_t requisition);

/// @brief The view of supplier @p row.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a currency outside its enumeration.
[[nodiscard]] SupplierView supplierView(db::SupplierRecord const& row);

/// @brief The view of purchase order @p order, with its lines and each line's item code.
/// @param mapper The mapper to read through.
/// @param order The order.
/// @return The view.
/// @throws NotFound if there is no such order, or its supplier or a line's item does not exist.
/// @throws ValidationError if a column holds a value outside its enumeration.
[[nodiscard]] PurchaseOrderView orderView(Lightweight::DataMapper& mapper, std::int64_t order);

/// @brief Whether a requisition's approval has escalated from its site to its company.
/// @param requisition The row.
/// @return `true` from its deadline on; a draft has no deadline yet.
[[nodiscard]] bool requisitionEscalated(db::RequisitionRecord const& requisition);

/// @brief The view of requisition @p requisition, with its lines and each line's item code.
/// @param mapper The mapper to read through.
/// @param requisition The requisition.
/// @return The view.
/// @throws NotFound if there is no such requisition, or a line's item does not exist.
/// @throws ValidationError if a column holds a value outside its enumeration.
[[nodiscard]] RequisitionView requisitionView(Lightweight::DataMapper& mapper, std::int64_t requisition);

}  // namespace lims::inventory
