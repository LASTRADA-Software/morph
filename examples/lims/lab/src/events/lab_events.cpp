// SPDX-License-Identifier: Apache-2.0
#include "lims/lab/events/lab_events.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lims/lab/events/lab_outbox_store.hpp"
#include "lims/org/core/model_support.hpp"

namespace lims {

void writeAnalysisPublished(Lightweight::DataMapper& mapper, db::SampleRecord const& sample,
                            std::string const& principal) {
    auto const sampleId = static_cast<std::int64_t>(sample.id.Value());
    std::int64_t const now = nowMillis();
    for (auto const& result : mapper.Query<db::ResultRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", sampleId)
                                  .All()) {
        auto const versions = mapper.Query<db::AnalysisVersionRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::id>, "=",
                                         static_cast<std::int64_t>(result.analysisVersion.Value()))
                                  .All();
        AnalysisPublished const event{
            .result = ResultId{static_cast<std::int64_t>(result.id.Value())},
            .sample = SampleId{sampleId},
            .analysis = versions.empty() ? AnalysisId{}
                                         : AnalysisId{static_cast<std::int64_t>(versions.front().analysis.Value())},
            .client = ClientId{static_cast<std::int64_t>(sample.client.Value())},
            .site = SiteId::fromOptional(sample.siteId.Value()),
            .publishedAt = timestampFromMillis(now),
        };
        db::LabOutboxRecord row;
        row.eventId =
            Lightweight::SqlAnsiString<128>{std::string{kAnalysisPublished} + ":" + std::to_string(*event.result) +
                                            ":" + std::to_string(sample.version.Value())};
        row.eventType = Lightweight::SqlAnsiString<64>{std::string{kAnalysisPublished}};
        row.entityKey = Lightweight::SqlAnsiString<64>{std::to_string(sampleId)};
        row.payload = Lightweight::SqlDynamicAnsiString<4096>{glz::write_json(event).value_or(std::string{})};
        row.principal = Lightweight::SqlAnsiString<64>{principal};
        row.recordedAt = now;
        mapper.Create(row);
    }
}

std::vector<::morph::journal::LogEntry> drainLabOutbox() {
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::LabOutboxRecord>().All();
    std::ranges::sort(rows, {}, [](db::LabOutboxRecord const& row) { return row.id.Value(); });
    std::vector<::morph::journal::LogEntry> entries;
    for (auto const& row : rows) {
        // journal-stamp-exempt: an event row carries its own type and payload; it was not produced by executing an
        // action, so there is no action shape to fingerprint.
        ::morph::journal::LogEntry entry;
        entry.modelType = "SampleModel";
        entry.entityKey = std::string{row.entityKey.Value()};
        entry.actionType = std::string{row.eventType.Value()};
        entry.payload = std::string{row.payload.Value()};
        entry.principal = std::string{row.principal.Value()};
        entry.timestampMs = row.recordedAt.Value();
        entry.idempotencyKey = std::string{row.eventId.Value()};
        entries.push_back(std::move(entry));
    }
    return entries;
}

void markLabOutboxRelayed(std::span<::morph::journal::LogEntry const> rows) {
    Lightweight::DataMapper mapper;
    for (auto const& entry : rows) {
        for (auto const& row :
             mapper.Query<db::LabOutboxRecord>()
                 .Where(::Lightweight::FieldNameOf<&db::LabOutboxRecord::eventId>, "=", entry.idempotencyKey)
                 .All()) {
            mapper.Delete(row);
        }
    }
}

}  // namespace lims
