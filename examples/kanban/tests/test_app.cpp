// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>

#include "kanban/app/app.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/core/errors.hpp"
#include "kanban/dto/activity_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using kanban::BoardModel;
using kanban::ProjectAdminModel;
using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief A fresh, empty action-log path per test. See
///        `bookmarks::tests::freshLogPath`'s identical rationale: a leftover
///        file from an earlier test would otherwise seed
///        `FileActionLog`'s on-disk idempotency-dedup state.
[[nodiscard]] std::filesystem::path freshLogPath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kanban_" + name + ".jsonl");
    std::filesystem::remove(path);
    return path;
}

/// @brief Builds a signed session `Context` for @p principal, issued by
///        @p issuer -- identical shape to
///        `test_shared_instance_lifecycle.cpp`'s own `tokenContextFor`.
[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

constexpr std::string_view kSecret = "app-test-secret-at-least-32-bytes-long";

}  // namespace

TEST_CASE("AuthModel::execute(Login) mints a token that verifies against the same App's authorizer", "[kanban][app]") {
    const auto logPath = freshLogPath("login");
    {
        const kanban::app::App app{logPath, std::string{kSecret}};
        kanban::AuthModel authModel;
        const auto result = authModel.execute(kanban::Login{.username = "alice"});
        REQUIRE(result.token.hasValue());
        CHECK(result.principal == "alice");

        // Verified against a *separately constructed* authorizer holding the
        // same secret -- exactly what the App's own RemoteServer installed.
        const kanban::auth::KanbanAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
        morph::session::Context ctx;
        ctx.token = *result.token;
        const auto principal = authz.authenticate(ctx);
        REQUIRE(principal.has_value());
        CHECK(*principal == "alice");
        CHECK(authz.authorize(ctx, "BoardModel", "OpenBoard"));

        // ...and does not verify against a different secret.
        const kanban::auth::KanbanAuthorizer other{"a-different-secret-entirely-too", morph::session::hmacSha256};
        CHECK_FALSE(other.authenticate(ctx).has_value());
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("AuthModel::execute(Login) refuses to mint a token in the reserved system: namespace", "[kanban][app]") {
    const auto logPath = freshLogPath("login_reserved");
    {
        const kanban::app::App app{logPath, std::string{kSecret}};
        kanban::AuthModel authModel;
        REQUIRE_THROWS_AS(authModel.execute(kanban::Login{.username = "system:anything"}), kanban::ValidationError);
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("AuthModel::execute(Login) throws when no App has installed a TokenIssuer", "[kanban][app]") {
    // Every other [kanban][app] case constructs its App as a scoped local,
    // and ~App clears the global issuer, so this case sees a clean nullptr
    // regardless of Catch2's run order.
    REQUIRE(kanban::auth::tokenIssuer() == nullptr);
    kanban::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(kanban::Login{.username = "alice"}), kanban::ValidationError);
}

// ═════════════════════════════════════════════════════════════════════════
// The load-bearing case: a registry-constructed BoardModel's own _log must
// be the SAME IActionLog instance the holder's auto-append writes to.
// ═════════════════════════════════════════════════════════════════════════
//
// Every BoardModel test in test_board_model.cpp constructs `kanban::BoardModel
// model;` directly and calls `model.execute(action)` -- BoardModel::execute
// straight, never through IModelHolder/ActionDispatcher/RemoteServer, so
// recordIfAttached's auto-append and RemoteServer::LogProvider's attach path
// never fire for that path at all. That leaves App's own real,
// registry-constructed, keyed-attach BoardModel instance -- the one a real
// socket client's RemoteServer::acquireSharedInstance actually builds --
// completely unexercised. This test dispatches through App's real
// RemoteServer (via SimulatedRemoteBackend, the identical in-process-but-
// real-dispatch path bookmarks::App's own metadata worker uses -- not a
// shortcut, not LocalBackend) so BoardModel is constructed exactly the way a
// real client's `register`/`attach` envelope constructs it: default-
// constructed by `_registry.create(env.typeId)`, then
// `attachLogIfConfigured` calls `holder->attachActionLog(log, contextKey)`,
// which (after this task's morph/core/model.hpp fix) forwards to
// `BoardModel::attachActionLog` via `IModelHolder::onActionLogAttached`.
// Before that fix, BoardModel::_log stayed null on this exact path and
// GetActivity returned an empty stream silently -- the gap Task 13's
// reviewer flagged.
TEST_CASE(
    "A registry-constructed BoardModel's GetActivity sees the entry auto-appended by the same "
    "dispatch that created it, over the real RemoteServer -- not a direct BoardModel construction",
    "[kanban][app][activity]") {
    DbFixture fixture;
    const auto logPath = freshLogPath("activity_e2e");
    {
        kanban::app::App app{logPath, std::string{kSecret}};

        const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
        auto backend = std::make_unique<::morph::backend::SimulatedRemoteBackend>(*app.server());
        Bridge bridge{std::move(backend)};
        bridge.setDefaultSession(tokenContextFor(issuer, "alice"));
        ::morph::qt::QtExecutor exec;

        // CreateProject over a plain (non-keyed) handler -- alice becomes
        // the project's Manager, per design spec §3.
        BridgeHandler<ProjectAdminModel> admin{bridge, &exec};
        const auto created = awaitQt(admin.execute(kanban::CreateProject{.name = "Real Dispatch Board"}));
        REQUIRE(created.id.hasValue());

        // The keyed/shared attach path -- BridgeHandler<BoardModel,
        // AllowShared> -- is exactly what sends a non-empty contextKey (==
        // the project id string) on its register/attach envelope
        // (bridge.hpp's attachHandler: `contextKey = primary`), which is
        // what makes RemoteServer::acquireSharedInstance's
        // attachLogIfConfigured consult App's installed LogProvider at all
        // -- this is the specific path this task's fix targets, and the one
        // path where App's *other* auto-attach (the process-wide default
        // log every ModelFactory::create<Model>() call picks up, regardless
        // of registration mode) does not by itself explain a populated
        // entityKey: attachLogIfConfigured's holder->attachActionLog(log,
        // contextKey) call is the *second* attach on this exact instance,
        // and it is the one that stamps entityKey with the real project id
        // instead of leaving it empty at registration time. See the companion
        // "plain (non-shared, non-keyed)" case below for why a plain
        // registration's GetActivity also isn't empty -- App wires both
        // paths, on purpose.
        BridgeHandler<BoardModel, AllowShared> board{bridge, &exec};
        const auto opened = awaitQt(board.execute(kanban::OpenBoard{.projectId = created.id}));
        CHECK(opened.name == "Real Dispatch Board");

        // A loggable mutating action -- CreateColumn -- dispatched through
        // the real server. If BoardModel::_log were still null on this
        // registry-constructed instance (the pre-fix gap), this call would
        // still succeed (BoardModel::logAction no-ops when _log is unset),
        // but GetActivity below would come back empty.
        const auto afterColumn = awaitQt(board.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}));
        REQUIRE(afterColumn.columns.size() == 1);

        const auto activity = awaitQt(board.execute(kanban::GetActivity{}));
        REQUIRE(activity.events.size() == 1);
        CHECK(activity.events.front().actionType == "CreateColumn");
        CHECK(activity.events.front().principal == "alice");

        // And the same durable sink App installed is what the entry landed
        // in -- not some other, disconnected log: reopening the very file
        // App's FileActionLog was constructed over shows the identical
        // entry, keyed by the project id, exactly as attachLogIfConfigured's
        // contextKey plumbing promises.
        const morph::journal::FileActionLog reopened{logPath};
        const auto entries = reopened.entries(std::to_string(*created.id));
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().actionType == "CreateColumn");
        CHECK(entries.front().modelType == "BoardModel");
        CHECK(entries.front().principal == "alice");
    }
    std::filesystem::remove(logPath);
}

TEST_CASE(
    "A plain (non-shared, non-keyed) BoardModel registration also sees its own GetActivity, via "
    "App's process-wide default log, independently of the LogProvider/contextKey path",
    "[kanban][app][activity]") {
    // Companion to the case above, verifying the *other* attach path App
    // wires: `_registry.create(env.typeId)` (the plain, non-keyed "register"
    // path -- morph/core/remote.hpp) calls `ModelFactory::create<BoardModel>()`
    // (morph/core/model.hpp), which auto-attaches the process-wide default
    // log `App::App()` installs via `morph::journal::setActionLog` -- this
    // runs *before* attachLogIfConfigured's LogProvider/contextKey path ever
    // gets a chance to, and does not depend on a contextKey being set at
    // all. Both attach paths reach BoardModel::attachActionLog identically
    // via this task's onActionLogAttached forward, so a plain
    // BridgeHandler<BoardModel> (no AllowShared, no contextKey on its
    // register envelope) still gets a working GetActivity once
    // OpenBoard::execute sets _projectIdStr to the real project id.
    DbFixture fixture;
    const auto logPath = freshLogPath("activity_plain");
    {
        kanban::app::App app{logPath, std::string{kSecret}};
        const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
        auto backend = std::make_unique<::morph::backend::SimulatedRemoteBackend>(*app.server());
        Bridge bridge{std::move(backend)};
        bridge.setDefaultSession(tokenContextFor(issuer, "alice"));
        ::morph::qt::QtExecutor exec;

        BridgeHandler<ProjectAdminModel> admin{bridge, &exec};
        const auto created = awaitQt(admin.execute(kanban::CreateProject{.name = "Plain Registration Board"}));

        BridgeHandler<BoardModel> board{bridge, &exec};
        awaitQt(board.execute(kanban::OpenBoard{.projectId = created.id}));
        awaitQt(board.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}));

        const auto activity = awaitQt(board.execute(kanban::GetActivity{}));
        REQUIRE(activity.events.size() == 1);
        CHECK(activity.events.front().actionType == "CreateColumn");
    }
    std::filesystem::remove(logPath);
}
