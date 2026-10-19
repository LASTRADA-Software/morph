// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/transaction_model.hpp (in-memory backend).

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"

namespace bank {

/// @brief Moves money and records the ledger (in-memory).
class TransactionModel {
public:
    dto::TxnInfo execute(const dto::Deposit& action);
    dto::TxnInfo execute(const dto::Withdraw& action);
    dto::TransferResult execute(const dto::Transfer& action);
    dto::HistoryPage execute(const dto::History& action);
};

}  // namespace bank

using bank::TransactionModel;
using bank::dto::Deposit;
using bank::dto::History;
using bank::dto::Transfer;
using bank::dto::Withdraw;

BRIDGE_REGISTER_MODEL(TransactionModel, "TransactionModel")
// Moving money writes a ledger entry and changes the balances it moves between.
BRIDGE_REGISTER_ACTION(TransactionModel, Deposit, "Deposit")
BRIDGE_REGISTER_EFFECTS(bank::dto::Deposit, ::morph::effects::changes<bank::dto::TxnInfo>,
                        ::morph::effects::changes<bank::dto::AccountInfo>)
BRIDGE_REGISTER_ACTION(TransactionModel, Withdraw, "Withdraw")
BRIDGE_REGISTER_EFFECTS(bank::dto::Withdraw, ::morph::effects::changes<bank::dto::TxnInfo>,
                        ::morph::effects::changes<bank::dto::AccountInfo>)
// The reply is the two balances, not the accounts, so the accounts are changed, not updated.
BRIDGE_REGISTER_ACTION(TransactionModel, Transfer, "Transfer")
BRIDGE_REGISTER_EFFECTS(bank::dto::Transfer, ::morph::effects::changes<bank::dto::TxnInfo>,
                        ::morph::effects::changes<bank::dto::AccountInfo>)
BRIDGE_REGISTER_ACTION(TransactionModel, History, "History")
