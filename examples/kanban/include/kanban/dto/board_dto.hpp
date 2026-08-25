// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kanban/core/types.hpp"

namespace kanban {

inline constexpr std::size_t kMaxColumnNameBytes = 100;
inline constexpr std::size_t kMaxSwimlaneNameBytes = 100;
inline constexpr std::size_t kMaxTaskTitleBytes = 200;

/// @brief Attaches this handler to `projectId`'s board -- the keyed attach
///        action, `BRIDGE_MODEL_KEY(BoardModel, OpenBoard, &OpenBoard::projectId)`.
struct OpenBoard {
    ProjectId projectId;

    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue(); }
};

/// @brief Returns the current state of this handler's attached board.
struct GetBoardState {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct CreateColumn {
    std::string name;
    std::int64_t wipLimit = 0;  // 0 = unlimited

    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= kMaxColumnNameBytes; }
};

struct CreateSwimlane {
    std::string name;

    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= kMaxSwimlaneNameBytes; }
};

struct CreateTask {
    ColumnId columnId;
    SwimlaneId swimlaneId;
    std::string title;

    [[nodiscard]] bool validate() const noexcept {
        return columnId.hasValue() && swimlaneId.hasValue() && !title.empty() && title.size() <= kMaxTaskTitleBytes;
    }
};

/// @brief Moves `taskId` to `(columnId, swimlaneId)` at `position` --
///        design spec §1's exactly-once centerpiece. `opId` is optional on
///        the wire (a caller not going through the offline queue need not
///        set one; an empty `opId` skips the ledger check entirely --
///        `BoardModel::execute()` treats "" as "no idempotency requested",
///        never as a literal ledger key) but is what the offline stack
///        (design spec §5) always sets.
struct MoveTaskPosition {
    TaskId taskId;
    ColumnId columnId;
    SwimlaneId swimlaneId;
    std::int64_t position = 0;
    std::string opId;

    static constexpr std::array<std::string_view, 1> optionalFields{"opId"};

    [[nodiscard]] bool validate() const noexcept {
        return taskId.hasValue() && columnId.hasValue() && swimlaneId.hasValue() && position >= 0;
    }
};

struct AddComment {
    TaskId taskId;
    std::string body;

    [[nodiscard]] bool validate() const noexcept { return taskId.hasValue() && !body.empty(); }
};

struct ColumnView {
    ColumnId id;
    std::string name;
    std::int64_t wipLimit = 0;
    std::int64_t taskCount = 0;
};

struct SwimlaneView {
    SwimlaneId id;
    std::string name;
};

struct TaskView {
    TaskId id;
    ColumnId columnId;
    SwimlaneId swimlaneId;
    std::string title;
    std::int64_t position = 0;
    /// @brief Free-form tag names attached to this task -- populated from the
    ///        `task_tags` join table (design spec §9's rules engine is the
    ///        only writer today, via `RuleMutationType::AddTag`/`RemoveTag`;
    ///        no manual tag-editing action exists yet). Order matches
    ///        `task_tags`' own row order (insertion order), not sorted.
    std::vector<std::string> tags;
};

struct CommentView {
    TaskId taskId;
    std::string principal;
    std::string body;
};

/// @brief The full rebuilt board state -- returned by every mutating action
///        in this file, per the ladder-wide "every mutating action returns
///        the full rebuilt state" convention (design spec §7).
struct GetBoardResult {
    ProjectId projectId;
    std::string name;
    std::vector<ColumnView> columns;
    std::vector<SwimlaneView> swimlanes;
    std::vector<TaskView> tasks;
    std::vector<CommentView> comments;
};

}  // namespace kanban
