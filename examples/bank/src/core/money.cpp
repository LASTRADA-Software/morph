// SPDX-License-Identifier: Apache-2.0

#include "bank/core/money.hpp"

#include <cstdlib>
#include <format>

namespace bank {

std::string format(Money amount) {
    const int decimals = currencyDecimals(amount.currency);
    const std::string_view code = currencyCode(amount.currency);
    const std::int64_t magnitude = std::llabs(amount.minor);
    const char* sign = amount.minor < 0 ? "-" : "";

    if (decimals == 0) {
        return std::format("{}{} {}", sign, magnitude, code);
    }

    std::int64_t scale = 1;
    for (int idx = 0; idx < decimals; ++idx) {
        scale *= 10;
    }
    const std::int64_t major = magnitude / scale;
    const std::int64_t minor = magnitude % scale;
    return std::format("{}{}.{:0{}} {}", sign, major, minor, decimals, code);
}

}  // namespace bank
