// SPDX-License-Identifier: Apache-2.0
#include "board_qml_bridge.hpp"

#include <QString>
#include <QUuid>
#include <QVariant>

#include <utility>

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
        {"principal", QString::fromStdString(comment.principal)},
        {"body", QString::fromStdString(comment.body)},
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

}  // namespace

BoardBridge::BoardBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor}, _bridge{bridge} {
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

}  // namespace kanban::gui
