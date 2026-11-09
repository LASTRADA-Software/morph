// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <span>
#include <string>

#include "lims/posting/posting.hpp"

/// @file
/// The one rule an entry must meet before it is recorded. Pure: the same lines always give the same answer.

namespace lims {

/// @brief Why @p lines cannot be recorded, if they cannot.
///
/// An entry needs two or more lines, each with a non-negative amount in @p transaction's minor units and a value in
/// @p company's minor units; the debits equal the credits in each; and when the currencies are the same, each line's
/// two amounts are equal. Nothing is rounded: a value with more decimals than its currency is refused.
/// @param lines The lines.
/// @param transaction The transaction currency.
/// @param company The company's currency.
/// @return The reason, in words; nothing when the lines may be recorded.
[[nodiscard]] std::optional<std::string> entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction,
                                                      CurrencyCode company);

}  // namespace lims
