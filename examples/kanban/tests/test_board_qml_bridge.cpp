// SPDX-License-Identifier: Apache-2.0
//
// The QML-adapter layer's own suite: `BoardBridge`
// (`gui_lib/board_qml_bridge.hpp`) — everything that stands between
// `BoardPresenter` and the QML board view. Mirrors
// test_project_admin_qml_bridge.cpp's shape and rationale: this is the only
// place a DTO becomes a `QVariantMap`/`QVariantList` property bag, and QML
// binds by *string*, so every assertion below pins a real string a future
// `BoardView.qml` will bind against.

#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <kanban/http/attachment_server.hpp>
#include <kanban/models/board_model.hpp>
#include <kanban/models/project_admin_model.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "board_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

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
    const auto id = morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Sprint Board"})).id;
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

/// @brief The signing secret shared by this file's own `TokenIssuer`/
///        `TokenVerifier` pair -- same shape as test_attachment_server.cpp's
///        `kSecret`, duplicated locally rather than shared, following that
///        file's own precedent.
constexpr std::string_view kAttachmentTestSecret = "board-qml-bridge-attachment-test-secret-32b";

/// @brief Builds a rig whose one bridge already carries a valid, *signed*
///        session for @p principal -- unlike `makeAuthedRig` above (which
///        only sets `Context::principal`), this also mints and installs a
///        real bearer token via @p issuer, since `BoardBridge::
///        uploadAttachment()`/`downloadAttachment()` read
///        `Bridge::defaultSession().token` to set their own
///        `Authorization: Bearer` header, and the real `AttachmentServer`
///        this test stands up alongside the rig verifies it for real.
/// @param issuer    The token issuer to mint from.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the adapter takes.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRigWithToken(const morph::session::TokenIssuer& issuer,
                                                                 std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = principal, .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief A fresh, empty storage directory for one test's `AttachmentServer`,
///        removed at scope entry -- same recipe as
///        test_attachment_server.cpp's own `freshStorageDir`.
/// @param name Distinguishes this test's directory from every other test's.
/// @return The directory path (not yet created -- `AttachmentServer`'s own
///         constructor creates it).
[[nodiscard]] std::filesystem::path freshAttachmentStorageDir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("board_qml_bridge_attachments_" + name);
    std::filesystem::remove_all(path);
    return path;
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
    REQUIRE(meta->indexOfProperty("rules") >= 0);
    REQUIRE(meta->indexOfProperty("attachments") >= 0);
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    // Task 6: queueDepth/deadLetterCount only exist when the offline stack
    // (MORPH_BUILD_OFFLINE_SQLITE) is compiled in -- see board_qml_bridge.hpp's
    // own gating of these two Q_PROPERTYs.
    REQUIRE(meta->indexOfProperty("queueDepth") >= 0);
    REQUIRE(meta->indexOfProperty("deadLetterCount") >= 0);
    CHECK(meta->propertyCount() - meta->propertyOffset() == 7);
#else
    CHECK(meta->propertyCount() - meta->propertyOffset() == 5);
#endif

    REQUIRE(meta->indexOfMethod("openBoard(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("refresh()") >= 0);
    REQUIRE(meta->indexOfMethod("createColumn(QString,int)") >= 0);
    REQUIRE(meta->indexOfMethod("createSwimlane(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("createTask(QString,QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("moveTask(QString,QString,QString,int)") >= 0);
    REQUIRE(meta->indexOfMethod("addComment(QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("setMyRole(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("createRule(QString,QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("getRules()") >= 0);
    REQUIRE(meta->indexOfMethod("deleteRule(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("setAttachmentServerUrl(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("uploadAttachment(QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("getAttachments(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("downloadAttachment(QString,QString)") >= 0);

    REQUIRE(meta->indexOfSignal("bound()") >= 0);
    REQUIRE(meta->indexOfSignal("boardChanged()") >= 0);
    REQUIRE(meta->indexOfSignal("activityChanged()") >= 0);
    REQUIRE(meta->indexOfSignal("myRoleChanged()") >= 0);
    REQUIRE(meta->indexOfSignal("taskMoved(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("commentAdded(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("rulesListed(QVariantList)") >= 0);
    REQUIRE(meta->indexOfSignal("ruleCreated()") >= 0);
    REQUIRE(meta->indexOfSignal("ruleDeleted()") >= 0);
    REQUIRE(meta->indexOfSignal("attachmentsListed(QVariantList)") >= 0);
    REQUIRE(meta->indexOfSignal("attachmentUploaded(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("attachmentDownloaded(QString)") >= 0);
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
    const QString col1 = bridge.board()
                             .value(QStringLiteral("columns"))
                             .toList()
                             .front()
                             .toMap()
                             .value(QStringLiteral("id"))
                             .toString();

    changed = false;
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 =
        bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

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
    const QString columnId = bridge.board()
                                 .value(QStringLiteral("columns"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("id"))
                                 .toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

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

TEST_CASE("BoardBridge::createRule/getRules/deleteRule round-trip a rule, updating the rules property",
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
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString columnId = bridge.board()
                                 .value(QStringLiteral("columns"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("id"))
                                 .toString();

    bool ruleCreated = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::ruleCreated, [&] { ruleCreated = true; });
    bridge.createRule(columnId, QStringLiteral("AddTag"), QStringLiteral("closed"));
    REQUIRE(pumpUntil([&] { return ruleCreated; }));

    bool rulesListed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::rulesListed,
                     [&](const QVariantList&) { rulesListed = true; });
    bridge.getRules();
    REQUIRE(pumpUntil([&] { return rulesListed; }));
    REQUIRE(bridge.rules().size() == 1);

    const QVariantMap ruleRow = bridge.rules().front().toMap();
    for (const char* key : {"id", "triggerColumnId", "mutationType", "mutationValue"}) {
        INFO("missing key: " << key);
        REQUIRE(ruleRow.contains(QString::fromLatin1(key)));
    }
    CHECK(ruleRow.value(QStringLiteral("triggerColumnId")).toString() == columnId);
    CHECK(ruleRow.value(QStringLiteral("mutationType")).toString() == QStringLiteral("AddTag"));
    CHECK(ruleRow.value(QStringLiteral("mutationValue")).toString() == QStringLiteral("closed"));
    const QString ruleId = ruleRow.value(QStringLiteral("id")).toString();

    bool ruleDeleted = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::ruleDeleted, [&] { ruleDeleted = true; });
    bridge.deleteRule(ruleId);
    REQUIRE(pumpUntil([&] { return ruleDeleted; }));

    rulesListed = false;
    bridge.getRules();
    REQUIRE(pumpUntil([&] { return rulesListed; }));
    CHECK(bridge.rules().isEmpty());
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
    const QString col1 = bridge.board()
                             .value(QStringLiteral("columns"))
                             .toList()
                             .front()
                             .toMap()
                             .value(QStringLiteral("id"))
                             .toString();

    changed = false;
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 =
        bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

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
    morph::ladder::testkit::awaitQt(
        otherClient.execute(kanban::MoveTaskPosition{.taskId = parseId<kanban::TaskId>(taskId),
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
    const auto onActivityChanged =
        QObject::connect(&bridge, &kanban::gui::BoardBridge::activityChanged, [&] { activityRefreshed = true; });

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

// ═════════════════════════════════════════════════════════════════════════
// Task 18 — attachment upload/download
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BoardBridge uploads a file and records its metadata, then downloads it back",
          "[kanban][gui][attachments]") {
    DbFixture fixture;
    const morph::session::TokenIssuer issuer{std::string{kAttachmentTestSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kAttachmentTestSecret}, morph::session::hmacSha256};

    const auto storageDir = freshAttachmentStorageDir("upload_and_record");
    kanban::http::AttachmentServer server{verifier, kanban::http::AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    auto rig = makeAuthedRigWithToken(issuer, "alice");
    const auto projectId = seedProject(*rig);
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    bridge.setAttachmentServerUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString columnId = bridge.board()
                                 .value(QStringLiteral("columns"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("id"))
                                 .toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

    changed = false;
    bridge.createTask(columnId, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    // A real local file to upload -- QTemporaryDir cleans it up automatically.
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString localFilePath = tempDir.filePath(QStringLiteral("report.pdf"));
    {
        QFile localFile{localFilePath};
        REQUIRE(localFile.open(QIODevice::WriteOnly));
        localFile.write(QByteArrayLiteral("this is the attachment's own bytes"));
    }

    QString failureMessage;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::failed,
                     [&](const QString& message) { failureMessage = message; });

    bool uploaded = false;
    QString uploadedTaskId;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::attachmentUploaded, [&](const QString& id) {
        uploadedTaskId = id;
        uploaded = true;
    });
    bridge.uploadAttachment(taskId, localFilePath);
    REQUIRE(pumpUntil([&] { return uploaded; }));
    INFO("failed() message, if any: " << failureMessage.toStdString());
    CHECK(uploadedTaskId == taskId);

    // uploadAttachment() refreshes `attachments` on success (via its own
    // internal getAttachments() call) -- no separate getAttachments() call
    // should be needed here, but this test waits for the property to reflect
    // exactly one row rather than assuming the refresh already landed by the
    // time attachmentUploaded() fired.
    REQUIRE(pumpUntil([&] { return bridge.attachments().size() == 1; }));
    const QVariantMap attachmentRow = bridge.attachments().front().toMap();
    for (const char* key :
         {"id", "taskId", "filename", "contentType", "sizeBytes", "storageKey", "uploadedBy", "uploadedAtMs"}) {
        INFO("missing key: " << key);
        REQUIRE(attachmentRow.contains(QString::fromLatin1(key)));
    }
    CHECK(attachmentRow.value(QStringLiteral("filename")).toString() == QStringLiteral("report.pdf"));
    CHECK(attachmentRow.value(QStringLiteral("taskId")).toString() == taskId);
    CHECK(attachmentRow.value(QStringLiteral("sizeBytes")).toLongLong() ==
          static_cast<qlonglong>(std::string_view{"this is the attachment's own bytes"}.size()));
    const QString storageKey = attachmentRow.value(QStringLiteral("storageKey")).toString();
    REQUIRE_FALSE(storageKey.isEmpty());

    // A second, independent getAttachments() call also reflects the upload --
    // proves the metadata is really committed server-side, not just cached on
    // this bridge from the upload's own response.
    bool attachmentsListedFired = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::attachmentsListed,
                     [&](const QVariantList&) { attachmentsListedFired = true; });
    bridge.getAttachments(taskId);
    REQUIRE(pumpUntil([&] { return attachmentsListedFired; }));
    REQUIRE(bridge.attachments().size() == 1);

    // downloadAttachment(): round-trips the same bytes back out to a second
    // local path.
    const QString downloadedFilePath = tempDir.filePath(QStringLiteral("report-downloaded.pdf"));
    bool downloaded = false;
    QString downloadedPath;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::attachmentDownloaded, [&](const QString& path) {
        downloadedPath = path;
        downloaded = true;
    });
    bridge.downloadAttachment(storageKey, downloadedFilePath);
    REQUIRE(pumpUntil([&] { return downloaded; }));
    CHECK(downloadedPath == downloadedFilePath);

    QFile downloadedFile{downloadedFilePath};
    REQUIRE(downloadedFile.open(QIODevice::ReadOnly));
    CHECK(downloadedFile.readAll() == QByteArrayLiteral("this is the attachment's own bytes"));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE(
    "BoardBridge::downloadAttachment reports failed() for a storageKey that was never uploaded "
    "(a real 404 from a real AttachmentServer)",
    "[kanban][gui][attachments]") {
    // The review finding this test closes: uploadAttachment's own
    // "no server configured" failed() test never issues an HTTP request at
    // all (it's a pure pre-flight guard). This test is the first one in this
    // file that actually drives downloadAttachment() against a real, running
    // AttachmentServer and asserts BoardBridge::failed(QString) fires from a
    // genuine 404 response -- mirrors test_attachment_server.cpp's own
    // "AttachmentServer returns 404 for a GET naming a storageKey that was
    // never uploaded" case, one layer up at the bridge.
    DbFixture fixture;
    const morph::session::TokenIssuer issuer{std::string{kAttachmentTestSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kAttachmentTestSecret}, morph::session::hmacSha256};

    const auto storageDir = freshAttachmentStorageDir("download_missing");
    kanban::http::AttachmentServer server{verifier, kanban::http::AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    auto rig = makeAuthedRigWithToken(issuer, "alice");
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    bridge.setAttachmentServerUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    // A syntactically-valid-looking storageKey (64 hex chars, matching
    // test_attachment_server.cpp's own fakeKey shape) that was never
    // produced by any upload -- there is no blob on disk and no
    // AttachmentRecord naming it.
    const QString neverUploadedKey = QString::fromStdString(std::string(64, 'a'));
    const QString localFilePath = tempDir.filePath(QStringLiteral("should-not-exist.bin"));

    QString message;
    bool failed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });
    bool downloaded = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::attachmentDownloaded,
                     [&](const QString&) { downloaded = true; });

    bridge.downloadAttachment(neverUploadedKey, localFilePath);
    REQUIRE(pumpUntil([&] { return failed || downloaded; }));

    CHECK(failed);
    CHECK_FALSE(downloaded);
    CHECK_FALSE(message.isEmpty());
    // No partial/empty file should be mistaken for a successful download --
    // downloadAttachment() only opens localFilePath for writing after a
    // successful HTTP response (board_qml_bridge.cpp's own downloadAttachment()).
    CHECK_FALSE(QFile::exists(localFilePath));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE(
    "BoardBridge::downloadAttachment reports failed() the same way for a storageKey that belongs to "
    "a DIFFERENT project the caller has no role on (authenticated, not authorized)",
    "[kanban][gui][attachments]") {
    // The stronger, security-relevant half of the same review finding:
    // mirrors test_attachment_server.cpp's own "AttachmentServer returns 404
    // (not 200) for a GET whose bearer token is validly signed for a
    // DIFFERENT project the principal has no role on" case, wired up through
    // two real BoardBridge instances (one per principal/project) rather than
    // raw sockets, proving the GUI layer collapses this case to failed() the
    // same way it does the plain-nonexistent-key case above -- neither case
    // is allowed to behave differently at the bridge, matching the server's
    // own deliberate 404-for-both design.
    DbFixture fixture;
    const morph::session::TokenIssuer issuer{std::string{kAttachmentTestSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kAttachmentTestSecret}, morph::session::hmacSha256};

    const auto storageDir = freshAttachmentStorageDir("cross_tenant_get");
    kanban::http::AttachmentServer server{verifier, kanban::http::AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());
    const QString serverUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());

    // alice's project owns the attachment: a real upload + AddAttachment
    // commit through a real BoardBridge, exactly like the upload/download
    // round-trip test above.
    auto aliceRig = makeAuthedRigWithToken(issuer, "alice");
    const auto aliceProjectId = seedProject(*aliceRig);
    kanban::gui::BoardBridge aliceBridge{aliceRig->bridge(0), aliceRig->executor()};
    aliceBridge.setAttachmentServerUrl(serverUrl);

    bool changed = false;
    QObject::connect(&aliceBridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    aliceBridge.openBoard(QString::number(aliceProjectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    aliceBridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString columnId = aliceBridge.board()
                                 .value(QStringLiteral("columns"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("id"))
                                 .toString();

    changed = false;
    aliceBridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = aliceBridge.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

    changed = false;
    aliceBridge.createTask(columnId, swimlaneId, QStringLiteral("Secret task"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId = aliceBridge.board()
                               .value(QStringLiteral("tasks"))
                               .toList()
                               .front()
                               .toMap()
                               .value(QStringLiteral("id"))
                               .toString();

    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString uploadFilePath = tempDir.filePath(QStringLiteral("secret.txt"));
    {
        QFile localFile{uploadFilePath};
        REQUIRE(localFile.open(QIODevice::WriteOnly));
        localFile.write(QByteArrayLiteral("secret attachment bytes only alice's project should see"));
    }

    bool uploaded = false;
    QObject::connect(&aliceBridge, &kanban::gui::BoardBridge::attachmentUploaded,
                     [&](const QString&) { uploaded = true; });
    aliceBridge.uploadAttachment(taskId, uploadFilePath);
    REQUIRE(pumpUntil([&] { return uploaded; }));
    REQUIRE(pumpUntil([&] { return aliceBridge.attachments().size() == 1; }));
    const QString storageKey =
        aliceBridge.attachments().front().toMap().value(QStringLiteral("storageKey")).toString();
    REQUIRE_FALSE(storageKey.isEmpty());

    // mallory: her own, entirely separate project -- a real, authenticated
    // principal (a real signed bearer token) with no role whatsoever on
    // alice's project.
    auto malloryRig = makeAuthedRigWithToken(issuer, "mallory");
    static_cast<void>(seedProject(*malloryRig));
    kanban::gui::BoardBridge malloryBridge{malloryRig->bridge(0), malloryRig->executor()};
    malloryBridge.setAttachmentServerUrl(serverUrl);

    QString message;
    bool failed = false;
    QObject::connect(&malloryBridge, &kanban::gui::BoardBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });
    bool downloaded = false;
    QObject::connect(&malloryBridge, &kanban::gui::BoardBridge::attachmentDownloaded,
                     [&](const QString&) { downloaded = true; });

    const QString downloadPath = tempDir.filePath(QStringLiteral("mallory-should-not-get-this.txt"));
    malloryBridge.downloadAttachment(storageKey, downloadPath);
    REQUIRE(pumpUntil([&] { return failed || downloaded; }));

    CHECK(failed);
    CHECK_FALSE(downloaded);
    CHECK_FALSE(message.isEmpty());
    CHECK_FALSE(QFile::exists(downloadPath));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("BoardBridge::uploadAttachment reports failed() when no attachment server is configured",
          "[kanban][gui][attachments]") {
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
    const QString columnId = bridge.board()
                                 .value(QStringLiteral("columns"))
                                 .toList()
                                 .front()
                                 .toMap()
                                 .value(QStringLiteral("id"))
                                 .toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

    changed = false;
    bridge.createTask(columnId, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString localFilePath = tempDir.filePath(QStringLiteral("report.pdf"));
    {
        QFile localFile{localFilePath};
        REQUIRE(localFile.open(QIODevice::WriteOnly));
        localFile.write(QByteArrayLiteral("bytes"));
    }

    QString message;
    bool failed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });
    // setAttachmentServerUrl() is never called here -- BoardBridge must not
    // guess an address, only report failed().
    bridge.uploadAttachment(taskId, localFilePath);
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(message.isEmpty());
}
