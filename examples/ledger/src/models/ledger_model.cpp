// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/core/units.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>

#include <glaze/glaze.hpp>

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
