// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/dto/sample_dto.hpp"

/// @file
/// Verification and the audit trail (README build order §6).
///
/// @par Four eyes, and where each eye is checked
/// "Four-eyes" is two separate rules that live in two separate places, and
/// conflating them is how such a control ends up not working:
///
/// 1. **The verifier must hold the role.** A *type*-level fact, checkable
///    before dispatch — `lims::auth::LimsAuthorizer` does it at the
///    `RemoteServer` edge, and `SampleModel` re-does it on every dispatch
///    path, because an authorizer does not run in `Local` mode at all
///    (`examples/IMPLEMENTATION.md` rule 1: client gates are UX, not
///    security).
/// 2. **The verifier must not be the analyst who captured the result.** A
///    *row*-level fact that `IAuthorizer` structurally cannot express — it is
///    handed the model and action type ids and the caller's session, never
///    the result being acted on. This one can only live in the model.

namespace lims {

/// @brief What a principal is permitted to do.
///
/// A closed set, so an `enum class` rather than free-form strings
/// (`examples/IMPLEMENTATION.md` rule 3). Roles are not hierarchical: a
/// supervisor who must also verify is granted `Verifier` explicitly, because
/// an implicit hierarchy is the kind of thing that quietly grants somebody
/// the second pair of eyes on their own work.
enum class LimsRole : std::uint8_t {
    Analyst,     ///< May capture results.
    Verifier,    ///< May verify somebody else's results.
    Supervisor,  ///< May grant and revoke roles.
};

/// @brief The stable ascii name of @p role, as it appears in a token claim and
///        in the journal.
/// @param role The role to name.
/// @return Its wire name.
[[nodiscard]] constexpr std::string_view roleName(LimsRole role) noexcept {
    switch (role) {
        case LimsRole::Analyst:
            return "analyst";
        case LimsRole::Verifier:
            return "verifier";
        case LimsRole::Supervisor:
            return "supervisor";
        default:
            return "unknown";
    }
}

/// @brief Grants @p role to @p principal.
///
/// Requires the caller to hold `Supervisor` — except on a lab with no
/// supervisor yet, where the first grant is allowed so the system can be
/// bootstrapped at all. That exception is deliberately narrow (it closes the
/// instant any supervisor exists) and is journaled like every other grant, so
/// it is visible in the audit trail rather than a silent back door.
struct GrantRole {
    /// @brief The principal to grant to.
    std::string principal;

    /// @brief The role to grant.
    LimsRole role = LimsRole::Analyst;

    /// @brief Whether this grant is well-formed.
    /// @return `true` when a principal is named.
    [[nodiscard]] bool validate() const noexcept { return !principal.empty(); }
};

/// @brief The roles one principal now holds.
struct RoleGrantResult {
    /// @brief The principal granted to.
    std::string principal;

    /// @brief Every role they hold, in grant order.
    std::vector<LimsRole> roles;
};

/// @brief The four-eyes verification of one captured result.
///
/// Refused if the caller lacks `Verifier`, if the caller is the analyst who
/// captured the result, if the sample is not awaiting verification, or if the
/// result has already been verified.
struct VerifyResult {
    /// @brief The result to verify.
    ResultId resultId;

    /// @brief Whether a result is named.
    /// @return `true` when `resultId` carries a value.
    [[nodiscard]] bool validate() const noexcept { return resultId.hasValue(); }
};

/// @brief One recorded verification.
struct VerificationView {
    /// @brief The verification's id.
    VerificationId id;

    /// @brief The result verified.
    ResultId resultId;

    /// @brief The sample it belongs to.
    SampleId sampleId;

    /// @brief The analyst who captured the result.
    std::string capturedBy;

    /// @brief The verifier — never the same person as `capturedBy`.
    std::string verifiedBy;

    /// @brief When it was verified.
    ::morph::time::Timestamp verifiedAt;
};

/// @brief Lists the verifications recorded against the attached sample.
struct ListVerifications {
    /// @brief Always ready: the listing takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The attached sample's verifications.
struct ListVerificationsResult {
    /// @brief Every verification recorded for the sample.
    std::vector<VerificationView> verifications;
};

/// @brief What one step of an audit trail records.
enum class AuditStepKind : std::uint8_t {
    LifecycleTransition,  ///< The sample changed state.
    ResultCaptured,       ///< A reading was recorded.
    ResultVerified,       ///< A reading passed four-eyes verification.
    OfflineReplay,        ///< A queued field update was applied or flagged.
    ConflictResolved,     ///< A human decided a flagged conflict.
    RoleGranted,          ///< A principal was granted a role.
    /// @brief The journal holds an entry this build cannot interpret.
    ///
    /// **Never silently dropped.** An audit trail that quietly omits what it
    /// could not read is worse than one that admits the gap: the omission is
    /// invisible, and "reconstructible from the journal alone" becomes a claim
    /// nobody can check. See the rung README's §6 decision.
    Unreadable,
};

/// @brief Whether the recorded attempt succeeded.
enum class AuditOutcome : std::uint8_t {
    Succeeded,  ///< The action was applied.
    Refused,    ///< The action was rejected; the sample did not change.
};

/// @brief One reconstructed step of a sample's history.
struct AuditStep {
    /// @brief When it happened.
    ::morph::time::Timestamp at;

    /// @brief Who did it.
    std::string principal;

    /// @brief The recorded action id, verbatim from the journal.
    std::string action;

    /// @brief What kind of step this is.
    AuditStepKind kind = AuditStepKind::Unreadable;

    /// @brief Whether it was applied or refused.
    AuditOutcome outcome = AuditOutcome::Succeeded;

    /// @brief The state the sample was left in. Meaningful only for
    ///        `LifecycleTransition` steps.
    SampleState state = SampleState::Registered;

    /// @brief The base version the sample was left at. Meaningful only for
    ///        `LifecycleTransition` steps.
    SampleVersion version;

    /// @brief Why it was refused, or why it could not be read. Empty otherwise.
    std::string detail;
};

/// @brief Reconstructs the attached sample's full history from the journal.
struct GetAuditTrail {
    /// @brief Always ready: the reconstruction takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The attached sample's history, oldest first.
struct AuditTrailResult {
    /// @brief Every recorded step, including the ones this build could not
    ///        interpret.
    std::vector<AuditStep> steps;

    /// @brief Every state the sample was ever in, in order — the README's
    ///        definition-of-done claim, made directly checkable.
    std::vector<SampleState> states;
};

}  // namespace lims

/// @brief Reflects `LimsRole` as its enumerator name.
template <>
struct glz::meta<lims::LimsRole> {
    using enum lims::LimsRole;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Analyst, Verifier, Supervisor);
};

/// @brief Reflects `AuditStepKind` as its enumerator name.
template <>
struct glz::meta<lims::AuditStepKind> {
    using enum lims::AuditStepKind;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(LifecycleTransition, ResultCaptured, ResultVerified, OfflineReplay,
                                                 ConflictResolved, RoleGranted, Unreadable);
};

/// @brief Reflects `AuditOutcome` as its enumerator name.
template <>
struct glz::meta<lims::AuditOutcome> {
    using enum lims::AuditOutcome;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Succeeded, Refused);
};
