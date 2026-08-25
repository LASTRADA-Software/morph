// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of PaymentModel for the WASM build.

#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/models/payment_model.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

dto::PaymentInfo toInfo(const wasm::PaymentRow& rec, const std::string& owner) {
    return dto::PaymentInfo{
        .id = static_cast<std::int64_t>(rec.id),
        .owner = owner,
        .fromAccountId = static_cast<std::int64_t>(rec.fromAccountId),
        .payeeId = static_cast<std::int64_t>(rec.payeeId),
        .amountMinor = rec.amountMinor,
        .currency = rec.currency,
        .schedule = rec.schedule,
        .status = rec.status,
        .dueAtMs = rec.dueAtMs,
        .intervalDays = rec.intervalDays,
        .description = rec.description,
    };
}

void requireOwnedPayee(wasm::Db& db, std::int64_t payeeId, std::uint64_t ownerId) {
    auto* payee = db.payees.find(static_cast<std::uint64_t>(payeeId));
    if (payee == nullptr) {
        throw NotFound{"payee not found"};
    }
    if (payee->userId != ownerId) {
        throw Unauthorized{"payee belongs to a different owner"};
    }
}

}  // namespace

dto::PaymentInfo PaymentModel::execute(const dto::PayBill& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid bill payment"};
    }
    const std::string owner = sessionPrincipal();
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    auto account = wasm::loadOwnedOpenAccount(db, action.fromAccountId, ownerId);
    requireOwnedPayee(db, action.payeeId, ownerId);

    wasm::PaymentRow payment;
    payment.userId = ownerId;
    payment.fromAccountId = static_cast<std::uint64_t>(action.fromAccountId);
    payment.payeeId = static_cast<std::uint64_t>(action.payeeId);
    payment.amountMinor = action.amountMinor;
    payment.currency = account.currency;
    payment.schedule = static_cast<int>(PaymentSchedule::OneOff);
    payment.status = static_cast<int>(PaymentStatus::Completed);
    payment.dueAtMs = wasm::nowMillis();
    payment.intervalDays = 0;
    payment.description = action.description;

    wasm::applyDebit(db, account, action.amountMinor, TxnKind::Payment, action.payeeId, action.description);
    payment.id = db.payments.insert(payment);
    return toInfo(payment, owner);
}

dto::PaymentInfo PaymentModel::execute(const dto::SchedulePayment& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid scheduled payment"};
    }
    const std::string owner = sessionPrincipal();
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    auto account = wasm::loadOwnedOpenAccount(db, action.fromAccountId, ownerId);
    requireOwnedPayee(db, action.payeeId, ownerId);

    wasm::PaymentRow payment;
    payment.userId = ownerId;
    payment.fromAccountId = static_cast<std::uint64_t>(action.fromAccountId);
    payment.payeeId = static_cast<std::uint64_t>(action.payeeId);
    payment.amountMinor = action.amountMinor;
    payment.currency = account.currency;
    payment.schedule = static_cast<int>(PaymentSchedule::Scheduled);
    payment.status = static_cast<int>(PaymentStatus::Pending);
    payment.dueAtMs = action.dueAtMs;
    payment.intervalDays = 0;
    payment.description = action.description;
    payment.id = db.payments.insert(payment);
    return toInfo(payment, owner);
}

dto::PaymentInfo PaymentModel::execute(const dto::CreateStandingOrder& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid standing order"};
    }
    const std::string owner = sessionPrincipal();
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    auto account = wasm::loadOwnedOpenAccount(db, action.fromAccountId, ownerId);
    requireOwnedPayee(db, action.payeeId, ownerId);

    wasm::PaymentRow payment;
    payment.userId = ownerId;
    payment.fromAccountId = static_cast<std::uint64_t>(action.fromAccountId);
    payment.payeeId = static_cast<std::uint64_t>(action.payeeId);
    payment.amountMinor = action.amountMinor;
    payment.currency = account.currency;
    payment.schedule = static_cast<int>(PaymentSchedule::Standing);
    payment.status = static_cast<int>(PaymentStatus::Pending);
    payment.dueAtMs = action.firstDueAtMs;
    payment.intervalDays = action.intervalDays;
    payment.description = action.description;
    payment.id = db.payments.insert(payment);
    return toInfo(payment, owner);
}

dto::CommandResult PaymentModel::execute(const dto::CancelPayment& action) {
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, sessionPrincipal());
    auto* payment = db.payments.find(static_cast<std::uint64_t>(action.id));
    if (payment == nullptr) {
        throw NotFound{"payment not found"};
    }
    if (payment->userId != ownerId) {
        throw Unauthorized{"payment belongs to a different owner"};
    }
    if (payment->status != static_cast<int>(PaymentStatus::Pending)) {
        throw ConflictError{"only pending payments can be cancelled"};
    }
    payment->status = static_cast<int>(PaymentStatus::Cancelled);
    db.payments.update(*payment);
    return dto::CommandResult{.ok = true, .message = "payment cancelled"};
}

dto::PaymentList PaymentModel::execute(const dto::ListPayments& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    dto::PaymentList out;
    for (const auto& rec : db.payments.where([&](const wasm::PaymentRow& p) { return p.userId == ownerId; })) {
        out.payments.push_back(toInfo(rec, owner));
    }
    return out;
}

}  // namespace bank
