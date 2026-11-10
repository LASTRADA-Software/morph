// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/rational.hpp>
#include <utility>

#include "lims/app/internal_client.hpp"
#include "lims/billing/dto/run_dto.hpp"
#include "lims/org/core/money.hpp"

/// @file
/// The billing schedule: each tick is one dispatched `RunBillingSchedule`, as the schedule principal. The server
/// ticks it once at start-up, which resumes a run a restart interrupted, and every ten seconds after.

namespace lims::app {

/// @brief Ticks billing's schedule through an internal client.
class BillingScheduler {
public:
    /// @brief A schedule dispatching through @p client.
    /// @param client The schedule principal's client. Borrowed: it must outlive this scheduler.
    /// @param batch How many items one tick drafts per run.
    explicit BillingScheduler(InternalClient& client,
                              Count batch = Count{::morph::math::Rational{50, ::morph::math::DecimalPlaces{0}}})
        : _client{client}, _batch{std::move(batch)} {}

    /// @brief One tick: starts each due company's run and advances each unfinished run by one batch.
    /// @return The runs it advanced.
    /// @throws std::exception the dispatch's refusal or failure; what committed stays, and the next tick resumes.
    BillingScheduleResult tick();

private:
    // Borrowed for the scheduler's life; never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    InternalClient& _client;
    Count _batch;
};

}  // namespace lims::app
