// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/budget_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <vector>

#include "clock.hpp"
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"

namespace ledger {

namespace {

/// @brief Parses a `"YYYY-MM"` month string into a half-open UTC
///        `[start, end)` millisecond range, matching
///        `TransactionJournalRecord::date`'s stored epoch-millis form.
///
///        A plain UTC month range -- not the local-timezone month-boundary
///        machinery a later task ("local-time month boundary handling")
///        builds; this task's scope only needs a defensible, simple
///        conversion. Callers (`SetBudgetLimit::validate()` /
///        `GetBudgetReport::validate()`, `ledger/dto/budget_dto.hpp`) reject
///        a malformed month -- wrong length, non-digit characters, or a
///        month number outside `[1, 12]` -- before this ever runs. This
///        function still double-checks both the `from_chars` parse status
///        and `year_month_day::ok()` and throws `ValidationError` rather
///        than silently returning a garbage range, as defense in depth
///        against a caller that bypasses `validate()`.
/// @param month The `"YYYY-MM"` string to parse. Must already have passed
///        `detail::isValidYearMonth` (see `budget_dto.hpp`).
/// @return The `[start, end)` epoch-millisecond range for that UTC month.
/// @throws ValidationError if `month` cannot be parsed as digits or does
///         not name a valid calendar month.
[[nodiscard]] std::pair<std::int64_t, std::int64_t> monthRangeMs(const std::string& month) {
    int year = 0;
    int monthNum = 0;
    const auto yearResult = std::from_chars(month.data(), month.data() + 4, year);
    const auto monthResult = std::from_chars(month.data() + 5, month.data() + 7, monthNum);
    if (yearResult.ec != std::errc{} || monthResult.ec != std::errc{}) {
        throw ValidationError{"monthRangeMs: \"" + month + "\" is not a well-formed YYYY-MM month"};
    }

    const auto startDate = std::chrono::year_month_day{
        std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(monthNum)}, std::chrono::day{1}};
    if (!startDate.ok()) {
        throw ValidationError{"monthRangeMs: \"" + month + "\" is not a valid calendar month"};
    }
    const auto startSysDays = static_cast<std::chrono::sys_days>(startDate);
    const auto endSysDays =
        static_cast<std::chrono::sys_days>(startDate.year() / startDate.month() / std::chrono::last) +
        std::chrono::days{1};

    const auto startMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(startSysDays.time_since_epoch()).count();
    const auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(endSysDays.time_since_epoch()).count();
    return {startMs, endMs};
}

}  // namespace

void BudgetModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _log = std::move(log);
    _entityKeyStr = std::move(entityKey);
}

template <typename Action, typename Result>
void BudgetModel::logAction(const Action& action, const Result& result, std::string causalParentId) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "BudgetModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.outcome = ::morph::journal::Outcome::Succeeded;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = (*morph::ladder::now().value).value.time_since_epoch().count();  // server-stamped audit
                                                                                         // timestamp -- see
                                                                                         // LedgerModel::logAction's
                                                                                         // identical comment
    entry.causalParentId = std::move(causalParentId);
    _log->append(std::move(entry));
    // See LedgerModel::logAction's identical comment for why this flush is
    // load-bearing, not optional.
    _log->flush();
}

CategoryId BudgetModel::execute(const CreateCategory& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"CreateCategory: ledgerId and name are required"};
    }
    Lightweight::DataMapper mapper;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                          .All();
    if (ledgerRows.empty()) {
        throw NotFound{"CreateCategory: no such ledger"};
    }
    db::CategoryRecord categoryRow;
    categoryRow.ledger = ledgerRows.front();
    categoryRow.name = action.name;
    mapper.Create(categoryRow);
    auto result = CategoryId{static_cast<std::int64_t>(categoryRow.id.Value())};
    logAction(action, result);
    return result;
}

AccountId BudgetModel::execute(const LinkAccountToCategory& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"LinkAccountToCategory: accountId and categoryId are required"};
    }
    Lightweight::DataMapper mapper;
    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                           .All();
    auto categoryRows = mapper.Query<db::CategoryRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::id>, "=", *action.categoryId)
                            .All();
    if (accountRows.empty() || categoryRows.empty()) {
        throw NotFound{"LinkAccountToCategory: no such account or category"};
    }
    accountRows.front().category = categoryRows.front();
    mapper.Update(accountRows.front());
    auto result = AccountId{static_cast<std::int64_t>(accountRows.front().id.Value())};
    logAction(action, result);
    return result;
}

BudgetId BudgetModel::execute(const CreateBudget& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"CreateBudget: ledgerId, name, and categoryId are required"};
    }
    Lightweight::DataMapper mapper;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                          .All();
    auto categoryRows = mapper.Query<db::CategoryRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::id>, "=", *action.categoryId)
                            .All();
    if (ledgerRows.empty() || categoryRows.empty()) {
        throw NotFound{"CreateBudget: no such ledger or category"};
    }
    db::BudgetRecord budgetRow;
    budgetRow.ledger = ledgerRows.front();
    budgetRow.name = action.name;
    budgetRow.category = categoryRows.front();
    mapper.Create(budgetRow);
    auto result = BudgetId{static_cast<std::int64_t>(budgetRow.id.Value())};
    logAction(action, result);
    return result;
}

BudgetId BudgetModel::execute(const SetBudgetLimit& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"SetBudgetLimit: budgetId and a YYYY-MM month are required"};
    }
    Lightweight::DataMapper mapper;
    auto budgetRows = mapper.Query<db::BudgetRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BudgetRecord::id>, "=", *action.budgetId)
                          .All();
    if (budgetRows.empty()) {
        throw NotFound{"SetBudgetLimit: no such budget"};
    }
    db::BudgetLimitRecord limitRow;
    limitRow.budget = budgetRows.front();
    limitRow.month = action.month;
    limitRow.limitNum = action.limit.numerator;
    limitRow.limitDen = action.limit.denominator;
    limitRow.limitDp = static_cast<int>(action.limit.decimalPlaces.value);
    limitRow.currencyCode = currencyToCode(action.currency);  // Task 7's helper
    mapper.Create(limitRow);
    logAction(action, action.budgetId);
    return action.budgetId;
}

GetBudgetReportResult BudgetModel::execute(const GetBudgetReport& action) {
    if (!action.validate()) {
        throw ValidationError{"GetBudgetReport: budgetId and a YYYY-MM month are required"};
    }
    Lightweight::DataMapper mapper;
    auto budgetRows = mapper.Query<db::BudgetRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BudgetRecord::id>, "=", *action.budgetId)
                          .All();
    if (budgetRows.empty()) {
        throw NotFound{"GetBudgetReport: no such budget"};
    }
    auto limitRows = mapper.Query<db::BudgetLimitRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::BudgetLimitRecord::budget>, "=", *action.budgetId)
                         .Where(::Lightweight::FieldNameOf<&db::BudgetLimitRecord::month>, "=", action.month)
                         .All();

    // In-model summation, never a raw SQL SUM() over the Rational columns
    // (design spec §3 -- SQL cannot combine differing per-row denominators
    // meaningfully). "Matching legs" = every TransactionLegRecord whose
    // account is linked (via the Task 10 schema addition) to this budget's
    // category, and whose parent TransactionJournalRecord's date falls
    // within the requested UTC month.
    //
    // Three narrow queries, joined in code (no join support needed beyond
    // WhereIn -- the same shape bank::StatementModel/BudgetModel already use
    // for an identical "accounts -> legs" fan-out):
    //   1. accounts belonging to the budget's category,
    //   2. journals belonging to the budget's own ledger AND whose date
    //      falls in the month's [start, end) range -- the ledger filter is
    //      required for correctness (a budget only ever reports on its own
    //      ledger's activity) and for scalability (without it this query
    //      collects every journal across every ledger in the database for
    //      that month, and step 3's WhereIn list grows unbounded as the
    //      database grows),
    //   3. legs whose account is in (1) AND whose journal is in (2).
    const auto categoryId = budgetRows.front().category.Value();
    const auto ledgerId = budgetRows.front().ledger.Value();
    auto categoryAccountRows = mapper.Query<db::AccountRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::AccountRecord::category>, "=", categoryId)
                                   .All();
    std::vector<std::uint64_t> accountIds;
    accountIds.reserve(categoryAccountRows.size());
    for (const auto& accountRow : categoryAccountRows) {
        accountIds.push_back(accountRow.id.Value());
    }

    morph::math::Rational spent{morph::math::Numerator{0}, morph::math::Denominator{1}, morph::math::DecimalPlaces{2}};
    if (!accountIds.empty()) {
        const auto [monthStartMs, monthEndMs] = monthRangeMs(action.month);
        auto journalRows =
            mapper.Query<db::TransactionJournalRecord>()
                .Where(::Lightweight::FieldNameOf<&db::TransactionJournalRecord::ledger>, "=", ledgerId)
                .Where(::Lightweight::FieldNameOf<&db::TransactionJournalRecord::date>, ">=", monthStartMs)
                .Where(::Lightweight::FieldNameOf<&db::TransactionJournalRecord::date>, "<", monthEndMs)
                .All();
        std::vector<std::uint64_t> journalIds;
        journalIds.reserve(journalRows.size());
        for (const auto& journalRow : journalRows) {
            journalIds.push_back(journalRow.id.Value());
        }

        if (!journalIds.empty()) {
            auto legRows = mapper.Query<db::TransactionLegRecord>()
                               .WhereIn(::Lightweight::FieldNameOf<&db::TransactionLegRecord::account>, accountIds)
                               .WhereIn(::Lightweight::FieldNameOf<&db::TransactionLegRecord::journal>, journalIds)
                               .All();
            for (const auto& legRow : legRows) {
                const auto legAmount = morph::math::Rational{
                    morph::math::Numerator{legRow.amountNum.Value()},
                    morph::math::Denominator{legRow.amountDen.Value()},
                    morph::math::DecimalPlaces{static_cast<std::uint32_t>(legRow.amountDp.Value())}};
                spent = spent + legAmount;
            }
        }
    }

    Currency currency = Currency::USD;
    morph::math::Rational limit = spent;
    if (!limitRows.empty()) {
        limit = morph::math::Rational{
            morph::math::Numerator{limitRows.front().limitNum.Value()},
            morph::math::Denominator{limitRows.front().limitDen.Value()},
            morph::math::DecimalPlaces{static_cast<std::uint32_t>(limitRows.front().limitDp.Value())}};
        currency = codeToCurrency(limitRows.front().currencyCode.Value().ToStringView());
    }
    return GetBudgetReportResult{.limit = limit, .spent = spent, .currency = currency};
}

}  // namespace ledger
