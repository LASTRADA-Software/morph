// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/customer_model.hpp (in-memory backend).

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <string>

#include "bank/dto/account_dto.hpp"

namespace bank {

/// @brief One customer: lists and opens the accounts they own (in-memory).
class CustomerModel {
public:
    /// @brief Owner username. Declaring this alias is what makes the model keyed.
    using PrimaryKey = std::string;

    dto::AccountInfo execute(const dto::OpenAccount& action);
    dto::AccountList execute(const dto::ListAccounts& action);
};

}  // namespace bank

using bank::CustomerModel;
using bank::dto::ListAccounts;
using bank::dto::OpenAccount;

BRIDGE_REGISTER_MODEL(CustomerModel, "CustomerModel")
BRIDGE_REGISTER_ACTION(CustomerModel, OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(CustomerModel, ListAccounts, "ListAccounts")

BRIDGE_KEY_FROM(ListAccounts, &ListAccounts::owner);
BRIDGE_KEY_FROM(OpenAccount, &OpenAccount::owner);
