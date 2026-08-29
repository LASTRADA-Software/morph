// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <string_view>

#include "crm/core/types.hpp"

/// @file
/// Role-based per-entity authorization (README build order §5).
///
/// The account is crm's authorization boundary — a "team" manages an
/// account, and everything hanging off it (contacts, opportunities, quotes)
/// inherits that account's role table, rather than each entity getting its
/// own. This is why `requireRole` lives here as one shared helper instead of
/// kanban's per-model-duplicated `requireRole`/`requireRoleOn`
/// (`kanban::BoardModel`'s own doc comment: "hand-duplicated... design spec
/// §3") — kanban's per-entity key (`ProjectId`) is one-model-only; crm's
/// (`AccountId`) is a shared key four different models resolve their own
/// records back to, so a single shared helper avoids four copies of the
/// identical DB lookup + comparison.

namespace crm {

/// @brief An account-scoped role, ordered `Viewer < Member < Manager` —
///        identical linear-hierarchy shape to `kanban::Role`.
enum class Role : std::uint8_t {
    Viewer,   ///< Read-only.
    Member,   ///< May create/edit ordinary records.
    Manager,  ///< May also edit Manager-restricted fields (e.g. Account::industry) and manage roles.
};

/// @brief Renders @p role as the string stored in `crm_account_roles.role`.
[[nodiscard]] constexpr std::string_view roleToString(Role role) noexcept {
    switch (role) {
        case Role::Viewer:
            return "viewer";
        case Role::Member:
            return "member";
        case Role::Manager:
            return "manager";
    }
    return "viewer";
}

/// @brief Parses a stored role string back to `Role`.
///
/// Unrecognized text defaults to `Viewer` — the least-privileged value,
/// matching `kanban::roleFromString`'s deliberate fail-safe-closed default:
/// a corrupted or unexpected role string must never be interpreted as more
/// privilege than the caller actually has.
/// @param text The stored role string.
/// @return The parsed role, or `Role::Viewer` if unrecognized.
[[nodiscard]] constexpr Role roleFromString(std::string_view text) noexcept {
    if (text == "manager") {
        return Role::Manager;
    }
    if (text == "member") {
        return Role::Member;
    }
    return Role::Viewer;
}

/// @brief Throws `Forbidden` unless the calling principal's role on
///        @p accountId is at least @p minimum.
///
/// The one shared per-entity gate every mutating action on
/// Account/Contact/Opportunity/Quote runs before touching a row — same
/// ordering discipline as `kanban::BoardModel::requireRoleOn` (checks the
/// role unconditionally, before any other guard): a caller with no role at
/// all, or a role below `minimum`, never learns anything about whether the
/// account/record even exists.
///
/// Fail-open convention, not fail-closed on a missing table row: an account
/// with **no** `crm_account_roles` rows at all (the common case for every
/// account this rung's own tests create, since step 1-4 never populated the
/// table) is **not** the same as "every principal is Viewer" — it is
/// deliberately the pre-authz-retrofit default of "any authenticated
/// principal may act", so steps 1-4's existing tests keep passing unchanged.
/// Only an account with **at least one** role row switches to enforced mode:
/// once *any* row exists for that account, an unlisted principal is denied
/// (implicit `Viewer`, i.e. fails any `minimum` above `Viewer`). This is
/// crm's own written decision (LADDER.md's discipline rule) — see
/// crm/README.md's "Design decisions" for the rationale and the migration
/// path a real deployment would need (backfilling every account with an
/// explicit Owner-or-equivalent role at creation time, which this rung's
/// scope does not require).
/// @param accountId The account whose role table to check.
/// @param minimum The least-privileged role that satisfies the gate.
/// @throws EmptyPrincipalError if no principal is authenticated.
/// @throws Forbidden if the account has role rows and the caller's role
///         (or implicit `Viewer` if unlisted) is below `minimum`.
void requireRole(AccountId accountId, Role minimum);

/// @brief The calling principal's role on @p accountId, without throwing.
///
/// The non-throwing counterpart `requireRole` is built on — used where a
/// caller needs to know the role itself (redaction decisions, e.g.
/// `AccountModel::execute(const GetAccountHistory&)`), not just gate on a
/// minimum. Same fail-open convention: an account with no declared roles at
/// all reports `Role::Manager` (the caller may act as though fully
/// privileged, matching `requireRole`'s "unmanaged account" behavior) rather
/// than `Role::Viewer`, so a redaction check keyed on this never redacts a
/// field for an unmanaged account's own creator moments after they made it.
/// @param accountId The account whose role table to check.
/// @return The caller's effective role.
/// @throws EmptyPrincipalError if no principal is authenticated.
[[nodiscard]] Role callerRoleOn(AccountId accountId);

}  // namespace crm

template <>
struct glz::meta<crm::Role> {
    using enum crm::Role;
    static constexpr auto value = glz::enumerate(Viewer, Member, Manager);
};
