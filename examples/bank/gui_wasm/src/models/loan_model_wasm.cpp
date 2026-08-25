// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of LoanModel for the WASM build.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/models/loan_model.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

dto::LoanInfo toInfo(const wasm::LoanRow& rec, const std::string& owner) {
    return dto::LoanInfo{
        .id = static_cast<std::int64_t>(rec.id),
        .owner = owner,
        .accountId = static_cast<std::int64_t>(rec.accountId),
        .principalMinor = rec.principalMinor,
        .outstandingMinor = rec.outstandingMinor,
        .currency = rec.currency,
        .rateBps = rec.rateBps,
        .termMonths = rec.termMonths,
        .status = rec.status,
        .createdAtMs = rec.createdAtMs,
    };
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

wasm::LoanRow requireOwnedLoan(wasm::Db& db, std::int64_t loanId) {
    const auto ownerId = wasm::requireUserId(db, sessionPrincipal());
    auto* loan = db.loans.find(static_cast<std::uint64_t>(loanId));
    if (loan == nullptr) {
        throw NotFound{"loan not found"};
    }
    if (loan->userId != ownerId) {
        throw Unauthorized{"loan belongs to a different owner"};
    }
    return *loan;
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
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    auto account = wasm::loadOwnedOpenAccount(db, action.accountId, ownerId);

    wasm::LoanRow loan;
    loan.userId = ownerId;
    loan.accountId = account.id;
    loan.principalMinor = action.principalMinor;
    loan.outstandingMinor = action.principalMinor;
    loan.currency = account.currency;
    loan.rateBps = action.rateBps;
    loan.termMonths = action.termMonths;
    loan.status = static_cast<int>(LoanStatus::Active);
    loan.createdAtMs = wasm::nowMillis();
    loan.id = db.loans.insert(loan);

    wasm::applyCredit(db, account, action.principalMinor, TxnKind::LoanDisbursement, 0, "loan disbursement");
    return toInfo(loan, owner);
}

dto::LoanInfo LoanModel::execute(const dto::RepayLoan& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid repayment"};
    }
    const std::string owner = sessionPrincipal();
    auto& db = wasm::sharedDb();
    auto loan = requireOwnedLoan(db, action.loanId);
    if (loan.status != static_cast<int>(LoanStatus::Active)) {
        throw ConflictError{"loan is not active"};
    }
    auto account = wasm::loadOwnedOpenAccount(db, action.fromAccountId, wasm::requireUserId(db, owner));
    const std::int64_t payment = std::min(action.amountMinor, loan.outstandingMinor);

    wasm::applyDebit(db, account, payment, TxnKind::LoanRepayment, 0, "loan repayment");
    loan.outstandingMinor -= payment;
    if (loan.outstandingMinor <= 0) {
        loan.outstandingMinor = 0;
        loan.status = static_cast<int>(LoanStatus::PaidOff);
    }
    db.loans.update(loan);
    return toInfo(loan, owner);
}

dto::LoanInfo LoanModel::execute(const dto::GetLoan& action) {
    const std::string owner = sessionPrincipal();
    return toInfo(requireOwnedLoan(wasm::sharedDb(), action.id), owner);
}

dto::LoanList LoanModel::execute(const dto::ListLoans& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    dto::LoanList out;
    for (const auto& rec : db.loans.where([&](const wasm::LoanRow& l) { return l.userId == ownerId; })) {
        out.loans.push_back(toInfo(rec, owner));
    }
    return out;
}

dto::LoanScheduleResult LoanModel::execute(const dto::LoanScheduleRequest& action) {
    auto loan = requireOwnedLoan(wasm::sharedDb(), action.loanId);

    const std::int64_t principal = loan.principalMinor;
    const int rateBps = loan.rateBps;
    const int term = loan.termMonths;
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
            principalPart = remaining;
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
