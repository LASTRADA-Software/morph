// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/models/invoice_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <morph/offline/replay_ledger.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr int kMaxLineState = static_cast<int>(BillableLineState::Invoiced);
constexpr int kMaxCurrencyCode = static_cast<int>(CurrencyCode::JPY);

/// @brief The scope billing's consumed events are recorded in.
constexpr std::string_view kLineEvents = "billing.lines";

/// @brief The events billing consumed, as an `IReplayLedger` over `billing_consumed_events`, constructed over the
///        caller's open mapper so a recorded event id commits in the transaction of the line it made. The payload is
///        the line's id.
class ConsumedEvents final : public ::morph::offline::IReplayLedger {
public:
    /// @param mapper The caller's mapper. Borrowed: it must outlive this instance.
    explicit ConsumedEvents(Lightweight::DataMapper& mapper) : _mapper{mapper} {}

protected:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        auto const rows =
            _mapper.Query<db::ConsumedEventRecord>()
                .Where(::Lightweight::FieldNameOf<&db::ConsumedEventRecord::scope>, "=", std::string{scope})
                .Where(::Lightweight::FieldNameOf<&db::ConsumedEventRecord::eventId>, "=", std::string{opId})
                .All();
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::string{rows.front().line.Value().ToStringView()};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void doRecord(std::string_view scope, std::string_view opId, std::string payload) override {
        db::ConsumedEventRecord row;
        row.scope = Lightweight::SqlAnsiString<32>{std::string{scope}};
        row.eventId = Lightweight::SqlAnsiString<128>{std::string{opId}};
        row.line = Lightweight::SqlAnsiString<32>{payload};
        _mapper.Create(row);
    }

private:
    // Borrowed for one call; never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    Lightweight::DataMapper& _mapper;
};

/// @brief Maps a line row to its view.
/// @param row The row.
/// @return The view; `amount` is set only when both halves of the price are stored.
/// @throws ValidationError if the row stores a state or currency outside its enumeration.
[[nodiscard]] BillableLineView toView(db::BillableLineRecord const& row) {
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
        view.amount =
            Amount{Rational{Numerator{*numerator}, Denominator{*denominator}, DecimalPlaces{decimalsOf(code)}}};
    }
    return view;
}

/// @brief The line @p lineId.
/// @param mapper The mapper to read through.
/// @param lineId The line's id.
/// @return Its view.
/// @throws NotFound if there is no such line.
[[nodiscard]] BillableLineView lineView(Lightweight::DataMapper& mapper, std::int64_t lineId) {
    auto const rows = mapper.Query<db::BillableLineRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::id>, "=", lineId)
                          .All();
    if (rows.empty()) {
        throw NotFound{"billable line " + std::to_string(lineId) + " does not exist"};
    }
    return toView(rows.front());
}

/// @brief The line an earlier delivery of @p eventId made.
/// @param eventId The event's id.
/// @return Its view, or nothing when no delivery of the event has committed.
[[nodiscard]] std::optional<BillableLineView> lineOfEvent(std::string const& eventId) {
    // A read of its own, outside the write's transaction: a read inside it would hold a lock that blocks a
    // concurrent writer's commit.
    Lightweight::DataMapper mapper;
    ConsumedEvents const consumed{mapper};
    auto const line = consumed.lookup(kLineEvents, eventId);
    if (!line.has_value()) {
        return std::nullopt;
    }
    return lineView(mapper, std::stoll(*line));
}

/// @brief Writes the line of @p action and records its event id, in one transaction.
/// @param action The event.
/// @param company The company that bills it.
/// @param price The price in force, if any.
/// @return The line.
/// @throws Lightweight::SqlException if the write fails, which includes the event id being recorded already.
[[nodiscard]] BillableLineView writeLine(RecordBillableLine const& action, CompanyId company,
                                         std::optional<PriceQuoteView> const& price) {
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::BillableLineRecord row;
    row.companyId = *company;
    row.siteId = *action.site;
    row.clientId = *action.client;
    row.sampleId = *action.sample;
    row.resultId = *action.result;
    row.analysisId = *action.analysis;
    row.publishedAt = millisOf(action.publishedAt);
    row.eventId = Lightweight::SqlAnsiString<128>{action.eventId};
    if (price.has_value()) {
        row.state = static_cast<int>(BillableLineState::Unbilled);
        row.amountNum = (*price->amount).numerator;
        row.amountDen = (*price->amount).denominator;
        row.currency = static_cast<int>(price->currency);
    } else {
        row.state = static_cast<int>(BillableLineState::Unpriced);
    }
    mapper.Create(row);
    ConsumedEvents consumed{mapper};
    consumed.record(kLineEvents, action.eventId, std::to_string(row.id.Value()));
    sqlTxn.Commit();
    return toView(row);
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillableLineView InvoiceModel::execute(const RecordBillableLine& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RecordBillableLine: event '" + action.eventId +
                              "' does not name everything a line needs"};
    }
    requireServicePrincipal("RecordBillableLine");
    requireRoleSomewhere<RecordBillableLine>();
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"RecordBillableLine: site " + std::to_string(*action.site) + " does not exist"};
    }
    requireAuthorized(action);
    return storeLine(action, site->companyId, {});
}

BillableLineView InvoiceModel::storeLine(RecordBillableLine const& action, CompanyId company,
                                         std::function<void()> const& betweenLookupAndWrite) {
    if (auto const earlier = lineOfEvent(action.eventId)) {
        return *earlier;
    }
    auto const price = PriceListModel::priceOn(company, action.client, action.analysis, action.publishedAt);
    if (betweenLookupAndWrite) {
        betweenLookupAndWrite();
    }
    try {
        return writeLine(action, company, price);
    } catch (Lightweight::SqlException const&) {
        // A concurrent delivery of the same event recorded it between this call's lookup and its write, and the
        // unique index refused this one. The winner's line is there now, so it is the answer. A failure that is not
        // that race finds no such line and surfaces as it is.
        if (auto const winner = lineOfEvent(action.eventId)) {
            return *winner;
        }
        throw;
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListBillableLinesResult InvoiceModel::execute(const ListBillableLines& action) {
    ListBillableLinesResult result;
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::BillableLineRecord>().All()) {
        auto const view = toView(row);
        if (std::ranges::find(companies, view.company) == companies.end() ||
            (action.company.hasValue() && view.company != action.company) ||
            (action.client.hasValue() && view.client != action.client) ||
            (action.sample.hasValue() && view.sample != action.sample)) {
            continue;
        }
        result.lines.push_back(view);
    }
    std::ranges::sort(result.lines, {}, [](BillableLineView const& line) { return *line.id; });
    return result;
}

}  // namespace lims
