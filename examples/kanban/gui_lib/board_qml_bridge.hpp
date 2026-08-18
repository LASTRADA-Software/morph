// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Guarded exactly like board_presenter.hpp's own includes: AUTOMOC runs moc
// over this header, and moc must not be pointed at morph's template-heavy
// bridge.hpp or at the model headers — see that header's own doc comment for
// the full rationale (mirrors project_admin_qml_bridge.hpp's identical
// guard).
#ifndef Q_MOC_RUN
#include "board_presenter.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace kanban::gui {

/// @brief QML-facing face of `kanban::gui::BoardPresenter`.
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
    ///        `GetProjectRoles` (`ProjectAdminBridge`'s own surface) — kept
    ///        here, not derived from any `BoardModel` result, since no
    ///        action in this rung's board DTOs returns the caller's role
    ///        (design spec §4.3 lists `myRole` alongside `board`/`activity`/
    ///        `principal` as one of this bridge's own state properties; a
    ///        QML shell sets it via `setMyRole()` once
    ///        `ProjectAdminBridge::rolesListed` reports it for the logged-in
    ///        principal). Empty until set.
    Q_PROPERTY(QString myRole READ myRole NOTIFY myRoleChanged)

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

    /// @brief Test-only accessor: the `opId` the most recent `moveTask()`
    ///        call minted. Empty before the first call. Exists solely so
    ///        `test_board_qml_bridge.cpp` can assert that two calls never
    ///        reuse the same id (exactly-once semantics rely on a fresh id
    ///        per user gesture, not per session) — no production code reads
    ///        this.
    /// @return The most recent `moveTask()` call's minted `opId`.
    [[nodiscard]] QString lastOpIdForTest() const { return _lastOpIdForTest; }

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
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

  private:
    /// @brief Installs a `board` value and emits `boardChanged`.
    /// @param result The board's full current state.
    void applyBoard(const GetBoardResult& result);

#ifndef Q_MOC_RUN
    BoardPresenter _presenter;
#endif
    QVariantMap _board;
    QVariantList _activity;
    QString _myRole;
    QString _lastOpIdForTest;
};

}  // namespace kanban::gui
