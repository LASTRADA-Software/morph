// SPDX-License-Identifier: Apache-2.0
//
// End-to-end user journeys: server plus payloads, no GUI.
//
// Every other layer of this rung's suite verifies a slice and assumes the
// surrounding sequence away. Authentication in particular is always a
// *precondition* -- rigs arrive already authenticated -- so no test covers a
// sign-in that fails and is then retried, which is close to the most common
// real interaction there is.
//
// These walk coherent workflows in order, over the whole backend-mode matrix,
// and require the same outcome in each. What only a sequence can catch: state
// leaking between steps, a failed step corrupting what follows, an error path
// that leaves the client wedged, a session that outlives sign-out.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/core/bridge.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/core/errors.hpp"
#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/journey.hpp"
#include "testkit/pump.hpp"

using kanban::BoardModel;
using kanban::ProjectAdminModel;
using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Journey;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "journey-test-secret-at-least-32-bytes-ok";

/// @brief Installs the process-global issuer `AuthModel::execute(Login)`
///        mints from, and removes it again.
///
/// `kanban::app::App` normally owns this; a journey that drives `AuthModel`
/// directly has to stand in for it, or `Login` fails with "no token issuer
/// installed" -- which is exactly how this test first failed.
class ScopedTokenIssuer {
public:
    explicit ScopedTokenIssuer(std::string secret) {
        kanban::auth::setTokenIssuer(
            std::make_shared<morph::session::TokenIssuer>(std::move(secret), morph::session::hmacSha256));
    }
    ScopedTokenIssuer(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer& operator=(const ScopedTokenIssuer&) = delete;
    ~ScopedTokenIssuer() { kanban::auth::setTokenIssuer(nullptr); }
};

[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief Runs @p call and returns the error message it failed with.
///
/// Asserts on the *message*, not the exception type, because the type does not
/// survive the wire: `Mode::Local` propagates `kanban::Forbidden` itself,
/// while `Mode::Socket` delivers the same rejection as an `err` envelope the
/// client surfaces as a plain `std::runtime_error`. A journey that asserted
/// the type would pass in one mode and fail in another while describing the
/// same user-visible outcome.
/// @param call The call expected to fail.
/// @return The failure message, or an empty string if @p call unexpectedly succeeded.
template <typename Call>
[[nodiscard]] std::string errorMessageFrom(Call&& call) {
    try {
        call();
    } catch (const std::exception& exc) {
        return exc.what();
    }
    return {};
}

[[nodiscard]] bool mentions(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief Whether @p message is one of the two rejections an unauthenticated
///        call can produce.
///
/// Deliberately accepts both spellings, because the *wording* is
/// deployment-dependent even though the user-visible outcome is not: in
/// `Mode::Local` the model itself refuses ("no authenticated principal"),
/// while in `Mode::Socket` the server's `KanbanAuthorizer` rejects the
/// envelope first and the client sees a bare "unauthorized". Asserting either
/// literal would make this journey pass in one mode and fail in another while
/// describing the same behaviour -- which is precisely the sort of divergence
/// running journeys over the whole mode matrix exists to surface.
[[nodiscard]] bool isRejection(std::string_view message) {
    return mentions(message, "principal") || mentions(message, "unauthorized");
}

}  // namespace

TEST_CASE("Journey: a rejected sign-in, a successful retry, and a session that ends at sign-out",
          "[kanban][journey]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{mode, 1, authorizer};
    const ScopedTokenIssuer installedIssuer{std::string{kSecret}};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    kanban::AuthModel auth;
    kanban::ProjectId projectId;

    Journey{"sign-in"}
        .step("acting before signing in is rejected, not silently allowed",
              [&] {
                  auto admin = rig.client<ProjectAdminModel>(0);
                  const auto message =
                      errorMessageFrom([&] { awaitQt(admin.execute(kanban::CreateProject{.name = "Too early"})); });
                  INFO("error was: " << message);
                  REQUIRE_FALSE(message.empty());
                  CHECK(isRejection(message));
              })
        .step("signing in with a malformed username is rejected with a usable error",
              [&] {
                  // No password exists in this rung to get wrong (AuthModel
                  // mints a token for any syntactically valid username), so
                  // the reachable rejection is a username the principal
                  // grammar refuses -- here a reserved `system:` identity.
                  const auto message =
                      errorMessageFrom([&] { auth.execute(kanban::Login{.username = "system:root"}); });
                  INFO("error was: " << message);
                  CHECK_FALSE(message.empty());
              })
        .step("the rejected sign-in left nothing behind -- a valid one still works",
              [&] {
                  const auto result = auth.execute(kanban::Login{.username = "alice"});
                  REQUIRE(result.token.hasValue());
                  CHECK(result.principal == "alice");
              })
        .step("with the session installed, the same call now succeeds",
              [&] {
                  rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));
                  auto admin = rig.client<ProjectAdminModel>(0);
                  const auto created = awaitQt(admin.execute(kanban::CreateProject{.name = "Q3"}));
                  REQUIRE(created.id.hasValue());
                  projectId = created.id;
              })
        .step("signing out ends the session -- the very same call is rejected again",
              [&] {
                  // The point of the step: a session must not outlive its
                  // sign-out. A client that kept working here would be holding
                  // authority the user believed they had given up.
                  rig.bridge(0).setDefaultSession({});
                  auto admin = rig.client<ProjectAdminModel>(0);
                  const auto message = errorMessageFrom(
                      [&] { awaitQt(admin.execute(kanban::CreateProject{.name = "After sign-out"})); });
                  INFO("error was: " << message);
                  REQUIRE_FALSE(message.empty());
                  CHECK(isRejection(message));
              })
        .run();
}

TEST_CASE("Journey: open a board, add work, move it, and have a second client converge", "[kanban][journey]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{mode, 2, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));
    rig.bridge(1).setDefaultSession(tokenContextFor(issuer, "alice"));

    kanban::ProjectId projectId;
    kanban::ColumnId backlogId;
    kanban::ColumnId inProgressId;
    kanban::SwimlaneId laneId;
    kanban::TaskId taskId;

    BridgeHandler<BoardModel, AllowShared> board{rig.bridge(0), rig.executor()};

    Journey{"board workflow"}
        .step("create project \"Q3\"",
              [&] {
                  auto admin = rig.client<ProjectAdminModel>(0);
                  const auto created = awaitQt(admin.execute(kanban::CreateProject{.name = "Q3"}));
                  REQUIRE(created.id.hasValue());
                  projectId = created.id;
              })
        .step("open its board -- loads, and is empty",
              [&] {
                  const auto opened = awaitQt(board.execute(kanban::OpenBoard{.projectId = projectId}));
                  CHECK(opened.name == "Q3");
                  CHECK(opened.columns.empty());
                  CHECK(opened.tasks.empty());
              })
        .step("set up a backlog and an in-progress column",
              [&] {
                  awaitQt(board.execute(kanban::CreateColumn{.name = "Backlog", .wipLimit = 0}));
                  const auto state =
                      awaitQt(board.execute(kanban::CreateColumn{.name = "In Progress", .wipLimit = 0}));
                  REQUIRE(state.columns.size() == 2);
                  backlogId = state.columns[0].id;
                  inProgressId = state.columns[1].id;
                  const auto lanes = awaitQt(board.execute(kanban::CreateSwimlane{.name = "Default"}));
                  REQUIRE(lanes.swimlanes.size() == 1);
                  laneId = lanes.swimlanes.front().id;
              })
        .step("create \"write the thing\" -- it appears in the backlog",
              [&] {
                  const auto state = awaitQt(board.execute(
                      kanban::CreateTask{.columnId = backlogId, .swimlaneId = laneId, .title = "write the thing"}));
                  REQUIRE(state.tasks.size() == 1);
                  CHECK(state.tasks.front().title == "write the thing");
                  CHECK(state.tasks.front().columnId == backlogId);
                  taskId = state.tasks.front().id;
              })
        .step("move it to in-progress -- it lands there, positions still dense and unique",
              [&] {
                  const auto state = awaitQt(board.execute(kanban::MoveTaskPosition{
                      .taskId = taskId, .columnId = inProgressId, .swimlaneId = laneId, .position = 0}));
                  REQUIRE(state.tasks.size() == 1);
                  CHECK(state.tasks.front().columnId == inProgressId);
                  CHECK(state.tasks.front().position == 0);
              })
        .step("comment on it -- the comment is attached to that task",
              [&] {
                  awaitQt(board.execute(kanban::AddComment{.taskId = taskId, .body = "on it"}));
                  const auto state = awaitQt(board.execute(kanban::GetBoardState{}));
                  REQUIRE(state.comments.size() == 1);
                  CHECK(state.comments.front().taskId == taskId);
              })
        .step("a second, independent client opens the same board and sees all of it",
              [&] {
                  BridgeHandler<BoardModel, AllowShared> second{rig.bridge(1), rig.executor()};
                  const auto state = awaitQt(second.execute(kanban::OpenBoard{.projectId = projectId}));
                  CHECK(state.name == "Q3");
                  REQUIRE(state.tasks.size() == 1);
                  CHECK(state.tasks.front().columnId == inProgressId);
                  REQUIRE(state.comments.size() == 1);
                  CHECK(state.comments.front().body == "on it");
              })
        .step("a rejected action mid-journey does not wedge the client",
              [&] {
                  // The step the per-action tests cannot express: an error has
                  // to leave the handler usable. A client that stopped working
                  // after one bad request would strand the user on a screen
                  // that no longer responds.
                  const auto message = errorMessageFrom([&] {
                      awaitQt(board.execute(kanban::MoveTaskPosition{.taskId = kanban::TaskId{999999},
                                                                     .columnId = inProgressId,
                                                                     .swimlaneId = laneId,
                                                                     .position = 0}));
                  });
                  INFO("error was: " << message);
                  REQUIRE_FALSE(message.empty());

                  const auto state = awaitQt(board.execute(kanban::GetBoardState{}));
                  REQUIRE(state.tasks.size() == 1);
                  CHECK(state.tasks.front().title == "write the thing");
              })
        .run();
}
