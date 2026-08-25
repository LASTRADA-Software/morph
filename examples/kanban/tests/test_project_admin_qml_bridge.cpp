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
    CHECK(meta->propertyCount() - meta->propertyOffset() == 3);

    REQUIRE(meta->indexOfMethod("login(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("refreshProjects()") >= 0);
    REQUIRE(meta->indexOfMethod("createProject(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("listRoles(qlonglong)") >= 0);
    REQUIRE(meta->indexOfMethod("setMemberRole(qlonglong,QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("removeMember(qlonglong,QString)") >= 0);

    REQUIRE(meta->indexOfSignal("bound()") >= 0);
    REQUIRE(meta->indexOfSignal("loggedIn(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("projectsListed(QVariantList)") >= 0);
    REQUIRE(meta->indexOfSignal("projectCreated(qlonglong,QString)") >= 0);
    REQUIRE(meta->indexOfSignal("rolesListed(QVariantList)") >= 0);
    REQUIRE(meta->indexOfSignal("memberRoleSet()") >= 0);
    REQUIRE(meta->indexOfSignal("memberRemoved()") >= 0);
    REQUIRE(meta->indexOfSignal("failed(QString)") >= 0);

    // Nothing else: an adapter method with no binding site is a stub, and one
    // removed from under a binding is a silent runtime gap.
    CHECK(ownMethodCount(meta) == 14);
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

TEST_CASE("ProjectAdminBridge relays failed() and never emits a raw token on any signal",
          "[kanban][gui][qml-bridge]") {
    // The real defect this rung must not reintroduce: bookmarks' own bridge
    // once serialized a live bearer token onto a generic signal before this
    // was caught and fixed. ProjectAdminBridge never re-serializes
    // LoginResult onto a signal at all -- loggedIn(QString) carries only the
    // principal -- so there is no seam for the token to leak through in the
    // first place. This case pins that: every signal ProjectAdminBridge
    // exposes carries only QString/QVariantList/qlonglong/no-argument
    // payloads, none of which is (or could carry) an AuthToken.
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
            CHECK(
                (typeId == QMetaType::QString || typeId == QMetaType::QVariantList || typeId == QMetaType::LongLong));
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
