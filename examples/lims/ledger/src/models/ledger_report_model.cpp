// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/models/ledger_report_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

/// @brief One posted line, in company currency, with what the reports need of its entry.
struct PostedLine {
    std::int64_t entryId = 0;
    std::int64_t dateMs = 0;
    std::string sourceReference;
    PostingSide side = PostingSide::Debit;
    Rational value;
};

/// @brief An account's running debit and credit totals.
struct AccountTotals {
    ExactSum debit;
    ExactSum credit;
};

using TotalsByAccount = std::map<std::uint64_t, AccountTotals>;

/// @brief Adds @p value to @p sum or throws `OutOfRange` naming @p what.
/// @param sum The running total.
/// @param value The value.
/// @param what What the total is, for the message.
/// @throws OutOfRange if the total would not fit.
void addExact(ExactSum& sum, Rational const& value, std::string_view what) {
    if (!sum.add(value)) {
        throw OutOfRange{std::string{what} + " is too large to total exactly"};
    }
}

/// @brief @p value as an amount at @p places decimals.
/// @param value The value.
/// @param places The company currency's decimals.
/// @return The amount.
[[nodiscard]] Amount asAmount(Rational value, DecimalPlaces places) {
    value.decimalPlaces = places;
    return Amount{value};
}

/// @brief Debit less credit.
/// @param debit The debit total.
/// @param credit The credit total.
/// @return The balance.
/// @throws OutOfRange if it does not fit.
[[nodiscard]] Rational balanceOf(Rational const& debit, Rational const& credit) {
    auto const balance = ::morph::math::checkedSub(debit, credit);
    if (!balance.has_value()) {
        throw OutOfRange{"an account's balance is too large to compute exactly"};
    }
    return *balance;
}

/// @brief The first instant after the month a period covers.
/// @param startMs The period's start.
/// @return The next month's first instant, in epoch milliseconds.
[[nodiscard]] std::int64_t periodEndMillis(std::int64_t startMs) {
    CalendarMonth next = calendarMonthOf(timestampFromMillis(startMs));
    if (next.month == Month::December) {
        next.month = Month::January;
        ++next.year;
    } else {
        next.month = static_cast<Month>(static_cast<unsigned>(next.month) + 1U);
    }
    return monthStartMillis(next);
}

/// @brief The company's currency's decimals.
/// @param company The company.
/// @return They.
/// @throws NotFound if the company does not exist.
[[nodiscard]] DecimalPlaces placesOf(CompanyId company) {
    auto const found = OrgModel::findCompany(company);
    if (!found.has_value()) {
        throw NotFound{"company " + std::to_string(*company) + " does not exist"};
    }
    return DecimalPlaces{decimalsOf(found->baseCurrency)};
}

/// @brief The row of account @p code of @p company.
/// @param mapper The open mapper.
/// @param company The company.
/// @param code The account's code.
/// @return The row.
/// @throws NotFound if the company has no such account.
[[nodiscard]] db::AccountRecord accountOf(Lightweight::DataMapper& mapper, CompanyId company,
                                          std::string const& code) {
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::code>, "=", code)
                    .All();
    if (rows.empty()) {
        throw NotFound{"company " + std::to_string(*company) + " has no account " + code};
    }
    return rows.front();
}

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return Whether a role of theirs reaches it.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

/// @brief The ids of @p company's closed periods that end on or before the instant after @p throughMs.
/// @param mapper The open mapper.
/// @param company The company.
/// @param throughMs The last instant counted.
/// @return The periods, each wholly inside the range.
[[nodiscard]] std::set<std::int64_t> frozenPeriods(Lightweight::DataMapper& mapper, CompanyId company,
                                                   std::int64_t throughMs) {
    std::set<std::int64_t> frozen;
    for (auto const& period : mapper.Query<db::PeriodRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *company)
                                  .All()) {
        if (period.state.Value() == static_cast<int>(PeriodState::Closed) &&
            periodEndMillis(period.startMs.Value()) - 1 <= throughMs) {
            frozen.insert(static_cast<std::int64_t>(period.id.Value()));
        }
    }
    return frozen;
}

/// @brief The posted lines of @p company dated from @p fromMs to @p toMs, outside @p skipPeriods.
/// @param mapper The open mapper.
/// @param company The company.
/// @param fromMs The first instant.
/// @param toMs The last instant.
/// @param skipPeriods Periods whose frozen totals stand in for their lines.
/// @param onlyAccount The account to keep; every account when empty.
/// @return The lines by date then entry, each with its account's row id in the pair's first member.
[[nodiscard]] std::vector<std::pair<std::uint64_t, PostedLine>> postedLines(Lightweight::DataMapper& mapper,
                                                                            CompanyId company, std::int64_t fromMs,
                                                                            std::int64_t toMs,
                                                                            std::set<std::int64_t> const& skipPeriods,
                                                                            std::optional<std::uint64_t> onlyAccount) {
    std::vector<std::pair<std::uint64_t, PostedLine>> lines;
    for (auto const& entry :
         mapper.Query<db::EntryRecord>()
             .Where(::Lightweight::FieldNameOf<&db::EntryRecord::companyId>, "=", *company)
             .Where(::Lightweight::FieldNameOf<&db::EntryRecord::state>, "=", static_cast<int>(EntryState::Posted))
             .All()) {
        if (entry.dateMs.Value() < fromMs || entry.dateMs.Value() > toMs ||
            skipPeriods.contains(entry.period.Value())) {
            continue;
        }
        for (auto const& line : mapper.Query<db::EntryLineRecord>()
                                    .Where(::Lightweight::FieldNameOf<&db::EntryLineRecord::entry>, "=",
                                           static_cast<std::int64_t>(entry.id.Value()))
                                    .All()) {
            auto const account = static_cast<std::uint64_t>(line.account.Value());
            if (onlyAccount.has_value() && account != *onlyAccount) {
                continue;
            }
            int const side = line.side.Value();
            if (side != static_cast<int>(PostingSide::Debit) && side != static_cast<int>(PostingSide::Credit)) {
                throw ValidationError{"entry " + std::to_string(entry.id.Value()) +
                                      " stores a line side out of range"};
            }
            lines.emplace_back(account,
                               PostedLine{
                                   .entryId = static_cast<std::int64_t>(entry.id.Value()),
                                   .dateMs = entry.dateMs.Value(),
                                   .sourceReference = std::string{entry.sourceReference.Value().ToStringView()},
                                   .side = static_cast<PostingSide>(side),
                                   .value = Rational{Numerator{line.companyNum.Value()},
                                                     Denominator{line.companyDen.Value()}, DecimalPlaces{0}},
                               });
        }
    }
    std::ranges::sort(lines, {}, [](auto const& line) { return std::pair{line.second.dateMs, line.second.entryId}; });
    return lines;
}

/// @brief Every account's totals through @p throughMs: frozen totals for the closed periods wholly inside the
///        range, and the lines of everything else.
/// @param mapper The open mapper.
/// @param company The company.
/// @param throughMs The last instant counted.
/// @param places The company currency's decimals.
/// @param onlyAccount The account to total; every account when empty.
/// @return The totals by account row id.
/// @throws OutOfRange if a total does not fit.
[[nodiscard]] TotalsByAccount totalsThrough(Lightweight::DataMapper& mapper, CompanyId company, std::int64_t throughMs,
                                            DecimalPlaces places, std::optional<std::uint64_t> onlyAccount) {
    TotalsByAccount totals;
    auto const frozen = frozenPeriods(mapper, company, throughMs);
    for (std::int64_t const period : frozen) {
        for (auto const& row : mapper.Query<db::PeriodBalanceRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::PeriodBalanceRecord::period>, "=", period)
                                   .All()) {
            auto const account = static_cast<std::uint64_t>(row.account.Value());
            if (onlyAccount.has_value() && account != *onlyAccount) {
                continue;
            }
            auto& sums = totals[account];
            addExact(sums.debit, Rational{Numerator{row.debitNum.Value()}, Denominator{row.debitDen.Value()}, places},
                     "an account's total");
            addExact(sums.credit,
                     Rational{Numerator{row.creditNum.Value()}, Denominator{row.creditDen.Value()}, places},
                     "an account's total");
        }
    }
    for (auto const& [account, line] : postedLines(mapper, company, 0, throughMs, frozen, onlyAccount)) {
        auto& sums = totals[account];
        addExact(line.side == PostingSide::Debit ? sums.debit : sums.credit, line.value, "an account's total");
    }
    return totals;
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
TrialBalanceView LedgerReportModel::execute(const TrialBalance& action) {
    if (!action.validate()) {
        throw ValidationError{"TrialBalance: a company and a date are required"};
    }
    TrialBalanceView view{
        .company = action.company,
        .asOf = action.asOf,
        .rows = {},
        .totalDebit = {},
        .totalCredit = {},
    };
    if (!visible(action.company)) {
        return view;
    }
    DecimalPlaces const places = placesOf(action.company);
    Lightweight::DataMapper mapper;
    auto const totals = totalsThrough(mapper, action.company, millisOf(action.asOf), places, std::nullopt);
    ExactSum totalDebit;
    ExactSum totalCredit;
    for (auto const& account :
         mapper.Query<db::AccountRecord>()
             .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *action.company)
             .All()) {
        auto const found = totals.find(account.id.Value());
        if (found == totals.end()) {
            continue;
        }
        Rational const debit = found->second.debit.total();
        Rational const credit = found->second.credit.total();
        addExact(totalDebit, debit, "the trial balance's debit total");
        addExact(totalCredit, credit, "the trial balance's credit total");
        view.rows.push_back(TrialBalanceRow{
            .accountCode = std::string{account.code.Value().ToStringView()},
            .accountName = std::string{account.name.Value().ToStringView()},
            .type = db::accountTypeOf(account),
            .debit = asAmount(debit, places),
            .credit = asAmount(credit, places),
        });
    }
    std::ranges::sort(view.rows, {}, &TrialBalanceRow::accountCode);
    view.totalDebit = asAmount(totalDebit.total(), places);
    view.totalCredit = asAmount(totalCredit.total(), places);
    return view;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
AccountBalanceView LedgerReportModel::execute(const AccountBalance& action) {
    if (!action.validate()) {
        throw ValidationError{"AccountBalance: a company, an account and a date are required"};
    }
    AccountBalanceView view{.accountCode = action.accountCode, .debit = {}, .credit = {}, .balance = {}};
    if (!visible(action.company)) {
        return view;
    }
    DecimalPlaces const places = placesOf(action.company);
    Lightweight::DataMapper mapper;
    auto const account = accountOf(mapper, action.company, action.accountCode).id.Value();
    auto const totals = totalsThrough(mapper, action.company, millisOf(action.asOf), places, account);
    Rational debit{0, places};
    Rational credit{0, places};
    if (auto const found = totals.find(account); found != totals.end()) {
        debit = found->second.debit.total();
        credit = found->second.credit.total();
    }
    view.debit = asAmount(debit, places);
    view.credit = asAmount(credit, places);
    view.balance = asAmount(balanceOf(debit, credit), places);
    return view;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
AccountLedgerView LedgerReportModel::execute(const AccountLedger& action) {
    if (!action.validate() || millisOf(action.to) < millisOf(action.from)) {
        throw ValidationError{
            "AccountLedger: a company, an account and a range that does not end before it starts "
            "are required"};
    }
    AccountLedgerView view{.accountCode = action.accountCode, .lines = {}};
    if (!visible(action.company)) {
        return view;
    }
    DecimalPlaces const places = placesOf(action.company);
    Lightweight::DataMapper mapper;
    auto const account = accountOf(mapper, action.company, action.accountCode).id.Value();
    std::int64_t const fromMs = millisOf(action.from);
    Rational balance{0, places};
    // What was posted before the range: the running balance starts there.
    if (auto const before = totalsThrough(mapper, action.company, fromMs - 1, places, account);
        before.contains(account)) {
        balance = balanceOf(before.at(account).debit.total(), before.at(account).credit.total());
    }
    for (auto const& posted : postedLines(mapper, action.company, fromMs, millisOf(action.to), {}, account)) {
        PostedLine const& line = posted.second;
        auto const next = line.side == PostingSide::Debit ? ::morph::math::checkedAdd(balance, line.value)
                                                          : ::morph::math::checkedSub(balance, line.value);
        if (!next.has_value()) {
            throw OutOfRange{"an account's running balance is too large to compute exactly"};
        }
        balance = *next;
        view.lines.push_back(AccountLedgerLine{
            .entryId = JournalEntryId{line.entryId},
            .date = timestampFromMillis(line.dateMs),
            .sourceReference = line.sourceReference,
            .side = line.side,
            .companyAmount = asAmount(line.value, places),
            .balance = asAmount(balance, places),
        });
    }
    return view;
}

}  // namespace lims
