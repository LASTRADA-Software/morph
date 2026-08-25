// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <exception>
#include <string>

#include "gui/presenter.hpp"
#include "kanban/dto/activity_dto.hpp"
#include "kanban/dto/attachment_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/event_dto.hpp"

// See kanban::gui::ProjectAdminPresenter's identical guard and doc comment
// (project_admin_presenter.hpp) for why moc must never see
// morph/core/bridge.hpp or this rung's model headers: moc is not a C++
// front end and mis-parses their template machinery.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "kanban/models/board_model.hpp"
#endif

namespace kanban::gui {

/// @brief Drives `kanban::BoardModel` for the board view: attaching to a
///        project's board, creating columns/swimlanes/tasks, moving a task,
///        adding a comment, and reading the activity/event streams. Routes
///        every action through its own `BridgeHandler<BoardModel,
///        AllowShared>` and translates the typed result into a Qt signal —
///        no QML dependency, no domain logic
///        (`examples/IMPLEMENTATION.md` rule 2's "translates and routes; it
///        never decides").
///
/// `AllowShared`, not a plain handler: `BoardModel` is keyed per-project
/// (`morph::model::ModelKeyTraits<BoardModel>`, `board_model.hpp`), so every
/// client attached to the same project must join the same shared-instance
/// directory the keyed `OpenBoard` attach relies on — a plain (`NoSharing`)
/// handler registers its own private instance eagerly at construction and
/// never attaches to another's, which would defeat the whole point of
/// sharing one board across clients (mirrors `PollPresenter`'s own
/// `_handler` — see that class's doc comment for the identical rationale).
///
/// `moveTask` takes an already-generated `opId`, not the identifiers to move
/// bare: the *bridge*, not this presenter, mints the id
/// (`QUuid::createUuid().toString()`, GUI design spec §6.2 step 4) — this
/// class stays transport-only, exactly like every other presenter in this
/// rung.
class BoardPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    BoardPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Attaches this handler to `projectId`'s board. Emits
    ///        `boardOpened` on success, `failed` on error.
    /// @param projectId The project whose board to attach to.
    void openBoard(ProjectId projectId);

    /// @brief Returns the current state of this handler's attached board.
    ///        Emits `boardOpened` on success, `failed` on error.
    void getBoardState();

    /// @brief Creates a new column on this handler's attached board. Emits
    ///        `boardOpened` (with the board's full post-creation state) on
    ///        success, `failed` on error.
    /// @param name     The column's name.
    /// @param wipLimit The column's WIP limit (`0` = unlimited).
    void createColumn(const QString& name, std::int64_t wipLimit);

    /// @brief Creates a new swimlane on this handler's attached board. Emits
    ///        `boardOpened` on success, `failed` on error.
    /// @param name The swimlane's name.
    void createSwimlane(const QString& name);

    /// @brief Creates a new task in the given column/swimlane. Emits
    ///        `boardOpened` on success, `failed` on error.
    /// @param columnId   The task's target column.
    /// @param swimlaneId The task's target swimlane.
    /// @param title      The task's title.
    void createTask(ColumnId columnId, SwimlaneId swimlaneId, const QString& title);

    /// @brief Moves `taskId` to `(columnId, swimlaneId)` at `position`,
    ///        replaying idempotently if `opId` has already been applied.
    ///        Emits `taskMoved(taskId)` on success, `failed` on error.
    ///
    ///        @p opId is the bridge's own id, generated once per user
    ///        gesture (`QUuid::createUuid().toString()`) — this method never
    ///        generates one itself, keeping the presenter transport-only.
    /// @param taskId     The task to move.
    /// @param columnId   The destination column.
    /// @param swimlaneId The destination swimlane.
    /// @param position   The destination position within `(columnId, swimlaneId)`.
    /// @param opId       The idempotency key this specific move was minted
    ///        with — captured alongside its own call's completion below
    ///        (see the design brief's cross-contamination lesson from
    ///        `ProjectAdminBridge::createProject`'s own fix round), never
    ///        stashed on a shared member.
    void moveTask(TaskId taskId, ColumnId columnId, SwimlaneId swimlaneId, std::int64_t position, QString opId);

    /// @brief Dedicated `Completion`-returning overload of `moveTask`, for
    ///        `BoardBridge`'s offline-queue replay path only
    ///        (`enableOfflineQueue()`) — never called from QML.
    ///
    ///        `moveTask()` above cannot serve a replay: it reports outcome
    ///        only through the shared `taskMoved(QString)`/`failed(QString)`
    ///        signals every other action on this presenter also uses, so a
    ///        concurrent user-driven `moveTask()` racing a queued replay could
    ///        have its outcome cross-attributed to the replay, or vice versa —
    ///        exactly the hazard `getEventsSinceForPolling`'s own doc comment
    ///        (just above) already documents for the identical reason, and
    ///        the same "no shared mutable field carries one call's data"
    ///        lesson this rung's `moveTask()` doc comment cites. This overload
    ///        instead dispatches directly through `_handler.execute()`,
    ///        returning that call's own independent `Completion<
    ///        GetBoardResult>` — identical in shape and rationale to
    ///        `getEventsSinceForPolling`.
    /// @param taskId     The task to move.
    /// @param columnId   The destination column.
    /// @param swimlaneId The destination swimlane.
    /// @param position   The destination position within `(columnId, swimlaneId)`.
    /// @param opId       The idempotency key this specific move was minted with.
    /// @return The call's own completion — nothing else can be attributed to it.
    [[nodiscard]] ::morph::async::Completion<GetBoardResult> moveTaskForReplay(TaskId taskId, ColumnId columnId,
                                                                               SwimlaneId swimlaneId,
                                                                               std::int64_t position, QString opId);

    /// @brief Appends a comment to a task on this handler's attached board.
    ///        Emits `commentAdded(taskId)` on success, `failed` on error.
    /// @param taskId The task to comment on.
    /// @param body   The comment's body.
    void addComment(TaskId taskId, const QString& body);

    /// @brief Lists every `board_events` row after `lastEventId`. Emits
    ///        `eventsReceived` on success, `failed` on error.
    /// @param lastEventId The cursor to list events after.
    void getEventsSince(BoardEventId lastEventId);

    /// @brief Lists every journal-derived activity entry for this handler's
    ///        attached board. Emits `activityUpdated` on success, `failed`
    ///        on error.
    void getActivity();

    /// @brief Dedicated `Completion`-returning overload of `getEventsSince`,
    ///        for `morph::ladder::gui::EventPoller`'s `Dispatch` closure only
    ///        (`BoardBridge::startPolling`) — never called from QML.
    ///
    /// `getEventsSince(BoardEventId)` above cannot serve as a poller's
    /// `Dispatch`: it is `void` and reports through the shared
    /// `eventsReceived`/`failed` signals every other action on this
    /// presenter also uses, so a concurrent in-flight action (e.g.
    /// `addComment`) racing a poll tick could have its outcome
    /// cross-attributed to the tick, or vice versa — exactly the hazard
    /// `event_poller.hpp`'s own doc comment warns against building a
    /// `Dispatch` out of. This overload instead dispatches directly through
    /// `_handler.execute()`, returning that call's own independent
    /// `Completion<GetEventsSinceResult>` — identical in shape and
    /// rationale to `polls::gui::PollFormsController::getEventsSince`
    /// (`poll_forms_controller.hpp`/`.cpp`), this rung's own precedent for
    /// exactly this seam.
    /// @param lastEventId The cursor to list events after.
    /// @return The call's own completion — nothing else can be attributed to
    ///         it.
    [[nodiscard]] ::morph::async::Completion<GetEventsSinceResult> getEventsSinceForPolling(BoardEventId lastEventId);

    /// @brief Creates a new automation rule on this handler's attached board:
    ///        "when a task moves into `triggerColumnId`, apply
    ///        `mutationType`/`mutationValue`." Manager-only. Emits
    ///        `ruleCreated` on success, `failed` on error.
    /// @param triggerColumnId The column whose arrival triggers this rule.
    /// @param mutationType    `"AddTag"` or `"RemoveTag"`.
    /// @param mutationValue   The tag name the mutation adds or removes.
    void createRule(ColumnId triggerColumnId, const QString& mutationType, const QString& mutationValue);

    /// @brief Lists every automation rule on this handler's attached board.
    ///        Emits `rulesListed` on success, `failed` on error.
    void getRules();

    /// @brief Deletes one automation rule. Manager-only. Emits `ruleDeleted`
    ///        on success, `failed` on error.
    /// @param ruleId The rule to delete.
    void deleteRule(RuleId ruleId);

    /// @brief Commits an attachment's metadata after its bytes have already
    ///        been uploaded through the separate HTTP side channel
    ///        (`kanban::http::AttachmentServer`, Task 17) -- @p storageKey is
    ///        that upload's own response, not something this method
    ///        interprets or validates itself (`attachment_dto.hpp`'s own
    ///        `@file` comment: "bytes over a side channel, metadata through
    ///        actions").
    ///
    ///        Returns its own `Completion<Ack>` rather than reporting through
    ///        a shared signal -- the same "no shared mutable field carries
    ///        one call's data" reasoning as `moveTaskForReplay`/
    ///        `getEventsSinceForPolling` above: `BoardBridge::uploadAttachment()`
    ///        chains this call after its own HTTP upload settles, and two
    ///        overlapping uploads (different tasks, or the same task twice)
    ///        must each resolve to their own outcome, never cross-attributed
    ///        via a shared `attachmentAdded(QString)`-style signal.
    /// @param taskId      The task to attach metadata to.
    /// @param filename    The uploaded file's original name.
    /// @param contentType The uploaded file's content type.
    /// @param sizeBytes   The uploaded file's size, in bytes.
    /// @param storageKey  The opaque key `AttachmentServer`'s upload response
    ///        returned.
    /// @return The call's own completion.
    [[nodiscard]] ::morph::async::Completion<Ack> addAttachment(TaskId taskId, const QString& filename,
                                                                const QString& contentType, std::int64_t sizeBytes,
                                                                const QString& storageKey);

    /// @brief Lists every attachment recorded against a task. Emits
    ///        `attachmentsListed`, or `failed`.
    /// @param taskId The task whose attachments to list.
    void getAttachments(TaskId taskId);

signals:
    /// @brief `OpenBoard`/`GetBoardState`/`CreateColumn`/`CreateSwimlane`/
    ///        `CreateTask` succeeded — the board's full rebuilt state (every
    ///        mutating action in this rung's DTOs returns it, design spec
    ///        §7), rendered as a property bag by the bridge layer.
    /// @param result The board's full current state.
    void boardOpened(kanban::GetBoardResult result);
    /// @brief `MoveTaskPosition` succeeded.
    /// @param taskId The moved task's id, as its plain number.
    void taskMoved(QString taskId);
    /// @brief `AddComment` succeeded.
    /// @param taskId The commented-on task's id, as its plain number.
    void commentAdded(QString taskId);
    /// @brief `GetEventsSince` succeeded.
    /// @param result Every matching event, oldest first.
    void eventsReceived(kanban::GetEventsSinceResult result);
    /// @brief `GetActivity` succeeded.
    /// @param result Every activity entry, oldest first.
    void activityUpdated(kanban::GetActivityResult result);
    /// @brief `CreateRule` succeeded.
    void ruleCreated();
    /// @brief `GetRules` succeeded.
    /// @param result Every rule on the attached board, in creation order.
    void rulesListed(kanban::GetRulesResult result);
    /// @brief `DeleteRule` succeeded.
    void ruleDeleted();
    /// @brief `GetAttachments` succeeded.
    /// @param result Every attachment on the requested task, in upload order.
    void attachmentsListed(kanban::GetAttachmentsResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

private:
    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument — see `Presenter::track()`'s doc comment
    ///        (`examples/common/gui/presenter.hpp`).
    /// @param err The failed completion's exception.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<BoardModel, ::morph::bridge::AllowShared> _handler;

    /// @brief The project `openBoard()` was last called with -- `CreateRule`/
    ///        `GetRules` both carry a `projectId` field their own
    ///        `validate()` requires engaged, even though `BoardModel::execute()`
    ///        never reads it back (the handler's own attach state, not the
    ///        DTO field, names the board -- see `board_model.cpp`'s
    ///        `execute(const GetRules&)` comment). Kept here purely to
    ///        satisfy that `validate()` gate; not consulted for RBAC or
    ///        board-selection, both of which stay attach-state-driven.
    ProjectId _projectId;
};

}  // namespace kanban::gui
