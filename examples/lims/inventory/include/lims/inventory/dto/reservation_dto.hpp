// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/inventory/core/types.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Reservations: the lots a bench holds for a sample's analyses until they are consumed or lapse.

namespace lims {

/// @brief Reserves, for one sample, what each recipe line of its analyses will consume.
///
/// Sent to a `ReservationModel` instance, which is one bench. Reserving again from the same bench replaces what it
/// held.
struct ReserveForSample {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The sample the stock is held for. Inventory cannot check that it exists.
    SampleId sample;
    /// @brief The sample's site; stock is held from the lots kept there, and the caller's role is judged there.
    SiteId site;
    /// @brief The analyses the sample will be captured under.
    std::vector<AnalysisId> analyses;

    /// @brief Whether the request names everything a reservation needs.
    /// @return `true` when a sample, a site and at least one analysis, each named, are given.
    [[nodiscard]] bool validate() const noexcept {
        if (!sample.hasValue() || !site.hasValue() || analyses.empty()) {
            return false;
        }
        for (auto const& analysis : analyses) {
            if (!analysis.hasValue()) {
                return false;
            }
        }
        return true;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Gives the bench's reservation up.
struct ReleaseReservation {
    /// @brief Whether the action is well-formed.
    /// @return Always `true`: it names nothing.
    [[nodiscard]] bool validate() const noexcept { return true; }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Reads who holds a sample's reservation and what it holds.
struct GetSampleReservation {
    /// @brief The sample.
    SampleId sample;

    /// @brief Whether a sample is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return sample.hasValue(); }
};

/// @brief One lot a sample's reservation holds, and how much of it.
struct ReservedLotView {
    /// @brief The lot.
    LotId lot;
    /// @brief The supplier's lot number.
    std::string lotNumber;
    /// @brief The item.
    ItemId item;
    /// @brief The item's code.
    std::string itemCode;
    /// @brief The quantity held, in @p unit.
    Measure quantity;
    /// @brief The item's unit.
    StockUnit unit = StockUnit::piece;
};

/// @brief A sample's live reservation.
struct ReservationView {
    /// @brief The sample; the reservation's key.
    SampleId sample;
    /// @brief Who holds it; empty when nothing is reserved for the sample.
    std::string holder;
    /// @brief The lots held, by lot.
    std::vector<ReservedLotView> lots;
    /// @brief When it lapses unless renewed; empty when nothing is reserved.
    ::morph::time::Timestamp expiresAt;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::ReservationView, "Inventory_Reservation",
                       ::morph::record::key<&lims::ReservationView::sample>)
