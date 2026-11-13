// SPDX-License-Identifier: Apache-2.0
#include "lims/app/event_wiring.hpp"

#include <glaze/glaze.hpp>
#include <memory>
#include <morph/journal/outbox.hpp>
#include <string>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
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

void InventoryEventSink::append(::morph::journal::LogEntry entry) {
    if (entry.actionType != kResultCaptured) {
        throw ValidationError{"inventory does not consume " + entry.actionType};
    }
    ResultCaptured captured;
    if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(captured, entry.payload)) {
        throw ValidationError{"event " + entry.idempotencyKey + " does not decode"};
    }
    static_cast<void>(_client.call<StockModel>(RecordConsumption{
        .eventId = entry.idempotencyKey,
        .result = captured.result,
        .sample = captured.sample,
        .analysis = captured.analysis,
        .site = captured.site,
        .capturedAt = captured.capturedAt,
    }));
    std::scoped_lock const lock{_mutex};
    _delivered.push_back(std::move(entry));
}

std::vector<::morph::journal::LogEntry> InventoryEventSink::entries(std::string_view entityKey) const {
    std::scoped_lock const lock{_mutex};
    std::vector<::morph::journal::LogEntry> matching;
    for (auto const& entry : _delivered) {
        if (entityKey.empty() || entry.entityKey == entityKey) {
            matching.push_back(entry);
        }
    }
    return matching;
}

void LabEventRouter::append(::morph::journal::LogEntry entry) {
    for (auto const& [type, sink] : _routes) {
        if (type == entry.actionType) {
            sink->append(std::move(entry));
            return;
        }
    }
    throw ValidationError{"no consumer is wired for " + entry.actionType};
}

void LabEventRouter::flush() {
    for (auto const& route : _routes) {
        route.second->flush();
    }
}

std::vector<::morph::journal::LogEntry> LabEventRouter::entries(std::string_view entityKey) const {
    std::vector<::morph::journal::LogEntry> all;
    for (auto const& route : _routes) {
        auto const mine = route.second->entries(entityKey);
        all.insert(all.end(), mine.begin(), mine.end());
    }
    return all;
}

std::shared_ptr<LabEventRouter> labEventRouter(InternalClient& events) {
    return std::make_shared<LabEventRouter>(
        std::vector<std::pair<std::string, std::shared_ptr<::morph::journal::IActionLog>>>{
            {std::string{kAnalysisPublished}, std::make_shared<BillingEventSink>(events)},
            {std::string{kResultCaptured}, std::make_shared<InventoryEventSink>(events)},
        });
}

std::size_t relayLabEvents(InternalClient& client) {
    ::morph::journal::OutboxRelay relay;
    relay.drainOutbox = [] { return drainLabOutbox(); };
    relay.markRelayed = [](std::span<::morph::journal::LogEntry const> rows) { markLabOutboxRelayed(rows); };
    relay.sink = labEventRouter(client);
    return relay.relay().relayed;
}

}  // namespace lims::app
