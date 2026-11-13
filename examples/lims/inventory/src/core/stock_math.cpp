// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/core/stock_math.hpp"

#include <utility>

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

PostingRequest receiptPostingFor(ReceiptToPost const& receipt) {
    PostingRequest request{.company = receipt.company,
                           .date = receipt.date,
                           .sourceKey = receipt.sourceKey,
                           .sourceReference = receipt.sourceReference,
                           .currency = receipt.currency,
                           .rate = Rate{receipt.rate},
                           .lines = {}};
    auto const inCompany = [&](::morph::math::Rational const& value) {
        auto const converted = ::morph::math::checkedMul(value, receipt.rate);
        if (!converted.has_value()) {
            throw OutOfRange{"a receipt line's value is too large to convert exactly"};
        }
        return roundToCurrency(*converted, receipt.companyCurrency);
    };
    ExactSum total;
    ExactSum companyLines;
    for (auto const& value : receipt.lineValues) {
        auto const company = inCompany(value);
        if (!total.add(value) || !companyLines.add(company)) {
            throw OutOfRange{"a receipt's total is too large to hold exactly"};
        }
        request.lines.push_back(PostingLine{.accountCode = receipt.stockAccount,
                                            .side = PostingSide::Debit,
                                            .amount = Amount{value},
                                            .companyAmount = Amount{company}});
    }
    auto const companyTotal = inCompany(total.total());
    request.lines.push_back(PostingLine{.accountCode = receipt.grniAccount,
                                        .side = PostingSide::Credit,
                                        .amount = Amount{total.total()},
                                        .companyAmount = Amount{companyTotal}});
    // The poster rounds line by line and the ledger never adjusts: what the per-line rounding left is a line of its
    // own, zero in the transaction currency, on whichever side balances the company currency.
    auto const difference = ::morph::math::checkedSub(companyTotal, companyLines.total());
    if (!difference.has_value()) {
        throw OutOfRange{"a receipt's rounding difference is too large to hold exactly"};
    }
    if (difference->numerator != 0) {
        bool const debit = difference->numerator > 0;
        // The same two operands reversed: this subtraction fits whenever the one above did.
        auto const magnitude =
            debit ? *difference : ::morph::math::checkedSub(companyLines.total(), companyTotal).value();
        request.lines.push_back(PostingLine{
            .accountCode = receipt.roundingAccount,
            .side = debit ? PostingSide::Debit : PostingSide::Credit,
            .amount = Amount{::morph::math::Rational{0, ::morph::math::DecimalPlaces{decimalsOf(receipt.currency)}}},
            .companyAmount = Amount{magnitude}});
    }
    return request;
}

PostingRequest consumptionPostingFor(ConsumptionToPost const& consumption) {
    PostingRequest request{.company = consumption.company,
                           .date = consumption.date,
                           .sourceKey = "inventory.consumption:" + consumption.eventId,
                           .sourceReference = "consumption " + consumption.eventId,
                           .currency = consumption.currency,
                           .rate = Rate{::morph::math::Rational{1, ::morph::math::DecimalPlaces{6}}},
                           .lines = {}};
    request.lines.push_back(PostingLine{.accountCode = consumption.consumptionAccount,
                                        .side = PostingSide::Debit,
                                        .amount = Amount{consumption.cost},
                                        .companyAmount = Amount{consumption.cost}});
    request.lines.push_back(PostingLine{.accountCode = consumption.stockAccount,
                                        .side = PostingSide::Credit,
                                        .amount = Amount{consumption.cost},
                                        .companyAmount = Amount{consumption.cost}});
    return request;
}

}  // namespace lims
