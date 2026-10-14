// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of TransactionModel for the WASM build. Mirrors
// src/models/transaction_model.cpp (single-threaded, so no SqlTransaction).

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

dto::TxnInfo toTxnInfo(const wasm::TxnRow& rec) {
    return dto::TxnInfo{
        .id = static_cast<std::int64_t>(rec.id),
        .accountId = static_cast<std::int64_t>(rec.accountId),
        .counterpartyId = rec.counterpartyId,
        .direction = rec.direction,
        .kind = rec.kind,
        .amountMinor = rec.amountMinor,
        .currency = rec.currency,
        .balanceAfterMinor = rec.balanceAfterMinor,
        .description = rec.description,
        .createdAtMs = rec.createdAtMs,
    };
}

}  // namespace

dto::TxnInfo TransactionModel::execute(const dto::Deposit& action) {
    if (!action.validate()) {
        throw ValidationError{"deposit amount must be positive"};
    }
    auto& db = wasm::sharedDb();
    auto account = wasm::loadOwnedOpenAccount(db, action.accountId, wasm::requireUserId(db, sessionPrincipal()));
    return toTxnInfo(wasm::applyCredit(db, account, action.amountMinor, TxnKind::Deposit, 0, action.description));
}

dto::TxnInfo TransactionModel::execute(const dto::Withdraw& action) {
    if (!action.validate()) {
        throw ValidationError{"withdrawal amount must be positive"};
    }
    auto& db = wasm::sharedDb();
    auto account = wasm::loadOwnedOpenAccount(db, action.accountId, wasm::requireUserId(db, sessionPrincipal()));
    return toTxnInfo(wasm::applyDebit(db, account, action.amountMinor, TxnKind::Withdrawal, 0, action.description));
}

dto::TransferResult TransactionModel::execute(const dto::Transfer& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid transfer (accounts must differ and amount be positive)"};
    }
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, sessionPrincipal());
    auto source = wasm::loadOwnedOpenAccount(db, action.fromAccountId, ownerId);
    auto dest = wasm::loadOwnedOpenAccount(db, action.toAccountId, ownerId);
    if (source.currency != dest.currency) {
        throw ValidationError{"cross-currency transfers are not supported"};
    }
    wasm::applyDebit(db, source, action.amountMinor, TxnKind::TransferOut, action.toAccountId, action.description);
    wasm::applyCredit(db, dest, action.amountMinor, TxnKind::TransferIn, action.fromAccountId, action.description);
    return dto::TransferResult{.fromBalanceMinor = source.balanceMinor, .toBalanceMinor = dest.balanceMinor};
}

dto::HistoryPage TransactionModel::execute(const dto::History& action) {
    dto::HistoryPage page;
    page.accountId = action.accountId;
    const auto offset = static_cast<std::size_t>(std::max(0, action.offset));
    const auto limit = static_cast<std::size_t>(std::max(0, action.limit));
    if (limit == 0) {
        return page;
    }
    auto& db = wasm::sharedDb();
    auto rows = db.txns.where(
        [&](const wasm::TxnRow& t) { return t.accountId == static_cast<std::uint64_t>(action.accountId); });
    // Newest first.
    std::ranges::sort(rows, [](const wasm::TxnRow& a, const wasm::TxnRow& b) { return a.id > b.id; });
    for (std::size_t i = offset; i < rows.size() && i < offset + limit; ++i) {
        page.entries.push_back(toTxnInfo(rows[i]));
    }
    return page;
}

}  // namespace bank
