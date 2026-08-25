// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bank/db/db_model.hpp"
#include "bank/dto/transaction_dto.hpp"

/// @file
/// The Transaction model: deposits, withdrawals, atomic transfers, and history.
/// Transfers update two accounts and write two ledger rows inside a single
/// `SqlTransaction`, so a failure leaves no partial state.
///
/// Deposit/Withdraw/Transfer default to `morph::model::Loggable::Yes` — when
/// the app attaches an action log via `IModelHolder::attachActionLog` (see
/// `bank_cli`'s `main.cpp`), every money movement is recorded automatically.
/// `History` is a pure read and opts out explicitly.

namespace bank {

/// @brief Moves money and records the ledger.
class TransactionModel : private db::WithMapper {
public:
    /// @brief Credits an account and records a Deposit entry.
    dto::TxnInfo execute(const dto::Deposit& action);

    /// @brief Debits an account (within overdraft) and records a Withdrawal entry.
    dto::TxnInfo execute(const dto::Withdraw& action);

    /// @brief Atomically moves money between two accounts of the same currency.
    dto::TransferResult execute(const dto::Transfer& action);

    /// @brief Returns a newest-first page of an account's ledger.
    dto::HistoryPage execute(const dto::History& action);
};

}  // namespace bank

using bank::TransactionModel;
using bank::dto::Deposit;
using bank::dto::History;
using bank::dto::Transfer;
using bank::dto::Withdraw;

BRIDGE_REGISTER_MODEL(TransactionModel, "TransactionModel")
BRIDGE_REGISTER_ACTION(TransactionModel, Deposit, "Deposit")
BRIDGE_REGISTER_ACTION(TransactionModel, Withdraw, "Withdraw")
BRIDGE_REGISTER_ACTION(TransactionModel, Transfer, "Transfer")
BRIDGE_REGISTER_ACTION(TransactionModel, History, "History", ::morph::model::Loggable::No)
