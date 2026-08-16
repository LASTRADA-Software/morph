// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kanban/core/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace kanban {

inline constexpr std::size_t kMaxProjectNameBytes = 200;

/// @brief Creates a project. The caller becomes its first `Manager` (design
///        spec §3's "who seeds the first manager role" decision) --
///        `ProjectAdminModel::execute()` writes that role row in the same
///        transaction that creates the project.
struct CreateProject {
    std::string name;

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

    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue() && !principal.empty(); }
};

/// @brief Removes `principal`'s role row entirely -- they can no longer
///        attach to the project's board at all. Manager-only.
struct RemoveMember {
    ProjectId projectId;
    std::string principal;

    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue() && !principal.empty(); }
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

using Ack = struct Ack {};

}  // namespace kanban
