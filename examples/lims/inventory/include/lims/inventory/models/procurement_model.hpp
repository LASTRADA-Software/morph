// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "lims/inventory/dto/requisition_dto.hpp"

/// @file
/// `ProcurementModel`: the requisitions that ask to buy stock, and their approval.

namespace lims {

/// @brief What a company has asked to buy, and who approved it.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. A
/// requisition's lifecycle is its row: a transition is a conditional update on the state the action read, so a
/// requisition raised on one connection can be submitted, escalated and approved days later on others, and two
/// approvers acting at once give one approval and one `Conflict`.
class ProcurementModel {
public:
    /// @brief Raises a requisition at a site, as a draft with no line, in the company's base currency.
    /// @param action The company, site and reason.
    /// @return The draft.
    /// @throws ValidationError if the request is malformed or the site is not the company's.
    /// @throws NotFound if the company does not exist.
    /// @throws Forbidden if the caller holds no role at the site.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RequisitionView execute(const CreateRequisition& action);

    /// @brief Adds a line to a draft requisition, and its estimate to the total.
    /// @param action The requisition, item, quantity with its unit, and estimate.
    /// @return The requisition as it stands.
    /// @throws ValidationError if the request is malformed, the quantity or the estimate is not positive, the
    ///         estimate is not a whole number of the company's minor units, or the quantity's unit is of another
    ///         dimension than the item's.
    /// @throws OutOfRange if the converted quantity or the new total does not fit.
    /// @throws NotFound if the requisition is not visible to the caller, or the item is not one of its company's.
    /// @throws IllegalTransition if the requisition is not a draft.
    /// @throws Forbidden if the caller holds no role at the site, or is not the requester.
    /// @throws Conflict if the requisition was submitted or changed meanwhile.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RequisitionView execute(const AddRequisitionLine& action);

    /// @brief Submits a draft requisition: records the submitter, the deadline (now plus the company's approval
    ///        hours) and the total the approver's limit is judged against.
    /// @param action The requisition.
    /// @return The requisition, submitted.
    /// @throws ValidationError if the request is malformed, or the requisition has no line.
    /// @throws NotFound if the requisition is not visible to the caller, or its company has no inventory settings.
    /// @throws IllegalTransition if the requisition is not a draft.
    /// @throws Forbidden if the caller holds no role at the site, or is not the requester.
    /// @throws Conflict if the requisition was submitted or changed meanwhile.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RequisitionView execute(const SubmitRequisition& action);

    /// @brief Approves a submitted requisition.
    ///
    /// By a purchase approver at the requisition's site before its deadline and at its company from the deadline on,
    /// within their value limit for that role, who neither raised nor submitted it.
    /// @param action The requisition.
    /// @return The requisition, approved.
    /// @throws ValidationError if the request is malformed.
    /// @throws IllegalTransition if the requisition is not submitted.
    /// @throws Forbidden if the caller is not a purchase approver where the requisition is, raised or submitted it,
    ///         has no value limit for the role there, or has one below its total.
    /// @throws Conflict if the requisition was approved meanwhile.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RequisitionView execute(const ApproveRequisition& action);

    /// @brief Reads a requisition, when the caller can see its company.
    /// @param action The requisition.
    /// @return The requisition.
    /// @throws ValidationError if no requisition is named.
    /// @throws NotFound if there is no such requisition, or it is of a company the caller cannot see.
    RequisitionView execute(const GetRequisition& action);

    /// @brief Lists a company's requisitions, when the caller can see the company.
    /// @param action The company.
    /// @return Its requisitions by id; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named.
    ListRequisitionsResult execute(const ListRequisitions& action);

    /// @brief Lists the submitted requisitions the caller could approve now: those whose approval rule allows them,
    ///        at the site before the deadline and at the company from it.
    /// @param action Carries nothing.
    /// @return The requisitions by id; none for a caller with no principal.
    ListRequisitionsResult execute(const ListPendingRequisitions& action);
};

}  // namespace lims

// The guard each registered action names must be complete here: the registration instantiates its execution.
#include "lims/inventory/authorization/inventory_authorization.hpp"

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::ProcurementModel, "ProcurementModel")
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CreateRequisition, "CreateRequisition")
BRIDGE_REGISTER_EFFECTS(lims::CreateRequisition, ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::AddRequisitionLine, "AddRequisitionLine")
BRIDGE_REGISTER_EFFECTS(lims::AddRequisitionLine, ::morph::effects::updates<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::SubmitRequisition, "SubmitRequisition")
BRIDGE_REGISTER_EFFECTS(lims::SubmitRequisition, ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ApproveRequisition, "ApproveRequisition")
BRIDGE_REGISTER_EFFECTS(lims::ApproveRequisition, ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::GetRequisition, "GetRequisition", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListRequisitions, "ListRequisitions",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListPendingRequisitions, "ListPendingRequisitions",
                       ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
