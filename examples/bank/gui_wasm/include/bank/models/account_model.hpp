// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/account_model.hpp (in-memory backend).

#include <morph/registry.hpp>

#include "bank/dto/account_dto.hpp"

namespace bank {

/// @brief Opens, lists, inspects, and closes customer accounts (in-memory).
class AccountModel {
public:
    dto::AccountInfo execute(const dto::OpenAccount& action);
    dto::AccountList execute(const dto::ListAccounts& action);
    dto::AccountInfo execute(const dto::GetAccount& action);
    dto::CommandResult execute(const dto::CloseAccount& action);
};

}  // namespace bank

using bank::AccountModel;
using bank::dto::CloseAccount;
using bank::dto::GetAccount;
using bank::dto::ListAccounts;
using bank::dto::OpenAccount;

BRIDGE_REGISTER_MODEL(AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(AccountModel, OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(AccountModel, ListAccounts, "ListAccounts")
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount")
BRIDGE_REGISTER_ACTION(AccountModel, CloseAccount, "CloseAccount")
