// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/billing/core/types.hpp"
#include "lims/billing/dto/line_dto.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// Invoices: one client's billable lines at one site, in the client's currency.

namespace lims {

/// @brief An invoice with its lines and totals.
struct InvoiceView {
    /// @brief Its id.
    InvoiceId id;
    /// @brief The company that bills.
    CompanyId company;
    /// @brief The site whose approver judges it.
    SiteId site;
    /// @brief The billed client.
    ClientId client;
    /// @brief The currency of every amount on it.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief Where it stands.
    InvoiceState state = InvoiceState::Draft;
    /// @brief Its number; empty until posted.
    std::string number;
    /// @brief Its lines, by id.
    std::vector<BillableLineView> lines;
    /// @brief The lines' exact sum.
    Amount net;
    /// @brief The flat tax on the net total, rounded half-up once.
    Amount tax;
    /// @brief The net plus the tax.
    Amount gross;
    /// @brief The reference the poster supplies to the ledger; empty until submitted.
    std::string reference;
    /// @brief Who drafted it.
    std::string createdBy;
    /// @brief Who submitted it; empty until submitted.
    std::string submittedBy;
    /// @brief Who approved it; empty until approved.
    std::string approvedBy;
    /// @brief Who posted it; empty until posted.
    std::string postedBy;
    /// @brief When it was submitted; empty until then.
    ::morph::time::Timestamp submittedAt;
    /// @brief When its approval escalates to the company; empty until submitted.
    ::morph::time::Timestamp approvalDeadline;
    /// @brief Who must approve it: `Company` once the deadline has passed.
    ApprovalLevel level = ApprovalLevel::Site;
    /// @brief Its journal entry; empty until posted.
    JournalEntryId entry;
};

/// @brief Drafts an invoice over every unbilled line of a client at a site.
struct CreateInvoice {
    /// @brief The company that bills.
    CompanyId company;
    /// @brief The site whose lines are collected.
    SiteId site;
    /// @brief The client.
    ClientId client;

    /// @brief Whether the invoice is fully named.
    /// @return `true` when the company, site and client are.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue() && site.hasValue() && client.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Reads one invoice.
struct GetInvoice {
    /// @brief The invoice.
    InvoiceId invoiceId;

    /// @brief Whether an invoice is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return invoiceId.hasValue(); }
};

/// @brief Lists a company's invoices.
struct ListInvoices {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The invoices, by id.
struct ListInvoicesResult {
    /// @brief The invoices.
    std::vector<InvoiceView> invoices;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::InvoiceView, "Billing_Invoice")
