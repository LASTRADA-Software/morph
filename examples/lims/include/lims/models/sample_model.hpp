// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/core/self_journal.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
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
    /// implementation happens to dedup at enqueue time (morph#175).
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
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RegisterClient, "RegisterClient")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RegisterSample, "RegisterSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::OpenSample, "OpenSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetSample, "GetSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ReceiveSample, "ReceiveSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::StartWork, "StartWork")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::SubmitForVerification, "SubmitForVerification")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ReturnForRework, "ReturnForRework")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::PublishSample, "PublishSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RejectSample, "RejectSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::CaptureConcentration, "CaptureConcentration")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListResults, "ListResults", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListResultQualifiers, "ListResultQualifiers",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListDilutionModes, "ListDilutionModes", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::QueuedCapture, "QueuedCapture")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListConflicts, "ListConflicts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ResolveConflict, "ResolveConflict")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GrantRole, "GrantRole")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::VerifyResult, "VerifyResult")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListVerifications, "ListVerifications", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetAuditTrail, "GetAuditTrail", ::morph::model::Loggable::No)

// `OpenSample` is the action that names a sample, so it defines the model's
// key. `BRIDGE_MODEL_KEY` deduces the key *type* from the member it is handed,
// which makes `PrimaryKeyOf<SampleModel>` `lims::SampleId` itself -- the strong
// id examples/IMPLEMENTATION.md rule 3 requires -- rather than the unwrapped
// `std::int64_t` this rung declared by hand while `morph::model::ModelKey`
// still admitted only `std::integral`/`std::string`. That restriction is what
// forced the hand-written specialisations here, in `ledger::LedgerModel` and
// in `kanban::BoardModel`; morph#163 widened the concept to admit a strong id
// wrapping a raw key, and morph#183 deleted all three rungs' blocks.
//
// Their bodies did `keyToString(*action.sampleId)` -- `operator*` on a
// possibly-disengaged `std::optional`, undefined behaviour for an action
// carrying an empty id, which handed back whatever the union held and routed
// the caller to an arbitrary instance. `morph::model::keyToString` refuses an
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
