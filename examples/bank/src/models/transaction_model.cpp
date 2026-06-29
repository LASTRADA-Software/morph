// SPDX-License-Identifier: Apache-2.0

#include "bank/models/transaction_model.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/ledger_ops.hpp"

namespace bank {

namespace {

dto::TxnInfo toTxnInfo(const db::TxnRecord& rec) {
    return dto::TxnInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .accountId = rec.accountId.Value(),
        .counterpartyId = rec.counterpartyId.Value(),
        .direction = rec.direction.Value(),
        .kind = rec.kind.Value(),
        .amountMinor = rec.amountMinor.Value(),
        .currency = rec.currency.Value(),
        .balanceAfterMinor = rec.balanceAfterMinor.Value(),
        .description = std::string{rec.description.Value().str()},
        .createdAtMs = rec.createdAtMs.Value(),
    };
}

}  // namespace

dto::TxnInfo TransactionModel::execute(const dto::Deposit& action) {
    if (!action.validate()) {
        throw ValidationError{"deposit amount must be positive"};
    }
    auto& dm = mapper();
    auto account = db::loadOwnedOpenAccount(dm, action.accountId, sessionPrincipal());
    // Balance update and its ledger entry must commit (or roll back) as a unit.
    Lightweight::SqlTransaction tx{dm.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto txn = db::applyCredit(dm, account, action.amountMinor, TxnKind::Deposit, 0, action.description);
    tx.Commit();
    return toTxnInfo(txn);
}

dto::TxnInfo TransactionModel::execute(const dto::Withdraw& action) {
    if (!action.validate()) {
        throw ValidationError{"withdrawal amount must be positive"};
    }
    auto& dm = mapper();
    auto account = db::loadOwnedOpenAccount(dm, action.accountId, sessionPrincipal());
    Lightweight::SqlTransaction tx{dm.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto txn = db::applyDebit(dm, account, action.amountMinor, TxnKind::Withdrawal, 0, action.description);
    tx.Commit();
    return toTxnInfo(txn);
}

dto::TransferResult TransactionModel::execute(const dto::Transfer& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid transfer (accounts must differ and amount be positive)"};
    }
    auto& dm = mapper();
    const std::string owner = sessionPrincipal();
    auto source = db::loadOwnedOpenAccount(dm, action.fromAccountId, owner);
    auto dest = db::loadOwnedOpenAccount(dm, action.toAccountId, owner);
    if (source.currency.Value() != dest.currency.Value()) {
        throw ValidationError{"cross-currency transfers are not supported"};
    }

    Lightweight::SqlTransaction tx{dm.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::applyDebit(dm, source, action.amountMinor, TxnKind::TransferOut, action.toAccountId,
                   action.description);
    db::applyCredit(dm, dest, action.amountMinor, TxnKind::TransferIn, action.fromAccountId,
                    action.description);
    tx.Commit();

    return dto::TransferResult{.fromBalanceMinor = source.balanceMinor.Value(),
                               .toBalanceMinor = dest.balanceMinor.Value()};
}

dto::HistoryPage TransactionModel::execute(const dto::History& action) {
    dto::HistoryPage page;
    page.accountId = action.accountId;
    const auto offset = static_cast<std::size_t>(std::max(0, action.offset));
    const auto limit = static_cast<std::size_t>(std::max(0, action.limit));
    if (limit == 0) {
        return page;
    }

    // Newest first, paginated in the database: only the requested window is
    // fetched instead of loading and sorting the whole ledger in memory.
    auto rows = mapper()
                    .Query<db::TxnRecord>()
                    .Where(Lightweight::FieldNameOf<&db::TxnRecord::accountId>, "=", action.accountId)
                    .OrderBy(Lightweight::FieldNameOf<&db::TxnRecord::id>,
                             Lightweight::SqlResultOrdering::DESCENDING)
                    .Range(offset, limit);

    page.entries.reserve(rows.size());
    for (const auto& rec : rows) {
        page.entries.push_back(toTxnInfo(rec));
    }
    return page;
}

}  // namespace bank
