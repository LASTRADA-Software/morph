// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "types.hpp"

/// @file
/// `Money` — a plain aggregate carrying an amount in **integer minor units**
/// (e.g. cents) plus its currency. Integer minor units avoid floating-point
/// rounding entirely; the database stores `minor` as a BIGINT column.
///
/// `Money` is deliberately a flat aggregate of trivially-serialisable members
/// so it round-trips through Glaze (and therefore the morph wire) with no
/// hand-written codec.

namespace bank {

/// @brief An amount of money: integer minor units tagged with a currency.
struct Money {
    std::int64_t minor = 0;            ///< amount in minor units (cents); may be negative
    Currency currency = Currency::USD;  ///< currency the amount is denominated in
};

/// @brief Convenience constructor.
[[nodiscard]] inline Money money(std::int64_t minor, Currency currency) noexcept {
    return Money{.minor = minor, .currency = currency};
}

/// @brief Adds two amounts. Caller must ensure currencies match.
[[nodiscard]] inline Money operator+(Money lhs, Money rhs) noexcept {
    return Money{.minor = lhs.minor + rhs.minor, .currency = lhs.currency};
}

/// @brief Subtracts @p rhs from @p lhs. Caller must ensure currencies match.
[[nodiscard]] inline Money operator-(Money lhs, Money rhs) noexcept {
    return Money{.minor = lhs.minor - rhs.minor, .currency = lhs.currency};
}

[[nodiscard]] inline bool operator==(Money lhs, Money rhs) noexcept {
    return lhs.minor == rhs.minor && lhs.currency == rhs.currency;
}

/// @brief Human-readable rendering, e.g. `1234` cents USD -> "12.34 USD".
[[nodiscard]] std::string format(Money amount);

}  // namespace bank
