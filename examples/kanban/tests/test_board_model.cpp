// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

#include <morph/session/session.hpp>

#include <catch2/catch_test_macros.hpp>

using morph::ladder::testkit::DbFixture;

namespace {

/// @brief See `bookmarks::test_bookmark_model.cpp`'s identical
///        `contextFor`/`ScopedPrincipal` pair for why this is not a
///        designated initializer (`-Wmissing-designated-field-initializers`
///        under this target's strict warnings).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

[[nodiscard]] kanban::ProjectId createProjectAs(const std::string& principal, const std::string& name) {
    const ScopedPrincipal p{principal};
    kanban::ProjectAdminModel admin;
    return admin.execute(kanban::CreateProject{.name = name}).id;
}
}  // namespace

TEST_CASE("OpenBoard attaches and returns the project's name with empty columns/tasks", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};

    const auto result = model.execute(kanban::OpenBoard{.projectId = projectId});
    CHECK(result.name == "Sprint Board");
    CHECK(result.columns.empty());
    CHECK(result.tasks.empty());
}

TEST_CASE("GetBoardState without a prior OpenBoard throws NotFound", "[kanban][model]") {
    DbFixture fixture;
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    CHECK_THROWS_AS(model.execute(kanban::GetBoardState{}), kanban::NotFound);
}

TEST_CASE("CreateColumn/CreateSwimlane/CreateTask populate GetBoardState", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    const auto afterColumn = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0});
    REQUIRE(afterColumn.columns.size() == 1);
    const auto columnId = afterColumn.columns.front().id;

    const auto afterSwimlane = model.execute(kanban::CreateSwimlane{.name = "Default"});
    REQUIRE(afterSwimlane.swimlanes.size() == 1);
    const auto swimlaneId = afterSwimlane.swimlanes.front().id;

    const auto afterTask =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Fix bug"});
    REQUIRE(afterTask.tasks.size() == 1);
    CHECK(afterTask.tasks.front().title == "Fix bug");
}

TEST_CASE("AddComment appends to GetBoardState's comments", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto columnId = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;

    const auto result = model.execute(kanban::AddComment{.taskId = taskId, .body = "looking into it"});
    REQUIRE(result.comments.size() == 1);
    CHECK(result.comments.front().body == "looking into it");
    CHECK(result.comments.front().principal == "alice");
}

TEST_CASE("MoveTaskPosition moves a task and renumbers positions densely", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto afterCol2 = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0});
    const auto col2 = afterCol2.columns.back().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;

    const auto result = model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = ""});
    const auto moved = result.tasks.front();
    CHECK(moved.columnId == col2);
    CHECK(moved.position == 0);
}

TEST_CASE("MoveTaskPosition rejects a move that would exceed the target column's WIP limit", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto afterCol2 = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 1});
    const auto col2 = afterCol2.columns.back().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskA =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "A"}).tasks.back().id;
    const auto taskB =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "B"}).tasks.back().id;

    // Filling col2 (limit 1) to capacity first.
    model.execute(
        kanban::MoveTaskPosition{.taskId = taskA, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    CHECK_THROWS_AS(model.execute(kanban::MoveTaskPosition{
                        .taskId = taskB, .columnId = col2, .swimlaneId = swimlaneId, .position = 1, .opId = ""}),
                    kanban::Conflict);
}

TEST_CASE("MoveTaskPosition with a repeated opId replays the stored result, not a fresh move", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto afterCol2 = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0});
    const auto col2 = afterCol2.columns.back().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;

    const auto first = model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = "op-1"});
    // A second CreateTask lands after the first move -- if the replay
    // re-derived state instead of replaying the ledgered result, the
    // replayed GetBoardResult would (wrongly) include this new task too.
    model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "New task"});

    const auto replayed = model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = "op-1"});
    CHECK(replayed.tasks.size() == first.tasks.size());
}

TEST_CASE("MoveTaskPosition into a column deleted mid-drag throws NotFound, not a silent orphan write",
          "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;
    // A column id that was never created -- stands in for "deleted between
    // GetBoard and MoveTaskPosition" (this rung has no DeleteColumn action
    // yet; the re-check this test proves exists is the same check that
    // catches a genuinely-deleted column once that action lands).
    const kanban::ColumnId neverExisted{99999};

    CHECK_THROWS_AS(model.execute(kanban::MoveTaskPosition{.taskId = taskId,
                                                            .columnId = neverExisted,
                                                            .swimlaneId = swimlaneId,
                                                            .position = 0,
                                                            .opId = ""}),
                    kanban::NotFound);
}

TEST_CASE("GetEventsSince returns every event after the cursor, oldest first", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0});
    model.execute(kanban::CreateSwimlane{.name = "Default"});

    const auto first = model.execute(kanban::GetEventsSince{.lastEventId = {}});
    CHECK(first.events.size() >= 2);  // at least the column-create and swimlane-create events

    const auto cursor = first.events.back().id;
    const auto colId = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.back().id;
    (void) colId;

    const auto second = model.execute(kanban::GetEventsSince{.lastEventId = cursor});
    REQUIRE(second.events.size() == 1);
}
