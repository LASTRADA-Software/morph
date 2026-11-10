// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

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

    /// @brief Stores the line of an event, answering the line an earlier or concurrent delivery made.
    ///
    /// The seam under `RecordBillableLine` after its checks. Two deliveries of one event race on the unique index of
    /// consumed events: the loser's write fails and rolls back, and it then finds the winner's line and answers it.
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
// NOLINTEND(misc-const-correctness)
