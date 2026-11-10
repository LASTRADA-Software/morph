// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/core/invoice_math.hpp"

#include "lims/org/core/errors.hpp"

namespace lims {

InvoiceTotals invoiceTotals(std::span<::morph::math::Rational const> lineAmounts,
                            ::morph::math::Rational const& taxRate, CurrencyCode currency) {
    ExactSum net;
    for (auto const& amount : lineAmounts) {
        if (!net.add(amount)) {
            throw OutOfRange{"an invoice's net total is too large to total exactly"};
        }
    }
    auto const product = ::morph::math::checkedMul(net.total(), taxRate);
    if (!product.has_value()) {
        throw OutOfRange{"an invoice's tax is too large to compute exactly"};
    }
    auto const tax = roundToCurrency(*product, currency);
    auto const gross = ::morph::math::checkedAdd(net.total(), tax);
    if (!gross.has_value()) {
        throw OutOfRange{"an invoice's gross total is too large to total exactly"};
    }
    auto netValue = net.total();
    netValue.decimalPlaces = ::morph::math::DecimalPlaces{decimalsOf(currency)};
    auto grossValue = *gross;
    grossValue.decimalPlaces = netValue.decimalPlaces;
    return InvoiceTotals{.net = netValue, .tax = tax, .gross = grossValue};
}

}  // namespace lims
