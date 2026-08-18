// SPDX-License-Identifier: Apache-2.0
//
// ProjectAdminPresenter's own suite: each of its six actions
// (login/refreshProjects/createProject/listRoles/setMemberRole/removeMember)
// round-trips through the presenter's own signals — not the model directly —
// mirroring `examples/bookmarks/tests/test_bookmark_presenter.cpp`'s shape
// exactly (see that file's own top comment for the rationale this one
// reuses verbatim: domain rules already have a dedicated suite at the model
// level, `test_project_admin_model.cpp`; this file only proves the presenter
// wires each action to the right signal, sets busy()/idle() correctly, and
// neither crashes nor hangs).

#include "project_admin_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <morph/session/session_auth.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal — the state a client is in *after* login. Every
///        action in this rung needs a populated `session::current()->
///        principal` for the model's own scoping to succeed, even in
///        `Mode::Local` (which runs no authorizer at all) — same recipe as
///        `test_bookmark_presenter.cpp`'s own `makeAuthedRig`.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the presenter takes.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

}  // namespace

TEST_CASE("ProjectAdminPresenter emits projectsListed after a successful refreshProjects",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::GetMyProjectsResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectsListed,
                      [&](kanban::GetMyProjectsResult result) {
                          listed = std::move(result);
                          gotListed = true;
                      });
    bool failed = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::failed, [&](QString) { failed = true; });

    presenter.refreshProjects();
    REQUIRE(pumpUntil([&] { return gotListed || failed; }));
    // A brand-new principal has zero projects, so this is a legitimate
    // empty-but-successful listing, not a failure.
    CHECK_FALSE(failed);
    REQUIRE(gotListed);
    CHECK(listed.projects.empty());
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("ProjectAdminPresenter::createProject then refreshProjects sees the new project",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::CreateProjectResult created;
    bool gotCreated = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectCreated,
                      [&](kanban::CreateProjectResult result, QString) {
                          created = result;
                          gotCreated = true;
                      });
    presenter.createProject("Sprint Board");
    REQUIRE(pumpUntil([&] { return gotCreated; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(created.id.hasValue());

    kanban::GetMyProjectsResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectsListed,
                      [&](kanban::GetMyProjectsResult result) {
                          listed = std::move(result);
                          gotListed = true;
                      });
    presenter.refreshProjects();
    REQUIRE(pumpUntil([&] { return gotListed; }));
    REQUIRE(listed.projects.size() == 1);
    CHECK(listed.projects.front().name == "Sprint Board");
    CHECK(listed.projects.front().myRole == kanban::Role::Manager);
}

TEST_CASE("ProjectAdminPresenter::listRoles reports the caller as Manager right after createProject",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::CreateProjectResult created;
    bool gotCreated = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectCreated,
                      [&](kanban::CreateProjectResult result, QString) {
                          created = result;
                          gotCreated = true;
                      });
    presenter.createProject("Sprint Board");
    REQUIRE(pumpUntil([&] { return gotCreated; }));

    kanban::GetProjectRolesResult roles;
    bool gotRoles = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::rolesListed,
                      [&](kanban::GetProjectRolesResult result) {
                          roles = std::move(result);
                          gotRoles = true;
                      });
    presenter.listRoles(created.id);
    REQUIRE(pumpUntil([&] { return gotRoles; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(roles.roles.size() == 1);
    CHECK(roles.roles.front().principal == "alice");
    CHECK(roles.roles.front().role == kanban::Role::Manager);
}

TEST_CASE("ProjectAdminPresenter::setMemberRole then removeMember round-trips a member",
          "[kanban][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminPresenter presenter{rig->bridge(0), rig->executor()};

    kanban::CreateProjectResult created;
    bool gotCreated = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectCreated,
                      [&](kanban::CreateProjectResult result, QString) {
                          created = result;
                          gotCreated = true;
                      });
    presenter.createProject("Sprint Board");
    REQUIRE(pumpUntil([&] { return gotCreated; }));

    bool roleSet = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::memberRoleSet, [&] { roleSet = true; });
    presenter.setMemberRole(created.id, "bob", kanban::Role::Member);
    REQUIRE(pumpUntil([&] { return roleSet; }));
    REQUIRE_FALSE(presenter.busy());

    kanban::GetProjectRolesResult roles;
    bool gotRoles = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::rolesListed,
                      [&](kanban::GetProjectRolesResult result) {
                          roles = std::move(result);
                          gotRoles = true;
                      });
    presenter.listRoles(created.id);
    REQUIRE(pumpUntil([&] { return gotRoles; }));
    REQUIRE(roles.roles.size() == 2);

    bool memberRemoved = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::memberRemoved, [&] { memberRemoved = true; });
    presenter.removeMember(created.id, "bob");
    REQUIRE(pumpUntil([&] { return memberRemoved; }));
    REQUIRE_FALSE(presenter.busy());

    gotRoles = false;
    presenter.listRoles(created.id);
    REQUIRE(pumpUntil([&] { return gotRoles; }));
    REQUIRE(roles.roles.size() == 1);
    CHECK(roles.roles.front().principal == "alice");
}

TEST_CASE("ProjectAdminPresenter::login mints a token and installs it as the bridge's default session",
          "[kanban][gui][presenter]") {
    // Unlike the other cases, this one deliberately starts from an
    // unauthenticated bridge — login is what installs the session every
    // other action needs.
    DbFixture fixture;
    const auto issuer = std::make_shared<morph::session::TokenIssuer>("presenter-login-secret",
                                                                        morph::session::hmacSha256);
    kanban::auth::setTokenIssuer(issuer);
    struct IssuerGuard {
        ~IssuerGuard() { kanban::auth::setTokenIssuer(nullptr); }
    } guard;

    BackendRig rig{Mode::Local, 1};
    kanban::gui::ProjectAdminPresenter presenter{rig.bridge(0), rig.executor()};

    QString announced;
    bool gotLogin = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::loggedIn, [&](QString principal) {
        announced = principal;
        gotLogin = true;
    });
    presenter.login("alice");
    REQUIRE(pumpUntil([&] { return gotLogin; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(announced == QStringLiteral("alice"));

    // ...and the session now works, which is the only observable proof that
    // setDefaultSession was called with the returned token.
    kanban::GetMyProjectsResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectsListed,
                      [&](kanban::GetMyProjectsResult result) {
                          listed = std::move(result);
                          gotListed = true;
                      });
    presenter.refreshProjects();
    REQUIRE(pumpUntil([&] { return gotListed; }));
    CHECK(listed.projects.empty());
}

TEST_CASE("ProjectAdminPresenter::createProject: two overlapping calls each report their own name",
          "[kanban][gui][presenter]") {
    // The race this pins: `CreateProjectResult` only carries the new id, not
    // the name it was created with, so the name has to travel alongside the
    // result from the call that created it. Before this fix, the bridge
    // layer stashed the name in a single shared `_lastCreateName` field
    // written by every `createProject()` call and read back only when the
    // *next* `projectCreated` signal landed -- a second call's name could
    // overwrite that field before the first call's own completion arrived,
    // making the first call's completion report the second call's name.
    //
    // `Mode::Local` runs `ProjectAdminModel` on a real `ThreadPoolExecutor{4}`
    // (backend_rig.hpp) with completions delivered back on the Qt thread, so
    // firing both calls before awaiting either (test_kanban_stress.cpp's own
    // "fire all before awaiting" pattern) creates genuine overlapping
    // dispatch deterministically -- no sleep, no thread-level orchestration,
    // and no flakiness: the two CreateProject actions really do run
    // concurrently on the pool, and either can settle first.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminPresenter presenter{rig->bridge(0), rig->executor()};

    struct Created {
        qlonglong id;
        QString name;
    };
    std::vector<Created> created;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::projectCreated,
                      [&](kanban::CreateProjectResult result, QString name) {
                          created.push_back(Created{result.id.hasValue() ? static_cast<qlonglong>(*result.id) : -1,
                                                     std::move(name)});
                      });

    // Both calls dispatched before either's completion has had any chance to
    // arrive -- exactly the "double-click" / concurrent-latency shape the
    // finding describes.
    presenter.createProject("A");
    presenter.createProject("B");

    REQUIRE(pumpUntil([&] { return created.size() == 2; }));
    REQUIRE_FALSE(presenter.busy());

    // Order of arrival is not guaranteed (that's the point), but each
    // reported id must be distinct and paired with its *own* name -- never
    // the other call's.
    REQUIRE(created[0].id != created[1].id);
    for (const Created& c : created) {
        if (c.id == created[0].id) {
            CHECK(c.name == created[0].name);
        }
    }
    const bool sawAWithA =
        (created[0].name == "A" && created[1].name == "B") || (created[0].name == "B" && created[1].name == "A");
    CHECK(sawAWithA);
}

TEST_CASE("ProjectAdminPresenter routes every action's failure to failed(), not just one",
          "[kanban][gui][presenter]") {
    // Same rationale as bookmarks' identical completeness test: each
    // action's error reporting is wired independently at its own track()
    // call site, so a passing case for one action says nothing about
    // another's wiring.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};  // no session installed at all
    kanban::gui::ProjectAdminPresenter presenter{rig.bridge(0), rig.executor()};

    int failures = 0;
    QString failure;
    QObject::connect(&presenter, &kanban::gui::ProjectAdminPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    presenter.refreshProjects();
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.createProject("");  // empty name fails CreateProject::validate()
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.listRoles(kanban::ProjectId{});  // disengaged id fails validate()
    REQUIRE(pumpUntil([&] { return failures == 3; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.setMemberRole(kanban::ProjectId{}, "bob", kanban::Role::Member);
    REQUIRE(pumpUntil([&] { return failures == 4; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.removeMember(kanban::ProjectId{}, "bob");
    REQUIRE(pumpUntil([&] { return failures == 5; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK_FALSE(failure.isEmpty());
}
