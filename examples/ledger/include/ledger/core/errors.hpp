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

/// @brief Thrown when an update carries a base version that no longer matches
///        the stored row -- someone else committed a change in between.
///
///        Distinct from `ValidationError`: the action was well-formed and the
///        principal was entitled to make it. What failed is that it was
///        computed against state that no longer exists, which is a *conflict*
///        to report to the user, not a malformed request to reject. Design
///        spec §10's Scenario B: rejected outright, never merged and never
///        silently overwritten.
class VersionConflict : public LedgerError {
public:
    VersionConflict() : LedgerError{"update rejected: the record changed since it was read"} {}
};

/// @brief Thrown when `UndoTransaction` names a journal that some other
///        journal already reverses.
///
///        A compensating entry is itself zero-sum, so applying one twice
///        leaves the ledger's per-currency sum at zero while moving real
///        money into the accounts -- reverse a -50.00/+50.00 shop twice and
///        Checking ends at +50.00, which the user never had. The zero-sum
///        invariant cannot see this; only this check can.
///
///        Two devices queueing a reversal of the same transaction while
///        offline is the ordinary way to hit it. Consistent with
///        `VersionConflict` and design spec §10, the second one is rejected
///        outright rather than silently applied.
class AlreadyReversed : public LedgerError {
public:
    AlreadyReversed() : LedgerError{"undo rejected: this transaction has already been reversed"} {}
};

/// @brief Thrown when a mutating action dispatches with an empty principal
///        (design spec §11) — never silently proceeds.
class EmptyPrincipalError : public LedgerError {
public:
    EmptyPrincipalError() : LedgerError{"mutating action dispatched with an empty principal"} {}
};

}  // namespace ledger
