# Kanban Rung-4 Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every remaining gap in kanban (rung 4 of the application
ladder) so PR #121 ships a structurally complete rung — a working desktop
GUI, a real client-side offline stack, the missing test coverage the rung's
own Definition of Done calls for, a CI leg that actually runs the concurrent
stress test under ThreadSanitizer, the cascade-journaling decision writeup —
and then implement the two previously-deferred features (automation rules,
task attachments) on top of that completed foundation.

**Architecture:** Seven phases, executed in strict order because each later
phase depends on an earlier one's output (the rules engine needs the
cascade-journaling decision; attachments need the GUI to display them in;
both need `GetMyProjects` and the presenter/bridge scaffolding Phase 1
builds). All phases land in this one branch/PR (`ladder-kanban-impl`,
backing PR #121), each ending in a green build + test run before the next
phase starts.

**Tech Stack:** C++23, Qt 6.5+ (Core, Qml, Quick, WebSockets), Catch2,
Lightweight ORM over SQLite, the morph framework (`morph::offline`,
`morph::session`, `morph::exec`, `morph::journal`).

**Spec:** `examples/kanban/README.md` (rung definition, Definition of Done),
`examples/LADDER.md` (ladder-wide conventions), `examples/IMPLEMENTATION.md`,
`examples/TESTING.md`, `docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`
(backend design record), `docs/superpowers/specs/2026-08-17-kanban-gui-design.md`
(GUI design record — verified current against today's backend surface in
Phase 1, Task 1).

## Global Constraints

- **Models are the application** (`IMPLEMENTATION.md` rule 1): all new logic
  (rules engine, attachment metadata) lives in `kanban::BoardModel` /
  `kanban::ProjectAdminModel` or new sibling models, never in GUI code.
- **Persistence exclusively through the Lightweight ORM** — no raw SQL
  strings; new tables follow `kanban::db::*Record` conventions in
  `examples/kanban/include/kanban/db/kanban_entity.hpp` and
  `examples/kanban/src/db/schema.cpp`'s `LIGHTWEIGHT_SQL_MIGRATION`.
- **Strong types only in DTOs; `std::string` the sole plain type**
  (`IMPLEMENTATION.md`).
- **Models are 100% unit tested** (`IMPLEMENTATION.md`).
- **Every rung's GUI is presenter-shaped and unit tested in both deployment
  modes** (`Local`/`QtWebSocketBackend`) via `examples/common/testkit`
  (`TESTING.md`).
- **No synthesized-mouse-event flows** for drag-and-drop testing
  (`TESTING.md`) — test the bridge method the gesture calls, not the
  gesture itself.
- **Per-rung RBAC enforced inside `execute()`**, never by teaching
  `IAuthorizer` about per-instance ownership (`docs/spec/core/shared_instances.md`).
- **Doxygen**: `docs/CMakeLists.txt`'s `DOCS_SOURCES` scans only
  `include/morph` + `ARCHITECTURE.md` — example code (including this rung's
  GUI and new models) is not Doxygen-gated, but write complete
  `@param`/`@return` comments anyway for consistency with the surrounding
  code (`CLAUDE.md`'s Doxygen rule applies in full to anything under
  `include/morph/`, which Phase 7's attachment side-channel may touch if it
  needs a framework-level HTTP helper — confirm before writing any new
  `include/morph/` file).
- **`CLAUDE.md` docs discipline**: if any phase invalidates a claim in
  `examples/kanban/README.md`, `docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`,
  or `docs/superpowers/specs/2026-08-17-kanban-gui-design.md`, update that
  spec in the same task, not just the code.
- **Present-tense docs only** — no "used to"/"before this fix" framing in
  any comment or spec update.

---

## Phase 1: GUI (`kanban/gui` + `kanban/gui_lib`)

Builds the desktop client per `docs/superpowers/specs/2026-08-17-kanban-gui-design.md`,
which is a complete, unexecuted design — verified accurate against the
current backend in Task 1 below (one gap found and fixed there:
`GetMyProjects` doesn't exist yet).

### Task 1: `GetMyProjects` backend action

**Files:**
- Modify: `examples/kanban/include/kanban/dto/project_dto.hpp`
- Modify: `examples/kanban/include/kanban/models/project_admin_model.hpp`
- Modify: `examples/kanban/src/models/project_admin_model.cpp`
- Test: `examples/kanban/tests/test_project_admin_model.cpp`

**Interfaces:**
- Consumes: `kanban::ProjectId`, `kanban::Role` (`kanban/core/types.hpp`),
  `kanban::db::ProjectRoleRecord` (`project` `BelongsTo<&ProjectRecord::id>`,
  `principal` `SqlAnsiString<64>`, `role` `SqlAnsiString<16>`),
  `kanban::db::ProjectRecord` (has `.name`), `kanban::roleFromString`,
  the free function `requireOwner()` already defined in
  `project_admin_model.cpp`'s anonymous namespace (returns
  `const std::string&`, throws `Forbidden` if no principal).
- Produces: `kanban::GetMyProjects` (empty action struct),
  `kanban::MyProjectSummary{ProjectId id; std::string name; Role myRole;}`,
  `kanban::GetMyProjectsResult{std::vector<MyProjectSummary> projects;}`,
  `ProjectAdminModel::execute(const GetMyProjects&) -> GetMyProjectsResult`.
  Phase 1 Task 3 (`ProjectAdminBridge`) calls this directly.

- [ ] **Step 1: Write the failing test**

Add to `examples/kanban/tests/test_project_admin_model.cpp` (mirror the
file's existing `BackendRig`/fixture setup used by other
`ProjectAdminModel` tests in that file — read the first ~40 lines for the
exact fixture names before writing this):

```cpp
TEST_CASE("GetMyProjects lists every project the caller has a role on, with their own role",
          "[kanban][project_admin]") {
    DbFixture db;
    BackendRig rig{Mode::Local, db.connectionString()};

    auto alice = rig.loginAs("alice");
    auto bob = rig.loginAs("bob");

    // alice creates two projects (Manager on both); bob is added as Viewer
    // on the second only.
    auto p1 = pumpUntilReady(alice.execute<ProjectAdminModel>(CreateProject{.name = "Alpha"}));
    auto p2 = pumpUntilReady(alice.execute<ProjectAdminModel>(CreateProject{.name = "Beta"}));
    pumpUntilReady(alice.execute<ProjectAdminModel>(
        SetMemberRole{.projectId = p2.projectId, .principal = "bob", .role = Role::Viewer}));

    auto aliceProjects = pumpUntilReady(alice.execute<ProjectAdminModel>(GetMyProjects{}));
    REQUIRE(aliceProjects.projects.size() == 2);
    auto findByName = [&](const auto& projects, const std::string& name) {
        return std::ranges::find_if(projects, [&](const auto& p) { return p.name == name; });
    };
    auto aliceAlpha = findByName(aliceProjects.projects, "Alpha");
    REQUIRE(aliceAlpha != aliceProjects.projects.end());
    CHECK(aliceAlpha->myRole == Role::Manager);

    auto bobProjects = pumpUntilReady(bob.execute<ProjectAdminModel>(GetMyProjects{}));
    REQUIRE(bobProjects.projects.size() == 1);
    CHECK(bobProjects.projects.front().name == "Beta");
    CHECK(bobProjects.projects.front().myRole == Role::Viewer);
}

TEST_CASE("GetMyProjects returns an empty list for a principal with no roles",
          "[kanban][project_admin]") {
    DbFixture db;
    BackendRig rig{Mode::Local, db.connectionString()};
    auto carol = rig.loginAs("carol");
    auto result = pumpUntilReady(carol.execute<ProjectAdminModel>(GetMyProjects{}));
    CHECK(result.projects.empty());
}
```

(Adjust the exact `BackendRig`/login/execute helper names to match
whatever `test_project_admin_model.cpp` actually uses today — read the
file first; the shape above follows this rung's established test idiom
but the precise fixture API must be copied verbatim from an existing test
in the same file, not guessed.)

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "GetMyProjects" --output-on-failure`
(substitute your configured preset; see Global Constraints — any preset
with `MORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=kanban` or `all` works)
Expected: FAIL to compile — `GetMyProjects`/`GetMyProjectsResult`/
`MyProjectSummary` are not declared yet.

- [ ] **Step 3: Add the DTO**

In `examples/kanban/include/kanban/dto/project_dto.hpp`, after
`GetProjectRolesResult`:

```cpp
/// @brief Lists every project the calling principal has any role on.
struct GetMyProjects {};

/// @brief One project the caller belongs to, with their own role on it.
struct MyProjectSummary {
    ProjectId id;
    std::string name;
    Role myRole;
};

/// @brief `GetMyProjects`' result: every project the caller has a role on,
///        ordered by project name.
struct GetMyProjectsResult {
    std::vector<MyProjectSummary> projects;
};
```

- [ ] **Step 4: Declare the model method**

In `examples/kanban/include/kanban/models/project_admin_model.hpp`, add
after `GetProjectRolesResult execute(const GetProjectRoles& action);`:

```cpp
    /// @brief Lists every project the calling principal has any role on,
    ///        with their own role, ordered by project name. No project-id
    ///        parameter — the principal comes from `session::current()`.
    GetMyProjectsResult execute(const GetMyProjects& action);
```

And in the same file, after the existing
`BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::GetProjectRoles, "GetProjectRoles", ...)`
line, add:

```cpp
BRIDGE_REGISTER_ACTION(kanban::ProjectAdminModel, kanban::GetMyProjects, "GetMyProjects")
```

- [ ] **Step 5: Implement**

In `examples/kanban/src/models/project_admin_model.cpp`, add near
`ProjectAdminModel::execute(const GetProjectRoles&)`:

```cpp
GetMyProjectsResult ProjectAdminModel::execute(const GetMyProjects&) {
    const auto& principal = requireOwner();

    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto roleRows = mapper->Query<db::ProjectRoleRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::principal>, "=", principal)
                        .All();

    GetMyProjectsResult result;
    result.projects.reserve(roleRows.size());
    for (const auto& roleRow : roleRows) {
        const auto projectDbId = roleRow.project.ReferencedKey();
        auto projectRow = mapper->QuerySingle<db::ProjectRecord>(projectDbId);
        if (!projectRow.has_value()) {
            continue;  // Project deleted underneath a stale role row; skip.
        }
        result.projects.push_back(MyProjectSummary{
            .id = ProjectId{static_cast<std::int64_t>(projectDbId)},
            .name = std::string{projectRow->name.Value().str()},
            .myRole = roleFromString(roleRow.role.Value().str()),
        });
    }
    std::ranges::sort(result.projects, {}, &MyProjectSummary::name);
    return result;
}
```

Read `db::ProjectRecord`'s actual field name/type for `.name` and confirm
the exact accessor for `BelongsTo::ReferencedKey()` and
`mapper->QuerySingle<T>(id)` against `kanban_entity.hpp` and an existing
call site in `project_admin_model.cpp` before finalizing — the shape above
is correct in spirit but the ORM's exact method names must be copied from
working code in the same file, not invented.

- [ ] **Step 6: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "GetMyProjects" --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Update the GUI design spec**

In `docs/superpowers/specs/2026-08-17-kanban-gui-design.md` §3, change the
heading from "New backend action: `GetMyProjects`" framing (which
describes it as work still to do) to note it is implemented — present
tense, per `CLAUDE.md`'s comment discipline. Keep the rest of §3 (it
documents the shape correctly).

- [ ] **Step 8: Commit**

```bash
git add examples/kanban/include/kanban/dto/project_dto.hpp \
        examples/kanban/include/kanban/models/project_admin_model.hpp \
        examples/kanban/src/models/project_admin_model.cpp \
        examples/kanban/tests/test_project_admin_model.cpp \
        docs/superpowers/specs/2026-08-17-kanban-gui-design.md
git commit -m "kanban: add GetMyProjects action for the GUI's project list view"
```

### Task 2: `ProjectAdminPresenter` + `ProjectAdminBridge`

**Files:**
- Create: `examples/kanban/gui_lib/project_admin_presenter.hpp`
- Create: `examples/kanban/gui_lib/project_admin_presenter.cpp`
- Create: `examples/kanban/gui_lib/project_admin_qml_bridge.hpp`
- Create: `examples/kanban/gui_lib/project_admin_qml_bridge.cpp`
- Test: `examples/kanban/tests/test_project_admin_presenter.cpp`
- Test: `examples/kanban/tests/test_project_admin_qml_bridge.cpp`
- Modify: `examples/kanban/CMakeLists.txt`

**Interfaces:**
- Consumes: `kanban::ProjectAdminModel` action surface (Task 1's
  `GetMyProjects` plus existing `CreateProject`/`SetMemberRole`/
  `RemoveMember`/`GetProjectRoles`/`Login`), `morph::client::BridgeHandler<Model>`
  (read `examples/bookmarks/gui_lib/bookmark_qml_bridges.hpp` for its exact
  template parameters and method names before writing), `morph::session::Context`,
  `morph::session::setDefaultSession` (confirm exact free-function name in
  `include/morph/session/session_auth.hpp`).
- Produces: `kanban::gui::ProjectAdminPresenter` (signals: `loggedIn(QString)`,
  `projectsListed(QVariantList)`, `projectCreated(QString id, QString name)`,
  `rolesListed(QVariantList)`, `failed(QString)`), `kanban::gui::ProjectAdminBridge`
  (`Q_OBJECT`, `Q_PROPERTY`s for principal/current project list/current
  roles list as QVariant/QVariantList, `Q_INVOKABLE`s `login(QString)`,
  `refreshProjects()`, `createProject(QString name)`, `setMemberRole(QString
  principal, QString role)`, `removeMember(QString principal)`,
  `listRoles(QString projectId)`). Phase 1 Task 4 (`Main.qml`/
  `ProjectListView.qml`) binds to `ProjectAdminBridge`'s properties/signals.
  Phase 2 (offline wiring) does NOT touch this bridge — offline applies to
  `BoardBridge` only (`MoveTaskPosition` is the only queued action).

- [ ] **Step 1: Read the pattern this mirrors**

Read `examples/bookmarks/gui_lib/bookmark_qml_bridges.hpp` and
`bookmark_qml_bridges.cpp` in full, and
`examples/polls/gui_lib/poll_presenter.cpp`'s `Poller` class (for the
`GetEventsSince`-driven `QTimer` pattern this presenter reuses via
`BoardPresenter` in Task 3, not this one — `ProjectAdminPresenter` has no
polling, it is request/response only). Note the exact `BridgeHandler<T>`
constructor signature, its execute-and-signal method name, and the
`_liveness` (`shared_ptr<const void>`) declared-last convention
(`docs/superpowers/specs/2026-08-16-kanban-rung4-design.md` or
`examples/common/gui/presenter.hpp`'s doc comment explains why it must be
last).

- [ ] **Step 2: Write the failing presenter test**

```cpp
// examples/kanban/tests/test_project_admin_presenter.cpp
#include "kanban/gui_lib/project_admin_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"
#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("ProjectAdminPresenter emits projectsListed after a successful GetMyProjects", "[kanban][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    kanban::gui::ProjectAdminPresenter presenter{rig.bridge()};

    QSignalSpy listedSpy{&presenter, &kanban::gui::ProjectAdminPresenter::projectsListed};
    QSignalSpy failedSpy{&presenter, &kanban::gui::ProjectAdminPresenter::failed};

    presenter.login("alice");
    pumpUntil([&] { return listedSpy.count() > 0 || failedSpy.count() > 0; });

    // A brand-new principal has zero projects, so this is a legitimate
    // empty-but-successful listing, not a failure.
    REQUIRE(failedSpy.isEmpty());
    REQUIRE(listedSpy.count() == 1);
}
```

Do not guess `BackendRig::bridge()`'s exact return type or `pumpUntil`'s
signature — copy them from an existing presenter test in a sibling rung
(`examples/polls/tests/test_poll_presenter.cpp` or
`examples/bookmarks/tests/test_bookmark_presenter.cpp`) verbatim, adjusting
only the model/action names.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ProjectAdminPresenter" --output-on-failure`
Expected: FAIL to compile — `project_admin_presenter.hpp` doesn't exist.

- [ ] **Step 4: Implement `ProjectAdminPresenter`**

Follow `examples/bookmarks/gui_lib/bookmark_qml_bridges.cpp`'s presenter
half exactly, substituting kanban's actions. Header
(`project_admin_presenter.hpp`):

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/models/project_admin_model.hpp"
#include <morph/client/bridge_handler.hpp>  // confirm exact path
#include <QObject>
#include <QVariantList>
#include <memory>

namespace kanban::gui {

/// @brief Drives `kanban::AuthModel`/`kanban::ProjectAdminModel` for the
///        login and project-list/member-management views. No QML
///        dependency — translates action results into Qt signals only.
class ProjectAdminPresenter : public QObject {
    Q_OBJECT
  public:
    explicit ProjectAdminPresenter(std::shared_ptr<morph::client::Bridge> bridge, QObject* parent = nullptr);

    void login(const QString& username);
    void refreshProjects();
    void createProject(const QString& name);
    void listRoles(const QString& projectId);
    void setMemberRole(const QString& projectId, const QString& principal, const QString& role);
    void removeMember(const QString& projectId, const QString& principal);

  signals:
    void loggedIn(QString principal);
    void projectsListed(QVariantList projects);
    void projectCreated(QString id, QString name);
    void rolesListed(QVariantList roles);
    void failed(QString message);

  private:
    morph::client::BridgeHandler<AuthModel> _authHandler;
    morph::client::BridgeHandler<ProjectAdminModel> _projectHandler;
    std::shared_ptr<const void> _liveness = std::make_shared<int>(0);  // must stay last-declared
};

}  // namespace kanban::gui
```

(`morph::client::Bridge`/`BridgeHandler` exact namespace/include path must
be copied from `bookmark_qml_bridges.hpp`'s own includes — do not guess.)

Implementation (`project_admin_presenter.cpp`) wires each method to
execute the matching action via the relevant handler and emit a signal on
success/`failed(QString)` on error — mirror
`BookmarksPresenter::login`/`::listBookmarks`'s exact
`.then(...).onError(...)` shape from `bookmark_qml_bridges.cpp`.

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "ProjectAdminPresenter" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Write the failing bridge test**

```cpp
// examples/kanban/tests/test_project_admin_qml_bridge.cpp
#include "kanban/gui_lib/project_admin_qml_bridge.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("ProjectAdminBridge exposes the expected Q_PROPERTYs and Q_INVOKABLEs", "[kanban][gui]") {
    kanban::gui::ProjectAdminBridge bridge{nullptr /* built with a real Bridge in the real test */};
    const auto* meta = bridge.metaObject();
    CHECK(meta->indexOfProperty("principal") >= 0);
    CHECK(meta->indexOfProperty("projects") >= 0);
    CHECK(meta->indexOfMethod("login(QString)") >= 0);
    CHECK(meta->indexOfMethod("createProject(QString)") >= 0);
}
```

(Expand with behavioral assertions — driving `login()`/`createProject()`
against a `BackendRig` and checking property updates — mirroring
`bookmark_qml_bridges`' own bridge test file structure exactly.)

- [ ] **Step 7: Run test to verify it fails, then implement `ProjectAdminBridge`**

Mirror `bookmark_qml_bridges.hpp`/`.cpp`'s bridge half: `Q_PROPERTY`s
backed by presenter signals, `Q_INVOKABLE`s forwarding to the presenter,
`setDefaultSession` called from the `loggedIn` handler per the GUI design
spec §5 step 3.

- [ ] **Step 8: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ProjectAdmin" --output-on-failure`
Expected: PASS (both presenter and bridge suites).

- [ ] **Step 9: Wire into CMakeLists.txt**

Add `gui_lib/project_admin_presenter.cpp`, `gui_lib/project_admin_qml_bridge.cpp`
and their test files to `examples/kanban/CMakeLists.txt`, following
`examples/bookmarks/CMakeLists.txt`'s exact target/test registration
pattern for its own `gui_lib` sources (`morph_add_rung` machinery — read
`cmake/morph_add_rung.cmake` if the exact hook-in point isn't obvious from
bookmarks' CMakeLists alone).

- [ ] **Step 10: Commit**

```bash
git add examples/kanban/gui_lib/project_admin_presenter.hpp \
        examples/kanban/gui_lib/project_admin_presenter.cpp \
        examples/kanban/gui_lib/project_admin_qml_bridge.hpp \
        examples/kanban/gui_lib/project_admin_qml_bridge.cpp \
        examples/kanban/tests/test_project_admin_presenter.cpp \
        examples/kanban/tests/test_project_admin_qml_bridge.cpp \
        examples/kanban/CMakeLists.txt
git commit -m "kanban: add ProjectAdminPresenter/Bridge for the GUI's login and project-list views"
```

### Task 3: `BoardPresenter` + `BoardBridge`

**Files:**
- Create: `examples/kanban/gui_lib/board_presenter.hpp`
- Create: `examples/kanban/gui_lib/board_presenter.cpp`
- Create: `examples/kanban/gui_lib/board_qml_bridge.hpp`
- Create: `examples/kanban/gui_lib/board_qml_bridge.cpp`
- Test: `examples/kanban/tests/test_board_presenter.cpp`
- Test: `examples/kanban/tests/test_board_qml_bridge.cpp`
- Test: `examples/kanban/tests/test_board_concurrent_drag.cpp`
- Modify: `examples/kanban/CMakeLists.txt`

**Interfaces:**
- Consumes: `kanban::BoardModel`'s full action surface (`OpenBoard`,
  `GetBoardState`, `CreateColumn`, `CreateSwimlane`, `CreateTask`,
  `MoveTaskPosition{taskId, columnId, position, swimlaneId, opId}`,
  `AddComment`, `GetEventsSince`, `GetActivity`), `examples/polls/gui_lib/poll_presenter.cpp`'s
  `Poller` class (the `GetEventsSince`-driven `QTimer` pattern to reuse
  verbatim for `BoardPresenter`'s own poller).
- Produces: `kanban::gui::BoardPresenter` (signals: `boardOpened(QVariantMap)`,
  `taskMoved(QString taskId)`, `commentAdded(QString taskId)`,
  `activityUpdated(QVariantList)`, `failed(QString)`),
  `kanban::gui::BoardBridge` (`Q_PROPERTY`s: `board` (QVariantMap, JSON-shaped
  per design spec §4.3), `activity` (QVariantList), `myRole` (QString);
  `Q_INVOKABLE`s: `openBoard(QString projectId)`, `createColumn(QString name,
  int wipLimit)`, `createSwimlane(QString name)`, `createTask(...)`,
  `moveTask(QString taskId, QString columnId, QString swimlaneId, int
  position)` — **generates `opId` internally via `QUuid::createUuid()`,
  per design spec §6.2 step 4; QML never sees or passes an opId** —
  `addComment(QString taskId, QString body)`). Phase 2 wraps
  `BoardBridge::moveTask` with the offline queue; Phase 1 Task 4 (QML)
  binds to this bridge; `test_board_concurrent_drag.cpp` in this task
  drives `BoardBridge::moveTask()` (not raw `BoardModel`) concurrently,
  mirroring `test_kanban_stress.cpp`'s invariant at the bridge layer.

- [ ] **Step 1: Read the pattern this mirrors**

Read `examples/polls/gui_lib/poll_presenter.cpp`'s `Poller` class in full
(the `GetEventsSince`-driven `QTimer`) and
`examples/bookmarks/gui_lib/bookmark_qml_bridges.cpp`'s bridge structure
again for the JSON-shaped `Q_PROPERTY` convention (how a `GetBoardResult`
becomes a `QVariantMap`).

- [ ] **Step 2: Write the failing presenter test — board open + move**

```cpp
// examples/kanban/tests/test_board_presenter.cpp
#include "kanban/gui_lib/board_presenter.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"
#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("BoardPresenter opens a board and reports a moved task", "[kanban][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    DbFixture db;
    KanbanAuthorizer authorizer;
    BackendRig rig{Mode::Local, db.connectionString(), authorizer};

    // Seed one project with a column and a task via the raw model first
    // (this test is about the presenter's board-open/move path, not
    // project bootstrap — reuse test_board_model.cpp's own seeding helper
    // if one exists, otherwise call ProjectAdminModel/BoardModel::execute
    // directly through rig).
    auto seed = seedOneColumnOneTask(rig);  // see test_board_model.cpp for the equivalent

    kanban::gui::BoardPresenter presenter{rig.bridge()};
    QSignalSpy openedSpy{&presenter, &kanban::gui::BoardPresenter::boardOpened};
    QSignalSpy movedSpy{&presenter, &kanban::gui::BoardPresenter::taskMoved};
    QSignalSpy failedSpy{&presenter, &kanban::gui::BoardPresenter::failed};

    presenter.openBoard(QString::fromStdString(seed.projectId));
    pumpUntil([&] { return openedSpy.count() > 0 || failedSpy.count() > 0; });
    REQUIRE(failedSpy.isEmpty());
    REQUIRE(openedSpy.count() == 1);

    presenter.moveTask(QString::fromStdString(seed.taskId), QString::fromStdString(seed.columnId),
                        QString{}, 0);
    pumpUntil([&] { return movedSpy.count() > 0 || failedSpy.count() > 1; });
    CHECK(movedSpy.count() == 1);
}
```

`seedOneColumnOneTask` is illustrative — read
`examples/kanban/tests/test_board_model.cpp`'s existing seeding helpers
(it already has fixtures for "one project, one column, one task") and
reuse the actual helper name found there instead of inventing a new one.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "BoardPresenter" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 4: Implement `BoardPresenter`**

Mirror `PollPresenter`'s poller wiring plus `BookmarksPresenter`'s
execute-and-signal shape, adapted to `BoardModel`'s action set. The
`moveTask` method takes an already-generated `opId` as a parameter (the
*bridge*, not the presenter, generates it per the design spec — keep this
boundary: presenter is transport-only, bridge owns UI-facing id
generation).

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "BoardPresenter" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Write the failing bridge test**

```cpp
// examples/kanban/tests/test_board_qml_bridge.cpp
#include "kanban/gui_lib/board_qml_bridge.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("BoardBridge exposes the expected surface and moveTask generates a fresh opId per call",
          "[kanban][gui]") {
    // ... construct with a real BackendRig-backed presenter ...
    kanban::gui::BoardBridge bridge{/* ... */};
    const auto* meta = bridge.metaObject();
    CHECK(meta->indexOfProperty("board") >= 0);
    CHECK(meta->indexOfMethod("moveTask(QString,QString,QString,int)") >= 0);

    // Two calls must not reuse the same opId (exactly-once semantics rely
    // on a fresh id per user-initiated move, not per session).
    bridge.moveTask("task1", "col1", "", 0);
    auto firstOpId = bridge.lastOpIdForTest();  // add a test-only accessor, or spy on the presenter call
    bridge.moveTask("task1", "col2", "", 0);
    auto secondOpId = bridge.lastOpIdForTest();
    CHECK(firstOpId != secondOpId);
}
```

- [ ] **Step 7: Implement `BoardBridge`**

`moveTask(QString, QString, QString, int)` generates
`QUuid::createUuid().toString()` and forwards `(taskId, columnId,
swimlaneId, position, opId)` to the presenter.

- [ ] **Step 8: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "Board.*Bridge" --output-on-failure`
Expected: PASS.

- [ ] **Step 9: Write the concurrent-drag bridge test**

```cpp
// examples/kanban/tests/test_board_concurrent_drag.cpp
//
// Mirrors test_kanban_stress.cpp's own invariant, but drives it through
// BoardBridge/BoardPresenter rather than raw BoardModel calls, proving the
// GUI's own code path (opId generation, signal plumbing) doesn't break the
// guarantee the backend already proves at the model level.
#include "kanban/gui_lib/board_qml_bridge.hpp"
// ... N BoardBridge instances (not raw BoardModel) call moveTask()
// concurrently against a shared BackendRig server; after settling, assert
// dense/unique positions per (columnId, swimlaneId) and no task
// duplicated/dropped, reading state back via one bridge's board property.
```

Write this by directly adapting `test_kanban_stress.cpp`'s client-setup and
invariant-check code (read that file in full first — its own header
comment already documents the two API gotchas it hit), substituting raw
`BoardModel::execute(MoveTaskPosition)` calls for `BoardBridge::moveTask()`
calls.

- [ ] **Step 10: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "concurrent_drag" --output-on-failure`
Expected: PASS.

- [ ] **Step 11: Wire into CMakeLists.txt and commit**

```bash
git add examples/kanban/gui_lib/board_presenter.hpp \
        examples/kanban/gui_lib/board_presenter.cpp \
        examples/kanban/gui_lib/board_qml_bridge.hpp \
        examples/kanban/gui_lib/board_qml_bridge.cpp \
        examples/kanban/tests/test_board_presenter.cpp \
        examples/kanban/tests/test_board_qml_bridge.cpp \
        examples/kanban/tests/test_board_concurrent_drag.cpp \
        examples/kanban/CMakeLists.txt
git commit -m "kanban: add BoardPresenter/Bridge, proving the concurrency invariant through the GUI's own code path"
```

### Task 4: QML views + `gui/main.cpp`

**Files:**
- Create: `examples/kanban/gui/main.cpp`
- Create: `examples/kanban/gui/qml/Main.qml`
- Create: `examples/kanban/gui/qml/LoginView.qml`
- Create: `examples/kanban/gui/qml/ProjectListView.qml`
- Create: `examples/kanban/gui/qml/BoardView.qml`
- Create: `examples/kanban/gui/qml/TaskDetailPopup.qml`
- Create: `examples/kanban/gui/qml/MembersView.qml`
- Test: `examples/kanban/tests/test_gui_qml_smoke.cpp`
- Modify: `examples/kanban/CMakeLists.txt`

**Interfaces:**
- Consumes: `kanban::gui::ProjectAdminBridge`, `kanban::gui::BoardBridge`
  (Tasks 2–3), `examples/bookmarks/gui/main.cpp`'s exact bootstrap pattern
  (`--server` flag, `setInitialProperties`, `loadFromModule`),
  `cmake/morph_add_rung.cmake`'s QML module registration (`qt_add_qml_module`).
- Produces: `ladder_kanban_gui` executable (name per
  `morph_add_rung.cmake`'s naming convention — confirm against
  `examples/bookmarks/CMakeLists.txt`'s own `morph_add_rung` call).

- [ ] **Step 1: `gui/main.cpp`**

Copy `examples/bookmarks/gui/main.cpp` verbatim, substituting
`ProjectAdminBridge`/`BoardBridge` for bookmarks' own bridges and
`KanbanAuthorizer` for bookmarks' authorizer, per design spec §4.4.

- [ ] **Step 2: `Main.qml` + `LoginView.qml`**

Mirror `examples/bookmarks/gui/qml/Main.qml`'s `StackView` shell and its
login view exactly (design spec §5 step 5: `StackView` reacts to
`loggedIn` by pushing the project list).

- [ ] **Step 3: `ProjectListView.qml`**

A `ListView` over `projectAdminBridge.projects` (populated by
`GetMyProjects` via Task 2's bridge), a "create project" affordance
calling `createProject(name)`, each row navigating to `BoardView.qml` on
tap (calling `boardBridge.openBoard(projectId)`).

- [ ] **Step 4: `BoardView.qml`**

Per design spec §6.1–6.2: swimlane sections (only rendered as distinct
chrome when more than one swimlane exists), horizontal `ListView` of
columns, vertical `ListView` of task cards per column, native `Drag`
attached property + `DropArea` for drag-and-drop (no custom mouse
tracking), WIP-limit header text and drop-target highlight color per §6.2
step 2.

- [ ] **Step 5: `TaskDetailPopup.qml` + `MembersView.qml`**

Per design spec §7 (comments/activity popup) and §8 (members list with
role `ComboBox` + remove button).

- [ ] **Step 6: Write the smoke test**

```cpp
// examples/kanban/tests/test_gui_qml_smoke.cpp
// Loads Main via QQmlApplicationEngine under the offscreen platform,
// asserts non-empty rootObjects() and zero QML warnings -- mirrors every
// sibling rung's own gui_qml_smoke test verbatim (e.g.
// examples/bookmarks/tests/test_gui_qml_smoke.cpp).
```

Copy the sibling rung's smoke test file structure exactly, substituting
the module URI (`Kanban`, matching whatever `morph_add_rung.cmake` derives
from `examples/kanban/CMakeLists.txt`'s `morph_add_rung()` call).

- [ ] **Step 7: Run all Phase 1 tests**

Run: `ctest --preset cl-qt-debug -L kanban --output-on-failure`
Expected: PASS (every test added in Tasks 1–4).

- [ ] **Step 8: Wire into CMakeLists.txt and commit**

```bash
git add examples/kanban/gui/ examples/kanban/tests/test_gui_qml_smoke.cpp examples/kanban/CMakeLists.txt
git commit -m "kanban: add the desktop GUI (login, project list, board, members) per the GUI design spec"
```

---

## Phase 2: Client-side offline stack

Wires `SqliteOfflineQueue`, `NetworkMonitor`, `SyncWorker`, and
`ReconnectCoordinator` into `BoardBridge`'s `moveTask` path, so drag moves
made while offline actually queue and replay — closing the gap the audit
found (today these classes are only exercised directly against
`BoardModel` in backend tests, never through a real client queue).

### Task 5: Wire the offline queue into `BoardBridge`

**Files:**
- Modify: `examples/kanban/gui_lib/board_qml_bridge.hpp`
- Modify: `examples/kanban/gui_lib/board_qml_bridge.cpp`
- Modify: `examples/kanban/gui/main.cpp`
- Modify: `examples/kanban/CMakeLists.txt` (link `morph::offline_sqlite`,
  gated on `MORPH_BUILD_OFFLINE_SQLITE`)
- Test: `examples/kanban/tests/test_board_offline_bridge.cpp`

**Interfaces:**
- Consumes: `morph::offline::SqliteOfflineQueue` (constructor takes a
  connection string per `include/morph/offline/sqlite_offline_queue.hpp` —
  read it for the exact signature), `morph::offline::SyncWorker{IOfflineQueue&,
  ReplayFunction, DeadLetterSink}` (`include/morph/offline/sync_worker.hpp`,
  already read in full — `ReplayFunction = std::function<bool(const
  std::string& payload)>`, `DeadLetterSink = std::function<void(const
  QueueItem&)>`), `morph::offline::NetworkMonitor` (constructor/config —
  read `include/morph/offline/network_monitor.hpp` for its exact
  `Deps`/`Config` shape, same pattern as `ReconnectCoordinator`'s already-read
  `Deps`), `morph::offline::ReconnectCoordinator{Deps, Config}` (already
  read in full above: `Deps{tryReconnect, activatePrimary, activateLocal,
  bindContext, replay, shouldContinue, sleep}`, `onOnline()`/`onOffline()`).
- Produces: `BoardBridge::moveTask` now enqueues to
  `SqliteOfflineQueue` (serializing `MoveTaskPosition` — including its
  `opId` — as the queue item's `payload`) instead of calling
  `BoardPresenter::moveTask` directly whenever `NetworkMonitor` reports
  offline; `BoardBridge` gains a new signal `syncStatusChanged(int
  queueDepth, int deadLettered)` that Phase 1's GUI (Task 6 below) surfaces.

- [ ] **Step 1: Write the failing test — enqueue while offline, replay on reconnect**

```cpp
// examples/kanban/tests/test_board_offline_bridge.cpp
//
// Drives BoardBridge (not raw BoardModel/SyncWorker) through: online move
// (goes straight through), forced-offline move (queues instead), then
// simulated reconnect (SyncWorker drains the queue and the move actually
// lands). This is the "queued moves replay on reconnect" DoD bullet,
// proven through the bridge's own code path -- the layer the earlier
// audit found untested.
#include "kanban/gui_lib/board_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("BoardBridge queues a move made while offline and replays it on reconnect",
          "[kanban][gui][offline]") {
    // ... construct BoardBridge with a real SqliteOfflineQueue (temp file
    // path per test, cleaned up in a fixture destructor) and a
    // NetworkMonitor test double forced into the offline state ...

    bridge.moveTask("task1", "col2", "", 0);
    // Assert: no MoveTaskPosition reached the server yet (BackendRig's own
    // call-count hook or journal read), and SqliteOfflineQueue::size() == 1.

    // ... flip the NetworkMonitor double to online, trigger
    // ReconnectCoordinator::onOnline() ...
    pumpUntil([&] { return /* queue drained */ true; });
    // Assert: the server's board state now reflects the move, and the
    // queue is empty.
}
```

Read `test_kanban_offline.cpp`'s existing fault-injection/reconnect tests
first (already read above) for the exact `BackendRig`/`FaultProxy`
call-count-assertion idiom to reuse here at the bridge layer instead of
inventing a new assertion mechanism.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "offline_bridge" --output-on-failure`
Expected: FAIL — `BoardBridge` has no offline-queue awareness yet.

- [ ] **Step 3: Implement the wiring**

In `BoardBridge`, add (gated `#ifdef MORPH_BUILD_OFFLINE_SQLITE`, matching
how `examples/kanban/src/models/board_model.cpp` or similar already gates
optional-dependency code, if any precedent exists — otherwise gate at the
CMakeLists.txt level only, compiling this file only when
`MORPH_BUILD_OFFLINE_SQLITE` is on, following whichever pattern
`examples/kanban/CMakeLists.txt` already uses for other optional
dependencies):

```cpp
class BoardBridge : public QObject {
    // ... existing members ...
  private:
    std::unique_ptr<morph::offline::SqliteOfflineQueue> _offlineQueue;
    std::unique_ptr<morph::offline::NetworkMonitor> _networkMonitor;
    std::unique_ptr<morph::offline::SyncWorker> _syncWorker;
    std::unique_ptr<morph::offline::ReconnectCoordinator> _reconnectCoordinator;
};
```

`moveTask` becomes:

```cpp
void BoardBridge::moveTask(const QString& taskId, const QString& columnId,
                            const QString& swimlaneId, int position) {
    const auto opId = QUuid::createUuid().toString();
    MoveTaskPosition action{
        .taskId = taskId.toStdString(),
        .columnId = columnId.toStdString(),
        .swimlaneId = swimlaneId.toStdString(),
        .position = position,
        .opId = opId.toStdString(),
    };
    if (_networkMonitor->isOnline()) {  // confirm exact accessor name against network_monitor.hpp
        _presenter->moveTask(action);
    } else {
        _offlineQueue->enqueue(serializeMoveTaskPosition(action));  // write this small (de)serializer
        emit syncStatusChanged(static_cast<int>(_offlineQueue->size()), 0);
    }
}
```

Write `serializeMoveTaskPosition`/`deserializeMoveTaskPosition` as small
free functions in `board_qml_bridge.cpp` (JSON via the same library
already used for `Q_PROPERTY` board-state serialization in Task 3 — reuse
it, don't add a second JSON dependency). The `SyncWorker`'s
`ReplayFunction` deserializes the payload and calls
`_presenter->moveTask(action)` synchronously (wrapped to return
`bool`/throw per `SyncWorker`'s documented contract), and the
`DeadLetterSink` emits `syncStatusChanged` with an incremented
dead-lettered count.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "offline_bridge" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Update the GUI design spec's out-of-scope claim**

`docs/superpowers/specs/2026-08-17-kanban-gui-design.md` §1 and §11
currently say the offline stack is out of scope for the GUI. Update both
(present tense, per `CLAUDE.md`) to state the offline stack is now wired,
summarizing the mechanism (a `SqliteOfflineQueue`-backed `BoardBridge`,
`NetworkMonitor`/`ReconnectCoordinator`-driven), and remove the "no 'N
changes pending sync' indicator" claim (Task 6 below adds exactly that).

- [ ] **Step 6: Commit**

```bash
git add examples/kanban/gui_lib/board_qml_bridge.hpp \
        examples/kanban/gui_lib/board_qml_bridge.cpp \
        examples/kanban/gui/main.cpp \
        examples/kanban/CMakeLists.txt \
        examples/kanban/tests/test_board_offline_bridge.cpp \
        docs/superpowers/specs/2026-08-17-kanban-gui-design.md
git commit -m "kanban: wire SqliteOfflineQueue/NetworkMonitor/SyncWorker/ReconnectCoordinator into BoardBridge"
```

### Task 6: Dead-letter + sync-status GUI indicator

**Files:**
- Modify: `examples/kanban/gui/qml/BoardView.qml`
- Modify: `examples/kanban/gui_lib/board_qml_bridge.hpp` (already has
  `syncStatusChanged` from Task 5 — add `Q_PROPERTY int queueDepth`,
  `Q_PROPERTY int deadLetterCount`)
- Modify: `examples/kanban/gui_lib/board_qml_bridge.cpp`
- Test: `examples/kanban/tests/test_board_qml_bridge.cpp` (extend)

**Interfaces:**
- Consumes: Task 5's `syncStatusChanged(int, int)` signal.
- Produces: `BoardBridge::queueDepth()`/`deadLetterCount()` `Q_PROPERTY`
  getters QML binds to; `BoardView.qml` shows "N changes could not be
  synced" (per README's exact required wording) whenever
  `deadLetterCount > 0`.

- [ ] **Step 1: Write the failing test**

```cpp
// extend test_board_qml_bridge.cpp
TEST_CASE("BoardBridge's deadLetterCount property reflects dead-lettered moves", "[kanban][gui][offline]") {
    // ... force 5 consecutive replay failures against a queued move (mirror
    // test_kanban_offline.cpp's own 5-cumulative-attempt setup, adapted to
    // drive it through the queue rather than SyncWorker directly) ...
    CHECK(bridge.deadLetterCount() == 1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "deadLetterCount" --output-on-failure`
Expected: FAIL — property doesn't exist yet.

- [ ] **Step 3: Add the properties**

```cpp
// board_qml_bridge.hpp
Q_PROPERTY(int queueDepth READ queueDepth NOTIFY syncStatusChanged)
Q_PROPERTY(int deadLetterCount READ deadLetterCount NOTIFY syncStatusChanged)
// ...
int queueDepth() const { return _queueDepth; }
int deadLetterCount() const { return _deadLetterCount; }
```

Update the `DeadLetterSink` from Task 5 to increment `_deadLetterCount`
and emit `syncStatusChanged`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "deadLetterCount" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Add the QML indicator**

In `BoardView.qml`, a small banner/label bound to
`boardBridge.deadLetterCount > 0`, text:
`"%1 changes could not be synced".arg(boardBridge.deadLetterCount)` — the
exact wording `examples/kanban/README.md`'s DoD names.

- [ ] **Step 6: Commit**

```bash
git add examples/kanban/gui_lib/board_qml_bridge.hpp \
        examples/kanban/gui_lib/board_qml_bridge.cpp \
        examples/kanban/gui/qml/BoardView.qml \
        examples/kanban/tests/test_board_qml_bridge.cpp
git commit -m "kanban: surface dead-lettered offline moves in the GUI per the rung's DoD"
```

---

## Phase 3: Missing test coverage

Three tests the audit found absent, each testing an already-implemented
backend invariant through a new lens — no production code changes
expected in this phase except if a test finds a genuine bug, in which case
stop and fix it before continuing (per `superpowers:test-driven-development`,
a red test here is diagnostic, not just a checkbox).

### Task 7: Two-client interleaved offline-queue replay

**Files:**
- Test: `examples/kanban/tests/test_kanban_offline.cpp` (extend)

**Interfaces:**
- Consumes: `kanban::BoardModel::execute(MoveTaskPosition)`, the existing
  `OfflineRig`/raw `QTcpServer`-reservation harness `test_kanban_offline.cpp`
  already builds for its single-client reconnect test (read that test's
  setup in full — already read above — and reuse its server/client stack
  construction verbatim for a *second* client).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("Two clients' offline queues replaying interleaved converge on a valid board",
          "[kanban][offline]") {
    // Build the same minimal revivable-port server/client stack
    // test_kanban_offline.cpp's single-reconnect test already uses, but for
    // two independent clients (two BoardModel-driving connections, each with
    // its own queued MoveTaskPosition actions accumulated while
    // disconnected).
    //
    // Both clients go offline, each queues 3-4 distinct MoveTaskPosition
    // actions (different taskIds/destinations) against the same shared
    // board. Reconnect both, replay both queues in an interleaved order
    // (alternate draining one item from each queue rather than draining
    // client A fully then client B).
    //
    // Assert the board invariant test_kanban_stress.cpp already checks:
    // positions dense and unique within every (columnId, swimlaneId), every
    // task present exactly once, no task lost or duplicated -- NOT any
    // specific final ordering (README's own wording for this scenario).
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "interleaved" --output-on-failure`
Expected: FAIL to compile (test doesn't exist).

- [ ] **Step 3: Implement and run**

Write the test body per Step 1's sketch, reusing
`test_kanban_stress.cpp`'s invariant-check helper if one is
extractable/shared, otherwise duplicating the dense/unique-position
assertion inline (small enough that a shared helper isn't obviously worth
the coupling — use judgment when writing this, per the "DRY" principle
this skill also holds, but don't force an extraction that adds coupling
between two independently-owned test files for a ~10-line assertion).

Run: `ctest --preset cl-debug -R "interleaved" --output-on-failure`
Expected: PASS. If it fails on a genuine invariant violation (not a test
bug), STOP — this is a real concurrency bug in `BoardModel`, not a test
gap, and must be fixed (likely in `MoveTaskPosition`'s position-renumbering
logic) before continuing to Task 8.

- [ ] **Step 4: Commit**

```bash
git add examples/kanban/tests/test_kanban_offline.cpp
git commit -m "kanban: test two clients' offline queues replaying interleaved converge on a valid board"
```

### Task 8: Permission revocation while attached

**Files:**
- Test: `examples/kanban/tests/test_shared_instance_lifecycle.cpp` (extend)

**Interfaces:**
- Consumes: `kanban::ProjectAdminModel::execute(SetMemberRole)`,
  `kanban::BoardModel::requireRole`/`requireRoleOn` (already gates every
  `execute()` call per-invocation, per README step 4 — this test proves
  the mechanism actually behaves correctly for the demotion-mid-session
  scenario, not that it needs new code), `kanban::BoardModel::execute(GetEventsSince)`
  and `execute(GetBoardState)` (the "reads must also be cut off" half).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("A member demoted mid-session has their next move rejected and reads cut off",
          "[kanban][auth]") {
    DbFixture db;
    KanbanAuthorizer authorizer;
    BackendRig rig{Mode::Local, db.connectionString(), authorizer};

    auto manager = rig.loginAs("manager");
    auto member = rig.loginAs("member");

    auto project = pumpUntilReady(manager.execute<ProjectAdminModel>(CreateProject{.name = "Demo"}));
    pumpUntilReady(manager.execute<ProjectAdminModel>(
        SetMemberRole{.projectId = project.projectId, .principal = "member", .role = Role::Member}));

    // member attaches and can read/act while still a Member.
    auto board = pumpUntilReady(member.execute<BoardModel>(OpenBoard{.projectId = project.projectId}));
    auto column = pumpUntilReady(manager.execute<BoardModel>(
        CreateColumn{.projectId = project.projectId, .name = "Todo", .wipLimit = 0}));
    auto task = pumpUntilReady(manager.execute<BoardModel>(
        CreateTask{.projectId = project.projectId, .columnId = column.columnId, .title = "T1"}));

    // Manager demotes member to Viewer mid-session (member's attached
    // BoardModel instance is never detached -- this is the point of the
    // test: authorization is per-execute, per docs/spec/core/shared_instances.md).
    pumpUntilReady(manager.execute<ProjectAdminModel>(
        SetMemberRole{.projectId = project.projectId, .principal = "member", .role = Role::Viewer}));

    // Next write from member (still Viewer-or-above-required action) is rejected.
    CHECK_THROWS_AS(
        pumpUntilReady(member.execute<BoardModel>(MoveTaskPosition{
            .taskId = task.taskId, .columnId = column.columnId, .swimlaneId = "", .position = 0,
            .opId = "demotion-test-1"})),
        Forbidden);

    // Reads are also cut off going forward, per README's explicit requirement
    // ("nothing detaches them ... unless the authorizer distinguishes reads" --
    // this asserts KanbanAuthorizer/BoardModel actually does).
    CHECK_THROWS_AS(pumpUntilReady(member.execute<BoardModel>(GetEventsSince{.projectId = project.projectId, .sinceEventId = 0})),
                    Forbidden);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "demoted" --output-on-failure`
Expected: Compiles; may PASS immediately if `requireRole` already covers
this (likely, since it's per-execute per the design), or FAIL if reads
specifically aren't cut off (the audit flagged this as unverified, not
necessarily broken).

- [ ] **Step 3: If it fails, fix the gap**

If `GetEventsSince`/`GetBoardState` don't call `requireRole` at all today
(read `board_model.cpp`'s actual implementations of both to check), add
the missing `requireRole(Role::Viewer)` call at the top of each — mirror
the exact call already present in `MoveTaskPosition`'s handler.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "demoted" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/tests/test_shared_instance_lifecycle.cpp
# if board_model.cpp/hpp changed:
git add examples/kanban/src/models/board_model.cpp examples/kanban/include/kanban/models/board_model.hpp
git commit -m "kanban: test and (if needed) enforce that a demoted member's reads are cut off, not just writes"
```

### Task 9: SQLite contention test under WAL mode

**Files:**
- Test: `examples/kanban/tests/test_kanban_offline.cpp` (extend)

**Interfaces:**
- Consumes: the existing 32-thread contention test at
  `test_kanban_offline.cpp:436-639` (already read above) and its
  `ScopedShortBusyTimeout`/`drainPoolIdleMappers()` helpers.

- [ ] **Step 1: Write the failing test**

Duplicate the existing rollback-journal contention `TEST_CASE` as a new
`TEST_CASE` with `PRAGMA journal_mode=WAL` set on the connection (via
whatever connection-string/pragma mechanism `DbFixture` or the mapper pool
already exposes — check `examples/kanban/tests/test_kanban_offline.cpp`'s
existing fixture setup and `Lightweight`'s own WAL-enabling call, if any
sibling rung already sets WAL anywhere in the tree — `grep -rn "journal_mode"
examples/` first):

```cpp
TEST_CASE("32 boards writing concurrently under SQLite contention (WAL mode): no timeout-then-committed double-apply",
          "[kanban][offline][contention]") {
    // Identical structure to the existing rollback-journal test at line 436,
    // with journal_mode=WAL set immediately after connect (mirror wherever
    // the existing test sets its busy_timeout pragma -- same connection
    // setup point).
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "WAL" --output-on-failure`
Expected: FAIL to compile initially (test doesn't exist); once written,
may reveal WAL mode changes the failure/success mix materially (WAL
allows concurrent readers during a writer, so contention shape differs) —
this is expected and fine, the assertions (no timeout-then-committed
double-apply, dense/unique positions) must still hold regardless of the
mix.

- [ ] **Step 3: Run and verify no double-apply under WAL**

Run: `ctest --preset cl-debug -R "WAL" --output-on-failure`
Expected: PASS. If it reveals an actual double-apply under WAL
specifically, STOP — this is a real gap in the applied-ops ledger's
transaction boundary under WAL's different locking semantics, not a test
bug, and must be fixed before continuing.

- [ ] **Step 4: Update README.md's claim**

`examples/kanban/README.md`'s contention bullet already says "measure
throughput collapse; assert no timeout-then-committed double-apply" for
"WAL on and off" — no wording change needed there (it already promises
both); this task fulfills the promise rather than needing to update text.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/tests/test_kanban_offline.cpp
git commit -m "kanban: extend the SQLite contention test to WAL mode, per the rung's DoD"
```

---

## Phase 4: TSan CI leg

Gets `test_kanban_stress.cpp`'s `[tsan]`-tagged test actually running under
ThreadSanitizer in CI — today it is excluded from every job that builds
kanban (`ladder-tests` excludes `-LE stress`; `linux-sanitizers`'
`clang-tsan` leg never builds the ladder at all).

### Task 10: Add a minimal non-Qt TSan leg for kanban's stress test

**Files:**
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: the existing `linux-sanitizers` job's `clang-tsan` matrix
  entry, `examples/kanban/tests/test_kanban_stress.cpp`'s `Mode::Local`
  design (already confirmed: runs on `ThreadPoolExecutor{4}`, no Qt/GUI
  involvement at all in this specific test — the "GUI stack under TSan is
  mostly noise" rationale that excludes the *whole ladder* from TSan today
  does not actually apply to this one test, since it never touches Qt).

- [ ] **Step 1: Add a new job (not modify the existing sanitizer matrix)**

Add a new job to `.github/workflows/ci.yml`, sibling to `linux-sanitizers`,
scoped tightly to avoid dragging the whole ladder (and its Qt dependency)
into the sanitizer matrix:

```yaml
  # ── Linux: kanban's concurrent-move stress test under ThreadSanitizer ──
  # test_kanban_stress.cpp's [tsan]-tagged TEST_CASE runs entirely on
  # Mode::Local's ThreadPoolExecutor{4} with no Qt/GUI involvement (see the
  # test file's own header comment), so the "a GUI stack under TSan is
  # mostly noise" rationale that keeps the ladder out of linux-sanitizers
  # does not apply to this one test. This job builds only what that test
  # needs -- MORPH_BUILD_LADDER=ON, MORPH_LADDER_RUNGS=kanban, no Qt GUI
  # modules beyond the WebSockets backend the ladder testkit itself
  # requires -- to keep it a minimal, fast, TSan-clean addition rather than
  # pulling every rung's Qt Quick/QML code into the sanitizer matrix.
  kanban-tsan:
    name: Kanban / ThreadSanitizer
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install Clang ${{ env.CLANG_VERSION }} from apt.llvm.org
        run: |
          sudo apt-get update -q
          sudo apt-get install -y ninja-build catch2 libsqlite3-dev unixodbc-dev libsqliteodbc libyaml-cpp-dev libzip-dev
          wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- ${{ env.CLANG_VERSION }}

      - name: Install Qt ${{ env.QT_VERSION }}
        uses: jurplel/install-qt-action@v4
        with:
          version: ${{ env.QT_VERSION }}
          modules: qtwebsockets
          cache: true

      - name: Cache sccache
        uses: actions/cache@v4
        with:
          path: /home/runner/.cache/sccache
          key: sccache-kanban-tsan-${{ github.sha }}
          restore-keys: sccache-kanban-tsan-

      - name: Install sccache
        run: |
          curl -sSL https://github.com/mozilla/sccache/releases/download/v0.9.1/sccache-v0.9.1-x86_64-unknown-linux-musl.tar.gz \
            | tar -xz --strip-components=1 -C /usr/local/bin sccache-v0.9.1-x86_64-unknown-linux-musl/sccache

      - name: Configure (clang-tsan, kanban only)
        run: |
          cmake --preset clang-tsan \
            -DMORPH_BUILD_QT=ON \
            -DMORPH_BUILD_LADDER=ON \
            -DMORPH_LADDER_RUNGS=kanban \
            -DCMAKE_C_COMPILER=clang-${{ env.CLANG_VERSION }} \
            -DCMAKE_CXX_COMPILER=clang++-${{ env.CLANG_VERSION }} \
            -DCMAKE_C_COMPILER_LAUNCHER=sccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=sccache

      - name: Build
        env:
          QT_QPA_PLATFORM: offscreen
        run: cmake --build --preset clang-tsan

      - name: Test (kanban's TSan-tagged stress test only)
        env:
          QT_QPA_PLATFORM: offscreen
        run: ctest --preset clang-tsan -L tsan --output-on-failure
```

Confirm `MORPH_LADDER_RUNGS=kanban` (a single rung, not "all") is
genuinely supported by `cmake/`'s rung-selection machinery (read
`CMakeLists.txt`'s `MORPH_LADDER_RUNGS` handling, already partly seen
above at line 43, and the loop that consumes it) before assuming this
value works — if only "all" or a full semicolon-separated list is
supported, list every prerequisite rung kanban's CMakeLists.txt
`add_subdirectory`-depends on explicitly instead.

- [ ] **Step 2: Push and verify the new job runs and is green**

Push the branch; check the Actions run for the new `Kanban /
ThreadSanitizer` job specifically. It must build only kanban (+
prerequisites) and complete faster than the full `linux-sanitizers` matrix
entries.

Expected: the `[kanban][stress][tsan]`-tagged test runs and passes under
real ThreadSanitizer instrumentation. If TSan reports a genuine data race,
STOP — this is a real concurrency bug the test was specifically written to
catch, not a CI-config problem, and must be fixed before continuing.

- [ ] **Step 3: Update LADDER.md / TESTING.md's claim if it changes their wording**

`examples/TESTING.md` describes the kanban-specific TSan note (per
`LADDER.md`'s own reference to it) — read it and update if it currently
implies no CI leg runs this (present tense, per `CLAUDE.md`).

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml examples/TESTING.md  # if TESTING.md needed an update
git commit -m "ci: add a minimal ThreadSanitizer leg that actually runs kanban's concurrent-move stress test"
```

---

## Phase 5: Cascade-journaling decision + divergence test

Writes the decision the rung's own Definition of Done calls for, silently
dropped when automation rules (step 6) were deferred. This decision gates
Phase 6's rules-engine design, so it must land first.

### Task 11: Write the decision

**Files:**
- Modify: `examples/kanban/README.md`
- Modify: `docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`

**Interfaces:** None (a design decision, not code) — but Phase 6's Task 12
consumes whichever option is chosen here.

- [ ] **Step 1: Decide**

`examples/kanban/README.md`'s build-order step 6 names exactly two
options — pick one and record it in both files (present tense, no
"we considered"/"previously" framing):

- **Option A**: journal cascades with a causal parent-id, suppress rule
  evaluation during replay.
- **Option B**: don't journal cascades, require rule determinism (accepted
  cost: breaks when rules are edited after the fact — see `ledger`'s
  rule-versioning note in `LADDER.md`).

Recommendation for this task (stated as a recommendation, not a mandate —
confirm with the user or a reviewer before locking it in, since it's a
genuine design fork, not a mechanical choice): **Option A**. Kanban's
journal is already positioned as an audit/activity-stream source (Phase 1
consumes it for `GetActivity`), and `LADDER.md`'s own "Journal honesty"
section already establishes that `morph::journal` has no causal-parent-id
concept today — adding one here is a small, well-scoped framework addition
that directly serves the activity stream too (a cascaded rule-fired
mutation should visibly say "caused by task move X" in the activity feed,
which Option B cannot offer at all). Option B's determinism requirement
also conflicts with the rules engine being user-editable at runtime
(Phase 6's whole premise), which is a second reason to prefer A here
specifically (this is a project-specific reason, not a general framework
argument — Option B is the right choice in other frameworks/rungs where
rule edits aren't expected).

- [ ] **Step 2: Update `examples/kanban/README.md`**

Replace the "Review sharpened the decision..." paragraph in build-order
step 6 with the chosen option stated as current design, plus a one-line
pointer to where the divergence test lives (Task 12).

- [ ] **Step 3: Update the backend design spec**

`docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`'s §9 (or
wherever it lists this as out-of-scope, confirmed at line 531-536 above)
needs updating: this decision is now in scope and made; the rules engine
itself (Phase 6) is what's newly in scope, not "automation rules" as a
whole category anymore.

- [ ] **Step 4: Commit**

```bash
git add examples/kanban/README.md docs/superpowers/specs/2026-08-16-kanban-rung4-design.md
git commit -m "kanban: record the cascade-journaling decision (causal parent-id, suppress rule eval during replay)"
```

### Task 12: Divergence test

**Files:**
- Test: `examples/kanban/tests/test_board_model.cpp` (extend) — or a new
  `test_kanban_cascade_journal.cpp` if the assertion doesn't fit naturally
  alongside existing `BoardModel` tests (judgment call at write time based
  on how large the addition turns out to be).

**Interfaces:**
- Consumes: whatever causal-parent-id mechanism Task 11 selected — this
  test is written against Option A's shape below; if a different option
  was chosen in Task 11, rewrite this test's body to match, keeping the
  same intent (prove replay does not double-fire a cascade).

- [ ] **Step 1: Write the failing test (proves the divergence Option A avoids)**

This test necessarily depends on Phase 6's rules engine existing to
trigger a real cascade — so this task's test is a **placeholder-free
skeleton with a `SKIP` mark removed once Phase 6 lands**, not deferred
silently. Write it now against `BoardModel`'s journal-replay entry point
directly (no rule needed yet — simulate a "cascade" by manually appending
a second journal entry with a causal-parent-id pointing at the first, then
replaying):

```cpp
TEST_CASE("Replaying a cascaded journal entry does not re-fire the cascade",
          "[kanban][journal]") {
    DbFixture db;
    KanbanAuthorizer authorizer;
    BackendRig rig{Mode::Local, db.connectionString(), authorizer};
    auto owner = rig.loginAs("owner");
    auto project = pumpUntilReady(owner.execute<ProjectAdminModel>(CreateProject{.name = "Demo"}));

    // Simulate a trigger action and its cascaded mutation, linked by a
    // causal parent-id, exactly as Phase 6's rules engine will produce for
    // real once it exists.
    auto column = pumpUntilReady(owner.execute<BoardModel>(
        CreateColumn{.projectId = project.projectId, .name = "Done", .wipLimit = 0}));
    auto task = pumpUntilReady(owner.execute<BoardModel>(
        CreateTask{.projectId = project.projectId, .columnId = column.columnId, .title = "T1"}));

    // The "trigger" journal entry (a move) and its "cascade" (e.g. a tag
    // add) must both exist, with the cascade's entry carrying the
    // trigger's entry id as its causal parent -- read morph::journal's
    // actual LogEntry shape (include/morph/journal/*.hpp) to confirm the
    // exact field name once Task 11's mechanism is implemented at the
    // framework level (this may require a small morph::journal addition;
    // if LogEntry has no causal-parent-id field today, add one as part of
    // this task -- it is the one piece of framework code this rung's
    // design explicitly calls for).

    // Replay the journal (morph::journal::JournalReader::entries() or
    // equivalent replay entry point -- confirm exact API) and assert the
    // cascade fires exactly once total (not once at record time, once
    // again at replay time == twice).
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "cascaded journal" --output-on-failure`
Expected: FAIL — no causal-parent-id field exists on `LogEntry` yet
(confirm by reading `include/morph/journal/` first).

- [ ] **Step 3: Add the causal-parent-id field to `morph::journal`**

This is a framework-level change (`include/morph/journal/*.hpp`), so it
needs full Doxygen (`CLAUDE.md`'s Docs workflow requirement) and a
`docs/spec/` update if `morph::journal` has an existing spec file (check
`docs/spec/` for a journal spec before editing — read it first per
`CLAUDE.md`'s "read the spec before changing a public type" rule). Add an
optional `causalParentId` field to `LogEntry` (default/sentinel value
meaning "no parent," matching the existing `callId=0` sentinel convention
per this codebase's memory of that pattern), and a replay-mode flag
`JournalReader` (or wherever replay is driven from) can check to suppress
rule evaluation — the actual suppression happens in Phase 6's rules
engine, which checks this flag before evaluating any rule; this task only
adds the plumbing (the field + a way to signal "currently replaying").

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "cascaded journal" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/morph/journal/ examples/kanban/tests/
git commit -m "journal: add causal-parent-id to LogEntry; kanban: prove replay doesn't re-fire a cascade"
```

---

## Phase 6: Automation rules engine (README step 6)

Event → condition → mutation rules (e.g. "task moved to Done ⇒ assign to
closer, add tag"), built on Phase 5's cascade-journaling decision.

### Task 13: Rule DTOs and storage

**Files:**
- Modify: `examples/kanban/include/kanban/db/kanban_entity.hpp` (add
  `RuleRecord`)
- Modify: `examples/kanban/src/db/schema.cpp` (migration for the new table)
- Create: `examples/kanban/include/kanban/dto/rule_dto.hpp`
- Test: `examples/kanban/tests/test_kanban_schema.cpp` (extend)

**Interfaces:**
- Produces: `kanban::db::RuleRecord{id, project (BelongsTo), triggerEvent,
  conditionField, conditionValue, mutationType, mutationValue}` (read
  `kanban_entity.hpp`'s existing records for the exact `Light::Field`/
  `Light::BelongsTo` template argument conventions before writing this —
  copy `ColumnRecord`'s shape verbatim, changing only field names/types),
  `kanban::CreateRule{projectId, triggerColumnId, mutationType,
  mutationValue}`, `kanban::CreateRuleResult{ruleId}`,
  `kanban::GetRules{projectId}`, `kanban::GetRulesResult{rules}`,
  `kanban::DeleteRule{ruleId}`.

- [ ] **Step 1: Write the failing schema test**

Mirror `test_kanban_schema.cpp`'s existing per-table assertions (table
exists, expected columns) for a new `rules` table.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "rules table" --output-on-failure`
Expected: FAIL — table doesn't exist.

- [ ] **Step 3: Add `RuleRecord` and the migration**

Follow `ColumnRecord`'s exact shape in `kanban_entity.hpp`; add the
`LIGHTWEIGHT_SQL_MIGRATION` entry in `schema.cpp` mirroring the existing
migrations' numbering/structure.

- [ ] **Step 4: Add the DTOs**

`rule_dto.hpp`, mirroring `board_dto.hpp`'s `struct` conventions
(`validate()` methods, DTO-only strong types per `IMPLEMENTATION.md`).

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "rules table" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add examples/kanban/include/kanban/db/kanban_entity.hpp \
        examples/kanban/src/db/schema.cpp \
        examples/kanban/include/kanban/dto/rule_dto.hpp \
        examples/kanban/tests/test_kanban_schema.cpp
git commit -m "kanban: add the rules table and CreateRule/GetRules/DeleteRule DTOs"
```

### Task 14: Rule evaluation on `MoveTaskPosition`

**Files:**
- Modify: `examples/kanban/include/kanban/models/board_model.hpp`
- Modify: `examples/kanban/src/models/board_model.cpp`
- Test: `examples/kanban/tests/test_board_model.cpp` (extend)

**Interfaces:**
- Consumes: Task 13's `db::RuleRecord`, Task 12's causal-parent-id +
  replay-mode-flag plumbing (rule evaluation checks the replay flag first
  and no-ops if set, implementing Phase 5's Option A decision).
- Produces: `BoardModel::execute(const CreateRule&) -> CreateRuleResult`,
  `execute(const GetRules&) -> GetRulesResult`, `execute(const DeleteRule&) -> Ack`,
  and a private `evaluateRules(TaskId movedTask, ColumnId newColumn)`
  called from the end of `execute(const MoveTaskPosition&)`'s existing
  body (after the position commit, before returning) — only when not
  currently replaying.

- [ ] **Step 1: Write the failing test — "task moved to Done ⇒ add tag"**

```cpp
TEST_CASE("A rule firing on move-to-column adds a tag, journaled with a causal parent",
          "[kanban][rules]") {
    // Seed a project, a "Done" column, a task, and a rule
    // (CreateRule{triggerColumnId=doneColumnId, mutationType=AddTag,
    // mutationValue="closed"}).
    //
    // Move the task into the Done column.
    //
    // Assert: the task now carries the "closed" tag, AND the journal has
    // two entries for this action -- the move itself, and the cascaded tag
    // add -- with the tag-add entry's causalParentId equal to the move
    // entry's id.
}

TEST_CASE("Replaying a move-to-Done journal entry does not re-fire its rule",
          "[kanban][rules]") {
    // Same seed as above. Perform the move once (rule fires, tag added).
    // Replay the journal from scratch against a fresh BoardModel instance
    // (mirror however test_board_model.cpp's existing replay tests, if
    // any, invoke replay -- otherwise use morph::journal's replay entry
    // point directly). Assert the tag is added exactly once (not twice),
    // proving Phase 5's suppress-during-replay decision actually holds for
    // a real rule, not just the Task 12 simulation.
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "rule firing" --output-on-failure`
Expected: FAIL — no rule evaluation exists yet.

- [ ] **Step 3: Implement `CreateRule`/`GetRules`/`DeleteRule` + `evaluateRules`**

`CreateRule`/`GetRules`/`DeleteRule` follow the exact CRUD pattern already
established by `CreateColumn`/`GetBoardState` (RBAC via `requireRole`,
Lightweight `Insert`/`Query`/`Delete`). `evaluateRules` queries
`db::RuleRecord` for the moved task's project and new column, and for each
match applies the mutation (starting with just "add tag," per the
README's own example — a full mutation-type enum can grow later, but this
task should NOT invent mutation kinds the README/design spec doesn't ask
for; "assign to closer" from the README's example needs a "closer"
concept that doesn't exist elsewhere in this rung — scope this task's
mutation support to what's mechanically supportable today: tag add/remove.
If the reviewing engineer wants "assign to closer" specifically, that
needs its own task with its own design decision — flag this explicitly in
the PR description rather than inventing an ungrounded "closer" concept).

Each fired mutation writes its own journal entry with `causalParentId` set
to the triggering `MoveTaskPosition` entry's id (Phase 5's mechanism), and
`evaluateRules` checks the replay-mode flag first, returning immediately
if replaying.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "rule" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/include/kanban/models/board_model.hpp \
        examples/kanban/src/models/board_model.cpp \
        examples/kanban/tests/test_board_model.cpp
git commit -m "kanban: evaluate automation rules on MoveTaskPosition, journaled with a causal parent, suppressed during replay"
```

### Task 15: Rules GUI (MembersView-style CRUD list)

**Files:**
- Create: `examples/kanban/gui/qml/RulesView.qml`
- Modify: `examples/kanban/gui_lib/board_qml_bridge.hpp` /
  `board_qml_bridge.cpp` (add `createRule`/`getRules`/`deleteRule`
  `Q_INVOKABLE`s + a `rules` `Q_PROPERTY`)
- Test: `examples/kanban/tests/test_board_qml_bridge.cpp` (extend)

**Interfaces:**
- Consumes: Task 14's `CreateRule`/`GetRules`/`DeleteRule`.
- Produces: a rules management view, structurally identical to
  `MembersView.qml` (Phase 1 Task 4) — a flat list, a create form, a delete
  button per row.

- [ ] **Step 1: Write the failing bridge test**

Mirror Task 6's property-existence + behavioral test shape for
`createRule`/`rules`/`deleteRule`.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "createRule\|BoardBridge.*rules" --output-on-failure`
Expected: FAIL.

- [ ] **Step 3: Implement the bridge additions**

Mirror `MembersView`'s bridge-side pattern from Phase 1 Task 3/`MembersView.qml`
exactly.

- [ ] **Step 4: Run test to verify it passes, add `RulesView.qml`**

Run: `ctest --preset cl-debug -R "rules" --output-on-failure`
Expected: PASS. `RulesView.qml` mirrors `MembersView.qml`'s layout.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/gui/qml/RulesView.qml \
        examples/kanban/gui_lib/board_qml_bridge.hpp \
        examples/kanban/gui_lib/board_qml_bridge.cpp \
        examples/kanban/tests/test_board_qml_bridge.cpp
git commit -m "kanban: add the rules management GUI view"
```

---

## Phase 7: Task attachments (README step 8)

Blob bytes over a side-channel HTTP endpoint next to the WebSocket server,
reusing `TokenVerifier`; metadata through actions. Per README/LADDER.md,
this is **the largest new attack surface in the ladder** — a hand-written
HTTP server beside the WebSocket server — so this phase gets the most
conservative, most-reviewed treatment of the seven.

### Task 16: Attachment metadata action + storage

**Files:**
- Modify: `examples/kanban/include/kanban/db/kanban_entity.hpp` (add
  `AttachmentRecord`)
- Modify: `examples/kanban/src/db/schema.cpp`
- Create: `examples/kanban/include/kanban/dto/attachment_dto.hpp`
- Modify: `examples/kanban/include/kanban/models/board_model.hpp` /
  `.cpp` (add `AddAttachment`/`GetAttachments`/`RemoveAttachment` actions —
  metadata only, no bytes)
- Test: `examples/kanban/tests/test_board_model.cpp` (extend)

**Interfaces:**
- Produces: `kanban::db::AttachmentRecord{id, task (BelongsTo), filename,
  contentType, sizeBytes, storageKey, uploadedBy, uploadedAtMs}`,
  `kanban::AddAttachment{taskId, filename, contentType, sizeBytes}` (called
  **after** the HTTP upload completes — Task 17 defines the flow order:
  upload bytes first, get a `storageKey` back, then commit metadata via
  this action, mirroring README step 8's "bytes over a side channel,
  metadata through actions").

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("AddAttachment records metadata for a task, GetAttachments lists it", "[kanban][attachments]") {
    // Seed project/column/task. Call AddAttachment{taskId, filename="report.pdf",
    // contentType="application/pdf", sizeBytes=1024, storageKey="abc123"}.
    // Assert GetAttachments{taskId} returns exactly that row.
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "AddAttachment" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `AttachmentRecord` + the three actions**

Follow `CommentRecord`'s exact shape (a task-scoped child table) for
`AttachmentRecord`; `AddAttachment`/`GetAttachments`/`RemoveAttachment`
follow `AddComment`'s exact RBAC/CRUD pattern in `board_model.cpp`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "Attachment" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/include/kanban/db/kanban_entity.hpp \
        examples/kanban/src/db/schema.cpp \
        examples/kanban/include/kanban/dto/attachment_dto.hpp \
        examples/kanban/include/kanban/models/board_model.hpp \
        examples/kanban/src/models/board_model.cpp \
        examples/kanban/tests/test_board_model.cpp
git commit -m "kanban: add attachment metadata actions (AddAttachment/GetAttachments/RemoveAttachment)"
```

### Task 17: HTTP side-channel upload/download server

**Files:**
- Create: `examples/kanban/include/kanban/http/attachment_server.hpp`
- Create: `examples/kanban/src/http/attachment_server.cpp`
- Modify: `examples/kanban/src/server/main.cpp` (start the HTTP server
  alongside the WebSocket server)
- Test: `examples/kanban/tests/test_attachment_server.cpp`

**Interfaces:**
- Consumes: `morph::session::TokenVerifier` (`include/morph/session/session_auth.hpp:421` —
  already confirmed to exist exactly where the README expects; read its
  full public interface — `verify(...)` signature — before writing this,
  it was not fully read in this planning pass).
- Produces: `kanban::http::AttachmentServer` — a minimal HTTP server (Qt's
  `QHttpServer` if already a dependency anywhere in the tree, otherwise a
  small hand-rolled listener per README's "hand-written HTTP server"
  framing — check whether any sibling rung or `include/morph/` already has
  an HTTP listener to reuse before writing a new one from scratch; if none
  exists, this is genuinely new surface and should stay as small as
  possible: `POST /attachments` (multipart or raw body + headers for
  filename/contentType, `Authorization: Bearer <token>` verified via
  `TokenVerifier` before accepting any bytes) returning a `storageKey`,
  `GET /attachments/{storageKey}` (same auth check) streaming bytes back).
  A hard size bound (**required** per README: "enforce its own size
  bound") rejects oversized uploads before buffering them fully in memory.

- [ ] **Step 1: Write the failing test — reject unauthenticated upload**

```cpp
TEST_CASE("AttachmentServer rejects an upload with no bearer token", "[kanban][attachments][http]") {
    kanban::http::AttachmentServer server{/* TokenVerifier, size bound, storage dir */};
    // POST with no Authorization header; assert 401/403 response, nothing written to storage.
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "AttachmentServer" --output-on-failure`
Expected: FAIL — class doesn't exist.

- [ ] **Step 3: Implement, TDD'ing each requirement as its own test first**

Do not write the full server before its tests — add one `TEST_CASE` per
requirement, red-then-green, in this order (each is its own
Step-3a/3b/3c... red/green pair, following this task's Step 1/2 pattern
repeated):
1. Valid token + valid body → 200, `storageKey` returned, bytes on disk.
2. Oversized body → 413 (or equivalent), rejected before full buffering
   (verify via a deliberately-small configured size bound in the test, not
   a real multi-GB payload).
3. `GET` with valid token for an existing `storageKey` → 200, correct
   bytes.
4. `GET` with valid token for a nonexistent `storageKey` → 404.
5. Malformed/garbage request body → the server's parser must not crash or
   hang (this is the fuzz-corpus requirement — README: "its request parser
   joins the fuzz corpus." Add a `test_attachment_server_fuzz.cpp` or fold
   a libFuzzer harness entry into the existing fuzz corpus under whatever
   directory `MORPH_BUILD_FUZZERS`'s existing harnesses live in — read
   that directory's structure first before adding a new harness file).
6. Upload dying after metadata commit (a dangling row): a test that calls
   `AddAttachment` (Task 16) with a `storageKey` that was never actually
   uploaded, then asserts `GET /attachments/{storageKey}` returns 404
   cleanly (not a crash) — proving the "upload dying after metadata
   commit" scenario the README names is at least non-corrupting, even
   though full transactional consistency between the HTTP upload and the
   metadata commit is out of scope for this pass (note this limitation
   explicitly in the class's own doc comment, present tense: "a
   dangling metadata row with no corresponding blob returns 404 on
   download, rather than being treated as an error state" — not a
   "this used to..." framing).

- [ ] **Step 4: Run the full attachment-server test suite**

Run: `ctest --preset cl-debug -R "AttachmentServer" --output-on-failure`
Expected: PASS, all 6 requirement tests green.

- [ ] **Step 5: Wire into `server/main.cpp`**

Start `AttachmentServer` alongside the existing `QtWebSocketServer` in
`examples/kanban/src/server/main.cpp`, sharing the same signing
secret/`TokenVerifier` instance the WebSocket server already constructs
(read `main.cpp`'s existing server setup to find that instance and pass it
in, rather than constructing a second `TokenVerifier` with a
separately-sourced secret — two verifiers with independently-configured
secrets is the exact kind of drift this reuse is meant to avoid).

- [ ] **Step 6: Update `docs/spec/security.md` if it documents side channels**

Check `docs/spec/security.md` (confirmed to exist, per the earlier
`ls docs/spec/` output) for any existing statement about side channels or
attack surface enumeration; add this HTTP server to that enumeration if
the spec tracks such a list, per `CLAUDE.md`'s "update the spec" rule.

- [ ] **Step 7: Commit**

```bash
git add examples/kanban/include/kanban/http/attachment_server.hpp \
        examples/kanban/src/http/attachment_server.cpp \
        examples/kanban/src/server/main.cpp \
        examples/kanban/tests/test_attachment_server.cpp \
        docs/spec/security.md
git commit -m "kanban: add the attachment HTTP side channel, reusing TokenVerifier, with its own size bound"
```

### Task 18: Attachments GUI

**Files:**
- Modify: `examples/kanban/gui/qml/TaskDetailPopup.qml`
- Modify: `examples/kanban/gui_lib/board_qml_bridge.hpp` /
  `board_qml_bridge.cpp` (add `uploadAttachment`/`downloadAttachment`
  `Q_INVOKABLE`s, an `attachments` `Q_PROPERTY` on the open task)
- Test: `examples/kanban/tests/test_board_qml_bridge.cpp` (extend)

**Interfaces:**
- Consumes: Task 16's `AddAttachment`/`GetAttachments`, Task 17's
  `AttachmentServer` HTTP endpoints (the bridge performs the HTTP
  upload/download itself via `QNetworkAccessManager`, then calls
  `AddAttachment` on success — mirroring the flow order Task 16 already
  specified).

- [ ] **Step 1: Write the failing bridge test**

```cpp
TEST_CASE("BoardBridge uploads a file and records its metadata", "[kanban][gui][attachments]") {
    // ... construct BoardBridge against a real AttachmentServer + BackendRig ...
    bridge.uploadAttachment(taskId, "/path/to/local/file.pdf");
    pumpUntil([&] { return /* attachments property updated */ true; });
    CHECK(bridge.attachments().size() == 1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "uploadAttachment" --output-on-failure`
Expected: FAIL.

- [ ] **Step 3: Implement**

`uploadAttachment` reads the local file, `POST`s it to
`AttachmentServer` via `QNetworkAccessManager`, then calls
`AddAttachment` with the returned `storageKey` on success, emitting
`failed(QString)` on any step's failure (network, server rejection, or
`AddAttachment` itself).

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "attachment" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Add the QML affordance**

`TaskDetailPopup.qml` gains an attachment list + an "attach file" button
(a `FileDialog` for local file selection), alongside the existing comment
list from Phase 1.

- [ ] **Step 6: Commit**

```bash
git add examples/kanban/gui/qml/TaskDetailPopup.qml \
        examples/kanban/gui_lib/board_qml_bridge.hpp \
        examples/kanban/gui_lib/board_qml_bridge.cpp \
        examples/kanban/tests/test_board_qml_bridge.cpp
git commit -m "kanban: add attachment upload/download to the task detail view"
```

---

## Final Phase: Whole-rung verification

### Task 19: Full test suite + README/spec reconciliation

**Files:** None new — verification only.

- [ ] **Step 1: Run the complete kanban test suite**

Run: `ctest --preset cl-qt-debug -L kanban --output-on-failure`
Expected: PASS, every test from every phase.

- [ ] **Step 2: Run the full ladder CI-equivalent locally**

Run (Windows): `cmake --build --preset windows-everything && ctest --preset windows-everything -L ladder --output-on-failure`
(Assumes PR #126's `windows-everything` fixes are merged; otherwise use
`cl-qt-debug` with `-DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=kanban`.)

- [ ] **Step 3: Re-read `examples/kanban/README.md`'s Definition of Done line by line**

Confirm every bullet now has a passing test or shipped feature backing it;
update the README's own status line ("Status: planned...") to reflect
completion, present tense.

- [ ] **Step 4: Commit the final README status update**

```bash
git add examples/kanban/README.md
git commit -m "kanban: mark rung 4 complete -- all DoD bullets met, deferred items implemented"
```

- [ ] **Step 5: Push and let CI run in full**

```bash
git push
```

Watch the full CI matrix (including the new `kanban-tsan` job from Phase 4)
go green before considering PR #121 ready for merge review.
