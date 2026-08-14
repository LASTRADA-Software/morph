// SPDX-License-Identifier: Apache-2.0

#include "bank/models/account_model.hpp"

#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/Lightweight.hpp>
#include <morph/core/registry.hpp>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/account_mapping.hpp"
#include "bank/db/ledger_ops.hpp"
#include "bank/db/row_versions.hpp"

namespace bank {

void AccountModel::hydrate(std::int64_t accountId) {
    const std::string owner = sessionPrincipal();
    // Re-read only when we have to: a different account, nothing cached yet, a
    // different principal asking, or somebody else wrote the row (a transfer or
    // bill payment settling on another model's connection — see
    // db/row_versions.hpp). Otherwise the answer is already in memory, which is
    // the entire point of a stateful model.
    const auto current = db::rowVersion(accountId);
    if (_loadedId == accountId && _owner == owner && _seenVersion == current) {
        return;
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    _row = db::loadOwned<db::AccountRecord>(mapper.Get(), accountId, owner, "account");
    _owner = owner;
    _loadedId = accountId;
    _seenVersion = current;
}

dto::AccountInfo AccountModel::execute(const dto::GetAccount& action) {
    hydrate(action.id);
    return db::toAccountInfo(_row, _owner);
}

dto::CommandResult AccountModel::execute(const dto::CloseAccount& action) {
    hydrate(action.id);
    // Best-effort zero-balance guard. The balance is read from the cached
    // `_row`, so a deposit committing on another model's connection (or
    // even another acquisition of this same pool) between that read and the
    // Update below could leave a Closed account holding funds — the same
    // cross-connection window documented in ledger_ops.hpp. A production
    // ledger would close the account inside the same transaction that
    // settles its balance, or gate on an atomic conditional update.
    if (_row.balanceMinor.Value() != 0) {
        return dto::CommandResult{.ok = false, .message = "account balance must be zero before closing"};
    }
    _row.status = static_cast<int>(AccountStatus::Closed);
    ::Lightweight::GlobalDataMapperPool().Acquire()->Update(_row);
    // Write through, then publish the new version so any other cached holder of
    // this row re-hydrates rather than serving a stale status.
    db::bumpRowVersion(action.id);
    _seenVersion = db::rowVersion(action.id);
    return dto::CommandResult{.ok = true, .message = "account closed"};
}

}  // namespace bank
