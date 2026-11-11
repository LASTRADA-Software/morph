// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/registry.hpp>

#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The inventory module's declared authorization: a company's inventory settings are a supervisor's; its items,
/// locations and opening lots are a stock keeper's, at the company or at the site of the location.

namespace lims {

/// @brief A company's inventory settings are a supervisor's.
template <>
struct Authorization<SetInventorySettings> : CompanyRule<SetInventorySettings, LimsRole::Supervisor> {};

/// @brief An item is created by a stock keeper at its company.
template <>
struct Authorization<CreateItem> : CompanyRule<CreateItem, LimsRole::StockKeeper> {};

/// @brief A location is created by a stock keeper at its site.
template <>
struct Authorization<CreateLocation> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;

    /// @brief The site the payload names.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(CreateLocation const& action) { return Scope{action.site}; }
};

/// @brief An opening lot is recorded by a stock keeper at the location's site.
template <>
struct Authorization<RecordOpeningLot> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;

    /// @brief The site of the location the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such location.
    [[nodiscard]] static Scope scopeOf(RecordOpeningLot const& action);
};

}  // namespace lims
