// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Transaction model: deposits, withdrawals, transfers, and
/// paginated history.

namespace bank::dto {

/// @brief A single ledger entry (the Transaction model's view of one row).
struct TxnInfo {
    std::int64_t id = 0;
    std::int64_t accountId = 0;
    std::int64_t counterpartyId = 0;
    int direction = 0;  ///< bank::TxnDirection
    int kind = 0;       ///< bank::TxnKind
    std::int64_t amountMinor = 0;
    int currency = 0;
    std::int64_t balanceAfterMinor = 0;
    std::string description;
    std::int64_t createdAtMs = 0;
};

/// @brief Pay money into an account.
struct Deposit {
    std::int64_t accountId = 0;
    std::int64_t amountMinor = 0;
    std::string description;

    [[nodiscard]] bool validate() const { return accountId > 0 && amountMinor > 0; }
};

/// @brief Take money out of an account (subject to overdraft limits).
struct Withdraw {
    std::int64_t accountId = 0;
    std::int64_t amountMinor = 0;
    std::string description;

    [[nodiscard]] bool validate() const { return accountId > 0 && amountMinor > 0; }
};

/// @brief Move money between two accounts atomically.
struct Transfer {
    std::int64_t fromAccountId = 0;
    std::int64_t toAccountId = 0;
    std::int64_t amountMinor = 0;
    std::string description;

    [[nodiscard]] bool validate() const {
        return fromAccountId > 0 && toAccountId > 0 && fromAccountId != toAccountId && amountMinor > 0;
    }
};

/// @brief Result of a transfer: the resulting balances of both accounts.
struct TransferResult {
    std::int64_t fromBalanceMinor = 0;
    std::int64_t toBalanceMinor = 0;
};

/// @brief Request a page of an account's ledger, newest first.
struct History {
    std::int64_t accountId = 0;
    int limit = 50;
    int offset = 0;
};

/// @brief A page of ledger entries.
struct HistoryPage {
    std::int64_t accountId = 0;
    std::vector<TxnInfo> entries;
};

}  // namespace bank::dto
