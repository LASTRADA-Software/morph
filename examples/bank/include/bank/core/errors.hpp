// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>

/// @file
/// Domain exceptions. A model's `execute(...)` throws one of these; morph
/// captures it as a `std::exception_ptr` and delivers it to the caller's
/// `.onError(...)` callback on the GUI executor. On a remote backend the
/// `what()` string travels back in the error envelope.

namespace bank {

/// @brief Base class for all banking domain errors.
struct BankError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief A referenced entity (account, payee, loan, …) does not exist.
struct NotFound : BankError {
    using BankError::BankError;
};

/// @brief An account lacked the funds (incl. overdraft) to complete a debit.
struct InsufficientFunds : BankError {
    using BankError::BankError;
};

/// @brief The caller's session is not permitted to perform the action.
struct Unauthorized : BankError {
    using BankError::BankError;
};

/// @brief The action's inputs failed validation.
struct ValidationError : BankError {
    using BankError::BankError;
};

/// @brief The action conflicts with current state (e.g. closing a non-empty account).
struct ConflictError : BankError {
    using BankError::BankError;
};

}  // namespace bank
