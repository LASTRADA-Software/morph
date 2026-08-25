// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ledger/dto/account_dto.hpp"
#include "ledger/dto/import_dto.hpp"
#include "ledger/dto/report_dto.hpp"
#include "ledger/dto/transaction_dto.hpp"

// Hidden from moc, which mis-parses this block: moc's own parser treats the
// forward declaration's `}` as closing the *class* rather than the
// namespace, leaves `namespace Lightweight` open, and then believes every
// later namespace is nested inside it -- emitting
// `Lightweight::ledger::gui::LedgerPresenter` and failing to compile with
// "no member named 'ledger' in namespace 'Lightweight'". `Q_MOC_RUN` is
// defined only while moc parses, so the real compiler still sees the
// declaration and moc skips a declaration it has no use for (it never needs
// to know what `DataMapper` is).
//
// Latent until now rather than new: this is the first Q_OBJECT header in the
// rung to include this one, which is why no earlier task tripped it, and why
// kanban -- whose model header carries no such block -- never did.
#ifndef Q_MOC_RUN
namespace Lightweight {
class DataMapper;
}  // namespace Lightweight
#endif

namespace ledger::db {
/// @brief Forward-declared rather than included: `ledger_entity.hpp` pulls in
///        `<Lightweight/DataMapper/DataMapper.hpp>`, and this header is
///        reachable from `ledger_presenter.hpp`, a `Q_OBJECT` header moc must
///        parse. moc mis-parses Lightweight's namespace structure and then
///        believes every later namespace is nested inside `Lightweight`,
///        emitting `Lightweight::ledger::gui::LedgerPresenter` and failing to
///        compile.
///
///        Only a reference to `std::vector<AccountRecord>` appears below, so
///        a declaration suffices; `ledger_model.cpp` includes the real header
///        for the definitions. This also matches rung 4's shape --
///        `kanban/models/board_model.hpp` includes only core and dto headers
///        and never its own entity header, which is precisely why no kanban
///        presenter ever hit this.
struct AccountRecord;
}  // namespace ledger::db

namespace ledger {

/// @brief Accounts + transaction journal, keyed by `LedgerId` (design spec
///        §1) -- one ledger per book. Plain default-constructible, per
///        `polls::PollModel`'s own real shape: the key lives in each
///        action, not in the model instance. No private caching member is
///        needed here (unlike `PollModel`'s `_pollId`) because every
///        action this model implements carries its own `ledgerId`
///        explicitly.
class LedgerModel {
public:
    /// @brief This model's primary-key type, declared rather than deduced.
    ///
    ///        `morph::model::PrimaryKeyOf` prefers a nested alias over
    ///        anything a `BRIDGE_MODEL_KEY` line would deduce (model_key.hpp's
    ///        `KeyTypeOf`), which is the documented way to say the key type is
    ///        not simply the type of some action's field. It is not, here: six
    ///        of this model's seven keyed actions carry a `LedgerId`, but
    ///        `GetReportStatus` carries a `ReportJobId` (report_dto.hpp), so
    ///        no single strong id is *the* key type. Both unwrap to the same
    ///        `std::int64_t`, which is what the directory has always keyed on,
    ///        and declaring it keeps `primary()`/`instances()` returning a
    ///        plain integer instead of claiming a `LedgerId` that would be
    ///        wrong for a handler attached through `GetReportStatus`.
    ///
    ///        Each action still uses `BRIDGE_KEY_FROM` (below this class), so
    ///        the strong-id unwrapping is `morph::model::keyToString`'s and
    ///        not restated per action.
    using PrimaryKey = std::int64_t;

    /// @brief The ordinary shape: owns a one-thread `ThreadPoolExecutor` for
    ///        `execute(SubmitReport)`'s worker, per `_reportExecutor`'s own
    ///        default. This is the constructor the bridge registry uses --
    ///        every non-test caller reaches the model this way.
    LedgerModel() = default;

    /// @brief Substitutes a caller-supplied executor for the default worker
    ///        pool. `_reportExecutor`'s comment has always claimed a caller
    ///        could do this "without this class changing shape"; until this
    ///        constructor existed there was in fact no way to, which is why
    ///        `test_ledger_reports.cpp` had to poll a real pool with sleeps
    ///        (morph#161).
    ///
    ///        Nothing else changes: `execute(SubmitReport)` still posts the
    ///        same task, capturing the same plain values. Passing
    ///        `morph::ladder::testkit::StepExecutor` makes the worker run
    ///        exactly when the test says `runOne()`, so "submitted, still
    ///        Pending, the worker has not run yet" becomes an assertion
    ///        rather than a sample.
    /// @param reportExecutor Where `execute(SubmitReport)` posts the report
    ///        aggregation.
    /// @throws std::invalid_argument if @p reportExecutor is null -- checked
    ///         here rather than left to a null dereference inside
    ///         `execute(SubmitReport)`, which would fire on whichever call
    ///         first submits a report, arbitrarily far from the construction
    ///         that caused it.
    explicit LedgerModel(std::shared_ptr<::morph::exec::IExecutor> reportExecutor);

    /// @brief Creates an account in the ledger named by `action.ledgerId`.
    ///        The model's first keyed action -- see the `BRIDGE_KEY_FROM`
    ///        lines below this class, and `PrimaryKey` above for why the key
    ///        type is declared there rather than deduced by a
    ///        `BRIDGE_MODEL_KEY`.
    ///
    ///        Returns the freshly created account's info rather than
    ///        `void`: `ActionTraits<A>::Result` deduces from
    ///        `decltype(model.execute(action))`, and the registry runner
    ///        (`morph/core/registry.hpp`'s `ActionDispatcher::registerAction`)
    ///        unconditionally does `auto result = model.execute(action);` --
    ///        a `void`-returning `execute` fails to compile there for any
    ///        action registered via `BRIDGE_REGISTER_ACTION`. Matches
    ///        `bank::CustomerModel::execute(const OpenAccount&)`'s own
    ///        `dto::AccountInfo` return, the established convention for a
    ///        creating mutation in this codebase.
    /// @param action Ledger id, name, kind, and currency for the new account.
    /// @return The newly created account's info.
    AccountInfo execute(const OpenAccount& action);

    /// @brief Returns the full current state of the ledger named by
    ///        `action.ledgerId`.
    /// @param action The ledger id.
    /// @return Every account in the ledger, per the ladder-wide
    ///         full-rebuilt-state convention.
    GetLedgerResult execute(const GetLedger& action);

    /// @brief Records a multi-leg transaction against `action.ledgerId`'s
    ///        accounts, enforcing the per-currency zero-sum invariant
    ///        (design spec §1): each leg's amount is partitioned by the
    ///        currency of the account it names, and every partition must
    ///        sum to canonical zero, or `ZeroSumViolation` is thrown and no
    ///        row is written (the whole action runs inside one
    ///        `Lightweight::SqlTransaction`).
    /// @param action The ledger id, description, date, and legs to record.
    /// @return The full rebuilt ledger state, per the ladder-wide
    ///         full-rebuilt-state convention.
    GetLedgerResult execute(const StoreTransaction& action);

    /// @brief Links `action.accountId` to `action.categoryId`, the ordinary,
    ///        directly-dispatchable path. Journals unconditionally with an
    ///        empty `causalParentId` (the default -- this is not a cascaded
    ///        call). See `SetCategory`'s own doc comment
    ///        (`transaction_dto.hpp`) for why this overload exists at all
    ///        even though design spec §4 never has a client dispatch it
    ///        directly: `morph::journal::replay()` re-dispatches every
    ///        recorded entry by its registered action-type string, and an
    ///        unregistered `"SetCategory"` would make `replay()` throw the
    ///        moment it reaches a cascade-produced entry.
    /// @param action The account/category to link, plus the firing rule's
    ///        identity and version (unused by this path's own logic, but
    ///        part of the wire shape shared with the cascade path).
    /// @return An empty placeholder result.
    SetCategoryResult execute(const SetCategory& action);

    /// @brief Undoes the previously-recorded journal named by
    ///        `action.journalId` as a compensating action: inserts a second,
    ///        reversing `TransactionJournalRecord` whose legs are the
    ///        original legs' amounts negated via `Rational::operator-()
    ///        const`, with `causalParentId` pointing at the undone entry
    ///        (design spec §6). Never rewinds or erases the original entry --
    ///        `morph::journal::undoLast()` is not used here.
    /// @param action The ledger id (verified against the looked-up journal's
    ///        own ledger) and the journal id to reverse.
    /// @return The full rebuilt ledger state, per the ladder-wide
    ///         full-rebuilt-state convention.
    GetLedgerResult execute(const UndoTransaction& action);

    /// @brief Imports one CSV chunk (`date,description,account_id,amount`
    ///        rows, one header line skipped) into `action.ledgerId`,
    ///        posting a two-leg entry per row against that row's own
    ///        `account_id` and `action.counterAccountId` (design spec §8).
    ///        Two layers of dedup: an opId-keyed `ledger_imported_ops` row
    ///        is populated per chunk (Task 15's own scope-narrowing --
    ///        populated for future use, not yet read back for an early
    ///        return; see this method's own implementation comment) and a
    ///        content-hash check against `ledger_imported_txn_hashes`
    ///        skips (never throws) any row whose `description + date +
    ///        amount` hash was already imported into this ledger, so the
    ///        same statement re-uploaded under a different opId is still
    ///        only recorded once.
    /// @param action The ledger id, counter account, raw CSV chunk, and
    ///        this chunk's idempotency key.
    /// @return How many rows were newly imported vs. skipped as
    ///        content-hash duplicates.
    ImportResult execute(const ImportLedgerChunk& action);

    /// @brief Enqueues a report computation for `action.ledgerId` and returns
    ///        its job id immediately (design spec §9's submit->poll pair).
    ///        Creates one `db::ReportJobRecord` in `ReportStatus::Pending`,
    ///        then posts the actual aggregation to `_reportExecutor` -- see
    ///        that member's own comment for why this model owns an executor
    ///        at all, and `docs/findings/003-no-model-level-background-job-seam.md`
    ///        for the missing framework seam this works around.
    ///
    ///        The posted worker acquires its OWN pooled `DataMapper` and
    ///        captures only plain values: nothing from this call's stack
    ///        frame (its `mapper`, its `morph::session::Context*`) survives
    ///        into the worker, since `execute()` returns long before the
    ///        worker runs.
    /// @param action The ledger id, report kind, and JSON-encoded params.
    /// @return The freshly created job's id, immediately -- long before the
    ///         report itself is computed.
    ReportJobId execute(const SubmitReport& action);

    /// @brief Reads the current state of the job named by `action.jobId`.
    ///        A pure read with no session-scoped side effect, so (like
    ///        `execute(GetLedger)`) it carries no empty-principal gate and is
    ///        registered `Loggable::No`.
    /// @param action The job id to poll.
    /// @return The job's status, plus its serialized report body once the
    ///         status has reached `ReportStatus::Done`.
    GetReportStatusResult execute(const GetReportStatus& action);

    /// @brief Attaches a durable action log and this instance's stable
    ///        identity, so every subsequent mutating `execute()` records
    ///        a `morph::journal::LogEntry`. Model-level mirror of
    ///        `morph::model::detail::IModelHolder::attachActionLog` for a
    ///        plain-constructed instance that never goes through the
    ///        framework's registry/dispatcher path: a `LedgerModel` a unit
    ///        test (or any caller) constructs directly with
    ///        `ledger::LedgerModel model;` has no `IModelHolder` wrapping
    ///        it, so `model.execute(action)` calls `LedgerModel::execute`
    ///        straight, never touching `IModelHolder` or the dispatcher's
    ///        runner -- `recordIfAttached`'s auto-append never fires for
    ///        this path. `LedgerModel` therefore keeps its own
    ///        `shared_ptr<IActionLog>` and appends its own `LogEntry` at
    ///        the end of every successful mutating `execute()` (see
    ///        `logAction` below) -- functionally the same effect
    ///        `recordIfAttached` gives a holder-wrapped instance, achieved
    ///        without one.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every LogEntry this
    ///        instance produces (this rung's ledger id, as a string).
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

private:
    /// @brief Records @p action/@p result as a LogEntry if a log is
    ///        attached; no-op otherwise.
    /// @tparam Action Concrete action type.
    /// @tparam Result Concrete result type.
    /// @param action The executed action.
    /// @param result The action's result.
    /// @param causalParentId Empty (the default) for every ordinary call
    ///        site; Task 12's evaluateRules is the only caller that
    ///        passes a non-empty value.
    template <typename Action, typename Result>
    void logAction(const Action& action, const Result& result, std::string causalParentId = {}) const;

    /// @brief Shared mutation behind both `execute(SetCategory)` and the
    ///        cascade path inside `execute(StoreTransaction)`: links
    ///        `action.accountId` to `action.categoryId`
    ///        (`AccountRecord::category`, Task 10's schema addition). Holds
    ///        no logging of its own -- callers log once, either
    ///        unconditionally (the public overload) or with a
    ///        `causalParentId` (the cascade path) -- never both for the
    ///        same firing.
    ///
    ///        Takes @p mapper by reference rather than opening its own --
    ///        the cascade call site is already inside `execute
    ///        (StoreTransaction)`'s own `Lightweight::SqlTransaction`, and
    ///        this mutation must commit atomically with the triggering
    ///        journal+legs insert (never through a second, separate
    ///        connection, which would not see the in-flight transaction's
    ///        uncommitted rows and would not be covered by its commit/
    ///        rollback). `execute(SetCategory)`'s own, non-cascaded call
    ///        site passes its own freshly opened mapper for the same reason
    ///        `buildLedgerState` takes one instead of opening its own.
    /// @param mapper The data mapper to mutate through -- the caller's own,
    ///        already-open connection/transaction.
    /// @param action The account/category to link.
    static void setCategoryImpl(Lightweight::DataMapper& mapper, const SetCategory& action);

    /// @brief Shared mutation behind `execute(UndoTransaction)`'s reversal
    ///        insert: opens its own `Lightweight::SqlTransaction`, creates
    ///        one `TransactionJournalRecord` plus one `TransactionLegRecord`
    ///        per @p legs/@p legAccounts pair, rebuilds the ledger's state via
    ///        `buildLedgerState`, commits, and returns that state. Mirrors
    ///        `execute(StoreTransaction)`'s own journal-insert + leg-insert +
    ///        `buildLedgerState` rebuild verbatim, but holds none of that
    ///        method's opId-ledger-write or cascade-evaluation blocks: a
    ///        reversal has no `opId` and never re-fires rules against its own
    ///        synthetic description, so this helper's transaction can close
    ///        immediately after the rebuild. `execute(StoreTransaction)`
    ///        keeps its own inline insert logic rather than calling this
    ///        helper, since threading its opId/cascade logic through this
    ///        signature would be more churn than sharing is worth -- this
    ///        helper exists solely for `execute(UndoTransaction)` to call,
    ///        mirroring `setCategoryImpl`'s own role as a single-caller
    ///        extraction, not a refactor of `execute(StoreTransaction)`.
    ///
    ///        Does not re-run `execute(StoreTransaction)`'s zero-sum
    ///        partitioning loop -- it trusts its caller already knows the
    ///        legs it's inserting are zero-sum (negating every leg of an
    ///        already-zero-sum set is itself zero-sum). Any future caller
    ///        that cannot make that guarantee must validate before calling
    ///        this helper.
    /// @param mapper The data mapper to mutate through -- opens its own
    ///        `Lightweight::SqlTransaction` on this mapper's connection.
    /// @param ledgerId The ledger the new journal belongs to.
    /// @param description The new journal's description.
    /// @param date The new journal's client-observable "when did this
    ///        happen" timestamp.
    /// @param legs The new journal's legs.
    /// @param legAccounts Each leg's own account row, positionally aligned
    ///        with @p legs (one lookup per leg's account, same shape as
    ///        `execute(StoreTransaction)`'s own `legAccounts`).
    /// @param causalParentId What this journal exists because of, in the same
    ///        `"transactionJournal:<id>"` shape `logAction` already uses, or
    ///        `std::nullopt` (the default) for an ordinary journal that
    ///        stands on its own. `execute(UndoTransaction)` is the one caller
    ///        that passes it, naming the entry being reversed -- which is
    ///        what makes a second reversal of that entry detectable.
    /// @return The full rebuilt ledger state, per the ladder-wide
    ///         full-rebuilt-state convention.
    [[nodiscard]] GetLedgerResult storeJournalImpl(Lightweight::DataMapper& mapper, const LedgerId& ledgerId,
                                                   const std::string& description, const morph::time::Timestamp& date,
                                                   const std::vector<TransactionLeg>& legs,
                                                   const std::vector<db::AccountRecord>& legAccounts,
                                                   std::optional<std::string> causalParentId = std::nullopt);

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;

    /// @brief Where `execute(SubmitReport)` posts the actual report
    ///        computation. Infrastructure, not model state -- it holds no
    ///        per-ledger data and does not violate this class's own "the key
    ///        lives in each action, not the instance" rule; every posted task
    ///        carries the ids it needs by value.
    ///
    ///        A member at all because no framework-level seam exists for a
    ///        model's own `execute()` to post background work: every other
    ///        "background job" in this codebase (bookmarks' metadata fetch)
    ///        lives at the App/Bridge/RemoteServer layer and re-enters its
    ///        model as an ordinary client dispatch, a layer this rung simply
    ///        does not have. Filed as
    ///        `docs/findings/003-no-model-level-background-job-seam.md`.
    ///
    ///        Declared LAST among the data members so it is destroyed FIRST:
    ///        `~ThreadPoolExecutor()` joins its workers, so by the time any
    ///        other member is torn down no posted task can still be running.
    ///        A `shared_ptr<IExecutor>` rather than a `ThreadPoolExecutor`
    ///        by value so a caller can substitute a different executor
    ///        (a `MainThreadExecutor`, a deterministic double) without this
    ///        class changing shape -- reachable through the
    ///        `explicit LedgerModel(std::shared_ptr<IExecutor>)` constructor
    ///        above, which is what `test_ledger_reports.cpp` uses to drive
    ///        the worker deterministically.
    std::shared_ptr<::morph::exec::IExecutor> _reportExecutor = std::make_shared<::morph::exec::ThreadPoolExecutor>(1);
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::LedgerModel, "LedgerModel")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::GetLedger, "GetLedger", ::morph::model::Loggable::No)

// `BRIDGE_KEY_FROM` per keyed action, and no `BRIDGE_MODEL_KEY` at all: the
// model's key *type* is declared in its own class body (`LedgerModel::
// PrimaryKey`, see that alias's comment for why it stays the raw scalar), and
// a nested alias wins over any deduced type (model_key.hpp's `KeyTypeOf`).
// Each of these lines records only that the action carries the key, and
// routes the value through `morph::model::keyToString`.
//
// Until morph#183 these were seven hand-written `ActionKeyTraits`
// specialisations plus a `ModelKeyTraits<LedgerModel>`, because
// `morph::model::ModelKey` admitted only `std::integral`/`std::string` and
// `ledger::LedgerId` (like every `LEDGER_DEFINE_STRONG_ID` type, types.hpp)
// wraps a `std::optional<std::int64_t>`. morph#163 widened the concept to
// admit a strong id, so the macros work on these fields now -- and each
// hand-written body's `*action.ledgerId` is gone with them. That dereference
// was `operator*` on a possibly-disengaged `std::optional`: undefined
// behaviour for an action carrying an empty id, which handed back whatever
// the union held and routed the caller to an arbitrary instance.
// `keyToString` refuses an empty strong id instead, and
// `BridgeHandler::execute`'s `catch (...)` around key extraction turns the
// refusal into a rejected `Completion`.
BRIDGE_KEY_FROM(ledger::OpenAccount, &ledger::OpenAccount::ledgerId);
BRIDGE_KEY_FROM(ledger::GetLedger, &ledger::GetLedger::ledgerId);

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::StoreTransaction, "StoreTransaction")

BRIDGE_KEY_FROM(ledger::StoreTransaction, &ledger::StoreTransaction::ledgerId);

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::SetCategory, "SetCategory")

// SetCategory carries accountId and categoryId -- two co-equal ids and no
// single natural "the" key, same shape as BudgetModel's own
// LinkAccountToCategory (see that model's own comment on this exact
// situation). model_key.hpp's ActionKeyTraits primary template already
// defaults to hasKey = false, so this deliberately gets no specialization
// here: it is dispatched keyless, exactly like LinkAccountToCategory.

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::UndoTransaction, "UndoTransaction")

BRIDGE_KEY_FROM(ledger::UndoTransaction, &ledger::UndoTransaction::ledgerId);

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::ImportLedgerChunk, "ImportLedgerChunk")

BRIDGE_KEY_FROM(ledger::ImportLedgerChunk, &ledger::ImportLedgerChunk::ledgerId);

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::SubmitReport, "SubmitReport")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::GetReportStatus, "GetReportStatus", ::morph::model::Loggable::No)

BRIDGE_KEY_FROM(ledger::SubmitReport, &ledger::SubmitReport::ledgerId);

// GetReportStatus carries no ledgerId, only jobId -- resolving its key by
// looking up the job row's own ledger_id would repeat Task 14's already-
// rejected DB-lookup-inside-key() pattern, so it keys directly on jobId
// itself. This is the action that keeps `LedgerModel::PrimaryKey` a raw
// `std::int64_t`: a ReportJobId's underlying integer is just as valid a value
// for that key type as a LedgerId's, but the two are different *types*, so
// there is no single strong id `BRIDGE_MODEL_KEY` could deduce that would be
// honest for both. Nothing requires every keyed action on one model to key by
// the same semantic field, only that the key type matches.
BRIDGE_KEY_FROM(ledger::GetReportStatus, &ledger::GetReportStatus::jobId);
