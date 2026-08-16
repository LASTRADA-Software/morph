// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

// Guarded exactly like bookmark_qml_bridges.hpp's own includes: AUTOMOC runs
// moc over this header, and moc must not be pointed at morph's template-heavy
// bridge.hpp or event_poller.hpp — see poll_presenter.hpp's identical guard
// and doc comment for the full rationale.
#ifndef Q_MOC_RUN
#include "gui/event_poller.hpp"
#include "poll_forms_controller.hpp"
#include "poll_presenter.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

/// @file
/// `PollBridge` — the one QML-facing adapter this rung's GUI shell needs,
/// mirroring bookmarks' `FormsBridge`/`BookmarkBridge` split folded into a
/// single class: this rung has exactly one model (`PollModel`), so splitting
/// "the schema-driven forms adapter" from "the domain adapter" the way
/// bookmarks does for its three models would only add a second class with
/// nothing of its own to route between. See `poll_forms_controller.hpp`'s
/// own doc comment for why `PollBridge` wraps *both* `PollFormsController`
/// (every already-open-poll action) and `PollPresenter` (`createPoll` only,
/// which needs no attachment story) rather than either alone.

namespace polls::gui {

/// @brief QML-facing face of `PollFormsController`/`PollPresenter`, plus the
///        one `morph::ladder::gui::EventPoller<PollEvent, PollEventId>` a
///        vote view owns while a poll is open.
///
/// Same surface `DynamicForm.qml` expects of a controller — a `schemasJson`
/// property, `submitIfValid(actionType, bodyJson)`, and a `replyReceived`
/// signal — for `AddComment`/`FinalizePoll`/`UndoLastVoteChange`. Every other
/// action (`createPoll`, `openPoll`, `refresh`, `submitVotes`/`updateVotes`)
/// is a dedicated invokable, because none of them are schema-driven (see
/// `poll_schemas.hpp`'s own doc comment for why, action by action).
///
/// @par Member declaration order is load-bearing
/// `_forms` must be declared **before** `_poller`. `EventPoller`'s own doc
/// comment establishes that destroying an `EventPoller` mid-tick is safe
/// (its `_liveness` token — its own last-declared member — is destroyed
/// first, so a completion callback that arrives afterward finds
/// `alive.expired() == true` and no-ops before touching anything else). That
/// guarantee only protects the `EventPoller` object itself; the *dispatch*
/// closure `startPolling()` builds below also calls back into `_forms`
/// (`PollFormsController::getEventsSince`), so `_forms`'s own
/// `BridgeHandler` must still be alive for as long as `_poller` might still
/// be mid-teardown. Members are destroyed in reverse declaration order, so
/// declaring `_forms` first — and therefore destroying it *after* `_poller`
/// — is what makes that true. Reordering the two members reintroduces a
/// use-after-free identical in shape to the one `EventPoller`'s own C1 fix
/// round closed (see this rung's `progress.md`, Task 15).
class PollBridge : public QObject {
    Q_OBJECT

    /// @brief `{actionType: schema}` JSON for `AddComment`/`FinalizePoll`/
    ///        `UndoLastVoteChange` — everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    PollBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief The schema document supplied to the wrapped
    ///        `PollFormsController` (`poll_schemas.hpp`).
    /// @return `{actionType: schema}` JSON.
    [[nodiscard]] QString schemasJson() const;

    /// @brief Creates a new poll. Native-client-only (this rung's Global
    ///        Constraints — see `examples/polls/README.md`); nothing in this
    ///        method itself enforces that, `gui/qml/Main.qml`'s own
    ///        `nativeClient` gate does. Emits `created` on success, `failed`
    ///        on error.
    /// @param title         The poll's title.
    /// @param optionLabels  Candidate option labels, in order — driven by
    ///        `CreatePollView.qml`'s hand-written list editor, a workaround
    ///        for `DynamicForm`'s array-field control only handling
    ///        arrays of strings, not `CreatePollOption` objects; see
    ///        `poll_schemas.hpp`.
    Q_INVOKABLE void createPoll(const QString& title, const QVariantList& optionLabels);

    /// @brief Attaches to the poll named by @p pollId and starts the
    ///        `EventPoller` ticking `GetEventsSince` on it. Emits `opened` on
    ///        success, `failed` on error.
    /// @param pollId The poll's shareable link id.
    Q_INVOKABLE void openPoll(const QString& pollId);

    /// @brief Re-reads the attached poll's full current state. Emits
    ///        `stateChanged` on success, `failed` on error.
    Q_INVOKABLE void refresh();

    /// @brief First-time vote submission. Emits `stateChanged` on success,
    ///        `failed` on error.
    /// @param participantName The voter's display name.
    /// @param votes           `{optionId, choice}` maps — `choice` one of
    ///        `"Yes"`/`"IfNeedBe"`/`"No"`, matching `VoteView.qml`'s picker.
    Q_INVOKABLE void submitVotes(const QString& participantName, const QVariantList& votes);

    /// @brief Replaces a participant's votes wholesale. Emits `stateChanged`
    ///        on success, `failed` on error.
    /// @param participantName The voter's display name.
    /// @param votes           Same shape as `submitVotes`.
    Q_INVOKABLE void updateVotes(const QString& participantName, const QVariantList& votes);

    /// @brief Installs @p token as the shared `Bridge`'s default session
    ///        token — this rung's whole admin identity (`FinalizePoll`'s
    ///        `requireAdmin()` compares it against the poll's stored admin
    ///        token; see `examples/polls/README.md`'s resolved design
    ///        decision 1). Every other action needs no token at all.
    /// @param token The poll's admin token, as `CreatePollResult` returned it.
    Q_INVOKABLE void setAdminToken(const QString& token);

    /// @brief Dispatches @p bodyJson as @p actionType's body through
    ///        `PollFormsController::submitIfValid` — `AddComment`,
    ///        `FinalizePoll` or `UndoLastVoteChange` only (see that
    ///        method's own doc comment). Emits `replyReceived` when the
    ///        reply (or the error) arrives.
    /// @param actionType One of `PollFormsController::kSchemaActions`.
    /// @param bodyJson   Fully-assembled JSON body, as `DynamicForm` builds it.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

    /// @brief Stops the `EventPoller`'s timer without treating it as a fatal
    ///        error — a vote view calls this when it is hidden/closed. A
    ///        no-op if no poll is currently open.
    Q_INVOKABLE void stopPolling();

  signals:
    /// @brief `createPoll` succeeded. @p result carries `pollId`,
    ///        `adminToken`, `participantToken`.
    /// @param result The new poll's identifiers, as a property bag.
    void created(const QVariantMap& result);

    /// @brief `openPoll` succeeded and polling has started. @p state is the
    ///        poll's full current state.
    /// @param state The poll's state, as a property bag.
    void opened(const QVariantMap& state);

    /// @brief `refresh`/`submitVotes`/`updateVotes` succeeded, or an
    ///        applied live event triggered a resync. @p state is the poll's
    ///        full current state.
    /// @param state The poll's state, as a property bag.
    void stateChanged(const QVariantMap& state);

    /// @brief One `PollEvent` the `EventPoller` just applied — for a live
    ///        activity log. Never itself a source of tally updates (`kind`/
    ///        `summary` carry no vote counts); `stateChanged` follows
    ///        shortly after, debounced, for that.
    /// @param event `{id, kind, summary}`.
    void eventReceived(const QVariantMap& event);

    /// @brief One `AddComment`/`FinalizePoll`/`UndoLastVoteChange` reply.
    /// @param actionType The action the reply belongs to.
    /// @param ok         Whether the dispatch succeeded.
    /// @param payload    Result JSON, or the error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

    /// @brief The `EventPoller` stopped for good (a non-timeout failure —
    ///        e.g. a stale cursor after the poll's event log was pruned in
    ///        a way this rung never actually does, or the poll no longer
    ///        exists). Polling does not resume on its own; the view should
    ///        show this and let the user re-open the poll.
    /// @param message What `EventPoller::OnFatalError` reported.
    void pollingStopped(const QString& message);

    /// @brief Any of `createPoll`/`openPoll`/`refresh`/`submitVotes`/
    ///        `updateVotes`'s failures, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

  private:
#ifndef Q_MOC_RUN
    using Poller = ::morph::ladder::gui::EventPoller<PollEvent, PollEventId>;

    /// @brief Builds and starts `_poller` against the just-opened poll. Its
    ///        `Dispatch` closure reuses `_forms`'s already-attached handler
    ///        via `PollFormsController::getEventsSince` — see this class's
    ///        own doc comment for why a *second*, independently-attached
    ///        handler is deliberately not used here.
    ///
    /// Constructs `Poller` with no interval/deadline override, so the real
    /// unscaled `Poller::kDefaultExecuteDeadline` is always armed — see that
    /// constant's own doc comment (`event_poller.hpp`) for the CI-flakiness
    /// risk this carries under a scaled `MORPH_LADDER_DEADLINE_MS` run, and
    /// why it is not "fixed" here by exposing an override on this adapter.
    /// @param cursor The starting cursor — `GetPollStateResult::lastEventId`
    ///        from the `openPoll` call that just succeeded.
    void startPolling(PollEventId cursor);

    /// @brief `_poller`'s `ApplyEvent`: relays @p event as `eventReceived`
    ///        and schedules a debounced `refresh()`.
    /// @param event One event `_poller` just applied.
    void onEventApplied(const PollEvent& event);
#endif

    PollPresenter _presenter;
    PollFormsController _forms;
    std::unique_ptr<Poller> _poller;
    ::morph::bridge::Bridge& _bridge;
    ::morph::exec::IExecutor* _executor;
    /// @brief Debounces `stateChanged` after a burst of applied events in
    ///        one poll tick — see `.cpp`'s `onEventApplied`.
    QTimer _refreshDebounce;

    /// @brief Weak-observable proof this object still exists.
    ///
    /// `PollBridge` is a `QObject`, but its `.then()`/`.onError()` completion
    /// callbacks (`openPoll`, `refresh`, `submitVotes`, `updateVotes`,
    /// `submitIfValid`, `startPolling`'s `Dispatch`) are plain
    /// `std::function`-based `Completion<T>` continuations, not
    /// `QObject::connect`-based signal/slot connections — Qt's own
    /// auto-disconnect-on-destruction machinery does not apply to them at
    /// all. Every one of those callbacks captures raw `this`; destroying a
    /// `PollBridge` while any of them is still in flight (an ordinary GUI
    /// case — a view closing mid-request) would otherwise write into freed
    /// memory. Same pattern, same reasoning, and the same **must remain the
    /// last declared member** requirement as
    /// `morph::ladder::gui::EventPoller::_liveness`
    /// (`examples/common/gui/event_poller.hpp`) and
    /// `morph::bridge::Bridge::_liveness` (`include/morph/core/bridge.hpp`):
    /// members are destroyed in reverse declaration order, so the
    /// last-declared member is destroyed first, and the weak_ptr each
    /// callback captures observes that before anything else it might touch
    /// has been torn down.
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

}  // namespace polls::gui
