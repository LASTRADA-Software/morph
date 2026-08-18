// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kanban/core/errors.hpp"
#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/project_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

/// @file
/// `ProjectAdminModel` -- project lifecycle and per-project RBAC (design
/// spec §2's "ProjectAdminModel's write surface is a separate strand"
/// decision: this model owns project/role administration, `BoardModel`
/// owns everything that mutates board content).

namespace kanban {

/// @brief Project-lifecycle and role-administration actions. Registered
///        plain, not `AllowShared` -- each caller's own admin operations
///        need no cross-caller shared state (unlike `BoardModel`).
class ProjectAdminModel {
  public:
    /// @brief Creates a project; the caller becomes its first `Manager`
    ///        (design spec §3).
    CreateProjectResult execute(const CreateProject& action);
    /// @brief Manager-only: sets or changes `action.principal`'s role.
    Ack execute(const SetMemberRole& action);
    /// @brief Manager-only: removes `action.principal`'s role entirely.
    Ack execute(const RemoveMember& action);
    /// @brief Any project member (Viewer and above) may list roles.
    GetProjectRolesResult execute(const GetProjectRoles& action);
    /// @brief Lists every project the calling principal has any role on,
    ///        with their own role, ordered by project name. No project-id
    ///        parameter -- the principal comes from `session::current()`.
    GetMyProjectsResult execute(const GetMyProjects& action);

  private:
    /// @brief Throws `Forbidden` unless the calling principal's role on
    ///        `projectId` is at least `minimum`. Loads the project row
    ///        first (to confirm it exists at all) -- a caller naming a
    ///        nonexistent project gets `NotFound`, not `Forbidden`.
    /// @throws NotFound if `projectId` names no project.
    /// @throws Forbidden if the caller has no role, or a role below `minimum`.
    void requireRole(ProjectId projectId, Role minimum) const;
};

/// @brief Mints session tokens -- mirrors `bookmarks::AuthModel` exactly.
class AuthModel {
  public:
    /// @brief Mints a signed session token for @p action's username, with no
    ///        registration or membership check (design spec's own stated
    ///        scope cut, inherited from `bookmarks::AuthModel`: any
    ///        syntactically valid username is accepted).
    /// @param action The username to mint a token for.
    /// @return The signed bearer token and the verified username.
    /// @throws ValidationError if `action.validate()` rejects the username,
    ///         if the username falls in the reserved `system:` principal
    ///         namespace, or if no token issuer has been installed.
    LoginResult execute(const Login& action);
};

}  // namespace kanban

BRIDGE_REGISTER_MODEL(kanban::ProjectAdminModel, "ProjectAdminModel")
BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::CreateProject, "CreateProject")
BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::SetMemberRole, "SetMemberRole")
BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::RemoveMember, "RemoveMember")
BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::GetProjectRoles, "GetProjectRoles",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::GetMyProjects, "GetMyProjects",
                       ::morph::model::Loggable::No)

BRIDGE_REGISTER_MODEL(kanban::AuthModel, "AuthModel")
BRIDGE_REGISTER_ACTION(kanban::AuthModel, kanban::Login, "Login")
