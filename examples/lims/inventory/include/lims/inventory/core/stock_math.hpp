// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// The arithmetic of ordering and receiving stock, checked: a product that does not fit is refused, never saturated.

namespace lims {

/// @brief The total of an order line: the quantity times the unit price, rounded half-up once to @p currency's
///        decimals.
/// @param quantity The quantity, in the item's stock unit.
/// @param unitPrice The price of one stock unit, in @p currency.
/// @param currency The order's currency.
/// @return The line total, tagged at the currency's decimals.
/// @throws OutOfRange if the product, or the rounded total, does not fit an `int64`.
[[nodiscard]] ::morph::math::Rational orderLineTotal(::morph::math::Rational const& quantity,
                                                     ::morph::math::Rational const& unitPrice, CurrencyCode currency);

/// @brief What a goods receipt gives the ledger to post.
struct ReceiptToPost {
    /// @brief The company whose books it goes in.
    CompanyId company;
    /// @brief The company's currency.
    CurrencyCode companyCurrency = CurrencyCode::EUR;
    /// @brief The order's currency, in which the lines are valued.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief Company-currency units per unit of @p currency on @p date; one when the currencies are the same.
    ::morph::math::Rational rate;
    /// @brief The receipt's date.
    ::morph::time::Timestamp date;
    /// @brief The poster's key for the receipt.
    std::string sourceKey;
    /// @brief What the entry shows as its source.
    std::string sourceReference;
    /// @brief The value of each receipt line in @p currency, rounded to its decimals.
    std::vector<::morph::math::Rational> lineValues;
    /// @brief The account stock is valued on.
    std::string stockAccount;
    /// @brief The account goods received and not yet invoiced are owed on.
    std::string grniAccount;
    /// @brief The account the difference per-line rounding leaves is posted to.
    std::string roundingAccount;
};

/// @brief The posting of a receipt: a debit to the stock account per line, valued at the rate and rounded per line,
///        against one credit to goods received not invoiced for the total rounded once, and a line to the rounding
///        account for what the per-line rounding left.
/// @param receipt The receipt.
/// @return The request; the lines are the stock debits in the receipt's order, the credit, and the rounding line when
///         there is a difference.
/// @throws OutOfRange if a conversion or a sum does not fit an `int64`.
[[nodiscard]] PostingRequest receiptPostingFor(ReceiptToPost const& receipt);

/// @brief What a consumption gives the ledger to post.
struct ConsumptionToPost {
    /// @brief The company whose books it goes in.
    CompanyId company;
    /// @brief The company's currency, in which the cost is valued.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief When the stock was used: the result's capture.
    ::morph::time::Timestamp date;
    /// @brief The event that consumed it.
    std::string eventId;
    /// @brief The cost of the stock taken, rounded to the currency's decimals.
    ::morph::math::Rational cost;
    /// @brief The account the cost is expensed to.
    std::string consumptionAccount;
    /// @brief The account stock is valued on.
    std::string stockAccount;
};

/// @brief The posting of a consumption: a debit to the consumption account against a credit to the stock account, for
///        the cost, at a rate of one.
/// @param consumption The consumption.
/// @return The request, whose source key is `inventory.consumption:<event id>`.
[[nodiscard]] PostingRequest consumptionPostingFor(ConsumptionToPost const& consumption);

}  // namespace lims
