// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "kanban/core/errors.hpp"
#include "kanban/core/types.hpp"
#include "kanban/dto/board_dto.hpp"

TEST_CASE("OpenBoard requires an engaged projectId", "[kanban][dto]") {
    CHECK_FALSE(kanban::OpenBoard{.projectId = {}}.validate());
    CHECK(kanban::OpenBoard{.projectId = kanban::ProjectId{1}}.validate());
}

TEST_CASE("CreateColumn requires a non-empty, bounded name", "[kanban][dto]") {
    CHECK_FALSE(kanban::CreateColumn{.name = ""}.validate());
    CHECK_FALSE(kanban::CreateColumn{.name = std::string(101, 'x')}.validate());
    CHECK(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}.validate());
}

TEST_CASE("CreateTask requires engaged columnId/swimlaneId and a bounded title", "[kanban][dto]") {
    kanban::CreateTask valid{.columnId = kanban::ColumnId{1}, .swimlaneId = kanban::SwimlaneId{1}, .title = "Fix bug"};
    CHECK(valid.validate());

    kanban::CreateTask noColumn = valid;
    noColumn.columnId = {};
    CHECK_FALSE(noColumn.validate());

    kanban::CreateTask emptyTitle = valid;
    emptyTitle.title = "";
    CHECK_FALSE(emptyTitle.validate());
}

TEST_CASE("MoveTaskPosition requires an engaged taskId/columnId/swimlaneId and a non-negative position",
          "[kanban][dto]") {
    kanban::MoveTaskPosition valid{.taskId = kanban::TaskId{1},
                                   .columnId = kanban::ColumnId{1},
                                   .swimlaneId = kanban::SwimlaneId{1},
                                   .position = 0};
    CHECK(valid.validate());

    kanban::MoveTaskPosition negative = valid;
    negative.position = -1;
    CHECK_FALSE(negative.validate());

    kanban::MoveTaskPosition noTask = valid;
    noTask.taskId = {};
    CHECK_FALSE(noTask.validate());
}

TEST_CASE("AddComment requires an engaged taskId and non-empty body", "[kanban][dto]") {
    CHECK_FALSE(kanban::AddComment{.taskId = {}, .body = "hi"}.validate());
    CHECK_FALSE(kanban::AddComment{.taskId = kanban::TaskId{1}, .body = ""}.validate());
    CHECK(kanban::AddComment{.taskId = kanban::TaskId{1}, .body = "hi"}.validate());
}

TEST_CASE("kanban::BoardEventId: fromRowId rejects the one value it cannot represent", "[kanban][types]") {
    // 0 is BoardEventId's "not entered" sentinel, so an event id of 0 would
    // arrive as *absent* and a real event would read as "no event"
    // (morph#215). Row ids start at 1, so this never fires in practice -- it
    // turns a silent collapse into a loud failure at the boundary.
    CHECK_THROWS_AS(kanban::BoardEventId::fromRowId(0), kanban::KanbanError);
}

TEST_CASE("kanban::BoardEventId: fromRowId wraps an ordinary row id unchanged", "[kanban][types]") {
    // Control case: without it the check above would pass against a factory
    // that rejected every input.
    auto const event = kanban::BoardEventId::fromRowId(42);
    CHECK(event.hasValue());
    CHECK(*event == 42);
}
