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
#include "lims/billing/db/billing_store.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

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

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return `true` when it is one of the caller's visible companies.
[[nodiscard]] bool isVisible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
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
    return billableLineView(rows.front());
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
    return billableLineView(row);
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
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"RecordBillableLine: site " + std::to_string(*action.site) + " does not exist"};
    }
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
        auto const view = billableLineView(row);
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

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
InvoiceView InvoiceModel::execute(const CreateInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateInvoice: a company, a site and a client are required"};
    }
    if (!OrgModel::findCompany(action.company).has_value()) {
        throw NotFound{"CreateInvoice: company " + std::to_string(*action.company) + " does not exist"};
    }
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"CreateInvoice: site " + std::to_string(*action.site) + " does not exist"};
    }
    if (site->companyId != action.company) {
        throw ValidationError{"CreateInvoice: site " + std::to_string(*action.site) + " is not a site of company " +
                              std::to_string(*action.company)};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const invoice = draftInvoice(mapper, action.company, action.site, action.client, currentPrincipal());
    if (!invoice.has_value()) {
        throw Conflict{"CreateInvoice: client " + std::to_string(*action.client) + " has no unbilled lines at site " +
                       std::to_string(*action.site)};
    }
    sqlTxn.Commit();
    return invoiceView(mapper, *invoice);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
InvoiceView InvoiceModel::execute(const GetInvoice& action) {
    if (!action.validate()) {
        throw ValidationError{"GetInvoice: an invoice is required"};
    }
    Lightweight::DataMapper mapper;
    auto const view = invoiceView(mapper, *action.invoiceId);
    if (!isVisible(view.company)) {
        throw NotFound{"GetInvoice: invoice " + std::to_string(*action.invoiceId) + " does not exist"};
    }
    return view;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
InvoiceLeaseView InvoiceModel::execute(const GetInvoiceLease& action) {
    if (!action.validate()) {
        throw ValidationError{"GetInvoiceLease: an invoice is required"};
    }
    Lightweight::DataMapper mapper;
    auto const invoice = invoiceView(mapper, *action.invoiceId);
    if (!isVisible(invoice.company)) {
        throw NotFound{"GetInvoiceLease: invoice " + std::to_string(*action.invoiceId) + " does not exist"};
    }
    InvoiceLeaseView view{.invoice = action.invoiceId, .holder = {}, .expiresAt = {}};
    auto const rows =
        mapper.Query<db::InvoiceLeaseRecord>()
            .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::invoiceId>, "=", *action.invoiceId)
            .All();
    if (!rows.empty() && rows.front().expiresAt.Value() > nowMillis()) {
        view.holder = std::string{rows.front().holder.Value().ToStringView()};
        view.expiresAt = timestampFromMillis(rows.front().expiresAt.Value());
    }
    return view;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListInvoicesResult InvoiceModel::execute(const ListInvoices& action) {
    if (!action.validate()) {
        throw ValidationError{"ListInvoices: a company is required"};
    }
    ListInvoicesResult result;
    if (!isVisible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::InvoiceRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::InvoiceRecord::companyId>, "=", *action.company)
                          .All();
    for (auto const& row : rows) {
        result.invoices.push_back(invoiceView(mapper, static_cast<std::int64_t>(row.id.Value())));
    }
    std::ranges::sort(result.invoices, {}, [](InvoiceView const& invoice) { return *invoice.id; });
    return result;
}

}  // namespace lims
