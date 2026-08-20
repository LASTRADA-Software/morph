// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/dto/account_dto.hpp"
#include "ledger/dto/transaction_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>

#include <memory>
#include <optional>
#include <string>

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
