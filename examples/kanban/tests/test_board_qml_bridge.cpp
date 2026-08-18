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
