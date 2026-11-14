// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>

#include "lims/inventory/dto/reservation_dto.hpp"
#include "lims/inventory/dto/stock_dto.hpp"

/// @file
/// `ReservationModel`: one bench's reservation of the lots a sample's analyses will consume.

namespace lims {

/// @brief One bench's reservations for one sample: the lots its analyses will consume.
///
/// One private instance per bench (a screen registers its own). The instance names its reservation rows with a token
/// of its own, so a second instance of the same principal is another bench. Destroying the instance deletes its rows:
/// that is what happens when its screen unmounts, and when the server reclaims the instance because the connection
/// that registered it closed. A reservation also lapses an hour after the last one, with no timer: a lapsed row
/// counts for nothing.
class ReservationModel {
public:
    /// @brief Names this bench with a fresh random token.
    ReservationModel();

    /// @brief Deletes this bench's reservation. A failure is logged, never thrown.
    ~ReservationModel();
    ReservationModel(ReservationModel const&) = delete;
    ReservationModel& operator=(ReservationModel const&) = delete;
    ReservationModel(ReservationModel&&) = delete;
    ReservationModel& operator=(ReservationModel&&) = delete;

    /// @brief Reserves, for one sample, what each recipe line of its analyses will consume, replacing this bench's
    ///        earlier reservation; renews it for an hour.
    ///
    /// For each item the analyses use, the first-expiring lot at the site whose stock on hand, less every other
    /// sample's live reservation, covers the item's need. Contention for a lot is ordered by the lot's
    /// `reservation_version`, written first and conditional on the version read: of two benches reserving the last
    /// lot, one succeeds and the other is refused in words naming who holds it.
    /// @param action The sample, its site and its analyses.
    /// @return The reservation.
    /// @throws ValidationError if the sample, the site or the analyses are missing.
    /// @throws NotFound if the site does not exist.
    /// @throws Conflict if another bench holds this sample's reservation, or no lot at the site has enough available
    ///         for an item, naming who holds the reservations that took it.
    /// @throws OutOfRange if a quantity does not fit.
    /// @throws Forbidden if the caller is not an analyst at the site.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ReservationView execute(const ReserveForSample& action);

    /// @brief Gives this bench's reservation up.
    /// @param action Carries nothing.
    /// @return The sample's reservation as it now is; its holder empty unless another bench took it since, and for a
    ///         bench that held none.
    ReservationView execute(const ReleaseReservation& action);

private:
    std::string _token;
    std::optional<std::int64_t> _sample;
};

}  // namespace lims

// The guard each registered action names must be complete here: the registration instantiates its execution.
#include "lims/inventory/authorization/inventory_authorization.hpp"

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::ReservationModel, "ReservationModel")
BRIDGE_REGISTER_ACTION(lims::ReservationModel, lims::ReserveForSample, "ReserveForSample")
BRIDGE_REGISTER_EFFECTS(lims::ReserveForSample, ::morph::effects::changes<lims::ReservationView>,
                        ::morph::effects::changes<lims::LotStockView>)
BRIDGE_REGISTER_ACTION(lims::ReservationModel, lims::ReleaseReservation, "ReleaseReservation")
BRIDGE_REGISTER_EFFECTS(lims::ReleaseReservation, ::morph::effects::changes<lims::ReservationView>,
                        ::morph::effects::changes<lims::LotStockView>)
// NOLINTEND(misc-const-correctness)
