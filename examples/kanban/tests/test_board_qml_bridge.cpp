// SPDX-License-Identifier: Apache-2.0
//
// The QML-adapter layer's own suite: `BoardBridge`
// (`gui_lib/board_qml_bridge.hpp`) — everything that stands between
// `BoardPresenter` and the QML board view. Mirrors
// test_project_admin_qml_bridge.cpp's shape and rationale: this is the only
// place a DTO becomes a `QVariantMap`/`QVariantList` property bag, and QML
// binds by *string*, so every assertion below pins a real string a future
// `BoardView.qml` will bind against.

#include "board_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <kanban/models/board_model.hpp>
#include <kanban/models/project_admin_model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <morph/session/session.hpp>

#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal. Same recipe as
///        test_project_admin_qml_bridge.cpp's own `makeAuthedRig`.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the adapter takes.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Seeds one project (alice is its Manager) directly through
///        `ProjectAdminModel`'s own `BridgeHandler`.
/// @param rig The rig whose bridge/executor to dispatch the seed through.
/// @return The new project's id, as its plain number.
[[nodiscard]] qlonglong seedProject(BackendRig& rig) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    const auto id =
        morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Sprint Board"})).id;
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief Parses a `BoardBridge::board()` row's plain-number-string id back
///        into a strong id — the test-side mirror of
///        `board_qml_bridge.cpp`'s own (anonymous-namespace, so not
///        reachable from here) `parseId`, needed here only to build a raw
///        `MoveTaskPosition` DTO for the "second client" this suite's own
///        poller test dispatches directly through a `BridgeHandler`, not
///        through a second `BoardBridge`.
/// @tparam IdT One of `ColumnId`/`SwimlaneId`/`TaskId`.
/// @param text The id, as a `board()` property row carries it.
/// @return The parsed id.
template <typename IdT>
[[nodiscard]] IdT parseId(const QString& text) {
    return IdT{static_cast<std::int64_t>(text.toLongLong())};
}

}  // namespace

TEST_CASE("BoardBridge exposes the expected surface", "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    const QMetaObject* meta = bridge.metaObject();

    REQUIRE(meta->indexOfProperty("board") >= 0);
    REQUIRE(meta->indexOfProperty("activity") >= 0);
    REQUIRE(meta->indexOfProperty("myRole") >= 0);
    CHECK(meta->propertyCount() - meta->propertyOffset() == 3);

    REQUIRE(meta->indexOfMethod("openBoard(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("refresh()") >= 0);
    REQUIRE(meta->indexOfMethod("createColumn(QString,int)") >= 0);
    REQUIRE(meta->indexOfMethod("createSwimlane(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("createTask(QString,QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("moveTask(QString,QString,QString,int)") >= 0);
    REQUIRE(meta->indexOfMethod("addComment(QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("setMyRole(QString)") >= 0);

    REQUIRE(meta->indexOfSignal("bound()") >= 0);
    REQUIRE(meta->indexOfSignal("boardChanged()") >= 0);
    REQUIRE(meta->indexOfSignal("activityChanged()") >= 0);
    REQUIRE(meta->indexOfSignal("myRoleChanged()") >= 0);
    REQUIRE(meta->indexOfSignal("taskMoved(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("commentAdded(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("failed(QString)") >= 0);
}

TEST_CASE("BoardBridge::openBoard then createColumn/createSwimlane/createTask updates the board property",
          "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });

    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));
    CHECK(bridge.board().value(QStringLiteral("name")).toString() == QStringLiteral("Sprint Board"));
    CHECK(bridge.board().value(QStringLiteral("columns")).toList().isEmpty());

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    REQUIRE(bridge.board().value(QStringLiteral("columns")).toList().size() == 1);
    const QVariantMap columnRow = bridge.board().value(QStringLiteral("columns")).toList().front().toMap();
    for (const char* key : {"id", "name", "wipLimit", "taskCount"}) {
        INFO("missing key: " << key);
        REQUIRE(columnRow.contains(QString::fromLatin1(key)));
    }
    const QString columnId = columnRow.value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    REQUIRE(bridge.board().value(QStringLiteral("swimlanes")).toList().size() == 1);
    const QVariantMap swimlaneRow = bridge.board().value(QStringLiteral("swimlanes")).toList().front().toMap();
    const QString swimlaneId = swimlaneRow.value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createTask(columnId, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    REQUIRE(bridge.board().value(QStringLiteral("tasks")).toList().size() == 1);
    const QVariantMap taskRow = bridge.board().value(QStringLiteral("tasks")).toList().front().toMap();
    for (const char* key : {"id", "columnId", "swimlaneId", "title", "position"}) {
        INFO("missing key: " << key);
        REQUIRE(taskRow.contains(QString::fromLatin1(key)));
    }
    CHECK(taskRow.value(QStringLiteral("title")).toString() == QStringLiteral("Fix bug"));
}

TEST_CASE("BoardBridge exposes the expected surface and moveTask generates a fresh opId per call",
          "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col1 = bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 = bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId =
        bridge.board().value(QStringLiteral("swimlanes")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createTask(col1, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    bool moved = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::taskMoved, [&](const QString&) { moved = true; });

    bridge.moveTask(taskId, col2, swimlaneId, 0);
    REQUIRE(pumpUntil([&] { return moved; }));
    const QString firstOpId = bridge.lastOpIdForTest();
    REQUIRE_FALSE(firstOpId.isEmpty());

    moved = false;
    bridge.moveTask(taskId, col1, swimlaneId, 0);
    REQUIRE(pumpUntil([&] { return moved; }));
    const QString secondOpId = bridge.lastOpIdForTest();
    REQUIRE_FALSE(secondOpId.isEmpty());

    // Two calls must not reuse the same opId (exactly-once semantics rely on
    // a fresh id per user-initiated move, not per session).
    CHECK(firstOpId != secondOpId);
}

TEST_CASE("BoardBridge::addComment reports the commented task", "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString columnId =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId =
        bridge.board().value(QStringLiteral("swimlanes")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createTask(columnId, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    QString commentedTaskId;
    bool commented = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::commentAdded, [&](const QString& id) {
        commentedTaskId = id;
        commented = true;
    });
    bridge.addComment(taskId, QStringLiteral("looking into it"));
    REQUIRE(pumpUntil([&] { return commented; }));
    CHECK(commentedTaskId == taskId);
}

TEST_CASE("BoardBridge::setMyRole updates the myRole property", "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    CHECK(bridge.myRole().isEmpty());

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::myRoleChanged, [&] { changed = true; });
    bridge.setMyRole(QStringLiteral("Manager"));
    CHECK(changed);
    CHECK(bridge.myRole() == QStringLiteral("Manager"));
}

TEST_CASE("BoardBridge relays failed() on a bad projectId", "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    QString message;
    bool failed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });
    bridge.openBoard(QStringLiteral("not-a-number"));
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(message.isEmpty());
}

// ═════════════════════════════════════════════════════════════════════════
// The live poller — EventPoller wired to a real BoardBridge
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BoardBridge's EventPoller applies another client's move and refreshes board/activity, end to end",
          "[kanban][gui][qml-bridge][event-poller]") {
    // Mirrors test_poll_qml_bridges.cpp's "PollBridge's EventPoller applies a
    // live event and refreshes state, end to end" case: this proves the
    // *real* production wiring (BoardBridge's Dispatch closure over
    // BoardPresenter::getEventsSinceForPolling, ticking on EventPoller's real
    // default 3s interval) rather than a manually-driven pollOnce(), which
    // BoardBridge does not expose (it owns the poller privately, matching a
    // real view). test_event_poller.cpp already covers the class's own
    // mechanics exhaustively with an artificial long interval + manual
    // ticks; this is the one place in this rung that proves the *wiring* to
    // a real screen's adapter actually ticks on its own and picks up a
    // change made by a second, independent client.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);

    // The bridge under test: opens the board, which (per BoardBridge's own
    // openBoard()/applyBoard() wiring) starts its EventPoller ticking.
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    // Seed a column/swimlane/task through the same bridge so there is a task
    // for a *second*, independent client to move.
    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col1 =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 =
        bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId =
        bridge.board().value(QStringLiteral("swimlanes")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createTask(col1, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    // A second, independent client: its own BridgeHandler<BoardModel,
    // AllowShared> attached to the same shared, keyed BoardModel instance —
    // Mode::Local's single rig means both this handler and the bridge under
    // test's own presenter share one server-side instance
    // (ModelKeyTraits<BoardModel>, board_model.hpp), exactly like
    // test_board_concurrent_drag.cpp's multi-bridge setup, except here only
    // one side is a BoardBridge; the "second client" is a bare handler,
    // matching the brief's fallback ("seed the mutation directly via a
    // second BoardModel::execute() call against the same shared
    // BackendRig") since no dedicated second-client seeding helper exists
    // anywhere in this rung's test files. MoveTaskPosition inserts its own
    // "move" board_events row directly (board_model.cpp), independent of any
    // action-log attachment, so this is a real event the poller's own
    // GetEventsSince tick will observe.
    morph::bridge::BridgeHandler<kanban::BoardModel, morph::bridge::AllowShared> otherClient{rig->bridge(0),
                                                                                              rig->executor()};
    morph::ladder::testkit::awaitQt(otherClient.execute(kanban::OpenBoard{.projectId = kanban::ProjectId{projectId}}));
    morph::ladder::testkit::awaitQt(otherClient.execute(kanban::MoveTaskPosition{
        .taskId = parseId<kanban::TaskId>(taskId),
        .columnId = parseId<kanban::ColumnId>(col2),
        .swimlaneId = parseId<kanban::SwimlaneId>(swimlaneId),
        .position = 0,
        .opId = "other-client-move"}));

    // The bridge under test never itself called moveTask() — its own
    // taskMoved never fires for this move. What must happen instead is its
    // EventPoller's next tick (kDefaultInterval == 3000ms) picking up the
    // "move" board_events row the other client's call just inserted, then
    // BoardBridge::onEventApplied() resyncing both board and activity.
    bool boardRefreshedAfterMove = false;
    bool activityRefreshed = false;
    const auto onBoardChanged =
        QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { boardRefreshedAfterMove = true; });
    const auto onActivityChanged = QObject::connect(&bridge, &kanban::gui::BoardBridge::activityChanged,
                                                     [&] { activityRefreshed = true; });

    // kDefaultInterval is 3000ms; a 6s budget comfortably covers one real
    // tick plus dispatch/round-trip overhead without hardcoding a tighter
    // margin that would make this test flaky on a loaded CI runner — same
    // budget test_poll_qml_bridges.cpp's identical case uses.
    REQUIRE(pumpUntil([&] { return boardRefreshedAfterMove && activityRefreshed; }, std::chrono::milliseconds{6000}));
    QObject::disconnect(onBoardChanged);
    QObject::disconnect(onActivityChanged);

    // The bridge under test's own board property now reflects the other
    // client's move: the task moved from col1 to col2, without this bridge
    // ever calling moveTask() itself.
    const QVariantList tasksAfter = bridge.board().value(QStringLiteral("tasks")).toList();
    REQUIRE(tasksAfter.size() == 1);
    const QVariantMap taskRowAfter = tasksAfter.front().toMap();
    CHECK(taskRowAfter.value(QStringLiteral("id")).toString() == taskId);
    CHECK(taskRowAfter.value(QStringLiteral("columnId")).toString() == col2);
    CHECK(taskRowAfter.value(QStringLiteral("position")).toLongLong() == 0);
}
