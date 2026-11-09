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

/// @brief The actions that change data and deliberately declare no role: any authenticated principal may run them.
///
/// The ignition-test actions are open because an ignition test belongs to no site: there is no scope to judge a role
/// in.
///
/// Listing an action here is a decision that it is open, made in one place. An action that changes data is either in
/// `declaredRequirements()` or here; the declared-authorization suite fails for a mutating action of the org, sample,
/// analysis-catalog or ignition model that is in neither, and for an id here that is also declared.
/// @return The action ids.
[[nodiscard]] inline std::array<std::string_view, 14> const& openMutatingActions() {
    static constexpr std::array<std::string_view, 14> open{
        "DefineAnalysis",       "ReviseAnalysis",     "RegisterClient",           "NewSample",      "LoadSample",
        "HeaderSection",        "SamplingSection",    "TurbiditySection",         "NitrateSection", "AddIgnitionTest",
        "RecordIgnitionValues", "DeleteIgnitionTest", "ClearOptionalMeasurement", "SelectProject",
    };
    return open;
}

/// @brief Every action a module declares a role, or some role, for.
/// @return The table.
[[nodiscard]] inline std::array<RoleRequirement, 18> const& declaredRequirements() {
    static constexpr std::array<RoleRequirement, 18> requirements{
        requirementOf<VerifyResult>(),         requirementOf<CreateCompany>(), requirementOf<CreateSite>(),
        requirementOf<AssignRole>(),           requirementOf<RevokeRole>(),    requirementOf<RegisterSample>(),
        requirementOf<ReceiveSample>(),        requirementOf<StartWork>(),     requirementOf<SubmitForVerification>(),
        requirementOf<ReturnForRework>(),      requirementOf<PublishSample>(), requirementOf<RejectSample>(),
        requirementOf<CaptureConcentration>(), requirementOf<QueuedCapture>(), requirementOf<ResolveConflict>(),
        requirementOf<SaveSample>(),           requirementOf<DeleteSample>(),  requirementOf<SetValueLimit>(),
    };
    return requirements;
}

/// @brief The requirement @p actionType declares.
/// @param actionType The action id.
/// @return Its requirement, or nothing for an action that declares none.
[[nodiscard]] inline std::optional<RoleRequirement> requirementFor(std::string_view actionType) {
    for (RoleRequirement const& requirement : declaredRequirements()) {
        if (requirement.actionType == actionType) {
            return requirement;
        }
    }
    return std::nullopt;
}

/// @brief The specific role @p actionType declares.
/// @param actionType The action id.
/// @return The role, or nothing for an action that declares none or accepts any role.
[[nodiscard]] inline std::optional<LimsRole> requiredRole(std::string_view actionType) {
    auto const requirement = requirementFor(actionType);
    return requirement.has_value() ? requirement->role : std::nullopt;
}

}  // namespace lims::app
