// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr int kMaxLineState = static_cast<int>(BillableLineState::Invoiced);
constexpr int kMaxInvoiceState = static_cast<int>(InvoiceState::Posted);
constexpr int kMaxCurrencyCode = static_cast<int>(CurrencyCode::JPY);

/// @brief An amount stored as a numerator and denominator, tagged with @p currency's decimals.
[[nodiscard]] Amount amountOf(std::int64_t numerator, std::int64_t denominator, CurrencyCode currency) {
    return Amount{Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{decimalsOf(currency)}}};
}

[[nodiscard]] std::string text(auto const& field) { return std::string{field.Value().ToStringView()}; }

[[nodiscard]] ::morph::time::Timestamp instant(std::int64_t millis) {
    return millis > 0 ? timestampFromMillis(millis) : ::morph::time::Timestamp{};
}

}  // namespace

BillableLineView billableLineView(db::BillableLineRecord const& row) {
    int const state = row.state.Value();
    int const currency = row.currency.Value();
    if (state < 0 || state > kMaxLineState) {
        throw ValidationError{"billable line " + std::to_string(row.id.Value()) + " stores a state outside its range"};
    }
    if (currency < 0 || currency > kMaxCurrencyCode) {
        throw ValidationError{"billable line " + std::to_string(row.id.Value()) +
                              " stores a currency outside its range"};
    }
    auto const code = static_cast<CurrencyCode>(currency);
    BillableLineView view{
        .id = BillableLineId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .site = SiteId{row.siteId.Value()},
        .client = ClientId{row.clientId.Value()},
        .sample = SampleId{row.sampleId.Value()},
        .result = ResultId{row.resultId.Value()},
        .analysis = AnalysisId{row.analysisId.Value()},
        .publishedAt = timestampFromMillis(row.publishedAt.Value()),
        .state = static_cast<BillableLineState>(state),
        .amount = {},
        .currency = code,
        .invoice = InvoiceId::fromOptional(row.invoiceId.Value()),
        .eventId = std::string{row.eventId.Value().ToStringView()},
    };
    auto const numerator = row.amountNum.Value();
    auto const denominator = row.amountDen.Value();
    if (numerator.has_value() && denominator.has_value()) {
        view.amount = amountOf(*numerator, *denominator, code);
    }
    return view;
}

std::optional<std::int64_t> draftInvoice(Lightweight::DataMapper& mapper, CompanyId company, SiteId site,
                                         ClientId client, std::string const& author) {
    auto const settings = PriceListModel::settingsOf(company);
    if (!settings.has_value()) {
        throw NotFound{"company " + std::to_string(*company) + " has no billing settings"};
    }
    auto const billed = PriceListModel::billingClient(client, company);
    if (!billed.has_value()) {
        throw NotFound{"client " + std::to_string(*client) + " is not billed at company " + std::to_string(*company)};
    }
    // Only this site's lines of this client: approval is judged at the site, so a line of another site belongs on
    // that site's invoice.
    auto rows = mapper.Query<db::BillableLineRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::siteId>, "=", *site)
                    .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::clientId>, "=", *client)
                    .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::state>, "=",
                           static_cast<int>(BillableLineState::Unbilled))
                    .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(rows, {}, [](db::BillableLineRecord const& row) { return row.id.Value(); });
    std::vector<Rational> amounts;
    amounts.reserve(rows.size());
    for (auto const& row : rows) {
        auto const line = billableLineView(row);
        // The amount type carries no currency, so nothing but this check keeps a yen line off a euro invoice.
        if (line.currency != billed->currency) {
            throw ValidationError{"line " + std::to_string(*line.id) + " is in " +
                                  std::string{currencyName(line.currency)} + "; client " + std::to_string(*client) +
                                  " is billed in " + std::string{currencyName(billed->currency)}};
        }
        if (!line.amount.hasValue()) {
            throw ValidationError{"line " + std::to_string(*line.id) + " is unbilled but has no amount"};
        }
        amounts.push_back(*line.amount);
    }
    auto const totals = invoiceTotals(amounts, *settings->taxRate, billed->currency);

    db::InvoiceRecord invoice;
    invoice.companyId = *company;
    invoice.siteId = *site;
    invoice.clientId = *client;
    invoice.currency = static_cast<int>(billed->currency);
    invoice.state = static_cast<int>(InvoiceState::Draft);
    invoice.netNum = totals.net.numerator;
    invoice.netDen = totals.net.denominator;
    invoice.taxNum = totals.tax.numerator;
    invoice.taxDen = totals.tax.denominator;
    invoice.grossNum = totals.gross.numerator;
    invoice.grossDen = totals.gross.denominator;
    invoice.createdBy = Lightweight::SqlAnsiString<64>{author};
    invoice.createdAt = nowMillis();
    mapper.Create(invoice);
    auto const invoiceId = static_cast<std::int64_t>(invoice.id.Value());
    // Only a line still Unbilled is claimed: a concurrent draft that took it first leaves nothing here to change, and
    // fewer claimed lines than were read is a Conflict that rolls the invoice row back with it.
    for (auto const& row : rows) {
        auto query = mapper.Connection().Query(db::BillableLineRecord::TableName).Update();
        query.Set("state", static_cast<int>(BillableLineState::Invoiced)).Set("invoice_id", invoiceId);
        (void)query.Where("id", "=", static_cast<std::int64_t>(row.id.Value()))
            .Where("state", "=", static_cast<int>(BillableLineState::Unbilled));
        Lightweight::SqlStatement statement{mapper.Connection()};
        if (statement.ExecuteDirect(query.ToSql()).NumRowsAffected() != 1U) {
            throw Conflict{"line " + std::to_string(row.id.Value()) + " was invoiced meanwhile"};
        }
    }
    // Inside the caller's transaction: a failure here takes the invoice row and the claimed lines back with it.
    faultPoint("run.afterInvoice", mapper.Connection());
    return invoiceId;
}

bool escalated(db::InvoiceRecord const& invoice) {
    return invoice.deadlineAt.Value() > 0 && nowMillis() >= invoice.deadlineAt.Value();
}

std::optional<db::InvoiceRecord> findInvoiceRecord(InvoiceId invoice) {
    if (!invoice.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::InvoiceRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::InvoiceRecord::id>, "=", *invoice)
                    .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front();
}

InvoiceView invoiceView(Lightweight::DataMapper& mapper, std::int64_t invoiceId) {
    auto const rows = mapper.Query<db::InvoiceRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::InvoiceRecord::id>, "=", invoiceId)
                          .All();
    if (rows.empty()) {
        throw NotFound{"invoice " + std::to_string(invoiceId) + " does not exist"};
    }
    auto const& row = rows.front();
    int const state = row.state.Value();
    int const currency = row.currency.Value();
    if (state < 0 || state > kMaxInvoiceState) {
        throw ValidationError{"invoice " + std::to_string(invoiceId) + " stores a state outside its range"};
    }
    if (currency < 0 || currency > kMaxCurrencyCode) {
        throw ValidationError{"invoice " + std::to_string(invoiceId) + " stores a currency outside its range"};
    }
    auto const code = static_cast<CurrencyCode>(currency);
    InvoiceView view{
        .id = InvoiceId{invoiceId},
        .company = CompanyId{row.companyId.Value()},
        .site = SiteId{row.siteId.Value()},
        .client = ClientId{row.clientId.Value()},
        .currency = code,
        .state = static_cast<InvoiceState>(state),
        .number = text(row.number),
        .lines = {},
        .net = amountOf(row.netNum.Value(), row.netDen.Value(), code),
        .tax = amountOf(row.taxNum.Value(), row.taxDen.Value(), code),
        .gross = amountOf(row.grossNum.Value(), row.grossDen.Value(), code),
        .reference = text(row.reference),
        .createdBy = text(row.createdBy),
        .submittedBy = text(row.submittedBy),
        .approvedBy = text(row.approvedBy),
        .postedBy = text(row.postedBy),
        .submittedAt = instant(row.submittedAt.Value()),
        .approvalDeadline = instant(row.deadlineAt.Value()),
        .level = escalated(row) ? ApprovalLevel::Company : ApprovalLevel::Site,
        .entry = JournalEntryId::fromOptional(row.entryId.Value()),
    };
    auto lines = mapper.Query<db::BillableLineRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::invoiceId>, "=", invoiceId)
                     .All();
    std::ranges::sort(lines, {}, [](db::BillableLineRecord const& line) { return line.id.Value(); });
    for (auto const& line : lines) {
        view.lines.push_back(billableLineView(line));
    }
    return view;
}

}  // namespace lims
