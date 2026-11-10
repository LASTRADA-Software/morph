// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/journal/action_log.hpp>
#include <mutex>
#include <string_view>
#include <vector>

#include "lims/app/internal_client.hpp"

/// @file
/// The one place lab's events meet billing: a journal sink that dispatches each `lab.AnalysisPublished` to billing's
/// consumer action through an internal client, and one relay pass over lab's outbox into it.

namespace lims::app {

/// @brief A `morph::journal::IActionLog` that delivers lab's events to billing instead of storing them.
///
/// `OutboxRelay` delivers to an action log; billing's consumer is an action. This adapter joins them: it decodes lab's
/// event and dispatches `RecordBillableLine` through the internal client, waiting for its answer, so the relay marks
/// a batch only once billing has committed every line in it. Delivery is at least once (the relay's contract) and
/// billing's consumer makes it once in effect.
class BillingEventSink final : public ::morph::journal::IActionLog {
public:
    /// @brief A sink dispatching through @p client.
    /// @param client The events principal's client. Borrowed: it must outlive this sink.
    explicit BillingEventSink(InternalClient& client) : _client{client} {}

    /// @brief Delivers @p entry to billing.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if the entry is not a `lab.AnalysisPublished` or its payload does not decode.
    /// @throws std::exception billing's refusal of the line.
    void append(::morph::journal::LogEntry entry) override;

    /// @brief Nothing to flush: every `append` committed before it returned.
    void flush() override {}

    /// @brief The entries delivered so far through this sink, for @p entityKey or all.
    /// @param entityKey A sample id, or empty for every entry.
    /// @return The entries, in delivery order.
    [[nodiscard]] std::vector<::morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override;

private:
    // Borrowed for the sink's life; never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    InternalClient& _client;
    mutable std::mutex _mutex;
    std::vector<::morph::journal::LogEntry> _delivered;
};

/// @brief One relay pass: every event in lab's outbox to billing, then marked relayed.
/// @param client The events principal's client.
/// @return How many events were relayed.
/// @throws std::exception from billing's consumer; the batch is then left unmarked and the next pass retries it.
std::size_t relayLabEvents(InternalClient& client);

}  // namespace lims::app
