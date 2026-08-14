// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "polls/dto/event_dto.hpp"
#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"

#include <exception>
#include <string>

// See pastebin::gui::PastePresenter's identical guard and doc comment
// (examples/pastebin/gui_lib/paste_presenter.hpp) for why moc must never see
// morph/core/bridge.hpp: its template machinery produces bogus moc output
// the same way poll_model.hpp historically did when it transitively pulled
// in Lightweight's DataMapper machinery through the since-removed
// polls/db/db_model.hpp -- poll_model.hpp itself no longer has any
// Lightweight/ODBC dependency at all, now that PollModel acquires a
// connection per execute() call from Lightweight::GlobalDataMapperPool()
// instead of owning one, but this guard stays for bridge.hpp's own sake.
#ifndef Q_MOC_RUN
#include "polls/models/poll_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace polls::gui {

/// @brief Routes every `PollModel` action through two `BridgeHandler`s.
///        Translates and routes only — no domain logic
///        (`IMPLEMENTATION.md` rule 2).
///
/// Two handlers, not one — this is the one real subtlety this presenter has
/// to get right, and getting it wrong fails every action at runtime with
/// "handler not bound" (confirmed empirically before this file settled on
/// the shape below):
///
///  - `_creator`, a plain (`NoSharing`) `BridgeHandler<PollModel>`, used
///    only by `createPoll`. `CreatePoll` carries no key of its own — it is
///    not `OpenPoll`, this rung's one `BRIDGE_MODEL_KEY`-registered action
///    (`poll_model.hpp`) — so dispatching it lands in
///    `BridgeHandler::execute`'s final, un-keyed `else` branch
///    (`morph/core/bridge.hpp`), which requires `_binding` to already be
///    bound to *some* instance. A plain handler satisfies that by
///    registering its own private instance eagerly at construction; an
///    `AllowShared` handler deliberately does not (`AllowShared`'s own doc
///    comment: "A shared handler that only ever runs *keyless* actions
///    never attaches, and its `execute` fails fast with 'handler not
///    bound'"). Mirrors `test_app.cpp`'s/`test_shared_instance_lifecycle.cpp`'s
///    own two-handler precedent (their `creator`, a plain `BridgeHandler<PollModel>`,
///    used identically).
///  - `_handler`, a `BridgeHandler<PollModel, AllowShared>`, used by every
///    other action. `PollModel` is keyed by `pollId`
///    (`BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId)`,
///    `poll_model.hpp`) — this rung's shared-instance showcase — so this
///    handler must join the shared instance directory the same way
///    `test_app.cpp`'s/`test_shared_instance_lifecycle.cpp`'s own `viewer`/
///    `handler` do, or `openPoll`'s keyed attach below fails to bind to (or
///    create) the poll's shared instance at all.
class PollPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    PollPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Creates a new poll. Emits `created` on success, `failed` on error.
    /// @param action The poll's title and candidate options.
    void createPoll(CreatePoll action);

    /// @brief Convenience wrapper around the keyed attach action —
    ///        dispatches `OpenPoll{.pollId = pollId}` (`handler_.execute`'s
    ///        payload-keyed attach) rather than requiring the caller to
    ///        build the DTO itself, since `pollId` is `OpenPoll`'s only
    ///        field. Attaches this handler to the named poll and returns its
    ///        full current state. Emits `opened` on success, `failed` on
    ///        error.
    ///
    ///        Task 15's polling helper drives its first `GetEventsSince`
    ///        call off this method's `opened` signal (`.lastEventId` in the
    ///        returned `GetPollStateResult` is exactly the starting cursor
    ///        `getEventsSince()` below needs) — that timer wiring is Task
    ///        15's own job; this method only exposes the primitive.
    /// @param pollId The poll's shareable link id.
    void openPoll(std::string pollId);

    /// @brief Returns the current state of the poll this handler was last
    ///        attached to via `openPoll`. Emits `stateLoaded` on success,
    ///        `failed` on error.
    /// @param action Carries no fields of its own.
    void getPollState(GetPollState action);

    /// @brief First-time vote submission for a participant against this
    ///        handler's attached poll. Emits `votesSubmitted` on success,
    ///        `failed` on error.
    /// @param action The participant's display name and full vote set.
    void submitVotes(SubmitVotes action);

    /// @brief Replaces a participant's votes wholesale against this
    ///        handler's attached poll. Emits `votesUpdated` on success,
    ///        `failed` on error.
    /// @param action The participant's display name and full new vote set.
    void updateVotes(UpdateVotes action);

    /// @brief Adds one comment to this handler's attached poll. Emits
    ///        `commentAdded` on success, `failed` on error.
    /// @param action The participant's display name and comment body.
    void addComment(AddComment action);

    /// @brief Admin-token-gated: marks this handler's attached poll
    ///        finalized. Emits `finalized` on success, `failed` on error.
    /// @param action The winning option's id.
    void finalizePoll(FinalizePoll action);

    /// @brief Reverses a participant's own most recent vote change against
    ///        this handler's attached poll. Emits `voteChangeUndone` on
    ///        success, `failed` on error.
    /// @param action The participant whose own most recent vote change is undone.
    void undoLastVoteChange(UndoLastVoteChange action);

    /// @brief Lists every event recorded for this handler's attached poll
    ///        strictly after `action.lastEventId`. Emits `eventsReceived` on
    ///        success, `failed` on error.
    ///
    ///        This method exposes the primitive Task 15's polling helper
    ///        drives on a timer — this task builds only the primitive, not
    ///        the timer/polling loop itself (see this rung's task brief).
    /// @param action Carries `lastEventId`, the caller's cursor.
    void getEventsSince(GetEventsSince action);

  signals:
    void created(CreatePollResult result);
    void opened(GetPollStateResult result);
    void stateLoaded(GetPollStateResult result);
    void votesSubmitted(GetPollStateResult result);
    void votesUpdated(GetPollStateResult result);
    void commentAdded(GetPollStateResult result);
    void finalized(GetPollStateResult result);
    void voteChangeUndone(UndoLastVoteChangeResult result);
    void eventsReceived(GetEventsSinceResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

  private:
    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument below — see `Presenter::track()`'s doc comment
    ///        (`examples/common/gui/presenter.hpp`) for why it is passed as
    ///        `track()`'s `onErr` parameter rather than attached via a
    ///        separate `.onError()` call beforehand.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<PollModel> _creator;
    ::morph::bridge::BridgeHandler<PollModel, ::morph::bridge::AllowShared> _handler;
};

}  // namespace polls::gui
