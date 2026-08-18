// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <string>

// Guarded exactly like board_presenter.hpp's own includes: AUTOMOC runs moc
// over this header, and moc must not be pointed at morph's template-heavy
// bridge.hpp or event_poller.hpp — see that header's own doc comment for the
// full rationale (mirrors poll_qml_bridges.hpp's identical guard).
#ifndef Q_MOC_RUN
#include "board_presenter.hpp"
#include "gui/event_poller.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

// The offline stack is optional (MORPH_BUILD_OFFLINE_SQLITE, needs SQLite3 —
// see CMakeLists.txt's own "SQLite-backed durable offline queue" block) and
// this header must still compile moc-clean and link when that option is OFF:
// every member and method these headers introduce below is itself guarded by
// the same macro, so an OFF configure simply gets a BoardBridge with no
// offline awareness at all -- the pre-Task-5 shape -- rather than a hard
// dependency on SQLite3.
#ifdef MORPH_BUILD_OFFLINE_SQLITE
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#endif
#endif

namespace kanban::gui {

/// @brief QML-facing face of `kanban::gui::BoardPresenter`, plus the one
///        `morph::ladder::gui::EventPoller<BoardEvent, BoardEventId>` a board
///        view owns while attached to a board.
///
/// Turns the presenter's DTO-carrying signals into `QVariantMap`/
/// `QVariantList` property bags and its typed calls into `Q_INVOKABLE`s —
/// same shape as `kanban::gui::ProjectAdminBridge`/
/// `bookmarks::gui::BookmarkBridge`: no decisions, only translation
/// (`examples/IMPLEMENTATION.md` rule 2).
///
/// This is the one class in this rung that mints `MoveTaskPosition`'s
/// `opId` — `QUuid::createUuid().toString()`, per the GUI design spec §6.2
/// step 4 — precisely so QML (and `BoardPresenter`, which only forwards
/// whatever `opId` it is given) never has to see or manage idempotency keys
/// at all. Each `moveTask()` call mints a fresh id; two calls, even for the
/// same task, never share one.
///
/// @par Member declaration order is load-bearing
/// `_presenter` must be declared **before** `_poller`, and `_liveness` must
/// stay the **last** declared member — same requirement, same reasoning, as
/// `polls::gui::PollBridge` (`examples/polls/gui_lib/poll_qml_bridges.hpp`,
/// see its own doc comment's identical section). `EventPoller`'s own
/// `_liveness` token protects the `EventPoller` object itself from a
/// completion callback arriving after it is destroyed, but `startPolling()`'s
/// `Dispatch` closure below also calls back into `_presenter`
/// (`BoardPresenter::getEventsSince`), so `_presenter` must still be alive
/// for as long as `_poller` might still be mid-teardown — which reverse
/// destruction order guarantees only if `_presenter` is declared first.
class BoardBridge : public QObject {
    Q_OBJECT

    /// @brief The most recent `openBoard`/`getBoardState`/`createColumn`/
    ///        `createSwimlane`/`createTask`/`moveTask`/`addComment` result:
    ///        `{projectId, name, columns, swimlanes, tasks, comments}` —
    ///        design spec §4.3's JSON-shaped board property. `columns` is a
    ///        list of `{id, name, wipLimit, taskCount}`; `swimlanes` a list
    ///        of `{id, name}`; `tasks` a list of `{id, columnId, swimlaneId,
    ///        title, position}`; `comments` a list of `{principal, body}`.
    Q_PROPERTY(QVariantMap board READ board NOTIFY boardChanged)
    /// @brief The most recent `getActivity` result: every journal-derived
    ///        activity entry, each a `{actionType, principal, timestampMs,
    ///        summary}` map, oldest first.
    Q_PROPERTY(QVariantList activity READ activity NOTIFY activityChanged)
    /// @brief The caller's own role on the open board, as reported by
    ///        `GetMyProjects` (`ProjectAdminBridge`'s own surface) — kept
    ///        here, not derived from any `BoardModel` result, since no
    ///        action in this rung's board DTOs returns the caller's role
    ///        (design spec §4.3 lists `myRole` alongside `board`/`activity`/
    ///        `principal` as one of this bridge's own state properties; a
    ///        QML shell sets it via `setMyRole()` once
    ///        `ProjectAdminBridge::projectsListed`/`projects` reports the
    ///        logged-in principal's own role for this project). Empty until
    ///        set.
    Q_PROPERTY(QString myRole READ myRole NOTIFY myRoleChanged)
    /// @brief The most recent `getRules` result: every automation rule on
    ///        the attached board, each a `{id, triggerColumnId, mutationType,
    ///        mutationValue}` map. `mutationType` is `"AddTag"`/`"RemoveTag"`.
    Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesListed)

#ifdef MORPH_BUILD_OFFLINE_SQLITE
    /// @brief Current pending-item count in the offline queue — the same
    ///        value `syncStatusChanged`'s own `queueDepth` parameter last
    ///        reported. `0` before `enableOfflineQueue()` is ever called.
    ///        Absent entirely from a `MORPH_BUILD_OFFLINE_SQLITE=OFF` build,
    ///        matching every other offline member's gating.
    Q_PROPERTY(int queueDepth READ queueDepth NOTIFY syncStatusChanged)
    /// @brief Cumulative count of moves dropped after exhausting
    ///        `SyncWorker`'s retry budget — the same running total
    ///        `syncStatusChanged`'s own `deadLettered` parameter last
    ///        reported. `0` before `enableOfflineQueue()` is ever called.
    ///        Absent entirely from a `MORPH_BUILD_OFFLINE_SQLITE=OFF` build,
    ///        matching every other offline member's gating.
    Q_PROPERTY(int deadLetterCount READ deadLetterCount NOTIFY syncStatusChanged)
#endif

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    BoardBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief The current board (see `board` property).
    /// @return The most recent result, as a property bag.
    [[nodiscard]] QVariantMap board() const { return _board; }
    /// @brief The current activity feed (see `activity` property).
    /// @return The most recent listing's rows.
    [[nodiscard]] QVariantList activity() const { return _activity; }
    /// @brief The caller's role on the open board (see `myRole` property).
    /// @return `"Viewer"`/`"Member"`/`"Manager"`, or empty before it is known.
    [[nodiscard]] QString myRole() const { return _myRole; }
    /// @brief The current rule list (see `rules` property).
    /// @return The most recent `getRules` result's rows.
    [[nodiscard]] QVariantList rules() const { return _rules; }

#ifdef MORPH_BUILD_OFFLINE_SQLITE
    /// @brief The offline queue's current depth (see `queueDepth` property).
    /// @return The most recent `syncStatusChanged` queue-depth value.
    [[nodiscard]] int queueDepth() const { return _queueDepth; }
    /// @brief The cumulative dead-letter count (see `deadLetterCount`
    ///        property).
    /// @return The most recent `syncStatusChanged` dead-lettered value.
    [[nodiscard]] int deadLetterCount() const { return _deadLetteredCount; }
#endif

    /// @brief Attaches to `projectId`'s board. Emits `boardChanged`, or
    ///        `failed`.
    /// @param projectId The project's id, as its plain number.
    Q_INVOKABLE void openBoard(const QString& projectId);

    /// @brief Re-reads the attached board's current state. Emits
    ///        `boardChanged`, or `failed`.
    Q_INVOKABLE void refresh();

    /// @brief Creates a new column. Emits `boardChanged`, or `failed`.
    /// @param name     The column's name.
    /// @param wipLimit The column's WIP limit (`0` = unlimited).
    Q_INVOKABLE void createColumn(const QString& name, int wipLimit);

    /// @brief Creates a new swimlane. Emits `boardChanged`, or `failed`.
    /// @param name The swimlane's name.
    Q_INVOKABLE void createSwimlane(const QString& name);

    /// @brief Creates a new task. Emits `boardChanged`, or `failed`.
    /// @param columnId   The task's target column, as its plain number.
    /// @param swimlaneId The task's target swimlane, as its plain number.
    /// @param title      The task's title.
    Q_INVOKABLE void createTask(const QString& columnId, const QString& swimlaneId, const QString& title);

    /// @brief Moves a task to a new column/swimlane/position. Mints a fresh
    ///        `opId` (`QUuid::createUuid().toString()`) internally for
    ///        every call — QML never sees or passes one, per design spec
    ///        §6.2 step 4. Emits `taskMoved`, or `failed`.
    ///
    ///        When `enableOfflineQueue()` has been called and
    ///        `_networkMonitor->isOnline()` is currently `false`, this call
    ///        does not reach `_presenter` at all: it serialises the move
    ///        (opId included) into the `SqliteOfflineQueue` instead and emits
    ///        `syncStatusChanged` with the new queue depth — no `taskMoved`,
    ///        no `failed`, until a later reconnect replays it. Every other
    ///        case (offline queue not enabled, or currently online) behaves
    ///        exactly as before this task.
    /// @param taskId     The task to move, as its plain number.
    /// @param columnId   The destination column, as its plain number.
    /// @param swimlaneId The destination swimlane, as its plain number.
    /// @param position   The destination position within `(columnId, swimlaneId)`.
    Q_INVOKABLE void moveTask(const QString& taskId, const QString& columnId, const QString& swimlaneId,
                               int position);

    /// @brief Appends a comment to a task. Emits `commentAdded`, or `failed`.
    /// @param taskId The task to comment on, as its plain number.
    /// @param body   The comment's body.
    Q_INVOKABLE void addComment(const QString& taskId, const QString& body);

    /// @brief Sets `myRole` (see that property's own doc comment). Pure
    ///        state — dispatches nothing.
    /// @param role The caller's role on the open board.
    Q_INVOKABLE void setMyRole(const QString& role);

    /// @brief Creates a new automation rule on the attached board: "when a
    ///        task moves into `triggerColumnId`, apply `mutationType`/
    ///        `mutationValue`." Manager-only. Emits `ruleCreated`, or `failed`.
    /// @param triggerColumnId The triggering column, as its plain number.
    /// @param mutationType    `"AddTag"` or `"RemoveTag"`.
    /// @param mutationValue   The tag name the mutation adds or removes.
    Q_INVOKABLE void createRule(const QString& triggerColumnId, const QString& mutationType,
                                const QString& mutationValue);

    /// @brief Lists every automation rule on the attached board. Emits
    ///        `rulesListed` (and updates the `rules` property), or `failed`.
    Q_INVOKABLE void getRules();

    /// @brief Deletes one automation rule. Manager-only. Emits `ruleDeleted`,
    ///        or `failed`.
    /// @param ruleId The rule to delete, as its plain number.
    Q_INVOKABLE void deleteRule(const QString& ruleId);

    /// @brief Stops the `EventPoller`'s timer without treating it as a fatal
    ///        error — a board view calls this when it is hidden/closed. A
    ///        no-op if no board is currently open.
    Q_INVOKABLE void stopPolling();

    /// @brief Test-only accessor: the `opId` the most recent `moveTask()`
    ///        call minted. Empty before the first call. Exists solely so
    ///        `test_board_qml_bridge.cpp` can assert that two calls never
    ///        reuse the same id (exactly-once semantics rely on a fresh id
    ///        per user gesture, not per session) — no production code reads
    ///        this.
    /// @return The most recent `moveTask()` call's minted `opId`.
    [[nodiscard]] QString lastOpIdForTest() const { return _lastOpIdForTest; }

#ifndef Q_MOC_RUN
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    /// @brief Turns on the offline queue/replay stack for this bridge: a
    ///        `SqliteOfflineQueue` at @p queuePath, a `SyncWorker` that
    ///        replays each queued item through `_presenter.moveTask()`, a
    ///        `NetworkMonitor` driving `_networkMonitor->isOnline()` (the
    ///        gate `moveTask()` checks below), and a `ReconnectCoordinator`
    ///        that sequences reconnect -> replay per `docs/spec/offline/
    ///        offline.md`'s "End-to-end integration".
    ///
    /// Not folded into the constructor: the queue needs a caller-chosen file
    /// path (`main.cpp`'s real deployment and a test's own temp file differ),
    /// and this whole method compiles away entirely when
    /// `MORPH_BUILD_OFFLINE_SQLITE` is off, so a constructor parameter would
    /// have to be conditionally compiled too -- an optional, idempotent
    /// setup call is the smaller surface. Calling this more than once is not
    /// supported (it replaces every member below without tearing down the
    /// previous `NetworkMonitor`'s probe thread first).
    ///
    /// @par Executor caveat: `onOnline()`/`onOffline()` run on the Qt GUI
    /// thread, not a background worker
    /// `docs/spec/offline/offline.md`'s "NetworkMonitor callback constraint"
    /// requires `ReconnectCoordinator::onOnline()`/`onOffline()` to be
    /// posted onto a worker executor, precisely because `onOnline()`'s retry
    /// loop runs synchronously on whatever thread calls it and can block for
    /// up to `maxAttempts * retryDelay` (~20s at `ReconnectCoordinatorConfig`'s
    /// defaults), plus `SyncWorker::run()`'s own unbounded replay work on
    /// top. This method instead posts both callbacks onto `_executor`, which
    /// (as `main.cpp` wires it) is a `QtExecutor` delivering onto the Qt GUI
    /// thread via `Qt::QueuedConnection` -- not a background thread. This is
    /// harmless *today* only because the default @p probe's paired
    /// `tryReconnect` (`enableOfflineQueue()`'s own default probe is
    /// always-online, and `main.cpp` wires no other) always succeeds
    /// immediately, so `onOnline()`'s retry loop never actually iterates or
    /// sleeps. If a future caller ever wires a real, retry-capable
    /// `tryReconnect` (an actual network probe / reconnect attempt) through
    /// this same `_executor`, it MUST first move `onOnline()`/`onOffline()`
    /// onto a genuine background worker executor -- otherwise a slow
    /// reconnect freezes the Qt GUI thread for the entire retry window. Do
    /// not assume this wiring is safe for a real `tryReconnect` without
    /// making that change.
    ///
    /// @param queuePath Where the durable `SqliteOfflineQueue` persists
    ///        pending moves (created if absent).
    /// @param probe     Connectivity probe `NetworkMonitor` polls on its own
    ///        background thread; defaults to always-online (no real
    ///        connectivity check), since this rung has no dedicated "ping"
    ///        action yet. A test supplies its own atomic-backed probe to
    ///        force a deterministic offline/online transition without a
    ///        real network dependency.
    /// @param monitorConfig Tuning passed straight to `NetworkMonitor` --
    ///        a test shortens `probeInterval`/`failureThreshold`/
    ///        `onlineThreshold` for fast, deterministic convergence.
    void enableOfflineQueue(const QString& queuePath,
                            ::morph::offline::NetworkMonitor::ProbeFunction probe = [] { return true; },
                            ::morph::offline::NetworkMonitor::Config monitorConfig = {});

    /// @brief Test-only accessor: whether `_networkMonitor` currently
    ///        reports the network online. Exists solely so
    ///        `test_board_offline_bridge.cpp` can wait for a real,
    ///        background-probe-driven online/offline transition to land
    ///        before driving the next `moveTask()` call, instead of racing a
    ///        stale assumption about when the transition happened — no
    ///        production code reads this (production code reads
    ///        `isOnline()` only from inside `moveTask()` itself).
    /// @return `true` if the offline stack isn't enabled yet, or if it is
    ///         and `_networkMonitor` reports online; `false` otherwise.
    [[nodiscard]] bool isNetworkOnlineForTest() const { return !_networkMonitor || _networkMonitor->isOnline(); }
#endif
#endif

  signals:
    /// @brief Emitted once the wrapped presenter's registration round trip
    ///        settles — see `ProjectAdminBridge::bound`'s identical doc
    ///        comment.
    void bound();
    /// @brief `board` changed — any of `openBoard`/`refresh`/`createColumn`/
    ///        `createSwimlane`/`createTask`/`moveTask`/`addComment`
    ///        succeeded.
    void boardChanged();
    /// @brief `activity` changed — a `getActivity` call succeeded.
    void activityChanged();
    /// @brief `myRole` changed — `setPrincipal`/`setMyRole` was called.
    void myRoleChanged();
    /// @brief A `moveTask` succeeded.
    /// @param taskId The moved task's id, as its plain number.
    void taskMoved(const QString& taskId);
    /// @brief An `addComment` succeeded.
    /// @param taskId The commented-on task's id, as its plain number.
    void commentAdded(const QString& taskId);
    /// @brief A `getRules` succeeded — see `rules` property.
    /// @param rules The listing's rows.
    void rulesListed(const QVariantList& rules);
    /// @brief A `createRule` succeeded.
    void ruleCreated();
    /// @brief A `deleteRule` succeeded.
    void ruleDeleted();
    /// @brief The `EventPoller` stopped for good (a non-timeout failure).
    ///        Polling does not resume on its own; the view should show this
    ///        and let the user re-open the board.
    /// @param message What `EventPoller::OnFatalError` reported.
    void pollingStopped(const QString& message);
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);
    /// @brief The offline queue's depth or dead-letter count changed —
    ///        emitted after every `moveTask()` that queues instead of
    ///        sending (depth), and after every `SyncWorker` replay pass
    ///        (depth, and dead-lettered if any item exhausted its retry
    ///        budget). A no-op signal (never emitted) when
    ///        `enableOfflineQueue()` was never called, e.g. a
    ///        `MORPH_BUILD_OFFLINE_SQLITE=OFF` build.
    /// @param queueDepth   Current pending-item count in the offline queue.
    /// @param deadLettered Cumulative items dropped after exhausting
    ///        `SyncWorker`'s retry budget, this bridge's lifetime.
    void syncStatusChanged(int queueDepth, int deadLettered);

  private:
    /// @brief Installs a `board` value and emits `boardChanged`. If this
    ///        result came from `openBoard()` (tracked via `_openPending`),
    ///        also (re)starts `_poller` — the equivalent trigger point to
    ///        `PollBridge::openPoll`'s own `startPolling()` call, adapted to
    ///        `BoardPresenter`'s single shared `boardOpened` signal (unlike
    ///        `PollFormsController::openPoll`, `BoardPresenter::openBoard`
    ///        has no dedicated per-call `Completion` to hook `startPolling()`
    ///        off of directly).
    /// @param result The board's full current state.
    void applyBoard(const GetBoardResult& result);

#ifndef Q_MOC_RUN
    using Poller = ::morph::ladder::gui::EventPoller<BoardEvent, BoardEventId>;

    /// @brief Builds and starts `_poller` against the just-opened board. Its
    ///        `Dispatch` closure reuses `_presenter`'s already-attached
    ///        handler via `BoardPresenter::getEventsSinceForPolling` — see
    ///        that method's own doc comment for why a *second*,
    ///        independently-attached handler (or the shared-signal
    ///        `getEventsSince`) is deliberately not used here.
    ///
    /// Constructs `Poller` with no interval/deadline override, so the real
    /// unscaled `Poller::kDefaultExecuteDeadline` is always armed — see that
    /// constant's own doc comment (`event_poller.hpp`) for the CI-flakiness
    /// risk this carries under a scaled `MORPH_LADDER_DEADLINE_MS` run, and
    /// why it is not "fixed" here by exposing an override on this adapter
    /// (mirrors `PollBridge::startPolling`'s identical note).
    ///
    /// `openBoard()`'s own `GetBoardResult` carries no cursor (unlike
    /// `polls::GetPollStateResult::lastEventId`), so every call starts from
    /// `BoardEventId{}` — `GetEventsSince`'s own documented "from the
    /// beginning" default (`kanban/dto/event_dto.hpp`), correct for this
    /// rung since a freshly attached board view has not yet seen any event.
    void startPolling();

    /// @brief `_poller`'s `ApplyEvent`: relays @p event as a refreshed
    ///        `activity`/`board` — see `.cpp`'s `onEventApplied`.
    /// @param event One event `_poller` just applied.
    void onEventApplied(const BoardEvent& event);
#endif

#ifndef Q_MOC_RUN
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    /// @brief `_syncWorker`'s `ReplayFunction`: deserialises @p payload and
    ///        replays it via `BoardPresenter::moveTaskForReplay`, blocking
    ///        (via a nested `QEventLoop`, the same idiom
    ///        `QtWebSocketBackend::sendSync` uses for its own synchronous
    ///        contract — `qt_websocket_backend.cpp`) until that call's own
    ///        `Completion` settles, since `SyncWorker::run()` calls this
    ///        function synchronously and needs an immediate `bool` back
    ///        (`sync_worker.hpp`'s documented `ReplayFunction` contract).
    /// @param payload One queued `QueueItem::payload` — a
    ///        `serializeMoveTaskPosition()`-encoded `MoveTaskPosition`.
    /// @return `true` (remove from queue) if @p payload decoded and the
    ///         replayed move succeeded; `false` (retry, subject to
    ///         `SyncWorker`'s 5-attempt dead-letter cap) if @p payload could
    ///         not be decoded or the replayed move's `Completion` failed.
    [[nodiscard]] bool replayMoveTaskPosition(const std::string& payload);
#endif
#endif

#ifndef Q_MOC_RUN
    BoardPresenter _presenter;
    std::unique_ptr<Poller> _poller;
    /// @brief The same `Bridge` `_presenter` was constructed with — kept
    ///        here only because `Poller`'s constructor needs a `Bridge&` for
    ///        `setExecuteDeadline()` (see `event_poller.hpp`), and
    ///        `BoardPresenter` does not expose its own reference to it.
    ///        Same reference `PollBridge::_bridge` keeps for the identical
    ///        reason.
    ::morph::bridge::Bridge& _bridge;
    /// @brief The executor `_presenter`'s `Completion` callbacks land on --
    ///        kept here (redundantly with `_presenter`'s own copy, which it
    ///        does not expose) only so `enableOfflineQueue()`'s
    ///        `NetworkMonitor` callbacks, which run on the monitor's own
    ///        probe thread, can `post()` the `ReconnectCoordinator`
    ///        sequencing onto the Qt thread instead of running it inline --
    ///        `docs/spec/offline/offline.md`'s "NetworkMonitor callback
    ///        constraint" (a blocking, seconds-long retry loop must never run
    ///        on the probe thread).
    ::morph::exec::IExecutor* _executor;
#endif
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    /// @brief Set only by `enableOfflineQueue()`; every offline member below
    ///        is `nullptr`/absent until then, and `moveTask()` skips the
    ///        offline branch entirely in that case (dispatches straight to
    ///        `_presenter`, the pre-Task-5 behaviour).
    ///
    /// @par Declaration order is load-bearing here too
    /// `_networkMonitor` must be declared **last** among these four members
    /// (i.e. destroyed **first**, reverse declaration order): its probe
    /// thread calls back into `_reconnectCoordinator` (via a `post()`ed
    /// lambda -- see the constructor's own comment), so that thread must be
    /// fully stopped (`~NetworkMonitor()` blocks until it is) before
    /// `_reconnectCoordinator`/`_syncWorker`/`_offlineQueue` are torn down.
    /// Declaring `_networkMonitor` *before* them (destroyed *after* them)
    /// would let a probe-thread callback still in flight during teardown
    /// reach an already-destroyed `_reconnectCoordinator` through a
    /// `unique_ptr` that had already been reset to null -- `_liveness`'s own
    /// `weak_ptr` guard does not catch this, since `_liveness` itself is
    /// destroyed even later still and would not yet be expired.
    std::unique_ptr<::morph::offline::SqliteOfflineQueue> _offlineQueue;
    std::unique_ptr<::morph::offline::SyncWorker> _syncWorker;
    std::unique_ptr<::morph::offline::ReconnectCoordinator> _reconnectCoordinator;
    std::unique_ptr<::morph::offline::NetworkMonitor> _networkMonitor;
    /// @brief Cumulative dead-lettered count `syncStatusChanged` reports —
    ///        `SyncWorker`'s own `SyncResult`/`DeadLetterSink` report
    ///        per-`run()` counts, not a running total, so this bridge keeps
    ///        the total itself.
    int _deadLetteredCount = 0;
    /// @brief Mirrors the most recent `syncStatusChanged` queue-depth value,
    ///        backing the `queueDepth` `Q_PROPERTY` getter — every
    ///        `syncStatusChanged` emission site already computes this same
    ///        value (`_offlineQueue->size()`) to pass as that signal's own
    ///        argument; this member just keeps the latest one around for a
    ///        plain getter to read without needing a live `_offlineQueue`
    ///        pointer at call time.
    int _queueDepth = 0;
#endif
    QVariantMap _board;
    QVariantList _activity;
    QString _myRole;
    QVariantList _rules;
    QString _lastOpIdForTest;
    /// @brief Set by `openBoard()`, consumed (and cleared) by the next
    ///        `boardOpened` this bridge relays — see `applyBoard()`'s own
    ///        doc comment for why this flag, rather than a dedicated signal,
    ///        is what marks "this particular boardOpened is the one to start
    ///        polling from".
    bool _openPending = false;

    /// @brief Weak-observable proof this object still exists.
    ///
    /// `startPolling()`'s `Dispatch` closure calls
    /// `_presenter.getEventsSinceForPolling()`, whose returned `Completion`'s
    /// `.then()`/`.onError()` continuations are plain `std::function`-based,
    /// not `QObject::connect`-based signal/slot connections — Qt's
    /// auto-disconnect-on-destruction machinery does not apply to them.
    /// Every one of those callbacks captures raw `this`; destroying a
    /// `BoardBridge` while a tick is still in flight (an ordinary GUI case —
    /// a view closing mid-poll) would otherwise write into freed memory. See
    /// `polls::gui::PollBridge::_liveness`'s identical doc comment
    /// (`poll_qml_bridges.hpp`) for the full rationale. Same pattern, same
    /// reasoning, and the same **must remain the last declared member**
    /// requirement as
    /// `morph::ladder::gui::EventPoller::_liveness`
    /// (`examples/common/gui/event_poller.hpp`) and
    /// `morph::bridge::Bridge::_liveness` (`include/morph/core/bridge.hpp`).
    ///
    /// `enableOfflineQueue()`'s three new callback sites guard on this exact
    /// token, the same way `startPolling()`'s three closures above already
    /// do: `NetworkMonitor`'s `onOffline`/`onOnline` (called on the probe
    /// thread, so they capture a `weak_ptr` and check `.expired()` before
    /// `post()`-ing anything that touches `this`) and the posted lambda that
    /// actually runs `_reconnectCoordinator->onOnline()`/`onOffline()` on the
    /// Qt thread (checked again there, since the `post()` can outlive this
    /// object between being queued and actually running). No separate
    /// lifetime mechanism is introduced for the offline stack.
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

}  // namespace kanban::gui
