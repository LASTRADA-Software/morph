// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/ui/author.hpp>

#include "lims/inventory/dto/order_dto.hpp"
#include "lims/inventory/dto/requisition_dto.hpp"

/// @file
/// `ProcurementModel`: the requisitions that ask to buy stock, their approval, and the suppliers and purchase orders
/// they become.

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
    /// @throws NotFound if there is no such requisition, or the caller cannot see its company; through the action
    /// guard
    ///         the same case reads as a `Forbidden` that says nothing of the requisition.
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

    /// @brief Counts the submitted requisitions at a company that the caller could approve now.
    /// @param action The company.
    /// @return The count, by the rule `ListPendingRequisitions` lists by.
    /// @throws ValidationError if no company is named.
    OpenWork<RequisitionView> execute(const CountPendingRequisitions& action);

    /// @brief Registers a supplier of a company, with the currency its orders are priced in.
    /// @param action The company, name and currency.
    /// @return The supplier.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company has a supplier of that name already.
    /// @throws Forbidden through the action guard if the caller is not a purchaser at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SupplierView execute(const CreateSupplier& action);

    /// @brief Lists a company's suppliers, when the caller can see the company.
    /// @param action The company.
    /// @return Its suppliers by id; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named.
    ListSuppliersResult execute(const ListSuppliers& action);

    /// @brief Orders an approved requisition from a supplier, in the supplier's currency.
    ///
    /// Every requisition line is priced once, per stock unit; a line total is the quantity times the unit price
    /// rounded half-up once to the currency's decimals, and the order total is their exact sum. The requisition moves
    /// from approved to ordered by a conditional update in the order's transaction, so it is ordered once.
    /// @param action The requisition, supplier and one price per line.
    /// @return The order, open.
    /// @throws ValidationError if the request is malformed, the supplier is another company's, a line is priced twice,
    ///         not at all or is not the requisition's, or a unit price is not positive or is not a whole number of
    ///         the supplier's minor units.
    /// @throws OutOfRange if a line total or the order total does not fit.
    /// @throws NotFound if the requisition or the supplier is not visible to the caller.
    /// @throws IllegalTransition if the requisition is not approved.
    /// @throws Forbidden if the caller is not a purchaser at the requisition's company.
    /// @throws Conflict if the requisition was ordered meanwhile.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PurchaseOrderView execute(const CreatePurchaseOrder& action);

    /// @brief Reads a purchase order, when the caller can see its company.
    /// @param action The order.
    /// @return The order.
    /// @throws ValidationError if no order is named.
    /// @throws NotFound if there is no such order, or it is of a company the caller cannot see.
    PurchaseOrderView execute(const GetPurchaseOrder& action);

    /// @brief Lists a company's purchase orders, when the caller can see the company.
    /// @param action The company.
    /// @return Its orders by id; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named.
    ListPurchaseOrdersResult execute(const ListPurchaseOrders& action);
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
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CountPendingRequisitions, "CountPendingRequisitions",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CreateSupplier, "CreateSupplier")
BRIDGE_REGISTER_EFFECTS(lims::CreateSupplier, ::morph::effects::changes<lims::SupplierView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListSuppliers, "ListSuppliers", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CreatePurchaseOrder, "CreatePurchaseOrder")
BRIDGE_REGISTER_EFFECTS(lims::CreatePurchaseOrder, ::morph::effects::changes<lims::PurchaseOrderView>,
                        ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::GetPurchaseOrder, "GetPurchaseOrder",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListPurchaseOrders, "ListPurchaseOrders",
                       ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)

/// @brief Procurement keeps no state between actions.
///        A shell part reads it through one shared instance, which a screen document may name only for a model that
///        keeps no state; the fact is declared here so that no translation unit sees the model without it.
template <>
struct morph::ui::author::ModelFacts<lims::ProcurementModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};
