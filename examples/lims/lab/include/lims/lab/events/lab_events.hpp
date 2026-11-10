// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/journal/action_log.hpp>
#include <morph/util/datetime.hpp>
#include <span>
#include <string_view>
#include <vector>

#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The facts lab tells other modules about, and its outbox: rows written in the transaction of the fact, read and
/// marked by a relay the app runs. ORM-free, so the screens and the app can include it.

namespace lims {

/// @brief The event type of a published result.
inline constexpr std::string_view kAnalysisPublished = "lab.AnalysisPublished";

/// @brief A result was published to its client.
struct AnalysisPublished {
    /// @brief The result.
    ResultId result;
    /// @brief Its sample.
    SampleId sample;
    /// @brief The analysis definition it was captured under.
    AnalysisId analysis;
    /// @brief The sample's client.
    ClientId client;
    /// @brief The sample's site.
    SiteId site;
    /// @brief When the sample was published.
    ::morph::time::Timestamp publishedAt;
};

/// @brief Every event lab committed and no relay has marked, oldest first, as journal entries: `actionType` is the
///        event type, `idempotencyKey` the event id, `payload` the event as JSON.
/// @return The entries; reading them changes nothing.
[[nodiscard]] std::vector<::morph::journal::LogEntry> drainLabOutbox();

/// @brief Deletes the outbox rows of @p rows, by event id: they were delivered.
/// @param rows Entries `drainLabOutbox` returned.
void markLabOutboxRelayed(std::span<::morph::journal::LogEntry const> rows);

}  // namespace lims
