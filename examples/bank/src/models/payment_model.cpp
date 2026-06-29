// SPDX-License-Identifier: Apache-2.0

#include "bank/models/payment_model.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/ledger_ops.hpp"
#include "bank/db/payee_entity.hpp"
#include "bank/db/payment_entity.hpp"

namespace bank {

namespace {

dto::PaymentInfo toInfo(const db::PaymentRecord& rec) {
    return dto::PaymentInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = std::string{rec.owner.Value().str()},
        .fromAccountId = rec.fromAccountId.Value(),
        .payeeId = rec.payeeId.Value(),
        .amountMinor = rec.amountMinor.Value(),
        .currency = rec.currency.Value(),
        .schedule = rec.schedule.Value(),
        .status = rec.status.Value(),
        .dueAtMs = rec.dueAtMs.Value(),
        .intervalDays = rec.intervalDays.Value(),
        .description = std::string{rec.description.Value().str()},
    };
}

/// Loads a payee, requiring it to exist and belong to @p owner.
db::PayeeRecord requireOwnedPayee(Lightweight::DataMapper& mapper, std::int64_t payeeId,
                                  const std::string& owner) {
    auto payee = mapper.QuerySingle<db::PayeeRecord>(static_cast<std::uint64_t>(payeeId));
    if (!payee.has_value()) {
        throw NotFound{"payee not found"};
    }
    if (std::string{payee->owner.Value().str()} != owner) {
        throw Unauthorized{"payee belongs to a different owner"};
    }
    return *payee;
}

/// Loads an open account, requiring it to belong to @p owner.
db::AccountRecord requireOwnedAccount(Lightweight::DataMapper& mapper, std::int64_t accountId,
                                      const std::string& owner) {
    auto account = db::loadOpenAccount(mapper, accountId);
    if (std::string{account.owner.Value().str()} != owner) {
        throw Unauthorized{"account belongs to a different owner"};
    }
    return account;
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
    auto& dm = mapper();
    auto account = requireOwnedAccount(dm, action.fromAccountId, owner);
    requireOwnedPayee(dm, action.payeeId, owner);

    db::PaymentRecord payment;
    payment.owner = Light::SqlAnsiString<64>{owner};
    payment.fromAccountId = action.fromAccountId;
    payment.payeeId = action.payeeId;
    payment.amountMinor = action.amountMinor;
    payment.currency = account.currency.Value();
    payment.schedule = static_cast<int>(PaymentSchedule::OneOff);
    payment.status = static_cast<int>(PaymentStatus::Completed);
    payment.dueAtMs = db::nowMillis();
    payment.intervalDays = 0;
    payment.description = Light::SqlAnsiString<128>{action.description};

    Lightweight::SqlTransaction tx{dm.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::applyDebit(dm, account, action.amountMinor, TxnKind::Payment, action.payeeId, action.description);
    dm.Create(payment);
    tx.Commit();

    return toInfo(payment);
}

dto::PaymentInfo PaymentModel::execute(const dto::SchedulePayment& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid scheduled payment"};
    }
    const std::string owner = sessionPrincipal();
    auto& dm = mapper();
    auto account = requireOwnedAccount(dm, action.fromAccountId, owner);
    requireOwnedPayee(dm, action.payeeId, owner);

    db::PaymentRecord payment;
    payment.owner = Light::SqlAnsiString<64>{owner};
    payment.fromAccountId = action.fromAccountId;
    payment.payeeId = action.payeeId;
    payment.amountMinor = action.amountMinor;
    payment.currency = account.currency.Value();
    payment.schedule = static_cast<int>(PaymentSchedule::Scheduled);
    payment.status = static_cast<int>(PaymentStatus::Pending);
    payment.dueAtMs = action.dueAtMs;
    payment.intervalDays = 0;
    payment.description = Light::SqlAnsiString<128>{action.description};
    dm.Create(payment);
    return toInfo(payment);
}

dto::PaymentInfo PaymentModel::execute(const dto::CreateStandingOrder& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid standing order"};
    }
    const std::string owner = sessionPrincipal();
    auto& dm = mapper();
    auto account = requireOwnedAccount(dm, action.fromAccountId, owner);
    requireOwnedPayee(dm, action.payeeId, owner);

    db::PaymentRecord payment;
    payment.owner = Light::SqlAnsiString<64>{owner};
    payment.fromAccountId = action.fromAccountId;
    payment.payeeId = action.payeeId;
    payment.amountMinor = action.amountMinor;
    payment.currency = account.currency.Value();
    payment.schedule = static_cast<int>(PaymentSchedule::Standing);
    payment.status = static_cast<int>(PaymentStatus::Pending);
    payment.dueAtMs = action.firstDueAtMs;
    payment.intervalDays = action.intervalDays;
    payment.description = Light::SqlAnsiString<128>{action.description};
    dm.Create(payment);
    return toInfo(payment);
}

dto::CommandResult PaymentModel::execute(const dto::CancelPayment& action) {
    auto payment = mapper().QuerySingle<db::PaymentRecord>(static_cast<std::uint64_t>(action.id));
    if (!payment.has_value()) {
        throw NotFound{"payment not found"};
    }
    if (std::string{payment->owner.Value().str()} != sessionPrincipal()) {
        throw Unauthorized{"payment belongs to a different owner"};
    }
    if (payment->status.Value() != static_cast<int>(PaymentStatus::Pending)) {
        throw ConflictError{"only pending payments can be cancelled"};
    }
    payment->status = static_cast<int>(PaymentStatus::Cancelled);
    mapper().Update(*payment);
    return dto::CommandResult{.ok = true, .message = "payment cancelled"};
}

dto::PaymentList PaymentModel::execute(const dto::ListPayments& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto rows = mapper()
                    .Query<db::PaymentRecord>()
                    .Where(Lightweight::FieldNameOf<&db::PaymentRecord::owner>, "=", owner)
                    .All();
    dto::PaymentList out;
    out.payments.reserve(rows.size());
    for (const auto& rec : rows) {
        out.payments.push_back(toInfo(rec));
    }
    return out;
}

}  // namespace bank
