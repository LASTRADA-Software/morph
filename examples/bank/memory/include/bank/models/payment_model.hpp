// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/payment_model.hpp (in-memory backend).

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bank/dto/common.hpp"
#include "bank/dto/payment_dto.hpp"

namespace bank {

/// @brief Pays beneficiaries and manages scheduled / standing instructions (in-memory).
class PaymentModel {
public:
    dto::PaymentInfo execute(const dto::PayBill& action);
    dto::PaymentInfo execute(const dto::SchedulePayment& action);
    dto::PaymentInfo execute(const dto::CreateStandingOrder& action);
    dto::CommandResult execute(const dto::CancelPayment& action);
    dto::PaymentList execute(const dto::ListPayments& action);
};

}  // namespace bank

using bank::PaymentModel;
using bank::dto::CancelPayment;
using bank::dto::CreateStandingOrder;
using bank::dto::ListPayments;
using bank::dto::PayBill;
using bank::dto::SchedulePayment;

BRIDGE_REGISTER_MODEL(PaymentModel, "PaymentModel")
BRIDGE_REGISTER_ACTION(PaymentModel, PayBill, "PayBill")
BRIDGE_REGISTER_ACTION(PaymentModel, SchedulePayment, "SchedulePayment")
BRIDGE_REGISTER_ACTION(PaymentModel, CreateStandingOrder, "CreateStandingOrder")
BRIDGE_REGISTER_ACTION(PaymentModel, CancelPayment, "CancelPayment")
BRIDGE_REGISTER_ACTION(PaymentModel, ListPayments, "ListPayments")
