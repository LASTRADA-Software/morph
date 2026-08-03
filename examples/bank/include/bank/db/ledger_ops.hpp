// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/db/entities.hpp"
#include "bank/db/row_versions.hpp"
#include "bank/db/user_ops.hpp"

/// @file
/// Reusable ledger operations shared by every model that moves money
/// (transactions, payments, cards, loans, interest). All persistence goes
/// through the typed Lightweight `DataMapper` — no hand-written SQL.
///
/// The caller passes its own `DataMapper`; when several writes must be atomic,
/// it wraps the calls in a `SqlTransaction` over `mapper.Connection()`.
///
/// Concurrency note: `applyCredit`/`applyDebit` read the balance, adjust it in
/// memory, and write it back. Because each model owns a *separate* SQLite
/// connection, two models mutating the same account concurrently could
/// interleave and lose an update. The example mitigates this by (a) giving every
/// connection a busy timeout (see `db::configure`) so writers serialize rather
/// than fail, and (b) wrapping each balance change in a `SqlTransaction` so the
/// balance write and its ledger entry commit as a unit. A production ledger
/// would additionally use an atomic `balance = balance - ?` update or an
/// optimistic version column to fully close the read-modify-write window.

namespace bank::db {

/// @brief Unix epoch milliseconds (used as the ledger's creation timestamp).
[[nodiscard]] inline std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief Points a `BelongsTo<>` field at the referenced row's primary key and
///        marks it modified so a subsequent `Update` writes the foreign key.
///
/// Models usually have the parent record's id (from the wire DTO) but not the
/// loaded parent object; this sets the foreign key directly. Works for both
/// mandatory (`uint64`) and nullable (`std::optional<uint64>`) relations.
template <typename BelongsToField>
inline void setReference(BelongsToField& field, std::uint64_t id) {
    field.MutableValue() = id;
    field.SetModified(true);
}

/// @brief Loads a record by id, requiring it to exist and be owned by @p owner.
///
/// Authorization is expressed through the relation itself: the record's
/// `BelongsTo<&UserRecord::id> user` is navigated (`rec.user->username`,
/// lazily loaded by `QuerySingle`) and compared to the caller's principal. One
/// guard serves every owned resource (accounts, loans, cards, payees, payments,
/// budgets, notifications); @p noun is woven into the thrown messages.
/// @throws NotFound if no row has that id; Unauthorized if it belongs elsewhere.
template <typename Record>
[[nodiscard]] Record loadOwned(Lightweight::DataMapper& mapper, std::int64_t id,
                               const std::string& owner, std::string_view noun) {
    auto rec = mapper.QuerySingle<Record>(static_cast<std::uint64_t>(id));
    if (!rec.has_value()) {
        throw NotFound{std::string{noun} + " not found"};
    }
    if (std::string{rec->user->username.Value().str()} != owner) {
        throw Unauthorized{std::string{noun} + " belongs to a different owner"};
    }
    return *rec;
}

/// @brief Loads an account, requiring it to exist, be owned by @p owner, and be
///        open — checked in that order so a non-owner never learns the account's
///        status. The single home for the "owned and open" rule that every
///        money-movement path (deposit, withdraw, transfer, payment, loan, card)
///        shares.
/// @throws NotFound if missing; Unauthorized if owned elsewhere; ConflictError if not open.
[[nodiscard]] inline AccountRecord loadOwnedOpenAccount(Lightweight::DataMapper& mapper,
                                                        std::int64_t accountId, const std::string& owner) {
    auto acct = mapper.QuerySingle<AccountRecord>(static_cast<std::uint64_t>(accountId));
    if (!acct.has_value()) {
        throw NotFound{"account not found"};
    }
    if (std::string{acct->user->username.Value().str()} != owner) {
        throw Unauthorized{"account belongs to a different owner"};
    }
    if (acct->status.Value() != static_cast<int>(AccountStatus::Open)) {
        throw ConflictError{"account is not open"};
    }
    return *acct;
}

/// @brief Inserts a ledger row reflecting @p account's *current* balance.
///        @p counterpartyId is the other account in a transfer, or 0 for none.
inline TxnRecord postEntry(Lightweight::DataMapper& mapper, const AccountRecord& account,
                           TxnDirection direction, TxnKind kind, std::int64_t amountMinor,
                           std::int64_t counterpartyId, const std::string& description) {
    TxnRecord txn;
    setReference(txn.account, account.id.Value());
    if (counterpartyId > 0) {
        setReference(txn.counterparty, static_cast<std::uint64_t>(counterpartyId));
    }
    txn.direction = static_cast<int>(direction);
    txn.kind = static_cast<int>(kind);
    txn.amountMinor = amountMinor;
    txn.currency = account.currency.Value();
    txn.balanceAfterMinor = account.balanceMinor.Value();
    txn.description = Light::SqlAnsiString<128>{description};
    txn.createdAtMs = nowMillis();
    mapper.Create(txn);
    return txn;
}

/// @brief Credits @p account by @p amountMinor, persists it, and posts an entry.
inline TxnRecord applyCredit(Lightweight::DataMapper& mapper, AccountRecord& account,
                             std::int64_t amountMinor, TxnKind kind, std::int64_t counterpartyId,
                             const std::string& description) {
    account.balanceMinor = account.balanceMinor.Value() + amountMinor;
    bumpRowVersion(static_cast<std::int64_t>(account.id.Value()));
    mapper.Update(account);
    return postEntry(mapper, account, TxnDirection::Credit, kind, amountMinor, counterpartyId, description);
}

/// @brief Debits @p account by @p amountMinor (respecting overdraft), persists
///        it, and posts an entry.
/// @throws InsufficientFunds if the debit would breach the overdraft limit.
inline TxnRecord applyDebit(Lightweight::DataMapper& mapper, AccountRecord& account,
                            std::int64_t amountMinor, TxnKind kind, std::int64_t counterpartyId,
                            const std::string& description) {
    const std::int64_t projected = account.balanceMinor.Value() - amountMinor;
    if (projected < -account.overdraftMinor.Value()) {
        throw InsufficientFunds{"amount exceeds available balance plus overdraft"};
    }
    account.balanceMinor = projected;
    bumpRowVersion(static_cast<std::int64_t>(account.id.Value()));
    mapper.Update(account);
    return postEntry(mapper, account, TxnDirection::Debit, kind, amountMinor, counterpartyId, description);
}

}  // namespace bank::db
