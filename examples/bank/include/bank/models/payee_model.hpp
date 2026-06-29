// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/registry.hpp>

#include "bank/db/db_model.hpp"
#include "bank/dto/common.hpp"
#include "bank/dto/payee_dto.hpp"

/// @file
/// The Payee model: manage the current owner's beneficiaries. `AddPayee` is a
/// good fit for morph's field-by-field `set<>` streaming surface — it only
/// fires once a name and a plausible IBAN are both present.

namespace bank {

/// @brief Stores and lists beneficiaries scoped to the session owner.
class PayeeModel : private db::WithMapper {
public:
    /// @brief Adds a beneficiary for the current owner; returns the saved payee.
    dto::PayeeInfo execute(const dto::AddPayee& action);

    /// @brief Removes a beneficiary the current owner owns.
    dto::CommandResult execute(const dto::RemovePayee& action);

    /// @brief Lists the current owner's beneficiaries.
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
