// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>

#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/lease_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"

/// @file
/// `InvoiceEditModel`: one clerk's editing of one draft invoice, guarded by a lease.

namespace lims {

/// @brief One clerk's editing of one draft invoice: the lease and the edits it allows.
///
/// One private instance per editor (a screen registers its own). The instance names its lease with a token of its
/// own, so a second instance of the same principal is another editor. Destroying the instance releases its lease:
/// that is what happens when its screen unmounts, and when the server reclaims the instance because the connection
/// that registered it closed. A lease also lapses fifteen minutes after its last edit, with no timer: an expired
/// lease is free to the next `LeaseInvoice`.
class InvoiceEditModel {
public:
    /// @brief Names this editor with a fresh random token.
    InvoiceEditModel();

    /// @brief Deletes this editor's lease. A failure is logged, never thrown.
    ~InvoiceEditModel();
    InvoiceEditModel(InvoiceEditModel const&) = delete;
    InvoiceEditModel& operator=(InvoiceEditModel const&) = delete;
    InvoiceEditModel(InvoiceEditModel&&) = delete;
    InvoiceEditModel& operator=(InvoiceEditModel&&) = delete;

    /// @brief Leases a draft invoice to the caller, or renews this editor's lease.
    ///
    /// Taking a lease is a conditional write on the row that was read, so two editors taking one free or expired
    /// lease at once give one success and one `Conflict`. An editor that leases a second invoice gives the first up.
    /// @param action The invoice.
    /// @return The lease.
    /// @throws ValidationError if no invoice is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is not a draft.
    /// @throws Conflict if another editor's lease on it is live, naming its holder, or another editor took it first.
    /// @throws Forbidden if the caller is not a billing clerk at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceLeaseView execute(const LeaseInvoice& action);

    /// @brief Gives this editor's lease up.
    /// @param action Carries nothing.
    /// @return The lease as it stands after the release: its holder empty when the invoice is free, the other editor
    ///         when one took over a lapsed lease; empty when this editor held none.
    InvoiceLeaseView execute(const ReleaseInvoice& action);

    /// @brief Takes a line off the leased draft and returns it to unbilled; renews the lease.
    /// @param action The line.
    /// @return The invoice with its totals recomputed.
    /// @throws ValidationError if no line is named.
    /// @throws NotFound if the line is not on an invoice, or on one of a company the caller cannot see.
    /// @throws IllegalTransition if the invoice is not a draft.
    /// @throws Conflict if this editor holds no live lease on the line's invoice, or the line was taken off meanwhile.
    /// @throws OutOfRange if a total does not fit.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const RemoveInvoiceLine& action);

    /// @brief Sets the leased draft's reference; renews the lease.
    /// @param action The reference.
    /// @return The invoice.
    /// @throws ValidationError if the reference is longer than 128 bytes.
    /// @throws IllegalTransition if the leased invoice is not a draft.
    /// @throws Conflict if this editor holds no live lease.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const SetInvoiceReference& action);

private:
    std::string _token;
    std::optional<std::int64_t> _invoice;
};

}  // namespace lims

// The guard each registered action names must be complete here: the registration instantiates its execution.
#include "lims/billing/authorization/billing_authorization.hpp"

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::InvoiceEditModel, "InvoiceEditModel")
BRIDGE_REGISTER_ACTION(lims::InvoiceEditModel, lims::LeaseInvoice, "LeaseInvoice")
BRIDGE_REGISTER_EFFECTS(lims::LeaseInvoice, ::morph::effects::changes<lims::InvoiceLeaseView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceEditModel, lims::ReleaseInvoice, "ReleaseInvoice")
BRIDGE_REGISTER_EFFECTS(lims::ReleaseInvoice, ::morph::effects::changes<lims::InvoiceLeaseView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceEditModel, lims::RemoveInvoiceLine, "RemoveInvoiceLine")
BRIDGE_REGISTER_EFFECTS(lims::RemoveInvoiceLine, ::morph::effects::updates<lims::InvoiceView>,
                        ::morph::effects::changes<lims::BillableLineView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceEditModel, lims::SetInvoiceReference, "SetInvoiceReference")
BRIDGE_REGISTER_EFFECTS(lims::SetInvoiceReference, ::morph::effects::updates<lims::InvoiceView>)
// NOLINTEND(misc-const-correctness)
