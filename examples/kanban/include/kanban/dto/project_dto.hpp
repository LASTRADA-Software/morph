// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <morph/forms/forms.hpp>
#include <string>
#include <vector>

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/core/types.hpp"

namespace kanban {

inline constexpr std::size_t kMaxProjectNameBytes = 200;

/// @brief Creates a project. The caller becomes its first `Manager` (design
///        spec §3's "who seeds the first manager role" decision) --
///        `ProjectAdminModel::execute()` writes that role row in the same
///        transaction that creates the project.
struct CreateProject {
    std::string name;

    /// Side-effectful, so the renderer draws its own Submit button instead of
    /// firing on the first non-empty prefix of a project name -- same
    /// declaration, for the same reason, as `kanban::Login` (auth_dto.hpp).
    static constexpr bool explicitSubmit = true;

    /// Carries over the placeholder the hand-built `TextField` this form
    /// replaced used to show (`gui/qml/ProjectListView.qml`).
    static constexpr std::array<::morph::forms::FieldMeta, 1> fieldMetadata{
        ::morph::forms::FieldMeta{.field = "name", .label = "Project name", .placeholder = "new project name"},
    };

    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= kMaxProjectNameBytes; }
};

struct CreateProjectResult {
    ProjectId id;
};

/// @brief Sets (or changes) `principal`'s role on `projectId`. Manager-only
///        (design spec §3's `requireRole(Role::Manager)` gate).
struct SetMemberRole {
    ProjectId projectId;
    std::string principal;
    Role role = Role::Viewer;

    /// Bounds `principal` the same way `Login::validate()` does (reuses
    /// `auth::isValidPrincipal`) -- `ProjectRoleRecord::principal` is a
    /// `SqlAnsiString<auth::kMaxPrincipalBytes>` column, and
    /// `Light::SqlFixedString`'s constructor silently truncates rather than
    /// throwing on an over-length value. Without this bound, a caller could
    /// grant a role under an untruncated principal that then can never be
    /// found (and so never removed) by `RemoveMember`'s equality lookup on
    /// the same untruncated string.
    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue() && auth::isValidPrincipal(principal); }
};

/// @brief Removes `principal`'s role row entirely -- they can no longer
///        attach to the project's board at all. Manager-only.
struct RemoveMember {
    ProjectId projectId;
    std::string principal;

    /// Same `auth::isValidPrincipal` bound as `SetMemberRole::validate()` --
    /// see that comment. An over-length `principal` here would never match
    /// any stored (truncated) row anyway; rejecting it up front is more
    /// honest than a silent no-op delete.
    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue() && auth::isValidPrincipal(principal); }
};

struct MemberRole {
    std::string principal;
    Role role = Role::Viewer;
};

/// @brief Lists every member's role on `projectId`. Any project member may
///        call this (Viewer and above) -- it is a read, not an admin action.
struct GetProjectRoles {
    ProjectId projectId;

    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue(); }
};

struct GetProjectRolesResult {
    std::vector<MemberRole> roles;
};

/// @brief Lists every project the calling principal has any role on.
struct GetMyProjects {};

/// @brief One project the caller belongs to, with their own role on it.
struct MyProjectSummary {
    ProjectId id;
    std::string name;
    Role myRole = Role::Viewer;
};

/// @brief `GetMyProjects`' result: every project the caller has a role on,
///        ordered by project name.
struct GetMyProjectsResult {
    std::vector<MyProjectSummary> projects;
};

using Ack = struct Ack {};

}  // namespace kanban
