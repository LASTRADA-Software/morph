// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "bank/db/entities.hpp"
#include "bank/dto/account_dto.hpp"

/// @file
/// Shared `AccountRecord` -> `AccountInfo` projection. Split out of
/// `account_model.cpp` when the model divided into `AccountModel` (per account)
/// and `CustomerModel` (per owner), so both halves project rows identically.

namespace bank::db {

/// @brief Translates a persisted `AccountRecord` into the wire `AccountInfo` DTO.
/// @param rec   Persisted account row.
/// @param owner Resolved owner username — the wire DTO carries the username
///              rather than the internal `user_id` the record stores.
/// @return The wire projection of @p rec.
[[nodiscard]] inline dto::AccountInfo toAccountInfo(const AccountRecord& rec, const std::string& owner) {
    return dto::AccountInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = owner,
        .number = std::string{rec.number.Value().str()},
        .kind = rec.kind.Value(),
        .currency = rec.currency.Value(),
        .balanceMinor = rec.balanceMinor.Value(),
        .overdraftMinor = rec.overdraftMinor.Value(),
        .status = rec.status.Value(),
        .interestBps = rec.interestBps.Value(),
    };
}

}  // namespace bank::db
