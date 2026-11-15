// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "lims/org/authorization/declared.hpp"

/// @file
/// The declared roles of every module, as the server edge uses them: by action id, held anywhere. The app is the one
/// place that knows every module, so the table is composed here; each entry is read off its declaration, never
/// restated.
/// The action guard (`lims::DeclaredRuleGuard`) enforces every declared rule on every execution. This table is the
/// edge's coarser answer, by id: on the server it refuses a principal without the role before the payload is decoded,
/// and it is what admits a watch, which runs no action and so no guard. It is not narrowed to what a guard would
/// refuse anyway, because the edge is not told whether it answers an execute or a watch.

namespace lims::app {

/// @brief The actions that change data and deliberately declare no role: any authenticated principal may run them.
///
/// The ignition-test actions are open because an ignition test belongs to no site: there is no scope to judge a role
/// in. The invoice editor's edits and its release are open because the editing instance's own live lease is what
/// permits them, and only `LeaseInvoice`, which declares a role, grants one. A bench's `ReleaseReservation` is open
/// for the same reason: it deletes only the rows of the reserving instance itself, and only `ReserveForSample`, which
/// declares a role, creates them. `SetDisplayName` is open because its
/// rule, the person or a supervisor of a company they hold a role in, relates two principals, which a role and a
/// scope cannot state; the org model checks it.
///
/// Listing an action here is a decision that it is open, made in one place. An action that changes data is either in
/// `declaredRequirements()` or here; the declared-authorization suite fails for an action of a listed model that is
/// neither guarded, open, nor a declared read, and for an id here that is also declared.
/// @return The action ids.
[[nodiscard]] std::span<std::string_view const> openMutatingActions();

/// @brief Every action a module declares a role, or some role, for.
/// @return The table.
[[nodiscard]] std::span<RoleRequirement const> declaredRequirements();

/// @brief The requirement @p actionType declares.
/// @param actionType The action id.
/// @return Its requirement, or nothing for an action that declares none.
[[nodiscard]] std::optional<RoleRequirement> requirementFor(std::string_view actionType);

/// @brief The specific role @p actionType declares.
/// @param actionType The action id.
/// @return The role, or nothing for an action that declares none or accepts any role.
[[nodiscard]] std::optional<LimsRole> requiredRole(std::string_view actionType);

}  // namespace lims::app
