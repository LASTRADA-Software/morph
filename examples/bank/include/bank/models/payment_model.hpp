// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/registry.hpp>
#include <morph/core/bridge.hpp>

#include "bank/dto/common.hpp"
#include "bank/dto/payment_dto.hpp"

/// @file
/// The Payment model: immediate bill payments (which debit an account and post
/// a ledger entry atomically), future-dated scheduled payments, and recurring
/// standing orders. Accounts and payees are validated to belong to the session
/// owner before any money moves.

namespace bank {

/// @brief Pays beneficiaries and manages scheduled / standing instructions.
///
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning one for its own lifetime.
class PaymentModel {
public:
    /// @brief Pays a beneficiary now; debits the account and records the payment.
    dto::PaymentInfo execute(const dto::PayBill& action);

    /// @brief Records a future-dated payment (no money moves yet).
    dto::PaymentInfo execute(const dto::SchedulePayment& action);

    /// @brief Records a recurring standing order (no money moves yet).
    dto::PaymentInfo execute(const dto::CreateStandingOrder& action);

    /// @brief Cancels a pending scheduled/standing payment.
    dto::CommandResult execute(const dto::CancelPayment& action);

    /// @brief Lists the current owner's payments.
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
BRIDGE_REGISTER_ACTION(PaymentModel, ListPayments, "ListPayments", ::morph::model::Loggable::No)
