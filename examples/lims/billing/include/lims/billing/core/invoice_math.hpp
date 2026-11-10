// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/rational.hpp>
#include <span>

#include "lims/org/core/money.hpp"

/// @file
/// An invoice's totals: the exact sum of its lines, the flat tax on it, and their sum.

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

}  // namespace lims
