// SPDX-License-Identifier: Apache-2.0
#include "lims/app/event_wiring.hpp"

#include <glaze/glaze.hpp>
#include <memory>
#include <morph/journal/outbox.hpp>
#include <string>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "lims/org/core/errors.hpp"

namespace lims::app {

void BillingEventSink::append(::morph::journal::LogEntry entry) {
    if (entry.actionType != kAnalysisPublished) {
        throw ValidationError{"billing does not consume " + entry.actionType};
    }
    AnalysisPublished published;
    if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(published, entry.payload)) {
        throw ValidationError{"event " + entry.idempotencyKey + " does not decode"};
    }
    static_cast<void>(_client.call<InvoiceModel>(RecordBillableLine{
        .eventId = entry.idempotencyKey,
        .result = published.result,
        .sample = published.sample,
        .analysis = published.analysis,
        .client = published.client,
        .site = published.site,
        .publishedAt = published.publishedAt,
    }));
    std::scoped_lock const lock{_mutex};
    _delivered.push_back(std::move(entry));
}

std::vector<::morph::journal::LogEntry> BillingEventSink::entries(std::string_view entityKey) const {
    std::scoped_lock const lock{_mutex};
    std::vector<::morph::journal::LogEntry> matching;
    for (auto const& entry : _delivered) {
        if (entityKey.empty() || entry.entityKey == entityKey) {
            matching.push_back(entry);
        }
    }
    return matching;
}

std::size_t relayLabEvents(InternalClient& client) {
    ::morph::journal::OutboxRelay relay;
    relay.drainOutbox = [] { return drainLabOutbox(); };
    relay.markRelayed = [](std::span<::morph::journal::LogEntry const> rows) { markLabOutboxRelayed(rows); };
    relay.sink = std::make_shared<BillingEventSink>(client);
    return relay.relay().relayed;
}

}  // namespace lims::app
