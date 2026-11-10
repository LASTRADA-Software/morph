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

PostingRequest postingFor(InvoiceToPost const& invoice) {
    auto const toCompany = [&invoice](::morph::math::Rational const& amount) {
        auto const converted = ::morph::math::checkedMul(amount, invoice.rate);
        if (!converted.has_value()) {
            throw OutOfRange{"an invoice amount is too large to convert exactly"};
        }
        return roundToCurrency(*converted, invoice.companyCurrency);
    };
    auto const line = [&toCompany](std::string account, PostingSide side, ::morph::math::Rational const& amount) {
        return PostingLine{
            .accountCode = std::move(account),
            .side = side,
            .amount = Amount{amount},
            .companyAmount = Amount{toCompany(amount)},
        };
    };
    auto receivable = line(invoice.receivableAccount, PostingSide::Debit, invoice.gross);
    auto revenue = line(invoice.revenueAccount, PostingSide::Credit, invoice.net);
    auto tax = line(invoice.taxAccount, PostingSide::Credit, invoice.tax);
    auto const debit = *receivable.companyAmount;
    auto const credit = ::morph::math::checkedAdd(*revenue.companyAmount, *tax.companyAmount);
    if (!credit.has_value()) {
        throw OutOfRange{"an invoice's credits are too large to total exactly"};
    }
    PostingRequest request{
        .company = invoice.company,
        .date = invoice.date,
        .sourceKey = invoice.sourceKey,
        .sourceReference = invoice.sourceReference,
        .currency = invoice.currency,
        .rate = Rate{invoice.rate},
        .lines = {std::move(receivable), std::move(revenue), std::move(tax)},
    };
    if (debit != *credit) {
        bool const debitShort = debit < *credit;
        auto const difference = ::morph::math::checkedSub(debitShort ? *credit : debit, debitShort ? debit : *credit);
        if (!difference.has_value()) {
            throw OutOfRange{"an invoice's rounding difference is too large to compute exactly"};
        }
        request.lines.push_back(PostingLine{
            .accountCode = invoice.roundingAccount,
            .side = debitShort ? PostingSide::Debit : PostingSide::Credit,
            .amount = Amount{::morph::math::Rational{0, ::morph::math::DecimalPlaces{decimalsOf(invoice.currency)}}},
            .companyAmount = Amount{*difference},
        });
    }
    return request;
}

}  // namespace lims
