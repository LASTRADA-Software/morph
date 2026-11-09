// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/billing/core/types.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Price lists and their prices, as the wire sees them.

namespace lims {

/// @brief Creates a price list for a company, in a currency, that applies from a date.
///
/// A list replaces the one in force before it whole: it holds every price that applies from its date on, and an
/// analysis it has no price for has none. A company has one list per currency and start.
struct CreatePriceList {
    /// @brief The company.
    CompanyId company;
    /// @brief The list's name.
    std::string name;
    /// @brief The currency of every price in it.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The first instant the list applies.
    ::morph::time::Timestamp validFrom;

    /// @brief Whether the list is well-formed.
    /// @return `true` when a company, a start and a name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && validFrom.hasValue() && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One price of one analysis, the list's own or one client's.
struct PriceView {
    /// @brief Its id.
    PriceId id;
    /// @brief The analysis definition.
    AnalysisId analysis;
    /// @brief The client it is for; empty for the list's own price.
    ClientId client;
    /// @brief The price, in the list's currency.
    Amount amount;
};

/// @brief A price list with its prices.
struct PriceListView {
    /// @brief Its id.
    PriceListId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The list's name.
    std::string name;
    /// @brief The currency of every price in it.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The first instant the list applies.
    ::morph::time::Timestamp validFrom;
    /// @brief Its prices, by analysis then client (the list's own first).
    std::vector<PriceView> prices;
};

/// @brief Sets the price of an analysis in a list: the list's own, or one client's.
struct SetPrice {
    /// @brief The price list.
    PriceListId priceList;
    /// @brief The analysis definition.
    AnalysisId analysis;
    /// @brief The client the price is for; empty for the list's own price.
    ClientId client;
    /// @brief The price: not negative, in whole minor units of the list's currency.
    Amount amount;

    /// @brief Whether the price is well-formed.
    /// @return `true` when a list, an analysis and an amount are named, and a client, if given, is above zero (the
    ///         stored key 0 means the list's own price).
    [[nodiscard]] bool validate() const noexcept {
        return priceList.hasValue() && analysis.hasValue() && amount.hasValue() && (!client.hasValue() || *client > 0);
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Lists a company's price lists with their prices.
struct ListPriceLists {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The price lists, by start then id.
struct ListPriceListsResult {
    /// @brief The lists.
    std::vector<PriceListView> priceLists;
};

/// @brief Asks the price of an analysis for a client at a company on a date.
struct QuotePrice {
    /// @brief The company.
    CompanyId company;
    /// @brief The client.
    ClientId client;
    /// @brief The analysis definition.
    AnalysisId analysis;
    /// @brief The date.
    ::morph::time::Timestamp on;

    /// @brief Whether the question is well-formed.
    /// @return `true` when all four are named.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && client.hasValue() && analysis.hasValue() && on.hasValue();
    }
};

/// @brief A price and where it came from.
struct PriceQuoteView {
    /// @brief The list in force that priced it.
    PriceListId priceList;
    /// @brief The price.
    Amount amount;
    /// @brief The client's currency at the company.
    CurrencyCode currency = CurrencyCode::EUR;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::PriceListView, "Billing_PriceList")
BRIDGE_REGISTER_RECORD(lims::PriceView, "Billing_Price")
