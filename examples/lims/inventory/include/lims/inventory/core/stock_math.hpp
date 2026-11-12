// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/rational.hpp>

#include "lims/org/core/money.hpp"

/// @file
/// The arithmetic of ordering stock, checked: a product that does not fit is refused, never saturated.

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

}  // namespace lims
