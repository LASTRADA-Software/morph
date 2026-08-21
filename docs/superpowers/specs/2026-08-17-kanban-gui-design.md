# Kanban GUI Design

Follow-on to the kanban backend (rung 4 of the [application ladder](../../../examples/LADDER.md),
`docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`). This spec covers the
desktop client only: a native Qt Quick app driving the already-implemented
`kanban::ProjectAdminModel`/`kanban::BoardModel` action surface.

## 1. Scope

**In scope:**
- Login (dev-mode, username-only, `SigningAuthorizer`-backed).
- Project bootstrap: list the caller's own projects, create a new one.
- The board itself: columns, swimlanes, tasks, drag-and-drop moves, comments,
  a journal-derived activity stream.
- A minimal member-management view (list/set-role/remove).
- Desktop client only, both `Local` (in-process) and `Remote` (WebSocket)
  modes, mirroring `examples/bookmarks/gui/main.cpp`'s `--server` flag.

**The offline stack is wired.** `BoardBridge` (`examples/kanban/gui_lib/
board_qml_bridge.hpp`/`.cpp`) owns a `SqliteOfflineQueue`-backed offline
queue, turned on via its own `enableOfflineQueue(queuePath, probe,
monitorConfig)` method: a `NetworkMonitor` drives `moveTask()`'s
online/offline branch (queues a serialised `MoveTaskPosition` -- opId
included -- instead of dispatching while offline), and a
`ReconnectCoordinator`/`SyncWorker` pair drains and replays the queue once
the monitor reports the network reachable again, through a dedicated
`BoardPresenter::moveTaskForReplay()` overload that bypasses the shared
`taskMoved`/`failed` signals (the same isolation `getEventsSinceForPolling`
already uses for the identical reason). `BoardBridge::syncStatusChanged(int
queueDepth, int deadLettered)` reports the queue's depth and cumulative
dead-lettered count after every enqueue and every replay pass -- the signal
Task 6's "N changes pending sync" indicator (below) surfaces. The whole
mechanism is gated behind `MORPH_BUILD_OFFLINE_SQLITE` (needs SQLite3); a
configure without it builds `BoardBridge` with no offline awareness at all,
the pre-wiring shape. `main.cpp`'s own desktop client currently supplies only
an always-online placeholder probe (this rung has no dedicated "ping" action
yet), so the queue/replay mechanism itself is proven end-to-end but a real
connectivity probe remains a follow-up (see §11).

**Explicitly out of scope** (unchanged from the backend's own out-of-scope
list — `examples/kanban/README.md`'s "Deferred within this rung" and the
backend design spec's §9):
- Automation rules (event → condition → mutation) — no action/DTO exists for
  this, so there is nothing for a GUI to drive.
- Task attachments — same reason.
- A WASM build. Desktop only, for this pass.
- Visual/UX design beyond "legible, with smooth drag feedback" — see §2.

## 2. Visual bar: "legible + smooth drag"

`LADDER.md` names kanban as the ladder's one deliberate exception to
`IMPLEMENTATION.md` rule 2's "zero styling effort" — "visually legible" is
the only elaboration given; everything beyond that is this spec's own
decision, made explicit here rather than left implicit:

- Default Qt Quick Controls 2 style (whatever style the other rungs already
  build with — no new style module, no custom `Material`/`Universal` theme).
- No custom color palette, no icons, no branding, no transition animations
  beyond what Qt Quick's `ListView` gives a `move` for free.
- The **one** deliberate exception: a dragged task card gets real visual
  feedback — it visually detaches and follows the cursor, and the column
  under the cursor highlights — because that is the one interaction default
  controls cannot fake, and a "showcase" board with no drag feedback would
  read as broken, not restrained.

No other rung is affected by this decision; every other rung's zero-styling
convention is unchanged.

## 3. Backend action: `GetMyProjects`

`CreateProject` returns exactly the one project it created, and
`GetProjectRoles` needs a project id already in hand — neither answers
"which projects does the caller belong to", which a project-list view
needs. `GetMyProjects` is implemented on `kanban::ProjectAdminModel` (same
model as `CreateProject`/`SetMemberRole`/`RemoveMember`/`GetProjectRoles` —
project-admin-scoped, not board-scoped):

```cpp
/// @brief Lists every project the calling principal has any role on.
struct GetMyProjects {};

/// @brief One project the caller belongs to, with their own role on it.
struct MyProjectSummary {
    ProjectId id;
    std::string name;
    Role myRole;
};

struct GetMyProjectsResult {
    std::vector<MyProjectSummary> projects;
};

GetMyProjectsResult execute(const GetMyProjects& action);
```

`GetMyProjects` takes no parameters — the principal comes from
`session::current()`, exactly like `AddComment`'s `requireOwner()` pattern
in `BoardModel`. It queries `db::ProjectRoleRecord` (the `project_has_roles`
table) filtered by `principal`, loads each referenced `db::ProjectRecord`,
and returns the results ordered by project name. No pagination — project
count per user is expected to be small at ladder-example scale, same
reasoning `docs/superpowers/specs/2026-08-16-kanban-rung4-design.md` already
applies to `BoardModel::buildState`'s unpaginated per-project reads.

## 4. Architecture

Three layers, following `examples/bookmarks`'/`examples/polls`' established
pattern exactly — see `examples/IMPLEMENTATION.md`'s "Presenters translate
and route; they never decide" and `examples/common/gui/presenter.hpp`'s
shared `Presenter` base.

### 4.1 Directory layout

```
examples/kanban/
  gui/
    main.cpp
    qml/
      Main.qml
      LoginView.qml
      ProjectListView.qml
      BoardView.qml
      TaskDetailPopup.qml
      MembersView.qml
  gui_lib/
    project_admin_presenter.hpp / .cpp
    project_admin_qml_bridge.hpp / .cpp
    board_presenter.hpp / .cpp
    board_qml_bridge.hpp / .cpp
  tests/
    test_project_admin_presenter.cpp
    test_project_admin_qml_bridge.cpp
    test_board_presenter.cpp
    test_board_qml_bridge.cpp
    test_board_concurrent_drag.cpp
    test_gui_qml_smoke.cpp
```

### 4.2 Layer split: two bridge/presenter pairs, not one

Split along the backend's own strand boundary (`ProjectAdminModel` vs.
`BoardModel` are already separate models/strands) rather than one unified
bridge — every sibling rung with more than one model keeps its bridges
split by model, and merging them here would be a one-off inconsistency to
save a small amount of boilerplate.

- **`ProjectAdminPresenter`/`ProjectAdminBridge`**: login, `GetMyProjects`,
  `CreateProject`, `GetProjectRoles`, `SetMemberRole`, `RemoveMember`.
- **`BoardPresenter`/`BoardBridge`**: `OpenBoard`, `GetBoardState`,
  `CreateColumn`, `CreateSwimlane`, `CreateTask`, `MoveTaskPosition`,
  `AddComment`, `GetEventsSince`, `GetActivity`.

### 4.3 Responsibilities per layer

- **Presenter** (`QObject`-derived, Qt-Core-only, no QML dependency): owns
  one `BridgeHandler<Model>`, translates action results into signals
  (`projectCreated`, `boardOpened`, `taskMoved`, `activityUpdated`,
  `failed(QString)`), owns the `GetEventsSince` poller as a `QTimer`
  (mirrors `examples/polls/gui_lib/poll_presenter.cpp`'s `Poller`).
- **Bridge** (`QObject`, `Q_OBJECT`, the QML-facing surface): owns the
  presenter, exposes state via `Q_PROPERTY` (current board as JSON, current
  project list as JSON, current role, principal), actions via `Q_INVOKABLE`
  (`createColumn(name, wipLimit)`, `moveTask(taskId, columnId, swimlaneId,
  position)`, `addComment(taskId, body)`, …), forwards/redacts presenter
  signals. `_liveness` (a `shared_ptr<const void>`) declared last, per the
  documented async-completion lifetime rule.
- **QML**: bindings only. The one place with real logic is the drag/drop
  handler computing a drop target and position (see §6) — kept small and
  isolated, not spread through the view.

### 4.4 Bootstrap (`gui/main.cpp`)

Mirrors `examples/bookmarks/gui/main.cpp` exactly: `--server <url>` selects
`Remote` mode (real `QtWebSocketBackend` over the flag's URL); the default
with no flag is `Local` (in-process `LocalBackend`). Bridges wired via
`engine.setInitialProperties({{"projectAdminBridge", ...}, {"boardBridge",
...}})`, then `engine.loadFromModule(uri, "Main")`.

## 5. Auth flow

Identical shape to `examples/bookmarks/gui_lib/bookmark_qml_bridges.cpp`'s
`FormsBridge::onLoginSucceeded` — kanban's `Login{username}` →
`LoginResult{AuthToken token, principal}` is structurally the same as
bookmarks', so the pattern transposes directly:

1. `LoginView.qml`: username field → `projectAdminBridge.login(username)`.
2. `ProjectAdminBridge::login(QString)` (`Q_INVOKABLE`) → presenter executes
   `Login` via its `BridgeHandler`.
3. On success, the presenter installs a `session::Context{principal, token}`
   onto the shared `Bridge` via `setDefaultSession` — **not** stored as a
   bridge/presenter member. Every subsequent action rides this session
   automatically.
4. The bridge emits `loggedIn(QString principal)`. **The raw token is never
   emitted on any signal.** If a reply payload is ever re-serialized for a
   generic `replyReceived`-style signal, construct a redacted copy first
   (`LoginResult redacted = *result; redacted.token = AuthToken{};`) before
   serializing — the exact defect this session already found and fixed once
   in bookmarks' bridge; do not reintroduce it here.
5. `Main.qml`'s `StackView` reacts to `loggedIn` → replaces to the project
   list page.

## 6. Board view and drag-and-drop

### 6.1 Layout

`BoardView.qml`: an outer vertical section per swimlane (only rendered as
distinct sections when the board has more than one swimlane — a
single-swimlane board, the common case, renders as a flat column row with
no swimlane chrome), each containing a horizontal `ListView` of columns
(`ListView.Horizontal`). Each column delegate is a `Rectangle` with a
header (name, `"{count}"` when `wipLimit == 0`, `"{count}/{wipLimit}"`
otherwise) and a vertical `ListView` of task-card delegates.

### 6.2 Drag mechanism

Native Qt Quick `Drag` attached property + `DropArea` — no custom
mouse-position tracking, no synthesized events:

1. Each task-card delegate: `Drag.active: dragHandler.active`, a
   `DragHandler` (or `MouseArea` with `drag.target`) reparenting the card to
   the board's root `Item` for the duration of the drag so it visually
   floats above the columns.
2. Each column (or a `DropArea` sized to the column's task list) sets
   `Drag.onEntered`/`onExited` to toggle a border highlight
   (`border.color: dropArea.containsDrag ? "steelblue" : "transparent"`).
   A column already at its WIP limit shows the highlight in a different
   color (e.g. red) instead of blue while a card is dragged over it, as an
   early visual cue — enforcement is still server-side; this is purely a
   hint.
3. On drop: compute the destination column id, destination swimlane id
   (from which swimlane section the drop y-coordinate falls under, when
   more than one exists), and destination position (index within the
   destination list nearest the drop point).
4. `boardBridge.moveTask(taskId, columnId, swimlaneId, position)` is
   called. The bridge — not QML — generates the `opId`
   (`QUuid::createUuid().toString()`) required for `MoveTaskPosition`'s
   exactly-once semantics; QML never sees or manages it.
5. **Optimistic UI**: the card list updates immediately (Qt Quick
   `ListView`'s built-in move transition) rather than waiting for the round
   trip. The next poll-triggered `GetBoardState`/`GetEventsSince` refresh is
   the authoritative source; if the move was rejected server-side (WIP
   limit hit between the drag starting and the drop landing, a deleted
   column, etc.), that refresh snaps the card back and `failed(QString)`
   surfaces the rejection reason via the standard error-`Label` pattern.

## 7. Comments and activity

- **Comments**: tapping (not dragging) a card opens `TaskDetailPopup.qml` —
  a `Popup`/`Drawer` overlay, not a full `StackView` page, so the board
  stays visible underneath. Shows the task's comment list plus an
  add-comment field, driven by `AddComment`.
- **Activity**: a separate, always-visible or toggleable panel rendering
  `GetActivity`'s `ActivityEvent{actionType, principal, timestampMs,
  summary}` list, refreshed on the same poll tick as the board (one
  `GetEventsSince`-driven timer covers both, rather than two independent
  pollers).

## 8. Members view

`MembersView.qml`: a flat `ListView` over `GetProjectRoles`' `MemberRole
{principal, role}` rows. Each row: principal text, a `ComboBox`
(Viewer/Member/Manager) calling `setMemberRole(principal, role)` on
selection change, and a remove button calling `removeMember(principal)`.
Adding a member is a text field (principal) + role picker, calling
`setMemberRole` directly — there is no "add member by search," per the
minimal-bootstrap scope decision (§1).

## 9. Testing

Follows the established convention exactly — no new testing pattern
invented:

- **Presenter tests** (`test_project_admin_presenter.cpp`,
  `test_board_presenter.cpp`): plain `QCoreApplication`, a `BackendRig`
  (Local mode) backing the presenter's `BridgeHandler`, signal emissions
  asserted via `QObject::connect` lambdas + `pumpUntil` — no QML.
- **Bridge tests** (`test_project_admin_qml_bridge.cpp`,
  `test_board_qml_bridge.cpp`): `QMetaObject` introspection
  (`indexOfProperty`, `indexOfMethod`) for the exposed surface, plus
  behavioral tests driving `Q_INVOKABLE`s against a `BackendRig` and
  asserting property/signal updates.
- **`test_gui_qml_smoke.cpp`**: loads `Main` via `QQmlApplicationEngine`
  under the offscreen platform (CI-set `QT_QPA_PLATFORM=offscreen`),
  asserts non-empty `rootObjects()` and zero QML warnings.
- **Drag-and-drop**: per `examples/TESTING.md`'s "no synthesized-mouse-event
  flows" rule, the visual gesture itself is not tested via simulated mouse
  events. `BoardBridge::moveTask()` — the actual dispatch the gesture
  triggers — is tested directly with hardcoded arguments in
  `test_board_qml_bridge.cpp`. A separate **`test_board_concurrent_drag.cpp`**
  mirrors the backend's own `test_kanban_stress.cpp`: multiple
  `BoardBridge` instances (not raw `BoardModel`s) call `moveTask()`
  concurrently against a shared `BackendRig` server. Pass criteria, mirroring
  the backend stress test's own invariant: after every call settles, a fresh
  `GetBoardState` read shows dense, unique positions within every
  `(columnId, swimlaneId)` pair and no task duplicated or dropped — the same
  property the backend already proves at the model level, now exercised
  through the GUI's own bridge/presenter code path rather than bypassing it.

## 10. Doxygen / CI

Per `CLAUDE.md`: any new public symbol needs complete `@param`/`@tparam`/
`@return` Doxygen or the Docs workflow fails. `docs/CMakeLists.txt`'s
`DOCS_SOURCES` currently scans only `include/morph` + `ARCHITECTURE.md` —
confirmed during the kanban backend's own final review (`include/morph/`
is scanned, `examples/` is not) — so this rung's GUI classes are not
actually gated by the Doxygen build, matching every sibling rung's GUI
code. Doxygen-style comments should still be written for consistency with
the surrounding codebase's convention, not because CI enforces it here.

## 11. Out-of-scope follow-ups this spec deliberately does not solve

- A real connectivity probe for `enableOfflineQueue()` (§1) — `main.cpp`
  currently wires an always-online placeholder, since this rung has no
  dedicated "ping" action yet; the queue/replay mechanism itself is already
  proven end-to-end against a test-supplied probe.
- A "N changes pending sync" GUI indicator surfacing
  `BoardBridge::syncStatusChanged` (§1) — the signal exists; no QML view
  consumes it yet (see Task 6 of the rung-4-completion plan).
- A WASM build (§1) — separate design pass if ever pursued.
- Attachments UI — no backend surface exists yet (Phase 7 of the plan).
  Automation rules now have both a backend surface (`CreateRule`/`GetRules`/
  `DeleteRule`, rule evaluation) and a GUI (`RulesView.qml`, opened from
  `BoardView.qml`'s "Rules" header button), so they are no longer part of
  this out-of-scope list.
- `GetMyProjects` pagination — not needed at ladder-example scale; revisit
  if a future rung's project count assumption changes.
