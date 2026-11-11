// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <morph/offline/replay_ledger.hpp>
#include <morph/session/session.hpp>
#include <morph/table/query.hpp>
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
#include "lims/ledger/db/ledger_store.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/plan_sql.hpp"
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

/// @brief The journal's columns as the table engine reflects them from `JournalEntryView`.
/// @return One column per member, in member order.
[[nodiscard]] std::vector<::morph::table::ColumnInfo> const& journalColumns() {
    static auto const columns = [] {
        ::morph::table::RowsSource<JournalEntryView> const source{
            std::make_shared<std::vector<JournalEntryView> const>(),
            [](JournalEntryView const& entry) { return ::morph::table::RowId{*entry.id}; }};
        auto const reflected = source.columns();
        return std::vector<::morph::table::ColumnInfo>(reflected.begin(), reflected.end());
    }();
    return columns;
}

/// @brief How the database reads each journal column it can filter and order by.
///
/// `lines` is a serialisation of another table and `rate` is a quantity held as a numerator and denominator, whose
/// exact comparison would multiply two `int64` values in SQLite, which turns an overflowing product into a real; both
/// are refused. The enum columns are mapped to the enumerator's name, which is how the table engine holds them.
/// @return The columns by id.
[[nodiscard]] plansql::Columns const& journalSqlColumns() {
    static plansql::Columns const columns{
        {"id", plansql::integer("id")},
        {"company", plansql::integer("company_id")},
        {"period", plansql::integer("period_id")},
        {"date", plansql::dateTime(plansql::wholeSeconds("date_ms"))},
        {"state", plansql::text(plansql::enumName<EntryState>("state", kMaxEntryState + 1))},
        {"sourceKey", plansql::storedText("source_key")},
        {"sourceReference", plansql::storedText("source_reference")},
        {"memo", plansql::storedText("memo")},
        {"currency", plansql::text(plansql::enumName<CurrencyCode>("currency", kMaxCurrencyCode + 1))},
        {"reverses", plansql::integer("reverses_id")},
        {"reversedBy", plansql::integer("reversed_by_id")},
        {"createdBy", plansql::storedText("created_by")},
        {"postedBy", plansql::storedText("posted_by")},
    };
    return columns;
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

/// @brief The posting sources of one company, as an `IReplayLedger` over `ledger_posting_sources`.
///
/// Constructed over the caller's open mapper for one `record()`, so a source key is recorded in the same transaction
/// as the entry it produced. The payload is the entry's id, which a repeated request answers.
class PostingSources final : public ::morph::offline::IReplayLedger {
public:
    /// @param mapper The caller's mapper. Borrowed: it must outlive this instance.
    explicit PostingSources(Lightweight::DataMapper& mapper) : _mapper{mapper} {}

protected:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        auto const rows =
            _mapper.Query<db::PostingSourceRecord>()
                .Where(::Lightweight::FieldNameOf<&db::PostingSourceRecord::scope>, "=", std::string{scope})
                .Where(::Lightweight::FieldNameOf<&db::PostingSourceRecord::sourceKey>, "=", std::string{opId})
                .All();
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::string{rows.front().entry.Value().ToStringView()};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void doRecord(std::string_view scope, std::string_view opId, std::string payload) override {
        db::PostingSourceRecord row;
        row.scope = Lightweight::SqlAnsiString<32>{std::string{scope}};
        row.sourceKey = Lightweight::SqlAnsiString<128>{std::string{opId}};
        row.entry = Lightweight::SqlAnsiString<32>{payload};
        _mapper.Create(row);
    }

private:
    // Borrowed for one record(); never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    Lightweight::DataMapper& _mapper;
};

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
JournalEntryView LedgerModel::execute(const CreateDraftEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateDraftEntry: a company, a date, a rate and two or more lines are required"};
    }
    CurrencyCode const base = companyCurrency(action.company);
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
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto entry = loadEntry(mapper, *action.entryId);
    if (!entry.has_value()) {
        throw NotFound{"PostEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    // Judged again inside the transaction: the scope is read from the row the write commits. The action's guard ran it
    // first, on every path.
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
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto original = loadEntry(mapper, *action.entryId);
    if (!original.has_value()) {
        throw NotFound{"ReverseEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    // Judged again inside the transaction: the scope is read from the row the write commits. The action's guard ran it
    // first, on every path.
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

bool LedgerModel::canTranslateEntryColumn(std::string_view column, ::morph::table::PlanUse /*use*/) {
    return plansql::translatable(journalSqlColumns(), column);
}

::morph::table::Page<JournalEntryView> LedgerModel::pageEntries(CompanyId company,
                                                                ::morph::table::QueryPlan const& plan) {
    auto const translation = plansql::translate(plan, journalSqlColumns(), "id");
    if (!translation) {
        throw ValidationError{"ListEntriesPage: " + translation.error().message};
    }
    Lightweight::DataMapper mapper;
    // One read transaction for the count and the page, so the total agrees with the rows. It is rolled back, never
    // committed, and no write happens inside it.
    Lightweight::SqlTransaction const reading{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const fetched = plansql::fetch(mapper, db::EntryRecord::TableName, "id",
                                        "company_id = " + std::to_string(*company), *translation, plan);
    ::morph::table::Page<JournalEntryView> page;
    page.offset = plan.offset;
    page.total = fetched.total;
    page.totalKind = ::morph::table::TotalKind::Exact;
    for (std::int64_t const id : fetched.ids) {
        page.rows.push_back(toView(mapper, loadWritten(mapper, id)));
    }
    return page;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::morph::table::Page<JournalEntryView> LedgerModel::execute(const ListEntriesPage& action) {
    if (!action.validate()) {
        throw ValidationError{"ListEntriesPage: a company is required"};
    }
    auto const plan = ::morph::table::planQuery(action.query, journalColumns(), {}, {}, {}, canTranslateEntryColumn);
    if (!plan) {
        throw ValidationError{"ListEntriesPage: " + plan.error().message};
    }
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visibleCompanies, action.company) == visibleCompanies.end()) {
        ::morph::table::Page<JournalEntryView> empty;
        empty.offset = plan->offset;
        empty.total = 0;
        return empty;
    }
    return pageEntries(action.company, *plan);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PeriodView LedgerModel::execute(const ClosePeriod& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ClosePeriod: a period is required"};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto periods = mapper.Query<db::PeriodRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", *action.periodId)
                       .All();
    if (periods.empty()) {
        throw NotFound{"ClosePeriod: period " + std::to_string(*action.periodId) + " does not exist"};
    }
    // Judged again inside the transaction: the scope is read from the row the write commits. The action's guard ran it
    // first, on every path.
    requireAuthorized(action);
    auto& period = periods.front();
    if (period.state.Value() != static_cast<int>(PeriodState::Closing)) {
        throw IllegalTransition{"ClosePeriod: a period is closed from closing, after its close has begun"};
    }
    auto const entries = mapper.Query<db::EntryRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::EntryRecord::period>, "=", *action.periodId)
                             .All();
    auto const drafts = std::ranges::count_if(entries, [](db::EntryRecord const& entry) {
        return entry.state.Value() == static_cast<int>(EntryState::Draft);
    });
    if (drafts > 0) {
        throw Conflict{"ClosePeriod: " + std::to_string(drafts) + " draft entries remain in the period"};
    }
    // Each account's totals in the period, in company currency, kept with the closed period.
    auto const places = DecimalPlaces{decimalsOf(companyCurrency(CompanyId{period.companyId.Value()}))};
    std::map<std::int64_t, std::pair<ExactSum, ExactSum>> totals;
    for (auto const& entry : entries) {
        for (auto const& line : mapper.Query<db::EntryLineRecord>()
                                    .Where(::Lightweight::FieldNameOf<&db::EntryLineRecord::entry>, "=",
                                           static_cast<std::int64_t>(entry.id.Value()))
                                    .All()) {
            auto& [debit, credit] = totals[line.account.Value()];
            Rational const value{Numerator{line.companyNum.Value()}, Denominator{line.companyDen.Value()}, places};
            if (!(line.side.Value() == static_cast<int>(PostingSide::Debit) ? debit : credit).add(value)) {
                throw OutOfRange{"ClosePeriod: an account's total is too large to freeze exactly"};
            }
        }
    }
    for (auto const& [account, sums] : totals) {
        db::PeriodBalanceRecord row;
        row.period = static_cast<std::int64_t>(period.id.Value());
        row.account = account;
        row.debitNum = sums.first.total().numerator;
        row.debitDen = sums.first.total().denominator;
        row.creditNum = sums.second.total().numerator;
        row.creditDen = sums.second.total().denominator;
        mapper.Create(row);
    }
    period.state = static_cast<int>(PeriodState::Closed);
    mapper.Update(period);
    sqlTxn.Commit();
    auto const start = timestampFromMillis(period.startMs.Value());
    return PeriodView{
        .id = PeriodId{static_cast<std::int64_t>(period.id.Value())},
        .company = CompanyId{period.companyId.Value()},
        .start = start,
        .label = monthLabel(calendarMonthOf(start)),
        .state = PeriodState::Closed,
    };
}

namespace {

/// @brief Whether entry @p entry, as recorded, is what @p request asks for: everything that reaches the books.
///
/// Amounts and the rate compare by value, so a figure written with other decimals is the same figure; the
/// entry's author and generated memo are not part of the request.
/// @param mapper The open mapper.
/// @param entry The recorded entry.
/// @param request The request.
/// @return Whether they agree.
/// @throws NotFound if the entry is gone.
[[nodiscard]] bool recordedAs(Lightweight::DataMapper& mapper, std::int64_t entry, PostingRequest const& request) {
    auto const stored = toView(mapper, loadWritten(mapper, entry));
    if (millisOf(stored.date) != millisOf(request.date) || stored.sourceReference != request.sourceReference ||
        stored.currency != request.currency || *stored.rate != *request.rate ||
        stored.lines.size() != request.lines.size()) {
        return false;
    }
    return std::ranges::all_of(std::views::zip(stored.lines, request.lines), [](auto const& pair) {
        auto const& [recorded, asked] = pair;
        return recorded.accountCode == asked.accountCode && recorded.side == asked.side &&
               *recorded.amount == *asked.amount && *recorded.companyAmount == *asked.companyAmount;
    });
}

/// @brief Answers a request whose source key is already recorded.
/// @param mapper The open mapper.
/// @param sources The posting sources.
/// @param scope The company's scope.
/// @param request The request.
/// @return The first entry when the request is the one recorded; nothing when the key is new.
/// @throws Conflict if the key is recorded for a different request.
[[nodiscard]] std::optional<PostingResult> answerKnown(Lightweight::DataMapper& mapper, PostingSources const& sources,
                                                       std::string const& scope, PostingRequest const& request) {
    auto const known = sources.lookup(scope, request.sourceKey);
    if (!known.has_value()) {
        return std::nullopt;
    }
    auto const entry = std::stoll(*known);
    if (!recordedAs(mapper, entry, request)) {
        throw Conflict{"record: source key " + request.sourceKey +
                       " is already recorded for a different posting; a corrected document needs a new key"};
    }
    auto const first = loadEntry(mapper, entry);
    if (!first.has_value()) {
        throw NotFound{"record: entry " + std::to_string(entry) + " of source key " + request.sourceKey + " is gone"};
    }
    return PostingResult{.entryId = JournalEntryId{entry}, .date = timestampFromMillis(first->header.dateMs.Value())};
}

}  // namespace

PostingResult recordPostingIn(Lightweight::DataMapper& mapper, PostingRequest const& request) {
    if (!request.company.hasValue() || !request.date.hasValue() || request.sourceKey.empty() ||
        request.sourceKey.size() > 128 || request.sourceReference.size() > 128 || !request.rate.hasValue()) {
        throw ValidationError{"record: a company, a date, a source key and a rate are required"};
    }
    CurrencyCode const base = companyCurrency(request.company);
    requireRate("record", *request.rate, request.currency, base);
    requireBalanced("record", request.lines, request.currency, base, request.rate);
    // The transaction opens with a write, so it holds the database's write lock before it reads anything: a
    // transaction that read first and wrote later is turned away at once with a raw "database is locked" when a
    // concurrent one already holds the write lock, instead of waiting for it. The statement changes no row.
    {
        auto lock = mapper.Connection().Query(db::PostingSourceRecord::TableName).Update();
        lock.Set("entry", std::string{});
        (void)lock.Where("id", "=", 0);
        Lightweight::SqlStatement statement{mapper.Connection()};
        static_cast<void>(statement.ExecuteDirect(lock.ToSql()));
    }
    PostingSources sources{mapper};
    std::string const scope = std::to_string(*request.company);
    // The key is looked up before anything that can change between calls (the dated rate, the period's state), so
    // an identical repeat answers the first entry after either has changed.
    if (auto const answer = answerKnown(mapper, sources, scope, request)) {
        return *answer;
    }
    if (request.currency != base) {
        auto const dated = LedgerModel::rateOn(request.company, request.currency, base, request.date);
        if (!dated.has_value() || *dated != *request.rate) {
            throw ValidationError{"record: the rate is not the ledger's " +
                                  std::string{currencyName(request.currency)} + "/" + std::string{currencyName(base)} +
                                  " rate on the posting's date"};
        }
    }
    auto const period = periodOf(mapper, request.company, request.date);
    requireRecordable(period, true, "record");
    auto const accounts = accountIds(mapper, request.company, request.lines);
    auto header = headerFor(request.company, request.date, period, EntryState::Posted, request.currency, *request.rate,
                            "posting");
    header.sourceKey = Lightweight::SqlAnsiString<128>{request.sourceKey};
    header.sourceReference = Lightweight::SqlAnsiString<128>{request.sourceReference};
    std::int64_t const entryId = writeEntry(mapper, header, accounts, request.lines);
    sources.record(scope, request.sourceKey, std::to_string(entryId));
    return PostingResult{.entryId = JournalEntryId{entryId}, .date = request.date};
}

PostingResult LedgerModel::record(PostingRequest const& request) {
    auto const attempt = [&request] {
        Lightweight::DataMapper attemptMapper;
        Lightweight::SqlTransaction sqlTxn{attemptMapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        auto const result = recordPostingIn(attemptMapper, request);
        sqlTxn.Commit();
        return result;
    };
    try {
        return attempt();
    } catch (Lightweight::SqlException const&) {
        // A concurrent first call with this key committed between this call's lookup and its write (the unique
        // key refuses the second row), and this call's transaction is rolled back. The second try finds the
        // winner's row and answers it, or reports the key's different body; a failure that is not that race fails
        // again and surfaces.
        try {
            return attempt();
        } catch (Lightweight::SqlException const& again) {
            if (!isBusy(again)) {
                throw;
            }
            throw Conflict{"record: the books are busy with another posting; try again"};
        }
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PostingResult LedgerModel::execute(const FindPosting& action) {
    if (!action.validate()) {
        throw ValidationError{"FindPosting: a company and a source key are required"};
    }
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visibleCompanies, action.query.company) == visibleCompanies.end()) {
        return PostingResult{};
    }
    Lightweight::DataMapper mapper;
    PostingSources const sources{mapper};
    auto const known = sources.lookup(std::to_string(*action.query.company), action.query.sourceKey);
    if (!known.has_value()) {
        return PostingResult{};
    }
    auto const entry = loadEntry(mapper, std::stoll(*known));
    if (!entry.has_value()) {
        return PostingResult{};
    }
    return PostingResult{
        .entryId = JournalEntryId{std::stoll(*known)},
        .date = timestampFromMillis(entry->header.dateMs.Value()),
    };
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PostingResult LedgerModel::execute(const RecordPosting& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RecordPosting: a company and a source key are required"};
    }
    requireServicePrincipal("RecordPosting");
    return record(action.request);
}

}  // namespace lims
