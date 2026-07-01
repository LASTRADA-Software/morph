// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of AccountModel for the WASM build. Mirrors the
// behaviour of src/models/account_model.cpp but persists to bank::wasm::Db.

#include "bank/models/account_model.hpp"

#include <random>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

std::string generateAccountNumber() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> digit{0, 9};
    std::string number = "DE";
    for (int idx = 0; idx < 20; ++idx) {
        number.push_back(static_cast<char>('0' + digit(rng)));
    }
    return number;
}

dto::AccountInfo toInfo(const wasm::AccountRow& rec, const std::string& owner) {
    return dto::AccountInfo{
        .id = static_cast<std::int64_t>(rec.id),
        .owner = owner,
        .number = rec.number,
        .kind = rec.kind,
        .currency = rec.currency,
        .balanceMinor = rec.balanceMinor,
        .overdraftMinor = rec.overdraftMinor,
        .status = rec.status,
        .interestBps = rec.interestBps,
    };
}

int defaultInterestBps(int kind) {
    return kind == static_cast<int>(AccountKind::Savings) ? 150 : 0;
}

}  // namespace

dto::AccountInfo AccountModel::execute(const dto::OpenAccount& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid account kind/currency/overdraft"};
    }
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal to own the account"};
    }
    auto& db = wasm::sharedDb();

    wasm::AccountRow rec;
    rec.userId = wasm::requireUserId(db, owner);
    rec.number = generateAccountNumber();
    rec.kind = action.kind;
    rec.currency = action.currency;
    rec.balanceMinor = 0;
    rec.overdraftMinor = action.overdraftMinor;
    rec.status = static_cast<int>(AccountStatus::Open);
    rec.interestBps = defaultInterestBps(action.kind);
    rec.id = db.accounts.insert(rec);
    return toInfo(rec, owner);
}

dto::AccountList AccountModel::execute(const dto::ListAccounts& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal to list accounts for"};
    }
    auto& db = wasm::sharedDb();
    const auto userId = wasm::requireUserId(db, owner);

    dto::AccountList out;
    for (const auto& rec : db.accounts.where([&](const wasm::AccountRow& a) { return a.userId == userId; })) {
        out.accounts.push_back(toInfo(rec, owner));
    }
    return out;
}

dto::AccountInfo AccountModel::execute(const dto::GetAccount& action) {
    const std::string owner = sessionPrincipal();
    auto& db = wasm::sharedDb();
    auto rec = wasm::loadOwnedAccount(db, action.id, wasm::requireUserId(db, owner));
    return toInfo(rec, owner);
}

dto::CommandResult AccountModel::execute(const dto::CloseAccount& action) {
    const std::string owner = sessionPrincipal();
    auto& db = wasm::sharedDb();
    auto rec = wasm::loadOwnedAccount(db, action.id, wasm::requireUserId(db, owner));
    if (rec.balanceMinor != 0) {
        return dto::CommandResult{.ok = false, .message = "account balance must be zero before closing"};
    }
    rec.status = static_cast<int>(AccountStatus::Closed);
    db.accounts.update(rec);
    return dto::CommandResult{.ok = true, .message = "account closed"};
}

}  // namespace bank
