// SPDX-License-Identifier: Apache-2.0

#include "bank/models/customer_model.hpp"

#include <Lightweight/Lightweight.hpp>
#include <morph/core/registry.hpp>
#include <random>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/account_mapping.hpp"
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

/// Default annual interest for savings accounts (1.5% = 150 bps); others earn 0.
int defaultInterestBps(int kind) { return kind == static_cast<int>(AccountKind::Savings) ? 150 : 0; }

}  // namespace

dto::AccountInfo CustomerModel::execute(const dto::OpenAccount& action) {
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
    return db::toAccountInfo(rec, owner);
}

dto::AccountList CustomerModel::execute(const dto::ListAccounts& action) {
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
        out.accounts.push_back(db::toAccountInfo(*account, owner));
    }
    return out;
}

}  // namespace bank
