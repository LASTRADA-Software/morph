// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"

/// @file
/// `InvoiceModel`: the billable lines published results make, and (as the module grows) the invoices that collect
/// them.

namespace lims {

/// @brief What a company has earned and not yet invoiced, and the invoices that collect it.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration.
class InvoiceModel {
public:
    /// @brief Turns a published result into a billable line, once per event id.
    ///
    /// Priced at the price in force on the publishing date (`PriceListModel::priceOn`); recorded `Unpriced` with no
    /// amount when there is none, since a consumer that refuses an event stops the relay at that event and every
    /// later event waits behind it. The event id is recorded in the line's own transaction through an
    /// `IReplayLedger`, so a redelivered event answers the line it made.
    /// @param action The event, as billing is told it.
    /// @return The line.
    /// @throws ValidationError if the event names no event id, site, client, sample, result, analysis or date.
    /// @throws Forbidden if the caller is not one of the server's own clients holding the service role at the
    ///         site's company.
    /// @throws NotFound if the site does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillableLineView execute(const RecordBillableLine& action);

    /// @brief Lists the billable lines of the companies the caller can see, filtered by each field the query names.
    /// @param action The filter.
    /// @return The lines by id; none of a company the caller cannot see.
    /// @throws ValidationError if a row holds a state or currency outside its enumeration.
    ListBillableLinesResult execute(const ListBillableLines& action);

    /// @brief Drafts an invoice over every unbilled line of a client at a site, in the client's currency.
    ///
    /// The lines are marked invoiced in the same transaction, so a second draft finds none. Tax is the company's flat
    /// rate on the net total, rounded half-up once.
    /// @param action The company, site and client.
    /// @return The draft, with its lines and totals.
    /// @throws ValidationError if the invoice is not fully named, the site is not the company's, or a line is in
    ///         another currency than the client is billed in.
    /// @throws NotFound if the company or site does not exist, the company has no billing settings, or the client
    ///         is not billed there.
    /// @throws Forbidden if the caller is not a billing clerk at the company.
    /// @throws Conflict if the client has no unbilled line at the site.
    /// @throws OutOfRange if a total does not fit.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const CreateInvoice& action);

    /// @brief Submits a draft invoice for approval.
    ///
    /// Records who submitted it, its gross in the company's currency, and the approval deadline: now plus the
    /// company's approval hours. The state moves by a conditional update, so two clerks submitting one invoice at
    /// once cannot both succeed.
    /// @param action The invoice.
    /// @return The submitted invoice.
    /// @throws ValidationError if no invoice is named, or the invoice is in another currency than its company's.
    /// @throws NotFound if there is no such invoice.
    /// @throws Forbidden if the caller is not a billing clerk at the invoice's company.
    /// @throws IllegalTransition if the invoice is not a draft.
    /// @throws Conflict if the invoice was submitted meanwhile.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const SubmitInvoice& action);

    /// @brief Approves a submitted invoice.
    ///
    /// The approver holds the invoice approver role at the invoice's site before its deadline and at its company
    /// from the deadline on, has a value limit there that covers the invoice's gross, and neither created nor
    /// submitted it. The state moves by a conditional update, so two approvers acting at once cannot both succeed.
    /// @param action The invoice.
    /// @return The approved invoice.
    /// @throws ValidationError if no invoice is named, or the submission recorded no company-currency gross.
    /// @throws NotFound if there is no such invoice.
    /// @throws IllegalTransition if the invoice is not submitted.
    /// @throws Forbidden if the caller may not approve it.
    /// @throws Conflict if the invoice was approved meanwhile.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const ApproveInvoice& action);

    /// @brief Lists the submitted invoices the caller could approve now, judged by the rule `ApproveInvoice` is.
    /// @param action The query.
    /// @return The invoices by id.
    ListInvoicesResult execute(const ListPendingApprovals& action);

    /// @brief Reads one invoice.
    /// @param action The invoice.
    /// @return It, with its lines.
    /// @throws ValidationError if no invoice is named.
    /// @throws NotFound if there is no such invoice, or it is of a company the caller cannot see.
    InvoiceView execute(const GetInvoice& action);

    /// @brief Lists a company's invoices.
    /// @param action The company.
    /// @return Its invoices by id; none when the caller cannot see the company.
    /// @throws ValidationError if no company is named.
    ListInvoicesResult execute(const ListInvoices& action);

    /// @brief Stores the line of an event, answering the line an earlier or concurrent delivery made.
    ///
    /// The seam under `RecordBillableLine` after its checks. Two deliveries of one event race on the unique index of
    /// consumed events: the loser's write fails and rolls back, and it then finds the winner's line and answers it.
    /// This is not an action: it checks nothing, so a caller must already have authorized the event.
    /// @param action The event, already validated and authorized.
    /// @param company The company that bills it.
    /// @param betweenLookupAndWrite Called once the lookup found no earlier delivery and before the write; a test
    ///        delivers the winner here. Empty in production.
    /// @return The line.
    /// @throws Lightweight::SqlException if the write fails for another reason.
    [[nodiscard]] static BillableLineView storeLine(RecordBillableLine const& action, CompanyId company,
                                                    std::function<void()> const& betweenLookupAndWrite);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::InvoiceModel, "InvoiceModel")
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::RecordBillableLine, "RecordBillableLine")
BRIDGE_REGISTER_EFFECTS(lims::RecordBillableLine, ::morph::effects::changes<lims::BillableLineView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::ListBillableLines, "ListBillableLines", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::CreateInvoice, "CreateInvoice")
BRIDGE_REGISTER_EFFECTS(lims::CreateInvoice, ::morph::effects::changes<lims::InvoiceView>,
                        ::morph::effects::changes<lims::BillableLineView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::SubmitInvoice, "SubmitInvoice")
BRIDGE_REGISTER_EFFECTS(lims::SubmitInvoice, ::morph::effects::updates<lims::InvoiceView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::ApproveInvoice, "ApproveInvoice")
BRIDGE_REGISTER_EFFECTS(lims::ApproveInvoice, ::morph::effects::updates<lims::InvoiceView>)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::ListPendingApprovals, "ListPendingApprovals",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::GetInvoice, "GetInvoice", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::ListInvoices, "ListInvoices", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
