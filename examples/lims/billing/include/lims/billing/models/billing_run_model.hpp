// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"
#include "lims/billing/dto/run_dto.hpp"

/// @file
/// `BillingRunModel`: the monthly billing run, whose cursor is a table of billing's own so that a run interrupted
/// anywhere resumes where it stood.

namespace lims {

/// @brief A company's invoices for one calendar month, drafted item by item.
///
/// A run belongs to a company and a month, at most one per pair. Starting it writes one item per client and site with
/// unbilled lines. Advancing it drafts one item's invoice and marks the item done in one transaction, so an
/// interruption leaves each item either wholly done or wholly pending, and a resumed run drafts only what is
/// pending. Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration.
class BillingRunModel {
public:
    /// @brief Starts the company's run for the month of now.
    /// @param action The company.
    /// @return The run, with every item pending.
    /// @throws ValidationError if no company is named.
    /// @throws NotFound if the company has no billing settings.
    /// @throws Forbidden if the caller is not a billing clerk at the company.
    /// @throws Conflict if the company already ran this month.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillingRunView execute(const StartBillingRun& action);

    /// @brief Drafts up to a batch of the run's pending items, and finishes the run when none is left.
    ///
    /// An item whose lines another draft took in the meantime stays pending and is tried again by the next advance.
    /// An item with no unbilled line left is done, with no invoice.
    /// @param action The run and the batch size.
    /// @return The run as it stands.
    /// @throws ValidationError if the run or the batch is not named, or the batch is not a whole number of at least
    ///         one.
    /// @throws NotFound if the run does not exist.
    /// @throws Forbidden if the caller is not a billing clerk at the run's company.
    /// @throws OutOfRange if an invoice's total does not fit.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillingRunView execute(const AdvanceBillingRun& action);

    /// @brief One tick of the billing schedule: starts the run of every company due and served, then advances every
    ///        unfinished run of the served companies by one batch.
    ///
    /// A company is due once the day of the month has reached its billing day and it has no run for the month. The
    /// caller serves a company where it holds the service role, so the role assignments say what the schedule may do.
    /// @param action The batch size.
    /// @return The runs it advanced, as they stand after the tick.
    /// @throws ValidationError if the batch is not a whole number of at least one.
    /// @throws Forbidden if the caller is not one of the server's own clients holding the service role.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillingScheduleResult execute(const RunBillingSchedule& action);

    /// @brief Lists the runs of a company the caller can see.
    /// @param action The company.
    /// @return Its runs by id; none when the caller cannot see the company.
    /// @throws ValidationError if no company is named, or a row holds a state outside its enumeration.
    ListBillingRunsResult execute(const ListBillingRuns& action);
};

}  // namespace lims

// The guard each registered action names must be complete here: the registration instantiates its execution.
#include "lims/billing/authorization/billing_authorization.hpp"

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::BillingRunModel, "BillingRunModel")
BRIDGE_REGISTER_ACTION(lims::BillingRunModel, lims::StartBillingRun, "StartBillingRun")
BRIDGE_REGISTER_EFFECTS(lims::StartBillingRun, ::morph::effects::changes<lims::BillingRunView>,
                        ::morph::effects::changes<lims::InvoiceView>,
                        ::morph::effects::changes<lims::BillableLineView>)
BRIDGE_REGISTER_ACTION(lims::BillingRunModel, lims::AdvanceBillingRun, "AdvanceBillingRun")
BRIDGE_REGISTER_EFFECTS(lims::AdvanceBillingRun, ::morph::effects::changes<lims::BillingRunView>,
                        ::morph::effects::changes<lims::InvoiceView>,
                        ::morph::effects::changes<lims::BillableLineView>)
BRIDGE_REGISTER_ACTION(lims::BillingRunModel, lims::RunBillingSchedule, "RunBillingSchedule")
BRIDGE_REGISTER_EFFECTS(lims::RunBillingSchedule, ::morph::effects::changes<lims::BillingRunView>,
                        ::morph::effects::changes<lims::InvoiceView>,
                        ::morph::effects::changes<lims::BillableLineView>)
BRIDGE_REGISTER_ACTION(lims::BillingRunModel, lims::ListBillingRuns, "ListBillingRuns", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
