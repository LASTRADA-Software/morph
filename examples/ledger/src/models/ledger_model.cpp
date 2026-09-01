// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/ledger_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clock.hpp"
#include "ledger/core/errors.hpp"
#include "ledger/core/money.hpp"
#include "ledger/core/time_util.hpp"
#include "ledger/core/units.hpp"
#include "ledger/db/ledger_entity.hpp"

namespace ledger {

static_assert(decltype(db::LedgerRecord::name)::ValueType{}.capacity() == kMaxLedgerNameBytes,
              "ledger::kMaxLedgerNameBytes must equal LedgerRecord::name's SqlAnsiString capacity -- otherwise "
              "CreateLedger either rejects a name that would have fit, or accepts one that gets silently truncated "
              "on the way into the row (Light::SqlFixedString's constructor is noexcept and truncates rather than "
              "throwing), so the caller is told 'ok' about a book stored under a name they never sent. Same guard, "
              "same reason, as kanban's own kMaxProjectNameBytes assertion in src/models/board_model.cpp.");

namespace {

/// @brief Sums every leg posted against @p accountId into a single
///        `Rational`, tagged at @p decimalPlaces. In-model summation via
///        `Rational::operator+`, never a raw SQL `SUM()` -- design spec
///        §3's rule against combining differently-denominated rows in SQL
///        applies here exactly as it does for budgets: each
///        `TransactionLegRecord` row can carry its own `amount_den`
///        (design spec §1), so only `Rational`'s own reduction logic can
///        combine them correctly.
/// @param mapper The data mapper to query legs through.
/// @param accountId The account whose legs to sum.
/// @param decimalPlaces The account's own currency precision, used to seed
///        the running total's zero starting value.
/// @param journalFilter When non-null, the set of journal ids inside the
///        report's period: legs belonging to any other journal are skipped.
///        Null sums every leg, which is what an all-time balance wants.
/// @return The account's balance over the legs the filter admits.
[[nodiscard]] morph::math::Rational sumAccountLegs(Lightweight::DataMapper& mapper, std::uint64_t accountId,
                                                   morph::math::DecimalPlaces decimalPlaces,
                                                   const std::unordered_set<std::uint64_t>* journalFilter = nullptr) {
    auto legRows = mapper.Query<db::TransactionLegRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::TransactionLegRecord::account>, "=", accountId)
                       .All();
    auto total = morph::math::Rational::zero(decimalPlaces);
    for (const auto& legRow : legRows) {
        // A period-scoped report counts only legs whose journal falls inside
        // the period; @p journalFilter is null for an all-time report, which
        // is every caller but the monthly statement.
        if (journalFilter != nullptr && !journalFilter->contains(legRow.journal.Value())) {
            continue;
        }
        const auto legAmount = morph::math::Rational{
            morph::math::Numerator{legRow.amountNum.Value()}, morph::math::Denominator{legRow.amountDen.Value()},
            morph::math::DecimalPlaces{static_cast<std::uint32_t>(legRow.amountDp.Value())}};
        total = total + legAmount;
    }
    return total;
}

/// @brief Builds the full current `GetLedgerResult` for @p ledgerId using
///        @p mapper directly -- the same pattern as kanban's own free
///        `buildState(mapper, project)` helper
///        (`ladder-kanban-impl:examples/kanban/src/models/board_model.cpp`).
///        Declared so `execute(StoreTransaction)` can rebuild the ledger's
///        state through the *same* mapper/transaction its own mutation
///        just ran on -- needed for Task 11b's applied-ops ledger write,
///        which must serialize this exact result and commit it atomically
///        with the journal+legs insert, not through a second, separate
///        `Lightweight::DataMapper` connection as a follow-up call would.
/// @param mapper The data mapper to query through.
/// @param ledgerId The ledger whose accounts/balances to rebuild.
/// @return Every account in the ledger, per the ladder-wide
///         full-rebuilt-state convention.
[[nodiscard]] GetLedgerResult buildLedgerState(Lightweight::DataMapper& mapper, LedgerId ledgerId) {
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::ledger>, "=", *ledgerId)
                    .All();

    GetLedgerResult result;
    result.accounts.reserve(rows.size());
    std::vector<std::uint64_t> accountIds;
    accountIds.reserve(rows.size());
    std::unordered_map<std::uint64_t, std::size_t> slotOf;
    slotOf.reserve(rows.size());
    for (const auto& row : rows) {
        const auto currency = codeToCurrency(row.currencyCode.Value().ToStringView());
        const auto decimalPlaces = morph::math::DecimalPlaces{UnitTraits<Currency>::meta(currency).defaultDecimals};
        slotOf.emplace(row.id.Value(), result.accounts.size());
        accountIds.push_back(row.id.Value());
        result.accounts.push_back(AccountInfo{
            .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
            .name = std::string{row.name.Value().ToStringView()},
            .kind = static_cast<AccountKind>(row.kind.Value()),
            .currency = currency,
            .balance = morph::math::Rational::zero(decimalPlaces),
        });
    }

    // Every leg of every account in one query, rather than `sumAccountLegs`
    // once per account.
    //
    // The point is atomicity, not the saved round trips. A per-account query
    // runs in its own implicit transaction, so a two-leg journal committed by
    // another client between the Checking query and the Groceries query is
    // seen on one account and not the other -- and the ledger's headline
    // invariant, that every currency sums to zero, appears violated by
    // exactly one leg. That is a torn read, not a broken invariant: the write
    // side is already atomic (`storeJournalImpl` holds a `SqlTransaction`),
    // and it is the read that was not. A single SELECT is one statement, so
    // it lands wholly before or wholly after any concurrent commit.
    //
    // Client-visible, not just a test artefact: a GUI polling `GetLedger`
    // while another client posts would render a double-entry ledger that does
    // not balance.
    if (!accountIds.empty()) {
        auto legRows = mapper.Query<db::TransactionLegRecord>()
                           .WhereIn(::Lightweight::FieldNameOf<&db::TransactionLegRecord::account>, accountIds)
                           .All();
        for (const auto& legRow : legRows) {
            const auto slot = slotOf.find(legRow.account.Value());
            if (slot == slotOf.end()) {
                continue;
            }
            auto& balance = result.accounts[slot->second].balance;
            balance = balance + morph::math::Rational{
                                    morph::math::Numerator{legRow.amountNum.Value()},
                                    morph::math::Denominator{legRow.amountDen.Value()},
                                    morph::math::DecimalPlaces{static_cast<std::uint32_t>(legRow.amountDp.Value())}};
        }
    }
    return result;
}

/// @brief Restates every leg's amount onto its own account currency's scale,
///        so the zero-sum partitioning that follows compares like with like.
///
///        A leg amount is a whole number of minor units at whatever
///        `decimalPlaces` the client chose (`ledger/core/money.hpp` documents
///        the encoding and why it is not `Rational`'s own reading of the same
///        triple). Nothing on the wire constrains that scale to the account's,
///        and `Rational::operator+` cannot notice the difference: it adds
///        numerators and propagates `std::max` of the two precisions. So
///        `{450, dp 2}` ($4.50) and `{-450, dp 1}` (-$45.00) sum to a
///        numerator of zero and pass a check they should fail, while
///        `{45, dp 1}` ($4.50) and `{-450, dp 2}` (-$4.50) sum to -405 and
///        fail one they should pass. Restating first removes both.
///
///        It also keeps every stored leg of an account on that account's own
///        scale, which the read side depends on: `buildLedgerState` seeds each
///        balance at the currency's precision and accumulates with the same
///        `std::max` propagation, so a single leg written at a wider scale
///        would pull that account's rendered balance off by a power of ten
///        for as long as the row exists.
/// @param legs The legs to restate, positionally aligned with @p legAccounts.
/// @param legAccounts Each leg's own account row, in the same order.
/// @param actionName The action name to prefix a rejection message with.
/// @return One restated amount per leg, in leg order.
/// @throws ValidationError When a leg's amount is not a whole number of its
///         account currency's minor units -- either more precision than the
///         currency has (`$4.505` in a USD account) or a non-integral
///         minor-unit count off the wire (`{"num":9,"den":2}`). Rejected, not
///         rounded: the model never rounds money (design spec §1).
[[nodiscard]] std::vector<morph::math::Rational> restateLegAmounts(const std::vector<TransactionLeg>& legs,
                                                                   const std::vector<db::AccountRecord>& legAccounts,
                                                                   std::string_view actionName) {
    std::vector<morph::math::Rational> restated;
    restated.reserve(legs.size());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        const std::string code{legAccounts[i].currencyCode.Value().ToStringView()};
        auto amount = restateMinorUnits(legs[i].amount, currencyDecimalPlaces(codeToCurrency(code)));
        if (!amount.has_value()) {
            throw ValidationError{std::string{actionName} + ": leg amount is not a whole number of " + code +
                                  " minor units"};
        }
        restated.push_back(*amount);
    }
    return restated;
}

/// @brief Partitions @p legAmounts by the currency of the matching entry in
///        @p legAccounts and requires every partition to sum to canonical
///        zero, throwing `ZeroSumViolation` on the first one that does not.
///
///        The single per-currency zero-sum check every journal-writing path
///        runs: `execute(StoreTransaction)` and `storeJournalImpl` (and, in
///        turn, everything that reaches the database through
///        `storeJournalImpl` -- `execute(UndoTransaction)` and
///        `execute(ImportLedgerChunk)`) all call this same function rather
///        than each re-deriving the partitioning loop. Grouping by account
///        currency, never a client-supplied field, is what makes a two-leg
///        entry across a USD account and a EUR account rejected as two
///        unbalanced single-legged postings instead of accepted as if
///        balanced (design spec §1).
/// @param legAmounts Each leg's amount, already restated onto its own
///        account currency's scale (`restateLegAmounts`) -- summing
///        un-restated amounts across differently-scaled legs of the same
///        currency is exactly the bug `restateLegAmounts` exists to prevent
///        (see that function's own doc comment).
/// @param legAccounts Each leg's own account row, positionally aligned with
///        @p legAmounts.
/// @throws ZeroSumViolation When any currency's legs do not sum to zero.
void checkZeroSumByCurrency(const std::vector<morph::math::Rational>& legAmounts,
                            const std::vector<db::AccountRecord>& legAccounts) {
    std::map<std::string, morph::math::Rational> sumsByCurrency;
    for (std::size_t i = 0; i < legAmounts.size(); ++i) {
        const std::string currency{legAccounts[i].currencyCode.Value().ToStringView()};
        auto it = sumsByCurrency.find(currency);
        if (it == sumsByCurrency.end()) {
            sumsByCurrency.emplace(currency, legAmounts[i]);
        } else {
            it->second = it->second + legAmounts[i];
        }
    }
    for (const auto& [currency, sum] : sumsByCurrency) {
        if (sum.numerator != 0) {
            throw ZeroSumViolation{currency, "legs did not sum to zero"};
        }
    }
}

/// @brief Splits @p text on every occurrence of @p delimiter, keeping empty
///        fields (so `"a,,b"` yields `{"a", "", "b"}`, and a trailing
///        delimiter yields a trailing empty field) -- the plain building
///        block `execute(ImportLedgerChunk)` uses twice: once to split a
///        CSV chunk into lines (on `'\n'`), once to split a line into its
///        four comma-separated fields (on `','`).
/// @param text The text to split.
/// @param delimiter The single character to split on.
/// @return Every field between consecutive delimiters, in order.
[[nodiscard]] std::vector<std::string> splitOn(std::string_view text, char delimiter) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            fields.emplace_back(text.substr(start));
            break;
        }
        fields.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

/// @brief Parses a decimal amount string like `"-4.50"` or `"12"` into a
///        `morph::math::Rational` by hand -- never through `std::stod`/
///        `std::atof` (a `double` intermediate reintroduces exactly the
///        floating-point imprecision `Rational`'s entire design exists to
///        avoid; see design spec §2/§7). Splits on `'.'`: the integer part
///        and fractional part concatenate into the numerator (`sign *
///        (integerPart * 10^fractionalDigits + fractionalPart)`),
///        `decimalPlaces` is the fractional part's own digit count (`0` for
///        a whole-amount field with no `'.'`), `denominator = 1`.
/// @param field The raw CSV amount field, e.g. `"-4.50"`.
/// @return The exact `Rational` the field denotes.
[[nodiscard]] morph::math::Rational parseAmount(const std::string& field) {
    std::string sign;
    std::string_view rest = field;
    if (!rest.empty() && (rest.front() == '-' || rest.front() == '+')) {
        sign = rest.front();
        rest = rest.substr(1);
    }
    const auto dotPos = rest.find('.');
    std::string integerPart;
    std::string fractionalPart;
    if (dotPos == std::string_view::npos) {
        integerPart = std::string{rest};
    } else {
        integerPart = std::string{rest.substr(0, dotPos)};
        fractionalPart = std::string{rest.substr(dotPos + 1)};
    }
    if (integerPart.empty()) {
        integerPart = "0";
    }
    const auto decimalPlaces = static_cast<std::uint32_t>(fractionalPart.size());
    // Concatenate the integer and fractional digit strings directly (rather
    // than computing integerPart * 10^decimalPlaces + fractionalPart in
    // std::int64_t arithmetic) so a field's magnitude is bounded only by
    // std::stoll's own range, not by an intermediate power-of-ten multiply
    // overflowing first.
    const std::string digits = integerPart + fractionalPart;
    const std::int64_t magnitude = digits.empty() ? 0 : std::stoll(digits);
    const std::int64_t numerator = (sign == "-") ? -magnitude : magnitude;
    return morph::math::Rational{morph::math::Numerator{numerator}, morph::math::Denominator{1},
                                 morph::math::DecimalPlaces{decimalPlaces}};
}

/// @brief RAII guard around a raw SQLite read-transaction snapshot
///        (`BEGIN DEFERRED` on construction, `COMMIT` on destruction).
///
///        Load-bearing because `Lightweight::DataMapperPool::Return`
///        performs no transaction cleanup on a returned connection (it
///        only tears down the async backend -- no `SQLEndTran`, no
///        autocommit reset): a connection returned to the pool with a
///        snapshot still open is silently inherited by whichever
///        unrelated caller acquires that connection next, which then
///        blocks for the full 60s `busy_timeout` the very first time it
///        tries to write. This guard's destructor always runs exactly
///        once per successful construction -- on the normal-exit path and
///        on every exception unwinding through it alike -- so every path
///        out of a scope holding one closes the transaction; a
///        constructor that itself throws leaves no transaction open in
///        the first place (see the constructor's own doc comment).
///
///        The destructor's own `COMMIT` is wrapped in `catch (...)`: a
///        failed release of a read-only transaction has nothing further to
///        report and must never mask whatever exception is already
///        propagating (or, on the non-exceptional path, silently eat a
///        real return value -- there is none here, this guard is void-only).
class WalSnapshotGuard {
public:
    /// @brief Pins a read snapshot on @p connection's connection by issuing
    ///        `BEGIN DEFERRED` as the first statement.
    /// @param connection The connection to pin. Must not already have an
    ///        open transaction -- this class does not check.
    explicit WalSnapshotGuard(Lightweight::SqlConnection& connection) : _connection{connection} {
        (void)::Lightweight::SqlStatement{_connection}.ExecuteDirect("BEGIN DEFERRED");
    }

    /// @brief Releases the pinned snapshot via `COMMIT`, swallowing any
    ///        failure (see this class's own doc comment for why).
    ~WalSnapshotGuard() {
        try {
            (void)::Lightweight::SqlStatement{_connection}.ExecuteDirect("COMMIT");
        } catch (...) {
            // Nothing left to report; must not mask a propagating exception.
        }
    }

    WalSnapshotGuard(const WalSnapshotGuard&) = delete;
    WalSnapshotGuard& operator=(const WalSnapshotGuard&) = delete;
    WalSnapshotGuard(WalSnapshotGuard&&) = delete;
    WalSnapshotGuard& operator=(WalSnapshotGuard&&) = delete;

private:
    Lightweight::SqlConnection& _connection;
};

/// @brief Decodes @p paramsJson as `MonthlyStatementParams`.
/// @param paramsJson The raw `SubmitReport::params` payload.
/// @return The decoded params, or `std::nullopt` when @p paramsJson is
///         absent, malformed, or names a month outside 1-12 -- in which case
///         the report falls back to an all-time balance rather than failing
///         the job, since a statement over a nonsensical month is a client
///         bug and an empty report is the more useful answer than a
///         permanently Failed row.
[[nodiscard]] std::optional<MonthlyStatementParams> decodeMonthlyParams(std::string_view paramsJson) {
    if (paramsJson.empty()) {
        return std::nullopt;
    }
    MonthlyStatementParams params;
    if (auto err = glz::read_json(params, paramsJson); err) {
        return std::nullopt;
    }
    if (params.month < 1 || params.month > 12) {
        return std::nullopt;
    }
    return params;
}

/// @brief Computes @p ledgerId's report body -- every account's balance
///        summed per currency -- against @p mapper, and serializes it to
///        JSON.
///
///        Deliberately simple for this rung's scope: `ReportKind` selects
///        nothing yet beyond being stored with the job.
///        `ReportKind::BudgetReport`'s own distinct aggregation is out of
///        scope here -- Task 10's `GetBudgetReport` already computes
///        budget-vs-spent, and this job does not duplicate that logic.
/// @param mapper The data mapper to query through -- expected to already be
///        inside a pinned read snapshot (see the worker lambda in
///        `execute(SubmitReport)`).
/// @param ledgerId The ledger to report on.
/// @param period When engaged, the local calendar month to scope the report
///        to; disengaged computes an all-time balance.
/// @return The serialized report body.
[[nodiscard]] std::string computeReportJson(Lightweight::DataMapper& mapper, const LedgerId& ledgerId,
                                            const std::optional<MonthlyStatementParams>& period) {
    // A monthly statement is a claim about the *client's* calendar, so the
    // month's own boundaries are converted to UTC instants once, here, and
    // every stored (UTC) journal date is compared against them -- never the
    // other way round. See ledger/core/time_util.hpp for why that direction
    // is the one that survives the boundary hours.
    std::optional<std::unordered_set<std::uint64_t>> journalsInPeriod;
    if (period.has_value()) {
        const auto [periodStart, periodEnd] =
            localMonthToUtcRange(period->year, period->month, period->timezoneOffsetMinutes);
        const auto startMillis = periodStart.value.time_since_epoch().count();
        const auto endMillis = periodEnd.value.time_since_epoch().count();
        journalsInPeriod.emplace();
        auto journalRows =
            mapper.Query<db::TransactionJournalRecord>()
                .Where(::Lightweight::FieldNameOf<&db::TransactionJournalRecord::ledger>, "=", *ledgerId)
                .All();
        for (const auto& journalRow : journalRows) {
            const auto date = journalRow.date.Value();
            // Half-open: an instant exactly at `end` is the next month's
            // first moment, so consecutive statements tile without counting
            // the boundary transaction twice.
            if (date >= startMillis && date < endMillis) {
                journalsInPeriod->insert(journalRow.id.Value());
            }
        }
    }
    const std::unordered_set<std::uint64_t>* const journalFilter =
        journalsInPeriod.has_value() ? &*journalsInPeriod : nullptr;

    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::ledger>, "=", *ledgerId)
                           .All();
    std::map<std::string, morph::math::Rational> totalsByCurrency;
    // Journals counted per currency, so the body says how many transactions
    // it covered rather than only what they netted to. Counted over journals
    // (not legs) so a two-leg transaction counts once, and through a set so
    // a transaction touching two accounts of the same currency still counts
    // once.
    std::map<std::string, std::unordered_set<std::uint64_t>> journalsByCurrency;
    for (const auto& accountRow : accountRows) {
        const auto currency = codeToCurrency(accountRow.currencyCode.Value().ToStringView());
        const auto decimalPlaces = morph::math::DecimalPlaces{UnitTraits<Currency>::meta(currency).defaultDecimals};
        const auto balance = sumAccountLegs(mapper, accountRow.id.Value(), decimalPlaces, journalFilter);
        const std::string code{accountRow.currencyCode.Value().ToStringView()};
        {
            auto accountLegs =
                mapper.Query<db::TransactionLegRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::TransactionLegRecord::account>, "=", accountRow.id.Value())
                    .All();
            for (const auto& legRow : accountLegs) {
                const auto journalId = legRow.journal.Value();
                if (journalFilter != nullptr && !journalFilter->contains(journalId)) {
                    continue;
                }
                journalsByCurrency[code].insert(journalId);
            }
        }
        auto it = totalsByCurrency.find(code);
        if (it == totalsByCurrency.end()) {
            totalsByCurrency.emplace(code, balance);
        } else {
            it->second = it->second + balance;
        }
    }
    std::vector<ReportLine> lines;
    lines.reserve(totalsByCurrency.size());
    for (const auto& [code, total] : totalsByCurrency) {
        const auto journalsForCode = journalsByCurrency.find(code);
        lines.push_back(
            ReportLine{.currency = code,
                       .numerator = total.numerator,
                       .denominator = total.denominator,
                       .decimalPlaces = total.decimalPlaces.value,
                       .transactionCount = journalsForCode == journalsByCurrency.end()
                                               ? 0
                                               : static_cast<std::int64_t>(journalsForCode->second.size())});
    }
    std::string resultJson;
    if (auto err = glz::write_json(lines, resultJson); err) {
        throw LedgerError{"SubmitReport: failed to serialize report result"};
    }
    return resultJson;
}

/// @brief Sets the `status` column of the report job row whose id is
///        @p jobId to @p status, and (when engaged) its `result_json` to
///        @p resultJson. A no-op if the row is gone (a test fixture dropping
///        every table out from under an in-flight worker is the realistic
///        way that happens; it is not an error worth throwing over).
/// @param mapper The data mapper to mutate through.
/// @param jobId The job row's primary key.
/// @param status The terminal status to record.
/// @param resultJson The serialized report body, or `std::nullopt` to leave
///        the column untouched.
void finishReportJob(Lightweight::DataMapper& mapper, std::int64_t jobId, ReportStatus status,
                     std::optional<std::string> resultJson) {
    auto jobRows =
        mapper.Query<db::ReportJobRecord>()
            .Where(::Lightweight::FieldNameOf<&db::ReportJobRecord::id>, "=", static_cast<std::uint64_t>(jobId))
            .All();
    if (jobRows.empty()) {
        return;
    }
    auto row = jobRows.front();
    row.status = static_cast<int>(status);
    if (resultJson.has_value()) {
        // Explicit std::optional{...} wrap, matching this file's own
        // established idiom for a nullable Field (see
        // execute(StoreTransaction)'s foreignAmountNum assignment):
        // resultJson's column type is
        // std::optional<Light::SqlMaxDynamicAnsiString>, not a bare
        // std::string.
        row.resultJson = std::optional{*std::move(resultJson)};
    }
    mapper.Update(row);
}

/// @brief Resolves one account by id and refuses it unless it belongs to
///        @p ledgerId -- the ledger the calling action names.
///
///        Account ids are a table-wide autoincrement, so another book's
///        account id is a perfectly well-formed number naming a real row: a
///        lookup by id alone finds it and accepts it. The leg then posts onto
///        that book's balance while the journal is filed under the ledger the
///        action named, and since every read *is* scoped, the named book's
///        reply does not mention the account it moved and the other book shows
///        a balance change with no journal of its own to explain it. The two
///        books disagree and neither report says so (morph#367).
///
///        The ledger is compared after the lookup rather than folded into it
///        as a second `Where`, so "no such account" and "that account is in
///        another book" stay two different answers -- a client that cannot
///        tell them apart cannot tell a dead id from a mis-scoped one. This is
///        `execute(UndoTransaction)`'s own idiom for the identical question
///        about a journal row ("journal does not belong to this ledger"),
///        applied to the account lookups that lacked it.
/// @param mapper The data mapper to query through.
/// @param accountId The account row's primary key.
/// @param ledgerId The ledger the calling action names.
/// @param action The calling action's name, prefixed onto both refusals.
/// @return The account row, which belongs to @p ledgerId.
/// @throws NotFound If no account has that id, or it belongs to another ledger.
[[nodiscard]] db::AccountRecord accountInLedger(Lightweight::DataMapper& mapper, const AccountId& accountId,
                                                const LedgerId& ledgerId, std::string_view action) {
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                    .All();
    if (rows.empty()) {
        throw NotFound{std::string{action} + ": no such account"};
    }
    if (rows.front().ledger.Value() != static_cast<std::uint64_t>(*ledgerId)) {
        throw NotFound{std::string{action} + ": account does not belong to this ledger"};
    }
    return rows.front();
}

}  // namespace

void LedgerModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _log = std::move(log);
    _entityKeyStr = std::move(entityKey);
}

template <typename Action, typename Result>
void LedgerModel::logAction(const Action& action, const Result& result, std::string causalParentId) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "LedgerModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    // Stamp the payload's shape fingerprint, the same value morph's own two
    // execution sites stamp on the entries they append. Without it every entry
    // this rung records is *unstamped*, and `journal::replay()`'s default
    // `UnstampedPayloadPolicy::Replay` would replay it unverified -- a later
    // build with a renamed field would decode it to a default and report a
    // state nobody ever recorded. Empty for an action whose `ActionTraits` is
    // hand-written; see docs/spec/journal/journal.md, "Payload schema
    // fingerprint".
    entry.schema = ::morph::model::detail::actionPayloadSchema<Action>();
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.outcome = ::morph::journal::Outcome::Succeeded;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = (*morph::ladder::now().value).value.time_since_epoch().count();  // server-stamped audit
                                                                                         // timestamp -- goes
                                                                                         // through the ladder's
                                                                                         // injectable clock
                                                                                         // convention, unlike
                                                                                         // StoreTransaction's own
                                                                                         // client-supplied date
    entry.causalParentId = std::move(causalParentId);
    _log->append(std::move(entry));
    // See kanban's own identical comment (design spec §5's citation) for
    // why this flush is load-bearing, not optional: append() writes
    // through buffered C stdio with no implicit flush for FileActionLog,
    // and entries() reads through a separate stream that cannot see
    // unflushed bytes. InMemoryActionLog::flush() is a no-op, so this
    // costs nothing for the log type most tests attach.
    _log->flush();
}

template <typename Action>
void LedgerModel::logFailure(const Action& action, const std::string& error) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "LedgerModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.schema = ::morph::model::detail::actionPayloadSchema<Action>();
    // No `result` -- there was none. `error` carries the rejecting
    // exception's text, matching LogEntry's own documented success/failure
    // shape (`morph/journal/action_log.hpp`).
    entry.error = error;
    entry.outcome = ::morph::journal::Outcome::Failed;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = (*morph::ladder::now().value).value.time_since_epoch().count();
    _log->append(std::move(entry));
    _log->flush();
}

CreateLedgerResult LedgerModel::execute(const CreateLedger& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        if (!action.validate()) {
            throw ValidationError{"CreateLedger: a non-empty name of at most 128 bytes is required"};
        }
        Lightweight::DataMapper mapper;
        db::LedgerRecord ledgerRow;
        ledgerRow.name = Light::SqlAnsiString<128>{action.name};
        mapper.Create(ledgerRow);
        auto result = CreateLedgerResult{.id = LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())}};
        logAction(action, result);
        return result;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

AccountInfo LedgerModel::execute(const OpenAccount& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        if (!action.validate()) {
            throw ValidationError{"OpenAccount: ledgerId and name are required"};
        }
        Lightweight::DataMapper mapper;
        // The ledger row must already exist -- `execute(const CreateLedger&)`
        // above is what creates one (morph#361). Load it by primary key
        // rather than fabricating a stub LedgerRecord, since
        // BelongsTo assignment needs the real persisted parent (per
        // polls::db::OptionRecord's own `opt.poll = poll;` usage, where `poll`
        // is a row that has actually round-tripped through Create/Query).
        auto ledgerRows = mapper.Query<db::LedgerRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                              .All();
        if (ledgerRows.empty()) {
            throw NotFound{"OpenAccount: no such ledger"};
        }
        db::AccountRecord accountRow;
        accountRow.ledger = ledgerRows.front();
        accountRow.name = action.name;
        accountRow.kind = static_cast<int>(action.kind);
        accountRow.currencyCode = currencyToCode(action.currency);
        mapper.Create(accountRow);
        // Returns the freshly created account's info, not void -- see
        // ledger_model.hpp's doc comment on this method for why a void
        // execute() cannot be registered via BRIDGE_REGISTER_ACTION.
        auto result = AccountInfo{
            .id = AccountId{static_cast<std::int64_t>(accountRow.id.Value())},
            .name = action.name,
            .kind = action.kind,
            .currency = action.currency,
            .balance = morph::math::Rational{morph::math::Numerator{0}, morph::math::Denominator{1},
                                             morph::math::DecimalPlaces{
                                                 UnitTraits<Currency>::meta(action.currency)
                                                     .defaultDecimals}},  // no
                                                                          // legs exist yet at this task's
                                                                          // scope -- Task 8 computes a real
                                                                          // balance -- but the placeholder
                                                                          // zero is still tagged at the
                                                                          // account's actual currency
                                                                          // precision (0 for JPY/KRW, 2 for
                                                                          // USD/EUR), not a hardcoded 2
        };
        logAction(action, result);
        return result;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

GetLedgerResult LedgerModel::execute(const GetLedger& action) {
    if (!action.validate()) {
        throw ValidationError{"GetLedger: ledgerId is required"};
    }
    Lightweight::DataMapper mapper;
    // Real balance per account: the sum of every leg posted against it,
    // computed in-model via Rational::operator+ (never a raw SQL SUM() --
    // see sumAccountLegs's own doc comment), via the shared buildLedgerState
    // helper (also used by execute(StoreTransaction) against its own
    // in-flight transaction's mapper -- see that helper's doc comment).
    return buildLedgerState(mapper, action.ledgerId);
}

GetLedgerResult LedgerModel::execute(const StoreTransaction& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        if (!action.validate()) {
            throw ValidationError{
                "StoreTransaction: description and at least two legs with engaged accountIds are required"};
        }
        Lightweight::DataMapper mapper;

        // Task 11b, design spec §1 (kanban's execute(MoveTaskPosition) pattern,
        // ladder-kanban-impl:examples/kanban/src/models/board_model.cpp):
        // ledger lookup, after the empty-principal/validate() checks above
        // (this action has no further role/auth gate), before any account
        // lookup or zero-sum partitioning. A disengaged opId (Task 8/9's own
        // existing call sites, which predate this field) skips this whole
        // block -- never attempts a lookup against an empty string key.
        if (action.opId.hasValue()) {
            auto existingOp =
                mapper.Query<db::AppliedOpRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AppliedOpRecord::ledger>, "=", *action.ledgerId)
                    .Where(::Lightweight::FieldNameOf<&db::AppliedOpRecord::opId>, "=", *action.opId)
                    .All();
            if (!existingOp.empty()) {
                GetLedgerResult replayed;
                if (auto err = glz::read_json(replayed, std::string{existingOp.front().resultJson.Value()}); err) {
                    throw LedgerError{"StoreTransaction: corrupt applied-ops ledger entry"};
                }
                // A ledger hit means this call performed nothing new -- it only
                // returned a previously-stored result -- so there is nothing to
                // journal here (verified against kanban's own identical point:
                // the framework's own auto-append does not double-log this
                // path, so skipping logAction on a ledger hit is correct).
                return replayed;
            }
        }

        // Partition legs by the account's OWN currency, never a client-supplied
        // field (design spec §1) -- look up every referenced account first.
        std::vector<db::AccountRecord> legAccounts;
        legAccounts.reserve(action.legs.size());
        for (const auto& leg : action.legs) {
            // Scoped to this action's own ledger -- see accountInLedger for why
            // an unscoped lookup here let a leg post onto another book.
            legAccounts.push_back(accountInLedger(mapper, leg.accountId, action.ledgerId, "StoreTransaction"));
        }

        // Every leg onto its own account currency's scale before anything sums
        // them, and the restated amounts -- not the client's -- are what get
        // stored below. See restateLegAmounts for why the invariant is unsound in
        // both directions without this.
        const auto legAmounts = restateLegAmounts(action.legs, legAccounts, "StoreTransaction");
        checkZeroSumByCurrency(legAmounts, legAccounts);

        // Constructor/commit shape copied verbatim from
        // bank::LoanModel::execute(const dto::TakeLoan&) (examples/bank/src/
        // models/loan_model.cpp:77-80) -- the exact multi-row-commit pattern
        // this rung's own StoreTransaction (journal + N legs) needs.
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        db::TransactionJournalRecord journalRow;
        journalRow.description = action.description;
        // DateTime->epoch-millis conversion copied verbatim from
        // bookmarks::db's own nowMs()/fromEpochMs() helpers
        // (bookmark_model.cpp:61-63): Timestamp::value is
        // std::optional<DateTime>, DateTime::value is
        // std::chrono::sys_time<milliseconds> -- .time_since_epoch().count()
        // gives the raw millisecond integer this entity column stores.
        // action.date is a client-supplied "when did this happen" field
        // (design spec §1) -- not a server audit stamp, so this does NOT go
        // through morph::ladder::now() (see this rung's own note on that
        // convention, which binds server-stamped timestamps like
        // ImportedOpRecord::appliedAtMs/ReportJobRecord::createdAtMs in later
        // tasks, not a client-supplied journal date).
        journalRow.date = action.date.value.has_value() ? (*action.date.value).value.time_since_epoch().count() : 0;
        auto ledgerRows = mapper.Query<db::LedgerRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                              .All();
        if (ledgerRows.empty()) {
            throw NotFound{"StoreTransaction: no such ledger"};
        }
        journalRow.ledger = ledgerRows.front();
        mapper.Create(journalRow);

        for (std::size_t i = 0; i < action.legs.size(); ++i) {
            db::TransactionLegRecord legRow;
            legRow.journal = journalRow;
            legRow.account = legAccounts[i];
            // The restated amount, never the client's: the stored scale is always
            // the account currency's own.
            legRow.amountNum = legAmounts[i].numerator;
            legRow.amountDen = legAmounts[i].denominator;
            legRow.amountDp = static_cast<int>(legAmounts[i].decimalPlaces.value);
            legRow.currencyCode = legAccounts[i].currencyCode.Value();
            // Foreign-amount triple: display/audit metadata only, never read by
            // the zero-sum partitioning loop above (design spec §1 step 3).
            // Assigned unconditionally -- std::optional's own empty state
            // already expresses "no foreign amount" through the nullable
            // column, so this never branches on whether the leg has one.
            const auto& foreignAmount = action.legs[i].foreignAmount;
            legRow.foreignAmountNum = foreignAmount ? std::optional{foreignAmount->numerator} : std::nullopt;
            legRow.foreignAmountDen = foreignAmount ? std::optional{foreignAmount->denominator} : std::nullopt;
            legRow.foreignAmountDp =
                foreignAmount ? std::optional{static_cast<int>(foreignAmount->decimalPlaces.value)} : std::nullopt;
            legRow.foreignCurrencyCode =
                action.legs[i].foreignCurrency
                    ? std::optional{Lightweight::SqlAnsiString<3>{currencyToCode(*action.legs[i].foreignCurrency)}}
                    : std::nullopt;
            mapper.Create(legRow);
        }

        // Rebuilt through the same mapper/in-flight transaction as the mutation
        // above (buildLedgerState, not a fresh execute(GetLedger{...}) call
        // against a second Lightweight::DataMapper connection) -- Task 11b's
        // applied-ops row below serializes this exact result and must commit
        // atomically with it.
        auto result = buildLedgerState(mapper, action.ledgerId);

        // Task 11b: written *inside* the same transaction, before sqlTxn.Commit()
        // -- confirmed against kanban's own real execute(MoveTaskPosition), which
        // creates its AppliedOpRecord before transaction.Commit() so the op-id
        // write and the business mutation commit atomically together.
        if (action.opId.hasValue()) {
            std::string resultJson;
            if (auto err = glz::write_json(result, resultJson); err) {
                throw LedgerError{"StoreTransaction: failed to serialize result for the applied-ops ledger"};
            }
            db::AppliedOpRecord op;
            op.ledger = ledgerRows.front();
            op.opId = *action.opId;
            op.resultJson = resultJson;
            op.createdAtMs = (*morph::ladder::now().value).value.time_since_epoch().count();
            mapper.Create(op);
        }

        // Cascade rule evaluation (design spec §4/§5): a matching
        // RuleTrigger::DescriptionContains rule cascades into a second,
        // causally-linked SetCategory mutation. The mutation itself
        // (setCategoryImpl below) runs inside this same SQL transaction, before
        // sqlTxn.Commit(), so it commits atomically with the triggering
        // journal+legs insert -- exactly like the applied-ops row above. The
        // *logging* of each fired cascade is deferred (into cascadesToLog) and
        // only emitted after the trigger's own logAction(action, result) call
        // below, so the trigger entry is always seq-ordered ahead of any cascade
        // entry it caused -- entries[0] is the trigger, per design spec §5's own
        // causal-order expectation and this task's own journal test.
        //
        // Gated on !isReplaying(): replay() re-dispatches this StoreTransaction
        // entry to reconstruct state, and the cascade it originally produced is
        // its own separate, already-recorded SetCategory entry later in the same
        // log -- re-evaluating rules here during replay would either double-apply
        // the cascade (if the rule is unchanged) or apply a *different* outcome
        // than what was actually recorded (if the rule was edited since), which
        // is exactly the divergence the causalParentId/ruleVersion pinning exists
        // to prevent. Live dispatch (isReplaying() == false) is the only time
        // this block ever runs.
        std::vector<SetCategory> cascadesToLog;
        if (!morph::journal::isReplaying()) {
            auto rules = mapper.Query<db::RuleRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::RuleRecord::ledger>, "=", *action.ledgerId)
                             .Where(::Lightweight::FieldNameOf<&db::RuleRecord::trigger>, "=",
                                    static_cast<int>(RuleTrigger::DescriptionContains))
                             .All();
            // Decision 1 (design spec's own step 4, transliterated for ledger):
            // the leg to categorize is the first Expense/Revenue account among
            // this transaction's own legs -- legAccounts is the same vector the
            // zero-sum partitioning loop above already built, positionally
            // aligned with action.legs.
            std::optional<std::size_t> categorizableLegIndex;
            for (std::size_t i = 0; i < legAccounts.size(); ++i) {
                const auto kind = static_cast<AccountKind>(legAccounts[i].kind.Value());
                if (kind == AccountKind::Expense || kind == AccountKind::Revenue) {
                    categorizableLegIndex = i;
                    break;
                }
            }
            if (categorizableLegIndex.has_value()) {
                for (const auto& rule : rules) {
                    if (action.description.find(std::string{rule.matchText.Value().ToStringView()}) ==
                        std::string::npos) {
                        continue;
                    }
                    // Decision 2: lookup, never auto-create -- a rule naming a
                    // category that doesn't exist in this ledger simply doesn't
                    // fire; it is not an error on the triggering transaction.
                    auto categoryRows =
                        mapper.Query<db::CategoryRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::ledger>, "=", *action.ledgerId)
                            .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::name>, "=",
                                   rule.actionValue.Value())
                            .All();
                    if (categoryRows.empty()) {
                        continue;
                    }
                    const SetCategory cascadeAction{
                        .accountId =
                            AccountId{static_cast<std::int64_t>(legAccounts[*categorizableLegIndex].id.Value())},
                        .categoryId = CategoryId{static_cast<std::int64_t>(categoryRows.front().id.Value())},
                        .ruleId = RuleId{static_cast<std::int64_t>(rule.id.Value())},
                        .ruleVersion = rule.version.Value()};
                    setCategoryImpl(mapper, cascadeAction);
                    cascadesToLog.push_back(cascadeAction);
                }
            }
        }

        sqlTxn.Commit();

        logAction(action, result);

        // Logged only now, after the trigger's own entry above, so the cascade
        // always lands strictly after its trigger in the log's seq order.
        // logAction is the *only* logger for each of these entries --
        // setCategoryImpl holds no logging of its own, and the public
        // execute(SetCategory) overload (which also calls setCategoryImpl, then
        // logs unconditionally with an empty causalParentId) is deliberately not
        // called from here, to avoid double-logging the same firing.
        const std::string triggerCausalId = "transactionJournal:" + std::to_string(journalRow.id.Value());
        for (const auto& cascadeAction : cascadesToLog) {
            logAction(cascadeAction, SetCategoryResult{}, triggerCausalId);
        }

        return result;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

GetLedgerResult LedgerModel::execute(const UndoTransaction& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        if (!action.validate()) {
            throw ValidationError{"UndoTransaction: ledgerId and journalId are required"};
        }
        Lightweight::DataMapper mapper;

        auto journalRows =
            mapper.Query<db::TransactionJournalRecord>()
                .Where(::Lightweight::FieldNameOf<&db::TransactionJournalRecord::id>, "=", *action.journalId)
                .All();
        if (journalRows.empty()) {
            throw NotFound{"UndoTransaction: no such journal"};
        }
        auto& originalJournalRow = journalRows.front();
        // Redundant-but-required field, per this action's own doc comment: a
        // wrong ledgerId cannot be used to target the wrong ledger's model
        // instance or bypass anything, since the journal's own ledger is
        // verified independently here.
        if (originalJournalRow.ledger.Value() != static_cast<std::uint64_t>(*action.ledgerId)) {
            throw NotFound{"UndoTransaction: journal does not belong to this ledger"};
        }

        // A compensating entry names the entry it reverses, so "has this already
        // been reversed?" is a query rather than mutable state on the original --
        // which keeps the original journal row immutable, as design spec §6
        // requires of an audit record.
        //
        // Without this check two devices that both queued a reversal of the same
        // transaction while offline each post one, and the second silently pays
        // the money back a second time. The ledger's per-currency zero-sum
        // invariant does not catch that: a reversal is itself zero-sum, so the
        // sum stays zero while the individual account balances go wrong.
        const auto reversalCausalParentId = "transactionJournal:" + std::to_string(originalJournalRow.id.Value());
        const auto existingReversals =
            mapper.Query<db::TransactionJournalRecord>()
                .Where(::Lightweight::FieldNameOf<&db::TransactionJournalRecord::causalParentId>, "=",
                       reversalCausalParentId)
                .All();
        if (!existingReversals.empty()) {
            throw AlreadyReversed{};
        }

        auto originalLegRows = mapper.Query<db::TransactionLegRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::TransactionLegRecord::journal>, "=",
                                          originalJournalRow.id.Value())
                                   .All();

        std::vector<TransactionLeg> reversalLegs;
        std::vector<db::AccountRecord> reversalLegAccounts;
        reversalLegs.reserve(originalLegRows.size());
        reversalLegAccounts.reserve(originalLegRows.size());
        for (const auto& legRow : originalLegRows) {
            const auto originalAmount = morph::math::Rational{
                morph::math::Numerator{legRow.amountNum.Value()}, morph::math::Denominator{legRow.amountDen.Value()},
                morph::math::DecimalPlaces{static_cast<std::uint32_t>(legRow.amountDp.Value())}};
            auto accountRows =
                mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", legRow.account.Value())
                    .All();
            if (accountRows.empty()) {
                throw NotFound{"UndoTransaction: no such account"};
            }
            reversalLegAccounts.push_back(accountRows.front());
            // Member unary negation (Rational::operator-() const), never the
            // free binary subtraction operator also declared in rational.hpp --
            // see this action's own doc comment.
            reversalLegs.push_back(TransactionLeg{
                .accountId = AccountId{static_cast<std::int64_t>(legRow.account.Value())}, .amount = -originalAmount});
        }

        // The reversal's own date is "now" (when the undo happened), via
        // morph::time::Timestamp::now() -- the same type/convention
        // StoreTransaction.date itself uses for a client-observable "when did
        // this happen" field (see execute(StoreTransaction)'s own comment on
        // why journalRow.date does NOT go through morph::ladder::now()) -- NOT
        // the original journal's own date, which belongs to the transaction
        // being reversed, not the reversal itself.
        auto result =
            storeJournalImpl(mapper, action.ledgerId,
                             "Reversal of: " + std::string{originalJournalRow.description.Value().ToStringView()},
                             morph::time::Timestamp::now(), reversalLegs, reversalLegAccounts, reversalCausalParentId);

        logAction(action, result, reversalCausalParentId);
        return result;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

ImportResult LedgerModel::execute(const ImportLedgerChunk& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        const std::string principal = ctx->principal;

        if (action.csvChunk.empty() || !action.ledgerId.hasValue() || !action.counterAccountId.hasValue()) {
            throw ValidationError{"ImportLedgerChunk: ledgerId, counterAccountId, and csvChunk are required"};
        }

        Lightweight::DataMapper mapper;

        // Chunk-retry dedup (design spec §8, Task 15's own scope-narrowing
        // ruling): the opId ledger is populated below (once per chunk, after
        // every row has been processed) so `ledger_imported_ops` satisfies this
        // task's own "chunk-level opId dedup" interface line, but it is
        // deliberately NOT read back here for an early return. `ImportedOpRecord`
        // (unlike `StoreTransaction`'s `AppliedOpRecord`) stores no
        // `result_json` -- mirroring bookmarks::db::ImportedOpRecord's own
        // shape exactly -- so an early return on a ledger hit could only ever
        // produce a zeroed `ImportResult{}`, which would UNDER-report a genuine
        // replay's real imported/duplicates counts. A replay is still a safe
        // no-op without this early return: re-parsing the identical csvChunk
        // re-derives identical content hashes, which the hash-dedup check below
        // re-skips on its own. Building a correctly-counted early return would
        // require storing those counts, which this task's own test does not
        // need -- recorded as a deliberate ruling, not an unresolved TODO.
        //
        // No single `Lightweight::SqlTransaction` wraps this whole loop:
        // `storeJournalImpl` (reused per row below) opens and commits its own
        // `SqlTransaction` on this same `mapper.Connection()`, and
        // `Lightweight::SqlTransaction`'s constructor/destructor toggle
        // `SQL_ATTR_AUTOCOMMIT` on the connection directly (confirmed against
        // its real implementation) -- nesting a second one around it would
        // have the inner `Commit()` re-enable autocommit and end the
        // transaction out from under the still-open outer one, silently
        // breaking rollback-on-throw for every row after the first. Every
        // other multi-row commit in this file (`execute(StoreTransaction)`,
        // `execute(UndoTransaction)`) also opens exactly one
        // `Lightweight::SqlTransaction` per call, never two nested ones on the
        // same connection -- this loop instead commits one row at a time,
        // atomically, via `storeJournalImpl`'s own transaction: a thrown
        // `ValidationError`/`NotFound`/`ZeroSumViolation` on a malformed or
        // unbalanced row still leaves every already-committed row from earlier
        // in the same chunk in place (each is checked and posted as its own
        // complete, zero-sum journal entry by `storeJournalImpl` -- see that
        // method's own doc comment), it just does not roll the whole chunk back
        // to empty.
        auto ledgerRows = mapper.Query<db::LedgerRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                              .All();
        if (ledgerRows.empty()) {
            throw NotFound{"ImportLedgerChunk: no such ledger"};
        }

        // Scoped to the chunk's own ledger, like every row's account below: a
        // counter account from another book would otherwise take one leg of
        // every imported row (see accountInLedger).
        const auto counterAccountRow =
            accountInLedger(mapper, action.counterAccountId, action.ledgerId, "ImportLedgerChunk");

        std::int64_t imported = 0;
        std::int64_t duplicates = 0;

        // Split on '\n', skip the header line (row 0).
        auto lines = splitOn(action.csvChunk, '\n');
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            if (lineIndex == 0) {
                continue;  // header row
            }
            if (lines[lineIndex].empty()) {
                continue;
            }
            auto fields = splitOn(lines[lineIndex], ',');
            if (fields.size() != 4) {
                throw ValidationError{"ImportLedgerChunk: malformed CSV row"};
            }
            const auto& dateField = fields[0];
            const auto& descriptionField = fields[1];
            const auto& accountIdField = fields[2];
            const auto& amountField = fields[3];

            auto parsedDate = morph::time::DateTime::fromIso8601(dateField);
            if (!parsedDate.has_value()) {
                throw ValidationError{"ImportLedgerChunk: malformed date"};
            }
            const auto rowAccountId = AccountId{std::stoll(accountIdField)};
            const auto amount = parseAmount(amountField);

            // The CSV's own `account_id` column is client-supplied text, so it
            // is scoped exactly like the counter account above.
            const auto rowAccountRow = accountInLedger(mapper, rowAccountId, action.ledgerId, "ImportLedgerChunk");

            // Content hash (design spec §8's "description + date + legs,
            // canonicalized" -- the amount IS the leg here, since each row is a
            // single two-leg entry whose only client-supplied amount is this one
            // value): description + "|" + ISO date string + "|" + numerator +
            // "|" + denominator + "|" + decimalPlaces.
            const std::string hashInput = descriptionField + "|" + dateField + "|" + std::to_string(amount.numerator) +
                                          "|" + std::to_string(amount.denominator) + "|" +
                                          std::to_string(amount.decimalPlaces.value);
            const std::string hash = std::to_string(std::hash<std::string>{}(hashInput));

            auto existingHashRows =
                mapper.Query<db::ImportedTxnHashRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ImportedTxnHashRecord::ledger>, "=", *action.ledgerId)
                    .Where(::Lightweight::FieldNameOf<&db::ImportedTxnHashRecord::hash>, "=", hash)
                    .All();
            if (!existingHashRows.empty()) {
                ++duplicates;
                continue;
            }

            const std::vector<TransactionLeg> legs{
                TransactionLeg{.accountId = rowAccountId, .amount = amount},
                TransactionLeg{.accountId = action.counterAccountId, .amount = -amount}};
            const std::vector<db::AccountRecord> legAccounts{rowAccountRow, counterAccountRow};
            [[maybe_unused]] auto rowResult = storeJournalImpl(mapper, action.ledgerId, descriptionField,
                                                               morph::time::Timestamp{*parsedDate}, legs, legAccounts);

            db::ImportedTxnHashRecord hashRow;
            hashRow.ledger = ledgerRows.front();
            hashRow.hash = hash;
            mapper.Create(hashRow);

            ++imported;
        }

        // Populated (not read back for an early-return -- see this method's own
        // comment above), but still guarded by a lookup rather than an
        // unconditional insert: `ledger_imported_ops` has a real UNIQUE index on
        // `(owner_principal, op_id)` (the migration's own constraint, mirroring
        // bookmarks::db::ImportedOpRecord's identical shape), so a replayed
        // chunk under the same opId would otherwise violate it on its second
        // call -- turning the intended safe no-op into a thrown SQL error. The
        // lookup here exists purely to keep the replay safe, not to short-
        // circuit any of the work above.
        if (action.opId.hasValue()) {
            auto existingOpRows =
                mapper.Query<db::ImportedOpRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::ownerPrincipal>, "=", principal)
                    .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::opId>, "=", *action.opId)
                    .All();
            if (existingOpRows.empty()) {
                db::ImportedOpRecord opRow;
                opRow.ownerPrincipal = principal;
                opRow.opId = *action.opId;
                opRow.appliedAtMs = (*morph::ladder::now().value).value.time_since_epoch().count();
                mapper.Create(opRow);
            }
        }

        auto result = ImportResult{.imported = imported, .duplicates = duplicates};
        logAction(action, result);
        return result;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

ReportJobId LedgerModel::execute(const SubmitReport& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"SubmitReport: ledgerId is required"};
    }
    Lightweight::DataMapper mapper;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                          .All();
    if (ledgerRows.empty()) {
        throw NotFound{"SubmitReport: no such ledger"};
    }

    db::ReportJobRecord jobRow;
    jobRow.ledger = ledgerRows.front();
    jobRow.kind = static_cast<int>(action.kind);
    jobRow.status = static_cast<int>(ReportStatus::Pending);
    jobRow.createdAtMs = (*morph::ladder::now().value).value.time_since_epoch().count();
    // Stored verbatim, uninterpreted, exactly as SubmitReport's doc comment
    // promises: whoever eventually runs this job -- a runner in another
    // process, or the same one after a restart -- has nothing but this row to
    // reconstruct the request from. Decoding is deferred to
    // execute(RunReportJob), where a malformed payload is still not an error
    // (decodeMonthlyParams falls back to an all-time report).
    jobRow.paramsJson = std::optional{action.params};
    mapper.Create(jobRow);
    // job_id stores the row's own stringified id. ReportJobRecord::job_id (a
    // string column) and ReportJobId (an int64-based strong id) both predate
    // this task, which is the first to populate either; reconciling them this
    // way keeps the column consistent with `id` rather than leaving it dead
    // schema, and needs no migration. (A later reviewer could reasonably
    // observe the column is now redundant with `id` and drop it -- out of
    // this task's scope to decide unilaterally.)
    jobRow.jobId = std::to_string(jobRow.id.Value());
    mapper.Update(jobRow);

    // Not journaled, exactly as before this action stopped owning a worker:
    // SubmitReport's own effect is the Pending row, and the entry worth
    // auditing is the one RunReportJob writes when the report is actually
    // produced. Adding a second entry here would be an unrelated change to
    // this rung's audit trail.
    return ReportJobId{static_cast<std::int64_t>(jobRow.id.Value())};
}

RunReportJobResult LedgerModel::execute(const RunReportJob& action) {
    // A genuine authorization boundary -- see kReportRunnerPrincipal's own
    // doc comment: no client can obtain a validly signed token for this
    // principal, so this re-check (the model's own, independent of whatever
    // RemoteServer's authorizer already verified -- docs/spec/security.md)
    // is what actually keeps a client dispatching RunReportJob, by mistake or
    // otherwise, from completing a job the runner owns.
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal != kReportRunnerPrincipal) {
        throw Forbidden{"RunReportJob: only the report runner may run a report job"};
    }
    if (!action.validate()) {
        throw ValidationError{"RunReportJob: jobId and ledgerId are required"};
    }

    Lightweight::DataMapper mapper;
    auto jobRows = mapper.Query<db::ReportJobRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::ReportJobRecord::id>, "=",
                              static_cast<std::uint64_t>(*action.jobId))
                       .All();
    if (jobRows.empty()) {
        throw NotFound{"RunReportJob: no such job"};
    }
    const auto currentStatus = static_cast<ReportStatus>(jobRows.front().status.Value());
    if (currentStatus != ReportStatus::Pending) {
        // Already settled: recompute nothing, overwrite nothing. The runner
        // re-dispatches a job whenever a pass ticks while an earlier pass's
        // dispatch for the same job is still outstanding, and both dispatches
        // land on this ledger's one strand, so the second one arrives here
        // and takes exactly this branch. It is also what makes
        // "byte-identical on re-poll" hold trivially -- there is only ever
        // one computation per job.
        return RunReportJobResult{.status = currentStatus};
    }

    // Decoded from the row, not from an action field: the params travelled
    // through the database, which is the whole point of storing them.
    std::optional<MonthlyStatementParams> reportPeriod;
    if (static_cast<ReportKind>(jobRows.front().kind.Value()) == ReportKind::MonthlyStatement) {
        const auto& storedParams = jobRows.front().paramsJson.Value();
        reportPeriod =
            decodeMonthlyParams(storedParams.has_value() ? storedParams->ToStringView() : std::string_view{});
    }

    try {
        // The ledger guard every sibling action already has -- OpenAccount,
        // StoreTransaction, ImportLedgerChunk, SubmitReport and
        // storeJournalImpl all refuse a ledger they cannot find, and this
        // action checked only its own job row. Without it a job whose ledger
        // has since been deleted aggregates an empty account set, produces
        // `[]` and settles Done, so a caller cannot tell "no such ledger"
        // from "a ledger with no activity" (morph#250).
        //
        // Raised *inside* this try on purpose. Throwing out of the method
        // instead would leave the row Pending, and ledger::app::App re-sweeps
        // every Pending row on every pass -- the same doomed job would be
        // re-dispatched forever. The catch below settles it Failed, which is
        // terminal, which is the property the row needs.
        auto ledgerRows = mapper.Query<db::LedgerRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                              .All();
        if (ledgerRows.empty()) {
            throw NotFound{"RunReportJob: no such ledger"};
        }

        std::string resultJson;
        {
            // Read-transaction snapshot pinning (IMPLEMENTATION.md rule 4's
            // pre-cleared raw-SQL escape tier): a raw BEGIN DEFERRED as the
            // FIRST statement on this connection for the aggregation, so
            // every query it makes sees one consistent snapshot rather than a
            // partial concurrent write mid-aggregation.
            //
            // Still needed even though this now runs on the ledger's own
            // strand: the strand serialises this ledger's *own* actions, and
            // says nothing about BudgetModel (its own strand) or any other
            // connection writing to the same database.
            // Lightweight::SqlTransaction cannot express this -- it only
            // toggles SQL_ATTR_AUTOCOMMIT via ODBC and issues no BEGIN of its
            // own (see examples/common/testkit/db_busy_fixture.hpp's own doc
            // comment, which demonstrates this same raw pattern with
            // IMMEDIATE). DataMapper::Query issues its SQL through exactly
            // the connection DataMapper::Connection() exposes, so the pin
            // covers every query below.
            //
            // WalSnapshotGuard's destructor closes this transaction (via
            // COMMIT, swallowing any failure -- see its own doc comment) no
            // matter how this scope is exited, including by
            // computeReportJson throwing.
            WalSnapshotGuard snapshot{mapper.Connection()};
            resultJson = computeReportJson(mapper, action.ledgerId, reportPeriod);
        }
        // Written only after the read snapshot has been released, so this
        // connection is not simultaneously holding a read lock and asking for
        // a write one.
        finishReportJob(mapper, *action.jobId, ReportStatus::Done, std::move(resultJson));
    } catch (...) {
        // Catch-all, not just `const std::exception&`: an escaping exception
        // of any type must still leave the job in a terminal state, or a
        // poller would spin against Pending forever. Unlike the thread-pool
        // worker this replaced, the failure is also *reported* -- the runner
        // gets a Failed result back rather than only a log line -- but the
        // job row is still settled here rather than by rethrowing, because a
        // thrown dispatch would leave the row Pending and the next pass would
        // retry an aggregation that has already been shown to fail.
        std::string detail = "unknown exception";
        try {
            throw;
        } catch (const std::exception& exc) {
            detail = exc.what();
        } catch (...) {
            // keep the placeholder
        }
        ::morph::log::logError("[ledger] RunReportJob failed: " + detail);
        try {
            finishReportJob(mapper, *action.jobId, ReportStatus::Failed, std::nullopt);
        } catch (...) {
            // A failure recording the failure has nowhere left to go at this
            // rung's scope -- the job stays Pending and the next pass retries
            // it, an accepted limitation rather than a silently swallowed one
            // (the same shape bookmarks' own fetchMetadataOnce() catch block
            // settles for).
            ::morph::log::logError("[ledger] RunReportJob failed and could not record failure");
        }
        return RunReportJobResult{.status = ReportStatus::Failed};
    }

    auto result = RunReportJobResult{.status = ReportStatus::Done};
    logAction(action, result);
    return result;
}

GetReportStatusResult LedgerModel::execute(const GetReportStatus& action) {
    if (!action.validate()) {
        throw ValidationError{"GetReportStatus: jobId is required"};
    }
    Lightweight::DataMapper mapper;
    auto jobRows = mapper.Query<db::ReportJobRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::ReportJobRecord::id>, "=",
                              static_cast<std::uint64_t>(*action.jobId))
                       .All();
    if (jobRows.empty()) {
        throw NotFound{"GetReportStatus: no such job"};
    }
    const auto& row = jobRows.front();
    return GetReportStatusResult{
        .status = static_cast<ReportStatus>(row.status.Value()),
        .result = row.resultJson.Value().has_value()
                      ? std::optional{std::string{row.resultJson.Value()->ToStringView()}}
                      : std::nullopt,
    };
}

SetCategoryResult LedgerModel::execute(const SetCategory& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        Lightweight::DataMapper mapper;
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        setCategoryImpl(mapper, action);
        sqlTxn.Commit();
        logAction(action, SetCategoryResult{});
        return SetCategoryResult{};
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

void LedgerModel::setCategoryImpl(Lightweight::DataMapper& mapper, const SetCategory& action) {
    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                           .All();
    auto categoryRows = mapper.Query<db::CategoryRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::id>, "=", *action.categoryId)
                            .All();
    if (accountRows.empty() || categoryRows.empty()) {
        throw NotFound{"SetCategory: no such account or category"};
    }
    accountRows.front().category = categoryRows.front();
    mapper.Update(accountRows.front());
}

GetLedgerResult LedgerModel::storeJournalImpl(Lightweight::DataMapper& mapper, const LedgerId& ledgerId,
                                              const std::string& description, const morph::time::Timestamp& date,
                                              const std::vector<TransactionLeg>& legs,
                                              const std::vector<db::AccountRecord>& legAccounts,
                                              std::optional<std::string> causalParentId) {
    // Before the transaction opens, so a rejected leg never leaves one behind:
    // the same restatement and per-currency zero-sum check
    // `execute(StoreTransaction)` performs, applied here too because this
    // path has its own callers (undo, CSV import) that reach the leg columns
    // without going through that method. Neither check is optional per
    // caller -- see this method's own doc comment in ledger_model.hpp for why
    // the check lives here rather than in each caller.
    const auto legAmounts = restateLegAmounts(legs, legAccounts, "storeJournalImpl");
    checkZeroSumByCurrency(legAmounts, legAccounts);
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::TransactionJournalRecord journalRow;
    journalRow.description = description;
    journalRow.causalParentId =
        causalParentId ? std::optional{Lightweight::SqlAnsiString<64>{*causalParentId}} : std::nullopt;
    journalRow.date = date.value.has_value() ? (*date.value).value.time_since_epoch().count() : 0;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *ledgerId)
                          .All();
    if (ledgerRows.empty()) {
        throw NotFound{"storeJournalImpl: no such ledger"};
    }
    journalRow.ledger = ledgerRows.front();
    mapper.Create(journalRow);
    for (std::size_t i = 0; i < legs.size(); ++i) {
        db::TransactionLegRecord legRow;
        legRow.journal = journalRow;
        legRow.account = legAccounts[i];
        legRow.amountNum = legAmounts[i].numerator;
        legRow.amountDen = legAmounts[i].denominator;
        legRow.amountDp = static_cast<int>(legAmounts[i].decimalPlaces.value);
        legRow.currencyCode = legAccounts[i].currencyCode.Value();
        const auto& foreignAmount = legs[i].foreignAmount;
        legRow.foreignAmountNum = foreignAmount ? std::optional{foreignAmount->numerator} : std::nullopt;
        legRow.foreignAmountDen = foreignAmount ? std::optional{foreignAmount->denominator} : std::nullopt;
        legRow.foreignAmountDp =
            foreignAmount ? std::optional{static_cast<int>(foreignAmount->decimalPlaces.value)} : std::nullopt;
        legRow.foreignCurrencyCode =
            legs[i].foreignCurrency
                ? std::optional{Lightweight::SqlAnsiString<3>{currencyToCode(*legs[i].foreignCurrency)}}
                : std::nullopt;
        mapper.Create(legRow);
    }
    auto result = buildLedgerState(mapper, ledgerId);
    sqlTxn.Commit();
    return result;
}

}  // namespace ledger
