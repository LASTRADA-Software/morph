// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session.hpp>
#include <string>

#include "bank/core/errors.hpp"

/// @file
/// Helpers for reading the authenticated principal from the morph session
/// context. The bridge attaches its default session (set once at login via
/// `Bridge::setDefaultSession`) to every call, and the model reads it here
/// without changing its `execute()` signatures.

namespace bank {

/// @brief Returns the current session principal, or empty if none is attached.
[[nodiscard]] inline std::string sessionPrincipal() {
    if (const auto* ctx = morph::session::current(); ctx != nullptr) {
        return ctx->principal;
    }
    return {};
}

/// @brief Resolves the owner an owner-named action is scoped to: the session
///        principal, always.
///
/// Ten actions across eight models carry an `owner` field on the wire
/// (`ListAccounts`, `ListCards`, `ListPayees`, `ListPayments`, `ListLoans`,
/// `ListBudgets`, `ListNotifications`, `GenerateStatement`, `OpenAccount` and
/// `MarkAllRead`). The field is **verified, not trusted**: it may name the
/// caller, in which case it is redundant, or it may be left empty, in which
/// case the session principal stands in. Naming anybody else is refused.
///
/// The alternative — ignoring `action.owner` outright — was rejected because
/// the field is load-bearing on the wire: it is `CustomerModel`'s bridge
/// routing key (`BRIDGE_MODEL_KEY(CustomerModel, ListAccounts,
/// &ListAccounts::owner)`), so a request naming another customer would be
/// routed to that customer's model instance and then quietly served the
/// caller's own rows. Refusing says what happened instead, and matches
/// `db::loadOwned`, which is how the id-addressed half of the same models has
/// always enforced ownership.
///
/// An empty session principal matches no name at all, so an anonymous caller
/// naming a real customer is refused here rather than served. An anonymous
/// caller naming *nobody* still gets the empty string back: each call site
/// carries its own `owner.empty()` guard and refuses with a message naming
/// what it was about to do.
///
/// @throws Unauthorized if @p explicitOwner is non-empty and is not the
///         session principal.
[[nodiscard]] inline std::string resolveOwner(const std::string& explicitOwner) {
    std::string principal = sessionPrincipal();
    if (!explicitOwner.empty() && explicitOwner != principal) {
        throw Unauthorized{"owner does not match the session principal"};
    }
    return principal;
}

}  // namespace bank
