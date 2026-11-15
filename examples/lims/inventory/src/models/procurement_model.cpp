// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/models/procurement_model.hpp"

#include <morph/core/bridge.hpp>

// The registrars are compiled here, once, rather than in every translation unit that includes the header.
// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL_SOURCE(lims::ProcurementModel)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::CreateRequisition)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::AddRequisitionLine)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::SubmitRequisition)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ApproveRequisition)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::GetRequisition)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ListRequisitions)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ListPendingRequisitions)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::CountPendingRequisitions)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::CreateSupplier)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ListSuppliers)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::CreatePurchaseOrder)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::GetPurchaseOrder)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ListPurchaseOrders)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ReceiveGoods)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::GetReceipt)
BRIDGE_REGISTER_ACTION_SOURCE(lims::ProcurementModel, lims::ListReceipts)
// NOLINTEND(misc-const-correctness)
