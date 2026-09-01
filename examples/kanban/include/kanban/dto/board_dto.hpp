// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <morph/forms/forms.hpp>
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

    /// `wipLimit` is genuinely optional *input*, not an optional field: 0 is
    /// its own documented "unlimited" value and is what the aggregate default
    /// above already supplies. Without this opt-out the generated form gates
    /// its Submit button on a number the user has no reason to type, and the
    /// hand-built control it replaced defaulted to 0 rather than demanding
    /// one. Left blank, `DynamicForm` omits the key entirely and glaze
    /// deserialises the default (`docs/spec/forms/forms.md`, "Empty state").
    static constexpr std::array<std::string_view, 1> optionalFields{"wipLimit"};

    /// Side-effectful, so the renderer must draw its own Submit button rather
    /// than firing the moment `name` is non-empty -- same declaration, for the
    /// same reason, as `kanban::Login` (auth_dto.hpp) and every other
    /// schema-driven form in this rung.
    static constexpr bool explicitSubmit = true;

    /// The one thing the generated label cannot say: what 0 means. "Wip Limit"
    /// is also the wrong casing for an acronym, which is exactly what
    /// `FieldMeta::label` exists to override.
    static constexpr std::array<::morph::forms::FieldMeta, 1> fieldMetadata{
        ::morph::forms::FieldMeta{.field = "wipLimit",
                                  .label = "WIP limit",
                                  .help = "Most tasks this column may hold. Leave empty for unlimited.",
                                  .placeholder = "unlimited"},
    };

    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= kMaxColumnNameBytes; }
};

struct CreateSwimlane {
    std::string name;

    /// See `CreateColumn::explicitSubmit`.
    static constexpr bool explicitSubmit = true;

    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= kMaxSwimlaneNameBytes; }
};

struct CreateTask {
    ColumnId columnId;
    SwimlaneId swimlaneId;
    std::string title;

    /// See `CreateColumn::explicitSubmit`.
    static constexpr bool explicitSubmit = true;

    /// `columnId`/`swimlaneId` are **context, not input**: a task is created
    /// into the column and swimlane whose card list the user is typing in, and
    /// the board view supplies both from the delegate that owns the form
    /// (`gui/qml/BoardView.qml`). `hidden` says exactly that to every
    /// renderer -- the fields still travel in the payload, and
    /// `BoardModel::execute()` still re-checks that both belong to the
    /// attached board, so this is presentation and never a security control
    /// (`docs/spec/forms/forms.md`, "Field metadata is not a security
    /// control").
    static constexpr std::array<::morph::forms::FieldMeta, 2> fieldMetadata{
        ::morph::forms::FieldMeta{.field = "columnId", .hidden = true},
        ::morph::forms::FieldMeta{.field = "swimlaneId", .hidden = true},
    };

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

    /// See `CreateColumn::explicitSubmit`.
    static constexpr bool explicitSubmit = true;

    /// `taskId` is context for the same reason `CreateTask`'s ids are: the
    /// comment goes on whichever task's detail popup is open, and
    /// `gui/qml/TaskDetailPopup.qml` supplies it. See that declaration's own
    /// comment for why `hidden` is presentation and not a control.
    static constexpr std::array<::morph::forms::FieldMeta, 2> fieldMetadata{
        ::morph::forms::FieldMeta{.field = "taskId", .hidden = true},
        ::morph::forms::FieldMeta{.field = "body", .label = "Comment", .placeholder = "add a comment"},
    };

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
