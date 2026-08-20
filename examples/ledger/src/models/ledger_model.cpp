// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/core/units.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <morph/core/logger.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ledger {

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
/// @return The account's real balance -- the sum of all its legs.
[[nodiscard]] morph::math::Rational sumAccountLegs(Lightweight::DataMapper& mapper, std::uint64_t accountId,
                                                    morph::math::DecimalPlaces decimalPlaces) {
    auto legRows = mapper.Query<db::TransactionLegRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::TransactionLegRecord::account>, "=", accountId)
                        .All();
    auto total = morph::math::Rational::zero(decimalPlaces);
    for (const auto& legRow : legRows) {
        const auto legAmount = morph::math::Rational{morph::math::Numerator{legRow.amountNum.Value()},
                                                       morph::math::Denominator{legRow.amountDen.Value()},
                                                       morph::math::DecimalPlaces{
                                                           static_cast<std::uint32_t>(legRow.amountDp.Value())}};
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
    for (const auto& row : rows) {
        const auto currency = codeToCurrency(row.currencyCode.Value().ToStringView());
        const auto decimalPlaces = morph::math::DecimalPlaces{UnitTraits<Currency>::meta(currency).defaultDecimals};
        result.accounts.push_back(AccountInfo{
            .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
            .name = std::string{row.name.Value().ToStringView()},
            .kind = static_cast<AccountKind>(row.kind.Value()),
            .currency = currency,
            .balance = sumAccountLegs(mapper, row.id.Value(), decimalPlaces),
        });
    }
    return result;
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
/// @return The serialized report body.
[[nodiscard]] std::string computeReportJson(Lightweight::DataMapper& mapper, const LedgerId& ledgerId) {
    auto accountRows = mapper.Query<db::AccountRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::AccountRecord::ledger>, "=", *ledgerId)
                            .All();
    std::map<std::string, morph::math::Rational> totalsByCurrency;
    for (const auto& accountRow : accountRows) {
        const auto currency = codeToCurrency(accountRow.currencyCode.Value().ToStringView());
        const auto decimalPlaces = morph::math::DecimalPlaces{UnitTraits<Currency>::meta(currency).defaultDecimals};
        const auto balance = sumAccountLegs(mapper, accountRow.id.Value(), decimalPlaces);
        const std::string code{accountRow.currencyCode.Value().ToStringView()};
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
        lines.push_back(ReportLine{.currency = code,
                                   .numerator = total.numerator,
                                   .denominator = total.denominator,
                                   .decimalPlaces = total.decimalPlaces.value});
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
    auto jobRows = mapper.Query<db::ReportJobRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::ReportJobRecord::id>, "=",
                               static_cast<std::uint64_t>(jobId))
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

AccountInfo LedgerModel::execute(const OpenAccount& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"OpenAccount: ledgerId and name are required"};
    }
    Lightweight::DataMapper mapper;
    // The ledger row must already exist -- this rung's scope has no
    // CreateLedger action (see the design note in the task brief); load it
    // by primary key rather than fabricating a stub LedgerRecord, since
    // BelongsTo assignment needs the real persisted parent (per
    // polls::db::OptionRecord's own `opt.poll = poll;` usage, where `poll`
    // is a row that has actually round-tripped through Create/Query).
    auto ledgerRows =
        mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId).All();
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
        .balance = morph::math::Rational{
            morph::math::Numerator{0}, morph::math::Denominator{1},
            morph::math::DecimalPlaces{UnitTraits<Currency>::meta(action.currency).defaultDecimals}},  // no
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
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"StoreTransaction: description and at least two legs with engaged accountIds are required"};
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
        auto existingOp = mapper.Query<db::AppliedOpRecord>()
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
    std::map<std::string, morph::math::Rational> sumsByCurrency;
    std::vector<db::AccountRecord> legAccounts;
    legAccounts.reserve(action.legs.size());
    for (const auto& leg : action.legs) {
        auto rows = mapper.Query<db::AccountRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *leg.accountId)
                        .All();
        if (rows.empty()) {
            throw NotFound{"StoreTransaction: no such account"};
        }
        legAccounts.push_back(rows.front());
        const std::string currency{legAccounts.back().currencyCode.Value().ToStringView()};
        auto it = sumsByCurrency.find(currency);
        if (it == sumsByCurrency.end()) {
            sumsByCurrency.emplace(currency, leg.amount);
        } else {
            it->second = it->second + leg.amount;
        }
    }
    for (const auto& [currency, sum] : sumsByCurrency) {
        if (sum.numerator != 0) {
            throw ZeroSumViolation{currency, "legs did not sum to zero"};
        }
    }

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
        legRow.amountNum = action.legs[i].amount.numerator;
        legRow.amountDen = action.legs[i].amount.denominator;
        legRow.amountDp = static_cast<int>(action.legs[i].amount.decimalPlaces.value);
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
                        .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::name>, "=", rule.actionValue.Value())
                        .All();
                if (categoryRows.empty()) {
                    continue;
                }
                const SetCategory cascadeAction{
                    .accountId = AccountId{static_cast<std::int64_t>(legAccounts[*categorizableLegIndex].id.Value())},
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
}

GetLedgerResult LedgerModel::execute(const UndoTransaction& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"UndoTransaction: ledgerId and journalId are required"};
    }
    Lightweight::DataMapper mapper;

    auto journalRows = mapper.Query<db::TransactionJournalRecord>()
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

    auto originalLegRows = mapper.Query<db::TransactionLegRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::TransactionLegRecord::journal>, "=",
                                       originalJournalRow.id.Value())
                                .All();

    std::vector<TransactionLeg> reversalLegs;
    std::vector<db::AccountRecord> reversalLegAccounts;
    reversalLegs.reserve(originalLegRows.size());
    reversalLegAccounts.reserve(originalLegRows.size());
    for (const auto& legRow : originalLegRows) {
        const auto originalAmount = morph::math::Rational{morph::math::Numerator{legRow.amountNum.Value()},
                                                            morph::math::Denominator{legRow.amountDen.Value()},
                                                            morph::math::DecimalPlaces{
                                                                static_cast<std::uint32_t>(legRow.amountDp.Value())}};
        auto accountRows = mapper.Query<db::AccountRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", legRow.account.Value())
                                .All();
        if (accountRows.empty()) {
            throw NotFound{"UndoTransaction: no such account"};
        }
        reversalLegAccounts.push_back(accountRows.front());
        // Member unary negation (Rational::operator-() const), never the
        // free binary subtraction operator also declared in rational.hpp --
        // see this action's own doc comment.
        reversalLegs.push_back(TransactionLeg{.accountId = AccountId{static_cast<std::int64_t>(legRow.account.Value())},
                                               .amount = -originalAmount});
    }

    // The reversal's own date is "now" (when the undo happened), via
    // morph::time::Timestamp::now() -- the same type/convention
    // StoreTransaction.date itself uses for a client-observable "when did
    // this happen" field (see execute(StoreTransaction)'s own comment on
    // why journalRow.date does NOT go through morph::ladder::now()) -- NOT
    // the original journal's own date, which belongs to the transaction
    // being reversed, not the reversal itself.
    auto result = storeJournalImpl(mapper, action.ledgerId,
                                    "Reversal of: " + std::string{originalJournalRow.description.Value().ToStringView()},
                                    morph::time::Timestamp::now(), reversalLegs, reversalLegAccounts);

    logAction(action, result, "transactionJournal:" + std::to_string(originalJournalRow.id.Value()));
    return result;
}

ImportResult LedgerModel::execute(const ImportLedgerChunk& action) {
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
    // `ValidationError`/`NotFound` on a malformed row still leaves every
    // already-committed row from earlier in the same chunk in place
    // (each was its own complete, self-balancing journal entry), it just
    // does not roll the whole chunk back to empty.
    auto ledgerRows =
        mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId).All();
    if (ledgerRows.empty()) {
        throw NotFound{"ImportLedgerChunk: no such ledger"};
    }

    auto counterAccountRows = mapper.Query<db::AccountRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.counterAccountId)
                                   .All();
    if (counterAccountRows.empty()) {
        throw NotFound{"ImportLedgerChunk: no such account"};
    }
    const auto& counterAccountRow = counterAccountRows.front();

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

        auto rowAccountRows = mapper.Query<db::AccountRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *rowAccountId)
                                   .All();
        if (rowAccountRows.empty()) {
            throw NotFound{"ImportLedgerChunk: no such account"};
        }
        const auto& rowAccountRow = rowAccountRows.front();

        // Content hash (design spec §8's "description + date + legs,
        // canonicalized" -- the amount IS the leg here, since each row is a
        // single two-leg entry whose only client-supplied amount is this one
        // value): description + "|" + ISO date string + "|" + numerator +
        // "|" + denominator + "|" + decimalPlaces.
        const std::string hashInput = descriptionField + "|" + dateField + "|" + std::to_string(amount.numerator) +
                                       "|" + std::to_string(amount.denominator) + "|" +
                                       std::to_string(amount.decimalPlaces.value);
        const std::string hash = std::to_string(std::hash<std::string>{}(hashInput));

        auto existingHashRows = mapper.Query<db::ImportedTxnHashRecord>()
                                     .Where(::Lightweight::FieldNameOf<&db::ImportedTxnHashRecord::ledger>, "=",
                                            *action.ledgerId)
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
        [[maybe_unused]] auto rowResult =
            storeJournalImpl(mapper, action.ledgerId, descriptionField, morph::time::Timestamp{*parsedDate}, legs, legAccounts);

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
        auto existingOpRows = mapper.Query<db::ImportedOpRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::ownerPrincipal>, "=",
                                          principal)
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
    auto ledgerRows =
        mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId).All();
    if (ledgerRows.empty()) {
        throw NotFound{"SubmitReport: no such ledger"};
    }

    db::ReportJobRecord jobRow;
    jobRow.ledger = ledgerRows.front();
    jobRow.kind = static_cast<int>(action.kind);
    jobRow.status = static_cast<int>(ReportStatus::Pending);
    jobRow.createdAtMs = (*morph::ladder::now().value).value.time_since_epoch().count();
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

    const auto jobId = ReportJobId{static_cast<std::int64_t>(jobRow.id.Value())};

    // Only plain values cross the thread boundary: the job's own integer id
    // and the ledger id, both copied. Nothing from this stack frame -- not
    // `mapper`, not `ctx` (a thread-local session pointer), not `action` --
    // is captured: `execute()` returns and tears all of that down long
    // before the worker runs. See _reportExecutor's own comment (and
    // docs/findings/003) for why this model owns an executor at all.
    const auto jobIdValue = *jobId;
    const auto ledgerId = action.ledgerId;
    _reportExecutor->post([jobIdValue, ledgerId] {
        try {
            auto workerMapper = ::Lightweight::GlobalDataMapperPool().Acquire();
            std::string resultJson;
            {
                // Read-transaction snapshot pinning (IMPLEMENTATION.md rule
                // 4's pre-cleared raw-SQL escape tier): a raw BEGIN DEFERRED
                // as the FIRST statement on this connection, before any
                // DataMapper::Query<T>() call, so every query the
                // aggregation makes sees one consistent snapshot rather than
                // a partial concurrent write mid-aggregation.
                // Lightweight::SqlTransaction cannot do this -- it only
                // toggles SQL_ATTR_AUTOCOMMIT via ODBC and issues no BEGIN
                // of its own (see examples/common/testkit/db_busy_fixture.hpp's
                // own doc comment, which demonstrates this same raw pattern
                // with IMMEDIATE). DataMapper::Query issues its SQL through
                // exactly the connection DataMapper::Connection() exposes,
                // so the pin covers every query below.
                //
                // COMMIT (never ROLLBACK) on both paths: this transaction
                // only ever reads, so there is nothing to undo, and ending
                // it promptly is what matters -- a read transaction left
                // open holds a SHARED lock that blocks every writer on every
                // other connection until it closes.
                //
                // The `(void)` discards match the same raw-statement idiom
                // in examples/common/testkit/db_busy_fixture.hpp:
                // ExecuteDirect returns a [[nodiscard]] value that carries
                // nothing useful for a BEGIN/COMMIT (a genuine failure
                // throws, and is handled by the catch blocks below).
                (void) ::Lightweight::SqlStatement{workerMapper->Connection()}.ExecuteDirect("BEGIN DEFERRED");
                try {
                    resultJson = computeReportJson(workerMapper.Get(), ledgerId);
                } catch (...) {
                    (void) ::Lightweight::SqlStatement{workerMapper->Connection()}.ExecuteDirect("COMMIT");
                    throw;
                }
                (void) ::Lightweight::SqlStatement{workerMapper->Connection()}.ExecuteDirect("COMMIT");
            }
            // Written only after the read snapshot has been released, so this
            // connection is not simultaneously holding a read lock and asking
            // for a write one.
            finishReportJob(workerMapper.Get(), jobIdValue, ReportStatus::Done, std::move(resultJson));
        } catch (...) {
            // Catch-all, not just `const std::exception&`: an escaping
            // exception of any type must still leave the job in a terminal
            // state, or a poller would spin against Pending forever. The
            // pool's own worker loop would log-and-continue on such a throw,
            // but it cannot record Failed on this job's behalf.
            std::string detail = "unknown exception";
            try {
                throw;
            } catch (const std::exception& exc) {
                detail = exc.what();
            } catch (...) {
                // keep the placeholder
            }
            ::morph::log::logError("[ledger] SubmitReport worker failed: " + detail);
            try {
                auto workerMapper = ::Lightweight::GlobalDataMapperPool().Acquire();
                finishReportJob(workerMapper.Get(), jobIdValue, ReportStatus::Failed, std::nullopt);
            } catch (...) {
                // A failure recording the failure has nowhere left to go at
                // this rung's scope -- the job stays Pending, an accepted
                // limitation rather than a silently swallowed one (the same
                // shape bookmarks' own fetchMetadataOnce() catch block
                // settles for).
                ::morph::log::logError("[ledger] SubmitReport worker failed and could not record failure");
            }
        }
    });

    return jobId;
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
                                                const std::vector<db::AccountRecord>& legAccounts) {
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::TransactionJournalRecord journalRow;
    journalRow.description = description;
    journalRow.date = date.value.has_value() ? (*date.value).value.time_since_epoch().count() : 0;
    auto ledgerRows =
        mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *ledgerId).All();
    if (ledgerRows.empty()) {
        throw NotFound{"storeJournalImpl: no such ledger"};
    }
    journalRow.ledger = ledgerRows.front();
    mapper.Create(journalRow);
    for (std::size_t i = 0; i < legs.size(); ++i) {
        db::TransactionLegRecord legRow;
        legRow.journal = journalRow;
        legRow.account = legAccounts[i];
        legRow.amountNum = legs[i].amount.numerator;
        legRow.amountDen = legs[i].amount.denominator;
        legRow.amountDp = static_cast<int>(legs[i].amount.decimalPlaces.value);
        legRow.currencyCode = legAccounts[i].currencyCode.Value();
        const auto& foreignAmount = legs[i].foreignAmount;
        legRow.foreignAmountNum = foreignAmount ? std::optional{foreignAmount->numerator} : std::nullopt;
        legRow.foreignAmountDen = foreignAmount ? std::optional{foreignAmount->denominator} : std::nullopt;
        legRow.foreignAmountDp =
            foreignAmount ? std::optional{static_cast<int>(foreignAmount->decimalPlaces.value)} : std::nullopt;
        legRow.foreignCurrencyCode =
            legs[i].foreignCurrency ? std::optional{Lightweight::SqlAnsiString<3>{currencyToCode(*legs[i].foreignCurrency)}}
                                     : std::nullopt;
        mapper.Create(legRow);
    }
    auto result = buildLedgerState(mapper, ledgerId);
    sqlTxn.Commit();
    return result;
}

}  // namespace ledger
