// SPDX-License-Identifier: Apache-2.0

#include "bank/core/money.hpp"

#include <cstdint>
#include <format>

namespace bank {

std::string format(Money amount) {
    const int decimals = currencyDecimals(amount.currency);
    const std::string_view code = currencyCode(amount.currency);
    // Take the magnitude in unsigned space: std::llabs(INT64_MIN) is UB, but
    // unsigned negation (0 - x, modulo 2^64) is well-defined for every value.
    const auto raw = static_cast<std::uint64_t>(amount.minor);
    const std::uint64_t magnitude = amount.minor < 0 ? 0ULL - raw : raw;
    const char* sign = amount.minor < 0 ? "-" : "";

    if (decimals == 0) {
        return std::format("{}{} {}", sign, magnitude, code);
    }

    const auto scale = static_cast<std::uint64_t>(currencyScale(amount.currency));
    const std::uint64_t major = magnitude / scale;
    const std::uint64_t minor = magnitude % scale;
    return std::format("{}{}.{:0{}} {}", sign, major, minor, decimals, code);
}

}  // namespace bank
