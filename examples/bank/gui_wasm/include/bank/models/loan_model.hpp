// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/loan_model.hpp (in-memory backend).

#include <morph/core/registry.hpp>
#include <morph/core/bridge.hpp>

#include "bank/dto/loan_dto.hpp"

namespace bank {

/// @brief Originates and services loans (in-memory).
class LoanModel {
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
BRIDGE_REGISTER_ACTION(LoanModel, GetLoan, "GetLoan")
BRIDGE_REGISTER_ACTION(LoanModel, ListLoans, "ListLoans")
BRIDGE_REGISTER_ACTION(LoanModel, LoanScheduleRequest, "LoanScheduleRequest")
