// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/payee_model.hpp (in-memory backend).

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bank/dto/common.hpp"
#include "bank/dto/payee_dto.hpp"

namespace bank {

/// @brief Stores and lists beneficiaries scoped to the session owner (in-memory).
class PayeeModel {
public:
    dto::PayeeInfo execute(const dto::AddPayee& action);
    dto::CommandResult execute(const dto::RemovePayee& action);
    dto::PayeeList execute(const dto::ListPayees& action);
};

}  // namespace bank

using bank::PayeeModel;
using bank::dto::AddPayee;
using bank::dto::ListPayees;
using bank::dto::RemovePayee;

BRIDGE_REGISTER_MODEL(PayeeModel, "PayeeModel")
BRIDGE_REGISTER_ACTION(PayeeModel, AddPayee, "AddPayee")
BRIDGE_REGISTER_ACTION(PayeeModel, RemovePayee, "RemovePayee")
BRIDGE_REGISTER_ACTION(PayeeModel, ListPayees, "ListPayees")
