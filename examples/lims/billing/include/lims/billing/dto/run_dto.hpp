// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/billing/core/types.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The billing run: one company's invoices for one calendar month, drafted item by item.

namespace lims {

/// @brief A billing run and how far it has got.
struct BillingRunView {
    /// @brief Its id.
    BillingRunId id;
    /// @brief The company it bills for.
    CompanyId company;
    /// @brief The calendar month it bills, `YYYY-MM`.
    std::string label;
    /// @brief Where it stands.
    BillingRunState state = BillingRunState::Running;
    /// @brief How many (client, site) items it has to draft.
    Count total;
    /// @brief How many of them are drafted.
    Count done;
    /// @brief When it started.
    ::morph::time::Timestamp startedAt;
    /// @brief Who started it.
    std::string startedBy;
};

/// @brief Starts a company's billing run for the current month.
struct StartBillingRun {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Drafts the next pending items of a run.
struct AdvanceBillingRun {
    /// @brief The run.
    BillingRunId runId;
    /// @brief How many items to draft at most.
    Count batch;

    /// @brief Whether a run and a batch are named.
    /// @return `true` when they are.
    [[nodiscard]] bool validate() const noexcept { return runId.hasValue() && batch.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One tick of the billing schedule: starts the run of every company due whose service role the caller holds,
///        then advances every unfinished run of those companies by one batch. Sent only by the app's schedule, as a
///        service principal.
struct RunBillingSchedule {
    /// @brief How many items to draft at most per run.
    Count batch;

    /// @brief Whether a batch is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return batch.hasValue(); }
};

/// @brief The runs the tick advanced, as they stand after it.
struct BillingScheduleResult {
    /// @brief The runs.
    std::vector<BillingRunView> runs;
};

/// @brief Lists a company's billing runs.
struct ListBillingRuns {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The runs, by id.
struct ListBillingRunsResult {
    /// @brief The runs.
    std::vector<BillingRunView> runs;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::BillingRunView, "Billing_Run")
