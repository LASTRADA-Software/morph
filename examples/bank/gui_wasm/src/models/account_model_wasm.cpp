// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of the stateful AccountModel for the WASM build.
// Mirrors src/models/account_model.cpp: one account per instance, cached in the
// instance, re-hydrated from bank::wasm::Db only when the row version moves.

#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/row_versions.hpp"
#include "bank/models/account_model.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

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

}  // namespace

void AccountModel::hydrate(std::int64_t accountId) {
    const std::string owner = sessionPrincipal();
    const auto current = db::rowVersion(accountId);
    if (_loadedId == accountId && _owner == owner && _seenVersion == current) {
        return;
    }
    auto& store = wasm::sharedDb();
    _row = wasm::loadOwnedAccount(store, accountId, wasm::requireUserId(store, owner));
    _owner = owner;
    _loadedId = accountId;
    _seenVersion = current;
}

dto::AccountInfo AccountModel::execute(const dto::GetAccount& action) {
    hydrate(action.id);
    return toInfo(_row, _owner);
}

dto::CommandResult AccountModel::execute(const dto::CloseAccount& action) {
    hydrate(action.id);
    if (_row.balanceMinor != 0) {
        return dto::CommandResult{.ok = false, .message = "account balance must be zero before closing"};
    }
    _row.status = static_cast<int>(AccountStatus::Closed);
    wasm::sharedDb().accounts.update(_row);
    db::bumpRowVersion(action.id);
    _seenVersion = db::rowVersion(action.id);
    return dto::CommandResult{.ok = true, .message = "account closed"};
}

}  // namespace bank
