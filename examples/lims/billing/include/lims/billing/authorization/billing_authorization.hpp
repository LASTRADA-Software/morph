// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/billing/dto/price_dto.hpp"
#include "lims/billing/dto/settings_dto.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The billing module's declared authorization: a company's billing settings are a supervisor's; its clients, price
/// lists and prices are a billing clerk's, at that company.

namespace lims {

/// @brief A company's billing settings are a supervisor's.
template <>
struct Authorization<SetBillingSettings> : CompanyRule<SetBillingSettings, LimsRole::Supervisor> {};

/// @brief A client is registered for billing by a billing clerk at the company.
template <>
struct Authorization<RegisterBillingClient> : CompanyRule<RegisterBillingClient, LimsRole::BillingClerk> {};

/// @brief A price list is created by a billing clerk at its company.
template <>
struct Authorization<CreatePriceList> : CompanyRule<CreatePriceList, LimsRole::BillingClerk> {};

/// @brief A price is set by a billing clerk at the price list's company.
template <>
struct Authorization<SetPrice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the price list the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such list.
    [[nodiscard]] static Scope scopeOf(SetPrice const& action);
};

}  // namespace lims
