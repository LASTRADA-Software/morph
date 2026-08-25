// SPDX-License-Identifier: Apache-2.0
//
// Task 14: examples/polls/tests/test_shared_instance_lifecycle.cpp's own
// coverage, ported to kanban's BoardModel/KanbanAuthorizer. The three pieces
// of coverage that test proved for PollModel:
//
// 1. The backend-mode matrix for the *keyed* attach path: CreateProject (a
//    direct, non-keyed call over a plain BridgeHandler<ProjectAdminModel>,
//    exactly like test_project_admin_model.cpp's own tests and
//    test_board_model.cpp's createProjectAs helper) -> handler.execute(
//    OpenBoard{projectId}) to attach -> CreateColumn -> GetBoardState, across
//    Mode::Local, Mode::LocalSingleThread, Mode::Socket. Proves the *keyed*
//    attach path (registerModelShared/attachModel, docs/spec/core/
//    shared_instances.md) works identically across all three modes for
//    BoardModel, not just the plain-registration path test_board_model.cpp's
//    own tests already exercise.
// 2. Shared-instance lifetime: N BridgeHandler<BoardModel, AllowShared>
//    instances attach to the same projectId, observe each other's writes,
//    and handler.instances() reflects the instance's real lifetime (present
//    while attached, absent once every attacher has released it).
// 3. Poisoned-instance attach: docs/spec/core/shared_instances.md's
//    "Failure modes" section documents that an instance whose very first
//    action's outcome fails is marked and evicted from the directory "the
//    next time anyone else attaches to that key -- not immediately", and
//    that "the handler that hit the failure does not self-heal: its primary
//    is already set to the poisoned key, so retrying the same keyed action
//    re-points nowhere (attachHandler's no-op-on-same-primary guard skips
//    the backend entirely) -- it keeps its broken instance". This test
//    attaches to a bad projectId twice from the *same* handler: the second
//    execute() never re-attaches (same primary, no-op guard), it just
//    re-dispatches OpenBoard against the same broken instance, and
//    BoardModel::execute(OpenBoard) re-runs loadProjectById() on every call
//    (board_model.cpp) -- so both attempts fail identically with NotFound,
//    proving there is no silently half-hydrated success on retry.
//
// Unlike polls' AllowAllAuthorizer-derived PollsAuthorizer, KanbanAuthorizer
// is SigningAuthorizer-derived (kanban_authorizer.hpp's own @file comment):
// BoardModel::requireRole() keys its project_has_roles lookup off
// session::current()->principal, and only a verifying authorizer supplies a
// trustworthy one. Every BackendRig client below therefore needs a real,
// signed session token installed via Bridge::setDefaultSession before it can
// do anything -- the identical setup test_bookmark_model.cpp's own
// "BookmarkModel over the full backend-mode matrix" case uses for
// BookmarksAuthorizer, kanban's other SigningAuthorizer-derived authorizer.
// A bare ScopedPrincipal (test_board_model.cpp's in-process helper) has no
// wire representation at all and cannot be used here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using kanban::BoardModel;
using kanban::CreateColumn;
using kanban::CreateProject;
using kanban::OpenBoard;
using kanban::ProjectAdminModel;
using kanban::ProjectId;
using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

namespace {

/// @brief Builds a signed session `Context` for @p principal, issued by
///        @p issuer. Mirrors test_bookmark_model.cpp's identical inline
///        pattern for BookmarksAuthorizer.
[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

}  // namespace

TEST_CASE("BoardModel over the full backend-mode matrix: create -> keyed-attach -> CreateColumn round trip",
          "[kanban][model]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    constexpr std::string_view kSecret = "matrix-test-secret-at-least-32-bytes";
    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{mode, 1, authorizer};

    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));

    // Plain (NoSharing) handler for CreateProject: CreateProject carries no
    // key, so nothing about it is shared/keyed -- the direct, non-keyed call
    // test_project_admin_model.cpp's own tests (and test_board_model.cpp's
    // createProjectAs helper) already use.
    auto creator = rig.client<ProjectAdminModel>(0);
    const auto created = awaitQt(creator.execute(CreateProject{.name = "Matrix board"}));
    REQUIRE(created.id.hasValue());

    // A fresh, AllowShared handler attaches via the *keyed* path --
    // handler.execute(OpenBoard{projectId}) -- proving keyed attach (not
    // just plain registration) works identically in every mode.
    BridgeHandler<BoardModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    const auto opened = awaitQt(handler.execute(OpenBoard{.projectId = created.id}));
    CHECK(opened.name == "Matrix board");
    CHECK(opened.columns.empty());

    const auto afterColumn = awaitQt(handler.execute(CreateColumn{.name = "To Do", .wipLimit = 0}));
    REQUIRE(afterColumn.columns.size() == 1);
    CHECK(afterColumn.columns.front().name == "To Do");

    const auto state = awaitQt(handler.execute(kanban::GetBoardState{}));
    REQUIRE(state.columns.size() == 1);
    CHECK(state.columns.front().name == "To Do");
}

TEST_CASE(
    "N shared handlers on one projectId observe each other's writes, and instances() reflects "
    "the instance's real lifetime",
    "[kanban][model][shared-instances]") {
    // 5 clients, not 4: the fifth connection is reserved for the fresh
    // "prober" handler below. Reusing one of the four attached connections
    // for it would race a fire-and-forget deregister's unsolicited (callId
    // 0) "ok" reply -- sent by BridgeHandler::~BridgeHandler on connection
    // teardown, per QtWebSocketBackend::deregisterModel's own doc comment --
    // against the prober's own synchronous instances() call on that same
    // connection. See test_shared_instance_lifecycle.cpp's (polls) identical
    // comment for the full mechanism; kept here regardless of that race's
    // framework-side fix since it costs nothing and still exercises the same
    // call shape.
    DbFixture fixture;
    constexpr std::string_view kSecret = "matrix-test-secret-at-least-32-bytes";
    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{Mode::Socket, 5, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    for (std::size_t i = 0; i < 5; ++i) {
        rig.bridge(i).setDefaultSession(tokenContextFor(issuer, "alice"));
    }

    // Client 0's plain handler creates the project -- CreateProject carries
    // no key.
    auto creator = rig.client<ProjectAdminModel>(0);
    const auto created = awaitQt(creator.execute(CreateProject{.name = "Team board"}));

    // Four independent AllowShared handlers, each its own socket client, all
    // attach to the same projectId -- exercising cross-connection sharing,
    // not merely cross-handler sharing within one connection.
    std::vector<std::unique_ptr<BridgeHandler<BoardModel, AllowShared>>> handlers;
    for (std::size_t i = 0; i < 4; ++i) {
        handlers.push_back(std::make_unique<BridgeHandler<BoardModel, AllowShared>>(rig.bridge(i), rig.executor()));
        const auto opened = awaitQt(handlers.back()->execute(OpenBoard{.projectId = created.id}));
        CHECK(opened.name == "Team board");
    }

    // All four attached to one shared instance -- instances() reports
    // exactly one live key while at least one handler holds it. BoardModel's
    // PrimaryKey is kanban::ProjectId itself, deduced by BRIDGE_MODEL_KEY
    // from &OpenBoard::projectId (board_model.hpp) -- not a std::string like
    // PollModel's pollId, and no longer the unwrapped std::int64_t this rung
    // used to declare by hand, so the expected vector element type is the
    // strong id.
    REQUIRE(awaitQt(handlers[0]->instances()) == std::vector<kanban::ProjectId>{created.id});

    // One handler creates a column; the other three see it on their next
    // GetBoardState, proving they share one instance's state, not four
    // divergent copies.
    (void)awaitQt(handlers[0]->execute(CreateColumn{.name = "In Progress", .wipLimit = 0}));
    for (std::size_t i = 1; i < handlers.size(); ++i) {
        const auto state = awaitQt(handlers[i]->execute(kanban::GetBoardState{}));
        REQUIRE(state.columns.size() == 1);
        CHECK(state.columns.front().name == "In Progress");
    }

    // Detach all four -- releasing the shared instance, which destructs.
    // ~BridgeHandler's deregister is deliberately fire-and-forget over a
    // socket (QtWebSocketBackend::deregisterModel's own doc comment: no
    // nested QEventLoop in a destructor), so this call returns before the
    // server has necessarily *processed* all four -- there is no
    // synchronous handshake to wait on here, only the directory eventually
    // reflecting the release.
    handlers.clear();

    // A fifth, fresh handler -- on its own never-before-used connection, see
    // this test's opening comment -- probes the directory: the key must be
    // gone now that every prior attacher has released it, not merely "the
    // test didn't crash". Polled, not a single snapshot: the four
    // deregisters above are still in flight the instant handlers.clear()
    // returns, so the first instances() reply can legitimately still list
    // the key -- pumpUntil retries the (synchronous, round-tripping)
    // instances() call until the directory catches up or the deadline
    // elapses.
    BridgeHandler<BoardModel, AllowShared> prober{rig.bridge(4), rig.executor()};
    std::vector<kanban::ProjectId> remaining;
    REQUIRE(pumpUntil([&] {
        remaining = awaitQt(prober.instances());
        return remaining.empty();
    }));
    CHECK(remaining.empty());
}

TEST_CASE(
    "Opening a stale projectId is NotFound through .onError(), not a crash, and a second attempt "
    "to the same bad key gets a fresh (still-failing) instance, not stale poisoned state",
    "[kanban][model][shared-instances]") {
    // Per docs/spec/core/shared_instances.md's "Failure modes" section: this
    // handler's primary is set to the poisoned key on the very first
    // execute() (attachHandler records the primary before dispatch), so its
    // own second execute() re-points nowhere -- the no-op-on-same-primary
    // guard skips the backend attach round trip entirely, and the action
    // simply re-dispatches against the same (still-broken) instance. Both
    // attempts fail identically -- NotFound, via .onError(), never a crash
    // and never a silently half-hydrated success -- because
    // BoardModel::execute(OpenBoard) re-runs loadProjectById() on every
    // call, not only the first.
    DbFixture fixture;
    constexpr std::string_view kSecret = "matrix-test-secret-at-least-32-bytes";
    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{Mode::Socket, 1, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));

    // A projectId with a real value but naming no row -- OpenBoard::validate()
    // only rejects an *unset* id, so this reaches loadProjectById() and
    // fails there with NotFound, exactly like the poll template's
    // "not-a-real-poll" stale-string key.
    const ProjectId badProjectId{999999};
    auto handler = rig.client<BoardModel>(0);

    bool firstFailed = false;
    handler.execute(OpenBoard{.projectId = badProjectId}).onError([&firstFailed](auto) { firstFailed = true; });
    REQUIRE(pumpUntil([&firstFailed] { return firstFailed; }));

    bool secondFailed = false;
    handler.execute(OpenBoard{.projectId = badProjectId}).onError([&secondFailed](auto) { secondFailed = true; });
    REQUIRE(pumpUntil([&secondFailed] { return secondFailed; }));

    // Both attempts are genuinely NotFound (loadProjectById's own message),
    // not merely "something failed" -- confirmed directly rather than only
    // inferred from the onError firing. Checked by message, not by C++
    // exception type: over Mode::Socket the server-side kanban::NotFound
    // does not survive the wire -- RemoteServer's dispatchExecute catches it
    // and replies "err" with only exc.what(), and QtWebSocketBackend::
    // onTextMessage reconstructs that as a generic std::runtime_error
    // carrying the same message (morph/qt/qt_websocket_backend.cpp's
    // execute-reply handling).
    try {
        (void)awaitQt(handler.execute(OpenBoard{.projectId = badProjectId}));
        FAIL("expected a third attempt against the same poisoned handler to fail identically");
    } catch (const std::exception& exc) {
        CHECK(std::string{exc.what()}.find("project not found") != std::string::npos);
    }
}

TEST_CASE("A Viewer's role on one project does not grant Member-level access on a different project",
          "[kanban][model][shared-instances]") {
    // BoardModel is keyed per-project (each project is its own shared
    // instance), so this ought to be implied by the per-instance keying
    // alone -- but a bug in requireRole()'s project-row lookup
    // (board_model.cpp: it queries project_has_roles keyed off *this
    // instance's own* attached projectId and the caller's principal) could
    // silently let a role granted on one project leak into another.
    // Written explicitly rather than assumed -- mirrors the poll template's
    // cross-poll admin-token isolation test, adapted to kanban's role model
    // (there is no per-poll admin token here; the analogous boundary is
    // per-project role isolation).
    DbFixture fixture;
    constexpr std::string_view kSecret = "matrix-test-secret-at-least-32-bytes";
    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{Mode::Socket, 2, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));
    rig.bridge(1).setDefaultSession(tokenContextFor(issuer, "bob"));

    auto adminAlice = rig.client<ProjectAdminModel>(0);
    const auto projectA = awaitQt(adminAlice.execute(CreateProject{.name = "Alice's board"}));
    // alice makes bob a Member on project A only.
    awaitQt(adminAlice.execute(
        kanban::SetMemberRole{.projectId = projectA.id, .principal = "bob", .role = kanban::Role::Member}));

    auto adminBob = rig.client<ProjectAdminModel>(1);
    const auto projectB = awaitQt(adminBob.execute(CreateProject{.name = "Bob's own board"}));
    // bob is the creator (Manager) of project B, not merely a Member there --
    // this asserts the isolation goes both ways: alice's grant on A does not
    // implicitly touch bob's standing on his own, separate project B either.

    // bob attaches to project A via a shared BoardModel handler and confirms
    // his Member role there works (CreateColumn requires >= Member).
    BridgeHandler<BoardModel, AllowShared> bobOnA{rig.bridge(1), rig.executor()};
    awaitQt(bobOnA.execute(OpenBoard{.projectId = projectA.id}));
    CHECK_NOTHROW(awaitQt(bobOnA.execute(CreateColumn{.name = "Bob's column on A", .wipLimit = 0})));

    // alice has no role at all on project B -- her attempt to even attach
    // (read) there must be Forbidden, not silently succeed just because she
    // is a Manager elsewhere. C1 fix: OpenBoard itself is now gated
    // (Role::Viewer minimum), so this must fail before any write is ever
    // attempted -- this line used to succeed and only the subsequent write
    // was asserted Forbidden, which demonstrated the read-side bypass rather
    // than proving isolation.
    BridgeHandler<BoardModel, AllowShared> aliceOnB{rig.bridge(0), rig.executor()};
    bool openFailed = false;
    aliceOnB.execute(OpenBoard{.projectId = projectB.id}).onError([&openFailed](auto) { openFailed = true; });
    REQUIRE(pumpUntil([&openFailed] { return openFailed; }));

    // Even though OpenBoard failed, exercise the write path too -- a
    // handler whose attach failed must not somehow still permit a write
    // through the same primary.
    bool failed = false;
    aliceOnB.execute(CreateColumn{.name = "Should be forbidden", .wipLimit = 0}).onError([&failed](auto) {
        failed = true;
    });
    REQUIRE(pumpUntil([&failed] { return failed; }));
}

TEST_CASE("A member demoted mid-session has their next move rejected and reads cut off",
          "[kanban][model][shared-instances][auth]") {
    // Task 8 (kanban rung-4 completion): BoardModel::requireRole runs on
    // every execute() call, not just at attach time -- so a role change
    // made through a *separate* ProjectAdminModel handler must be visible
    // to a BoardModel instance that has been sitting attached the whole
    // time and is never detached in between. Mode::Local (not Socket): the
    // assertions below check the concrete kanban::Forbidden exception
    // type, which only LocalBackend preserves end-to-end -- RemoteServer's
    // wire path (see this file's "Opening a stale projectId" test) collapses
    // every server-side exception to a generic std::runtime_error carrying
    // just .what(). Mode::Local's every "client" shares one Bridge (see
    // BackendRig's own doc comment), so there is only one default session at
    // a time -- this test flips it with setDefaultSession immediately before
    // each principal's call and always fully awaits (awaitQt) that call
    // before flipping again, so no two calls ever race over which session
    // Bridge::executeVia's synchronous `call.session = _defaultSession`
    // snapshot picks up.
    DbFixture fixture;
    constexpr std::string_view kSecret = "matrix-test-secret-at-least-32-bytes";
    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{Mode::Local, 1, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    auto asManager = [&] { rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "manager")); };
    auto asMember = [&] { rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "member")); };

    // manager creates the project (becoming its first Manager -- design
    // spec §3) and promotes "member" to Role::Member.
    asManager();
    auto admin = rig.client<ProjectAdminModel>(0);
    const auto project = awaitQt(admin.execute(CreateProject{.name = "Demo"}));
    awaitQt(admin.execute(
        kanban::SetMemberRole{.projectId = project.id, .principal = "member", .role = kanban::Role::Member}));

    // "member" attaches via a shared handler and stays attached for the
    // whole test -- this instance is never detached/recreated, which is
    // the point: authorization must be re-checked per-execute, not only at
    // attach time.
    asMember();
    BridgeHandler<BoardModel, AllowShared> memberBoard{rig.bridge(0), rig.executor()};
    const auto opened = awaitQt(memberBoard.execute(OpenBoard{.projectId = project.id}));
    CHECK(opened.name == "Demo");

    // Manager sets up a column/swimlane/task the member will try to move
    // while still a Member (proving normal write access before demotion).
    // Mode::Local's shared Bridge means "manager's" BoardModel handler
    // below is a distinct BridgeHandler instance from memberBoard, but both
    // ultimately dispatch through the one shared LocalBackend/project row --
    // there is only one server-side BoardModel instance for this project,
    // and manager's writes are what member observes next.
    asManager();
    auto managerBoard = rig.client<BoardModel>(0);
    awaitQt(managerBoard.execute(OpenBoard{.projectId = project.id}));
    const auto column = awaitQt(managerBoard.execute(CreateColumn{.name = "Todo", .wipLimit = 0}));
    const auto swimlane = awaitQt(managerBoard.execute(kanban::CreateSwimlane{.name = "Default"}));
    const auto afterTask = awaitQt(managerBoard.execute(kanban::CreateTask{
        .columnId = column.columns.front().id, .swimlaneId = swimlane.swimlanes.front().id, .title = "T1"}));
    const auto taskId = afterTask.tasks.back().id;

    // Manager demotes member to Viewer mid-session (member's attached
    // BoardModel instance is never detached -- this is the point of the
    // test: authorization is per-execute, per
    // docs/spec/core/shared_instances.md).
    asManager();
    awaitQt(admin.execute(
        kanban::SetMemberRole{.projectId = project.id, .principal = "member", .role = kanban::Role::Viewer}));

    // Next write from member (a Member-or-above-required action) is
    // rejected on the very same, still-attached instance.
    asMember();
    CHECK_THROWS_AS(awaitQt(memberBoard.execute(kanban::MoveTaskPosition{.taskId = taskId,
                                                                         .columnId = column.columns.front().id,
                                                                         .swimlaneId = swimlane.swimlanes.front().id,
                                                                         .position = 0,
                                                                         .opId = "demotion-test-1"})),
                    kanban::Forbidden);

    // Viewer is still >= the Viewer minimum GetBoardState/GetEventsSince
    // require, so reads correctly still succeed at this point -- demoting
    // to Viewer intentionally does not revoke read access, only Member-or-
    // above write actions. This is the control that proves the next
    // assertion below is a real transition, not a pre-existing rejection.
    CHECK_NOTHROW(awaitQt(memberBoard.execute(kanban::GetBoardState{})));

    // Manager now removes member's role entirely (e.g. offboarding, or a
    // stricter demotion than "downgrade to Viewer") -- the README's actual
    // "reads must also be cut off" strain point: with *no* role row left,
    // requireRole(Role::Viewer) must reject even the read-only actions on
    // this same, still-attached instance, not just Member-level writes.
    asManager();
    awaitQt(admin.execute(kanban::RemoveMember{.projectId = project.id, .principal = "member"}));

    // Reads are cut off going forward -- design spec's "nothing detaches
    // them; the *next* execute() re-checks the role" guarantee applies to
    // reads too, not just writes.
    asMember();
    CHECK_THROWS_AS(awaitQt(memberBoard.execute(kanban::GetEventsSince{.lastEventId = {}})), kanban::Forbidden);
    CHECK_THROWS_AS(awaitQt(memberBoard.execute(kanban::GetBoardState{})), kanban::Forbidden);
}
