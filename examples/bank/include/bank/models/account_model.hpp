// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>

#include "bank/db/entities.hpp"
#include "bank/dto/account_dto.hpp"

/// @file
/// The Account model — one customer account, **held in memory** for the lifetime
/// of the instance.
///
/// This is the shape morph is designed around and the reason the framework runs
/// each model on its own strand: the instance owns mutable state, so an
/// unsynchronised read-modify-write of `_row` is correct precisely because no
/// two actions on the same instance ever overlap. A stateless model (as every
/// bank model used to be) makes that strand protect nothing.
///
/// The model declares `PrimaryKey`, so morph keys instances by account id: two
/// `BridgeHandler<AccountModel, AllowShared>` handlers naming the same account —
/// in one GUI or in two clients — reach one instance and see one balance.
/// SQLite stays authoritative; the instance is a cache with identity, hydrated
/// on first use and written through on every mutation.

namespace bank {

/// @brief One customer account: its row, cached, with reads served from memory.
///
/// Holds no database state itself beyond `_row`: `hydrate()` and
/// `execute(CloseAccount&)` each acquire a `Lightweight::GlobalDataMapperPool()`
/// connection for their own duration and return it before returning, rather
/// than owning one for the whole model instance's lifetime.
class AccountModel {
public:
    /// @brief Returns the account, hydrating from SQLite only when needed.
    dto::AccountInfo execute(const dto::GetAccount& action);

    /// @brief Closes a zero-balance account; returns ok/message.
    dto::CommandResult execute(const dto::CloseAccount& action);

private:
    /// @brief Loads `_row` for @p accountId if it is absent, stale, or for another account.
    void hydrate(std::int64_t accountId);

    db::AccountRecord _row{};      ///< the account itself — in memory, not re-queried
    std::string _owner;            ///< resolved owner username for `_row`
    std::int64_t _loadedId = 0;    ///< which account `_row` holds; 0 = none
    std::uint64_t _seenVersion = 0;  ///< row version `_row` was hydrated at
};

}  // namespace bank

// ─── morph registration ──────────────────────────────────────────────────────
// Registration lives in the header (not the .cpp) on purpose: the
// BRIDGE_REGISTER_ACTION macro specialises `morph::model::ActionTraits<Action>`,
// and every translation unit that calls `handler.execute(Action{...})` needs
// that specialisation visible to deduce the result type.
using bank::AccountModel;
using bank::dto::CloseAccount;
using bank::dto::GetAccount;

BRIDGE_REGISTER_MODEL(AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(AccountModel, CloseAccount, "CloseAccount")

// Both actions name the account they act on, so a shared handler attaches (or
// re-points) to that account's instance on the way through.
BRIDGE_MODEL_KEY(AccountModel, GetAccount, &GetAccount::id);
BRIDGE_KEY_FROM(CloseAccount, &CloseAccount::id);
