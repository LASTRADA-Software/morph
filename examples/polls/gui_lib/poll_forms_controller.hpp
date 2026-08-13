// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "polls/models/poll_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>

#include <array>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace polls::gui {

/// @brief Owns the *one* `BridgeHandler<PollModel, AllowShared>` a vote-view
///        screen dispatches every already-open-poll action through, and
///        exposes both the schema-driven `submitIfValid` surface
///        `bookmarks::gui::BookmarkFormsController` established and the
///        typed convenience methods that surface cannot cover.
///
/// @par Why this is not a verbatim copy of `BookmarkFormsController`
/// `BookmarkFormsController` owns one `BridgeHandler` *per model* (three, for
/// three models) precisely because `BookmarkModel`/`TagModel`/`AuthModel` are
/// all plain (`NoSharing`) — each handler registers its own private instance
/// eagerly at construction, so which handler object serves a given call
/// never matters. `PollModel` is different: it is `AllowShared` and keyed by
/// `pollId` (`poll_model.hpp`'s own doc comment; this rung's shared-instance
/// showcase). An `AllowShared` handler starts **unattached** and only joins
/// the poll's shared instance the first time a payload-keyed action
/// (`OpenPoll`) dispatches through *that specific handler object* — every
/// other action on the same poll must reuse that exact handler, or it hits
/// "handler not bound" (no instance to run against). A second, independently
/// constructed `BridgeHandler<PollModel, AllowShared>` — as
/// `BookmarkFormsController`'s per-model shape would produce if copied
/// verbatim — would need its *own* `OpenPoll` attach before anything routed
/// through it could work, doubling the shared instance's live attachment
/// count for no benefit and, worse, silently failing every call issued
/// before that second attach completed. So this class owns exactly one
/// `_handler`, and every method below — schema-driven or typed — dispatches
/// through it.
///
/// @par Why `openPoll`/`submitVotes`/`updateVotes`/`getEventsSince` are not schema-driven
/// - `openPoll`: `OpenPoll` is this rung's one payload-keyed action.
///   Dispatching a payload-keyed action via the generic
///   `BridgeHandler::executeJson` path silently skips the attach step
///   entirely on an `AllowShared` handler — see
///   `docs/findings/034-executejson-skips-payload-keyed-attach-for-allowshared-handlers.md`,
///   found while building this class. `openPoll()` below calls the
///   templated `execute<OpenPoll>()` directly instead, which resolves the
///   real `AllowShared` attach branch at compile time.
/// - `submitVotes`/`updateVotes`: `SubmitVotes::votes`/`UpdateVotes::votes`
///   are `std::vector<OneVote>` — a JSON `array` field `DynamicForm` cannot
///   render (finding 031). `gui/qml/VoteView.qml` drives these from a
///   hand-rolled picker; the two methods below give that picker's C++-side
///   adapter (`PollBridge`) a `Completion`-returning call to attach its own
///   `.then()`/`.onError()` to, on the same attached `_handler`.
/// - `getEventsSince`: exists **only** for `morph::ladder::gui::EventPoller`'s
///   `Dispatch` closure (see that class's own doc comment's "production-safe
///   wiring" section) — never called directly by QML. It deliberately
///   returns a fresh `Completion<GetEventsSinceResult>` per call rather than
///   routing through any shared signal, so concurrent ticks/actions on this
///   same `_handler` can never cross-attribute a failure (each `execute<T>()`
///   call gets its own independent `CompletionState`; nothing here is
///   multiplexed the way a `Presenter`'s signals are).
///
/// @par `PollPresenter` is intentionally not reused here
/// `PollPresenter` (`poll_presenter.hpp`) already threads one shared
/// `_handler` correctly across `openPoll`/`submitVotes`/.../`getEventsSince`
/// — but only via `void` methods that report exclusively through Qt
/// signals, one of which (`failed(QString)`) is shared by all nine actions.
/// Building a generic per-call `submitIfValid(actionType, body, onReply,
/// onError)` on top of that would mean temporarily connecting `onReply`/
/// `onError` to those shared signals per call, reproducing exactly the
/// cross-attribution hazard `EventPoller`'s own doc comment warns against
/// for the identical reason. This class instead owns its own handler and
/// gets a genuine per-call `Completion` for every dispatch, `PollPresenter`
/// included nowhere in its implementation. `PollPresenter` remains the right
/// tool for `PollBridge::createPoll` (a `NoSharing` handler, no attachment
/// story to preserve), which is the one thing this class does not cover.
class PollFormsController {
  public:
    /// @param bridge      The shared `Bridge` `AppContext` owns.
    /// @param executor    The executor `Completion` callbacks land on.
    /// @param schemasJson Pre-assembled `{actionType: schemaJson<A>()}` map
    ///        — `poll_schemas.hpp`'s `pollSchemasJson()` builds the one every
    ///        shell passes.
    PollFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, std::string schemasJson);

    /// @brief The `{actionType: schema}` JSON supplied at construction.
    /// @return A reference to the cached schema-set JSON.
    [[nodiscard]] const std::string& schemasJson() const noexcept { return _schemasJson; }

    /// @brief Dispatches @p bodyJson as @p actionType's body via
    ///        `BridgeHandler::executeJson`, invoking @p onReply / @p onError
    ///        on the GUI thread once the reply arrives.
    ///
    /// @p actionType must be one of `kSchemaActions` below (`AddComment`,
    /// `FinalizePoll`, `UndoLastVoteChange`) — every other `PollModel` action
    /// is still registered on `_handler` (every action shares one model's
    /// handler here) but is deliberately refused by this method rather than
    /// silently mis-dispatched: `OpenPoll` in particular would hit finding
    /// 034 if it ever reached `executeJson` by mistake.
    ///
    /// @tparam OnReply Callable invoked with the result JSON (`std::string`) on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param actionType One of `kSchemaActions`.
    /// @param bodyJson   Fully-assembled JSON body for the action.
    /// @param onReply    Success callback.
    /// @param onError    Failure callback.
    template <typename OnReply, typename OnError>
    void submitIfValid(std::string actionType, std::string bodyJson, OnReply onReply, OnError onError) {
        if (std::ranges::find(kSchemaActions, actionType) == kSchemaActions.end()) {
            onError(std::make_exception_ptr(std::runtime_error{
                "PollFormsController::submitIfValid: '" + actionType +
                "' is not a schema-driven action (see poll_schemas.hpp / this class's own doc comment)"}));
            return;
        }
        _handler.executeJson(actionType, bodyJson)
            .then([onReply = std::move(onReply)](std::string resultJson) mutable { onReply(std::move(resultJson)); })
            .onError([onError = std::move(onError)](const std::exception_ptr& err) mutable { onError(err); });
    }

    /// @brief Attaches `_handler` to the poll named by @p pollId and returns
    ///        its full current state. See this class's own doc comment for
    ///        why this bypasses `submitIfValid` entirely.
    /// @param pollId The poll's shareable link id.
    /// @return Completion resolving with the poll's full current state.
    [[nodiscard]] ::morph::async::Completion<GetPollStateResult> openPoll(std::string pollId);

    /// @brief Returns the current state of the poll `_handler` is attached
    ///        to. A plain refresh — `GetPollState` carries no fields a
    ///        person types, so it is not part of the schema document.
    /// @return Completion resolving with the poll's full current state.
    [[nodiscard]] ::morph::async::Completion<GetPollStateResult> getPollState();

    /// @brief First-time vote submission. See this class's own doc comment
    ///        for why `SubmitVotes` is not schema-driven.
    /// @param action The participant's display name and full vote set.
    /// @return Completion resolving with the freshly-rebuilt poll state.
    [[nodiscard]] ::morph::async::Completion<GetPollStateResult> submitVotes(SubmitVotes action);

    /// @brief Replaces a participant's votes wholesale. See this class's own
    ///        doc comment for why `UpdateVotes` is not schema-driven.
    /// @param action The participant's display name and full new vote set.
    /// @return Completion resolving with the freshly-rebuilt poll state.
    [[nodiscard]] ::morph::async::Completion<GetPollStateResult> updateVotes(UpdateVotes action);

    /// @brief Lists every event recorded on the attached poll strictly after
    ///        @p action.lastEventId. Exists only for
    ///        `morph::ladder::gui::EventPoller`'s `Dispatch` closure — see
    ///        this class's own doc comment.
    /// @param action Carries `lastEventId`, the poller's current cursor.
    /// @return Completion resolving with the events, oldest first.
    [[nodiscard]] ::morph::async::Completion<GetEventsSinceResult> getEventsSince(GetEventsSince action);

    /// @brief The three action-type ids `submitIfValid` accepts, matching
    ///        `poll_schemas.hpp`'s document exactly.
    static constexpr std::array<std::string_view, 3> kSchemaActions{"AddComment", "FinalizePoll",
                                                                     "UndoLastVoteChange"};

  private:
    ::morph::bridge::BridgeHandler<PollModel, ::morph::bridge::AllowShared> _handler;
    std::string _schemasJson;
};

}  // namespace polls::gui
