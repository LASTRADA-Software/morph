// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>

#include "lims/billing/core/types.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The edit lease of a draft invoice and the edits it guards.

namespace lims {

/// @brief Who may edit a draft invoice now.
struct InvoiceLeaseView {
    /// @brief The invoice.
    InvoiceId invoice;
    /// @brief Who holds the lease; empty when the invoice is free.
    std::string holder;
    /// @brief When the lease lapses unless renewed; empty when the invoice is free.
    ::morph::time::Timestamp expiresAt;
};

/// @brief Leases a draft invoice to the editing instance that sends it.
struct LeaseInvoice {
    /// @brief The invoice.
    InvoiceId invoiceId;

    /// @brief Whether an invoice is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return invoiceId.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Gives the editing instance's lease up.
struct ReleaseInvoice {
    /// @brief Whether the action is well-formed.
    /// @return Always `true`: it names nothing.
    [[nodiscard]] bool validate() const noexcept { return true; }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Takes a line off the leased draft invoice.
struct RemoveInvoiceLine {
    /// @brief The line.
    BillableLineId line;

    /// @brief Whether a line is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return line.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Sets the client reference of the leased draft invoice.
struct SetInvoiceReference {
    /// @brief The reference, at most `kMaxReferenceBytes` bytes.
    std::string reference;

    /// @brief The longest reference the invoice stores.
    static constexpr std::size_t kMaxReferenceBytes = 128;

    /// @brief Whether the reference fits the invoice's column.
    /// @return `true` when it is at most `kMaxReferenceBytes` bytes.
    [[nodiscard]] bool validate() const noexcept { return reference.size() <= kMaxReferenceBytes; }
};

/// @brief Reads who holds a draft invoice's lease.
struct GetInvoiceLease {
    /// @brief The invoice.
    InvoiceId invoiceId;

    /// @brief Whether an invoice is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return invoiceId.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::InvoiceLeaseView, "Billing_Lease", ::morph::record::key<&lims::InvoiceLeaseView::invoice>)
