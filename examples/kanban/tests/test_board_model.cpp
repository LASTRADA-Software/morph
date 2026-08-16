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
