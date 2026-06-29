// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Loan model: applications, repayments, and a computed
/// amortization schedule.

namespace bank::dto {

/// @brief A loan and its current state.
struct LoanInfo {
    std::int64_t id = 0;
    std::string owner;
    std::int64_t accountId = 0;
    std::int64_t principalMinor = 0;
    std::int64_t outstandingMinor = 0;
    int currency = 0;
    int rateBps = 0;
    int termMonths = 0;
    int status = 0;  ///< bank::LoanStatus
    std::int64_t createdAtMs = 0;
};

/// @brief Apply for a loan, disbursed into an account.
struct ApplyLoan {
    std::int64_t accountId = 0;
    std::int64_t principalMinor = 0;
    int rateBps = 0;
    int termMonths = 0;

    [[nodiscard]] bool validate() const {
        return accountId > 0 && principalMinor > 0 && rateBps >= 0 && termMonths > 0;
    }
};

/// @brief Repay part (or all) of a loan from an account.
struct RepayLoan {
    std::int64_t loanId = 0;
    std::int64_t fromAccountId = 0;
    std::int64_t amountMinor = 0;

    [[nodiscard]] bool validate() const { return loanId > 0 && fromAccountId > 0 && amountMinor > 0; }
};

/// @brief Fetch one loan by id.
struct GetLoan {
    std::int64_t id = 0;
};

/// @brief List the current owner's loans.
struct ListLoans {
    std::string owner;  ///< empty => session principal
};

/// @brief Result of `ListLoans`.
struct LoanList {
    std::vector<LoanInfo> loans;
};

/// @brief One row of an amortization schedule.
struct Installment {
    int month = 0;
    std::int64_t paymentMinor = 0;
    std::int64_t principalMinor = 0;
    std::int64_t interestMinor = 0;
    std::int64_t remainingMinor = 0;
};

/// @brief Request the amortization schedule for a loan.
struct LoanScheduleRequest {
    std::int64_t loanId = 0;
};

/// @brief Result of `LoanScheduleRequest`.
struct LoanScheduleResult {
    std::int64_t loanId = 0;
    std::int64_t monthlyPaymentMinor = 0;
    std::vector<Installment> installments;
};

}  // namespace bank::dto
