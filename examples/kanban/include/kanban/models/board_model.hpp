// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kanban/core/errors.hpp"
#include "kanban/core/types.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/event_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>

#include <cstdint>
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
    GetBoardResult execute(const OpenBoard& action);

    /// @brief Returns the current state of this handler's attached board.
    /// @param action Unused -- carries no fields.
    /// @return The attached board's full state.
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
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
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateTask& action);

    /// @brief Appends a comment to the given task.
    /// @param action The target task id and comment body.
    /// @return The board's full state after the comment is appended.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `taskId` or an empty body).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
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
    GetBoardResult execute(const MoveTaskPosition& action);

    /// @brief Design spec §1's polling read side -- lists every
    ///        `board_events` row after `action.lastEventId`, oldest first.
    /// @param action The cursor to list events after; `{}` (its default)
    ///        means "from the beginning".
    /// @return Every matching event, oldest first.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (a negative `lastEventId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`.
    GetEventsSinceResult execute(const GetEventsSince& action);

  private:
    /// @brief Throws `Forbidden` unless the calling principal's role on
    ///        this handler's attached project is at least `minimum`. Same
    ///        shape as `ProjectAdminModel::requireRole` -- not shared code
    ///        (design spec §3): `BoardModel` and `ProjectAdminModel` are
    ///        separate classes with separate mapper/entity access, so each
    ///        gets its own copy.
    /// @param minimum The minimum role the caller must hold.
    /// @throws Forbidden if no principal is authenticated, or the caller
    ///         has no role on the attached project, or a role below
    ///         `minimum`.
    void requireRole(Role minimum) const;

    /// @brief The project this handler is attached to, cached on the first
    ///        successful `execute(OpenBoard)`. Unset until then.
    std::optional<std::string> _projectIdStr;
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
        return morph::model::keyToString(static_cast<std::int64_t>(*action.projectId));
    }
};
template <>
struct morph::model::ModelKeyTraits<kanban::BoardModel> {
    using PrimaryKey = std::int64_t;
};
