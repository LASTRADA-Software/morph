// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

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
    CHECK(result.comments.front().taskId == taskId);
}

TEST_CASE("GetBoardState's comments each carry the taskId of the task they belong to, not just the board's",
          "[kanban][model]") {
    // Regression test for the QML TaskDetailPopup gap: CommentView used to
    // have no taskId, so a board with comments on more than one task could
    // not be filtered client-side to just the tapped task's own comments --
    // every comment on the whole board looked identical once serialized.
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto columnId = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;

    const auto taskA =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Task A"})
            .tasks.front()
            .id;
    const auto afterTaskB =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Task B"});
    const auto taskB = std::ranges::find_if(afterTaskB.tasks, [](const auto& t) { return t.title == "Task B"; })->id;

    model.execute(kanban::AddComment{.taskId = taskA, .body = "comment on A"});
    const auto result = model.execute(kanban::AddComment{.taskId = taskB, .body = "comment on B"});

    REQUIRE(result.comments.size() == 2);
    const auto commentOnA =
        std::ranges::find_if(result.comments, [](const auto& c) { return c.body == "comment on A"; });
    const auto commentOnB =
        std::ranges::find_if(result.comments, [](const auto& c) { return c.body == "comment on B"; });
    REQUIRE(commentOnA != result.comments.end());
    REQUIRE(commentOnB != result.comments.end());
    CHECK(commentOnA->taskId == taskA);
    CHECK(commentOnB->taskId == taskB);
    CHECK(commentOnA->taskId != commentOnB->taskId);

    // What TaskDetailPopup.qml's own filter now expresses client-side: only
    // taskA's comments should survive a filter keyed on taskA's id.
    const auto commentsForTaskA = std::ranges::count_if(
        result.comments, [&](const auto& c) { return c.taskId == taskA; });
    CHECK(commentsForTaskA == 1);
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

TEST_CASE("MoveTaskPosition across columns also renumbers the source column densely, not just the destination",
          "[kanban][model]") {
    // Regression test: the destination-only renumbering pass leaves a gap
    // behind in the column a task departs from -- e.g. moving the task that
    // sat at position 2 out of a 5-task column left the other four at
    // {0, 1, 3, 4} forever instead of {0, 1, 2, 3}, silently violating design
    // spec §2's "position is dense within its (columnId, swimlaneId) pair"
    // invariant for the source side. Task 19's concurrent-move stress test
    // (test_kanban_stress.cpp) caught this by chance via random cross-column
    // moves; this is the minimal, deterministic single-threaded reproduction.
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto col2 = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.back().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;

    // Five tasks in col1, at positions 0..4 in creation order.
    std::vector<kanban::TaskId> taskIds;
    for (int i = 0; i < 5; ++i) {
        const auto after = model.execute(
            kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "Task " + std::to_string(i)});
        taskIds.push_back(after.tasks.back().id);
    }

    // Move the task at position 2 (taskIds[2]) out to col2.
    const auto result = model.execute(kanban::MoveTaskPosition{
        .taskId = taskIds[2], .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    std::vector<std::int64_t> col1Positions;
    for (const auto& task : result.tasks) {
        if (task.columnId == col1) {
            col1Positions.push_back(task.position);
        }
    }
    std::sort(col1Positions.begin(), col1Positions.end());
    CHECK(col1Positions == std::vector<std::int64_t>{0, 1, 2, 3});
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

TEST_CASE("A Viewer cannot CreateTask or MoveTaskPosition -- Forbidden, not a silent write", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    {
        kanban::ProjectAdminModel admin;
        const ScopedPrincipal alice{"alice"};
        admin.execute(kanban::SetMemberRole{.projectId = projectId, .principal = "bob", .role = kanban::Role::Viewer});
    }

    kanban::BoardModel model;
    const ScopedPrincipal bob{"bob"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    CHECK_THROWS_AS(model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}), kanban::Forbidden);
}

TEST_CASE("A Member can CreateTask; GetBoardState needs no role at all beyond Viewer", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    {
        kanban::ProjectAdminModel admin;
        const ScopedPrincipal alice{"alice"};
        admin.execute(kanban::SetMemberRole{.projectId = projectId, .principal = "bob", .role = kanban::Role::Member});
    }

    kanban::BoardModel model;
    const ScopedPrincipal bob{"bob"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    CHECK_NOTHROW(model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}));
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

TEST_CASE("GetActivity lists journal entries for this board", "[kanban][model]") {
    DbFixture fixture;
    auto log = std::make_shared<::morph::journal::InMemoryActionLog>();
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.attachActionLog(log, std::to_string(*projectId));
    model.execute(kanban::OpenBoard{.projectId = projectId});
    model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0});

    const auto activity = model.execute(kanban::GetActivity{});
    // At least one entry for the CreateColumn call -- OpenBoard/GetBoardState
    // are Loggable::No, so they never appear.
    REQUIRE(activity.events.size() >= 1);
    CHECK(activity.events.front().actionType == "CreateColumn");
}

TEST_CASE("GetActivity without an attached log returns an empty stream, not an error", "[kanban][model]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0});

    const auto activity = model.execute(kanban::GetActivity{});
    CHECK(activity.events.empty());
}

TEST_CASE("GetActivity shows a single entry for a repeated-opId MoveTaskPosition -- the replay journals nothing",
          "[kanban][model]") {
    DbFixture fixture;
    auto log = std::make_shared<::morph::journal::InMemoryActionLog>();
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.attachActionLog(log, std::to_string(*projectId));
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto afterCol2 = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0});
    const auto col2 = afterCol2.columns.back().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;

    model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = "op-1"});
    // Replaying the identical opId must not double-journal (design spec §4,
    // corrected: a ledger hit performs nothing new and no longer logs
    // anything -- verified here by confirming only one entry exists, not by
    // a read-side collapse).
    model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = "op-1"});

    const auto activity = model.execute(kanban::GetActivity{});
    const auto moveCount = std::ranges::count_if(
        activity.events, [](const auto& event) { return event.actionType == "MoveTaskPosition"; });
    CHECK(moveCount == 1);
}

// C2: cross-tenant write re-checks. bob is a Member of project A only;
// project B belongs to alice and bob has no standing on it whatsoever.
// Each of these attempts a write against project B's own column/task ids
// while bob's handler is attached to *project A* -- the attack C2
// describes is supplying another project's row id by number, not attaching
// to the wrong project (that is C1's attack, covered elsewhere). Every one
// of these must throw NotFound, not silently corrupt or leak into the
// other project's board.

TEST_CASE("CreateTask rejects a columnId that belongs to a different project", "[kanban][model][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Project A");
    const auto projectB = createProjectAs("alice", "Project B");
    {
        kanban::ProjectAdminModel admin;
        const ScopedPrincipal alice{"alice"};
        admin.execute(kanban::SetMemberRole{.projectId = projectA, .principal = "bob", .role = kanban::Role::Member});
    }

    // Seed project B's own column/swimlane as alice.
    kanban::ColumnId columnOnB;
    kanban::SwimlaneId swimlaneOnB;
    {
        kanban::BoardModel modelB;
        const ScopedPrincipal alice{"alice"};
        modelB.execute(kanban::OpenBoard{.projectId = projectB});
        columnOnB = modelB.execute(kanban::CreateColumn{.name = "B's column", .wipLimit = 0}).columns.front().id;
        swimlaneOnB = modelB.execute(kanban::CreateSwimlane{.name = "B's swimlane"}).swimlanes.front().id;
    }

    // bob attaches to project A (where he is a genuine Member) and tries to
    // create a task pointing at project B's column/swimlane by id.
    kanban::BoardModel modelA;
    const ScopedPrincipal bob{"bob"};
    modelA.execute(kanban::OpenBoard{.projectId = projectA});
    CHECK_THROWS_AS(
        modelA.execute(kanban::CreateTask{.columnId = columnOnB, .swimlaneId = swimlaneOnB, .title = "Sneaky task"}),
        kanban::NotFound);
}

TEST_CASE("CreateTask rejects a swimlaneId that belongs to a different project, even with a valid columnId",
          "[kanban][model][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Project A");
    const auto projectB = createProjectAs("alice", "Project B");
    {
        kanban::ProjectAdminModel admin;
        const ScopedPrincipal alice{"alice"};
        admin.execute(kanban::SetMemberRole{.projectId = projectA, .principal = "bob", .role = kanban::Role::Member});
    }

    kanban::SwimlaneId swimlaneOnB;
    {
        kanban::BoardModel modelB;
        const ScopedPrincipal alice{"alice"};
        modelB.execute(kanban::OpenBoard{.projectId = projectB});
        swimlaneOnB = modelB.execute(kanban::CreateSwimlane{.name = "B's swimlane"}).swimlanes.front().id;
    }

    kanban::BoardModel modelA;
    const ScopedPrincipal bob{"bob"};
    modelA.execute(kanban::OpenBoard{.projectId = projectA});
    const auto columnOnA = modelA.execute(kanban::CreateColumn{.name = "A's column", .wipLimit = 0}).columns.front().id;
    CHECK_THROWS_AS(
        modelA.execute(kanban::CreateTask{.columnId = columnOnA, .swimlaneId = swimlaneOnB, .title = "Sneaky task"}),
        kanban::NotFound);
}

TEST_CASE("AddComment rejects a taskId that belongs to a different project", "[kanban][model][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Project A");
    const auto projectB = createProjectAs("alice", "Project B");
    {
        kanban::ProjectAdminModel admin;
        const ScopedPrincipal alice{"alice"};
        admin.execute(kanban::SetMemberRole{.projectId = projectA, .principal = "bob", .role = kanban::Role::Member});
    }

    kanban::TaskId taskOnB;
    {
        kanban::BoardModel modelB;
        const ScopedPrincipal alice{"alice"};
        modelB.execute(kanban::OpenBoard{.projectId = projectB});
        const auto colB = modelB.execute(kanban::CreateColumn{.name = "B's column", .wipLimit = 0}).columns.front().id;
        const auto swB = modelB.execute(kanban::CreateSwimlane{.name = "B's swimlane"}).swimlanes.front().id;
        taskOnB = modelB.execute(kanban::CreateTask{.columnId = colB, .swimlaneId = swB, .title = "B's task"})
                      .tasks.front()
                      .id;
    }

    // bob, attached to project A, tries to inject a comment onto project
    // B's task by id -- if this succeeded, the comment would surface in
    // project B's own GetBoardState (buildState's comment list), a
    // cross-tenant content injection into a board bob has no standing on.
    kanban::BoardModel modelA;
    const ScopedPrincipal bob{"bob"};
    modelA.execute(kanban::OpenBoard{.projectId = projectA});
    CHECK_THROWS_AS(modelA.execute(kanban::AddComment{.taskId = taskOnB, .body = "sneaky comment"}),
                    kanban::NotFound);

    // Confirm no injection happened: project B's own board still shows zero
    // comments.
    kanban::BoardModel checkB;
    const ScopedPrincipal alice{"alice"};
    const auto stateB = checkB.execute(kanban::OpenBoard{.projectId = projectB});
    CHECK(stateB.comments.empty());
}

TEST_CASE("MoveTaskPosition rejects a taskId that belongs to a different project", "[kanban][model][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Project A");
    const auto projectB = createProjectAs("alice", "Project B");
    {
        kanban::ProjectAdminModel admin;
        const ScopedPrincipal alice{"alice"};
        admin.execute(kanban::SetMemberRole{.projectId = projectA, .principal = "bob", .role = kanban::Role::Member});
    }

    kanban::TaskId taskOnB;
    {
        kanban::BoardModel modelB;
        const ScopedPrincipal alice{"alice"};
        modelB.execute(kanban::OpenBoard{.projectId = projectB});
        const auto colB = modelB.execute(kanban::CreateColumn{.name = "B's column", .wipLimit = 0}).columns.front().id;
        const auto swB = modelB.execute(kanban::CreateSwimlane{.name = "B's swimlane"}).swimlanes.front().id;
        taskOnB = modelB.execute(kanban::CreateTask{.columnId = colB, .swimlaneId = swB, .title = "B's task"})
                      .tasks.front()
                      .id;
    }

    // bob, attached to project A, tries to move project B's task into one
    // of project A's own columns by id.
    kanban::BoardModel modelA;
    const ScopedPrincipal bob{"bob"};
    modelA.execute(kanban::OpenBoard{.projectId = projectA});
    const auto colA = modelA.execute(kanban::CreateColumn{.name = "A's column", .wipLimit = 0}).columns.front().id;
    const auto swA = modelA.execute(kanban::CreateSwimlane{.name = "A's swimlane"}).swimlanes.front().id;
    CHECK_THROWS_AS(modelA.execute(kanban::MoveTaskPosition{
                        .taskId = taskOnB, .columnId = colA, .swimlaneId = swA, .position = 0, .opId = ""}),
                    kanban::NotFound);

    // Confirm no orphaning happened: project B's task is still there, still
    // in its own project's column.
    kanban::BoardModel checkB;
    const ScopedPrincipal alice{"alice"};
    const auto stateB = checkB.execute(kanban::OpenBoard{.projectId = projectB});
    const auto found = std::ranges::find_if(stateB.tasks, [&](const auto& t) { return t.id == taskOnB; });
    REQUIRE(found != stateB.tasks.end());
}

// Ledger triage item #14: the swimlane-belongs-to-project check
// (MoveTaskPosition's inline check next to requireColumnBelongsToProject)
// had no dedicated unit test -- only ever exercised implicitly by every
// other test supplying a real swimlane. Same shape as "MoveTaskPosition
// into a column deleted mid-drag throws NotFound" above, but for the
// swimlane half of the destination.
TEST_CASE("MoveTaskPosition into a swimlane deleted mid-drag throws NotFound, not a silent orphan write",
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
    // A swimlane id that was never created -- stands in for "deleted
    // between GetBoard and MoveTaskPosition" (this rung has no
    // DeleteSwimlane action yet, same rationale as the column-deleted test
    // above).
    const kanban::SwimlaneId neverExisted{99999};

    CHECK_THROWS_AS(model.execute(kanban::MoveTaskPosition{.taskId = taskId,
                                                            .columnId = col1,
                                                            .swimlaneId = neverExisted,
                                                            .position = 0,
                                                            .opId = ""}),
                    kanban::NotFound);
}
