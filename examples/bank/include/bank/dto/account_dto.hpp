// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bank/dto/common.hpp"

/// @file
/// Wire DTOs for the Account model. These are plain aggregates so Glaze can
/// serialise them with no hand-written codec; they are the action/result types
/// the GUI (or CLI) exchanges with `AccountModel` through the morph bridge.
///
/// Amounts are carried as integer minor units; `kind`/`currency`/`status` are
/// carried as the integer values of the `bank::AccountKind` / `bank::Currency`
/// / `bank::AccountStatus` enums.

namespace bank::dto {

/// @brief Open a new account for the current session's owner.
struct OpenAccount {
    std::string owner;                ///< empty => use the session principal
    int kind = 0;                     ///< bank::AccountKind
    int currency = 0;                 ///< bank::Currency
    std::int64_t overdraftMinor = 0;  ///< permitted overdraft, minor units

    /// Form-readiness predicate (used by `morph::flows::FlowSession::set<>` streaming).
    [[nodiscard]] bool validate() const {
        return kind >= 0 && kind <= 2 && currency >= 0 && currency <= 4 && overdraftMinor >= 0;
    }
};

/// @brief A snapshot of one account (the Account model's primary result type).
struct AccountInfo {
    std::int64_t id = 0;
    std::string owner;
    std::string number;
    int kind = 0;
    int currency = 0;
    std::int64_t balanceMinor = 0;
    std::int64_t overdraftMinor = 0;
    int status = 0;
    int interestBps = 0;
};

/// @brief List the accounts belonging to an owner.
struct ListAccounts {
    std::string owner;  ///< empty => use the session principal
};

/// @brief Result of `ListAccounts`.
struct AccountList {
    std::vector<AccountInfo> accounts;
};

/// @brief Fetch a single account by id.
struct GetAccount {
    std::int64_t id = 0;
};

/// @brief Close an account (only permitted when its balance is zero).
struct CloseAccount {
    std::int64_t id = 0;
};

}  // namespace bank::dto
