// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/tagged.hpp>
#include <string>
#include <vector>

#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"

/// @file
/// Offline field capture (README build order §7) — the queue payload format,
/// and the conflict records replay produces.
///
/// @par The base-version contract, stated once
/// A `QueuedCapture` names the sample version it was **prepared against**.
/// Replay applies it only if the server still holds that version; otherwise
/// the update is *flagged* for a human, never silently merged and never
/// silently dropped. That is ODK Central's offline-Entities answer, and it is
/// the one this rung implements.
///
/// @par Why a queued update is not simply a `CaptureConcentration`
/// The capture says what to record; the envelope says what the field client
/// believed when it decided to record it. Those are different facts with
/// different lifetimes — the capture is still meaningful after a conflict is
/// resolved, the belief is not — so they are different types.

namespace lims {

/// @brief A stable dedup token naming one logical field update.
///
/// A protocol scalar, so a named opaque newtype rather than a loose
/// `std::string` (`examples/IMPLEMENTATION.md` rule 3). It is carried in the
/// payload *and* set as the queue item's `idempotencyKey`, which is what makes
/// it the shared identity `docs/spec/offline/offline.md` requires across the
/// queue, the journal and process restarts.
using OperationKey = ::morph::util::Tagged<std::string, "OperationKey">;

/// @brief Why replay could not apply a queued update.
enum class ConflictReason : std::uint8_t {
    /// @brief The sample moved on while the client was offline: the update's
    ///        base version is not the version the server holds.
    StaleBase,
    /// @brief The sample left the state where results may be captured (it was
    ///        submitted for verification, published, or rejected meanwhile).
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
/// A conflict is an **outcome**, not an error, so it is returned rather than
/// thrown. Throwing would abort the replay of every later item in the queue
/// and lose the very flag the run exists to raise — and
/// `examples/IMPLEMENTATION.md`'s "never encode failure as a magic value" rule
/// is about failures, which this is not: the two-enumerator `enum class` here
/// is the same shape as `polls::Restored` and `polls::Finalized`.
enum class ReplayOutcome : std::uint8_t {
    Applied,     ///< The update landed; the sample advanced.
    Conflicted,  ///< The update was flagged; the sample is unchanged.
    Skipped,     ///< This operation had already been applied (idempotency key).
};

/// @brief The stable ascii name of @p reason, for display and logging.
/// @param reason The conflict reason.
/// @return Its name.
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

/// @brief The stable ascii name of @p status, for display and logging.
/// @param status The conflict's status.
/// @return Its name.
[[nodiscard]] constexpr std::string_view conflictStatusName(ConflictStatus status) noexcept {
    switch (status) {
        case ConflictStatus::Open:
            return "open";
        case ConflictStatus::Discarded:
            return "discarded";
        case ConflictStatus::Applied:
            return "applied";
        default:
            return "unknown";
    }
}

/// @brief One capture prepared in the field, with the sample version it was
///        prepared against.
///
/// This is the offline queue's payload format for this rung. The queue itself
/// treats it as an opaque string (`QueueItem::payload`), so the round trip is
/// entirely this rung's responsibility — hence its registration as a real
/// action, which gives it the same codec every other action gets rather than a
/// second, hand-written one.
struct QueuedCapture {
    /// @brief The sample the update is for. Carried in the payload because a
    ///        queue item has no other way to name one.
    SampleId sampleId;

    /// @brief The sample version this update was prepared against.
    SampleVersion baseVersion;

    /// @brief The principal who captured the reading in the field.
    ///
    /// Replay refuses unless this equals the authenticated principal, so a
    /// queued update cannot be replayed *as* somebody else. In a 21 CFR Part
    /// 11-style trail the author of a reading is not a field a client may
    /// assert freely.
    std::string capturedBy;

    /// @brief This update's stable dedup token.
    ///
    /// Carried here as well as on the queue item so replay can enforce
    /// at-most-once *whatever* delivered it — the queue, a re-dispatch, or a
    /// journal replay. The queue itself is documented not to enforce
    /// uniqueness, and the shipped implementations disagree about whether they
    /// do anyway (`docs/findings/007`), so the enforcement has to live where
    /// it is asked for: in the consumer.
    OperationKey operationKey;

    /// @brief What to record.
    CaptureConcentration capture;

    /// @brief Whether this queued update is well-formed.
    /// @return `true` when the sample, the author, the dedup token and the
    ///         capture are all usable.
    [[nodiscard]] bool validate() const noexcept {
        return sampleId.hasValue() && !capturedBy.empty() && !(*operationKey).empty() && capture.validate();
    }
};

/// @brief The result of replaying one queued update.
struct ReplayCaptureResult {
    /// @brief What replay did.
    ReplayOutcome outcome = ReplayOutcome::Applied;

    /// @brief The sample the update was for.
    SampleId sampleId;

    /// @brief The version the update claimed as its base.
    SampleVersion baseVersion;

    /// @brief The version the server actually held.
    SampleVersion serverVersion;

    /// @brief The conflict raised, engaged only when `outcome == Conflicted`.
    ConflictId conflictId;

    /// @brief Why it conflicted; meaningless unless `outcome == Conflicted`.
    ConflictReason reason = ConflictReason::StaleBase;
};

/// @brief One flagged conflict, as served to whoever has to resolve it.
struct ConflictView {
    /// @brief The conflict's id.
    ConflictId id;

    /// @brief The sample it is about.
    SampleId sampleId;

    /// @brief The version the queued update was prepared against.
    SampleVersion baseVersion;

    /// @brief The version the server held when replay ran.
    SampleVersion serverVersion;

    /// @brief Why replay could not apply it.
    ConflictReason reason = ConflictReason::StaleBase;

    /// @brief Where the conflict has got to.
    ConflictStatus status = ConflictStatus::Open;

    /// @brief The queued payload verbatim, as the field client serialised it.
    std::string payload;

    /// @brief Who was replaying when it was flagged.
    std::string detectedBy;

    /// @brief When it was flagged.
    ::morph::time::Timestamp detectedAt;

    /// @brief Who resolved it; empty while open.
    std::string resolvedBy;

    /// @brief The resolver's stated rationale; empty while open.
    std::string resolutionNote;
};

/// @brief Lists conflicts flagged against the attached sample.
struct ListConflicts {
    /// @brief Always ready: the listing takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The attached sample's flagged conflicts, oldest first.
struct ListConflictsResult {
    /// @brief Every conflict ever flagged for the sample, resolved or not.
    std::vector<ConflictView> conflicts;
};

/// @brief A human's decision about one flagged conflict.
struct ResolveConflict {
    /// @brief The conflict to resolve.
    ConflictId conflictId;

    /// @brief What to do with it.
    ConflictResolution resolution = ConflictResolution::DiscardStale;

    /// @brief Why. Required: a discarded lab reading with no stated reason is
    ///        not an auditable decision.
    std::string note;

    /// @brief Whether this decision is well-formed.
    /// @return `true` when a conflict is named and a reason is given.
    [[nodiscard]] bool validate() const noexcept { return conflictId.hasValue() && !note.empty(); }
};

}  // namespace lims

/// @brief Reflects `ConflictReason` as its name, for the same reason
///        `SampleState` is reflected that way: the journal payload *is* the
///        audit record.
template <>
struct glz::meta<lims::ConflictReason> {
    using enum lims::ConflictReason;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(StaleBase, LifecycleClosed);
};

/// @brief Reflects `ConflictStatus` as its name.
template <>
struct glz::meta<lims::ConflictStatus> {
    using enum lims::ConflictStatus;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Open, Discarded, Applied);
};

/// @brief Reflects `ConflictResolution` as its name.
template <>
struct glz::meta<lims::ConflictResolution> {
    using enum lims::ConflictResolution;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(DiscardStale, ApplyAnyway);
};

/// @brief Reflects `ReplayOutcome` as its name.
template <>
struct glz::meta<lims::ReplayOutcome> {
    using enum lims::ReplayOutcome;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Applied, Conflicted, Skipped);
};
