// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>
#include <vector>

#include "polls/core/errors.hpp"
#include "polls/dto/event_dto.hpp"
#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"

/// @file
/// `PollModel` -- this rung's one entity-owning model, keyed by `pollId`
/// (`BRIDGE_MODEL_KEY` below, `BridgeHandler<PollModel, AllowShared>` at the
/// wiring layer).
///
/// Unlike `bookmarks::BookmarkModel`'s "declared once, complete" header
/// (`examples/bookmarks/include/bookmarks/models/bookmark_model.hpp`, which
/// pre-declares every `execute()` overload the whole rung ever adds, before
/// most of them have bodies), this header declares **only** the actions
/// implemented so far: Task 5's `CreatePoll`/`OpenPoll`/`GetPollState`,
/// Task 6's `SubmitVotes`/`UpdateVotes`/`AddComment`, plus Task 7's
/// `FinalizePoll` below.
/// Verified reason for the deviation, not a style choice: `BRIDGE_REGISTER_ACTION`
/// (`morph/core/registry.hpp`) unconditionally instantiates a static-init-time
/// registrar (`ActionExecuteRegistry::registerAction<Model, Action>`,
/// `morph/core/bridge.hpp`) whose stored lambda takes the *address* of
/// `Model::execute(Action)` -- unlike `ActionTraits<Action>::Result`'s
/// `decltype(...)` (declaration-only, never ODR-uses the body),
/// this registrar genuinely needs a linkable definition. Registering
/// `FinalizePoll`/`UndoLastVoteChange`/`GetEventsSince` here before Tasks
/// 7-9 give them bodies produced a real `ld: symbol(s) not found` failure
/// against this task's own test binary (confirmed by hand before this
/// header was written this way) -- so Tasks 7/8/9 each add their own
/// action's declaration **and** its `BRIDGE_REGISTER_ACTION` line to this
/// header alongside their own `.cpp` body, not just a `.cpp` change.
/// Task 9's `GetEventsSince` below is the last of these -- every action this
/// rung's DTOs declare now has a real `execute()` body.
///
/// Registered plain, not `AllowShared` at the *authorization* layer -- the
/// shared *instance* directory is what `AllowShared` opts into at the
/// wiring layer; what token gating exists is entirely this model's own job.
/// There is no framework authorizer for a bare shared-secret-per-entity
/// capability token (this rung's admin token), so `requireAdmin()`
/// hand-verifies `session::current()->token` against the poll row's own
/// `adminToken` column -- see the rung README's resolved design decision 1.
///
/// @par What is actually gated, stated exactly
/// **`execute(FinalizePoll)` is the only token-gated action in this model.**
/// Every other action -- `SubmitVotes`, `UpdateVotes`, `AddComment`,
/// `UndoLastVoteChange`, `GetPollState`, `GetEventsSince`, and the keyed
/// `OpenPoll` attach itself -- runs for any caller that can name the
/// `pollId`, with no token check of any kind. That is the design, not an
/// omission: `pollId` is a 22-character base64url encoding of 16 bytes of
/// `std::random_device` entropy (see `randomToken()` in this model's `.cpp`),
/// so knowing it *is* the capability, exactly as design decision 2 says
/// ("attaching to a poll by id is meant to be as open as knowing the link").
/// A participant gate on top of it would add no authority anyway: one
/// participant token is minted per *poll*, not per participant, so every
/// voter shares the same secret and it can distinguish no one from anyone.
///
/// `CreatePollResult::participantToken` is therefore generated, stored,
/// returned and displayed -- and verified by nothing. It is reserved for a
/// later rung that wants a second capability level the organizer can hand
/// out and revoke separately from the link itself; until such a rung exists,
/// no code reads it back. An earlier draft of this header carried a private
/// `requireParticipant()` helper "every later participant-gated action
/// reuses"; it had no call sites and has been removed rather than left to
/// imply a check that does not happen.

namespace polls {

/// @brief One scheduling poll: its options, votes, comments, and event log,
///        backed by SQLite via Lightweight. Keyed by `pollId` -- see the
///        `BRIDGE_MODEL_KEY` declaration below.
///
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning a connection for its own
/// lifetime -- including while this instance is shared across every
/// participant of the same poll (`AllowShared`, below): dispatched calls
/// against a shared instance are still serialized one at a time on its own
/// strand, so no two `execute()` calls ever contend for one acquisition.
class PollModel {
public:
    /// @brief Creates a poll with its candidate options.
    /// @param action Title and 2-20 bounded-label options.
    /// @return The generated `pollId`/`adminToken`/`participantToken`.
    CreatePollResult execute(const CreatePoll& action);

    /// @brief Attaches this handler to the poll named by `action.pollId` and
    ///        returns its full current state. The keyed attach action --
    ///        `BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId)`.
    /// @param action The poll's shareable link id.
    /// @return The poll's full current state.
    GetPollStateResult execute(const OpenPoll& action);

    /// @brief Returns the current state of the poll this handler was last
    ///        attached to via `execute(OpenPoll)`.
    /// @param action Carries no fields of its own.
    /// @return The poll's full current state.
    GetPollStateResult execute(const GetPollState& action);

    /// @brief First-time vote submission for `action.participantName` against
    ///        this handler's attached poll. Idempotent on retry: a duplicate
    ///        submission for the same participant is a replace, not a second
    ///        set of rows (`applyVotes()`'s delete-then-recreate, backed by
    ///        `votes`' own `(poll, participantName, option)` unique index --
    ///        see `poll_entity.hpp`).
    /// @param action The participant's display name and full vote set.
    /// @return The freshly-rebuilt state of this handler's attached poll.
    /// @throws ValidationError if `action.validate()` rejects the input.
    /// @throws Conflict if the poll is already finalized.
    GetPollStateResult execute(const SubmitVotes& action);

    /// @brief Replaces `action.participantName`'s votes wholesale against
    ///        this handler's attached poll. Same underlying write as
    ///        `execute(SubmitVotes)` (both go through `applyVotes()`) --
    ///        kept as a distinct action only so the event log records
    ///        "updated votes" rather than "submitted votes".
    /// @param action The participant's display name and full new vote set.
    /// @return The freshly-rebuilt state of this handler's attached poll.
    /// @throws ValidationError if `action.validate()` rejects the input.
    /// @throws Conflict if the poll is already finalized.
    GetPollStateResult execute(const UpdateVotes& action);

    /// @brief Adds one comment to this handler's attached poll. Writes no
    ///        `VoteHistoryRecord` -- comments are not undoable (only vote
    ///        *changes* are, matching `UndoLastVoteChange`'s own name).
    /// @param action The participant's display name and comment body.
    /// @return The freshly-rebuilt state of this handler's attached poll.
    /// @throws ValidationError if `action.validate()` rejects the input.
    /// @throws Conflict if the poll is already finalized (finalizing makes
    ///         a poll read-only -- see `FinalizePoll`'s own doc comment).
    GetPollStateResult execute(const AddComment& action);

    /// @brief Admin-token-gated state transition: marks this handler's
    ///        attached poll finalized with `action.optionId` as the winning
    ///        option. Makes the poll read-only for every future write (see
    ///        `SubmitVotes`/`UpdateVotes`/`AddComment`'s own `Conflict`
    ///        checks). The caller must present the poll's own admin token
    ///        in `session::current()->token` -- checked via `requireAdmin()`
    ///        **before** the poll's `finalized` state is even inspected, so
    ///        a caller with no token or the wrong (e.g. participant) token
    ///        learns nothing about whether the poll happens to already be
    ///        finalized (see this method's `.cpp` doc comment for why the
    ///        ordering matters).
    /// @param action The winning option's id.
    /// @return The freshly-rebuilt state of this handler's attached poll,
    ///         with `finalized == Finalized::Yes` and `finalizedOptionId` set.
    /// @throws ValidationError if `action.validate()` rejects the input.
    /// @throws Forbidden if the caller's token is not this poll's admin token.
    /// @throws Conflict if the poll is already finalized.
    GetPollStateResult execute(const FinalizePoll& action);

    /// @brief Reverses `action.participantName`'s own most recent vote
    ///        change against this handler's attached poll -- a
    ///        principal-scoped **compensating action**, not
    ///        `SessionLog::undoLast()` (see this rung's README, resolved
    ///        design decision 3, and this method's own `.cpp` doc comment
    ///        for the headline design record this task exists to produce).
    ///        Reads `db::VoteHistoryRecord`'s most recent row for
    ///        `(pollId, action.participantName)`, restores the vote set it
    ///        captured via the same delete-then-recreate write `applyVotes()`
    ///        (Task 6) already implements -- passing `WriteHistory::No` so
    ///        the restore itself writes no new history row -- then deletes
    ///        that one consumed row inside the very same transaction as the
    ///        restore write: undo is one-shot, not a redo stack, and there is
    ///        no window where the restore is committed but the consumed row
    ///        (or a spurious new one) still exists.
    /// @param action The participant whose own most recent vote change is undone.
    /// @return `.restored == Restored::Yes` on success (`Conflict` is thrown
    ///         instead of ever returning `Restored::No` -- see the field's
    ///         own doc comment in `vote_dto.hpp`).
    /// @throws ValidationError if `action.validate()` rejects the input.
    /// @throws NotFound if this handler was never attached via `OpenPoll`.
    /// @throws Conflict if `action.participantName` has no vote-history entry
    ///         left to undo for this poll (never voted, or already undone).
    /// @throws Conflict if the poll is already finalized.
    UndoLastVoteChangeResult execute(const UndoLastVoteChange& action);

    /// @brief Lists every `PollEvent` recorded for this handler's attached
    ///        poll strictly after @p action.lastEventId -- the Zulip-pattern
    ///        event log's read side. `action.lastEventId == PollEventId{}`
    ///        (its default) means "from the beginning": `poll_events.id` is a
    ///        SQLite `ServerSideAutoIncrement` primary key, which starts at 1,
    ///        so `WHERE id > 0` already matches every row with no special
    ///        case needed. Oldest-first, ascending by id -- the opposite
    ///        direction and full-result-set counterpart of `buildState()`'s
    ///        own `lastEvent` lookup (`.OrderBy(id, DESCENDING).First()`),
    ///        which this method mirrors for its query shape
    ///        (`Where(poll=...).Where(id > ...)`) but not its ordering or
    ///        cardinality.
    ///
    ///        Durable persistence alone closes the Zulip-pattern gap this
    ///        rung's README documents as design decision 4: the event log
    ///        survives this handler's own destruction/rebirth (a fresh
    ///        `PollModel` reading the same `poll_events` table sees every row
    ///        a now-gone instance wrote), and a stale cursor simply gets
    ///        every real event since it -- no epoch token needed, because the
    ///        table-wide autoincrement `id` never resets or repeats across
    ///        instance lifetimes.
    /// @param action Carries `lastEventId`, the caller's cursor.
    /// @return Every event with `id > action.lastEventId`, oldest first.
    /// @throws ValidationError if `action.validate()` rejects the input.
    /// @throws NotFound if this handler was never attached via `OpenPoll`.
    GetEventsSinceResult execute(const GetEventsSince& action);

private:
    /// @brief Throws `Forbidden` unless `session::current()->token` equals
    ///        @p adminToken. Takes the already-decoded token rather than a
    ///        `db::PollRecord&` deliberately: the entity is an
    ///        implementation detail of this TU (this header exposes only
    ///        DTOs -- see `pastebin::PasteModel`'s identical `paste_model.hpp`
    ///        precedent), so callers in `poll_model.cpp` pass
    ///        `AdminToken{textOf(poll.adminToken.Value())}`.
    /// @param adminToken The poll's stored admin token, decoded to text and
    ///        wrapped in its own opaque newtype (`dto/poll_dto.hpp`) so a
    ///        `ParticipantToken` can never be passed here by mistake.
    void requireAdmin(const AdminToken& adminToken) const;

    /// @brief Whether `applyVotes()` should append a `VoteHistoryRecord`
    ///        capturing the pre-change vote set it is about to replace.
    ///
    ///        A strong type instead of a bare `bool` so call sites read as
    ///        intent (`WriteHistory::No`) rather than an unexplained `false`
    ///        -- same convention as `morph::model::Loggable`
    ///        (`morph/core/registry.hpp`).
    ///
    ///        `SubmitVotes`/`UpdateVotes` pass `WriteHistory::Yes`: their
    ///        history row is `UndoLastVoteChange`'s normal data source.
    ///        `execute(UndoLastVoteChange)`'s own restore call passes
    ///        `WriteHistory::No` -- writing a history row for a restore
    ///        would let a second undo call "undo the undo", turning a
    ///        one-shot compensating action into an unbounded ping-pong.
    enum class WriteHistory : std::uint8_t { No, Yes };

    /// @brief Shared body of `execute(SubmitVotes)`/`execute(UpdateVotes)`/
    ///        `execute(UndoLastVoteChange)`: loads this handler's attached
    ///        poll, throws `Conflict` if it is finalized, then -- inside one
    ///        transaction -- deletes @p participantName's prior vote rows
    ///        for this poll (if any), writes one fresh `VoteRecord` per
    ///        @p votes entry, appends a `VoteHistoryRecord` capturing the
    ///        pre-change vote set if @p writeHistory is `WriteHistory::Yes`,
    ///        deletes the `VoteHistoryRecord` row named by
    ///        @p historyRowIdToDelete if set, and appends a
    ///        `PollEventRecord` whose summary embeds @p summaryVerb --
    ///        all inside that same one transaction, which is exactly why
    ///        @p historyRowIdToDelete exists as a parameter here rather than
    ///        being deleted by the caller afterward: it lets
    ///        `execute(UndoLastVoteChange)` fold its own history-row cleanup
    ///        into this same commit instead of opening a second transaction
    ///        that could fail independently, after the restore has already
    ///        landed. Takes only DTO-shaped/primitive parameters, never a
    ///        `db::PollRecord&`/`db::VoteHistoryRecord&` -- this header
    ///        exposes only DTOs (see the file comment).
    /// @param participantName The (unauthenticated) participant's display name.
    /// @param votes The participant's full new vote set -- replaces, never merges.
    /// @param summaryVerb Event-summary verb distinguishing the callers:
    ///        `"submitted votes"` for `SubmitVotes`, `"updated votes"` for
    ///        `UpdateVotes`, `"undid their last vote change"` for
    ///        `UndoLastVoteChange`.
    /// @param writeHistory Whether to append a fresh `VoteHistoryRecord` for
    ///        this write. See `WriteHistory`'s own doc comment above.
    /// @param historyRowIdToDelete If set, the primary-key id of one
    ///        `VoteHistoryRecord` row to delete inside this same transaction
    ///        -- `execute(UndoLastVoteChange)` passes the id of the history
    ///        row it just consumed, so the restore write and that row's
    ///        deletion commit together or not at all.
    /// @return The freshly-rebuilt state of this handler's attached poll.
    /// @throws Conflict if the poll is already finalized.
    GetPollStateResult applyVotes(const std::string& participantName, const std::vector<OneVote>& votes,
                                  const std::string& summaryVerb, WriteHistory writeHistory,
                                  std::optional<std::uint64_t> historyRowIdToDelete = std::nullopt);

    /// @brief The poll this handler is attached to, cached on the first
    ///        successful `execute(OpenPoll)`. Unset until then -- reading it
    ///        from `execute(GetPollState)` before any `OpenPoll` attach is a
    ///        caller error (see that method's `.cpp` doc comment).
    std::optional<std::string> _pollId;
};

}  // namespace polls

BRIDGE_REGISTER_MODEL(polls::PollModel, "PollModel")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::CreatePoll, "CreatePoll")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::OpenPoll, "OpenPoll", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::GetPollState, "GetPollState", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::SubmitVotes, "SubmitVotes")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::UpdateVotes, "UpdateVotes")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::AddComment, "AddComment")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::FinalizePoll, "FinalizePoll")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::UndoLastVoteChange, "UndoLastVoteChange")
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::GetEventsSince, "GetEventsSince", ::morph::model::Loggable::No)

// PollModel is keyed by OpenPoll::pollId -- deferred from Task 3 to here per
// that task's own review (matching docs/spec/core/shared_instances.md's
// worked example and examples/bank's two keyed-model precedents,
// account_model.hpp/customer_model.hpp, both placing this macro immediately
// after the model's own BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION block).
BRIDGE_MODEL_KEY(polls::PollModel, polls::OpenPoll, &polls::OpenPoll::pollId);
