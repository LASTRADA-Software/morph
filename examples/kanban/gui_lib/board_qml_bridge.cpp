// SPDX-License-Identifier: Apache-2.0
#include "board_qml_bridge.hpp"

#include <QString>
#include <QUuid>
#include <QVariant>

#include <glaze/glaze.hpp>

#include <optional>
#include <utility>

#ifdef MORPH_BUILD_OFFLINE_SQLITE
#include <QEventLoop>

#include <chrono>
#include <thread>
#endif

namespace kanban::gui {

namespace {

/// @brief A strong id type (`ColumnId`/`SwimlaneId`/`TaskId`/`ProjectId`) as
///        the plain number a QML row/invokable carries — `-1` when
///        unengaged, same convention as `kanban::gui::idNumber`
///        (`project_admin_qml_bridge.cpp`).
template <typename IdT>
[[nodiscard]] qlonglong idNumber(const IdT& id) {
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief Parses a QML-supplied id string (a plain integer, as every
///        invokable in this class receives ids) back into a strong id.
///        An unparseable string yields a disengaged id, which the model's
///        own `validate()` then rejects with `ValidationError` — the same
///        fail-safe shape `ProjectAdminBridge`'s `qlonglong`-typed ids get
///        for free from Qt's own integer marshalling; this class takes
///        `QString` ids instead (design spec's own invokable signatures use
///        `QString` throughout for board-scoped ids), so the parse has to
///        happen here explicitly.
/// @tparam IdT One of `ColumnId`/`SwimlaneId`/`TaskId`/`ProjectId`.
/// @param text The id, as QML passed it.
/// @return The parsed id, or a disengaged `IdT{}` if @p text is not a valid
///         non-negative integer.
template <typename IdT>
[[nodiscard]] IdT parseId(const QString& text) {
    bool ok = false;
    const qlonglong value = text.toLongLong(&ok);
    if (!ok || value <= 0) {
        return IdT{};
    }
    return IdT{static_cast<std::int64_t>(value)};
}

[[nodiscard]] QVariantMap toVariantMap(const ColumnView& column) {
    return QVariantMap{
        {"id", idNumber(column.id)},
        {"name", QString::fromStdString(column.name)},
        {"wipLimit", static_cast<qlonglong>(column.wipLimit)},
        {"taskCount", static_cast<qlonglong>(column.taskCount)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const SwimlaneView& swimlane) {
    return QVariantMap{
        {"id", idNumber(swimlane.id)},
        {"name", QString::fromStdString(swimlane.name)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const TaskView& task) {
    return QVariantMap{
        {"id", idNumber(task.id)},
        {"columnId", idNumber(task.columnId)},
        {"swimlaneId", idNumber(task.swimlaneId)},
        {"title", QString::fromStdString(task.title)},
        {"position", static_cast<qlonglong>(task.position)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const CommentView& comment) {
    return QVariantMap{
        {"taskId", idNumber(comment.taskId)},
        {"principal", QString::fromStdString(comment.principal)},
        {"body", QString::fromStdString(comment.body)},
    };
}

/// @brief One `RuleView` row as the property bag the rules view binds
///        against — `mutationType`'s wire string (`"AddTag"`/`"RemoveTag"`,
///        `rule_dto.hpp`'s `ruleMutationTypeToString`) rendered as a
///        `QString`, same convention as `kanban::gui::roleText`
///        (`project_admin_qml_bridge.cpp`).
[[nodiscard]] QVariantMap toVariantMap(const RuleView& rule) {
    const auto mutationType = ruleMutationTypeToString(rule.mutationType);
    return QVariantMap{
        {"id", idNumber(rule.id)},
        {"triggerColumnId", idNumber(rule.triggerColumnId)},
        {"mutationType", QString::fromUtf8(mutationType.data(), static_cast<qsizetype>(mutationType.size()))},
        {"mutationValue", QString::fromStdString(rule.mutationValue)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const ActivityEvent& event) {
    return QVariantMap{
        {"actionType", QString::fromStdString(event.actionType)},
        {"principal", QString::fromStdString(event.principal)},
        {"timestampMs", static_cast<qlonglong>(event.timestampMs)},
        {"summary", QString::fromStdString(event.summary)},
    };
}

template <typename Rows>
[[nodiscard]] QVariantList toVariantList(const Rows& rows) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows.size()));
    for (const auto& row : rows) {
        out.append(toVariantMap(row));
    }
    return out;
}

/// @brief `GetBoardResult` as the JSON-shaped property bag `board` exposes —
///        design spec §4.3.
[[nodiscard]] QVariantMap toVariantMap(const GetBoardResult& state) {
    return QVariantMap{
        {"projectId", idNumber(state.projectId)},
        {"name", QString::fromStdString(state.name)},
        {"columns", toVariantList(state.columns)},
        {"swimlanes", toVariantList(state.swimlanes)},
        {"tasks", toVariantList(state.tasks)},
        {"comments", toVariantList(state.comments)},
    };
}

#ifdef MORPH_BUILD_OFFLINE_SQLITE
/// @brief Renders a `MoveTaskPosition` (including its already-minted `opId`)
///        as the JSON payload `enableOfflineQueue()`'s `SqliteOfflineQueue`
///        stores while offline. Plain `glz::write_json` over the DTO's own
///        aggregate reflection — no explicit `glz::meta<MoveTaskPosition>`
///        exists or is needed (glaze reflects a plain struct's named members
///        automatically; `TaskId`/`ColumnId`/`SwimlaneId` already have their
///        own `glz::meta` specialisations, `kanban/core/types.hpp`), the same
///        pattern `bookmarks::gui::decodeLoginResult`
///        (`bookmark_qml_bridges.cpp`) uses for its own bridge-level JSON.
/// @param action The move to serialise.
/// @return Its JSON encoding, ready for `IOfflineQueue::enqueue()`.
[[nodiscard]] std::string serializeMoveTaskPosition(const MoveTaskPosition& action) {
    return glz::write_json(action).value_or("{}");
}

/// @brief Parses a queued payload `serializeMoveTaskPosition` produced back
///        into a `MoveTaskPosition`.
/// @param payload The `QueueItem::payload` a `SyncWorker` replay is handling.
/// @return The decoded action, or `std::nullopt` if @p payload is not valid
///         JSON for this shape (a corrupt or foreign row — `SyncWorker`'s
///         `ReplayFunction` contract treats that as a replay failure, not a
///         crash).
[[nodiscard]] std::optional<MoveTaskPosition> deserializeMoveTaskPosition(const std::string& payload) {
    MoveTaskPosition action;
    if (glz::read_json(action, payload)) {
        return std::nullopt;
    }
    return action;
}
#endif

}  // namespace

BoardBridge::BoardBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor}, _bridge{bridge}, _executor{executor} {
    // Direct (same-thread) connections throughout — same "no meta-type
    // registration needed" note as ProjectAdminBridge's identical
    // constructor comment.
    connect(&_presenter, &BoardPresenter::bound, this, &BoardBridge::bound);
    connect(&_presenter, &BoardPresenter::boardOpened, this,
            [this](GetBoardResult result) { applyBoard(result); });
    connect(&_presenter, &BoardPresenter::taskMoved, this, &BoardBridge::taskMoved);
    connect(&_presenter, &BoardPresenter::commentAdded, this, &BoardBridge::commentAdded);
    connect(&_presenter, &BoardPresenter::activityUpdated, this, [this](GetActivityResult result) {
        _activity = toVariantList(result.events);
        emit activityChanged();
    });
    connect(&_presenter, &BoardPresenter::rulesListed, this, [this](GetRulesResult result) {
        _rules = toVariantList(result.rules);
        emit rulesListed(_rules);
    });
    connect(&_presenter, &BoardPresenter::ruleCreated, this, &BoardBridge::ruleCreated);
    connect(&_presenter, &BoardPresenter::ruleDeleted, this, &BoardBridge::ruleDeleted);
    connect(&_presenter, &BoardPresenter::failed, this, &BoardBridge::failed);
}

void BoardBridge::applyBoard(const GetBoardResult& result) {
    _board = toVariantMap(result);
    emit boardChanged();
    if (_openPending) {
        _openPending = false;
        startPolling();
    }
}

void BoardBridge::openBoard(const QString& projectId) {
    _openPending = true;
    _presenter.openBoard(parseId<ProjectId>(projectId));
}

void BoardBridge::refresh() {
    _presenter.getBoardState();
}

void BoardBridge::createColumn(const QString& name, int wipLimit) {
    _presenter.createColumn(name, static_cast<std::int64_t>(wipLimit));
}

void BoardBridge::createSwimlane(const QString& name) {
    _presenter.createSwimlane(name);
}

void BoardBridge::createTask(const QString& columnId, const QString& swimlaneId, const QString& title) {
    _presenter.createTask(parseId<ColumnId>(columnId), parseId<SwimlaneId>(swimlaneId), title);
}

void BoardBridge::moveTask(const QString& taskId, const QString& columnId, const QString& swimlaneId, int position) {
    // Minted fresh on every call, never read back from (or stashed on) any
    // shared field beyond `_lastOpIdForTest` (test-only, written here purely
    // for that accessor) -- the exactly-once contract MoveTaskPosition
    // relies on needs a distinct opId per user gesture, not per session; see
    // this class's own doc comment and design spec §6.2 step 4.
    const QString opId = QUuid::createUuid().toString();
    _lastOpIdForTest = opId;

#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_offlineQueue && _networkMonitor && !_networkMonitor->isOnline()) {
        // Offline: queue instead of dispatching. The action (opId included)
        // travels entirely inside this queued payload -- nothing about this
        // specific move is stashed on any shared bridge-level field, so many
        // distinct queued moves over time never cross-contaminate each
        // other's data (the Task 2 lesson this task's brief calls out
        // explicitly). idempotencyKey == opId: the same key a later replay's
        // MoveTaskPosition::opId carries, ready for a host that also dedups
        // against the journal (docs/spec/offline/offline.md's "idempotency
        // key" section) -- this bridge's own replay path (enableOfflineQueue
        // below) doesn't need it for correctness (BoardModel::execute()
        // already dedups on opId via its own ledger), but stamping it here
        // costs nothing and keeps the contract available to a future replay
        // consumer that isn't this bridge.
        const MoveTaskPosition action{.taskId = parseId<TaskId>(taskId),
                                       .columnId = parseId<ColumnId>(columnId),
                                       .swimlaneId = parseId<SwimlaneId>(swimlaneId),
                                       .position = static_cast<std::int64_t>(position),
                                       .opId = opId.toStdString()};
        _offlineQueue->enqueue(serializeMoveTaskPosition(action), action.opId);
        _queueDepth = static_cast<int>(_offlineQueue->size());
        emit syncStatusChanged(_queueDepth, _deadLetteredCount);
        return;
    }
#endif

    _presenter.moveTask(parseId<TaskId>(taskId), parseId<ColumnId>(columnId), parseId<SwimlaneId>(swimlaneId),
                        static_cast<std::int64_t>(position), opId);
}

void BoardBridge::addComment(const QString& taskId, const QString& body) {
    _presenter.addComment(parseId<TaskId>(taskId), body);
}

void BoardBridge::setMyRole(const QString& role) {
    _myRole = role;
    emit myRoleChanged();
}

void BoardBridge::createRule(const QString& triggerColumnId, const QString& mutationType,
                              const QString& mutationValue) {
    _presenter.createRule(parseId<ColumnId>(triggerColumnId), mutationType, mutationValue);
}

void BoardBridge::getRules() {
    _presenter.getRules();
}

void BoardBridge::deleteRule(const QString& ruleId) {
    _presenter.deleteRule(parseId<RuleId>(ruleId));
}

void BoardBridge::stopPolling() {
    if (_poller) {
        _poller->stop();
    }
}

void BoardBridge::startPolling() {
    // Declaration-order note in board_qml_bridge.hpp explains why `_poller`
    // may safely outlive individual ticks of `_presenter`'s handler but must
    // itself be torn down before `_presenter` is.
    //
    // openBoard()'s own GetBoardResult carries no cursor, so every
    // (re)start dispatches from BoardEventId{} — GetEventsSince's own
    // "from the beginning" default (kanban/dto/event_dto.hpp) — which is
    // correct here since a freshly (re)attached board view has not yet seen
    // any event.
    _poller = std::make_unique<Poller>(
        _bridge, BoardEventId{},
        [this, alive = std::weak_ptr<const void>{_liveness}](BoardEventId lastEventId, Poller::OnSuccess onSuccess,
                                                              Poller::OnError onError) {
            if (alive.expired()) {
                return;
            }
            // The production-safe Dispatch shape event_poller.hpp's own doc
            // comment asks for: built directly over one call's own
            // Completion, never over a Presenter's shared failed(QString)
            // signal. BoardPresenter::getEventsSinceForPolling returns a
            // fresh, independent Completion<GetEventsSinceResult> per call —
            // see that method's own doc comment. onSuccess/onError are
            // EventPoller's own callbacks, already guarded on its own
            // _liveness token (see event_poller.hpp) — nothing further to
            // add here beyond not touching `_presenter` past this object's
            // own lifetime, which the `alive` check above already covers.
            _presenter.getEventsSinceForPolling(lastEventId)
                .then([lastEventId, onSuccess](GetEventsSinceResult result) {
                    const BoardEventId newLastEventId =
                        result.events.empty() ? lastEventId : result.events.back().id;
                    onSuccess(std::move(result.events), newLastEventId);
                })
                .onError([onError](const std::exception_ptr& err) { onError(err); });
        },
        [this, alive = std::weak_ptr<const void>{_liveness}](const BoardEvent& event) {
            if (alive.expired()) {
                return;
            }
            onEventApplied(event);
        },
        [this, alive = std::weak_ptr<const void>{_liveness}](const QString& message) {
            if (alive.expired()) {
                return;
            }
            emit pollingStopped(message);
        });
}

void BoardBridge::onEventApplied(const BoardEvent&) {
    // A board event's own shape (kind + summary, kanban/dto/event_dto.hpp)
    // carries nothing the board/activity property bags expose beyond what a
    // full resync already reports, so every applied event simply triggers a
    // refresh of both `board` and `activity` — both actions this rung's
    // design spec §7 already documents as staying in sync with each other.
    refresh();
    _presenter.getActivity();
}

#ifdef MORPH_BUILD_OFFLINE_SQLITE

bool BoardBridge::replayMoveTaskPosition(const std::string& payload) {
    const auto decoded = deserializeMoveTaskPosition(payload);
    if (!decoded) {
        // Not this bridge's own shape (corrupt row, or a foreign payload a
        // future action type also queued into the same file) -- a replay
        // failure per SyncWorker's contract (false, not a throw: the payload
        // itself isn't going to reparse differently next attempt, but
        // treating it as a hard error here would still let SyncWorker's own
        // 5-attempt dead-letter cap eventually drop it, exactly as intended
        // for an unreplayable item).
        return false;
    }
    const MoveTaskPosition& action = *decoded;

    // Nested QEventLoop, parked until moveTaskForReplay()'s own Completion
    // settles -- the same idiom QtWebSocketBackend::sendSync uses for its
    // own synchronous contract (qt_websocket_backend.cpp), needed here
    // because SyncWorker::run() calls this function synchronously and wants
    // an immediate bool back (sync_worker.hpp's documented ReplayFunction
    // contract), while BoardPresenter's own Completion-based API is
    // fundamentally asynchronous. SyncWorker::run() drains and replays one
    // item at a time on whichever thread called run() (here, the Qt thread,
    // via enableOfflineQueue()'s posted onOnline() below) -- never two
    // replays in flight together -- so there is no reentrant-parking hazard
    // the way a second concurrent sendSync() would have.
    QEventLoop loop;
    bool succeeded = false;
    // No `alive`/weak_ptr guard here, unlike every async callback elsewhere
    // in this file -- none is needed. `.then()`/`.onError()` below capture
    // only `succeeded`/`loop` (plain stack locals), never `this`, so there is
    // nothing in this pair of lambdas for a dangling `this` to corrupt even
    // in principle. More fundamentally, this whole call is synchronous, not
    // posted: replayMoveTaskPosition() is called directly, on the calling
    // thread, from SyncWorker::run() (sync_worker.hpp's ReplayFunction
    // contract), which is itself called directly from
    // ReconnectCoordinator::onOnline()'s `replay` dep (reconnect_coordinator.hpp),
    // which enableOfflineQueue() below wires to run only from inside the
    // already-`alive`-checked lambda `_networkMonitor` posts onto `_executor`.
    // That whole chain is one uninterrupted call stack with no re-entrant
    // return to the executor's event loop in between, so the frame that
    // verified `this` was alive is still on the stack, still holding `this`
    // alive, for every nanosecond this function runs -- there is no window in
    // which `this` could be destroyed out from under it.
    _presenter.moveTaskForReplay(action.taskId, action.columnId, action.swimlaneId, action.position,
                                  QString::fromStdString(action.opId))
        .then([&succeeded, &loop](GetBoardResult) {
            succeeded = true;
            loop.quit();
        })
        .onError([&loop](const std::exception_ptr&) { loop.quit(); });
    loop.exec();
    return succeeded;
}

void BoardBridge::enableOfflineQueue(const QString& queuePath, ::morph::offline::NetworkMonitor::ProbeFunction probe,
                                      ::morph::offline::NetworkMonitor::Config monitorConfig) {
    _offlineQueue = std::make_unique<::morph::offline::SqliteOfflineQueue>(queuePath.toStdString());

    _syncWorker = std::make_unique<::morph::offline::SyncWorker>(
        *_offlineQueue, [this](const std::string& payload) { return replayMoveTaskPosition(payload); },
        [this](const ::morph::offline::QueueItem&) {
            // DeadLetterSink: one more item exhausted SyncWorker's 5-attempt
            // cap and was just dropped from the queue. The running total
            // (not SyncWorker's own per-run count, which resets every
            // run()) is what syncStatusChanged reports, so a GUI's "N
            // dropped" indicator (Task 6) never regresses between polls.
            ++_deadLetteredCount;
            _queueDepth = static_cast<int>(_offlineQueue->size());
            emit syncStatusChanged(_queueDepth, _deadLetteredCount);
        });

    // ReconnectCoordinator::Deps: this bridge has no separate "primary vs.
    // local backend" to switch between (unlike docs/spec/offline/offline.md's
    // End-to-end integration example, which assumes a Bridge that owns both)
    // -- moveTask()'s own _networkMonitor->isOnline() check is this bridge's
    // entire backend-selection mechanism, so activatePrimary/activateLocal/
    // bindContext are no-ops here: there is nothing to activate or rebind,
    // only the queue to replay. shouldContinue reads the monitor's own
    // current state (not a captured snapshot), matching the "went offline
    // again mid-retry" abort case the coordinator's doc comment describes.
    _reconnectCoordinator = std::make_unique<::morph::offline::ReconnectCoordinator>(
        ::morph::offline::ReconnectCoordinator::Deps{
            .tryReconnect = [] { return true; },
            .activatePrimary = [] {},
            .activateLocal = [] {},
            .bindContext = [] {},
            .replay =
                [this] {
                    // `_deadLetteredCount` itself is updated by the
                    // DeadLetterSink below (once per exhausted item, as it
                    // happens) -- this handler only reports the queue's
                    // post-run depth, since a successful or merely-retried
                    // (still-queued) item never touches that counter.
                    const ::morph::offline::SyncResult result = _syncWorker->run();
                    _queueDepth = static_cast<int>(_offlineQueue->size());
                    emit syncStatusChanged(_queueDepth, _deadLetteredCount);
                    // A successful replay applied a move server-side that
                    // this bridge's cached `board`/`activity` do not yet
                    // reflect (moveTaskForReplay() deliberately never
                    // touches `_board` itself -- see that method's own doc
                    // comment on why it bypasses every shared signal). Only
                    // refresh if something actually landed: an all-offline
                    // run (every item re-queued, nothing succeeded) has
                    // nothing new to fetch.
                    if (result.successful > 0) {
                        refresh();
                    }
                },
            .shouldContinue = [this] { return _networkMonitor && _networkMonitor->isOnline(); },
            .sleep = [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); },
        });

    // NetworkMonitor's own callbacks run on its dedicated probe thread
    // (network_monitor.hpp's documented callback constraint) and must do
    // O(1) work only -- they post the coordinator's sequencing onto
    // `_executor` (the Qt thread) rather than running it inline, exactly
    // docs/spec/offline/offline.md's "End-to-end integration" pattern. Both
    // posted lambdas re-check `alive` after landing on the Qt thread, since
    // the post() can outlive this object between being queued and actually
    // running (same two-layer guard startPolling()'s closures use above:
    // once before capturing anything, implicitly by capturing only `alive`
    // and `this`, and once again on arrival).
    _networkMonitor = std::make_unique<::morph::offline::NetworkMonitor>(
        std::move(probe),
        [this, alive = std::weak_ptr<const void>{_liveness}] {
            if (alive.expired()) {
                return;
            }
            _executor->post([this, alive] {
                if (alive.expired()) {
                    return;
                }
                _reconnectCoordinator->onOffline();
            });
        },
        [this, alive = std::weak_ptr<const void>{_liveness}] {
            if (alive.expired()) {
                return;
            }
            _executor->post([this, alive] {
                if (alive.expired()) {
                    return;
                }
                _reconnectCoordinator->onOnline();
            });
        },
        monitorConfig);
}

#endif

}  // namespace kanban::gui
