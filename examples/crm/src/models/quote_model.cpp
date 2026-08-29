// SPDX-License-Identifier: Apache-2.0
#include "crm/models/quote_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

morph::math::Rational toRational(std::int64_t num, std::int64_t den, int dp) {
    return morph::math::Rational{morph::math::Numerator{num}, morph::math::Denominator{den},
                                 morph::math::DecimalPlaces{static_cast<std::uint32_t>(dp)}};
}

QuoteLine toLineView(const db::QuoteLineRecord& row) {
    return QuoteLine{
        .productName = std::string{row.productName.Value().ToStringView()},
        .quantity = toRational(row.quantityNum.Value(), row.quantityDen.Value(), row.quantityDp.Value()),
        .unitPrice = toRational(row.unitPriceNum.Value(), row.unitPriceDen.Value(), row.unitPriceDp.Value()),
        .discount = toRational(row.discountNum.Value(), row.discountDen.Value(), row.discountDp.Value()),
        .total = toRational(row.totalNum.Value(), row.totalDen.Value(), row.totalDp.Value()),
    };
}

/// @brief Throws `ValidationError` unless every line in @p lines passes its
///        own `validate()` — the per-element half of the "app-level
///        recursive validator" crm/README.md names, since
///        `allRequiredEngaged`/`ActionValidator::ready` stop at the
///        top-level action (docs/spec/forms/forms.md).
void requireEveryLineValid(const std::vector<QuoteLine>& lines) {
    if (!std::ranges::all_of(lines, [](const QuoteLine& line) { return line.validate(); })) {
        throw ValidationError{
            "quote line: productName, positive quantity, and non-negative unitPrice/discount "
            "are required"};
    }
}

/// @brief The whole-quote grand total: sum of every (already per-line
///        recomputed) line's `total`, plus tax on that sum.
///
/// Hand-written accumulation, matching `ledger::checkZeroSumByCurrency`'s/
/// `buildLedgerState`'s identical pattern — `computedFields` cannot fold
/// over a `std::vector<QuoteLine>` member (quote_dto.hpp's header comment),
/// so this is the rung's own resolution of that gap, not a framework
/// mechanism.
morph::math::Rational computeGrandTotal(const std::vector<QuoteLine>& lines, const morph::math::Rational& taxRate) {
    auto subtotal = morph::math::Rational::zero(morph::math::DecimalPlaces{2});
    for (const auto& line : lines) {
        subtotal = subtotal + line.total;
    }
    return subtotal + subtotal * taxRate;
}

/// @brief Recomputes every line's `total` in place (per-element
///        `recomputeAll<QuoteLine>`, since `computedFields` only fires
///        automatically when `QuoteLine` is the top-level dispatched type,
///        which it never is here).
void recomputeLines(std::vector<QuoteLine>& lines) {
    for (auto& line : lines) {
        morph::forms::recomputeAll<QuoteLine>(line);
    }
}

QuoteView toView(const db::QuoteRecord& row, std::vector<QuoteLine> lines) {
    return QuoteView{
        .id = QuoteId{static_cast<std::int64_t>(row.id.Value())},
        .opportunityId = OpportunityId{static_cast<std::int64_t>(row.opportunity.Value())},
        .status = static_cast<QuoteStatus>(row.status.Value()),
        .lines = std::move(lines),
        .taxRate = toRational(row.taxRateNum.Value(), row.taxRateDen.Value(), row.taxRateDp.Value()),
        .grandTotal = toRational(row.grandTotalNum.Value(), row.grandTotalDen.Value(), row.grandTotalDp.Value()),
        .version = row.version.Value(),
    };
}

std::vector<db::QuoteLineRecord> fetchLines(Lightweight::DataMapper& mapper, std::uint64_t quoteId) {
    auto rows = mapper.Query<db::QuoteLineRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::QuoteLineRecord::quote>, "=", quoteId)
                    .OrderBy(::Lightweight::FieldNameOf<&db::QuoteLineRecord::lineOrder>)
                    .All();
    return rows;
}

std::vector<QuoteLine> toLineViews(const std::vector<db::QuoteLineRecord>& rows) {
    std::vector<QuoteLine> lines;
    lines.reserve(rows.size());
    for (const auto& row : rows) {
        lines.push_back(toLineView(row));
    }
    return lines;
}

/// @brief Throws `IllegalTransition` unless @p row is still `Draft` — the
///        only status this rung allows editing lines/tax under.
void requireDraft(const db::QuoteRecord& row) {
    if (static_cast<QuoteStatus>(row.status.Value()) != QuoteStatus::Draft) {
        throw IllegalTransition{"quote is not Draft and can no longer have its lines/tax edited"};
    }
}

void writeLines(Lightweight::DataMapper& mapper, const db::QuoteRecord& quoteRow,
                const std::vector<QuoteLine>& lines) {
    int order = 0;
    for (const auto& line : lines) {
        db::QuoteLineRecord lineRow;
        lineRow.quote = quoteRow;
        lineRow.lineOrder = order++;
        lineRow.productName = Lightweight::SqlAnsiString<128>{line.productName};
        lineRow.quantityNum = line.quantity.numerator;
        lineRow.quantityDen = line.quantity.denominator;
        lineRow.quantityDp = static_cast<int>(line.quantity.decimalPlaces.value);
        lineRow.unitPriceNum = line.unitPrice.numerator;
        lineRow.unitPriceDen = line.unitPrice.denominator;
        lineRow.unitPriceDp = static_cast<int>(line.unitPrice.decimalPlaces.value);
        lineRow.discountNum = line.discount.numerator;
        lineRow.discountDen = line.discount.denominator;
        lineRow.discountDp = static_cast<int>(line.discount.decimalPlaces.value);
        lineRow.totalNum = line.total.numerator;
        lineRow.totalDen = line.total.denominator;
        lineRow.totalDp = static_cast<int>(line.total.decimalPlaces.value);
        mapper.Create(lineRow);
    }
}

void deleteLines(Lightweight::DataMapper& mapper, std::uint64_t quoteId) {
    for (auto& row : fetchLines(mapper, quoteId)) {
        mapper.Delete(row);
    }
}

}  // namespace

CreateQuoteResult QuoteModel::execute(const CreateQuote& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateQuote: opportunityId and at least one valid line are required"};
    }
    requireEveryLineValid(action.lines);

    Lightweight::DataMapper mapper;
    auto opportunityRows =
        mapper.Query<db::OpportunityRecord>()
            .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::id>, "=", *action.opportunityId)
            .All();
    if (opportunityRows.empty()) {
        throw NotFound{"CreateQuote: no such opportunity"};
    }

    auto lines = action.lines;
    recomputeLines(lines);
    const auto grandTotal = computeGrandTotal(lines, action.taxRate);

    Lightweight::SqlTransaction transaction{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

    db::QuoteRecord quoteRow;
    quoteRow.opportunity = opportunityRows.front();
    quoteRow.status = static_cast<int>(QuoteStatus::Draft);
    quoteRow.taxRateNum = action.taxRate.numerator;
    quoteRow.taxRateDen = action.taxRate.denominator;
    quoteRow.taxRateDp = static_cast<int>(action.taxRate.decimalPlaces.value);
    quoteRow.grandTotalNum = grandTotal.numerator;
    quoteRow.grandTotalDen = grandTotal.denominator;
    quoteRow.grandTotalDp = static_cast<int>(grandTotal.decimalPlaces.value);
    quoteRow.createdAt = nowMillis();
    quoteRow.version = 1;
    mapper.Create(quoteRow);

    writeLines(mapper, quoteRow, lines);
    transaction.Commit();

    CreateQuoteResult result{.quote = toView(quoteRow, std::move(lines))};
    _journal.recordSuccess<QuoteModel>(action, result, nowMillis());
    return result;
}

UpdateQuoteResult QuoteModel::execute(const UpdateQuote& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"UpdateQuote: quoteId and at least one valid line are required"};
    }
    requireEveryLineValid(action.lines);

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::QuoteRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::QuoteRecord::id>, "=", *action.quoteId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"UpdateQuote: no such quote"};
    }
    auto& row = rows.front();
    if (row.version.Value() != action.expectedVersion) {
        throw Conflict{"UpdateQuote: version mismatch — record was edited concurrently"};
    }
    requireDraft(row);

    auto lines = action.lines;
    recomputeLines(lines);
    const auto grandTotal = computeGrandTotal(lines, action.taxRate);

    Lightweight::SqlTransaction transaction{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

    deleteLines(mapper, row.id.Value());
    writeLines(mapper, row, lines);

    row.taxRateNum = action.taxRate.numerator;
    row.taxRateDen = action.taxRate.denominator;
    row.taxRateDp = static_cast<int>(action.taxRate.decimalPlaces.value);
    row.grandTotalNum = grandTotal.numerator;
    row.grandTotalDen = grandTotal.denominator;
    row.grandTotalDp = static_cast<int>(grandTotal.decimalPlaces.value);
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    transaction.Commit();

    UpdateQuoteResult result{.quote = toView(row, std::move(lines))};
    _journal.recordSuccess<QuoteModel>(action, result, nowMillis());
    return result;
}

QuoteView QuoteModel::execute(const SendQuote& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SendQuote: quoteId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::QuoteRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::QuoteRecord::id>, "=", *action.quoteId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"SendQuote: no such quote"};
    }
    auto& row = rows.front();
    requireDraft(row);

    row.status = static_cast<int>(QuoteStatus::Sent);
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    auto result = toView(row, toLineViews(fetchLines(mapper, row.id.Value())));
    _journal.recordSuccess<QuoteModel>(action, result, nowMillis());
    return result;
}

QuoteView QuoteModel::execute(const DecideQuote& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"DecideQuote: quoteId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::QuoteRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::QuoteRecord::id>, "=", *action.quoteId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"DecideQuote: no such quote"};
    }
    auto& row = rows.front();
    if (static_cast<QuoteStatus>(row.status.Value()) != QuoteStatus::Sent) {
        throw IllegalTransition{"DecideQuote: quote must be Sent before it can be Accepted or Rejected"};
    }

    row.status = static_cast<int>(action.accepted ? QuoteStatus::Accepted : QuoteStatus::Rejected);
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    auto result = toView(row, toLineViews(fetchLines(mapper, row.id.Value())));
    _journal.recordSuccess<QuoteModel>(action, result, nowMillis());
    return result;
}

QuoteView QuoteModel::execute(const GetQuote& action) {
    if (!action.validate()) {
        throw ValidationError{"GetQuote: quoteId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::QuoteRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::QuoteRecord::id>, "=", *action.quoteId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"GetQuote: no such quote"};
    }
    return toView(rows.front(), toLineViews(fetchLines(mapper, rows.front().id.Value())));
}

ListQuotesResult QuoteModel::execute(const ListQuotes& action) {
    Lightweight::DataMapper mapper;
    auto query = mapper.Query<db::QuoteRecord>();
    std::vector<db::QuoteRecord> rows;
    if (action.opportunityId.has_value() && action.opportunityId->hasValue()) {
        rows =
            query.Where(::Lightweight::FieldNameOf<&db::QuoteRecord::opportunity>, "=", **action.opportunityId).All();
    } else {
        rows = query.All();
    }
    ListQuotesResult result;
    result.quotes.reserve(rows.size());
    for (const auto& row : rows) {
        result.quotes.push_back(toView(row, toLineViews(fetchLines(mapper, row.id.Value()))));
    }
    return result;
}

}  // namespace crm
