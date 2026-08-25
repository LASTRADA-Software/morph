// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Payment model: one-off bill payments, future-dated
/// scheduled payments, and recurring standing orders.

namespace bank::dto {

/// @brief A payment instruction in any of its lifecycle states.
struct PaymentInfo {
    std::int64_t id = 0;
    std::string owner;
    std::int64_t fromAccountId = 0;
    std::int64_t payeeId = 0;
    std::int64_t amountMinor = 0;
    int currency = 0;
    int schedule = 0;  ///< bank::PaymentSchedule
    int status = 0;    ///< bank::PaymentStatus
    std::int64_t dueAtMs = 0;
    int intervalDays = 0;
    std::string description;
};

/// @brief Pay a beneficiary immediately from an account.
struct PayBill {
    std::int64_t fromAccountId = 0;
    std::int64_t payeeId = 0;
    std::int64_t amountMinor = 0;
    std::string description;

    [[nodiscard]] bool validate() const { return fromAccountId > 0 && payeeId > 0 && amountMinor > 0; }
};

/// @brief Schedule a one-time future payment.
struct SchedulePayment {
    std::int64_t fromAccountId = 0;
    std::int64_t payeeId = 0;
    std::int64_t amountMinor = 0;
    std::int64_t dueAtMs = 0;
    std::string description;

    [[nodiscard]] bool validate() const { return fromAccountId > 0 && payeeId > 0 && amountMinor > 0 && dueAtMs > 0; }
};

/// @brief Create a recurring standing order.
struct CreateStandingOrder {
    std::int64_t fromAccountId = 0;
    std::int64_t payeeId = 0;
    std::int64_t amountMinor = 0;
    int intervalDays = 0;
    std::int64_t firstDueAtMs = 0;
    std::string description;

    [[nodiscard]] bool validate() const {
        return fromAccountId > 0 && payeeId > 0 && amountMinor > 0 && intervalDays > 0;
    }
};

/// @brief Cancel a pending scheduled/standing payment.
struct CancelPayment {
    std::int64_t id = 0;
};

/// @brief List the current owner's payments.
struct ListPayments {
    std::string owner;  ///< empty => session principal
};

/// @brief Result of `ListPayments`.
struct PaymentList {
    std::vector<PaymentInfo> payments;
};

}  // namespace bank::dto
