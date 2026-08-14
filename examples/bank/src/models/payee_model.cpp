// SPDX-License-Identifier: Apache-2.0

#include "bank/models/payee_model.hpp"

#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/db/ledger_ops.hpp"
#include "bank/db/user_ops.hpp"

namespace bank {

namespace {

/// Works for either the `PayeeRecord` aggregate or the `PayeeRow` projection —
/// both expose the same scalar fields.
template <typename Record>
dto::PayeeInfo toInfo(const Record& rec, const std::string& owner) {
    return dto::PayeeInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = owner,
        .name = std::string{rec.name.Value().str()},
        .iban = std::string{rec.iban.Value().str()},
        .bankName = std::string{rec.bankName.Value().str()},
    };
}

}  // namespace

dto::PayeeInfo PayeeModel::execute(const dto::AddPayee& action) {
    if (!action.validate()) {
        throw ValidationError{"payee needs a name and a valid IBAN"};
    }
    const std::string owner = sessionPrincipal();
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }

    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PayeeRecord rec;
    db::setReference(rec.user, db::requireUserId(mapper.Get(), owner));
    rec.name = Light::SqlAnsiString<128>{action.name};
    rec.iban = Light::SqlAnsiString<34>{action.iban};
    rec.bankName = Light::SqlAnsiString<128>{action.bankName};
    mapper->Create(rec);
    return toInfo(rec, owner);
}

dto::CommandResult PayeeModel::execute(const dto::RemovePayee& action) {
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto rec = db::loadOwned<db::PayeeRecord>(mapper.Get(), action.id, sessionPrincipal(), "payee");
    mapper->Delete(rec);
    return dto::CommandResult{.ok = true, .message = "payee removed"};
}

dto::PayeeList PayeeModel::execute(const dto::ListPayees& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    // Fluent list query uses the relation-free `PayeeRow` projection (the
    // `PayeeRecord` aggregate carries a `HasMany` the fluent builder can't select).
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto userId = db::requireUserId(mapper.Get(), owner);
    auto rows = mapper
                    ->Query<db::PayeeRow>()
                    .Where(Lightweight::FieldNameOf<&db::PayeeRow::user>, "=", userId)
                    .All();
    dto::PayeeList out;
    out.payees.reserve(rows.size());
    for (const auto& rec : rows) {
        out.payees.push_back(toInfo(rec, owner));
    }
    return out;
}

}  // namespace bank
