// SPDX-License-Identifier: Apache-2.0
#include "kanban/dto/board_dto.hpp"

#include <catch2/catch_test_macros.hpp>

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

