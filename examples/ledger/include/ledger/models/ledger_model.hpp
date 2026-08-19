// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/dto/account_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>

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
