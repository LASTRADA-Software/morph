// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/wasm/store.hpp"

/// @file
/// Store-level helpers mirroring the native `bank/db/user_ops.hpp` +
/// `bank/db/ledger_ops.hpp`: principal→user resolution, the ownership guard, and
/// the credit/debit/post-entry ledger primitives — reimplemented against the
/// in-memory `Db`. They throw the same `bank::` domain errors so the GUI
/// controllers classify failures identically.

namespace bank::wasm {

/// @brief Unix epoch milliseconds (ledger timestamp / ordering key).
[[nodiscard]] inline std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ── Users ────────────────────────────────────────────────────────────────────

/// @brief Id of the user named @p username, if any.
[[nodiscard]] inline std::optional<std::uint64_t> findUserId(Db& db, std::string_view username) {
    auto rows = db.users.where([&](const UserRow& u) { return u.username == username; });
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front().id;
}

/// @brief Resolves @p username to its id, or throws Unauthorized.
[[nodiscard]] inline std::uint64_t requireUserId(Db& db, std::string_view username) {
    if (auto id = findUserId(db, username)) {
        return *id;
    }
    throw Unauthorized{"unknown user: " + std::string{username}};
}

/// @brief Gets or creates the user named @p username; returns its id.
inline std::uint64_t ensureUser(Db& db, std::string_view username, std::string_view displayName = {}) {
    if (auto id = findUserId(db, username)) {
        return *id;
    }
    UserRow row;
    row.username = std::string{username};
    row.displayName = std::string{displayName.empty() ? username : displayName};
    row.status = 0;
    return db.users.insert(std::move(row));
}

// ── Ownership guards ─────────────────────────────────────────────────────────

/// @brief Loads an account, requiring it to exist and be owned by @p ownerId.
/// @throws NotFound / Unauthorized.
[[nodiscard]] inline AccountRow loadOwnedAccount(Db& db, std::int64_t accountId,
                                                 std::uint64_t ownerId) {
    auto* acct = db.accounts.find(static_cast<std::uint64_t>(accountId));
    if (acct == nullptr) {
        throw NotFound{"account not found"};
    }
    if (acct->userId != ownerId) {
        throw Unauthorized{"account belongs to a different owner"};
    }
    return *acct;
}

/// @brief Like loadOwnedAccount but also requires the account to be Open.
/// @throws NotFound / Unauthorized / ConflictError.
[[nodiscard]] inline AccountRow loadOwnedOpenAccount(Db& db, std::int64_t accountId,
                                                     std::uint64_t ownerId) {
    auto acct = loadOwnedAccount(db, accountId, ownerId);
    if (acct.status != static_cast<int>(AccountStatus::Open)) {
        throw ConflictError{"account is not open"};
    }
    return acct;
}

// ── Ledger primitives ────────────────────────────────────────────────────────

/// @brief Appends a ledger row reflecting @p account's *current* balance.
inline TxnRow postEntry(Db& db, const AccountRow& account, TxnDirection direction, TxnKind kind,
                        std::int64_t amountMinor, std::int64_t counterpartyId,
                        const std::string& description) {
    TxnRow txn;
    txn.accountId = account.id;
    txn.counterpartyId = counterpartyId;
    txn.direction = static_cast<int>(direction);
    txn.kind = static_cast<int>(kind);
    txn.amountMinor = amountMinor;
    txn.currency = account.currency;
    txn.balanceAfterMinor = account.balanceMinor;
    txn.description = description;
    txn.createdAtMs = nowMillis();
    txn.id = db.txns.insert(txn);
    return txn;
}

/// @brief Credits @p account, persists it, and posts a ledger entry.
inline TxnRow applyCredit(Db& db, AccountRow& account, std::int64_t amountMinor, TxnKind kind,
                          std::int64_t counterpartyId, const std::string& description) {
    account.balanceMinor += amountMinor;
    db.accounts.update(account);
    return postEntry(db, account, TxnDirection::Credit, kind, amountMinor, counterpartyId, description);
}

/// @brief Debits @p account (respecting overdraft), persists it, and posts an entry.
/// @throws InsufficientFunds if the debit would breach the overdraft limit.
inline TxnRow applyDebit(Db& db, AccountRow& account, std::int64_t amountMinor, TxnKind kind,
                         std::int64_t counterpartyId, const std::string& description) {
    const std::int64_t projected = account.balanceMinor - amountMinor;
    if (projected < -account.overdraftMinor) {
        throw InsufficientFunds{"amount exceeds available balance plus overdraft"};
    }
    account.balanceMinor = projected;
    db.accounts.update(account);
    return postEntry(db, account, TxnDirection::Debit, kind, amountMinor, counterpartyId, description);
}

}  // namespace bank::wasm
