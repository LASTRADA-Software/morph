// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <string>

#include "bank/dto/account_dto.hpp"

/// @file
/// The Customer model — the *per-owner* half of what used to be one
/// `AccountModel` doing two unrelated jobs.
///
/// `ListAccounts` and `OpenAccount` were never account-scoped operations: they
/// are scoped by owner, which is exactly why both DTOs carry an `owner` field
/// while `GetAccount`/`CloseAccount` carry an account id. Splitting the model by
/// the entity it is actually about is what gives each half a primary key that
/// identifies something.

namespace bank {

/// @brief One customer: lists and opens the accounts they own.
///
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning one for its own lifetime.
class CustomerModel {
public:
    /// @brief Opens a new account for the requested (or session) owner.
    dto::AccountInfo execute(const dto::OpenAccount& action);

    /// @brief Lists accounts owned by the requested (or session) owner.
    dto::AccountList execute(const dto::ListAccounts& action);
};

}  // namespace bank

using bank::CustomerModel;
using bank::dto::ListAccounts;
using bank::dto::OpenAccount;

BRIDGE_REGISTER_MODEL(CustomerModel, "CustomerModel")
BRIDGE_REGISTER_ACTION(CustomerModel, OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(CustomerModel, ListAccounts, "ListAccounts", ::morph::model::Loggable::No)

// Keyed by owner. An empty `owner` means "the session principal", which is not
// a key the directory can share on, so such a call simply runs on whatever
// instance the handler already holds.
BRIDGE_MODEL_KEY(CustomerModel, ListAccounts, &ListAccounts::owner);
BRIDGE_KEY_FROM(OpenAccount, &OpenAccount::owner);
