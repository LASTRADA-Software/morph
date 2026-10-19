// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/core/self_journal.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
#include "lims/dto/sample_entry_dto.hpp"
#include "lims/dto/verification_dto.hpp"

/// @file
/// `SampleModel` — the sample lifecycle (README build order §2), keyed by
/// sample id so a bench client and an office client attach to the same
/// instance.

namespace lims {

/// @brief One sample: its registration and its position in the lab workflow.
///
/// Keyed by `SampleId` (the `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` lines below
/// this class). Holds no connection of its own — each `execute()`
/// opens a `Lightweight::DataMapper` for its own duration, which is safe
/// because morph serialises every call on one instance onto its own strand.
///
/// **Every transition is guarded and journaled, including the ones that
/// fail.** An attempt to publish an unverified sample is rejected with
/// `IllegalTransition` *and* appended to the action log with
/// `Outcome::Failed`. A regulatory trail that records only the transitions
/// that succeeded cannot answer "who tried", which is half of what such a
/// trail is for.
class SampleModel {
public:
    /// @brief Registers a client samples can belong to.
    /// @param action The client's name.
    /// @return The new client's id.
    /// @throws ValidationError if the name is empty.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RegisterClientResult execute(const RegisterClient& action);

    /// @brief Logs a new sample, `Registered` at version 1, and attaches this
    ///        handler to it.
    /// @param action The owning client and the lab's container reference.
    /// @return The new sample's full state.
    /// @throws ValidationError if the action is not well-formed.
    /// @throws NotFound if the named client does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const RegisterSample& action);

    /// @brief Attaches this handler to the sample named by @p action.
    /// @param action The sample's id.
    /// @return That sample's full state.
    /// @throws ValidationError if no sample is named.
    /// @throws NotFound if the sample does not exist.
    SampleView execute(const OpenSample& action);

    /// @brief Re-reads the attached sample.
    /// @param action Carries no fields of its own.
    /// @return The attached sample's full state.
    /// @throws NotFound if this handler is not attached, or the sample is gone.
    SampleView execute(const GetSample& action);

    /// @brief `Registered → Received`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `Registered`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const ReceiveSample& action);

    /// @brief `Received → InProgress`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `Received`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const StartWork& action);

    /// @brief `InProgress → ToBeVerified`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `InProgress`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const SubmitForVerification& action);

    /// @brief `ToBeVerified → InProgress`, with the verifier's stated reason.
    /// @param action The reason the sample went back to the bench.
    /// @return The sample's state after the transition.
    /// @throws ValidationError if no reason is given.
    /// @throws IllegalTransition if the sample is not `ToBeVerified`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const ReturnForRework& action);

    /// @brief `ToBeVerified → Published`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `ToBeVerified`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const PublishSample& action);

    /// @brief `Registered|Received → Rejected`, with the stated reason.
    /// @param action Why the container is refused.
    /// @return The sample's state after the transition.
    /// @throws ValidationError if no reason is given.
    /// @throws IllegalTransition if the sample has already been worked.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const RejectSample& action);

    /// @brief Captures a concentration result against the attached sample.
    ///
    /// The submitted value is in the analysis version's canonical unit; the
    /// entry-unit conversion is the renderer's job, driven by the
    /// `x-unitAlternatives` this rung's `UnitTraits::relations` generate.
    ///
    /// Re-capturing the same analysis version replaces the previous result
    /// rather than appending a second row: a result is the lab's current
    /// answer for one analysis on one sample, and two live answers would make
    /// "the sample's results" ambiguous. The superseded value is not lost —
    /// the journal holds every capture.
    /// @param action The version, and exactly one of value/qualifier.
    /// @return The stored result.
    /// @throws ValidationError if the encoding is violated, the qualifier code
    ///         is unknown, the version's canonical unit is not mg/L, or the
    ///         value carries more precision than the version declares.
    /// @throws NotFound if the analysis version does not exist.
    /// @throws IllegalTransition if the sample is not `InProgress`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ResultView execute(const CaptureConcentration& action);

    /// @brief Lists the attached sample's results, oldest first.
    /// @param action Carries no fields of its own.
    /// @return Every result captured against the sample.
    /// @throws NotFound if this handler is not attached to a sample.
    ListResultsResult execute(const ListResults& action);

    /// @brief Serves the dilution picklist a `DilutionChoice` renders from.
    /// @param action Carries no fields of its own.
    /// @return The preparation modes, with display text.
    ListDilutionModesResult execute(const ListDilutionModes& action);

    /// @brief Serves the qualifier picklist a `QualifierChoice` renders from.
    /// @param action Carries no fields of its own.
    /// @return The three "no number" codes, with display text.
    ListResultQualifiersResult execute(const ListResultQualifiers& action);

    /// @brief Starts a new, empty working copy and detaches the handler from any sample.
    /// @param action Carries no fields of its own.
    /// @return The new working copy's header.
    HeaderView execute(const NewSample& action);

    /// @brief Loads a stored sample's entry data into the working copy and attaches the handler to the sample.
    /// @param action The sample.
    /// @return The working copy's header.
    /// @throws ValidationError if no sample is named.
    /// @throws NotFound if the sample does not exist.
    HeaderView execute(const LoadSample& action);

    /// @brief Commits the working copy: registers the sample when it is new, then stores its entry data.
    ///
    /// An incomplete working copy is an outcome, not an exception: the reply's `error` names what is missing, so
    /// a client shows one message per reason and the working copy stays as it was.
    /// @param action Carries no fields of its own.
    /// @return The saved sample, or why the save was refused.
    /// @throws NotFound if the header's client does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SaveSampleResult execute(const SaveSample& action);

    /// @brief Deletes the stored sample the working copy was loaded from, and discards the working copy.
    /// @param action Carries no fields of its own.
    /// @return The deleted sample.
    /// @throws NotFound if the working copy was never saved.
    /// @throws IllegalTransition if the sample has left `Registered`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    DeleteSampleResult execute(const DeleteSample& action);

    /// @brief Replaces the working copy's header.
    /// @param action The header.
    /// @return The working copy's header.
    /// @throws NotFound if there is no working copy.
    HeaderView execute(const HeaderSection& action);

    /// @brief Replaces the working copy's sampling section.
    /// @param action The spike and the measurements.
    /// @return The dilution series it implies.
    /// @throws NotFound if there is no working copy.
    DilutionSeries execute(const SamplingSection& action);

    /// @brief Sets the working copy's turbidity reading.
    /// @param action The reading.
    /// @return The optional measurements.
    /// @throws NotFound if there is no working copy.
    MeasurementsView execute(const TurbiditySection& action);

    /// @brief Sets the working copy's nitrate reading.
    /// @param action The reading.
    /// @return The optional measurements.
    /// @throws NotFound if there is no working copy.
    MeasurementsView execute(const NitrateSection& action);

    /// @brief Removes an optional measurement's reading from the working copy.
    /// @param action The measurement.
    /// @return The optional measurements.
    /// @throws ValidationError if the measurement is not an optional one.
    /// @throws NotFound if there is no working copy.
    MeasurementsView execute(const ClearOptionalMeasurement& action);

    /// @brief Reads the working copy's header.
    /// @param action Carries no fields of its own.
    /// @return The header.
    /// @throws NotFound if there is no working copy.
    HeaderView execute(const GetHeader& action);

    /// @brief Reads the working copy's optional measurements.
    /// @param action Carries no fields of its own.
    /// @return The measurements, and which of them hold data.
    /// @throws NotFound if there is no working copy.
    MeasurementsView execute(const GetMeasurements& action);

    /// @brief Computes the dilution series from the working copy's sampling section.
    ///
    /// The recovery and the limit check are the model's, never a client's: the value shown is the value judged.
    /// @param action Carries no fields of its own.
    /// @return The series; empty until a spike is entered.
    /// @throws NotFound if there is no working copy.
    DilutionSeries execute(const GetDilutionSeries& action);

    /// @brief Lists the projects a sample can be filed under, sorted, filtered and paged by the table engine.
    /// @param action The table query.
    /// @return The page.
    /// @throws ValidationError if the query names a column the rows do not have, or exceeds the limits.
    ::morph::table::Page<ProjectRow> execute(const ListProjects& action);

    /// @brief Files the working copy under a project.
    /// @param action The project.
    /// @return The working copy's header.
    /// @throws NotFound if there is no working copy, or no such project.
    HeaderView execute(const SelectProject& action);

    /// @brief Every role @p principal currently holds.
    ///
    /// Static because `lims::auth::LimsAuthorizer` wires its own `RoleLookup`
    /// to the very same query: one source of truth for what a principal may
    /// do, read by both enforcement points.
    /// @param principal The principal to look up.
    /// @return The roles held, in grant order.
    [[nodiscard]] static std::vector<LimsRole> rolesOf(std::string_view principal);

    /// @brief Grants a role to a principal.
    ///
    /// Requires `LimsRole::Supervisor`, except on a lab that has no supervisor
    /// yet, where the first grant is allowed so the system can be bootstrapped.
    /// The exception closes the instant any supervisor exists, and the
    /// bootstrap grant is journaled like every other one.
    /// @param action The principal and the role.
    /// @return Every role that principal now holds.
    /// @throws ValidationError if no principal is named.
    /// @throws Forbidden if the caller is not a supervisor and one already exists.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RoleGrantResult execute(const GrantRole& action);

    /// @brief Records the four-eyes verification of one captured result.
    ///
    /// Both halves of the control are enforced here, because only one of them
    /// *can* be enforced anywhere else: the caller must hold
    /// `LimsRole::Verifier` (which `lims::auth::LimsAuthorizer` also checks at
    /// the `RemoteServer` edge, and which is re-checked here because no
    /// authorizer runs on the `Local` path), and the caller must not be the
    /// analyst who captured the result (which `IAuthorizer` structurally
    /// cannot express — it never sees the result).
    /// @param action The result to verify.
    /// @return The recorded verification.
    /// @throws ValidationError if no result is named.
    /// @throws NotFound if the result does not exist.
    /// @throws Forbidden if the caller lacks `Verifier`, or captured the result.
    /// @throws IllegalTransition if the sample is not awaiting verification.
    /// @throws Conflict if the result is already verified.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    VerificationView execute(const VerifyResult& action);

    /// @brief Lists the verifications recorded against the attached sample.
    /// @param action Carries no fields of its own.
    /// @return Every verification for the sample.
    /// @throws NotFound if this handler is not attached to a sample.
    ListVerificationsResult execute(const ListVerifications& action);

    /// @brief Reconstructs the attached sample's history **from the journal
    ///        alone**.
    ///
    /// Reads the attached action log and nothing else — no database access of
    /// any kind — which is what makes the README's definition of done ("every
    /// state a sample was ever in is reconstructible from the journal alone")
    /// a claim this method can actually support rather than assert.
    ///
    /// An entry this build cannot interpret is reported as
    /// `AuditStepKind::Unreadable`, never skipped: an audit trail that quietly
    /// omits what it could not read is worse than one that admits the gap,
    /// because the omission is invisible.
    /// @param action Carries no fields of its own.
    /// @return Every recorded step, and the state sequence they imply.
    /// @throws NotFound if this handler is not attached to a sample.
    AuditTrailResult execute(const GetAuditTrail& action);

    /// @brief Applies one queued field update, or flags it as a conflict.
    ///
    /// The heart of §7. Applies @p action only if the server still holds the
    /// version the update was prepared against; otherwise records an
    /// `OfflineConflictRecord` for a human and leaves the sample untouched.
    /// A conflict is a returned **outcome**, not a thrown error — throwing
    /// would abort the replay of every later item in the queue and lose the
    /// flag the run exists to raise.
    ///
    /// Redelivery is idempotent: an @p opKey this server has already applied
    /// is skipped. That enforcement belongs here, not in the queue
    /// (`docs/spec/offline/offline.md`), and it is what makes replay correct
    /// against every shipped `IOfflineQueue` regardless of whether that
    /// implementation happens to dedup at enqueue time.
    /// @param action The queued update, with the base version it assumed.
    /// @return What replay did, and the conflict id if it flagged one.
    /// @throws ValidationError if the envelope is not well-formed.
    /// @throws Forbidden if `capturedBy` is not the authenticated principal.
    /// @throws NotFound if the sample or the analysis version does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ReplayCaptureResult execute(const QueuedCapture& action);

    /// @brief Lists the conflicts flagged against the attached sample.
    /// @param action Carries no fields of its own.
    /// @return Every conflict for the sample, oldest first, resolved or not.
    /// @throws NotFound if this handler is not attached to a sample.
    ListConflictsResult execute(const ListConflicts& action);

    /// @brief Records a human's decision about one flagged conflict.
    ///
    /// `DiscardStale` closes it and leaves the server's value standing;
    /// `ApplyAnyway` **rebases** the queued capture onto the sample's current
    /// version and applies it. Either way the decision, its author and its
    /// stated reason are journaled — a discarded lab reading with no recorded
    /// rationale is not an auditable decision.
    /// @param action The conflict, the decision, and why.
    /// @return The conflict in its resolved state.
    /// @throws ValidationError if no conflict is named or no reason is given.
    /// @throws NotFound if the conflict does not exist.
    /// @throws Conflict if it has already been resolved.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ConflictView execute(const ResolveConflict& action);

    /// @brief Drains the attached offline queue and replays every item.
    ///
    /// The framework's own hook for this (`docs/spec/offline/offline.md`,
    /// "Conflict resolution on replay"): `Bridge::switchBackend` posts this
    /// onto the model's own strand when a client reconnects, so it never
    /// overlaps an `execute()` on the same instance and needs no locking.
    ///
    /// Every drained item is `markDone()`d whatever the outcome — applied,
    /// flagged, skipped, or rejected outright — because this path has no
    /// retry budget and must not leave an item needing a later attempt. An
    /// item that cannot even be decoded, or that names a sample this server
    /// does not have, is journaled as a failure and dropped; it is
    /// unresolvable by definition, and leaving it in the queue would block
    /// every later item behind it forever.
    void onBackendChanged();

    /// @brief Attaches the offline queue `onBackendChanged()` drains.
    /// @param queue The queue field clients enqueue into. Null detaches.
    void attachOfflineQueue(std::shared_ptr<::morph::offline::IOfflineQueue> queue);

    /// @brief Attaches a durable action log, so every mutating `execute()`
    ///        records a `LogEntry`.
    ///
    /// Needed because a plain-constructed model never goes through the
    /// registry/dispatcher path the framework's own auto-append lives on —
    /// see `lims::SelfJournal`'s file comment.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every entry; re-stamped
    ///        with the sample's own id as soon as this handler attaches to one.
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

private:
    /// @brief Shared body of every transition action.
    ///
    /// Runs the guard, writes the new state and the bumped base version in one
    /// transaction, and journals the outcome — success or rejection.
    /// @tparam Action The transition action type (supplies the journal's
    ///         action id and payload).
    /// @param action The transition being attempted.
    /// @param target The state the lifecycle is being asked to move to.
    /// @return The sample's state after the transition.
    template <typename Action>
    SampleView transition(const Action& action, SampleState target);

    /// @brief Loads the attached sample and returns its view.
    /// @return The attached sample's full state.
    /// @throws NotFound if this handler is not attached, or the row is gone.
    [[nodiscard]] SampleView loadAttached() const;

    /// @brief Writes @p target and the bumped base version to the attached row.
    /// @param target The new state (already checked legal by the caller).
    /// @return The sample's state after the write.
    [[nodiscard]] SampleView writeState(SampleState target) const;

    /// @brief Shared body of `execute(QueuedCapture)` and the `ApplyAnyway`
    ///        half of `execute(ResolveConflict)`.
    ///
    /// Applies @p capture to the sample named by @p sampleId, on behalf of
    /// @p author, without consulting the base version — the caller has already
    /// decided the write is legitimate.
    /// @param sampleId The sample to write against.
    /// @param capture What to record.
    /// @param author The principal the stored result is attributed to.
    /// @param opKey The replayed operation's dedup token, recorded in the
    ///        same transaction as the capture itself; empty (the default)
    ///        records nothing, for the two callers that are not replaying a
    ///        queued item. Passed in rather than written by the caller
    ///        afterwards because the two writes have to commit together and
    ///        this function owns the only transaction they can share:
    ///        `Lightweight::SqlTransaction` does not nest, so a caller-side
    ///        transaction over the same connection would end this one rather
    ///        than contain it.
    /// @return The stored result.
    ResultView applyCapture(SampleId sampleId, const CaptureConcentration& capture, const std::string& author,
                            const OperationKey& opKey = {});

    /// @brief Throws `IllegalTransition` unless every result captured against
    ///        the attached sample has a verification recorded.
    ///
    /// The `PublishSample` precondition, and the reason the four-eyes step is
    /// load-bearing rather than decorative.
    void requireEveryResultVerified() const;

    /// @brief Throws `Forbidden` unless the authenticated principal holds @p role.
    /// @param role The role the action requires.
    static void requireRole(LimsRole role);

    /// @brief Whether @p opKey names an operation this server already decided.
    /// @param opKey The operation's dedup token; empty is never a match.
    /// @return `true` when the key is recorded in `lims_replayed_ops`.
    [[nodiscard]] static bool alreadyDecided(const std::string& opKey);

    /// @brief The working copy, or throws.
    /// @return The working copy.
    /// @throws NotFound if no sample was created or loaded.
    [[nodiscard]] SampleDraft& draft();

    /// @brief The working copy's header as a client sees it.
    /// @return The header.
    [[nodiscard]] HeaderView headerView() const;

    /// @brief The working copy, set by `NewSample` and `LoadSample`; unset until then.
    std::optional<SampleDraft> _draft;

    /// @brief The sample this handler is attached to, set by `RegisterSample`
    ///        or `OpenSample`. Unset until then.
    std::optional<std::int64_t> _sampleId;

    /// @brief This instance's own journal (inert until `attachActionLog`).
    SelfJournal _journal;

    /// @brief The offline queue `onBackendChanged()` drains; null until
    ///        `attachOfflineQueue`, and then this instance records nothing new
    ///        of its own — the queue is shared with the field clients' outboxes.
    std::shared_ptr<::morph::offline::IOfflineQueue> _queue;
};

}  // namespace lims

BRIDGE_REGISTER_MODEL(lims::SampleModel, "SampleModel")
// No screen lists clients: a registered client is only named by the sample registered for it next.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RegisterClient, "RegisterClient")
BRIDGE_REGISTER_EFFECTS(lims::RegisterClient, ::morph::effects::none)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RegisterSample, "RegisterSample")
BRIDGE_REGISTER_EFFECTS(lims::RegisterSample, ::morph::effects::changes<lims::SampleView>)
// Opening attaches this instance to a sample; it changes nothing stored.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::OpenSample, "OpenSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::OpenSample, ::morph::effects::none)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetSample, "GetSample", ::morph::model::Loggable::No)
// A transition answers the sample as it now is.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ReceiveSample, "ReceiveSample")
BRIDGE_REGISTER_EFFECTS(lims::ReceiveSample, ::morph::effects::updates<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::StartWork, "StartWork")
BRIDGE_REGISTER_EFFECTS(lims::StartWork, ::morph::effects::updates<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::SubmitForVerification, "SubmitForVerification")
BRIDGE_REGISTER_EFFECTS(lims::SubmitForVerification, ::morph::effects::updates<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ReturnForRework, "ReturnForRework")
BRIDGE_REGISTER_EFFECTS(lims::ReturnForRework, ::morph::effects::updates<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::PublishSample, "PublishSample")
BRIDGE_REGISTER_EFFECTS(lims::PublishSample, ::morph::effects::updates<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RejectSample, "RejectSample")
BRIDGE_REGISTER_EFFECTS(lims::RejectSample, ::morph::effects::updates<lims::SampleView>)
// A capture replaces the version's previous result with a new row under a new id, and moves the sample's
// version, so both are changed rather than updated.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::CaptureConcentration, "CaptureConcentration")
BRIDGE_REGISTER_EFFECTS(lims::CaptureConcentration, ::morph::effects::changes<lims::ResultView>,
                        ::morph::effects::changes<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListResults, "ListResults", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListResultQualifiers, "ListResultQualifiers",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListDilutionModes, "ListDilutionModes", ::morph::model::Loggable::No)
// An offline replay applies its capture, or records why it could not.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::QueuedCapture, "QueuedCapture")
BRIDGE_REGISTER_EFFECTS(lims::QueuedCapture, ::morph::effects::changes<lims::ResultView>,
                        ::morph::effects::changes<lims::SampleView>, ::morph::effects::changes<lims::ConflictView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListConflicts, "ListConflicts", ::morph::model::Loggable::No)
// Applying a conflict anyway applies its capture.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ResolveConflict, "ResolveConflict")
BRIDGE_REGISTER_EFFECTS(lims::ResolveConflict, ::morph::effects::changes<lims::ConflictView>,
                        ::morph::effects::changes<lims::ResultView>, ::morph::effects::changes<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GrantRole, "GrantRole")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::VerifyResult, "VerifyResult")
BRIDGE_REGISTER_EFFECTS(lims::VerifyResult, ::morph::effects::changes<lims::VerificationView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListVerifications, "ListVerifications", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetAuditTrail, "GetAuditTrail", ::morph::model::Loggable::No)

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
// The sample-entry actions edit this instance's working copy, which the entry screen's queries read.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::NewSample, "NewSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::NewSample, ::morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::LoadSample, "LoadSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::LoadSample, ::morph::effects::changesInstance)
// Saving stores the working copy as the sample.
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::SaveSample, "SaveSample")
BRIDGE_REGISTER_EFFECTS(lims::SaveSample, ::morph::effects::changesInstance,
                        ::morph::effects::changes<lims::SampleView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::DeleteSample, "DeleteSample")
BRIDGE_REGISTER_EFFECTS(lims::DeleteSample, ::morph::effects::changesInstance,
                        ::morph::effects::changes<lims::SampleView>, ::morph::effects::changes<lims::ResultView>)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::HeaderSection, "HeaderSection", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::HeaderSection, ::morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::SamplingSection, "SamplingSection", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::SamplingSection, ::morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::TurbiditySection, "TurbiditySection", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::TurbiditySection, ::morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::NitrateSection, "NitrateSection", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::NitrateSection, ::morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ClearOptionalMeasurement, "ClearOptionalMeasurement",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::ClearOptionalMeasurement, ::morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetHeader, "GetHeader", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetMeasurements, "GetMeasurements", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetDilutionSeries, "GetDilutionSeries", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListProjects, "ListProjects", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::SelectProject, "SelectProject", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::SelectProject, ::morph::effects::changesInstance)
// NOLINTEND(misc-const-correctness)

// `OpenSample` is the action that names a sample, so it defines the model's
// key. `BRIDGE_MODEL_KEY` deduces the key *type* from the member it is handed,
// which makes `PrimaryKeyOf<SampleModel>` `lims::SampleId` itself -- the strong
// id examples/IMPLEMENTATION.md rule 3 requires -- rather than an unwrapped
// `std::int64_t`: `morph::model::ModelKey` admits a strong id wrapping a raw
// key, so no hand-written key specialisation is needed.
//
// Unwrapping by hand -- `keyToString(*action.sampleId)` -- would apply
// `operator*` to a possibly-disengaged `std::optional`: undefined behaviour
// for an action carrying an empty id, handing back whatever the union held
// and routing the caller to an arbitrary instance. `morph::model::keyToString` refuses an
// empty strong id instead. Key extraction runs *before* `SampleModel::execute`
// ever sees the action (`BridgeHandler::execute`, include/morph/core/
// bridge.hpp), so this is the only place that rejection can happen -- and that
// call site's own `catch (...)` resolves the `Completion` with the error
// rather than letting it escape.
BRIDGE_MODEL_KEY(lims::SampleModel, lims::OpenSample, &lims::OpenSample::sampleId);

// A queued update names its own sample: it was prepared on a device that was
// not attached to anything, and by the time it replays the handler draining
// the queue may be attached to a different sample entirely.
BRIDGE_KEY_FROM(lims::QueuedCapture, &lims::QueuedCapture::sampleId);

// Registration generates the id rather than naming it, so its key comes off
// the `SampleView` it returns -- the result-keyed path, which promotes the
// instance the action already ran on instead of attaching before dispatch.
BRIDGE_KEY_FROM_RESULT(lims::RegisterSample, &lims::SampleView::id);
