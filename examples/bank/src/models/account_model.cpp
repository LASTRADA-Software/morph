// SPDX-License-Identifier: Apache-2.0

#include "bank/models/account_model.hpp"

#include <Lightweight/Lightweight.hpp>
#include <morph/core/registry.hpp>
#include <random>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/ledger_ops.hpp"
#include "bank/db/user_ops.hpp"

namespace bank {

namespace {

/// Generates a pseudo-IBAN-ish account number: "DE" + 20 digits. Good enough
/// for an example; not a real check-digit-valid IBAN.
std::string generateAccountNumber() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> digit{0, 9};
    std::string number = "DE";
    for (int idx = 0; idx < 20; ++idx) {
        number.push_back(static_cast<char>('0' + digit(rng)));
    }
    return number;
}

/// Translates a persisted `AccountRecord` into the wire `AccountInfo` DTO.
/// @p owner is the resolved owner username (the wire DTO carries the username
/// rather than the internal `user_id` the record stores).
dto::AccountInfo toInfo(const db::AccountRecord& rec, const std::string& owner) {
    return dto::AccountInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = owner,
        .number = std::string{rec.number.Value().str()},
        .kind = rec.kind.Value(),
        .currency = rec.currency.Value(),
        .balanceMinor = rec.balanceMinor.Value(),
        .overdraftMinor = rec.overdraftMinor.Value(),
        .status = rec.status.Value(),
        .interestBps = rec.interestBps.Value(),
    };
}

/// Default annual interest for savings accounts (1.5% = 150 bps); others earn 0.
int defaultInterestBps(int kind) { return kind == static_cast<int>(AccountKind::Savings) ? 150 : 0; }

}  // namespace

dto::AccountInfo AccountModel::execute(const dto::OpenAccount& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid account kind/currency/overdraft"};
    }
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal to own the account"};
    }

    db::AccountRecord rec;
    db::setReference(rec.user, db::requireUserId(mapper(), owner));
    rec.number = Light::SqlAnsiString<34>{generateAccountNumber()};
    rec.kind = action.kind;
    rec.currency = action.currency;
    rec.balanceMinor = 0;
    rec.overdraftMinor = action.overdraftMinor;
    rec.status = static_cast<int>(AccountStatus::Open);
    rec.interestBps = defaultInterestBps(action.kind);

    mapper().Create(rec);
    return toInfo(rec, owner);
}

dto::AccountList AccountModel::execute(const dto::ListAccounts& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal to list accounts for"};
    }

    // Load the owner and walk the `UserRecord::accounts` HasMany relation rather
    // than issuing a manual `WHERE user_id = ?` — the relation resolves the join
    // for us and returns the user's accounts directly.
    const auto userId = db::requireUserId(mapper(), owner);
    auto user = mapper().QuerySingle<db::UserRecord>(userId).value();

    dto::AccountList out;
    out.accounts.reserve(user.accounts.Count());
    for (const auto& account : user.accounts.All()) {
        out.accounts.push_back(toInfo(*account, owner));
    }
    return out;
}

dto::AccountInfo AccountModel::execute(const dto::GetAccount& action) {
    const std::string owner = sessionPrincipal();
    auto rec = db::loadOwned<db::AccountRecord>(mapper(), action.id, owner, "account");
    return toInfo(rec, owner);
}

dto::CommandResult AccountModel::execute(const dto::CloseAccount& action) {
    auto rec = db::loadOwned<db::AccountRecord>(mapper(), action.id, sessionPrincipal(), "account");
    // Best-effort zero-balance guard. The balance is read on this model's own
    // connection, so a deposit committing on another model's connection between
    // this read and the Update could leave a Closed account holding funds — the
    // same cross-connection window documented in ledger_ops.hpp. A production
    // ledger would close the account inside the same transaction that settles
    // its balance, or gate on an atomic conditional update.
    if (rec.balanceMinor.Value() != 0) {
        return dto::CommandResult{.ok = false, .message = "account balance must be zero before closing"};
    }
    rec.status = static_cast<int>(AccountStatus::Closed);
    mapper().Update(rec);
    return dto::CommandResult{.ok = true, .message = "account closed"};
}

}  // namespace bank
