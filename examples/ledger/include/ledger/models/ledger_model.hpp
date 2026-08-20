// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/db/ledger_entity.hpp"
#include "ledger/dto/account_dto.hpp"
#include "ledger/dto/import_dto.hpp"
#include "ledger/dto/transaction_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Lightweight {
class DataMapper;
}  // namespace Lightweight

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
    /// @brief Creates an account in the ledger named by `action.ledgerId`.
    ///        The model's first keyed action -- see the hand-written
    ///        `ModelKeyTraits`/`ActionKeyTraits` specialisations below this
    ///        class (not the `BRIDGE_MODEL_KEY` macro -- see their own
    ///        comment for why).
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
    /// @return The full rebuilt ledger state, per the ladder-wide
    ///         full-rebuilt-state convention.
    [[nodiscard]] GetLedgerResult storeJournalImpl(Lightweight::DataMapper& mapper, const LedgerId& ledgerId,
                                                     const std::string& description, const morph::time::Timestamp& date,
                                                     const std::vector<TransactionLeg>& legs,
                                                     const std::vector<db::AccountRecord>& legAccounts);

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::LedgerModel, "LedgerModel")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::GetLedger, "GetLedger", ::morph::model::Loggable::No)

// Hand-written ModelKeyTraits/ActionKeyTraits instead of BRIDGE_MODEL_KEY/
// BRIDGE_KEY_FROM: those macros route the key through
// morph::model::keyToString<K>, which is constrained by the
// morph::model::ModelKey concept (std::integral or std::string only --
// model_key.hpp). ledger::LedgerId (like every LEDGER_DEFINE_STRONG_ID type,
// types.hpp) wraps std::optional<std::int64_t>, so it satisfies neither arm
// and BRIDGE_MODEL_KEY(LedgerModel, OpenAccount, &OpenAccount::ledgerId)
// fails to compile (confirmed by a real build: "ledger::LedgerId does not
// satisfy ModelKey"). No existing rung's keyed model (bank::AccountModel/
// CustomerModel, polls::PollModel) hits this: their key fields are plain
// std::int64_t/std::string, never a strong-id struct. Rather than widen
// morph::model::ModelKey itself (a core, already-shipped framework concept
// also load-bearing for bank/polls -- out of this ledger-only task's scope),
// PrimaryKey is declared std::int64_t directly here and key() unwraps
// LedgerId's payload by hand. This is the plain, non-macro customisation
// point model_key.hpp's own doc comments already anticipate ("Specialise
// via BRIDGE_KEY_FROM ... or by hand").
template <>
struct morph::model::ActionKeyTraits<ledger::OpenAccount> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::OpenAccount& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
template <>
struct morph::model::ModelKeyTraits<ledger::LedgerModel> {
    using PrimaryKey = std::int64_t;
};
template <>
struct morph::model::ActionKeyTraits<ledger::GetLedger> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::GetLedger& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::StoreTransaction, "StoreTransaction")

template <>
struct morph::model::ActionKeyTraits<ledger::StoreTransaction> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::StoreTransaction& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::SetCategory, "SetCategory")

// SetCategory carries accountId and categoryId -- two co-equal ids and no
// single natural "the" key, same shape as BudgetModel's own
// LinkAccountToCategory (see that model's own comment on this exact
// situation). model_key.hpp's ActionKeyTraits primary template already
// defaults to hasKey = false, so this deliberately gets no specialization
// here: it is dispatched keyless, exactly like LinkAccountToCategory.

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::UndoTransaction, "UndoTransaction")

template <>
struct morph::model::ActionKeyTraits<ledger::UndoTransaction> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::UndoTransaction& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};

BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::ImportLedgerChunk, "ImportLedgerChunk")

template <>
struct morph::model::ActionKeyTraits<ledger::ImportLedgerChunk> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::ImportLedgerChunk& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
