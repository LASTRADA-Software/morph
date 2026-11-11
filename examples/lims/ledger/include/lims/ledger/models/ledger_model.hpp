// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/table/query.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/dto/entry_dto.hpp"
#include "lims/ledger/dto/rate_dto.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// `LedgerModel`: a company's chart of accounts, its monthly periods and its journal entries.

namespace lims {

/// @brief The books of every company.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. Knows
/// nothing of invoices or stock: a poster's document reaches it as a `PostingRequest`.
class LedgerModel {
public:
    /// @brief Adds an account to a company's chart.
    /// @param action The company, code, name and type.
    /// @return The account.
    /// @throws ValidationError if the account is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company's chart already has the code.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    AccountView execute(const CreateAccount& action);

    /// @brief Lists a company's accounts, when the caller can see the company.
    /// @param action The company.
    /// @return Its accounts by code; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named, or a row holds a type outside its enumeration.
    ListAccountsResult execute(const ListAccounts& action);

    /// @brief Opens the month an instant falls in.
    /// @param action The company and an instant in the month.
    /// @return The period, open.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the month is already a period of the company.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const OpenPeriod& action);

    /// @brief Lists a company's periods, when the caller can see the company.
    /// @param action The company.
    /// @return Its periods by start.
    /// @throws ValidationError if no company is named, or a row holds a state outside its enumeration.
    ListPeriodsResult execute(const ListPeriods& action);

    /// @brief `Open → Closing`.
    /// @param action The period.
    /// @return The period, closing.
    /// @throws ValidationError if no period is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is not open.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const BeginClosePeriod& action);

    /// @brief Drafts an entry in the period its date falls in.
    ///
    /// The ledger never rounds or balances for the caller: the lines must already balance in the transaction currency
    /// and in the company's currency, each at its currency's decimals.
    /// @param action The company, date, memo, currency, rate and lines.
    /// @return The draft.
    /// @throws ValidationError if the request is malformed, the lines do not balance or are not rounded, the rate is
    /// not positive, or the rate is not one for an entry in the company's own currency.
    /// @throws NotFound if the company, the date's period or an account code does not exist.
    /// @throws Conflict if the period is closing or closed.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    JournalEntryView execute(const CreateDraftEntry& action);

    /// @brief `Draft → Posted`: the entry enters the balances and can no longer change.
    /// @param action The entry.
    /// @return The entry, posted.
    /// @throws ValidationError if no entry is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is already posted.
    /// @throws Conflict if its period is closed.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    JournalEntryView execute(const PostEntry& action);

    /// @brief Corrects a posted entry with a posted reversal that names it; the original is not edited.
    /// @param action The entry and the reversal's date.
    /// @return The reversal.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the entry, or the period of the date, does not exist.
    /// @throws IllegalTransition if the entry is a draft.
    /// @throws Conflict if the entry is already reversed or the date's period is closed.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    JournalEntryView execute(const ReverseEntry& action);

    /// @brief `Closing → Closed`: freezes each account's totals in the period, in company currency.
    ///
    /// After this the period takes no draft, post or reversal, and its frozen totals no longer change.
    /// @param action The period.
    /// @return The period, closed.
    /// @throws ValidationError if no period is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is not closing.
    /// @throws Conflict if a draft entry remains in it.
    /// @throws OutOfRange if an account's total is too large to freeze exactly.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const ClosePeriod& action);

    /// @brief Enters a rate for a currency pair, valid from an instant.
    /// @param action The company, pair, start and rate.
    /// @return The rate.
    /// @throws ValidationError if the request is malformed, the rate is not positive or the currencies are the same.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the pair already has a rate from that instant.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ExchangeRateView execute(const SetExchangeRate& action);

    /// @brief Lists a company's rates, when the caller can see the company.
    /// @param action The company.
    /// @return Its rates by pair then start; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named, or a row holds a currency outside its enumeration.
    ListExchangeRatesResult execute(const ListExchangeRates& action);

    /// @brief Reads one entry, when the caller can see its company.
    /// @param action The entry.
    /// @return The entry.
    /// @throws ValidationError if no entry is named.
    /// @throws NotFound if it does not exist or its company is not visible to the caller.
    JournalEntryView execute(const GetEntry& action);

    /// @brief Lists a company's entries, when the caller can see the company.
    /// @param action The company.
    /// @return Its entries by id; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named.
    ListEntriesResult execute(const ListEntries& action);

    /// @brief Lists a page of a company's entries, sorted, filtered and counted in the database.
    ///
    /// The query is planned by `table::planQuery` and translated into one `SELECT` over the entries table, so the
    /// cost follows the page, not the journal. The order ends with the entry's id. A sort or filter on `lines` or
    /// `rate`, which the database cannot compare as the table engine does, is refused. The count and the page are
    /// read in one transaction and the total is exact.
    /// @param action The company and the query.
    /// @return The page; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named or the query is refused.
    ::morph::table::Page<JournalEntryView> execute(const ListEntriesPage& action);

    /// @brief Whether the database can sort or filter the journal by @p column.
    /// @param column The column's id.
    /// @param use What the column is wanted for.
    /// @return `false` for `lines` and `rate`, and for a name that is no column.
    [[nodiscard]] static bool canTranslateEntryColumn(std::string_view column, ::morph::table::PlanUse use);

    /// @brief Reads a page of @p company's entries for a validated plan, without checking who may see the company.
    /// @param company The company.
    /// @param plan The plan from `table::planQuery`, made with `canTranslateEntryColumn`.
    /// @return The page, with an exact total.
    /// @throws ValidationError if the plan holds an operand the database cannot compare.
    /// @throws Lightweight::SqlException if a statement fails.
    [[nodiscard]] static ::morph::table::Page<JournalEntryView> pageEntries(CompanyId company,
                                                                            ::morph::table::QueryPlan const& plan);

    /// @brief Records a posting the app's posting client sends, once per company and source key.
    /// @param action The request.
    /// @return The entry and its date; the first one recorded under the request's key when it is repeated.
    /// @throws ValidationError, NotFound, Conflict as `record`.
    /// @throws Forbidden if the caller is not one of the server's own clients (only the server's own clients send
    ///         it), or holds no service role at the request's company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PostingResult execute(const RecordPosting& action);

    /// @brief Reads the entry recorded under a source key, as a poster asks it before it retries.
    ///
    /// A repeated key must repeat the request, date included, so a poster that retries after a failure reads the
    /// first entry's date and sends that.
    /// @param action The question.
    /// @return The entry and its date; an empty entry for a company the caller cannot see, or when the key is not
    ///         recorded. The empty answer for an unseen company cannot mislead a poster: its next step, recording the
    ///         posting, refuses the same caller with the reason.
    /// @throws ValidationError if no company or source key is named.
    PostingResult execute(const FindPosting& action);

    /// @brief Reads the rate valid on a date, as a poster asks it.
    /// @param action The question.
    /// @return The rate; empty when no rate is valid on the date.
    /// @throws ValidationError if no company or date is named.
    /// @throws Forbidden if the caller cannot see the company: a missing grant is not reported as a missing rate.
    RateAnswer execute(const GetRateOn& action);

    /// @brief Records a poster's request as a posted entry, once per company and source key, in a transaction of
    ///        its own.
    ///
    /// Not itself an action: `RecordPosting` is, and calls it. Postings reach it from a module's document through
    /// the app's wiring; `recordPostingIn` is the same recording through a caller's open mapper. Checks what
    /// a hand-made entry is checked for, and that the request's rate is the ledger's rate on its date (one when the
    /// currencies are the same). The source key is recorded in the entry's own transaction, so a repeated request
    /// answers the first entry and writes nothing. A key is looked up before the dated rate, the period or the
    /// accounts are checked, so an identical repeat answers the first entry whatever has changed since. Two
    /// simultaneous first calls with one key record one entry and both answer it.
    ///
    /// A repeated key must repeat the request: its date, source reference, currency, rate and every line's account,
    /// side and amounts, compared by value. A key recorded for another request is refused; a corrected document
    /// takes a new key.
    /// @param request The request.
    /// @return The entry; the first one recorded under the request's key when it is repeated.
    /// @throws ValidationError if the request is malformed, does not balance, or names a rate other than the dated
    ///         one.
    /// @throws NotFound if the company, the period of its date or an account does not exist.
    /// @throws Conflict if the period is closed, or if the source key is already recorded for a different request.
    [[nodiscard]] static PostingResult record(PostingRequest const& request);

    /// @brief The rate from @p from to @p target valid on @p date: the one with the latest start not after it.
    /// @param company The company whose rates are asked.
    /// @param from The source currency.
    /// @param target The target currency.
    /// @param date The date.
    /// @return Units of @p target per unit of @p from; nothing when no rate is valid yet.
    [[nodiscard]] static std::optional<::morph::math::Rational> rateOn(CompanyId company, CurrencyCode from,
                                                                       CurrencyCode target,
                                                                       ::morph::time::Timestamp const& date);

    /// @brief Stores a rate, answering a start another call entered first as a Conflict.
    ///
    /// The seam under `SetExchangeRate` after its checks. A concurrent entry of the same start is refused by the
    /// unique index at the insert; the lookup then finds the winner's row and the answer is the same Conflict a
    /// later call gets.
    /// @param action The rate, already validated and authorized.
    /// @param setBy Who enters it.
    /// @param betweenLookupAndInsert Called once the lookup found no such start and before the insert; a test
    ///        enters the winner's row here. Empty in production.
    /// @return The stored rate.
    /// @throws Conflict if the start is already entered.
    /// @throws Lightweight::SqlException if the insert fails for another reason.
    [[nodiscard]] static ExchangeRateView enterRate(SetExchangeRate const& action, std::string const& setBy,
                                                    std::function<void()> const& betweenLookupAndInsert);
};

}  // namespace lims

// The guard each registered action names must be complete here: the registration instantiates its execution.
#include "lims/ledger/authorization/ledger_authorization.hpp"

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::LedgerModel, "LedgerModel")
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::CreateAccount, "CreateAccount")
BRIDGE_REGISTER_EFFECTS(lims::CreateAccount, ::morph::effects::changes<lims::AccountView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListAccounts, "ListAccounts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::OpenPeriod, "OpenPeriod")
BRIDGE_REGISTER_EFFECTS(lims::OpenPeriod, ::morph::effects::changes<lims::PeriodView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListPeriods, "ListPeriods", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::BeginClosePeriod, "BeginClosePeriod")
BRIDGE_REGISTER_EFFECTS(lims::BeginClosePeriod, ::morph::effects::updates<lims::PeriodView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::CreateDraftEntry, "CreateDraftEntry")
BRIDGE_REGISTER_EFFECTS(lims::CreateDraftEntry, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::PostEntry, "PostEntry")
BRIDGE_REGISTER_EFFECTS(lims::PostEntry, ::morph::effects::updates<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ReverseEntry, "ReverseEntry")
BRIDGE_REGISTER_EFFECTS(lims::ReverseEntry, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ClosePeriod, "ClosePeriod")
BRIDGE_REGISTER_EFFECTS(lims::ClosePeriod, ::morph::effects::updates<lims::PeriodView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::SetExchangeRate, "SetExchangeRate")
BRIDGE_REGISTER_EFFECTS(lims::SetExchangeRate, ::morph::effects::changes<lims::ExchangeRateView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListExchangeRates, "ListExchangeRates", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::GetEntry, "GetEntry", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListEntries, "ListEntries", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListEntriesPage, "ListEntriesPage", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::RecordPosting, "RecordPosting")
BRIDGE_REGISTER_EFFECTS(lims::RecordPosting, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::FindPosting, "FindPosting", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::GetRateOn, "GetRateOn", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
