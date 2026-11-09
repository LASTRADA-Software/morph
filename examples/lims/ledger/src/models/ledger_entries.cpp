// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <morph/session/session.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/ledger/core/balance.hpp"
#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/ledger/models/ledger_model.hpp"
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

constexpr int kMaxEntryState = static_cast<int>(EntryState::Posted);
constexpr int kMaxCurrencyCode = static_cast<int>(CurrencyCode::JPY);

/// @brief @p num / @p den at @p currency's decimals.
/// @param num The numerator.
/// @param den The denominator.
/// @param currency The currency.
/// @return The amount.
[[nodiscard]] Amount amountOf(std::int64_t num, std::int64_t den, CurrencyCode currency) {
    return Amount{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{decimalsOf(currency)}}};
}

/// @brief The rows of entry @p id: its header and its lines in order.
struct LoadedEntry {
    db::EntryRecord header;
    std::vector<db::EntryLineRecord> lines;
};

/// @brief Loads entry @p id.
/// @param mapper The open mapper.
/// @param entryId The entry.
/// @return It, or nothing.
[[nodiscard]] std::optional<LoadedEntry> loadEntry(Lightweight::DataMapper& mapper, std::int64_t entryId) {
    auto headers =
        mapper.Query<db::EntryRecord>().Where(::Lightweight::FieldNameOf<&db::EntryRecord::id>, "=", entryId).All();
    if (headers.empty()) {
        return std::nullopt;
    }
    auto lines = mapper.Query<db::EntryLineRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::EntryLineRecord::entry>, "=", entryId)
                     .All();
    std::ranges::sort(lines, {}, [](db::EntryLineRecord const& line) { return line.position.Value(); });
    return LoadedEntry{.header = headers.front(), .lines = std::move(lines)};
}

/// @brief Loads entry @p entryId, which the caller has just written.
/// @param mapper The open mapper.
/// @param entryId The entry.
/// @return It.
/// @throws NotFound if it is gone.
[[nodiscard]] LoadedEntry loadWritten(Lightweight::DataMapper& mapper, std::int64_t entryId) {
    auto entry = loadEntry(mapper, entryId);
    if (!entry.has_value()) {
        throw NotFound{"entry " + std::to_string(entryId) + " does not exist"};
    }
    return *std::move(entry);
}

/// @brief The company's base currency.
/// @param company The company.
/// @return It.
/// @throws NotFound if the company does not exist.
[[nodiscard]] CurrencyCode companyCurrency(CompanyId company) {
    auto const found = OrgModel::findCompany(company);
    if (!found.has_value()) {
        throw NotFound{"company " + std::to_string(*company) + " does not exist"};
    }
    return found->baseCurrency;
}

/// @brief Maps an entry's rows to its view.
/// @param mapper The open mapper, for the account codes.
/// @param entry The rows.
/// @return The view.
/// @throws ValidationError if a row holds a state, currency or side outside its enumeration, or a line names an
///         account that does not exist.
[[nodiscard]] JournalEntryView toView(Lightweight::DataMapper& mapper, LoadedEntry const& entry) {
    auto const& row = entry.header;
    int const state = row.state.Value();
    int const currency = row.currency.Value();
    if (state < 0 || state > kMaxEntryState || currency < 0 || currency > kMaxCurrencyCode) {
        throw ValidationError{"entry " + std::to_string(row.id.Value()) + " stores a state or currency out of range"};
    }
    CompanyId const company{row.companyId.Value()};
    auto const transaction = static_cast<CurrencyCode>(currency);
    CurrencyCode const base = companyCurrency(company);
    JournalEntryView view{
        .id = JournalEntryId{static_cast<std::int64_t>(row.id.Value())},
        .company = company,
        .period = PeriodId{row.period.Value()},
        .date = timestampFromMillis(row.dateMs.Value()),
        .state = static_cast<EntryState>(state),
        .sourceKey = std::string{row.sourceKey.Value().ToStringView()},
        .sourceReference = std::string{row.sourceReference.Value().ToStringView()},
        .memo = std::string{row.memo.Value().ToStringView()},
        .currency = transaction,
        .rate = Rate{Rational{Numerator{row.rateNum.Value()}, Denominator{row.rateDen.Value()}, DecimalPlaces{6}}},
        .reverses = JournalEntryId::fromOptional(row.reversesId.Value()),
        .reversedBy = JournalEntryId::fromOptional(row.reversedById.Value()),
        .lines = {},
        .createdBy = std::string{row.createdBy.Value().ToStringView()},
        .postedBy = std::string{row.postedBy.Value().ToStringView()},
    };
    for (auto const& line : entry.lines) {
        int const side = line.side.Value();
        if (side != static_cast<int>(PostingSide::Debit) && side != static_cast<int>(PostingSide::Credit)) {
            throw ValidationError{"entry " + std::to_string(row.id.Value()) + " stores a line side out of range"};
        }
        auto const accounts = mapper.Query<db::AccountRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", line.account.Value())
                                  .All();
        if (accounts.empty()) {
            throw ValidationError{"entry " + std::to_string(row.id.Value()) + " has a line on account " +
                                  std::to_string(line.account.Value()) + ", which does not exist"};
        }
        view.lines.push_back(PostingLine{
            .accountCode = std::string{accounts.front().code.Value().ToStringView()},
            .side = static_cast<PostingSide>(side),
            .amount = amountOf(line.amountNum.Value(), line.amountDen.Value(), transaction),
            .companyAmount = amountOf(line.companyNum.Value(), line.companyDen.Value(), base),
        });
    }
    return view;
}

/// @brief Changes the entry row @p entryId only where it still holds the state the caller read.
///
/// One conditional UPDATE, so two callers acting on the same read cannot both succeed: the second finds the row
/// changed and matches nothing.
/// @param mapper The open mapper, inside the caller's transaction.
/// @param entryId The entry.
/// @param set The columns to set and their new values, as SQL literals built by the query formatter.
/// @param guard Adds the condition the row must still meet.
/// @return Whether a row matched and changed.
template <typename Set, typename Guard>
[[nodiscard]] bool updateEntryIf(Lightweight::DataMapper& mapper, std::int64_t entryId, Set const& set,
                                 Guard const& guard) {
    auto query = mapper.Connection().Query(db::EntryRecord::TableName).Update();
    set(query);
    (void)query.Where("id", "=", entryId);
    guard(query);
    Lightweight::SqlStatement statement{mapper.Connection()};
    auto const cursor = statement.ExecuteDirect(query.ToSql());
    return cursor.NumRowsAffected() == 1U;
}

/// @brief The period of @p company that @p date falls in.
/// @param mapper The open mapper.
/// @param company The company.
/// @param date The date.
/// @return Its row.
/// @throws NotFound if the month is not a period of the company.
[[nodiscard]] db::PeriodRecord periodOf(Lightweight::DataMapper& mapper, CompanyId company,
                                        ::morph::time::Timestamp const& date) {
    auto const month = calendarMonthOf(date);
    auto rows = mapper.Query<db::PeriodRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::startMs>, "=", monthStartMillis(month))
                    .All();
    if (rows.empty()) {
        throw NotFound{"company " + std::to_string(*company) + " has no period " + monthLabel(month)};
    }
    return rows.front();
}

/// @brief Throws `Conflict` when @p period does not take what the caller is recording.
/// @param period The period.
/// @param allowsClosing Whether a closing period takes it.
/// @param what What is refused, for the message.
void requireRecordable(db::PeriodRecord const& period, bool allowsClosing, std::string_view what) {
    int const state = period.state.Value();
    std::string const label = monthLabel(calendarMonthOf(timestampFromMillis(period.startMs.Value())));
    if (state == static_cast<int>(PeriodState::Closed)) {
        throw Conflict{std::string{what} + ": period " + label + " is closed"};
    }
    if (state == static_cast<int>(PeriodState::Closing) && !allowsClosing) {
        throw Conflict{std::string{what} + ": period " + label + " is closing and takes no new drafts"};
    }
}

/// @brief The account id of each line's code in @p company's chart.
/// @param mapper The open mapper.
/// @param company The company.
/// @param lines The lines.
/// @return The ids, in line order.
/// @throws NotFound if a code is not in the chart.
[[nodiscard]] std::vector<std::uint64_t> accountIds(Lightweight::DataMapper& mapper, CompanyId company,
                                                    std::span<PostingLine const> lines) {
    std::vector<std::uint64_t> ids;
    for (PostingLine const& line : lines) {
        auto const rows = mapper.Query<db::AccountRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *company)
                              .Where(::Lightweight::FieldNameOf<&db::AccountRecord::code>, "=", line.accountCode)
                              .All();
        if (rows.empty()) {
            throw NotFound{"company " + std::to_string(*company) + " has no account " + line.accountCode};
        }
        ids.push_back(rows.front().id.Value());
    }
    return ids;
}

/// @brief Writes an entry and its lines; the caller commits.
/// @param mapper The open mapper, inside the caller's transaction.
/// @param header The header, its id unset.
/// @param accounts The lines' account ids.
/// @param lines The lines.
/// @return The new entry's id.
std::int64_t writeEntry(Lightweight::DataMapper& mapper, db::EntryRecord& header,
                        std::vector<std::uint64_t> const& accounts, std::span<PostingLine const> lines) {
    mapper.Create(header);
    int position = 0;
    for (auto const& [account, line] : std::views::zip(accounts, lines)) {
        db::EntryLineRecord row;
        row.entry = static_cast<std::int64_t>(header.id.Value());
        row.position = position++;
        row.account = static_cast<std::int64_t>(account);
        row.side = static_cast<int>(line.side);
        row.amountNum = (*line.amount).numerator;
        row.amountDen = (*line.amount).denominator;
        row.companyNum = (*line.companyAmount).numerator;
        row.companyDen = (*line.companyAmount).denominator;
        mapper.Create(row);
    }
    return static_cast<std::int64_t>(header.id.Value());
}

/// @brief A header for @p company on @p date in @p period.
/// @return The header, its id unset.
[[nodiscard]] db::EntryRecord headerFor(CompanyId company, ::morph::time::Timestamp const& date,
                                        db::PeriodRecord const& period, EntryState state, CurrencyCode currency,
                                        Rational const& rate, std::string const& author) {
    db::EntryRecord header;
    header.companyId = *company;
    header.period = static_cast<std::int64_t>(period.id.Value());
    header.dateMs = millisOf(date);
    header.state = static_cast<int>(state);
    header.currency = static_cast<int>(currency);
    header.rateNum = rate.numerator;
    header.rateDen = rate.denominator;
    header.createdBy = Lightweight::SqlAnsiString<64>{author};
    header.postedBy = Lightweight::SqlAnsiString<64>{state == EntryState::Posted ? author : std::string{}};
    return header;
}

/// @brief Throws `ValidationError` naming @p where when @p lines cannot be recorded.
/// @param where The action, for the message.
/// @param lines The lines.
/// @param transaction The transaction currency.
/// @param company The company's currency.
/// @param rate Company-currency units per transaction-currency unit.
void requireBalanced(std::string_view where, std::span<PostingLine const> lines, CurrencyCode transaction,
                     CurrencyCode company, Rate const& rate) {
    if (auto const problem = entryProblem(lines, transaction, company, rate)) {
        throw ValidationError{std::string{where} + ": " + *problem};
    }
}

/// @brief Throws `ValidationError` unless @p rate is a usable conversion from @p transaction to @p company.
/// @param where The action, for the message.
/// @param rate The rate: company-currency units per transaction-currency unit.
/// @param transaction The transaction currency.
/// @param company The company's currency.
void requireRate(std::string_view where, Rational const& rate, CurrencyCode transaction, CurrencyCode company) {
    if (rate.isZero() || rate.isNegative()) {
        throw ValidationError{std::string{where} + ": the rate must be positive"};
    }
    if (transaction == company && rate != Rational{1, DecimalPlaces{6}}) {
        throw ValidationError{std::string{where} + ": an entry in the company's own currency has the rate one"};
    }
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
JournalEntryView LedgerModel::execute(const CreateDraftEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateDraftEntry: a company, a date, a rate and two or more lines are required"};
    }
    requireRoleSomewhere<CreateDraftEntry>();
    CurrencyCode const base = companyCurrency(action.company);
    requireAuthorized(action);
    requireRate("CreateDraftEntry", *action.rate, action.currency, base);
    requireBalanced("CreateDraftEntry", action.lines, action.currency, base, action.rate);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const period = periodOf(mapper, action.company, action.date);
    requireRecordable(period, false, "CreateDraftEntry");
    auto const accounts = accountIds(mapper, action.company, action.lines);
    auto header = headerFor(action.company, action.date, period, EntryState::Draft, action.currency, *action.rate,
                            ::morph::session::current()->principal);
    header.memo = Lightweight::SqlAnsiString<255>{action.memo};
    std::int64_t const created = writeEntry(mapper, header, accounts, action.lines);
    sqlTxn.Commit();
    return toView(mapper, loadWritten(mapper, created));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
JournalEntryView LedgerModel::execute(const PostEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"PostEntry: an entry is required"};
    }
    requireRoleSomewhere<PostEntry>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto entry = loadEntry(mapper, *action.entryId);
    if (!entry.has_value()) {
        throw NotFound{"PostEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    requireAuthorized(action);
    if (entry->header.state.Value() != static_cast<int>(EntryState::Draft)) {
        throw IllegalTransition{"PostEntry: entry " + std::to_string(*action.entryId) + " is already posted"};
    }
    auto const periods =
        mapper.Query<db::PeriodRecord>()
            .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", entry->header.period.Value())
            .All();
    if (periods.empty()) {
        throw NotFound{"PostEntry: the period of entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    requireRecordable(periods.front(), true, "PostEntry");
    entry->header.state = static_cast<int>(EntryState::Posted);
    entry->header.postedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    // Only a row still in Draft is posted: a concurrent post that won leaves nothing here to change.
    if (!updateEntryIf(
            mapper, *action.entryId,
            [&](auto& query) {
                query.Set("state", static_cast<int>(EntryState::Posted))
                    .Set("posted_by", ::morph::session::current()->principal);
            },
            [](auto& query) { (void)query.Where("state", "=", static_cast<int>(EntryState::Draft)); })) {
        throw Conflict{"PostEntry: entry " + std::to_string(*action.entryId) + " was posted meanwhile"};
    }
    sqlTxn.Commit();
    return toView(mapper, *entry);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
JournalEntryView LedgerModel::execute(const ReverseEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ReverseEntry: an entry and a date are required"};
    }
    requireRoleSomewhere<ReverseEntry>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto original = loadEntry(mapper, *action.entryId);
    if (!original.has_value()) {
        throw NotFound{"ReverseEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    requireAuthorized(action);
    if (original->header.state.Value() != static_cast<int>(EntryState::Posted)) {
        throw IllegalTransition{"ReverseEntry: only a posted entry is reversed; a draft is not on the books"};
    }
    if (original->header.reversedById.Value().has_value()) {
        throw Conflict{"ReverseEntry: entry " + std::to_string(*action.entryId) + " is already reversed"};
    }
    auto const view = toView(mapper, *original);
    std::vector<PostingLine> lines = view.lines;
    for (PostingLine& line : lines) {
        line.side = line.side == PostingSide::Debit ? PostingSide::Credit : PostingSide::Debit;
    }
    // The mirror is checked like any entry: a mirroring defect is a refusal, not bad books.
    requireBalanced("ReverseEntry", lines, view.currency, companyCurrency(view.company), view.rate);
    auto const period = periodOf(mapper, view.company, action.date);
    requireRecordable(period, true, "ReverseEntry");
    auto const accounts = accountIds(mapper, view.company, lines);
    auto header = headerFor(view.company, action.date, period, EntryState::Posted, view.currency, *view.rate,
                            ::morph::session::current()->principal);
    header.sourceReference = Lightweight::SqlAnsiString<128>{"reversal of entry " + std::to_string(*action.entryId)};
    header.reversesId = *action.entryId;
    std::int64_t const created = writeEntry(mapper, header, accounts, lines);
    // Only an entry still unreversed is marked: a concurrent reversal that won leaves nothing here to change, and
    // this transaction's reversal rolls back with it.
    if (!updateEntryIf(
            mapper, *action.entryId, [&](auto& query) { query.Set("reversed_by_id", created); },
            [](auto& query) { (void)query.WhereNull("reversed_by_id"); })) {
        throw Conflict{"ReverseEntry: entry " + std::to_string(*action.entryId) + " was reversed meanwhile"};
    }
    sqlTxn.Commit();
    return toView(mapper, loadWritten(mapper, created));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
JournalEntryView LedgerModel::execute(const GetEntry& action) {
    if (!action.validate()) {
        throw ValidationError{"GetEntry: an entry is required"};
    }
    Lightweight::DataMapper mapper;
    auto const entry = loadEntry(mapper, *action.entryId);
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (!entry.has_value() ||
        std::ranges::find(visibleCompanies, CompanyId{entry->header.companyId.Value()}) == visibleCompanies.end()) {
        throw NotFound{"GetEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    return toView(mapper, *entry);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListEntriesResult LedgerModel::execute(const ListEntries& action) {
    if (!action.validate()) {
        throw ValidationError{"ListEntries: a company is required"};
    }
    ListEntriesResult result;
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visibleCompanies, action.company) == visibleCompanies.end()) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& header : mapper.Query<db::EntryRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::EntryRecord::companyId>, "=", *action.company)
                                  .All()) {
        result.entries.push_back(toView(mapper, loadWritten(mapper, static_cast<std::int64_t>(header.id.Value()))));
    }
    std::ranges::sort(result.entries, {}, [](JournalEntryView const& entry) { return *entry.id; });
    return result;
}

}  // namespace lims
