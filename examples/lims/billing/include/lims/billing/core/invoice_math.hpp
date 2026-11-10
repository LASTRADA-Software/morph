// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <span>
#include <string>

#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// An invoice's totals: the exact sum of its lines, the flat tax on it, and their sum; and the posting request an
/// invoice becomes.

namespace lims {

/// @brief What an invoice adds up to, each tagged with its currency's decimals.
struct InvoiceTotals {
    /// @brief The lines' exact sum.
    ::morph::math::Rational net;
    /// @brief The tax on @p net, rounded half-up once.
    ::morph::math::Rational tax;
    /// @brief @p net plus @p tax.
    ::morph::math::Rational gross;
};

/// @brief Totals an invoice: the exact sum of the lines, the flat tax on that sum rounded half-up once to the
///        currency's decimals, and their sum.
///
/// The tax is computed on the net total, not per line: rounding each line's tax would drift from the rate on the
/// whole. Every step is checked, because `Rational`'s own operators saturate silently.
/// @param lineAmounts The lines' amounts.
/// @param taxRate The company's flat rate.
/// @param currency The invoice's currency.
/// @return The net, tax and gross.
/// @throws OutOfRange when a total does not fit.
[[nodiscard]] InvoiceTotals invoiceTotals(std::span<::morph::math::Rational const> lineAmounts,
                                          ::morph::math::Rational const& taxRate, CurrencyCode currency);

/// @brief What a posting is made from: an invoice's amounts, the accounts they go to, and the rate that converts them.
struct InvoiceToPost {
    /// @brief The company whose books it goes in.
    CompanyId company;
    /// @brief The company's currency.
    CurrencyCode companyCurrency = CurrencyCode::EUR;
    /// @brief The invoice's currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief Company-currency units per invoice-currency unit; one when the currencies are the same.
    ::morph::math::Rational rate;
    /// @brief The entry's date.
    ::morph::time::Timestamp date;
    /// @brief The ledger's key for the document, such as `billing.invoice:7`.
    std::string sourceKey;
    /// @brief What the entry shows as its source.
    std::string sourceReference;
    /// @brief The lines' exact sum, in the invoice's currency.
    ::morph::math::Rational net;
    /// @brief The tax, in the invoice's currency.
    ::morph::math::Rational tax;
    /// @brief The net plus the tax, in the invoice's currency.
    ::morph::math::Rational gross;
    /// @brief The account the gross is debited to.
    std::string receivableAccount;
    /// @brief The account the net is credited to.
    std::string revenueAccount;
    /// @brief The account the tax is credited to.
    std::string taxAccount;
    /// @brief The account the conversion's rounding difference is booked to.
    std::string roundingAccount;
};

/// @brief The posting request an invoice becomes: the gross debited to the receivable account, the net and the tax
///        credited to the revenue and tax accounts, each converted at the rate and rounded half-up to the company
///        currency's decimals on its own.
///
/// Each line is rounded alone, so the company-currency sides can differ by the rounding; the difference is booked,
/// explicitly, to the rounding account, with no amount in the invoice's currency. The ledger rounds nothing and
/// refuses an entry whose sides differ.
/// @param invoice What the posting is made from.
/// @return The request: three lines, and a fourth when the rounded sides differ.
/// @throws OutOfRange when an amount is too large to convert or total exactly.
[[nodiscard]] PostingRequest postingFor(InvoiceToPost const& invoice);

}  // namespace lims
