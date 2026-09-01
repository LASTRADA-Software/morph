// SPDX-License-Identifier: Apache-2.0
#include "board_presenter.hpp"

#include <glaze/glaze.hpp>
#include <string>
#include <utility>

#include "gui/error_text.hpp"
#include "gui/id_qml.hpp"

namespace kanban::gui {

namespace {

// A `TaskId` as the plain-number-shaped `QString` `taskMoved`/`commentAdded`
// carry — text rather than a number here because these two signals are
// presenter-level, not bridge-level (the bridge translates/relays them
// unchanged), and the rung's board invokables are `QString`-shaped throughout.
using ::morph::ladder::gui::idText;

}  // namespace

BoardPresenter::BoardPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void BoardPresenter::reportError(const std::exception_ptr& err) { emit failed(::morph::ladder::gui::errorText(err)); }

void BoardPresenter::openBoard(ProjectId projectId) {
    // Stashed for createRule()/getRules() below -- see `_projectId`'s own doc
    // comment (board_presenter.hpp) for why this is the one piece of
    // attach-adjacent state this otherwise-transport-only presenter keeps.
    _projectId = projectId;
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

void BoardPresenter::submitForm(const QString& actionType, const QString& bodyJson) {
    // The four board forms `kanban_schemas.hpp` publishes, and nothing else.
    // Checked against a literal list rather than handed straight to
    // `executeJson`: `BoardModel` also registers actions no form renders
    // (`MoveTaskPosition`, `DeleteRule`, `ApplyTagMutation`, ...), and a
    // renderer -- or a typo in a QML `actionType:` string -- must not be able
    // to reach one of those through this seam just because the model happens
    // to serve it. QML names types as strings; an unroutable one has to arrive
    // somewhere a human reads it.
    const std::string type = actionType.toStdString();
    if (type != "CreateColumn" && type != "CreateSwimlane" && type != "CreateTask" && type != "AddComment") {
        emit formReplyReceived(actionType, false,
                               QStringLiteral("no board form in this client serves action '") + actionType + u'\'');
        return;
    }
    // `AddComment`'s taskId, echoed back on `commentAdded` below. Decoded from
    // the submitted body, not from the reply (which is the whole board), and
    // captured in this call's own continuation rather than stashed on a shared
    // member -- the same discipline `addComment()` below already documents.
    AddComment comment;
    const bool decodedComment = type == "AddComment" && !glz::read_json(comment, bodyJson.toStdString());
    track<std::string>(
        _handler.executeJson(type, bodyJson.toStdString()),
        [this, actionType, decodedComment, commentTaskId = comment.taskId](std::string resultJson) {
            // Every action above returns the full rebuilt board state (design
            // spec §7), so the reply is decoded and re-emitted as
            // `boardOpened` -- the same signal the typed calls below emit.
            // Without it `BoardBridge::board` would only catch up on the next
            // poll tick, and every binding over it (the columns, the task
            // cards, the comment list) would sit stale after a submit the user
            // just made.
            GetBoardResult state;
            if (glz::read_json(state, resultJson)) {
                emit formReplyReceived(actionType, false,
                                       QStringLiteral("the action succeeded but its reply could not be decoded"));
                return;
            }
            emit boardOpened(std::move(state));
            if (decodedComment) {
                emit commentAdded(idText(commentTaskId));
            }
            emit formReplyReceived(actionType, true, QString::fromStdString(resultJson));
        },
        [this, actionType](const std::exception_ptr& err) {
            emit formReplyReceived(actionType, false, ::morph::ladder::gui::errorText(err));
        });
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
        _handler.execute(CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = title.toStdString()}),
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
        [this, taskId](GetBoardResult) { emit taskMoved(idText(taskId)); },
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
        [this, taskId](GetBoardResult) { emit commentAdded(idText(taskId)); },
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

void BoardPresenter::createRule(ColumnId triggerColumnId, const QString& mutationType, const QString& mutationValue) {
    track<CreateRuleResult>(
        _handler.execute(CreateRule{.projectId = _projectId,
                                    .triggerColumnId = triggerColumnId,
                                    .mutationType = ruleMutationTypeFromString(mutationType.toStdString()),
                                    .mutationValue = mutationValue.toStdString()}),
        [this](CreateRuleResult) { emit ruleCreated(); }, [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::getRules() {
    track<GetRulesResult>(
        _handler.execute(GetRules{.projectId = _projectId}),
        [this](GetRulesResult result) { emit rulesListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BoardPresenter::deleteRule(RuleId ruleId) {
    track<Ack>(
        _handler.execute(DeleteRule{.ruleId = ruleId}), [this](Ack) { emit ruleDeleted(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

::morph::async::Completion<Ack> BoardPresenter::addAttachment(TaskId taskId, const QString& filename,
                                                              const QString& contentType, std::int64_t sizeBytes,
                                                              const QString& storageKey) {
    return _handler.execute(AddAttachment{.taskId = taskId,
                                          .filename = filename.toStdString(),
                                          .contentType = contentType.toStdString(),
                                          .sizeBytes = sizeBytes,
                                          .storageKey = storageKey.toStdString()});
}

void BoardPresenter::getAttachments(TaskId taskId) {
    track<GetAttachmentsResult>(
        _handler.execute(GetAttachments{.taskId = taskId}),
        [this](GetAttachmentsResult result) { emit attachmentsListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace kanban::gui
