// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kanban/core/errors.hpp"
#include "kanban/core/types.hpp"
#include "kanban/dto/activity_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/event_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

/// @file
/// `BoardModel` -- this rung's shared/keyed board model (design spec §2).
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration,
/// exactly like `bookmarks::BookmarkModel`/`polls::PollModel`.

namespace kanban {

class BoardModel {
  public:
    /// @brief Attaches this handler to `action.projectId`'s board -- the
    ///        keyed attach action.
    /// @param action The project to attach to; `action.projectId` becomes
    ///        this handler's cached attach state on success.
    /// @return The freshly attached board's full state.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `projectId`).
    /// @throws NotFound if `action.projectId` names no project.
    /// @throws Forbidden if the caller has no role (at least `Role::Viewer`)
    ///         on `action.projectId` -- checked against the *target* project,
    ///         not this handler's (not-yet-set) attach state, so an
    ///         authenticated principal with no standing on the project
    ///         cannot read it merely by attaching.
    GetBoardResult execute(const OpenBoard& action);

    /// @brief Returns the current state of this handler's attached board.
    /// @param action Unused -- carries no fields.
    /// @return The attached board's full state.
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetBoardResult execute(const GetBoardState& action);

    /// @brief Creates a new column on this handler's attached board.
    /// @param action The column's name and WIP limit (`0` = unlimited).
    /// @return The board's full state after the column is created.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an empty or over-length name).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateColumn& action);

    /// @brief Creates a new swimlane on this handler's attached board.
    /// @param action The swimlane's name.
    /// @return The board's full state after the swimlane is created.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an empty or over-length name).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateSwimlane& action);

    /// @brief Creates a new task in the given column/swimlane on this
    ///        handler's attached board.
    /// @param action The task's target column id, target swimlane id, and
    ///        title.
    /// @return The board's full state after the task is created.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `columnId`/`swimlaneId`, or an empty or
    ///         over-length title).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.columnId`/`action.swimlaneId` does not belong to the
    ///         attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateTask& action);

    /// @brief Appends a comment to the given task.
    /// @param action The target task id and comment body.
    /// @return The board's full state after the comment is appended.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `taskId` or an empty body).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.taskId` does not belong to the attached project.
    /// @throws Forbidden if no principal is authenticated on the calling
    ///         session, or the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const AddComment& action);

    /// @brief Design spec §1's exactly-once centerpiece -- added in Task 10.
    /// @param action The task's target position (column, swimlane,
    ///        position) and optional idempotency key.
    /// @return The board's full state after the move (or, for a
    ///         previously-applied `opId`, the replayed result).
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`. Checked before the idempotency-ledger
    ///         lookup, so a demoted caller replaying a known `opId` cannot
    ///         retrieve a stored result their current role could no longer
    ///         produce.
    /// @throws NotFound if `action.taskId` does not belong to the attached
    ///         project, or if `action.columnId`/`action.swimlaneId` does not.
    GetBoardResult execute(const MoveTaskPosition& action);

    /// @brief Design spec §1's polling read side -- lists every
    ///        `board_events` row after `action.lastEventId`, oldest first.
    /// @param action The cursor to list events after; `{}` (its default)
    ///        means "from the beginning".
    /// @return Every matching event, oldest first.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (a negative `lastEventId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetEventsSinceResult execute(const GetEventsSince& action);

    /// @brief Design spec §4's activity stream -- derived from `IActionLog::
    ///        entries(entityKey)`, not a parallel table.
    /// @param action Unused -- carries no fields.
    /// @return Every activity entry for this handler's attached board,
    ///         oldest first. Empty (not an error) if this handler has no log
    ///         attached.
    /// @throws NotFound if this handler was never attached via `OpenBoard`.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetActivityResult execute(const GetActivity& action);

    /// @brief Attaches a durable action log and this instance's stable
    ///        identity, so every subsequent mutating `execute()` records a
    ///        `morph::journal::LogEntry` that `execute(GetActivity)` can
    ///        later read back.
    ///
    /// This is a **model-level** mirror of `morph::model::detail::
    /// IModelHolder::attachActionLog` -- not a call into that framework
    /// method. `IModelHolder::attachActionLog`/`recordIfAttached` live on the
    /// type-erased holder that wraps a *registry-constructed* model (created
    /// via `ModelFactory::create<Model>()` and dispatched through
    /// `ActionDispatcher`/`Bridge::executeVia`); a `BoardModel` a unit test
    /// (or any caller) constructs directly with `kanban::BoardModel model;`
    /// has no such holder wrapping it; `model.execute(action)` calls
    /// `BoardModel::execute` straight, never touching `IModelHolder` or the
    /// dispatcher's runner, so `recordIfAttached`'s auto-append never fires
    /// for this path. `BoardModel` therefore keeps its own
    /// `shared_ptr<IActionLog>` and appends its own `LogEntry` at the end of
    /// every successful mutating `execute()` (see `logAction` below) --
    /// functionally the same effect `recordIfAttached` gives a
    /// holder-wrapped instance, achieved without one.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every `LogEntry` this
    ///        instance produces (this rung's project id, as a string).
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

  private:
    /// @brief Records @p action/@p result as a `LogEntry` if a log is
    ///        attached; no-op otherwise. Called at the end of every
    ///        successful mutating `execute()` -- the model-level equivalent
    ///        of `IModelHolder::recordIfAttached` for a plain, non-holder-
    ///        wrapped `BoardModel` instance (see `attachActionLog`'s doc
    ///        comment for why this instance cannot rely on the framework's
    ///        own auto-append instead). Flushes `_log` after appending, so a
    ///        `GetActivity` call immediately afterward (the common case: a
    ///        client polls right after its own mutating call) reliably sees
    ///        the entry even when `_log` is a `FileActionLog` -- `append()`
    ///        writes through buffered C stdio with no implicit flush, and
    ///        `entries()` reads through a separate `ifstream` that cannot
    ///        see unflushed bytes still sitting in that buffer.
    /// @tparam Action Concrete action type; used to look up
    ///         `morph::model::ActionTraits<Action>::typeId()`/`toJson()`.
    /// @tparam Result Concrete result type; used to look up
    ///         `morph::model::ActionTraits<Action>::resultToJson()`.
    /// @param action The executed action, for its type-id and JSON payload.
    /// @param result The action's result, for its JSON encoding.
    template <typename Action, typename Result>
    void logAction(const Action& action, const Result& result) const;

    /// @brief Throws `Forbidden` unless the calling principal's role on
    ///        this handler's attached project is at least `minimum`. Same
    ///        shape as `ProjectAdminModel::requireRole` -- not shared code
    ///        (design spec §3): `BoardModel` and `ProjectAdminModel` are
    ///        separate classes with separate mapper/entity access, so each
    ///        gets its own copy. Delegates to `requireRoleOn` using this
    ///        handler's own `_projectIdStr` as the target project -- every
    ///        call site attached via `OpenBoard` keeps working unchanged.
    /// @param minimum The minimum role the caller must hold.
    /// @throws Forbidden if no principal is authenticated, or the caller
    ///         has no role on the attached project, or a role below
    ///         `minimum`.
    void requireRole(Role minimum) const;

    /// @brief Throws `Forbidden` unless the calling principal's role on
    ///        @p projectDbId is at least `minimum`. The explicit-project
    ///        variant `requireRole(Role)` cannot use: `execute(OpenBoard)`
    ///        must gate access to the project it is *attaching to*, before
    ///        `_projectIdStr` (which `OpenBoard` itself sets) is available
    ///        to read.
    /// @param projectDbId The project to check the caller's role against.
    /// @param minimum The minimum role the caller must hold.
    /// @throws Forbidden if no principal is authenticated, or the caller
    ///         has no role on @p projectDbId, or a role below `minimum`.
    void requireRoleOn(std::uint64_t projectDbId, Role minimum) const;

    /// @brief The project this handler is attached to, cached on the first
    ///        successful `execute(OpenBoard)`. Also set (independently) by
    ///        `attachActionLog`, whose `entityKey` parameter is the string
    ///        form of the same project id in every path this rung exercises
    ///        -- `OpenBoard` overwrites it with the identical value, so the
    ///        two writers never disagree in practice. Unset until the first
    ///        of either call.
    std::optional<std::string> _projectIdStr;

    /// @brief Durable action log this instance appends to, if any -- set by
    ///        `attachActionLog`. Null (the default) for a handler that never
    ///        had one attached; every `logAction` call is then a no-op and
    ///        `execute(GetActivity)` returns an empty stream rather than
    ///        throwing (design spec §4: "Local-mode-without-attach is a
    ///        stated limitation", not an error).
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace kanban

BRIDGE_REGISTER_MODEL(kanban::BoardModel, "BoardModel")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::OpenBoard, "OpenBoard", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetBoardState, "GetBoardState", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateColumn, "CreateColumn")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateSwimlane, "CreateSwimlane")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateTask, "CreateTask")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::AddComment, "AddComment")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::MoveTaskPosition, "MoveTaskPosition")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetEventsSince, "GetEventsSince", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetActivity, "GetActivity", ::morph::model::Loggable::No)

// `BRIDGE_MODEL_KEY(kanban::BoardModel, kanban::OpenBoard, &kanban::OpenBoard::projectId)`
// cannot be used verbatim here: that macro deduces the model's PrimaryKey as
// the *type* of the pointed-to member (`morph::model::detail::MemberTypeOf`),
// which for `&OpenBoard::projectId` is `kanban::ProjectId` -- a struct
// wrapping `std::optional<std::int64_t>`, not an integral or `std::string`,
// so it fails `morph::model::ModelKey`'s concept. `BoardModel` is keyed on
// the same value in its unwrapped, wire-canonical form (`std::int64_t`)
// instead, by hand-writing the two specializations the macro would otherwise
// generate.
template <>
struct morph::model::ActionKeyTraits<kanban::OpenBoard> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const kanban::OpenBoard& action) {
        return morph::model::keyToString(*action.projectId);
    }
};
template <>
struct morph::model::ModelKeyTraits<kanban::BoardModel> {
    using PrimaryKey = std::int64_t;
};
