// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/core/balance.hpp"

namespace lims {

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

}  // namespace

std::optional<std::string> entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction,
                                        CurrencyCode company) {
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
    if (debit.total().isZero() && companyDebit.total().isZero()) {
        return std::string{"an entry moves no amount"};
    }
    return std::nullopt;
}

}  // namespace lims
