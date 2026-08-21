// SPDX-License-Identifier: Apache-2.0
//
// BoardPresenter's own suite: each action round-trips through the
// presenter's own signals — not the model directly — mirroring
// test_project_admin_presenter.cpp's shape exactly (see that file's own top
// comment for the rationale reused verbatim here: domain rules already have
// a dedicated suite at the model level, test_board_model.cpp; this file only
// proves the presenter wires each action to the right signal and neither
// crashes nor hangs).

#include "board_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <kanban/models/project_admin_model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <morph/session/session.hpp>

#include <memory>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal. Same recipe as
///        test_project_admin_presenter.cpp's own `makeAuthedRig`.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the presenter takes.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Seeds one project (alice is its Manager) directly through
///        `ProjectAdminModel`'s own `BridgeHandler`, bypassing
///        `ProjectAdminPresenter` entirely — this suite is about
///        `BoardPresenter`, not project bootstrap.
/// @param rig The rig whose bridge/executor to dispatch the seed through.
/// @return The new project's id.
[[nodiscard]] kanban::ProjectId seedProject(BackendRig& rig) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    return morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Sprint Board"})).id;
}

}  // namespace

TEST_CASE("BoardPresenter::openBoard attaches and reports the board's empty initial state",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::GetBoardResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::boardOpened,
                      [&](kanban::GetBoardResult result) {
                          opened = std::move(result);
                          gotOpened = true;
                      });
    presenter.openBoard(projectId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(opened.name == "Sprint Board");
    CHECK(opened.columns.empty());
    CHECK(opened.tasks.empty());
}

TEST_CASE("BoardPresenter::createColumn/createSwimlane/createTask populate the reported board state",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::GetBoardResult state;
    bool gotState = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::boardOpened, [&](kanban::GetBoardResult result) {
        state = std::move(result);
        gotState = true;
    });

    presenter.openBoard(projectId);
    REQUIRE(pumpUntil([&] { return gotState; }));

    gotState = false;
    presenter.createColumn("To Do", 0);
    REQUIRE(pumpUntil([&] { return gotState; }));
    REQUIRE(state.columns.size() == 1);
    const auto columnId = state.columns.front().id;

    gotState = false;
    presenter.createSwimlane("Default");
    REQUIRE(pumpUntil([&] { return gotState; }));
    REQUIRE(state.swimlanes.size() == 1);
    const auto swimlaneId = state.swimlanes.front().id;

    gotState = false;
    presenter.createTask(columnId, swimlaneId, "Fix bug");
    REQUIRE(pumpUntil([&] { return gotState; }));
    REQUIRE(state.tasks.size() == 1);
    CHECK(state.tasks.front().title == "Fix bug");
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("BoardPresenter opens a board, creates a column/swimlane/task, and reports a moved task",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::GetBoardResult state;
    bool gotState = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::boardOpened, [&](kanban::GetBoardResult result) {
        state = std::move(result);
        gotState = true;
    });
    bool failed = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::failed, [&](QString) { failed = true; });

    presenter.openBoard(projectId);
    REQUIRE(pumpUntil([&] { return gotState || failed; }));
    REQUIRE_FALSE(failed);

    gotState = false;
    presenter.createColumn("To Do", 0);
    REQUIRE(pumpUntil([&] { return gotState; }));
    const auto col1 = state.columns.front().id;

    gotState = false;
    presenter.createColumn("Done", 0);
    REQUIRE(pumpUntil([&] { return gotState; }));
    REQUIRE(state.columns.size() == 2);
    const auto col2 = state.columns.back().id;

    gotState = false;
    presenter.createSwimlane("Default");
    REQUIRE(pumpUntil([&] { return gotState; }));
    const auto swimlaneId = state.swimlanes.front().id;

    gotState = false;
    presenter.createTask(col1, swimlaneId, "Fix bug");
    REQUIRE(pumpUntil([&] { return gotState; }));
    const auto taskId = state.tasks.front().id;

    QString movedTaskId;
    bool moved = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::taskMoved,
                      [&](QString id) {
                          movedTaskId = std::move(id);
                          moved = true;
                      });
    presenter.moveTask(taskId, col2, swimlaneId, 0, QStringLiteral("op-1"));
    REQUIRE(pumpUntil([&] { return moved || failed; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(moved);
    CHECK(movedTaskId == QString::number(*taskId));
}

TEST_CASE("BoardPresenter::addComment reports the commented task and getActivity/getEventsSince round-trip",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::GetBoardResult state;
    bool gotState = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::boardOpened, [&](kanban::GetBoardResult result) {
        state = std::move(result);
        gotState = true;
    });
    presenter.openBoard(projectId);
    REQUIRE(pumpUntil([&] { return gotState; }));

    gotState = false;
    presenter.createColumn("To Do", 0);
    REQUIRE(pumpUntil([&] { return gotState; }));
    const auto columnId = state.columns.front().id;

    gotState = false;
    presenter.createSwimlane("Default");
    REQUIRE(pumpUntil([&] { return gotState; }));
    const auto swimlaneId = state.swimlanes.front().id;

    gotState = false;
    presenter.createTask(columnId, swimlaneId, "Fix bug");
    REQUIRE(pumpUntil([&] { return gotState; }));
    const auto taskId = state.tasks.front().id;

    QString commentedTaskId;
    bool commented = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::commentAdded,
                      [&](QString id) {
                          commentedTaskId = std::move(id);
                          commented = true;
                      });
    presenter.addComment(taskId, "looking into it");
    REQUIRE(pumpUntil([&] { return commented; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(commentedTaskId == QString::number(*taskId));

    // getEventsSince: no board_events row has been created by any action
    // above (design spec §1 -- Task 10 wires GetEventsSince's own producer
    // separately), so the only thing pinned here is that the round trip
    // itself succeeds and reports through eventsReceived, not failed.
    kanban::GetEventsSinceResult events;
    bool gotEvents = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::eventsReceived,
                      [&](kanban::GetEventsSinceResult result) {
                          events = std::move(result);
                          gotEvents = true;
                      });
    presenter.getEventsSince(kanban::BoardEventId{});
    REQUIRE(pumpUntil([&] { return gotEvents; }));
    REQUIRE_FALSE(presenter.busy());

    // getActivity: no action log is attached to this directly-constructed
    // BoardModel instance (BoardModel::attachActionLog is a holder-level
    // concern -- see board_model.hpp's own doc comment), so an empty result
    // is the correct, non-error outcome here.
    kanban::GetActivityResult activity;
    bool gotActivity = false;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::activityUpdated,
                      [&](kanban::GetActivityResult result) {
                          activity = std::move(result);
                          gotActivity = true;
                      });
    presenter.getActivity();
    REQUIRE(pumpUntil([&] { return gotActivity; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(activity.events.empty());
}

TEST_CASE("BoardPresenter routes every action's failure to failed(), not just one", "[kanban][gui][presenter]") {
    // Same rationale as ProjectAdminPresenter's identical completeness test:
    // each action's error reporting is wired independently at its own
    // track() call site, so a passing case for one action says nothing
    // about another's wiring.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};  // no session installed at all -- every dispatch is Forbidden
    kanban::gui::BoardPresenter presenter{rig.bridge(0), rig.executor()};

    int failures = 0;
    QString failure;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    presenter.openBoard(kanban::ProjectId{1});
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.getBoardState();
    REQUIRE(pumpUntil([&] { return failures == 2; }));

    presenter.createColumn("To Do", 0);
    REQUIRE(pumpUntil([&] { return failures == 3; }));

    presenter.createSwimlane("Default");
    REQUIRE(pumpUntil([&] { return failures == 4; }));

    presenter.createTask(kanban::ColumnId{1}, kanban::SwimlaneId{1}, "Fix bug");
    REQUIRE(pumpUntil([&] { return failures == 5; }));

    presenter.moveTask(kanban::TaskId{1}, kanban::ColumnId{1}, kanban::SwimlaneId{1}, 0, QStringLiteral("op"));
    REQUIRE(pumpUntil([&] { return failures == 6; }));

    presenter.addComment(kanban::TaskId{1}, "looking into it");
    REQUIRE(pumpUntil([&] { return failures == 7; }));

    presenter.getEventsSince(kanban::BoardEventId{});
    REQUIRE(pumpUntil([&] { return failures == 8; }));

    presenter.getActivity();
    REQUIRE(pumpUntil([&] { return failures == 9; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK_FALSE(failure.isEmpty());
}
