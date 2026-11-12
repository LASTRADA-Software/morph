// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/core/stock_math.hpp"

#include "lims/org/core/errors.hpp"

namespace lims {

::morph::math::Rational orderLineTotal(::morph::math::Rational const& quantity,
                                       ::morph::math::Rational const& unitPrice, CurrencyCode currency) {
    auto const product = ::morph::math::checkedMul(quantity, unitPrice);
    if (!product.has_value()) {
        throw OutOfRange{"an order line's total is too large to hold exactly"};
    }
    return roundToCurrency(*product, currency);
}

}  // namespace lims
