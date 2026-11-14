// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/models/billing_run_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <morph/util/rational.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Rational;

constexpr int kItemPending = 0;
constexpr int kItemDone = 1;
constexpr int kItemFailed = 2;
constexpr int kMaxRunState = static_cast<int>(BillingRunState::FinishedWithFailures);
/// @brief The longest error text an item stores.
constexpr std::size_t kMaxErrorLength = 255;

/// @brief A batch's size as a whole number of items.
/// @param batch The batch.
/// @param action The action, for the refusal.
/// @return The number of items, at least one.
/// @throws ValidationError if the batch is fractional or below one.
[[nodiscard]] std::int64_t itemsIn(Count const& batch, std::string const& action) {
    auto const& value = *batch;
    if (value.denominator == 0 || value.numerator % value.denominator != 0 ||
        value.numerator / value.denominator < 1) {
        throw ValidationError{action + ": the batch must be a whole number of at least one item"};
    }
    return value.numerator / value.denominator;
}

/// @brief A number of items as a count.
[[nodiscard]] Count countOf(std::size_t items) {
    return Count{Rational{static_cast<std::int64_t>(items), DecimalPlaces{0}}};
}

/// @brief The pending items of run @p runId, by id.
[[nodiscard]] std::vector<db::BillingRunItemRecord> pendingItems(Lightweight::DataMapper& mapper, std::int64_t runId) {
    auto items = mapper.Query<db::BillingRunItemRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::runId>, "=", runId)
                     .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::state>, "=", kItemPending)
                     .All();
    std::ranges::sort(items, {}, [](db::BillingRunItemRecord const& item) { return item.id.Value(); });
    return items;
}

/// @brief The run @p runId as a view, with its progress counted from its items.
/// @throws NotFound if it does not exist.
/// @throws ValidationError if the row stores a state outside its enumeration.
[[nodiscard]] BillingRunView runView(Lightweight::DataMapper& mapper, std::int64_t runId) {
    auto const runs = mapper.Query<db::BillingRunRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", runId)
                          .All();
    if (runs.empty()) {
        throw NotFound{"billing run " + std::to_string(runId) + " does not exist"};
    }
    auto const& row = runs.front();
    int const state = row.state.Value();
    if (state < 0 || state > kMaxRunState) {
        throw ValidationError{"billing run " + std::to_string(runId) + " stores a state outside its range"};
    }
    auto const items = mapper.Query<db::BillingRunItemRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::runId>, "=", runId)
                           .All();
    auto const done = static_cast<std::size_t>(std::ranges::count_if(
        items, [](db::BillingRunItemRecord const& item) { return item.state.Value() == kItemDone; }));
    // The error of the failed item with the highest id, which is the one the run met last.
    std::size_t failed = 0;
    std::string lastError;
    std::int64_t lastFailed = -1;
    for (auto const& item : items) {
        if (item.state.Value() != kItemFailed) {
            continue;
        }
        ++failed;
        if (auto const itemId = static_cast<std::int64_t>(item.id.Value());
            itemId > lastFailed && item.error.Value()) {
            lastFailed = itemId;
            lastError = std::string{item.error.Value()->ToStringView()};
        }
    }
    return BillingRunView{
        .id = BillingRunId{runId},
        .company = CompanyId{row.companyId.Value()},
        .label = std::string{row.label.Value().ToStringView()},
        .state = static_cast<BillingRunState>(state),
        .total = countOf(items.size()),
        .done = countOf(done),
        .failed = countOf(failed),
        .lastError = std::move(lastError),
        .startedAt = timestampFromMillis(row.startedAt.Value()),
        .startedBy = std::string{row.startedBy.Value().ToStringView()},
    };
}

/// @brief The runs of @p company for the month @p label.
[[nodiscard]] std::vector<db::BillingRunRecord> runsOf(Lightweight::DataMapper& mapper, CompanyId company,
                                                       std::string const& label) {
    return mapper.Query<db::BillingRunRecord>()
        .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::companyId>, "=", *company)
        .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::label>, "=", label)
        .All();
}

/// @brief Starts @p company's run for the month of now, in the caller's transaction: the run, and one pending item per
///        client and site with unbilled lines.
/// @return The run's id.
/// @throws Conflict if the company already ran this month.
[[nodiscard]] std::int64_t startRun(Lightweight::DataMapper& mapper, CompanyId company, std::string const& author) {
    std::string const label = monthLabel(calendarMonthOf(::morph::ladder::now()));
    if (!runsOf(mapper, company, label).empty()) {
        throw Conflict{"company " + std::to_string(*company) + " already ran " + label};
    }
    std::set<std::pair<std::int64_t, std::int64_t>> pairs;
    for (auto const& line : mapper.Query<db::BillableLineRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::companyId>, "=", *company)
                                .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::state>, "=",
                                       static_cast<int>(BillableLineState::Unbilled))
                                .All()) {
        pairs.emplace(line.clientId.Value(), line.siteId.Value());
    }
    db::BillingRunRecord run;
    run.companyId = *company;
    run.label = Lightweight::SqlAnsiString<7>{label};
    // A run with nothing to draft has nothing left to do, so it never reads as running.
    run.state = static_cast<int>(pairs.empty() ? BillingRunState::Finished : BillingRunState::Running);
    run.startedAt = nowMillis();
    run.startedBy = Lightweight::SqlAnsiString<64>{author};
    faultPoint("run.beforeStartWrite", mapper.Connection());
    try {
        mapper.Create(run);
    } catch (std::exception const&) {
        // Another start slipped in between the lookup above and this write, and the unique index refused this one.
        // Anything else is not a lost race and propagates.
        if (!runsOf(mapper, company, label).empty()) {
            throw Conflict{"company " + std::to_string(*company) + " already ran " + label};
        }
        throw;
    }
    auto const runId = static_cast<std::int64_t>(run.id.Value());
    for (auto const& [client, site] : pairs) {
        db::BillingRunItemRecord item;
        item.runId = runId;
        item.clientId = client;
        item.siteId = site;
        item.state = kItemPending;
        mapper.Create(item);
    }
    return runId;
}

/// @brief Marks @p item failed with @p reason, if it is still pending.
void failItem(Lightweight::DataMapper& mapper, db::BillingRunItemRecord const& item, std::string reason) {
    reason.resize(std::min(reason.size(), kMaxErrorLength));
    auto query = mapper.Connection().Query(db::BillingRunItemRecord::TableName).Update();
    query.Set("state", kItemFailed).Set("error", reason);
    (void)query.Where("id", "=", static_cast<std::int64_t>(item.id.Value())).Where("state", "=", kItemPending);
    Lightweight::SqlStatement statement{mapper.Connection()};
    static_cast<void>(statement.ExecuteDirect(query.ToSql()));
}

/// @brief Drafts @p item's invoice and marks the item done, in one transaction.
///
/// The invoice and the item's being done are one fact: a run resumed after a failure here drafts the item again from
/// scratch, and one resumed after the commit skips it. The item is claimed by a conditional update that requires it
/// still pending, so two advances of one run cannot both finish an item and the second one's invoice rolls back.
///
/// A lost claim leaves the item pending for the next advance to judge. A refusal by the domain (a currency mismatch,
/// a total that does not fit) rolls the draft back and marks the item failed with the refusal's text, so one poison
/// item neither repeats on every tick nor holds up the items and runs after it. Any other exception, such as a
/// database failure or a server stopping, is not the item's fault and propagates with the item still pending.
void draftItem(Lightweight::DataMapper& mapper, db::BillingRunItemRecord const& item, CompanyId company,
               std::string const& author) {
    try {
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        auto const invoice =
            draftInvoice(mapper, company, SiteId{item.siteId.Value()}, ClientId{item.clientId.Value()}, author);
        faultPoint("run.beforeItemDone", mapper.Connection());
        auto query = mapper.Connection().Query(db::BillingRunItemRecord::TableName).Update();
        query.Set("state", kItemDone);
        if (invoice.has_value()) {
            query.Set("invoice_id", *invoice);
        }
        (void)query.Where("id", "=", static_cast<std::int64_t>(item.id.Value())).Where("state", "=", kItemPending);
        Lightweight::SqlStatement statement{mapper.Connection()};
        if (statement.ExecuteDirect(query.ToSql()).NumRowsAffected() != 1U) {
            throw Conflict{"billing run item " + std::to_string(item.id.Value()) + " was drafted meanwhile"};
        }
        sqlTxn.Commit();
    } catch (Conflict const&) {
        // The transaction rolled back with the unwinding, so the item is exactly as it was: pending.
        return;
    } catch (LimsError const& refusal) {
        // Rolled back likewise; the failure is recorded outside the draft's transaction, which is gone.
        failItem(mapper, item, refusal.what());
    }
}

/// @brief Drafts up to @p items pending items of run @p runId, and ends the run when none is left: Finished when every
///        item is done, FinishedWithFailures when any failed.
///
/// The caller has read the run in the same call and no action deletes one, so it exists.
void advanceRun(Lightweight::DataMapper& mapper, std::int64_t runId, CompanyId company, std::int64_t items,
                std::string const& author) {
    for (auto const& item : pendingItems(mapper, runId)) {
        if (items-- == 0) {
            break;
        }
        draftItem(mapper, item, company, author);
    }
    if (pendingItems(mapper, runId).empty()) {
        auto const failed = !mapper.Query<db::BillingRunItemRecord>()
                                 .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::runId>, "=", runId)
                                 .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::state>, "=", kItemFailed)
                                 .All()
                                 .empty();
        auto runs = mapper.Query<db::BillingRunRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", runId)
                        .All();
        runs.front().state =
            static_cast<int>(failed ? BillingRunState::FinishedWithFailures : BillingRunState::Finished);
        mapper.Update(runs.front());
    }
}

/// @brief Every company whose billing day has come in the month of @p now and that has no run for the month.
[[nodiscard]] std::vector<CompanyId> dueCompanies(Lightweight::DataMapper& mapper,
                                                  ::morph::time::Timestamp const& now) {
    std::string const label = monthLabel(calendarMonthOf(now));
    std::int64_t const today = dayOfMonth(now);
    std::vector<CompanyId> due;
    for (auto const& settings : mapper.Query<db::BillingSettingsRecord>().All()) {
        if (settings.billingDay.Value() > today) {
            continue;
        }
        auto const company = settings.companyId.Value();
        if (runsOf(mapper, CompanyId{company}, label).empty()) {
            due.emplace_back(company);
        }
    }
    std::ranges::sort(due, {}, [](CompanyId company) { return *company; });
    return due;
}

/// @brief Every run that is still drafting, by id.
[[nodiscard]] std::vector<db::BillingRunRecord> runningRuns(Lightweight::DataMapper& mapper) {
    auto runs = mapper.Query<db::BillingRunRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::state>, "=",
                           static_cast<int>(BillingRunState::Running))
                    .All();
    std::ranges::sort(runs, {}, [](db::BillingRunRecord const& run) { return run.id.Value(); });
    return runs;
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillingRunView BillingRunModel::execute(const StartBillingRun& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"StartBillingRun: a company is required"};
    }
    if (!PriceListModel::settingsOf(action.company).has_value()) {
        throw NotFound{"StartBillingRun: company " + std::to_string(*action.company) + " has no billing settings"};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    std::int64_t const started = startRun(mapper, action.company, currentPrincipal());
    sqlTxn.Commit();
    return runView(mapper, started);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillingRunView BillingRunModel::execute(const AdvanceBillingRun& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AdvanceBillingRun: a run and a batch are required"};
    }
    auto const items = itemsIn(action.batch, "AdvanceBillingRun");
    Lightweight::DataMapper mapper;
    auto const runs = mapper.Query<db::BillingRunRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", *action.runId)
                          .All();
    if (runs.empty()) {
        throw NotFound{"AdvanceBillingRun: run " + std::to_string(*action.runId) + " does not exist"};
    }
    advanceRun(mapper, *action.runId, CompanyId{runs.front().companyId.Value()}, items, currentPrincipal());
    return runView(mapper, *action.runId);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillingScheduleResult BillingRunModel::execute(const RunBillingSchedule& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RunBillingSchedule: a batch is required"};
    }
    auto const items = itemsIn(action.batch, "RunBillingSchedule");
    requireServicePrincipal("RunBillingSchedule");
    std::string const principal = currentPrincipal();
    // The schedule acts only where its principal holds the service role, so the role assignments say what it may do.
    auto const serves = [&principal](CompanyId company) {
        auto const held = OrgModel::rolesIn(principal, Scope{company});
        return std::ranges::find(held, LimsRole::Service) != held.end();
    };
    Lightweight::DataMapper mapper;
    for (CompanyId const company : dueCompanies(mapper, ::morph::ladder::now())) {
        if (serves(company)) {
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            static_cast<void>(startRun(mapper, company, principal));
            sqlTxn.Commit();
        }
    }
    BillingScheduleResult result;
    for (auto const& run : runningRuns(mapper)) {
        CompanyId const company{run.companyId.Value()};
        if (serves(company)) {
            auto const runId = static_cast<std::int64_t>(run.id.Value());
            advanceRun(mapper, runId, company, items, principal);
            result.runs.emplace_back(runView(mapper, runId));
        }
    }
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListBillingRunsResult BillingRunModel::execute(const ListBillingRuns& action) {
    if (!action.validate()) {
        throw ValidationError{"ListBillingRuns: a company is required"};
    }
    ListBillingRunsResult result;
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(companies, action.company) == companies.end()) {
        return result;
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::BillingRunRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::companyId>, "=", *action.company)
                          .All();
    for (auto const& row : rows) {
        result.runs.emplace_back(runView(mapper, static_cast<std::int64_t>(row.id.Value())));
    }
    std::ranges::sort(result.runs, {}, [](BillingRunView const& run) { return *run.id; });
    return result;
}

}  // namespace lims
