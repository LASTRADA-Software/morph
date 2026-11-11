// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/models/ledger_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

constexpr int kMaxPeriodState = static_cast<int>(PeriodState::Closed);

/// @brief Maps an account row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a type outside its enumeration.
[[nodiscard]] AccountView toView(db::AccountRecord const& row) {
    return AccountView{
        .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .code = std::string{row.code.Value().ToStringView()},
        .name = std::string{row.name.Value().ToStringView()},
        .type = db::accountTypeOf(row),
    };
}

/// @brief Maps a period row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a state outside its enumeration.
[[nodiscard]] PeriodView toView(db::PeriodRecord const& row) {
    int const state = row.state.Value();
    if (state < 0 || state > kMaxPeriodState) {
        throw ValidationError{"period " + std::to_string(row.id.Value()) + " stores a state outside its range"};
    }
    auto const start = timestampFromMillis(row.startMs.Value());
    return PeriodView{
        .id = PeriodId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .start = start,
        .label = monthLabel(calendarMonthOf(start)),
        .state = static_cast<PeriodState>(state),
    };
}

/// @brief Throws `NotFound` unless @p company exists.
/// @param where The action, for the message.
/// @param company The company.
void requireCompany(std::string_view where, CompanyId company) {
    if (!OrgModel::findCompany(company).has_value()) {
        throw NotFound{std::string{where} + ": company " + std::to_string(*company) + " does not exist"};
    }
}

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return Whether a role of theirs reaches it.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
AccountView LedgerModel::execute(const CreateAccount& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "CreateAccount: a company, a code of 1 to 16 upper-case letters and digits and a name are required"};
    }
    requireCompany("CreateAccount", action.company);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!mapper.Query<db::AccountRecord>()
             .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *action.company)
             .Where(::Lightweight::FieldNameOf<&db::AccountRecord::code>, "=", action.code)
             .All()
             .empty()) {
        throw Conflict{"CreateAccount: company " + std::to_string(*action.company) + " already has account " +
                       action.code};
    }
    db::AccountRecord row;
    row.companyId = *action.company;
    row.code = Lightweight::SqlAnsiString<16>{action.code};
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.type = static_cast<int>(action.type);
    mapper.Create(row);
    sqlTxn.Commit();
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListAccountsResult LedgerModel::execute(const ListAccounts& action) {
    if (!action.validate()) {
        throw ValidationError{"ListAccounts: a company is required"};
    }
    ListAccountsResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::AccountRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *action.company)
                               .All()) {
        result.accounts.push_back(toView(row));
    }
    std::ranges::sort(result.accounts, {}, &AccountView::code);
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PeriodView LedgerModel::execute(const OpenPeriod& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"OpenPeriod: a company and a month are required"};
    }
    requireCompany("OpenPeriod", action.company);
    auto const month = calendarMonthOf(action.month);
    std::int64_t const start = monthStartMillis(month);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!mapper.Query<db::PeriodRecord>()
             .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *action.company)
             .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::startMs>, "=", start)
             .All()
             .empty()) {
        throw Conflict{"OpenPeriod: " + monthLabel(month) + " is already a period of company " +
                       std::to_string(*action.company)};
    }
    db::PeriodRecord row;
    row.companyId = *action.company;
    row.startMs = start;
    row.state = static_cast<int>(PeriodState::Open);
    mapper.Create(row);
    sqlTxn.Commit();
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListPeriodsResult LedgerModel::execute(const ListPeriods& action) {
    if (!action.validate()) {
        throw ValidationError{"ListPeriods: a company is required"};
    }
    ListPeriodsResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::PeriodRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *action.company)
                               .All()) {
        result.periods.push_back(toView(row));
    }
    std::ranges::sort(result.periods, {}, [](PeriodView const& period) { return period.start; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PeriodView LedgerModel::execute(const BeginClosePeriod& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"BeginClosePeriod: a period is required"};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto rows = mapper.Query<db::PeriodRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", *action.periodId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"BeginClosePeriod: period " + std::to_string(*action.periodId) + " does not exist"};
    }
    // Judged again inside the transaction: the scope is read from the row the write commits. The action's guard ran it
    // first, on every path.
    requireAuthorized(action);
    auto& row = rows.front();
    if (row.state.Value() != static_cast<int>(PeriodState::Open)) {
        throw IllegalTransition{"BeginClosePeriod: period " + toView(row).label + " is not open"};
    }
    row.state = static_cast<int>(PeriodState::Closing);
    mapper.Update(row);
    sqlTxn.Commit();
    return toView(row);
}

}  // namespace lims
