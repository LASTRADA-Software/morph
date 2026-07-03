// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/registry.hpp>

#include "bank/db/db_model.hpp"
#include "bank/dto/account_dto.hpp"

/// @file
/// The Account model. A plain, single-threaded C++ class — morph runs each
/// instance on its own strand, so the model never deals with concurrency. It
/// owns one Lightweight `DataMapper` (lazily opened on first use, on the strand
/// thread) and translates between wire DTOs and the `AccountRecord` entity.

namespace bank {

/// @brief Opens, lists, inspects, and closes customer accounts.
class AccountModel : private db::WithMapper {
public:
    /// @brief Opens a new account; returns the freshly created account.
    dto::AccountInfo execute(const dto::OpenAccount& action);

    /// @brief Lists accounts owned by the requested (or session) owner.
    dto::AccountList execute(const dto::ListAccounts& action);

    /// @brief Returns one account by id, or throws `NotFound`.
    dto::AccountInfo execute(const dto::GetAccount& action);

    /// @brief Closes a zero-balance account; returns ok/message.
    dto::CommandResult execute(const dto::CloseAccount& action);
};

}  // namespace bank

// ─── morph registration ──────────────────────────────────────────────────────
// Registration lives in the header (not the .cpp) on purpose: the
// BRIDGE_REGISTER_ACTION macro specialises `morph::model::ActionTraits<Action>`,
// and every translation unit that calls `handler.execute(Action{...})` needs
// that specialisation visible to deduce the result type. The macros must sit at
// global scope and token-paste unqualified identifiers, so the types are pulled
// in with using-declarations first. The static registration runs once per
// including TU; the underlying registry assignment is idempotent.
using bank::AccountModel;
using bank::dto::CloseAccount;
using bank::dto::GetAccount;
using bank::dto::ListAccounts;
using bank::dto::OpenAccount;

BRIDGE_REGISTER_MODEL(AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(AccountModel, OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(AccountModel, ListAccounts, "ListAccounts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(AccountModel, CloseAccount, "CloseAccount")
