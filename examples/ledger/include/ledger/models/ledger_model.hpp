// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
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

    /// @brief Creates a book and returns its id -- the bootstrap every other
    ///        action on this model depends on (morph#361).
    ///
    ///        The one action here that carries no `ledgerId`, because it is
    ///        the action that produces one. It is therefore dispatched
    ///        **keyless** (no `BRIDGE_KEY_FROM` line below this class) and
    ///        runs on whichever worker the dispatcher picks rather than on a
    ///        per-book strand: there is no book yet to have a strand. That is
    ///        `polls::PollModel`'s exact shape -- a model keyed by `pollId`
    ///        whose `CreatePoll` is registered on it without a key -- and the
    ///        reason no separate admin model was introduced for this the way
    ///        `kanban::ProjectAdminModel` holds `CreateProject`.
    ///
    ///        Two consequences of running off-strand, both benign here: the
    ///        insert is a single `DataMapper::Create` into `ledgers`, which
    ///        touches no row any other action can be mid-way through (nothing
    ///        can name a book that does not exist yet); and the `LogEntry` it
    ///        appends carries whatever `entityKey` `attachActionLog` was
    ///        given, since the id the entry is *about* is only known once the
    ///        row is written.
    /// @param action The new book's name.
    /// @return The new book's id.
    /// @throws EmptyPrincipalError if no authenticated principal is in scope.
    /// @throws ValidationError if the name is empty or longer than
    ///         `kMaxLedgerNameBytes`.
    CreateLedgerResult execute(const CreateLedger& action);

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

    /// @brief Lists the journal entries `action.ledgerId` recorded during
    ///        `action.month`, oldest first, each carrying the `JournalId` a
    ///        client needs to name it (morph#428).
    ///
    ///        The read that makes `UndoTransaction` -- and the Undo control
    ///        `gui/qml/LedgerView.qml` ships -- drivable at all: before this,
    ///        `JournalId` appeared in exactly one DTO field in the rung, and
    ///        that field was `UndoTransaction`'s own input, so a client could
    ///        only ever pass an id it had guessed.
    ///
    ///        Gated by `db::requireOwnedBook` exactly as `GetLedger`,
    ///        `GetBudgetReport` and `GetReportStatus` are (morph#382): a
    ///        listing of a book's entries is precisely the kind of read that
    ///        gate exists for. Carries no `EmptyPrincipalError` gate, for the
    ///        same reason `execute(GetLedger)` does not -- an empty principal
    ///        never matches a recorded owner, so it is refused on an owned
    ///        book and admitted only on an unowned one.
    ///
    ///        The month is a half-open UTC `[start, end)` millisecond range
    ///        over `TransactionJournalRecord::date`, via the shared
    ///        `monthRangeMs` (`ledger/core/time_util.hpp`) that
    ///        `BudgetModel::execute(const GetBudgetReport&)` also uses, so
    ///        the two agree on what "August" means.
    /// @param action The ledger id and the `"YYYY-MM"` month to list.
    /// @return That month's entries, each with its id, description, date and
    ///         legs; an empty list for a month with nothing in it.
    /// @throws ValidationError if the ledger id is unengaged or the month is
    ///         not a well-formed `"YYYY-MM"`.
    /// @throws NotFound if no book has that id.
    /// @throws Forbidden if the book belongs to a different principal.
    ListTransactionsResult execute(const ListTransactions& action);

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
    ///
    ///        Each row's two legs go through `storeJournalImpl`, which
    ///        enforces the same per-currency zero-sum invariant
    ///        `execute(StoreTransaction)` does (see that method's own doc
    ///        comment): a row whose account and `action.counterAccountId`
    ///        hold different currencies is not balanced in either currency,
    ///        so it is rejected rather than posted as two unbalanced
    ///        single-legged postings.
    /// @param action The ledger id, counter account, raw CSV chunk, and
    ///        this chunk's idempotency key.
    /// @return How many rows were newly imported vs. skipped as
    ///        content-hash duplicates.
    /// @throws ZeroSumViolation When a row's account and
    ///         `action.counterAccountId` do not share a currency (or, more
    ///         generally, when a row's two legs do not sum to zero within
    ///         some currency).
    ImportResult execute(const ImportLedgerChunk& action);

    /// @brief Enqueues a report computation for `action.ledgerId` and returns
    ///        its job id immediately (design spec §9's submit->poll pair).
    ///
    ///        Writes one `db::ReportJobRecord` in `ReportStatus::Pending`,
    ///        carrying `action.kind` and `action.params` verbatim, and that
    ///        is the whole of it: nothing is scheduled, no thread is started,
    ///        and this model owns no executor. Draining that row is
    ///        `ledger::app::App`'s report runner's job, which dispatches
    ///        `RunReportJob` back at this model as an ordinary client action
    ///        (morph#160) -- the same shape bookmarks' metadata worker uses,
    ///        and the reason this header no longer includes
    ///        `<morph/core/executor.hpp>` at all.
    /// @param action The ledger id, report kind, and JSON-encoded params.
    /// @return The freshly created job's id, immediately -- long before the
    ///         report itself is computed.
    ReportJobId execute(const SubmitReport& action);

    /// @brief Computes the job named by `action.jobId` and settles its row.
    ///
    ///        The business half of the report job, and the reason the
    ///        aggregation did not move out of this class along with the
    ///        scheduling: a monthly statement is domain logic, and domain
    ///        logic lives in a model (`examples/IMPLEMENTATION.md` rule 1).
    ///        Only *when* it runs moved out.
    ///
    ///        Runs on the strand for `action.ledgerId` -- the same strand
    ///        every other action against that ledger runs on -- so the
    ///        aggregation can no longer interleave with a concurrent
    ///        `StoreTransaction` against the same book, which the previous
    ///        off-strand worker could. The `BEGIN DEFERRED` read snapshot is
    ///        kept regardless: `BudgetModel` writes to the same database from
    ///        a strand of its own, and that one the ledger's strand says
    ///        nothing about.
    ///
    ///        Idempotent: a job already in a terminal status is left
    ///        untouched and its status returned. A failing aggregation is
    ///        recorded as `ReportStatus::Failed` rather than thrown, so a
    ///        poller never spins against `Pending` forever; the dispatch
    ///        itself only throws for the cases that are the *caller's* fault
    ///        (bad principal, unengaged ids, no such job).
    /// @param action The job to run, plus the ledger whose strand to run it
    ///        on.
    /// @return The job's terminal status -- never `ReportStatus::Pending`.
    /// @throws Forbidden if the dispatching principal is not
    ///         `kReportRunnerPrincipal`.
    /// @throws ValidationError if either id is unengaged.
    /// @throws NotFound if no job row has that id.
    RunReportJobResult execute(const RunReportJob& action);

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

    /// @brief Records a rejected @p action as a `LogEntry` with
    ///        `Outcome::Failed` and @p error, if a log is attached; no-op
    ///        otherwise. The refused-attempt counterpart to `logAction`
    ///        above: every `execute()` overload that mutates state catches
    ///        its own `LedgerError` hierarchy around the whole body and
    ///        calls this before rethrowing, so a `ZeroSumViolation`,
    ///        `AlreadyReversed`, `VersionConflict`, or any other refusal
    ///        leaves the same audit trace a success does -- see
    ///        `lims::SelfJournal::recordFailure` for the identical rationale
    ///        this mirrors (`include/lims/core/self_journal.hpp`).
    /// @tparam Action Concrete action type.
    /// @param action The rejected action.
    /// @param error The rejecting exception's `what()`.
    template <typename Action>
    void logFailure(const Action& action, const std::string& error) const;

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
    ///        helper is called by `execute(UndoTransaction)` (once, for the
    ///        reversing entry) and by `execute(ImportLedgerChunk)` (once per
    ///        CSV row), never by `execute(StoreTransaction)`.
    ///
    ///        Runs `execute(StoreTransaction)`'s own per-currency zero-sum
    ///        check itself (`checkZeroSumByCurrency` in
    ///        `src/models/ledger_model.cpp`), on the restated amounts below,
    ///        before opening any transaction. The check lives here rather
    ///        than in each caller precisely because a caller can otherwise
    ///        skip it: `execute(ImportLedgerChunk)` did, for as long as this
    ///        helper only trusted its caller to have validated already (the
    ///        two-leg entry a CSV row posts can name any two accounts, and
    ///        nothing about parsing a row constrains them to share a
    ///        currency). `execute(UndoTransaction)`'s reversal legs pass the
    ///        check trivially -- negating every leg of an already-zero-sum
    ///        set is itself zero-sum in every currency the original entry
    ///        touched -- so hoisting the check here changes nothing for that
    ///        caller.
    ///
    ///        Also restates every leg onto its own account currency's scale,
    ///        exactly as `execute(StoreTransaction)` does, because that is a
    ///        property of the rows this method writes rather than of the
    ///        caller's own checking -- see `restateLegAmounts` in
    ///        `src/models/ledger_model.cpp`. A leg that is not a whole
    ///        number of its currency's minor units is rejected with
    ///        `ValidationError` before any transaction is opened.
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
    /// @throws ZeroSumViolation When @p legs, restated onto their own account
    ///         currencies, do not sum to zero within some currency.
    [[nodiscard]] GetLedgerResult storeJournalImpl(Lightweight::DataMapper& mapper, const LedgerId& ledgerId,
                                                   const std::string& description, const morph::time::Timestamp& date,
                                                   const std::vector<TransactionLeg>& legs,
                                                   const std::vector<db::AccountRecord>& legAccounts,
                                                   std::optional<std::string> causalParentId = std::nullopt);

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::LedgerModel, "LedgerModel")

// Deliberately gets no `BRIDGE_KEY_FROM`: `CreateLedger` carries no id at all
// -- it is what mints one -- so `ActionKeyTraits`'s primary template's
// `hasKey = false` default is the correct answer, and it is dispatched
// keyless exactly like `SetCategory` below. See this action's doc comment in
// the class body for why that is safe.
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::CreateLedger, "CreateLedger")

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

// A pure read, so `Loggable::No` exactly like `GetLedger` above: the action
// log is an audit trail of what changed the book, and listing it changes
// nothing. Keyed on `ledgerId` like every other action that names a book, so
// it runs on that book's own strand.
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::ListTransactions, "ListTransactions",
                       ::morph::model::Loggable::No)

BRIDGE_KEY_FROM(ledger::ListTransactions, &ledger::ListTransactions::ledgerId);

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
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::RunReportJob, "RunReportJob")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::GetReportStatus, "GetReportStatus", ::morph::model::Loggable::No)

BRIDGE_KEY_FROM(ledger::SubmitReport, &ledger::SubmitReport::ledgerId);

// RunReportJob keys on its `ledgerId`, not its `jobId`, and the difference
// matters: keying on the ledger puts the aggregation on the *same* strand as
// every other action against that book, which is what makes a report and a
// concurrent StoreTransaction against one ledger serialise instead of race.
// Keying on jobId would give every job a strand of its own -- more
// parallelism, and exactly the interleaving with mid-action ledger state the
// off-strand worker this action replaced was criticised for. It is also why
// the action carries a `ledgerId` at all rather than looking the job's own
// ledger up here (see `BRIDGE_KEY_FROM(ledger::GetReportStatus, ...)` below on
// why key() does not touch the database).
BRIDGE_KEY_FROM(ledger::RunReportJob, &ledger::RunReportJob::ledgerId);

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
