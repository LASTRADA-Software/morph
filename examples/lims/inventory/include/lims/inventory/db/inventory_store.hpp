// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

#include "lims/inventory/core/units.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
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
/// @return The lot's new stock on hand.
/// @throws Conflict if the lot changed since it was read.
/// @throws ValidationError if the movement would take the lot below zero.
/// @throws OutOfRange if the new stock on hand does not fit.
::morph::math::Rational moveLot(Lightweight::DataMapper& mapper, db::LotRecord const& lot, MovementKind kind,
                                ::morph::math::Rational const& quantity, std::string const& source,
                                std::string const& principal);

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

/// @brief The row of lot @p lot.
/// @param mapper The mapper to read through.
/// @param lot The lot.
/// @return It.
/// @throws NotFound if there is no such lot.
[[nodiscard]] db::LotRecord lotRecord(Lightweight::DataMapper& mapper, std::int64_t lot);

}  // namespace lims::inventory
