// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of PayeeModel for the WASM build.

#include "bank/models/payee_model.hpp"

#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

dto::PayeeInfo toInfo(const wasm::PayeeRow& rec, const std::string& owner) {
    return dto::PayeeInfo{
        .id = static_cast<std::int64_t>(rec.id),
        .owner = owner,
        .name = rec.name,
        .iban = rec.iban,
        .bankName = rec.bankName,
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
    auto& db = wasm::sharedDb();
    wasm::PayeeRow rec;
    rec.userId = wasm::requireUserId(db, owner);
    rec.name = action.name;
    rec.iban = action.iban;
    rec.bankName = action.bankName;
    rec.id = db.payees.insert(rec);
    return toInfo(rec, owner);
}

dto::CommandResult PayeeModel::execute(const dto::RemovePayee& action) {
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, sessionPrincipal());
    auto* payee = db.payees.find(static_cast<std::uint64_t>(action.id));
    if (payee == nullptr) {
        throw NotFound{"payee not found"};
    }
    if (payee->userId != ownerId) {
        throw Unauthorized{"payee belongs to a different owner"};
    }
    db.payees.erase(payee->id);
    return dto::CommandResult{.ok = true, .message = "payee removed"};
}

dto::PayeeList PayeeModel::execute(const dto::ListPayees& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    dto::PayeeList out;
    for (const auto& rec : db.payees.where([&](const wasm::PayeeRow& p) { return p.userId == ownerId; })) {
        out.payees.push_back(toInfo(rec, owner));
    }
    return out;
}

}  // namespace bank
