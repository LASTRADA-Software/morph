// SPDX-License-Identifier: Apache-2.0
#include "board_presenter.hpp"

#include <utility>

namespace kanban::gui {

namespace {

/// @brief A `TaskId` as the plain-number-shaped `QString` `taskMoved`/
///        `commentAdded` carry — matches `kanban::gui::idNumber`-family
///        conventions used at the bridge boundary elsewhere in this rung,
///        rendered as text here since these two signals are
///        presenter-level, not bridge-level (the bridge itself further
///        translates/relays them unchanged).
[[nodiscard]] QString taskIdText(const TaskId& id) {
    return id.hasValue() ? QString::number(*id) : QString{};
}

}  // namespace

BoardPresenter::BoardPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void BoardPresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

void BoardPresenter::openBoard(ProjectId projectId) {
    track<GetBoardResult>(
        _handler.execute(OpenBoard{.projectId = projectId}),
        [this](GetBoardResult result) { emit boardOpened(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::getBoardState() {
    track<GetBoardResult>(
        _handler.execute(GetBoardState{}), [this](GetBoardResult result) { emit boardOpened(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::createColumn(const QString& name, std::int64_t wipLimit) {
    track<GetBoardResult>(
        _handler.execute(CreateColumn{.name = name.toStdString(), .wipLimit = wipLimit}),
        [this](GetBoardResult result) { emit boardOpened(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::createSwimlane(const QString& name) {
    track<GetBoardResult>(
        _handler.execute(CreateSwimlane{.name = name.toStdString()}),
        [this](GetBoardResult result) { emit boardOpened(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::createTask(ColumnId columnId, SwimlaneId swimlaneId, const QString& title) {
    track<GetBoardResult>(
        _handler.execute(
            CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = title.toStdString()}),
        [this](GetBoardResult result) { emit boardOpened(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::moveTask(TaskId taskId, ColumnId columnId, SwimlaneId swimlaneId, std::int64_t position,
                               QString opId) {
    // `taskId` is captured by this call's own lambda, not stashed on any
    // shared member: two overlapping moveTask() calls (this task's own
    // concurrent-drag test drives exactly that) each get their own track()
    // continuation with their own captured `taskId`, so taskMoved()'s
    // payload can never cross between them regardless of completion order —
    // the same lesson Task 2's ProjectAdminBridge::createProject fix round
    // established (see project_admin_presenter.hpp's projectCreated() doc
    // comment for the fuller account of that defect).
    track<GetBoardResult>(
        _handler.execute(MoveTaskPosition{.taskId = taskId,
                                           .columnId = columnId,
                                           .swimlaneId = swimlaneId,
                                           .position = position,
                                           .opId = opId.toStdString()}),
        [this, taskId](GetBoardResult) { emit taskMoved(taskIdText(taskId)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

::morph::async::Completion<GetBoardResult> BoardPresenter::moveTaskForReplay(TaskId taskId, ColumnId columnId,
                                                                             SwimlaneId swimlaneId,
                                                                             std::int64_t position, QString opId) {
    return _handler.execute(MoveTaskPosition{.taskId = taskId,
                                              .columnId = columnId,
                                              .swimlaneId = swimlaneId,
                                              .position = position,
                                              .opId = opId.toStdString()});
}

void BoardPresenter::addComment(TaskId taskId, const QString& body) {
    // Same per-call capture discipline as moveTask() above: `taskId` travels
    // with this call's own continuation, not a shared field.
    track<GetBoardResult>(
        _handler.execute(AddComment{.taskId = taskId, .body = body.toStdString()}),
        [this, taskId](GetBoardResult) { emit commentAdded(taskIdText(taskId)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::getEventsSince(BoardEventId lastEventId) {
    track<GetEventsSinceResult>(
        _handler.execute(kanban::GetEventsSince{.lastEventId = lastEventId}),
        [this](GetEventsSinceResult result) { emit eventsReceived(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::getActivity() {
    track<GetActivityResult>(
        _handler.execute(GetActivity{}), [this](GetActivityResult result) { emit activityUpdated(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

::morph::async::Completion<GetEventsSinceResult> BoardPresenter::getEventsSinceForPolling(BoardEventId lastEventId) {
    return _handler.execute(kanban::GetEventsSince{.lastEventId = lastEventId});
}

}  // namespace kanban::gui
