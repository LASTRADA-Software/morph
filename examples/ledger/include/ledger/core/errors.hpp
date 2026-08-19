// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace ledger {

class LedgerError : public std::runtime_error {
  public:
    explicit LedgerError(std::string message) : std::runtime_error{std::move(message)} {}
};

class ValidationError : public LedgerError {
  public:
    explicit ValidationError(std::string message) : LedgerError{std::move(message)} {}
};

class NotFound : public LedgerError {
  public:
    explicit NotFound(std::string message) : LedgerError{std::move(message)} {}
};

class Forbidden : public LedgerError {
  public:
    explicit Forbidden(std::string message) : LedgerError{std::move(message)} {}
};

/// @brief Thrown when a `StoreTransaction`'s legs, partitioned by currency,
///        do not sum to canonical zero for at least one partition. Never
///        thrown for rounding — the model never rounds (design spec §1).
class ZeroSumViolation : public LedgerError {
  public:
    ZeroSumViolation(std::string currency, std::string message)
        : LedgerError{"zero-sum violation in " + currency + ": " + message}, currencyCode{std::move(currency)} {}
    std::string currencyCode;
};

/// @brief Thrown when a mutating action dispatches with an empty principal
///        (design spec §11) — never silently proceeds.
class EmptyPrincipalError : public LedgerError {
  public:
    EmptyPrincipalError() : LedgerError{"mutating action dispatched with an empty principal"} {}
};

}  // namespace ledger
