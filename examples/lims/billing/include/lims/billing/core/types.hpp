// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>

#include "lims/org/core/strong_id.hpp"

/// @file
/// Billing's identities and closed sets.

namespace lims {

LIMS_DEFINE_STRONG_ID(BillingClientId);
LIMS_DEFINE_STRONG_ID(PriceListId);
LIMS_DEFINE_STRONG_ID(PriceId);
LIMS_DEFINE_STRONG_ID(BillableLineId);
LIMS_DEFINE_STRONG_ID(InvoiceId);
LIMS_DEFINE_STRONG_ID(BillingRunId);

/// @brief Where a billable line stands.
enum class BillableLineState : std::uint8_t {
    Unbilled,  ///< Priced and waiting for an invoice.
    Unpriced,  ///< No price was in force for its analysis when it arose.
    Invoiced,  ///< On an invoice.
};

/// @brief Where an invoice stands.
enum class InvoiceState : std::uint8_t {
    Draft,      ///< Being assembled and edited.
    Submitted,  ///< Waiting for approval.
    Approved,   ///< Approved and waiting to be posted.
    Posted,     ///< Numbered and in the ledger.
};

/// @brief Who must approve an invoice.
enum class ApprovalLevel : std::uint8_t {
    Site,     ///< An approver at the invoice's site.
    Company,  ///< An approver at the company.
};

/// @brief Where a billing run stands.
enum class BillingRunState : std::uint8_t {
    Running,   ///< Drafting invoices.
    Finished,  ///< Every client has been considered.
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(BillingClientId);
LIMS_DEFINE_STRONG_ID_WIRE(PriceListId);
LIMS_DEFINE_STRONG_ID_WIRE(PriceId);
LIMS_DEFINE_STRONG_ID_WIRE(BillableLineId);
LIMS_DEFINE_STRONG_ID_WIRE(InvoiceId);
LIMS_DEFINE_STRONG_ID_WIRE(BillingRunId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(BillingClientId, "lims.billingClientId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(PriceListId, "lims.priceListId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(PriceId, "lims.priceId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(BillableLineId, "lims.billableLineId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(InvoiceId, "lims.invoiceId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(BillingRunId, "lims.billingRunId");

/// @brief Reflects `BillableLineState` as its enumerator name.
template <>
struct glz::meta<lims::BillableLineState> {
    using enum lims::BillableLineState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Unbilled, Unpriced, Invoiced);
};

/// @brief Reflects `InvoiceState` as its enumerator name.
template <>
struct glz::meta<lims::InvoiceState> {
    using enum lims::InvoiceState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Draft, Submitted, Approved, Posted);
};

/// @brief Reflects `ApprovalLevel` as its enumerator name.
template <>
struct glz::meta<lims::ApprovalLevel> {
    using enum lims::ApprovalLevel;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Site, Company);
};

/// @brief Reflects `BillingRunState` as its enumerator name.
template <>
struct glz::meta<lims::BillingRunState> {
    using enum lims::BillingRunState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Running, Finished);
};
