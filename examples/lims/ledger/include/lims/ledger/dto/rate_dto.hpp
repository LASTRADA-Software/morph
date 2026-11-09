// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Exchange rates as the wire sees them: entered by hand, per company and currency pair, from a date.

namespace lims {

/// @brief Enters the rate of converting @p from into @p to for a company, valid from an instant.
///
/// The rate valid on a date is the one with the latest `validFrom` not after it.
struct SetExchangeRate {
    /// @brief The company whose books the rate converts for.
    CompanyId company;
    /// @brief The transaction currency.
    CurrencyCode from = CurrencyCode::USD;
    /// @brief The company-side currency.
    CurrencyCode to = CurrencyCode::EUR;
    /// @brief The first instant the rate applies.
    ::morph::time::Timestamp validFrom;
    /// @brief Units of `to` per unit of `from`; positive.
    Rate rate;

    /// @brief Whether the rate is well-formed.
    /// @return `true` when a company, an instant and a positive rate are named for two different currencies.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && validFrom.hasValue() && rate.hasValue() && from != to && !(*rate).isZero() &&
               !(*rate).isNegative();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One entered rate.
struct ExchangeRateView {
    /// @brief Its id.
    ExchangeRateId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The transaction currency.
    CurrencyCode from = CurrencyCode::USD;
    /// @brief The company-side currency.
    CurrencyCode to = CurrencyCode::EUR;
    /// @brief The first instant the rate applies.
    ::morph::time::Timestamp validFrom;
    /// @brief Units of `to` per unit of `from`.
    Rate rate;
    /// @brief Who entered it.
    std::string setBy;
};

/// @brief Lists a company's rates.
struct ListExchangeRates {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The rates, by currency pair then start.
struct ListExchangeRatesResult {
    /// @brief The rates.
    std::vector<ExchangeRateView> rates;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::ExchangeRateView, "Ledger_ExchangeRate")
