// SPDX-License-Identifier: Apache-2.0

#include "bank/models/budget_model.hpp"

#include <Lightweight/Lightweight.hpp>
#include <cstdint>
#include <map>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/ledger_ops.hpp"
#include "bank/db/user_ops.hpp"

namespace bank {

namespace {

dto::BudgetInfo toInfo(const db::BudgetRecord& rec, const std::string& owner) {
    return dto::BudgetInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = owner,
        .category = std::string{rec.category.Value().str()},
        .monthlyLimitMinor = rec.monthlyLimitMinor.Value(),
        .currency = rec.currency.Value(),
    };
}

}  // namespace

dto::BudgetInfo BudgetModel::execute(const dto::SetBudget& action) {
    if (!action.validate()) {
        throw ValidationError{"category required and limit must be non-negative"};
    }
    const std::string owner = sessionPrincipal();
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }

    // Upsert: update the existing row for (user, category) or create a new one.
    const auto userId = db::requireUserId(mapper(), owner);
    auto existing = mapper()
                        .Query<db::BudgetRecord>()
                        .Where(Lightweight::FieldNameOf<&db::BudgetRecord::user>, "=", userId)
                        .Where(Lightweight::FieldNameOf<&db::BudgetRecord::category>, "=", action.category)
                        .All();
    if (!existing.empty()) {
        auto rec = existing.front();
        rec.monthlyLimitMinor = action.monthlyLimitMinor;
        rec.currency = action.currency;
        mapper().Update(rec);
        return toInfo(rec, owner);
    }

    db::BudgetRecord rec;
    db::setReference(rec.user, userId);
    rec.category = Light::SqlAnsiString<64>{action.category};
    rec.monthlyLimitMinor = action.monthlyLimitMinor;
    rec.currency = action.currency;
    mapper().Create(rec);
    return toInfo(rec, owner);
}

dto::CommandResult BudgetModel::execute(const dto::DeleteBudget& action) {
    auto rec = db::loadOwned<db::BudgetRecord>(mapper(), action.id, sessionPrincipal(), "budget");
    mapper().Delete(rec);
    return dto::CommandResult{.ok = true, .message = "budget deleted"};
}

dto::BudgetList BudgetModel::execute(const dto::ListBudgets& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    const auto userId = db::requireUserId(mapper(), owner);
    auto rows =
        mapper().Query<db::BudgetRecord>().Where(Lightweight::FieldNameOf<&db::BudgetRecord::user>, "=", userId).All();
    dto::BudgetList out;
    out.budgets.reserve(rows.size());
    for (const auto& rec : rows) {
        out.budgets.push_back(toInfo(rec, owner));
    }
    return out;
}

dto::SpendingReport BudgetModel::execute(const dto::SpendingByKind& action) {
    // Push the account/direction/time filters into the query so only the rows we
    // aggregate cross the wire; the by-kind rollup stays in code (no GROUP BY SQL).
    auto rows =
        mapper()
            .Query<db::TxnRecord>()
            .Where(Lightweight::FieldNameOf<&db::TxnRecord::account>, "=", action.accountId)
            .Where(Lightweight::FieldNameOf<&db::TxnRecord::direction>, "=", static_cast<int>(TxnDirection::Debit))
            .Where(Lightweight::FieldNameOf<&db::TxnRecord::createdAtMs>, ">=", action.sinceMs)
            .All();

    std::map<int, dto::KindSpend> byKind;
    std::int64_t totalDebits = 0;
    for (const auto& rec : rows) {
        const int kind = rec.kind.Value();
        auto& entry = byKind[kind];
        entry.kind = kind;
        entry.totalMinor += rec.amountMinor.Value();
        entry.count += 1;
        totalDebits += rec.amountMinor.Value();
    }

    dto::SpendingReport report;
    report.accountId = action.accountId;
    report.totalDebitsMinor = totalDebits;
    report.byKind.reserve(byKind.size());
    for (const auto& [kind, spend] : byKind) {
        report.byKind.push_back(spend);
    }
    return report;
}

}  // namespace bank
