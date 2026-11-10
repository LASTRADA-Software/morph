// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <morph/core/registry.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/lease_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"
#include "lims/billing/dto/price_dto.hpp"
#include "lims/billing/dto/run_dto.hpp"
#include "lims/billing/dto/settings_dto.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_edit_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The billing module's declared authorization: a company's billing settings are a supervisor's; its clients, price
/// lists, prices, draft invoices, their editing lease, their submission and billing runs are a billing clerk's, at
/// that company; an invoice is approved by an invoice approver within a value limit, who is not the one who created or
/// submitted it; a billable line is recorded by a service principal, and the billing schedule is run by one.

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

/// @brief A draft invoice is leased for editing by a billing clerk at its company. The edits themselves declare no
///        role: they are refused without the editing instance's own live lease, which only this action grants.
template <>
struct Authorization<LeaseInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the invoice the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(LeaseInvoice const& action);
};

/// @brief An invoice is submitted by a billing clerk at its company.
template <>
struct Authorization<SubmitInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the invoice the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(SubmitInvoice const& action);
};

/// @brief An invoice is approved by an invoice approver who neither created nor submitted it and whose value limit
///        covers it: at the invoice's site while its approval is within its deadline, at its company after. The
///        scope moving from site to company at the deadline is the escalation.
template <>
struct Authorization<ApproveInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::InvoiceApprover;

    /// @brief The pairs: whoever created the invoice, and whoever submitted it, may not also approve it.
    static constexpr std::array<SeparatedFrom, 2> separations{
        SeparatedFrom{
            .action = ::morph::model::ActionTraits<CreateInvoice>::typeId(),
            .refusal = "created this invoice and cannot also approve it",
        },
        SeparatedFrom{
            .action = ::morph::model::ActionTraits<SubmitInvoice>::typeId(),
            .refusal = "submitted this invoice and cannot also approve it",
        },
    };

    /// @brief The invoice's site before its deadline, its company from the deadline on.
    /// @param action The action.
    /// @return The scope; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(ApproveInvoice const& action);

    /// @brief The invoice's gross in its company's currency, as its submission recorded it.
    /// @param action The action.
    /// @return The amount; zero when there is no such invoice.
    /// @throws ValidationError when the submission recorded no company-currency gross.
    [[nodiscard]] static ::morph::math::Rational amountOf(ApproveInvoice const& action);

    /// @brief @p principal's value limit at the company of @p scope.
    /// @param principal The principal.
    /// @param scope The scope `scopeOf` gave.
    /// @return The limit, or nothing.
    [[nodiscard]] static std::optional<::morph::math::Rational> limitOf(std::string_view principal,
                                                                        Scope const& scope);

    /// @brief Who created the invoice and who submitted it, in `separations`' order.
    /// @param action The action.
    /// @return The two; each empty when there is no such invoice or no submission.
    [[nodiscard]] static std::array<std::optional<std::string>, 2> firstPerformersOf(ApproveInvoice const& action);
};

/// @brief An approved invoice is posted by a billing clerk at its company.
template <>
struct Authorization<PostInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the invoice the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(PostInvoice const& action);
};

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
