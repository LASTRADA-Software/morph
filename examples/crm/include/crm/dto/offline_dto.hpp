// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/tagged.hpp>
#include <string>
#include <vector>

#include "crm/dto/opportunity_dto.hpp"

/// @file
/// Offline field edits (README build order §8) — the queue payload format,
/// and the conflict records replay produces. Same shape as
/// `lims::offline_dto.hpp` (that rung's §7), adapted to crm's own entity: a
/// rep updating an opportunity from the field, rather than a lab operator
/// capturing a result.
///
/// @par The base-version contract, stated once
/// A `QueuedOpportunityUpdate` names the opportunity version it was
/// **prepared against**. Replay applies it only if the server still holds
/// that version; otherwise the update is *flagged* for a human, never
/// silently merged and never silently dropped — the same ODK Central-shaped
/// answer lims's rung already implements, reused here rather than
/// reinvented.
///
/// @par Why a queued update is not simply an `UpdateOpportunity`
/// `UpdateOpportunity` says what to change; the envelope says what the field
/// client believed (which version, which operator) when it decided to queue
/// that change. Those facts have different lifetimes — the edit is still
/// meaningful after a conflict is resolved, the belief is not — so, exactly
/// as lims's `QueuedCapture`/`CaptureConcentration` split, they are different
/// types. `UpdateOpportunity`'s own `CRM_OPPORTUNITY_FORM_RULES` (the
/// requiredWhen rule from README §7) also does not fit here unmodified: a
/// queued update's `validate()` only needs to confirm the envelope itself is
/// well-formed, not re-run the live form rule against a field snapshot that
/// may be stale by the time it replays — replay's own execute(UpdateOpportunity)
/// call re-validates the underlying edit anyway.

namespace crm {

/// @brief A stable dedup token naming one logical field update.
///
/// A protocol scalar (`examples/IMPLEMENTATION.md` rule 3), matching
/// `lims::OperationKey`'s exact shape. Carried in the payload *and* set as
/// the queue item's `idempotencyKey`.
using OperationKey = ::morph::util::Tagged<std::string, "OperationKey">;

/// @brief Why replay could not apply a queued opportunity update.
enum class ConflictReason : std::uint8_t {
    /// @brief The opportunity moved on while the client was offline: the
    ///        update's base version is not the version the server holds.
    StaleBase,
    /// @brief The opportunity reached a terminal stage (Won or Lost) meanwhile
    ///        — the same "closed while you were away" shape as lims's
    ///        `LifecycleClosed`, named for crm's own pipeline instead of a
    ///        sample's lifecycle.
    LifecycleClosed,
};

/// @brief Where a flagged conflict has got to.
enum class ConflictStatus : std::uint8_t {
    Open,       ///< Awaiting a human.
    Discarded,  ///< A human decided the queued update is superseded.
    Applied,    ///< A human rebased the queued update onto the current version.
};

/// @brief What a human decided to do with a flagged conflict.
enum class ConflictResolution : std::uint8_t {
    /// @brief Drop the queued update; the server's current value stands.
    DiscardStale,
    /// @brief Apply the queued update anyway, rebased onto the current version.
    ApplyAnyway,
};

/// @brief What replay did with one queued update.
///
/// An outcome, not an error (same rationale as `lims::ReplayOutcome`): a
/// conflict must not abort replay of the rest of the queue, so it is
/// returned rather than thrown.
enum class ReplayOutcome : std::uint8_t {
    Applied,     ///< The update landed; the opportunity advanced.
    Conflicted,  ///< The update was flagged; the opportunity is unchanged.
    Skipped,     ///< This operation had already been applied (idempotency key).
};

/// @brief The stable ascii name of @p reason, for display and logging.
[[nodiscard]] constexpr std::string_view conflictReasonName(ConflictReason reason) noexcept {
    switch (reason) {
        case ConflictReason::StaleBase:
            return "staleBase";
        case ConflictReason::LifecycleClosed:
            return "lifecycleClosed";
        default:
            return "unknown";
    }
}

/// @brief One queued opportunity edit, with the version it was prepared
///        against.
///
/// The offline queue's payload format for this rung's step 8. The queue
/// itself treats it as an opaque string (`QueueItem::payload`), so the round
/// trip is this rung's own responsibility — hence registering it as a real
/// action (its own codec), rather than hand-writing one.
struct QueuedOpportunityUpdate {
    /// @brief The opportunity the update is for.
    OpportunityId opportunityId;

    /// @brief The opportunity version this update was prepared against.
    std::int32_t baseVersion = 0;

    /// @brief The principal who made this edit in the field.
    ///
    /// Replay refuses unless this equals the authenticated principal — a
    /// queued update cannot be replayed *as* somebody else, the same
    /// author-integrity guard `lims::QueuedCapture::capturedBy` enforces.
    std::string capturedBy;

    /// @brief This update's stable dedup token.
    OperationKey operationKey;

    /// @brief The account the opportunity belongs to (unchanged by this
    ///        edit, but `UpdateOpportunity` needs it to re-validate).
    OpportunityAccountChoice account;

    /// @brief The opportunity's primary contact, as this edit leaves it.
    PrimaryContactChoice primaryContact;

    /// @brief The opportunity's name, as this edit leaves it.
    std::string name;

    /// @brief The opportunity's expected close value, as this edit leaves it.
    Money expectedCloseValue;

    /// @brief Whether this queued update is well-formed.
    /// @return `true` when the opportunity, the author, the dedup token and
    ///         the name are all usable. Does not re-run
    ///         `CRM_OPPORTUNITY_FORM_RULES` — see this file's own doc comment
    ///         for why that check belongs to replay's `execute(UpdateOpportunity)`
    ///         call instead, not to this envelope's own validation.
    [[nodiscard]] bool validate() const noexcept {
        return opportunityId.hasValue() && account.hasValue() && !capturedBy.empty() && !(*operationKey).empty() &&
               !name.empty();
    }
};

/// @brief The result of replaying one queued update.
struct ReplayOpportunityUpdateResult {
    ReplayOutcome outcome = ReplayOutcome::Applied;
    OpportunityId opportunityId;
    std::int32_t baseVersion = 0;
    std::int32_t serverVersion = 0;

    /// @brief The conflict raised, engaged only when `outcome == Conflicted`.
    ConflictId conflictId;

    /// @brief Why it conflicted; meaningless unless `outcome == Conflicted`.
    ConflictReason reason = ConflictReason::StaleBase;
};

/// @brief One flagged conflict, as served to whoever has to resolve it.
struct ConflictView {
    ConflictId id;
    OpportunityId opportunityId;
    std::int32_t baseVersion = 0;
    std::int32_t serverVersion = 0;
    ConflictReason reason = ConflictReason::StaleBase;
    ConflictStatus status = ConflictStatus::Open;

    /// @brief The queued payload verbatim, as the field client serialised it.
    ///
    /// Not re-encoded from a decoded struct — re-encoding would silently
    /// normalise away anything the current build no longer understands,
    /// exactly the journal-payload-evolution failure the rung README warns
    /// about (lims's `OfflineConflictRecord::payload` follows the same rule).
    std::string payload;

    std::string detectedBy;
    std::int64_t detectedAtMs = 0;
    std::string resolvedBy;
    std::string resolutionNote;
};

/// @brief Lists conflicts flagged against the attached opportunity.
struct ListConflicts {
    OpportunityId opportunityId;

    [[nodiscard]] bool validate() const noexcept { return opportunityId.hasValue(); }
};

struct ListConflictsResult {
    /// @brief Every conflict ever flagged for the opportunity, resolved or not.
    std::vector<ConflictView> conflicts;
};

/// @brief A human's decision about one flagged conflict.
struct ResolveConflict {
    ConflictId conflictId;
    ConflictResolution resolution = ConflictResolution::DiscardStale;

    /// @brief Why. Required: a discarded field edit with no stated reason is
    ///        not an auditable decision (same rule as lims's `ResolveConflict`).
    std::string note;

    [[nodiscard]] bool validate() const noexcept { return conflictId.hasValue() && !note.empty(); }

    /// @brief Opts the generated form out of auto-submit-on-validity
    ///        (docs/spec/forms/forms.md, "Explicit submit mode").
    static constexpr bool explicitSubmit = true;
};

}  // namespace crm

/// @brief Reflects `ConflictReason` as its name — the journal payload *is*
///        the audit record, same rationale as lims's identical reflection.
template <>
struct glz::meta<crm::ConflictReason> {
    using enum crm::ConflictReason;
    static constexpr auto value = glz::enumerate(StaleBase, LifecycleClosed);
};

template <>
struct glz::meta<crm::ConflictStatus> {
    using enum crm::ConflictStatus;
    static constexpr auto value = glz::enumerate(Open, Discarded, Applied);
};

template <>
struct glz::meta<crm::ConflictResolution> {
    using enum crm::ConflictResolution;
    static constexpr auto value = glz::enumerate(DiscardStale, ApplyAnyway);
};

template <>
struct glz::meta<crm::ReplayOutcome> {
    using enum crm::ReplayOutcome;
    static constexpr auto value = glz::enumerate(Applied, Conflicted, Skipped);
};
