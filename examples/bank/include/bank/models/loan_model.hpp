// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/registry.hpp>
#include <morph/core/bridge.hpp>

#include "bank/db/db_model.hpp"
#include "bank/dto/loan_dto.hpp"

/// @file
/// The Loan model: apply (disbursing the principal into an account), repay
/// (debiting an account and reducing the outstanding balance), and compute an
/// amortization schedule. Disbursement and repayment each touch an account and
/// the ledger inside a single `SqlTransaction`.

namespace bank {

/// @brief Originates and services loans.
class LoanModel : private db::WithMapper {
public:
    dto::LoanInfo execute(const dto::ApplyLoan& action);
    dto::LoanInfo execute(const dto::RepayLoan& action);
    dto::LoanInfo execute(const dto::GetLoan& action);
    dto::LoanList execute(const dto::ListLoans& action);
    dto::LoanScheduleResult execute(const dto::LoanScheduleRequest& action);
};

}  // namespace bank

using bank::LoanModel;
using bank::dto::ApplyLoan;
using bank::dto::GetLoan;
using bank::dto::ListLoans;
using bank::dto::LoanScheduleRequest;
using bank::dto::RepayLoan;

BRIDGE_REGISTER_MODEL(LoanModel, "LoanModel")
BRIDGE_REGISTER_ACTION(LoanModel, ApplyLoan, "ApplyLoan")
BRIDGE_REGISTER_ACTION(LoanModel, RepayLoan, "RepayLoan")
BRIDGE_REGISTER_ACTION(LoanModel, GetLoan, "GetLoan", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(LoanModel, ListLoans, "ListLoans", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(LoanModel, LoanScheduleRequest, "LoanScheduleRequest", ::morph::model::Loggable::No)
