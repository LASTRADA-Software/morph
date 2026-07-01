// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Statement model: a date-ranged summary across all of an
/// owner's accounts.

namespace bank::dto {

/// @brief Per-account summary line within a statement.
struct StatementLine {
    std::int64_t accountId = 0;
    std::string number;
    int currency = 0;
    std::int64_t creditsMinor = 0;
    std::int64_t debitsMinor = 0;
    std::int64_t closingBalanceMinor = 0;
    int entryCount = 0;
};

/// @brief Generate a statement for the owner over a time range.
struct GenerateStatement {
    std::string owner;           ///< empty => session principal
    std::int64_t fromMs = 0;     ///< inclusive lower bound (epoch ms)
    std::int64_t toMs = 0;       ///< inclusive upper bound; 0 => no upper bound
};

/// @brief Result of `GenerateStatement`.
struct Statement {
    std::string owner;
    std::int64_t fromMs = 0;
    std::int64_t toMs = 0;
    std::vector<StatementLine> lines;
    std::int64_t totalCreditsMinor = 0;
    std::int64_t totalDebitsMinor = 0;
};

}  // namespace bank::dto
