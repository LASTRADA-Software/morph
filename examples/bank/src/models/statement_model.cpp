// SPDX-License-Identifier: Apache-2.0

#include "bank/models/statement_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/account_entity.hpp"
#include "bank/db/txn_entity.hpp"

namespace bank {

dto::Statement StatementModel::execute(const dto::GenerateStatement& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }

    auto accounts = mapper()
                        .Query<db::AccountRecord>()
                        .Where(Lightweight::FieldNameOf<&db::AccountRecord::owner>, "=", owner)
                        .All();

    dto::Statement statement;
    statement.owner = owner;
    statement.fromMs = action.fromMs;
    statement.toMs = action.toMs;

    // One statement line per account, indexed by id so the single txn query
    // below can be folded in without re-scanning.
    std::map<std::int64_t, std::size_t> lineIndex;
    std::vector<std::int64_t> accountIds;
    statement.lines.reserve(accounts.size());
    accountIds.reserve(accounts.size());
    for (const auto& account : accounts) {
        const auto accountId = static_cast<std::int64_t>(account.id.Value());
        dto::StatementLine line;
        line.accountId = accountId;
        line.number = std::string{account.number.Value().str()};
        line.currency = account.currency.Value();
        line.closingBalanceMinor = account.balanceMinor.Value();
        lineIndex.emplace(accountId, statement.lines.size());
        statement.lines.push_back(std::move(line));
        accountIds.push_back(accountId);
    }
    if (accountIds.empty()) {
        return statement;
    }

    // All of the owner's transactions in one query (no per-account round-trip);
    // the window's lower bound is pushed down, the optional upper bound
    // (toMs == 0 means "open ended") stays as a cheap in-loop check.
    auto entries = mapper()
                       .Query<db::TxnRecord>()
                       .WhereIn(Lightweight::FieldNameOf<&db::TxnRecord::accountId>, accountIds)
                       .Where(Lightweight::FieldNameOf<&db::TxnRecord::createdAtMs>, ">=", action.fromMs)
                       .All();

    for (const auto& entry : entries) {
        const std::int64_t when = entry.createdAtMs.Value();
        if (action.toMs != 0 && when > action.toMs) {
            continue;
        }
        const auto it = lineIndex.find(entry.accountId.Value());
        if (it == lineIndex.end()) {
            continue;
        }
        auto& line = statement.lines[it->second];
        if (entry.direction.Value() == static_cast<int>(TxnDirection::Credit)) {
            line.creditsMinor += entry.amountMinor.Value();
        } else {
            line.debitsMinor += entry.amountMinor.Value();
        }
        line.entryCount += 1;
    }

    for (const auto& line : statement.lines) {
        statement.totalCreditsMinor += line.creditsMinor;
        statement.totalDebitsMinor += line.debitsMinor;
    }

    return statement;
}

}  // namespace bank
