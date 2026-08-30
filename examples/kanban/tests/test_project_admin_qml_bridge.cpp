// SPDX-License-Identifier: Apache-2.0
//
// The QML-adapter layer's own suite: `ProjectAdminBridge`
// (`gui_lib/project_admin_qml_bridge.hpp`) — everything that stands between
// `ProjectAdminPresenter` and the QML login/project-list/member-management
// views. Mirrors `examples/bookmarks/tests/test_bookmark_qml_bridges.cpp`'s
// shape and rationale (see that file's own top comment): this is the only
// place a DTO becomes a `QVariantMap`/`QVariantList` property bag, and QML
// binds by *string*, so every assertion below pins a real string a future
// `LoginView.qml`/`ProjectListView.qml`/`MembersView.qml` will bind against.

#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <kanban/auth/kanban_authorizer.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <vector>

#include "project_admin_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal — the state a client is in *after* login. See
///        `test_project_admin_presenter.cpp`'s identical helper.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the adapter takes.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Installs a process-global `TokenIssuer` for a scope and clears it
///        again on the way out — `AuthModel::execute(const Login&)` throws
///        without one. Same shape as
///        `test_bookmark_qml_bridges.cpp`'s `ScopedTokenIssuer`.
class ScopedTokenIssuer {
public:
    explicit ScopedTokenIssuer(std::shared_ptr<morph::session::TokenIssuer> issuer) {
        kanban::auth::setTokenIssuer(std::move(issuer));
    }
    ~ScopedTokenIssuer() { kanban::auth::setTokenIssuer(nullptr); }
    ScopedTokenIssuer(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer& operator=(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer(ScopedTokenIssuer&&) = delete;
    ScopedTokenIssuer& operator=(ScopedTokenIssuer&&) = delete;
};

/// @brief How many methods a class declares itself (signals + `Q_INVOKABLE`s),
///        i.e. excluding everything it inherits from `QObject`.
/// @param meta The class's meta-object.
/// @return The count of own methods.
[[nodiscard]] int ownMethodCount(const QMetaObject* meta) { return meta->methodCount() - meta->methodOffset(); }

}  // namespace

TEST_CASE("ProjectAdminBridge exposes exactly the surface a login/project-list/members view binds against",
          "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};

    const QMetaObject* meta = bridge.metaObject();

    REQUIRE(meta->indexOfProperty("principal") >= 0);
    REQUIRE(meta->indexOfProperty("projects") >= 0);
    REQUIRE(meta->indexOfProperty("roles") >= 0);
    REQUIRE(meta->indexOfProperty("schemasJson") >= 0);
    CHECK(meta->propertyCount() - meta->propertyOffset() == 4);

    // `login` is intentionally absent: it is no longer Q_INVOKABLE, because
    // LoginView.qml submits through the schema renderer and nothing in
    // gui/qml/ calls it any more.
    CHECK(meta->indexOfMethod("login(QString)") < 0);
    REQUIRE(meta->indexOfMethod("submitIfValid(QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("refreshProjects()") >= 0);
    REQUIRE(meta->indexOfMethod("createProject(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("listRoles(qlonglong)") >= 0);
    REQUIRE(meta->indexOfMethod("setMemberRole(qlonglong,QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("removeMember(qlonglong,QString)") >= 0);

    REQUIRE(meta->indexOfSignal("bound()") >= 0);
    REQUIRE(meta->indexOfSignal("loggedIn(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("replyReceived(QString,bool,QString)") >= 0);
    REQUIRE(meta->indexOfSignal("projectsListed(QVariantList)") >= 0);
    REQUIRE(meta->indexOfSignal("projectCreated(qlonglong,QString)") >= 0);
    REQUIRE(meta->indexOfSignal("rolesListed(QVariantList)") >= 0);
    REQUIRE(meta->indexOfSignal("memberRoleSet()") >= 0);
    REQUIRE(meta->indexOfSignal("memberRemoved()") >= 0);
    REQUIRE(meta->indexOfSignal("failed(QString)") >= 0);

    // Nothing else: an adapter method with no binding site is a stub, and one
    // removed from under a binding is a silent runtime gap.
    CHECK(ownMethodCount(meta) == 15);
}

TEST_CASE("ProjectAdminBridge::login installs the returned token and updates the principal property",
          "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    const ScopedTokenIssuer issuer{
        std::make_shared<morph::session::TokenIssuer>("qml-bridge-secret", morph::session::hmacSha256)};
    BackendRig rig{Mode::Local, 1};
    kanban::gui::ProjectAdminBridge bridge{rig.bridge(0), rig.executor()};

    // Before login the bridge carries no session at all, so a domain action
    // is refused — the state a just-launched client is in.
    {
        bool failed = false;
        const auto connection = QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::failed,
                                                 [&](const QString&) { failed = true; });
        bridge.refreshProjects();
        REQUIRE(pumpUntil([&] { return failed; }));
        QObject::disconnect(connection);
    }

    QString announced;
    bool gotLogin = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::loggedIn, [&](const QString& principal) {
        announced = principal;
        gotLogin = true;
    });
    bridge.login(QStringLiteral("alice"));
    REQUIRE(pumpUntil([&] { return gotLogin; }));
    CHECK(announced == QStringLiteral("alice"));
    CHECK(bridge.principal() == QStringLiteral("alice"));

    // ...and the bridge now works, which is the only observable proof that
    // the returned token was actually installed.
    QVariantList rows;
    bool listed = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectsListed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    bridge.refreshProjects();
    REQUIRE(pumpUntil([&] { return listed; }));
    CHECK(rows.isEmpty());
}

TEST_CASE("ProjectAdminBridge::createProject then refreshProjects updates the projects property",
          "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};

    qlonglong createdId = -1;
    QString createdName;
    bool created = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectCreated,
                     [&](qlonglong id, const QString& name) {
                         createdId = id;
                         createdName = name;
                         created = true;
                     });
    bridge.createProject(QStringLiteral("Sprint Board"));
    REQUIRE(pumpUntil([&] { return created; }));
    CHECK(createdId > 0);
    CHECK(createdName == QStringLiteral("Sprint Board"));

    bool listed = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectsListed,
                     [&](const QVariantList&) { listed = true; });
    bridge.refreshProjects();
    REQUIRE(pumpUntil([&] { return listed; }));

    REQUIRE(bridge.projects().size() == 1);
    const QVariantMap row = bridge.projects().front().toMap();
    for (const char* key : {"id", "name", "myRole"}) {
        INFO("missing key: " << key);
        REQUIRE(row.contains(QString::fromLatin1(key)));
    }
    CHECK(row.size() == 3);
    CHECK(row.value(QStringLiteral("id")).toLongLong() == createdId);
    CHECK(row.value(QStringLiteral("name")).toString() == QStringLiteral("Sprint Board"));
    CHECK(row.value(QStringLiteral("myRole")).toString() == QStringLiteral("Manager"));
}

TEST_CASE("ProjectAdminBridge::listRoles/setMemberRole/removeMember round-trip a member, updating the roles property",
          "[kanban][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};

    qlonglong projectId = -1;
    bool created = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectCreated, [&](qlonglong id, const QString&) {
        projectId = id;
        created = true;
    });
    bridge.createProject(QStringLiteral("Sprint Board"));
    REQUIRE(pumpUntil([&] { return created; }));
    REQUIRE(projectId > 0);

    bool roleSet = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::memberRoleSet, [&] { roleSet = true; });
    bridge.setMemberRole(projectId, QStringLiteral("bob"), QStringLiteral("Member"));
    REQUIRE(pumpUntil([&] { return roleSet; }));

    bool rolesListed = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::rolesListed,
                     [&](const QVariantList&) { rolesListed = true; });
    bridge.listRoles(projectId);
    REQUIRE(pumpUntil([&] { return rolesListed; }));
    REQUIRE(bridge.roles().size() == 2);

    bool foundBob = false;
    for (const QVariant& entry : bridge.roles()) {
        const QVariantMap row = entry.toMap();
        for (const char* key : {"principal", "role"}) {
            INFO("missing key: " << key);
            REQUIRE(row.contains(QString::fromLatin1(key)));
        }
        CHECK(row.size() == 2);
        if (row.value(QStringLiteral("principal")).toString() == QStringLiteral("bob")) {
            foundBob = true;
            CHECK(row.value(QStringLiteral("role")).toString() == QStringLiteral("Member"));
        }
    }
    CHECK(foundBob);

    bool removed = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::memberRemoved, [&] { removed = true; });
    bridge.removeMember(projectId, QStringLiteral("bob"));
    REQUIRE(pumpUntil([&] { return removed; }));

    rolesListed = false;
    bridge.listRoles(projectId);
    REQUIRE(pumpUntil([&] { return rolesListed; }));
    REQUIRE(bridge.roles().size() == 1);
    CHECK(bridge.roles().front().toMap().value(QStringLiteral("principal")).toString() == QStringLiteral("alice"));
}

TEST_CASE("ProjectAdminBridge::createProject: two overlapping calls each report their own name",
          "[kanban][gui][qml-bridge]") {
    // Regression pin for the fix: ProjectAdminBridge used to stash the
    // in-flight createProject() name in a single shared `_lastCreateName`
    // field, read back only when the *next* projectCreated signal arrived --
    // a second call issued before the first's completion landed would
    // overwrite that field, so the first call's completion reported the
    // second call's name. The fix threads the name through
    // ProjectAdminPresenter::projectCreated(result, name) instead, captured
    // per-call in that call's own track() continuation, so it can never
    // cross between two in-flight calls. See
    // test_project_admin_presenter.cpp's identical presenter-level case for
    // the full rationale on why Mode::Local's real ThreadPoolExecutor makes
    // this race genuinely (not just theoretically) observable, and
    // deterministic without any sleep.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};

    struct Created {
        qlonglong id;
        QString name;
    };
    std::vector<Created> created;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectCreated,
                     [&](qlonglong id, const QString& name) { created.push_back(Created{id, name}); });

    // Dispatched back-to-back, before either call's completion has any
    // chance to arrive -- a double-click, or real latency under
    // Mode::Remote/Mode::Socket, would create the same overlap.
    bridge.createProject(QStringLiteral("A"));
    bridge.createProject(QStringLiteral("B"));

    REQUIRE(pumpUntil([&] { return created.size() == 2; }));
    REQUIRE(created[0].id != created[1].id);
    REQUIRE(created[0].id > 0);
    REQUIRE(created[1].id > 0);

    const bool sawAWithA = (created[0].name == QStringLiteral("A") && created[1].name == QStringLiteral("B")) ||
                           (created[0].name == QStringLiteral("B") && created[1].name == QStringLiteral("A"));
    CHECK(sawAWithA);
}

TEST_CASE("ProjectAdminBridge::submitIfValid logs in through the schema path and redacts the token",
          "[kanban][gui][qml-bridge][issue344]") {
    // LoginView.qml no longer hand-builds a TextField: `DynamicForm` assembles
    // Login's body from schemaJson<Login>() and calls submitIfValid, so this
    // is the path the flagship GUI actually takes now. Two things must hold on
    // it, and the second is new risk this path introduced.
    DbFixture fixture;
    // A real issuer, not an authed rig: this case drives login itself rather
    // than starting from the post-login state, so `AuthModel::execute(Login)`
    // needs something to mint with.
    const ScopedTokenIssuer issuer{
        std::make_shared<morph::session::TokenIssuer>("schema-path-secret", morph::session::hmacSha256)};
    BackendRig rig{Mode::Local, 1};
    kanban::gui::ProjectAdminBridge bridge{rig.bridge(0), rig.executor()};

    QString replyPayload;
    bool ok = false;
    bool replied = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::replyReceived,
                     [&](const QString& actionType, bool succeeded, const QString& payload) {
                         if (actionType != QStringLiteral("Login")) {
                             return;
                         }
                         replyPayload = payload;
                         ok = succeeded;
                         replied = true;
                     });

    // Exactly the body DynamicForm builds for Login's one string member.
    bridge.submitIfValid(QStringLiteral("Login"), QStringLiteral(R"({"username":"alice"})"));
    REQUIRE(pumpUntil([&] { return replied; }));
    CHECK(ok);

    // 1. It really logged in -- the token was installed, which is only
    //    observable by a subsequent authorized action succeeding.
    CHECK(bridge.principal() == QStringLiteral("alice"));
    QVariantList rows;
    bool listed = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectsListed, [&](const QVariantList& projects) {
        rows = projects;
        listed = true;
    });
    bridge.refreshProjects();
    REQUIRE(pumpUntil([&] { return listed; }));

    // 2. ...and the token did NOT ride out on the signal. replyReceived
    //    broadcasts a result JSON to every bound QML handler, and Login's
    //    result carries the token -- a seam this rung did not have before the
    //    schema path existed. submitForm redacts it; this is what proves it,
    //    rather than the type sweep below, which only sees `QString`.
    const auto session = rig.bridge(0).defaultSession();
    REQUIRE_FALSE(session.token.empty());
    CHECK_FALSE(replyPayload.contains(QString::fromStdString(session.token)));
}

TEST_CASE("ProjectAdminBridge relays failed() and never emits a raw token on any signal",
          "[kanban][gui][qml-bridge]") {
    // The real defect this rung must not reintroduce: bookmarks' own bridge
    // once serialized a live bearer token onto a generic signal before this
    // was caught and fixed.
    //
    // This rung used to have no seam at all -- loggedIn(QString) carries only
    // the principal, and nothing re-serialized LoginResult. It does now:
    // `replyReceived(actionType, ok, payload)` broadcasts a *result JSON*
    // payload to every bound QML handler, and Login's own result carries the
    // token. That is why ProjectAdminPresenter::submitForm redacts the token
    // out of its copy before emitting, and why the next case below asserts
    // the redaction directly rather than trusting the type sweep here.
    //
    // The sweep still earns its place as the structural half: no signal may
    // carry a type that *is* an AuthToken (or a container that could hold
    // one). `bool` joins the allowlist for `replyReceived`'s ok flag on
    // exactly that basis -- it cannot carry a token.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};
    const QMetaObject* meta = bridge.metaObject();

    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Signal) {
            continue;
        }
        for (int p = 0; p < method.parameterCount(); ++p) {
            const auto typeId = method.parameterMetaType(p).id();
            INFO("signal: " << method.methodSignature().toStdString());
            CHECK((typeId == QMetaType::QString || typeId == QMetaType::QVariantList ||
                   typeId == QMetaType::LongLong || typeId == QMetaType::Bool));
        }
    }

    // Failure path itself: a bad projectId still surfaces on failed(), not a
    // crash, and carries no token-shaped content.
    QString message;
    bool failed = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });
    bridge.listRoles(-1);
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(message.isEmpty());
}
