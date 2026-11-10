// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"
#include "lims/billing/dto/price_dto.hpp"
#include "lims/billing/dto/run_dto.hpp"
#include "lims/billing/dto/settings_dto.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The billing module's declared authorization: a company's billing settings are a supervisor's; its clients, price
/// lists, prices, draft invoices and billing runs are a billing clerk's, at that company; a billable line is recorded
/// by a service principal, and the billing schedule is run by one.

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

/// @brief An invoice is drafted by a billing clerk at its company.
template <>
struct Authorization<CreateInvoice> : CompanyRule<CreateInvoice, LimsRole::BillingClerk> {};

/// @brief A billing run is started by a billing clerk at its company.
template <>
struct Authorization<StartBillingRun> : CompanyRule<StartBillingRun, LimsRole::BillingClerk> {};

/// @brief A billing run is advanced by a billing clerk at its company.
template <>
struct Authorization<AdvanceBillingRun> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the run the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such run.
    [[nodiscard]] static Scope scopeOf(AdvanceBillingRun const& action);
};

/// @brief The schedule is run by a service principal. It spans companies, so the rule names no scope: the model
///        acts for each company where the caller holds the service role.
template <>
struct Authorization<RunBillingSchedule> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Service;
};

/// @brief A billable line is recorded by a service principal holding the service role at the site's company.
template <>
struct Authorization<RecordBillableLine> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Service;

    /// @brief The site the published sample belongs to.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(RecordBillableLine const& action) { return Scope{action.site}; }
};

}  // namespace lims
