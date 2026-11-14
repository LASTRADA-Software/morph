// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/core/balance.hpp"

namespace lims {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

namespace {

/// @brief Why @p line alone cannot be recorded, if it cannot.
/// @param line The line.
/// @param transaction The transaction currency.
/// @param company The company's currency.
/// @return The reason; nothing when the line is well-formed.
[[nodiscard]] std::optional<std::string> lineProblem(PostingLine const& line, CurrencyCode transaction,
                                                     CurrencyCode company) {
    std::string const where = "line on account " + line.accountCode;
    if (!line.amount.hasValue() || !line.companyAmount.hasValue()) {
        return where + " has no amount";
    }
    auto const& amount = *line.amount;
    auto const& value = *line.companyAmount;
    if (amount.isNegative() || value.isNegative()) {
        return where + " is negative; its side says which way";
    }
    if (!fitsCurrency(amount, transaction)) {
        return where + " has more decimals than " + std::string{currencyName(transaction)};
    }
    if (!fitsCurrency(value, company)) {
        return where + " is not rounded to company currency " + std::string{currencyName(company)};
    }
    if (transaction == company && amount != value) {
        return where + " values its amount differently in its own currency";
    }
    return std::nullopt;
}

/// @brief Why @p line's company value is not its amount at @p rate, if it is not.
///
/// The value must lie within half a company minor unit of amount x rate, which is where rounding half-up puts it. The
/// product is computed exactly; one that does not fit is refused rather than saturated. A line of zero amount carries
/// the poster's explicit rounding difference and has no rate to check.
/// @param line The line.
/// @param rate Company-currency units per transaction-currency unit.
/// @param company The company's currency.
/// @return The reason; nothing when the value follows from the amount.
[[nodiscard]] std::optional<std::string> rateProblem(PostingLine const& line, Rational const& rate,
                                                     CurrencyCode company) {
    if ((*line.amount).isZero()) {
        return std::nullopt;
    }
    constexpr std::int64_t kTen = 10;
    std::int64_t scale = 2;
    for (std::uint32_t digit = 0; digit < decimalsOf(company); ++digit) {
        scale *= kTen;
    }
    Rational const half{Numerator{1}, Denominator{scale}, DecimalPlaces{decimalsOf(company)}};
    auto const product = ::morph::math::checkedMul(*line.amount, rate);
    if (!product.has_value()) {
        return std::string{"the entry is too large to total exactly"};
    }
    auto const difference = ::morph::math::checkedSub(*line.companyAmount, *product);
    if (!difference.has_value()) {
        return std::string{"the entry is too large to total exactly"};
    }
    if (*difference > half || *difference < -half) {
        return "line on account " + line.accountCode + " is not its amount at the entry's rate, to the cent of " +
               std::string{currencyName(company)};
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction,
                                        CurrencyCode company, Rate const& rate) {
    if (lines.size() < 2U) {
        return std::string{"an entry needs two or more lines"};
    }
    ExactSum debit;
    ExactSum credit;
    ExactSum companyDebit;
    ExactSum companyCredit;
    for (PostingLine const& line : lines) {
        if (auto problem = lineProblem(line, transaction, company)) {
            return problem;
        }
        bool const isDebit = line.side == PostingSide::Debit;
        if (!(isDebit ? debit : credit).add(*line.amount) ||
            !(isDebit ? companyDebit : companyCredit).add(*line.companyAmount)) {
            return std::string{"the entry is too large to total exactly"};
        }
    }
    if (debit.total() != credit.total()) {
        return "debits and credits differ in " + std::string{currencyName(transaction)};
    }
    if (companyDebit.total() != companyCredit.total()) {
        return "debits and credits differ in company currency " + std::string{currencyName(company)};
    }
    if (transaction != company) {
        for (PostingLine const& line : lines) {
            if (auto problem = rateProblem(line, *rate, company)) {
                return problem;
            }
        }
    }
    if (debit.total().isZero() && companyDebit.total().isZero()) {
        return std::string{"an entry moves no amount"};
    }
    return std::nullopt;
}

}  // namespace lims
