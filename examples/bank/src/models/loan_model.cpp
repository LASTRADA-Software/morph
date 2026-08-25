// SPDX-License-Identifier: Apache-2.0

#include "bank/models/loan_model.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/ledger_ops.hpp"
#include "bank/db/user_ops.hpp"

namespace bank {

namespace {

dto::LoanInfo toInfo(const db::LoanRecord& rec, const std::string& owner) {
    return dto::LoanInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = owner,
        .accountId = static_cast<std::int64_t>(rec.account.Value()),
        .principalMinor = rec.principalMinor.Value(),
        .outstandingMinor = rec.outstandingMinor.Value(),
        .currency = rec.currency.Value(),
        .rateBps = rec.rateBps.Value(),
        .termMonths = rec.termMonths.Value(),
        .status = rec.status.Value(),
        .createdAtMs = rec.createdAtMs.Value(),
    };
}

db::LoanRecord requireOwnedLoan(Lightweight::DataMapper& mapper, std::int64_t loanId) {
    return db::loadOwned<db::LoanRecord>(mapper, loanId, sessionPrincipal(), "loan");
}

/// Fixed monthly payment (minor units) for an amortizing loan.
std::int64_t monthlyPayment(std::int64_t principalMinor, int rateBps, int termMonths) {
    const double principal = static_cast<double>(principalMinor);
    const double monthlyRate = static_cast<double>(rateBps) / 10000.0 / 12.0;
    if (monthlyRate <= 0.0) {
        return static_cast<std::int64_t>(std::llround(principal / termMonths));
    }
    const double factor = std::pow(1.0 + monthlyRate, -termMonths);
    return static_cast<std::int64_t>(std::llround(principal * monthlyRate / (1.0 - factor)));
}

}  // namespace

dto::LoanInfo LoanModel::execute(const dto::ApplyLoan& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid loan application"};
    }
    const std::string owner = sessionPrincipal();
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto& dm = mapper();
    auto account = db::loadOwnedOpenAccount(dm, action.accountId, owner);

    db::LoanRecord loan;
    db::setReference(loan.user, db::requireUserId(dm, owner));
    db::setReference(loan.account, account.id.Value());
    loan.principalMinor = action.principalMinor;
    loan.outstandingMinor = action.principalMinor;
    loan.currency = account.currency.Value();
    loan.rateBps = action.rateBps;
    loan.termMonths = action.termMonths;
    loan.status = static_cast<int>(LoanStatus::Active);
    loan.createdAtMs = db::nowMillis();

    Lightweight::SqlTransaction tx{dm.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    dm.Create(loan);
    db::applyCredit(dm, account, action.principalMinor, TxnKind::LoanDisbursement, 0, "loan disbursement");
    tx.Commit();

    return toInfo(loan, owner);
}

dto::LoanInfo LoanModel::execute(const dto::RepayLoan& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid repayment"};
    }
    auto& dm = mapper();
    const std::string owner = sessionPrincipal();
    auto loan = requireOwnedLoan(dm, action.loanId);
    if (loan.status.Value() != static_cast<int>(LoanStatus::Active)) {
        throw ConflictError{"loan is not active"};
    }
    auto account = db::loadOwnedOpenAccount(dm, action.fromAccountId, sessionPrincipal());
    const std::int64_t payment = std::min(action.amountMinor, loan.outstandingMinor.Value());

    Lightweight::SqlTransaction tx{dm.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::applyDebit(dm, account, payment, TxnKind::LoanRepayment, 0, "loan repayment");
    loan.outstandingMinor = loan.outstandingMinor.Value() - payment;
    if (loan.outstandingMinor.Value() <= 0) {
        loan.outstandingMinor = 0;
        loan.status = static_cast<int>(LoanStatus::PaidOff);
    }
    dm.Update(loan);
    tx.Commit();

    return toInfo(loan, owner);
}

dto::LoanInfo LoanModel::execute(const dto::GetLoan& action) {
    const std::string owner = sessionPrincipal();
    return toInfo(requireOwnedLoan(mapper(), action.id), owner);
}

dto::LoanList LoanModel::execute(const dto::ListLoans& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    const auto userId = db::requireUserId(mapper(), owner);
    auto rows =
        mapper().Query<db::LoanRecord>().Where(Lightweight::FieldNameOf<&db::LoanRecord::user>, "=", userId).All();
    dto::LoanList out;
    out.loans.reserve(rows.size());
    for (const auto& rec : rows) {
        out.loans.push_back(toInfo(rec, owner));
    }
    return out;
}

dto::LoanScheduleResult LoanModel::execute(const dto::LoanScheduleRequest& action) {
    auto loan = requireOwnedLoan(mapper(), action.loanId);

    const std::int64_t principal = loan.principalMinor.Value();
    const int rateBps = loan.rateBps.Value();
    const int term = loan.termMonths.Value();
    const double monthlyRate = static_cast<double>(rateBps) / 10000.0 / 12.0;
    const std::int64_t payment = monthlyPayment(principal, rateBps, term);

    dto::LoanScheduleResult out;
    out.loanId = action.loanId;
    out.monthlyPaymentMinor = payment;

    std::int64_t remaining = principal;
    for (int month = 1; month <= term && remaining > 0; ++month) {
        const std::int64_t interest =
            static_cast<std::int64_t>(std::llround(static_cast<double>(remaining) * monthlyRate));
        std::int64_t principalPart = payment - interest;
        if (month == term || principalPart > remaining) {
            principalPart = remaining;  // final instalment clears the balance
        }
        remaining -= principalPart;
        out.installments.push_back(dto::Installment{
            .month = month,
            .paymentMinor = principalPart + interest,
            .principalMinor = principalPart,
            .interestMinor = interest,
            .remainingMinor = remaining,
        });
    }
    return out;
}

}  // namespace bank
