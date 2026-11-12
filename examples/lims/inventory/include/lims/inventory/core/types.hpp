// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>

#include "lims/org/core/strong_id.hpp"

/// @file
/// Inventory's identities and closed sets.

namespace lims {

LIMS_DEFINE_STRONG_ID(ItemId);
LIMS_DEFINE_STRONG_ID(LocationId);
LIMS_DEFINE_STRONG_ID(LotId);
LIMS_DEFINE_STRONG_ID(MovementId);
LIMS_DEFINE_STRONG_ID(RequisitionId);
LIMS_DEFINE_STRONG_ID(RequisitionLineId);

/// @brief Why a lot's stock on hand changed.
enum class MovementKind : std::uint8_t {
    Opening,      ///< Stock a company held before lims, recorded as a lot.
    Receipt,      ///< Goods received against a purchase order.
    Consumption,  ///< Stock used up by an event.
    Shortfall,    ///< A consumption that found too little stock; it names no lot.
};

/// @brief Where a requisition stands.
enum class RequisitionState : std::uint8_t {
    Draft,      ///< Raised; its requester is still adding lines.
    Submitted,  ///< Awaiting a purchase approver.
    Approved,   ///< Approved; nothing has ordered it yet.
    Ordered,    ///< Turned into a purchase order.
};

/// @brief Who may approve a submitted requisition now.
enum class RequisitionLevel : std::uint8_t {
    Site,     ///< The approvers at the requisition's site, until its deadline.
    Company,  ///< The approvers at its company, from the deadline on.
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(ItemId);
LIMS_DEFINE_STRONG_ID_WIRE(LocationId);
LIMS_DEFINE_STRONG_ID_WIRE(LotId);
LIMS_DEFINE_STRONG_ID_WIRE(MovementId);
LIMS_DEFINE_STRONG_ID_WIRE(RequisitionId);
LIMS_DEFINE_STRONG_ID_WIRE(RequisitionLineId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ItemId, "lims.itemId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(LocationId, "lims.locationId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(LotId, "lims.lotId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(MovementId, "lims.movementId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(RequisitionId, "lims.requisitionId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(RequisitionLineId, "lims.requisitionLineId");

/// @brief Reflects `MovementKind` as its enumerator name.
template <>
struct glz::meta<lims::MovementKind> {
    using enum lims::MovementKind;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Opening, Receipt, Consumption, Shortfall);
};

/// @brief Reflects `RequisitionState` as its enumerator name.
template <>
struct glz::meta<lims::RequisitionState> {
    using enum lims::RequisitionState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Draft, Submitted, Approved, Ordered);
};

/// @brief Reflects `RequisitionLevel` as its enumerator name.
template <>
struct glz::meta<lims::RequisitionLevel> {
    using enum lims::RequisitionLevel;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Site, Company);
};
