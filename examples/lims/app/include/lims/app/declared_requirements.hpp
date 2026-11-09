// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "lims/lab/authorization/lab_authorization.hpp"
#include "lims/org/authorization/org_authorization.hpp"

/// @file
/// The declared roles of every module, as the server edge uses them: by action id, held anywhere. The app is the one
/// place that knows every module, so the table is composed here; each entry is read off its declaration, never
/// restated.

namespace lims::app {

/// @brief Every action a module declares a role for.
/// @return The table.
[[nodiscard]] inline std::array<RoleRequirement, 5> const& declaredRequirements() {
    static constexpr std::array<RoleRequirement, 5> requirements{
        requirementOf<VerifyResult>(), requirementOf<CreateCompany>(), requirementOf<CreateSite>(),
        requirementOf<AssignRole>(),   requirementOf<RevokeRole>(),
    };
    return requirements;
}

/// @brief The role @p actionType declares.
/// @param actionType The action id.
/// @return The role, or nothing for an action that declares none.
[[nodiscard]] inline std::optional<LimsRole> requiredRole(std::string_view actionType) {
    for (RoleRequirement const& requirement : declaredRequirements()) {
        if (requirement.actionType == actionType) {
            return requirement.role;
        }
    }
    return std::nullopt;
}

}  // namespace lims::app
