// SPDX-License-Identifier: Apache-2.0

#include "bank/models/payee_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/db/payee_entity.hpp"

namespace bank {

namespace {

dto::PayeeInfo toInfo(const db::PayeeRecord& rec) {
    return dto::PayeeInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = std::string{rec.owner.Value().str()},
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

    db::PayeeRecord rec;
    rec.owner = Light::SqlAnsiString<64>{owner};
    rec.name = Light::SqlAnsiString<128>{action.name};
    rec.iban = Light::SqlAnsiString<34>{action.iban};
    rec.bankName = Light::SqlAnsiString<128>{action.bankName};
    mapper().Create(rec);
    return toInfo(rec);
}

dto::CommandResult PayeeModel::execute(const dto::RemovePayee& action) {
    auto rec = mapper().QuerySingle<db::PayeeRecord>(static_cast<std::uint64_t>(action.id));
    if (!rec.has_value()) {
        throw NotFound{"payee not found"};
    }
    if (std::string{rec->owner.Value().str()} != sessionPrincipal()) {
        throw Unauthorized{"payee belongs to a different owner"};
    }
    mapper().Delete(*rec);
    return dto::CommandResult{.ok = true, .message = "payee removed"};
}

dto::PayeeList PayeeModel::execute(const dto::ListPayees& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto rows = mapper()
                    .Query<db::PayeeRecord>()
                    .Where(Lightweight::FieldNameOf<&db::PayeeRecord::owner>, "=", owner)
                    .All();
    dto::PayeeList out;
    out.payees.reserve(rows.size());
    for (const auto& rec : rows) {
        out.payees.push_back(toInfo(rec));
    }
    return out;
}

}  // namespace bank
