// SPDX-License-Identifier: Apache-2.0

#include "bank/models/statement_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string>

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

    for (const auto& account : accounts) {
        const auto accountId = static_cast<std::int64_t>(account.id.Value());
        auto entries = mapper()
                           .Query<db::TxnRecord>()
                           .Where(Lightweight::FieldNameOf<&db::TxnRecord::accountId>, "=", accountId)
                           .All();

        dto::StatementLine line;
        line.accountId = accountId;
        line.number = std::string{account.number.Value().str()};
        line.currency = account.currency.Value();
        line.closingBalanceMinor = account.balanceMinor.Value();

        for (const auto& entry : entries) {
            const std::int64_t when = entry.createdAtMs.Value();
            if (when < action.fromMs) {
                continue;
            }
            if (action.toMs != 0 && when > action.toMs) {
                continue;
            }
            if (entry.direction.Value() == static_cast<int>(TxnDirection::Credit)) {
                line.creditsMinor += entry.amountMinor.Value();
            } else {
                line.debitsMinor += entry.amountMinor.Value();
            }
            line.entryCount += 1;
        }

        statement.totalCreditsMinor += line.creditsMinor;
        statement.totalDebitsMinor += line.debitsMinor;
        statement.lines.push_back(std::move(line));
    }

    return statement;
}

}  // namespace bank
