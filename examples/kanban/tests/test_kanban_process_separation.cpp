// SPDX-License-Identifier: Apache-2.0
//
// True process separation: real client processes over a real WebSocket,
// against a server hosted in this test process.
//
// Every other multi-client test in the ladder runs its clients as objects in
// one process on one pumped thread, because QtExecutor posts to
// QCoreApplication::instance() and nothing else. That is enough to interleave
// operations, but it cannot reach the one condition this file exists for: a
// client that *stops existing* without unwinding. An in-process "crash" still
// runs destructors, still deregisters its handlers, still closes its backend.
// A SIGKILLed process does none of that, so the server sees exactly what it
// sees when a real client segfaults or its machine loses power -- a socket
// that goes quiet, and nothing else.
//
// The server stays in-process on purpose: assertions can then read
// RemoteServer::health() directly rather than inventing an IPC channel to ask
// a server binary what it thinks its own state is.

#include "kanban/app/app.hpp"

#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/process_pool.hpp"
#include "testkit/pump.hpp"

#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::ProcessPool;
using morph::ladder::testkit::pumpUntil;

namespace {

constexpr std::string_view kSecret = "process-separation-secret-at-least-32-bytes";

[[nodiscard]] std::filesystem::path freshLogPath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kanban_procsep_" + name + ".jsonl");
    std::filesystem::remove(path);
    return path;
}

[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                       std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief Server, transport, and a seeded project with one task -- everything
///        a spawned client needs a URL and an id for.
struct Fixture {
    DbFixture db;
    kanban::app::App app;
    morph::session::TokenIssuer issuer;
    morph::qt::QtWebSocketServer transport;
    morph::qt::QtExecutor exec;
    kanban::ProjectId projectId;
    std::string token;

    explicit Fixture(const std::string& name)
        : app{freshLogPath(name), std::string{kSecret}},
          issuer{std::string{kSecret}, morph::session::hmacSha256},
          transport{*app.server(), 0} {}

    /// @brief Seeds a project with one column, one swimlane, and one task,
    ///        through the same RemoteServer the socket clients will reach.
    void seed() {
        Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*app.server())};
        const auto ctx = tokenContextFor(issuer, "alice");
        token = ctx.token;
        bridge.setDefaultSession(ctx);

        BridgeHandler<kanban::ProjectAdminModel> admin{bridge, &exec};
        const auto created = morph::ladder::testkit::awaitQt(
            admin.execute(kanban::CreateProject{.name = "Process Separation Board"}));
        REQUIRE(created.id.hasValue());
        projectId = created.id;

        BridgeHandler<kanban::BoardModel, morph::bridge::AllowShared> board{bridge, &exec};
        morph::ladder::testkit::awaitQt(board.execute(kanban::OpenBoard{.projectId = projectId}));
        const auto column = morph::ladder::testkit::awaitQt(
            board.execute(kanban::CreateColumn{.name = "Todo", .wipLimit = 0}));
        const auto lane = morph::ladder::testkit::awaitQt(
            board.execute(kanban::CreateSwimlane{.name = "Default"}));
        REQUIRE_FALSE(column.columns.empty());
        REQUIRE_FALSE(lane.swimlanes.empty());
        morph::ladder::testkit::awaitQt(board.execute(kanban::CreateTask{.columnId = column.columns.front().id,
                                                                         .swimlaneId = lane.swimlanes.front().id,
                                                                         .title = "Shared task"}));
    }

    [[nodiscard]] QString url() const {
        return QStringLiteral("ws://127.0.0.1:%1").arg(transport.port());
    }

    [[nodiscard]] QStringList clientArgs() const {
        return QStringList{QStringLiteral("--url"),       url(),
                           QStringLiteral("--project"),   QString::number(*projectId),
                           QStringLiteral("--token"),     QString::fromStdString(token),
                           QStringLiteral("--principal"), QStringLiteral("alice")};
    }
};

}  // namespace

TEST_CASE("Process separation: real client processes drive one shared board", "[kanban][process]") {
    REQUIRE(QFileInfo::exists(QStringLiteral(MORPH_LADDER_HEADLESS_BIN)));

    Fixture fixture{"multi"};
    REQUIRE(fixture.transport.listen());
    fixture.seed();

    // Four clients, each in its own address space, each attaching to the same
    // keyed BoardModel and commenting on the same task.
    constexpr int kClients = 4;
    ProcessPool pool{QStringLiteral(MORPH_LADDER_HEADLESS_BIN)};
    for (int idx = 0; idx < kClients; ++idx) {
        auto args = fixture.clientArgs();
        args << QStringLiteral("--comments") << QStringLiteral("1");
        INFO("spawning client " << idx);
        REQUIRE(pool.spawn(args) != nullptr);
    }

    // Pumped, not blocking: this process *is* the server (see
    // ProcessPool::allExited).
    REQUIRE(pumpUntil([&] { return pool.allExited(); }, std::chrono::seconds{30}));

    for (std::size_t idx = 0; idx < pool.size(); ++idx) {
        INFO("client " << idx << " stderr: " << pool[idx].stderrText());
        CHECK_FALSE(pool[idx].crashed());
        CHECK(pool[idx].exitCode() == 0);
    }

    // Every client's comment landed on the one shared instance.
    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*fixture.app.server())};
    bridge.setDefaultSession(tokenContextFor(fixture.issuer, "alice"));
    BridgeHandler<kanban::BoardModel, morph::bridge::AllowShared> board{bridge, &fixture.exec};
    const auto state = morph::ladder::testkit::awaitQt(
        board.execute(kanban::OpenBoard{.projectId = fixture.projectId}));
    CHECK(state.comments.size() == static_cast<std::size_t>(kClients));
}

TEST_CASE("Process separation: a killed client's models are reclaimed", "[kanban][process]") {
    REQUIRE(QFileInfo::exists(QStringLiteral(MORPH_LADDER_HEADLESS_BIN)));

    Fixture fixture{"crash"};
    REQUIRE(fixture.transport.listen());
    fixture.seed();

    const auto baseline = fixture.app.server()->health().liveModels;

    ProcessPool pool{QStringLiteral(MORPH_LADDER_HEADLESS_BIN)};
    auto args = fixture.clientArgs();
    args << QStringLiteral("--hold");
    auto* client = pool.spawn(args);
    REQUIRE(client != nullptr);

    // Wait for the client to say it is attached. Killing it before the attach
    // would leave nothing to reclaim, and the test would pass while proving
    // nothing -- the failure mode this whole file is about.
    REQUIRE(pumpUntil(
        [&] {
            return client->process().readAllStandardOutput().contains("ATTACHED")
                   || fixture.app.server()->health().liveModels > baseline;
        },
        std::chrono::seconds{20}));
    REQUIRE(pumpUntil([&] { return fixture.app.server()->health().liveModels > baseline; },
                      std::chrono::seconds{10}));
    const auto attached = fixture.app.server()->health().liveModels;
    INFO("liveModels baseline=" << baseline << " attached=" << attached);

    // SIGKILL: no destructor runs, no deregister is sent, the socket simply
    // stops. The server learns of it only from the transport, which is the
    // whole point -- this is unreachable with an in-process client.
    client->kill();
    CHECK(client->crashed());

    // closeConnection() reclaims every model still registered under that
    // connection's scope, so liveModels must fall back to where it started.
    // Without that reclamation a crashed client leaks its board registration
    // for the server's lifetime.
    const bool reclaimed =
        pumpUntil([&] { return fixture.app.server()->health().liveModels <= baseline; }, std::chrono::seconds{20});
    INFO("liveModels after kill=" << fixture.app.server()->health().liveModels);
    CHECK(reclaimed);

    // And the board itself survived the crash: a fresh client still opens it.
    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*fixture.app.server())};
    bridge.setDefaultSession(tokenContextFor(fixture.issuer, "alice"));
    BridgeHandler<kanban::BoardModel, morph::bridge::AllowShared> board{bridge, &fixture.exec};
    const auto state = morph::ladder::testkit::awaitQt(
        board.execute(kanban::OpenBoard{.projectId = fixture.projectId}));
    CHECK(state.name == "Process Separation Board");
}
