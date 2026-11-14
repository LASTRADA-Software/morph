// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims/app/internal_client.hpp"

/// @file
/// The one place lab's events meet their consumers: a journal sink per consumer that dispatches the event to the
/// consumer's action through an internal client (`lab.AnalysisPublished` to billing, `lab.ResultCaptured` to
/// inventory), a router that hands each event to the sink its type is routed to, and one relay pass over lab's outbox
/// into it.

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

/// @brief A `morph::journal::IActionLog` that delivers lab's result-captured events to inventory's consumer.
///
/// Decodes lab's event and dispatches `RecordConsumption` through the internal client, waiting for its answer, so the
/// relay marks a batch only once inventory has committed every consumption in it. Delivery is at least once (the
/// relay's contract) and inventory's consumer makes it once in effect.
class InventoryEventSink final : public ::morph::journal::IActionLog {
public:
    /// @brief A sink dispatching through @p client.
    /// @param client The events principal's client. Borrowed: it must outlive this sink.
    explicit InventoryEventSink(InternalClient& client) : _client{client} {}

    /// @brief Delivers @p entry to inventory.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if the entry is not a `lab.ResultCaptured` or its payload does not decode.
    /// @throws std::exception inventory's refusal.
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

/// @brief The relay's one sink: hands each event to the sink its type is routed to.
///
/// `OutboxRelay` has one sink and marks a drained batch whole, and lab's outbox holds every event type lab writes, so
/// one pass delivers several types; this routes each to its consumer. An event type with no route is refused, never
/// skipped: the relay deletes what it marks, so a skipped event would be lost to the consumer that should have had it.
class LabEventRouter final : public ::morph::journal::IActionLog {
public:
    /// @brief A router over @p routes.
    /// @param routes Each event type with the sink it goes to.
    explicit LabEventRouter(std::vector<std::pair<std::string, std::shared_ptr<::morph::journal::IActionLog>>> routes)
        : _routes{std::move(routes)} {}

    /// @brief Delivers @p entry to its type's sink.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if no sink is routed for its type.
    /// @throws std::exception the sink's failure.
    void append(::morph::journal::LogEntry entry) override;

    /// @brief Flushes every routed sink.
    void flush() override;

    /// @brief The entries every routed sink delivered, for @p entityKey or all.
    /// @param entityKey A sample id, or empty for every entry.
    /// @return The entries, each sink's in its delivery order, sinks in route order.
    [[nodiscard]] std::vector<::morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override;

private:
    std::vector<std::pair<std::string, std::shared_ptr<::morph::journal::IActionLog>>> _routes;
};

/// @brief The router the server relays lab's outbox into: analysis-published to billing, result-captured to
///        inventory, both through @p events.
/// @param events The events principal's client.
/// @return The router.
[[nodiscard]] std::shared_ptr<LabEventRouter> labEventRouter(InternalClient& events);

/// @brief One relay pass: every event in lab's outbox to the consumer of its type, then marked relayed.
/// @param client The events principal's client.
/// @return How many events were relayed.
/// @throws std::exception from a consumer; the batch is then left unmarked and the next pass retries it.
std::size_t relayLabEvents(InternalClient& client);

}  // namespace lims::app
