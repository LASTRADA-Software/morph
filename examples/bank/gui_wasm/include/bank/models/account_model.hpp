// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/account_model.hpp (in-memory backend).
// Kept in lockstep with the desktop model: one account per instance, held in
// memory, keyed by account id.

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <string>

#include "bank/dto/account_dto.hpp"
#include "bank/wasm/store.hpp"

namespace bank {

/// @brief One customer account, cached in the instance (in-memory backend).
class AccountModel {
public:
    /// @brief Account id. Declaring this alias is what makes the model keyed.
    using PrimaryKey = std::int64_t;

    dto::AccountInfo execute(const dto::GetAccount& action);
    dto::CommandResult execute(const dto::CloseAccount& action);

private:
    void hydrate(std::int64_t accountId);

    wasm::AccountRow _row{};
    std::string _owner;
    std::int64_t _loadedId = 0;
    std::uint64_t _seenVersion = 0;
};

}  // namespace bank

using bank::AccountModel;
using bank::dto::CloseAccount;
using bank::dto::GetAccount;

BRIDGE_REGISTER_MODEL(AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount")
BRIDGE_REGISTER_ACTION(AccountModel, CloseAccount, "CloseAccount")

BRIDGE_KEY_FROM(GetAccount, &GetAccount::id);
BRIDGE_KEY_FROM(CloseAccount, &CloseAccount::id);
