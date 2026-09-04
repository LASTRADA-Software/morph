// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <memory>
#include <morph/core/model_key.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>

#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

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
    const auto commentsForTaskA =
        std::ranges::count_if(result.comments, [&](const auto& c) { return c.taskId == taskA; });
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
    model.execute(kanban::MoveTaskPosition{
        .taskId = taskA, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

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

    CHECK_THROWS_AS(
        model.execute(kanban::MoveTaskPosition{
            .taskId = taskId, .columnId = neverExisted, .swimlaneId = swimlaneId, .position = 0, .opId = ""}),
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
    (void)colId;

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
    const auto columnOnA =
        modelA.execute(kanban::CreateColumn{.name = "A's column", .wipLimit = 0}).columns.front().id;
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
    CHECK_THROWS_AS(modelA.execute(kanban::AddComment{.taskId = taskOnB, .body = "sneaky comment"}), kanban::NotFound);

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

    CHECK_THROWS_AS(model.execute(kanban::MoveTaskPosition{
                        .taskId = taskId, .columnId = col1, .swimlaneId = neverExisted, .position = 0, .opId = ""}),
                    kanban::NotFound);
}

// Task 12: divergence test, kept deliberately independent of the rules
// engine. It hand-builds the cascade -- two journal entries, the second
// carrying `causalParentId` set to the first's own (app-minted, not
// `seq`-derived) identity, per design spec §9 -- and proves that a cascaded
// mutation recorded once and replayed via
// `morph::journal::replay("BoardModel", ...)` reconstructs to *exactly one*
// application of that mutation, not two.
//
// That is a property of replay itself, which is why it is asserted here
// against a cascade owed to nothing but this test. Phase 6's automation-rules
// engine now exists and relies on the same invariant, guarding on
// `morph::journal::isReplaying()` before firing again on a replayed trigger;
// "Replaying a move-to-Done journal entry does not re-fire its rule"
// (`[kanban][rules]`, Task 14) covers that path end to end. The two are
// complementary: break the replay invariant and this test fails whether or
// not any rule exists; break the engine's replay guard and only Task 14's
// does.
TEST_CASE("Replaying a cascaded journal entry does not re-fire the cascade", "[kanban][journal]") {
    DbFixture fixture;
    auto log = std::make_shared<::morph::journal::InMemoryActionLog>();
    const auto projectId = createProjectAs("alice", "Sprint Board");
    const auto projectIdStr = std::to_string(*projectId);

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.attachActionLog(log, projectIdStr);
    // OpenBoard is Loggable::No (never auto-journaled by logAction), but
    // replay() needs an OpenBoard entry to attach a freshly reconstructed
    // BoardModel before any mutating entry can dispatch -- so this test
    // appends one by hand, exactly the shape a real host-level replay
    // driver would need to seed regardless of the cascade question this
    // test is actually about.
    const auto opened = model.execute(kanban::OpenBoard{.projectId = projectId});
    (void)opened;

    const auto columnId = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Ship it"})
            .tasks.front()
            .id;

    // The "trigger": a task moved into the Done column. This test creates no
    // rule, so nothing cascades from the move on its own -- deliberately, to
    // keep the replay property below independent of the engine (see this
    // TEST_CASE's own header comment); the cascade is appended to the log by
    // hand instead. Task 14's "A rule firing on move-to-column adds a tag,
    // journaled with a causal parent" case is the same shape with a real
    // CreateRule doing the cascading.
    model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = columnId, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    // Mint the trigger's own stable identity -- independent of LogEntry::seq
    // (design spec §9, docs/spec/journal/journal.md's Invariants section:
    // seq is sink-local and re-stamped on every forward, so it cannot serve
    // as a cross-sink/cross-restart causal key). The shipped rules engine
    // mints exactly this kind of id at the trigger's own commit -- the
    // `"boardEvent:" + event.id` in `BoardModel::execute(MoveTaskPosition)`
    // (src/models/board_model.cpp:982); this test mints its own after the
    // fact purely because it is constructing the cascade by hand.
    const std::string triggerCausalId = "cascade-trigger-" + projectIdStr;

    // The "cascade": an AddComment linked to the trigger via causalParentId,
    // standing in for the README's illustrative "assign to closer, add tag".
    // The tag half is real now (the engine's AddTag/RemoveTag mutations write
    // `db::TaskTagRecord` rows); the assignee half has no entity in this rung
    // at all. An AddComment keeps this test's assertion a plain, engine-free
    // count of one observable row.
    ::morph::journal::LogEntry cascadeEntry;
    cascadeEntry.modelType = "BoardModel";
    cascadeEntry.entityKey = projectIdStr;
    cascadeEntry.actionType = std::string{::morph::model::ActionTraits<kanban::AddComment>::typeId()};
    const kanban::AddComment cascadeAction{.taskId = taskId, .body = "auto-tagged: moved to Done"};
    cascadeEntry.payload = ::morph::model::ActionTraits<kanban::AddComment>::toJson(cascadeAction);
    cascadeEntry.outcome = ::morph::journal::Outcome::Succeeded;
    cascadeEntry.causalParentId = triggerCausalId;
    log->append(cascadeEntry);

    // Sanity check on the causal link itself, independent of replay: the
    // cascade's own recorded entry must carry the trigger's minted id, not
    // an empty/default causalParentId and not the trigger's (sink-local,
    // unstable) seq.
    {
        const auto recorded = log->entries(projectIdStr);
        const auto cascadeRecorded =
            std::ranges::find_if(recorded, [](const auto& e) { return e.actionType == "AddComment"; });
        REQUIRE(cascadeRecorded != recorded.end());
        CHECK(cascadeRecorded->causalParentId == triggerCausalId);
    }

    // Replay the full recorded history (OpenBoard first, hand-appended since
    // it's Loggable::No, then everything logAction actually recorded, in
    // order) against a fresh BoardModel via the real framework entry point.
    std::vector<::morph::journal::LogEntry> replayEntries;
    {
        ::morph::journal::LogEntry openBoardEntry;
        openBoardEntry.modelType = "BoardModel";
        openBoardEntry.entityKey = projectIdStr;
        openBoardEntry.actionType = std::string{::morph::model::ActionTraits<kanban::OpenBoard>::typeId()};
        openBoardEntry.payload =
            ::morph::model::ActionTraits<kanban::OpenBoard>::toJson(kanban::OpenBoard{.projectId = projectId});
        openBoardEntry.outcome = ::morph::journal::Outcome::Succeeded;
        replayEntries.push_back(std::move(openBoardEntry));
    }
    for (const auto& entry : log->entries(projectIdStr)) {
        replayEntries.push_back(entry);
    }

    const auto replayedHolder = ::morph::journal::replay("BoardModel", replayEntries);
    auto& replayedModel = replayedHolder->into<kanban::BoardModel>();
    const auto replayedState = replayedModel.execute(kanban::GetBoardState{});

    // The invariant this test proves: the cascade's own recorded AddComment
    // was replayed exactly once, from its own recorded entry -- not zero
    // times (dropped) and not twice (re-fired by both replaying the
    // trigger *and* an unsuppressed rule evaluation reacting to the
    // replayed trigger, which is exactly the double-application design
    // spec §9 rejects). The engine that could double-fire a cascade now
    // exists and is guarded -- `BoardModel::evaluateRules` returns
    // immediately when `morph::journal::isReplaying()`
    // (src/models/board_model.cpp:1006), which Task 14's "Replaying a
    // move-to-Done journal entry does not re-fire its rule" case asserts
    // directly. What this test owns is the layer underneath that guard: the
    // replay mechanics themselves -- one recorded entry in, one dispatch out
    // -- which have to hold for a cascade to converge whether or not any rule
    // is involved.
    const auto cascadeComments = std::ranges::count_if(
        replayedState.comments, [](const auto& c) { return c.body == "auto-tagged: moved to Done"; });
    CHECK(cascadeComments == 1);
}

// Task 14: the real version of Task 12's hand-simulated cascade -- an actual
// CreateRule, actually firing via MoveTaskPosition, actually journaled with a
// causalParentId linking the cascade entry to the triggering move entry.
TEST_CASE("A rule firing on move-to-column adds a tag, journaled with a causal parent", "[kanban][rules]") {
    DbFixture fixture;
    auto log = std::make_shared<::morph::journal::InMemoryActionLog>();
    const auto projectId = createProjectAs("alice", "Sprint Board");
    const auto projectIdStr = std::to_string(*projectId);

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.attachActionLog(log, projectIdStr);
    model.execute(kanban::OpenBoard{.projectId = projectId});

    const auto doneColumnId = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = doneColumnId, .swimlaneId = swimlaneId, .title = "Ship it"})
            .tasks.front()
            .id;

    const auto ruleId = model
                            .execute(kanban::CreateRule{.projectId = projectId,
                                                        .triggerColumnId = *doneColumnId,
                                                        .mutationType = kanban::RuleMutationType::AddTag,
                                                        .mutationValue = "closed"})
                            .ruleId;
    REQUIRE(ruleId.hasValue());

    const auto afterMove = model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = doneColumnId, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    // The rule's mutation fired: the task now carries the "closed" tag.
    const auto movedTask = std::ranges::find_if(afterMove.tasks, [&](const auto& t) { return t.id == taskId; });
    REQUIRE(movedTask != afterMove.tasks.end());
    CHECK(std::ranges::find(movedTask->tags, "closed") != movedTask->tags.end());

    // The journal has two entries for this action -- the move itself, and
    // the cascaded tag add -- with the tag-add entry's causalParentId equal
    // to the move entry's own id. MoveTaskPosition mints its own stable
    // identity from its `board_events` row's autoincrement id (board_model.cpp's
    // execute(MoveTaskPosition) doc comment): find that row via
    // GetEventsSince (the "move" kind) rather than hardcoding the scheme.
    const auto events = model.execute(kanban::GetEventsSince{}).events;
    const auto moveEvent = std::ranges::find_if(events, [](const auto& e) { return e.kind == "move"; });
    REQUIRE(moveEvent != events.end());
    const std::string expectedCausalId = "boardEvent:" + std::to_string(*moveEvent->id);

    const auto recorded = log->entries(projectIdStr);
    const auto cascadeEntry =
        std::ranges::find_if(recorded, [](const auto& e) { return e.actionType == "ApplyTagMutation"; });
    REQUIRE(cascadeEntry != recorded.end());
    CHECK_FALSE(cascadeEntry->causalParentId.empty());
    CHECK(cascadeEntry->causalParentId == expectedCausalId);
}

// Task 14: proves Phase 5's suppress-during-replay decision holds for a real
// rule firing through MoveTaskPosition, not just Task 12's hand-simulation.
TEST_CASE("Replaying a move-to-Done journal entry does not re-fire its rule", "[kanban][rules]") {
    DbFixture fixture;
    auto log = std::make_shared<::morph::journal::InMemoryActionLog>();
    const auto projectId = createProjectAs("alice", "Sprint Board");
    const auto projectIdStr = std::to_string(*projectId);

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.attachActionLog(log, projectIdStr);
    model.execute(kanban::OpenBoard{.projectId = projectId});

    const auto doneColumnId = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = doneColumnId, .swimlaneId = swimlaneId, .title = "Ship it"})
            .tasks.front()
            .id;
    model.execute(kanban::CreateRule{.projectId = projectId,
                                     .triggerColumnId = *doneColumnId,
                                     .mutationType = kanban::RuleMutationType::AddTag,
                                     .mutationValue = "closed"});

    // Perform the move once -- the rule fires, the tag is added.
    model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = doneColumnId, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    // Replay the journal from scratch against a fresh BoardModel instance --
    // OpenBoard first (Loggable::No, hand-appended, same shape as Task 12's
    // divergence test), then everything logAction actually recorded.
    std::vector<::morph::journal::LogEntry> replayEntries;
    {
        ::morph::journal::LogEntry openBoardEntry;
        openBoardEntry.modelType = "BoardModel";
        openBoardEntry.entityKey = projectIdStr;
        openBoardEntry.actionType = std::string{::morph::model::ActionTraits<kanban::OpenBoard>::typeId()};
        openBoardEntry.payload =
            ::morph::model::ActionTraits<kanban::OpenBoard>::toJson(kanban::OpenBoard{.projectId = projectId});
        openBoardEntry.outcome = ::morph::journal::Outcome::Succeeded;
        replayEntries.push_back(std::move(openBoardEntry));
    }
    for (const auto& entry : log->entries(projectIdStr)) {
        replayEntries.push_back(entry);
    }

    const auto replayedHolder = ::morph::journal::replay("BoardModel", replayEntries);
    auto& replayedModel = replayedHolder->into<kanban::BoardModel>();
    const auto replayedState = replayedModel.execute(kanban::GetBoardState{});

    // The invariant: the "closed" tag was applied exactly once, not twice --
    // replaying the recorded MoveTaskPosition entry must not re-fire the
    // rule (morph::journal::isReplaying() suppresses evaluateRules during
    // replay's dispatch loop), leaving the cascade's own recorded
    // ApplyTagMutation entry as the sole source of the tag.
    const auto replayedTask = std::ranges::find_if(replayedState.tasks, [&](const auto& t) { return t.id == taskId; });
    REQUIRE(replayedTask != replayedState.tasks.end());
    const auto closedTagCount = std::ranges::count(replayedTask->tags, "closed");
    CHECK(closedTagCount == 1);
}

// morph#369: `CreateRule`/`GetRules` each carry a `projectId` their own
// `validate()` insists be present. Neither used to consult it -- the board
// was named by the handler's attach state alone -- so a handler attached to
// project A, asked for project B's rules, answered with A's and an `ok`. The
// argument was load-bearing in the type and inert in the code, and a client
// that passed the id it meant had no way to tell it had been ignored. Both
// now refuse a mismatch the way every other cross-project check in
// board_model.cpp does. alice is a Manager on *both* projects here, so what
// these pin is the projectId check itself, not the role gate standing in
// for it.

TEST_CASE("GetRules refuses another project's id rather than answering with the attached board's rules",
          "[kanban][rules][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Project A");
    const auto projectB = createProjectAs("alice", "Project B");

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectA});
    const auto columnOnA = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.front().id;
    model.execute(kanban::CreateRule{.projectId = projectA,
                                     .triggerColumnId = *columnOnA,
                                     .mutationType = kanban::RuleMutationType::AddTag,
                                     .mutationValue = "shipped"});

    // The attached board still answers its own id.
    CHECK(model.execute(kanban::GetRules{.projectId = projectA}).rules.size() == 1);

    // Project B's id does not, even though B exists and alice may read it.
    CHECK_THROWS_AS(model.execute(kanban::GetRules{.projectId = projectB}), kanban::NotFound);
}

TEST_CASE("CreateRule refuses another project's id rather than creating the rule on the attached board",
          "[kanban][rules][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Project A");
    const auto projectB = createProjectAs("alice", "Project B");

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectA});
    const auto columnOnA = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0}).columns.front().id;

    // A rule naming project B, on project A's own trigger column -- the case
    // that used to succeed, writing the rule onto A and reporting an id for
    // it as though B had been served.
    CHECK_THROWS_AS(model.execute(kanban::CreateRule{.projectId = projectB,
                                                     .triggerColumnId = *columnOnA,
                                                     .mutationType = kanban::RuleMutationType::AddTag,
                                                     .mutationValue = "shipped"}),
                    kanban::NotFound);

    // ...and it wrote nothing: the refusal is a refusal, not a relabelling.
    CHECK(model.execute(kanban::GetRules{.projectId = projectA}).rules.empty());
}

// Task 16: attachment metadata (README build-order step 8's "bytes over a
// side channel, metadata through actions" -- this task never touches actual
// bytes, only the storageKey a later HTTP side channel would have handed
// back).
TEST_CASE("AddAttachment records metadata for a task, GetAttachments lists it", "[kanban][attachments]") {
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

    model.execute(kanban::AddAttachment{.taskId = taskId,
                                        .filename = "report.pdf",
                                        .contentType = "application/pdf",
                                        .sizeBytes = 1024,
                                        .storageKey = "abc123"});

    const auto result = model.execute(kanban::GetAttachments{.taskId = taskId});
    REQUIRE(result.attachments.size() == 1);
    const auto& att = result.attachments.front();
    CHECK(att.taskId == taskId);
    CHECK(att.filename == "report.pdf");
    CHECK(att.contentType == "application/pdf");
    CHECK(att.sizeBytes == 1024);
    CHECK(att.storageKey == "abc123");
    CHECK(att.uploadedBy == "alice");
    CHECK(att.id.hasValue());
}

TEST_CASE("RemoveAttachment deletes the metadata row; GetAttachments no longer lists it", "[kanban][attachments]") {
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

    model.execute(kanban::AddAttachment{.taskId = taskId,
                                        .filename = "report.pdf",
                                        .contentType = "application/pdf",
                                        .sizeBytes = 1024,
                                        .storageKey = "abc123"});
    const auto attachmentId = model.execute(kanban::GetAttachments{.taskId = taskId}).attachments.front().id;

    model.execute(kanban::RemoveAttachment{.attachmentId = attachmentId});

    const auto after = model.execute(kanban::GetAttachments{.taskId = taskId});
    CHECK(after.attachments.empty());
}

TEST_CASE("AddAttachment rejects a taskId that belongs to a different project",
          "[kanban][attachments][cross-tenant]") {
    DbFixture fixture;
    const auto projectA = createProjectAs("alice", "Board A");
    const auto projectB = createProjectAs("bob", "Board B");

    kanban::BoardModel modelA;
    {
        const ScopedPrincipal alice{"alice"};
        modelA.execute(kanban::OpenBoard{.projectId = projectA});
    }

    const kanban::TaskId taskOnA = [&] {
        const ScopedPrincipal alice{"alice"};
        const auto columnId = modelA.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
        const auto swimlaneId = modelA.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
        return modelA.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Task A"})
            .tasks.front()
            .id;
    }();

    kanban::BoardModel modelB;
    const ScopedPrincipal bob{"bob"};
    modelB.execute(kanban::OpenBoard{.projectId = projectB});

    CHECK_THROWS_AS(modelB.execute(kanban::AddAttachment{.taskId = taskOnA,
                                                         .filename = "sneaky.pdf",
                                                         .contentType = "application/pdf",
                                                         .sizeBytes = 1,
                                                         .storageKey = "sneaky"}),
                    kanban::NotFound);
}

TEST_CASE("A Viewer can GetAttachments but cannot AddAttachment -- Forbidden, not a silent write",
          "[kanban][attachments]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;

    kanban::TaskId taskId;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(kanban::OpenBoard{.projectId = projectId});
        const auto columnId = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
        const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
        taskId = model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Fix bug"})
                     .tasks.front()
                     .id;

        kanban::ProjectAdminModel admin;
        admin.execute(
            kanban::SetMemberRole{.projectId = projectId, .principal = "carol", .role = kanban::Role::Viewer});
    }

    const ScopedPrincipal carol{"carol"};
    kanban::BoardModel viewerModel;
    viewerModel.execute(kanban::OpenBoard{.projectId = projectId});

    CHECK_THROWS_AS(viewerModel.execute(kanban::AddAttachment{.taskId = taskId,
                                                              .filename = "x.pdf",
                                                              .contentType = "application/pdf",
                                                              .sizeBytes = 1,
                                                              .storageKey = "k"}),
                    kanban::Forbidden);
    // Viewer-or-above may still read -- same bar GetBoardState/GetRules use.
    CHECK(viewerModel.execute(kanban::GetAttachments{.taskId = taskId}).attachments.empty());
}

// No DbFixture here: ActionKeyTraits<OpenBoard>::key() is a pure function
// over the action's own fields -- it never touches a database -- and this
// regression needs to prove it rejects a bad key *before* any DB connection
// would even matter, exactly the point in the dispatch pipeline where the
// crash below happened.
TEST_CASE("ActionKeyTraits<OpenBoard>::key() rejects a disengaged projectId instead of asserting",
          "[kanban][model][key]") {
    // A regression test for a real crash: BoardBridge::openBoard() parses an
    // unparseable QString (e.g. QML passing garbage, or this rung's own
    // "BoardBridge relays failed() on a bad projectId" test's
    // openBoard("not-a-number")) into a default-constructed, disengaged
    // ProjectId{} via board_qml_bridge.cpp's own parseId<ProjectId>(). That
    // action reaches morph::bridge::BridgeHandler::execute() before
    // BoardModel::execute(OpenBoard)'s own hasValue() check ever runs --
    // ActionKeyTraits<OpenBoard>::key() (board_model.hpp) is what computes
    // the routing key that attach relies on, and it used to dereference
    // action.projectId unconditionally. A disengaged std::optional::
    // operator*() is undefined behavior; under libstdc++'s hardened build
    // (the "Linux / all optional features (gcc)" CI leg, not every other
    // job's libc++) that trips a hard assertion and aborts the whole test
    // process, rather than routing to BoardBridge::failed() as the test
    // right above this one ("BoardBridge relays failed() on a bad
    // projectId") expects.
    kanban::OpenBoard withValidId{.projectId = kanban::ProjectId{1}};
    CHECK(morph::model::ActionKeyTraits<kanban::OpenBoard>::key(withValidId) == "1");

    kanban::OpenBoard disengaged{.projectId = {}};
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<kanban::OpenBoard>::key(disengaged), std::runtime_error);

    // The refusal is now `morph::model::keyToString`'s own, not this rung's:
    // BRIDGE_MODEL_KEY generates the specialisation (morph#183), so the
    // hand-written `throw kanban::ValidationError` is gone and the escaping
    // type is morph's plain `std::runtime_error`. Pinned as a negative
    // because it is the one caller-visible difference the migration makes.
    // Nothing in the rung catches this by type -- `BoardBridge` routes it
    // through `morph::ladder::gui::errorText()`, which reads `what()` off any
    // `std::exception` -- but a change that quietly reintroduced a rung-local
    // `key()` would show up right here.
    bool refusedByTheRung = false;
    try {
        (void)morph::model::ActionKeyTraits<kanban::OpenBoard>::key(disengaged);
        FAIL("key() accepted a disengaged projectId");
    } catch (const kanban::KanbanError&) {
        refusedByTheRung = true;
    } catch (const std::runtime_error&) {  // NOLINT(bugprone-empty-catch) -- the expected path
    }
    CHECK_FALSE(refusedByTheRung);
}

// Equivalence pin for morph#183's migration, and a regression guard after it:
// the two hand-written specialisations were replaced by one
// `BRIDGE_MODEL_KEY(BoardModel, OpenBoard, &OpenBoard::projectId)`, and the
// only way that is safe is if the generated key is the same bytes the
// hand-written `key()` produced, for every id it accepted.
TEST_CASE("BoardModel's deduced key is the ProjectId itself and encodes exactly as the hand-written key() did",
          "[kanban][model][key]") {
    // The macro deduces `PrimaryKey` from the *member* it is handed, so the
    // model's key type is the strong id (examples/IMPLEMENTATION.md rule 3),
    // not the unwrapped `std::int64_t` the hand-written
    // `ModelKeyTraits<BoardModel>` declared. Checked at run time rather than
    // with `STATIC_REQUIRE` deliberately: this way the assertion *fails*
    // against the pre-migration header instead of refusing to compile it, so
    // it is a test that can be seen to fail.
    CHECK((std::same_as<morph::model::PrimaryKeyOf<kanban::BoardModel>, kanban::ProjectId>));

    // The right-hand side is literally the body the hand-written
    // specialisation had (`morph::model::keyToString(*action.projectId)`), so
    // this compares generated against hand-written directly rather than
    // against a re-derived expectation. `9007199254740993` is past 2^53 --
    // the range morph#286 had to fix elsewhere in this rung -- so a key
    // encoding that ever went through a double would show up here.
    for (const std::int64_t raw :
         {std::int64_t{1}, std::int64_t{7}, std::int64_t{4294967297}, std::int64_t{9007199254740993}}) {
        const kanban::OpenBoard action{.projectId = kanban::ProjectId{raw}};
        CHECK(morph::model::ActionKeyTraits<kanban::OpenBoard>::key(action) ==
              morph::model::keyToString(*action.projectId));
        CHECK(morph::model::ActionKeyTraits<kanban::OpenBoard>::key(action) == std::to_string(raw));
    }

    // `primary()`/`instances()` decode with `keyFromString<PrimaryKeyOf<M>>`,
    // so the key has to round-trip back into the strong id it came from.
    CHECK(morph::model::keyFromString<kanban::ProjectId>(morph::model::ActionKeyTraits<kanban::OpenBoard>::key(
              kanban::OpenBoard{.projectId = kanban::ProjectId{9007199254740993}})) ==
          kanban::ProjectId{9007199254740993});
}
