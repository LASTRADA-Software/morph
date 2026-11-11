// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/table/query.hpp>

#include "lims/inventory/dto/stock_dto.hpp"

/// @file
/// `StockModel`: a company's inventory settings, items, locations, lots and stock on hand.

namespace lims {

/// @brief What each company keeps in stock, where, and how much.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. An item
/// has one stock unit, chosen when it is created, and every lot of it holds its stock on hand in that unit; a quantity
/// entered in another unit of the same dimension is converted exactly before it is stored. A lot's stock on hand is a
/// column of the lot, changed only by a movement written in the same transaction and conditional on the stock on hand
/// that was read, so a lot's movements always sum to it and two movements on one lot cannot both apply to one read.
class StockModel {
public:
    /// @brief Sets a company's inventory settings, replacing any it had.
    /// @param action The company, the four account codes and the approval hours.
    /// @return The settings.
    /// @throws ValidationError if the request is malformed or the approval hours are not a whole number from 1 to
    /// 8760.
    /// @throws NotFound if the company does not exist.
    /// @throws Forbidden if the caller is not a supervisor at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InventorySettingsView execute(const SetInventorySettings& action);

    /// @brief Reads a company's inventory settings, when the caller can see the company.
    /// @param action The company.
    /// @return The settings.
    /// @throws ValidationError if no company is named.
    /// @throws NotFound if the company has no inventory settings or is not visible to the caller.
    InventorySettingsView execute(const GetInventorySettings& action);

    /// @brief Creates an item.
    /// @param action The company, code, name and unit.
    /// @return The item.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company already has an item with the code.
    /// @throws Forbidden if the caller is not a stock keeper at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ItemView execute(const CreateItem& action);

    /// @brief Lists a company's items, when the caller can see the company.
    /// @param action The company.
    /// @return Its items by code; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named, or a row holds a unit outside its enumeration.
    ListItemsResult execute(const ListItems& action);

    /// @brief Creates a location at a site.
    /// @param action The site and name.
    /// @return The location.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the site does not exist.
    /// @throws Conflict if the site already has a location with the name.
    /// @throws Forbidden if the caller is not a stock keeper at the site.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    LocationView execute(const CreateLocation& action);

    /// @brief Lists a company's locations, when the caller can see the company.
    /// @param action The company.
    /// @return Its locations by site then name; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named.
    ListLocationsResult execute(const ListLocations& action);

    /// @brief Records a lot of stock a company held before lims, posting nothing.
    /// @param action The item, location, lot number, expiry, quantity with its unit, and cost per stock unit.
    /// @return The lot as it stands, its stock on hand in the item's unit.
    /// @throws ValidationError if the request is malformed, the quantity is not positive, the cost is negative or not
    ///         a whole number of the company's minor units, or the quantity's unit is of another dimension than the
    ///         item's.
    /// @throws OutOfRange if the converted quantity does not fit.
    /// @throws NotFound if the item does not exist, or the location does not exist or is not the item's company's.
    /// @throws Conflict if the item already has a lot with the number.
    /// @throws Forbidden if the caller is not a stock keeper at the location's site.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    LotStockView execute(const RecordOpeningLot& action);

    /// @brief Lists the lots of a company, when the caller can see the company.
    /// @param action The company and, optionally, one item.
    /// @return Its lots by expiry then id, with their live reserved quantity; none for a company the caller cannot
    /// see.
    /// @throws ValidationError if no company is named, or a row holds a value outside its enumeration.
    StockOnHandResult execute(const StockOnHand& action);

    /// @brief Lists the movements of an item, through the table engine's query.
    /// @param action The item and the query.
    /// @return The page of movements by id, none of a company the caller cannot see.
    /// @throws ValidationError if no item is named, the query is refused, or a row holds a value outside its
    ///         enumeration.
    ::morph::table::Page<StockMovementView> execute(const ListStockMovements& action);
};

}  // namespace lims

// The guard each registered action names must be complete here: the registration instantiates its execution.
#include "lims/inventory/authorization/inventory_authorization.hpp"

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::StockModel, "StockModel")
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::SetInventorySettings, "SetInventorySettings")
BRIDGE_REGISTER_EFFECTS(lims::SetInventorySettings, ::morph::effects::changes<lims::InventorySettingsView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::GetInventorySettings, "GetInventorySettings",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::CreateItem, "CreateItem")
BRIDGE_REGISTER_EFFECTS(lims::CreateItem, ::morph::effects::changes<lims::ItemView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListItems, "ListItems", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::CreateLocation, "CreateLocation")
BRIDGE_REGISTER_EFFECTS(lims::CreateLocation, ::morph::effects::changes<lims::LocationView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListLocations, "ListLocations", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::RecordOpeningLot, "RecordOpeningLot")
BRIDGE_REGISTER_EFFECTS(lims::RecordOpeningLot, ::morph::effects::changes<lims::LotStockView>,
                        ::morph::effects::changes<lims::StockMovementView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::StockOnHand, "StockOnHand", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListStockMovements, "ListStockMovements", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
