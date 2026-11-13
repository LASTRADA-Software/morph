// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Consumption: what a captured result takes out of stock.

namespace lims {

/// @brief The longest event id a consumption accepts. The ledger holds a source key in 128 bytes and the posting's key
///        is the event id behind a 22-byte prefix.
inline constexpr std::size_t kMaxConsumptionEventIdBytes = 106;

/// @brief Takes the reagents of a captured result out of stock: what lab's `lab.ResultCaptured` becomes in inventory.
///
/// Sent only by the app's event relay, as a service principal; a redelivered event id answers the consumption it made
/// and consumes nothing more.
struct RecordConsumption {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The event's id, `<module>.<event>:<aggregate id>:<aggregate version>`; the same for a redelivery.
    std::string eventId;
    /// @brief The captured result.
    ResultId result;
    /// @brief Its sample.
    SampleId sample;
    /// @brief The analysis definition it was captured under.
    AnalysisId analysis;
    /// @brief The sample's site; stock is taken from the lots kept there, and its company's books pay for it.
    SiteId site;
    /// @brief When the result was captured; a lot that expired before then is not used, and the posting is dated by
    /// it.
    ::morph::time::Timestamp capturedAt;

    /// @brief Whether the event names everything a consumption needs.
    /// @return `true` when it has an event id of 1 to 106 bytes and every id and the date.
    [[nodiscard]] bool validate() const noexcept {
        return !eventId.empty() && eventId.size() <= kMaxConsumptionEventIdBytes && result.hasValue() &&
               sample.hasValue() && analysis.hasValue() && site.hasValue() && capturedAt.hasValue();
    }
};

/// @brief What one event took out of stock.
struct ConsumptionView {
    /// @brief The event's id; the consumption's key.
    std::string eventId;
    /// @brief The sample the result belongs to.
    SampleId sample;
    /// @brief The movements the event made, lot by lot, then the shortfall movement of an item the lots could not
    ///        cover.
    std::vector<StockMovementView> movements;
    /// @brief What the lots could not cover: the shortfall movements' quantities added, each as held in its item's
    ///        unit; the movements tell the items apart.
    Measure shortfall;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::ConsumptionView, "Inventory_Consumption",
                       ::morph::record::key<&lims::ConsumptionView::eventId>)
