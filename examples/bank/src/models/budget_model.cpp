// SPDX-License-Identifier: Apache-2.0

#include "bank/models/budget_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <map>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/budget_entity.hpp"
#include "bank/db/txn_entity.hpp"

namespace bank {

namespace {

dto::BudgetInfo toInfo(const db::BudgetRecord& rec) {
    return dto::BudgetInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = std::string{rec.owner.Value().str()},
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

    // Upsert: update the existing row for (owner, category) or create a new one.
    auto existing = mapper()
                        .Query<db::BudgetRecord>()
                        .Where(Lightweight::FieldNameOf<&db::BudgetRecord::owner>, "=", owner)
                        .Where(Lightweight::FieldNameOf<&db::BudgetRecord::category>, "=", action.category)
                        .All();
    if (!existing.empty()) {
        auto rec = existing.front();
        rec.monthlyLimitMinor = action.monthlyLimitMinor;
        rec.currency = action.currency;
        mapper().Update(rec);
        return toInfo(rec);
    }

    db::BudgetRecord rec;
    rec.owner = Light::SqlAnsiString<64>{owner};
    rec.category = Light::SqlAnsiString<64>{action.category};
    rec.monthlyLimitMinor = action.monthlyLimitMinor;
    rec.currency = action.currency;
    mapper().Create(rec);
    return toInfo(rec);
}

dto::CommandResult BudgetModel::execute(const dto::DeleteBudget& action) {
    auto rec = mapper().QuerySingle<db::BudgetRecord>(static_cast<std::uint64_t>(action.id));
    if (!rec.has_value()) {
        throw NotFound{"budget not found"};
    }
    if (std::string{rec->owner.Value().str()} != sessionPrincipal()) {
        throw Unauthorized{"budget belongs to a different owner"};
    }
    mapper().Delete(*rec);
    return dto::CommandResult{.ok = true, .message = "budget deleted"};
}

dto::BudgetList BudgetModel::execute(const dto::ListBudgets& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto rows = mapper()
                    .Query<db::BudgetRecord>()
                    .Where(Lightweight::FieldNameOf<&db::BudgetRecord::owner>, "=", owner)
                    .All();
    dto::BudgetList out;
    out.budgets.reserve(rows.size());
    for (const auto& rec : rows) {
        out.budgets.push_back(toInfo(rec));
    }
    return out;
}

dto::SpendingReport BudgetModel::execute(const dto::SpendingByKind& action) {
    auto rows = mapper()
                    .Query<db::TxnRecord>()
                    .Where(Lightweight::FieldNameOf<&db::TxnRecord::accountId>, "=", action.accountId)
                    .All();

    // Aggregate debits by kind in code (kept on the typed DataMapper path rather
    // than hand-written GROUP BY SQL).
    std::map<int, dto::KindSpend> byKind;
    std::int64_t totalDebits = 0;
    for (const auto& rec : rows) {
        if (rec.direction.Value() != static_cast<int>(TxnDirection::Debit)) {
            continue;
        }
        if (rec.createdAtMs.Value() < action.sinceMs) {
            continue;
        }
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
